/*
 *  rdna4-pvexec.c
 *  AMDRadeonRDNA4
 *
 *  The paravirtual host's compiler service (PVGPU builds, include/rdna4pvjob.h).
 *  A Metal client's compute dispatch reaches the kext as the metallib it
 *  compiled, the buffers it bound and the grid; this program takes each one,
 *  compiles the AIR for gfx1201 (tools/metal/air/compile-metallib.sh, cached
 *  by the metallib and the threadgroup size), runs it on the card through
 *  the compute runtime with copies of the buffers, and writes them back into
 *  the client's memory. The kext completes the client's command buffer when
 *  the job is done.
 *
 *    rdna4-pvexec [-s compile-metallib.sh] [-c cache dir] [-v]
 *
 *  Needs root. Jobs only come in a boot with the compute runtime
 *  (rdna4-compute=7) and the paravirtual host (rdna4-pvgpu=1); in any other
 *  it waits, trying again every few seconds, so that it can run as a LaunchDaemon
 *  (userspace/rdna4-pvexec/install.sh). LLVM for the compiler is taken from
 *  $LLVM/bin or PATH.
 */

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "librdna4.h"

#define MAX_PROGRAMS 64
#define MAX_KERNARGS 64
#define MAX_BUFFER_BYTES (256ull << 20)
#define DISPATCH_TIMEOUT_MS 5000
#define RETRY_SECONDS 5

/* A compiled function, by the hash of its metallib and its threadgroup size. */
typedef struct {
	char key[64];
	rdna4_program_t prog;
	char abi[MAX_KERNARGS][24];
	int nabi;
} program_t;

static const char *gScript = "tools/metal/air/compile-metallib.sh";
static const char *gCache = "/var/tmp/rdna4-pvexec";
static int gVerbose;
static volatile sig_atomic_t gStop;
static program_t gPrograms[MAX_PROGRAMS];
static int gNumPrograms;

static void onSignal(int sig)
{
	(void)sig;
	gStop = 1;
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;

	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ull;

	return h;
}

static void *readFile(const char *path, size_t *bytes)
{
	FILE *f = fopen(path, "rb");
	void *buf = NULL;
	long n;

	if (!f)
		return NULL;

	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
		buf = malloc((size_t)n);
		if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
			free(buf);
			buf = NULL;
		}
		*bytes = (size_t)n;
	}

	fclose(f);

	return buf;
}

static int writeFile(const char *path, const void *buf, size_t bytes)
{
	FILE *f = fopen(path, "wb");
	int ok;

	if (!f)
		return 0;

	ok = fwrite(buf, 1, bytes, f) == bytes;

	return fclose(f) == 0 && ok;
}

/* compile-metallib.sh IN TG DIR; true when DIR/kernel.hsaco is there afterwards. */
static int compile(const char *dir, const uint32_t tg[3])
{
	char in[512], size[48], hsaco[512];
	struct stat st;
	pid_t pid;
	int status;

	snprintf(hsaco, sizeof(hsaco), "%s/kernel.hsaco", dir);
	if (stat(hsaco, &st) == 0)
		return 1;

	snprintf(in, sizeof(in), "%s/in.metallib", dir);
	snprintf(size, sizeof(size), "%u,%u,%u", tg[0], tg[1], tg[2]);

	pid = fork();
	if (pid == 0) {
		execl("/bin/sh", "sh", gScript, in, size, dir, (char *)NULL);
		_exit(127);
	}

	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
		return 0;

	return stat(hsaco, &st) == 0;
}

/* The kernargs of a compiled function, one line each (air2amdgcn --abi). */
static int readAbi(const char *dir, program_t *p)
{
	char path[512], line[64];
	FILE *f;

	snprintf(path, sizeof(path), "%s/kernel.abi", dir);
	f = fopen(path, "r");
	if (!f)
		return 0;

	p->nabi = 0;
	while (p->nabi < MAX_KERNARGS && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		snprintf(p->abi[p->nabi++], sizeof(p->abi[0]), "%s", line);
	}

	fclose(f);

	return p->nabi > 0;
}

