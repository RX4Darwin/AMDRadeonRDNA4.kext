/*
 *  rdna4-logd.c
 *  AMDRadeonRDNA4
 *
 *  Follows the kext's log ring (debug.rdna4_log_read, src/klog.cpp) into a
 *  file per boot, so that a boot's log survives the ring wrapping and the
 *  boot itself: DIR/boot-YYYYMMDD-HHMMSS.log, named after the boot time,
 *  the oldest removed beyond KEEP files. Bytes the ring dropped before they
 *  were read are marked in the file.
 *
 *    rdna4-logd [-d dir] [-k keep]
 *
 *  Needs root. Waits while no kext with the sysctl is loaded, so that it can
 *  run as a LaunchDaemon (userspace/rdna4-logd/install.sh).
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define CHUNK (64 * 1024)       /* the kext's kReadChunk */
#define MAX_FILES 256

static volatile sig_atomic_t gStop;

static void onSignal(int sig)
{
	(void)sig;
	gStop = 1;
}

/* The log from `from`: the bytes into buf, their first offset and the bytes ever logged; -1 without the sysctl. */
static ssize_t readLog(uint64_t from, char *buf, uint64_t *first, uint64_t *written)
{
	static char raw[16 + CHUNK];
	size_t len = sizeof(raw);

	if (sysctlbyname("debug.rdna4_log_read", raw, &len, &from, sizeof(from)) || len < 16)
		return -1;

	memcpy(first, raw, 8);
	memcpy(written, raw + 8, 8);
	memcpy(buf, raw + 16, len - 16);

	return (ssize_t)(len - 16);
}

static int byName(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Keep the newest `keep` boot-*.log files of dir. */
static void prune(const char *dir, int keep)
{
	char *names[MAX_FILES];
	struct dirent *e;
	int n = 0;
	DIR *d = opendir(dir);

	if (!d)
		return;

	while ((e = readdir(d)) && n < MAX_FILES) {
		if (!strncmp(e->d_name, "boot-", 5) && strstr(e->d_name, ".log"))
			names[n++] = strdup(e->d_name);
	}

	closedir(d);
	qsort(names, n, sizeof(names[0]), byName);

	for (int i = 0; i < n; i++) {
		char path[1024];

		if (i < n - keep) {
			snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
			unlink(path);
		}

		free(names[i]);
	}
}

/* DIR/boot-<boot time>.log, opened for appending. */
static FILE *openBootLog(const char *dir)
{
	struct timeval boot;
	size_t len = sizeof(boot);
	char path[1024], stamp[32];
	time_t t;

	if (sysctlbyname("kern.boottime", &boot, &len, NULL, 0))
		return NULL;

	t = boot.tv_sec;
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
	snprintf(path, sizeof(path), "%s/boot-%s.log", dir, stamp);

	return fopen(path, "w");
}

/* Copy the log into f as it grows, until a signal. */
static void follow(FILE *f)
{
	static char buf[CHUNK];
	uint64_t at = 0;

	while (!gStop) {
		uint64_t first, written;
		ssize_t n = readLog(at, buf, &first, &written);

		if (n < 0) {
			sleep(1);
			continue;
		}

		if (first > at)
			fprintf(f, "\n[rdna4-logd: %" PRIu64 " bytes dropped by the ring]\n", first - at);

		fwrite(buf, 1, (size_t)n, f);
		at = first + (uint64_t)n;

		/* drained: flush, so that a hang or panic loses at most a second */
		if (at == written) {
			fflush(f);
			sleep(1);
		}
	}
}

int main(int argc, char **argv)
{
	const char *dir = "/var/log/rdna4";
	int keep = 10, opt;
	FILE *f;

	while ((opt = getopt(argc, argv, "d:k:")) != -1) {
		switch (opt) {
		case 'd':
			dir = optarg;
			break;
		case 'k':
			keep = atoi(optarg) > 0 ? atoi(optarg) : 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-d dir] [-k keep]\n", argv[0]);
			return 2;
		}
	}

	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);

	mkdir(dir, 0755);
	f = openBootLog(dir);
	if (!f) {
		fprintf(stderr, "rdna4-logd: cannot open a log in %s: %s\n", dir, strerror(errno));
		return 1;
	}

	prune(dir, keep);
	follow(f);
	fclose(f);

	return 0;
}