/* Compile (or find) and load the job's function for its threadgroup size. */
static program_t *program(rdna4_t *gpu, const RDNA4PvJob *job, const uint8_t *code)
{
	char key[64], dir[512], path[512], name[128] = { 0 };
	program_t *p;
	void *elf, *txt;
	size_t elfBytes, txtBytes;
	kern_return_t kr;

	snprintf(key, sizeof(key), "%016" PRIx64 "-%ux%ux%u", fnv1a(code, job->codeBytes),
	         job->groupSize[0], job->groupSize[1], job->groupSize[2]);

	for (int i = 0; i < gNumPrograms; i++) {
		if (!strcmp(gPrograms[i].key, key))
			return &gPrograms[i];
	}

	if (gNumPrograms == MAX_PROGRAMS)
		return NULL;

	snprintf(dir, sizeof(dir), "%s/%s", gCache, key);
	mkdir(gCache, 0755);
	mkdir(dir, 0755);

	snprintf(path, sizeof(path), "%s/in.metallib", dir);
	if (!writeFile(path, code, job->codeBytes) || !compile(dir, job->groupSize)) {
		fprintf(stderr, "rdna4-pvexec: %s: does not compile (%s)\n", key, dir);
		return NULL;
	}

	snprintf(path, sizeof(path), "%s/kernel.name", dir);
	txt = readFile(path, &txtBytes);
	if (txt) {
		snprintf(name, sizeof(name), "%.*s", (int)txtBytes, (const char *)txt);
		name[strcspn(name, "\n")] = 0;
		free(txt);
	}

	snprintf(path, sizeof(path), "%s/kernel.hsaco", dir);
	elf = readFile(path, &elfBytes);
	p = &gPrograms[gNumPrograms];
	if (!elf || !name[0] || !readAbi(dir, p)) {
		free(elf);
		return NULL;
	}

	kr = rdna4_load(gpu, elf, elfBytes, name, &p->prog);
	free(elf);
	if (kr) {
		fprintf(stderr, "rdna4-pvexec: %s: load: %s\n", key, rdna4_error(kr));
		return NULL;
	}

	snprintf(p->key, sizeof(p->key), "%s", key);
	gNumPrograms++;
	if (gVerbose)
		printf("rdna4-pvexec: %s: kernel %s, %d kernarg entries\n", key, name, p->nabi);

	return p;
}

static const RDNA4PvBuffer *bufferAt(const RDNA4PvJob *job, uint32_t slot)
{
	for (uint32_t i = 0; i < job->bufferCount; i++) {
		if (job->buffers[i].slot == slot)
			return &job->buffers[i];
	}

	return NULL;
}

/* The kernargs as the program's ABI lists them, from the GPU copies of the buffers. */
static int kernargs(const program_t *p, const RDNA4PvJob *job, const rdna4_buffer_t *gpuBufs,
                    uint8_t *out, uint32_t *bytes)
{
	uint32_t at = 0;

	for (int i = 0; i < p->nabi; i++) {
		unsigned slot;

		if (sscanf(p->abi[i], "buffer %u", &slot) == 1) {
			const RDNA4PvBuffer *b = bufferAt(job, slot);
			uint64_t va;

			if (!b)
				return 0;

			va = gpuBufs[b - job->buffers].gpu;
			at = (at + 7) & ~7u;
			memcpy(out + at, &va, 8);
			at += 8;
		} else if (!strcmp(p->abi[i], "grid")) {
			memcpy(out + at, job->threads, 12);
			at += 12;
		} else {
			return 0;   /* threadgroup memory arguments: not yet */
		}
	}

	*bytes = at;

	return 1;
}

/* Every buffer of the job between the client's memory and a GPU copy. */
static int moveBuffers(rdna4_t *gpu, const RDNA4PvJob *job, rdna4_buffer_t *gpuBufs, int toGpu)
{
	for (uint32_t i = 0; i < job->bufferCount; i++) {
		const RDNA4PvBuffer *b = &job->buffers[i];
		void *host = malloc(b->length);
		kern_return_t kr;

		if (!host)
			return 0;

		if (toGpu) {
			kr = rdna4_pv_access(gpu, job->task, b->va, host, b->length, 0);
			if (!kr)
				kr = rdna4_write(gpu, &gpuBufs[i], 0, host, b->length);
		} else {
			kr = rdna4_read(gpu, &gpuBufs[i], 0, host, b->length);
			if (!kr)
				kr = rdna4_pv_access(gpu, job->task, b->va, host, b->length, 1);
		}

		free(host);
		if (kr) {
			fprintf(stderr, "rdna4-pvexec: buffer slot %u: %s\n", b->slot, rdna4_error(kr));
			return 0;
		}
	}

	return 1;
}

/* One job: 0 when it ran. */
static uint32_t runJob(rdna4_t *gpu, const RDNA4PvJob *job)
{
	rdna4_buffer_t gpuBufs[RDNA4_PVJOB_MAX_BUFFERS] = { { 0 } };
	uint8_t args[RDNA4_MAX_KERNARG] = { 0 };
	uint32_t argBytes = 0, groups[3];
	uint8_t *code = NULL;
	program_t *p = NULL;
	uint64_t us = 0;
	uint32_t status = 1;
	kern_return_t kr;

	if (job->codeBytes && job->codeBytes <= RDNA4_PVJOB_MAX_CODE)
		code = malloc(job->codeBytes);

	if (code && !rdna4_pv_access(gpu, job->task, job->codeVa, code, job->codeBytes, 0))
		p = program(gpu, job, code);

	free(code);
	if (!p)
		return status;

	for (uint32_t i = 0; i < job->bufferCount; i++) {
		if (job->buffers[i].length > MAX_BUFFER_BYTES ||
		    rdna4_alloc(gpu, job->buffers[i].length, &gpuBufs[i]))
			goto out;
	}

	if (!kernargs(p, job, gpuBufs, args, &argBytes) || !moveBuffers(gpu, job, gpuBufs, 1))
		goto out;

	for (int d = 0; d < 3; d++)
		groups[d] = (job->threads[d] + job->groupSize[d] - 1) / job->groupSize[d];

	kr = rdna4_dispatch(gpu, &p->prog, groups, job->groupSize, args, argBytes, DISPATCH_TIMEOUT_MS, &us);
	if (kr) {
		fprintf(stderr, "rdna4-pvexec: job %u: dispatch: %s\n", job->id, rdna4_error(kr));
		goto out;
	}

	if (moveBuffers(gpu, job, gpuBufs, 0))
		status = 0;

	if (gVerbose)
		printf("rdna4-pvexec: job %u: %s over %ux%ux%u threads, %u buffer(s), %" PRIu64 " us\n", job->id,
		       p->key, job->threads[0], job->threads[1], job->threads[2], job->bufferCount, us);

out:
	for (uint32_t i = 0; i < job->bufferCount; i++) {
		if (gpuBufs[i].handle)
			rdna4_free(gpu, &gpuBufs[i]);
	}

	return status;
}

/* Take jobs until a signal or the runtime goes away; the reason, for the log. */
static const char *serve(rdna4_t *gpu)
{
	int told = 0;

	for (;;) {
		RDNA4PvJob job;
		kern_return_t kr;

		if (gStop)
			return "stopped";

		kr = rdna4_pv_next(gpu, 1000, &job);
		if (kr == kIOReturnUnsupported)
			return "the kext is not a paravirtual build";

		if (kr && kr != kIOReturnTimeout)
			return rdna4_error(kr);

		if (!told) {
			printf("rdna4-pvexec: waiting for compute dispatches\n");
			told = 1;
		}

		if (kr == kIOReturnTimeout)
			continue;

		rdna4_pv_done(gpu, job.id, runJob(gpu, &job));
	}
}

/* Wait out the retry interval; false when a signal came. */
static int waitRetry(void)
{
	for (int i = 0; i < RETRY_SECONDS && !gStop; i++)
		sleep(1);

	return !gStop;
}

int main(int argc, char **argv)
{
	const char *was = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "s:c:v")) != -1) {
		switch (opt) {
		case 's':
			gScript = optarg;
			break;
		case 'c':
			gCache = optarg;
			break;
		case 'v':
			gVerbose = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-s compile-metallib.sh] [-c cache dir] [-v]\n", argv[0]);
			return 2;
		}
	}

	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* Said once per change, not once a minute. */
	do {
		rdna4_t gpu;
		const char *why;
		kern_return_t kr = rdna4_open(&gpu);

		if (kr) {
			why = "no compute runtime (rdna4-compute=7)";
		} else {
			why = serve(&gpu);
			rdna4_close(&gpu);
			gNumPrograms = 0;
		}

		if (!was || strcmp(was, why))
			printf("rdna4-pvexec: %s; trying again every %d s\n", why, RETRY_SECONDS);

		was = why;
	} while (!gStop && waitRetry());

	return 0;
}
