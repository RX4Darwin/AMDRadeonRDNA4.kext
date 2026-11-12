// pathlog: E1c helper (docs/metal-spike.md s.11.4). Injected with DYLD_INSERT_LIBRARIES into the census tool, it logs the file-system paths the process
// looks up that can concern a Metal driver bundle, including the lookups that FAIL: Recovery has neither the unified log nor fs_usage, and a
// failed lookup leaves no dlopen for DYLD_PRINT_APIS. Interposes the libc entry points CFBundle/NSBundle use; nothing else is changed.
// Output: stderr, "RDNA4PATH|<pid> <call> <path> -> <result> [errno]". Only paths that contain one of the needles below are logged (the rest is noise),
// at most kMax lines.
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define INTERPOSE(replacement, replacee) \
	__attribute__((used)) static struct { const void *r; const void *o; } interpose_##replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void *)(unsigned long)&replacement, (const void *)(unsigned long)&replacee }

static atomic_uint gLines;
enum { kMax = 4000 };

static int interesting(const char *p) {
	if (!p)
		return 0;
	static const char *needles[] = { ".bundle", "GPUBundles", "MTLDriver", "AMDMTL", "RDNA4", "Extensions", "PlugIns", "Metal", NULL };
	for (int i = 0; needles[i]; i++)
		if (strstr(p, needles[i]))
			return 1;
	return 0;
}

static void note(const char *call, const char *path, long result, int err) {
	if (!interesting(path) || atomic_fetch_add(&gLines, 1) >= kMax)
		return;
	if (result < 0)
		fprintf(stderr, "RDNA4PATH|%d %s %s -> %ld errno %d\n", getpid(), call, path, result, err);
	else
		fprintf(stderr, "RDNA4PATH|%d %s %s -> %ld\n", getpid(), call, path, result);
}

static int my_open(const char *path, int flags, ...) {
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	int r = open(path, flags, mode);
	int e = errno;
	note("open", path, r, e);
	errno = e;
	return r;
}
static int my_openat(int fd, const char *path, int flags, ...) {
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	int r = openat(fd, path, flags, mode);
	int e = errno;
	note("openat", path, r, e);
	errno = e;
	return r;
}
static int my_stat(const char *path, struct stat *st) {
	int r = stat(path, st);
	int e = errno;
	note("stat", path, r, e);
	errno = e;
	return r;
}
static int my_lstat(const char *path, struct stat *st) {
	int r = lstat(path, st);
	int e = errno;
	note("lstat", path, r, e);
	errno = e;
	return r;
}
static int my_fstatat(int fd, const char *path, struct stat *st, int flag) {
	int r = fstatat(fd, path, st, flag);
	int e = errno;
	note("fstatat", path, r, e);
	errno = e;
	return r;
}
static int my_access(const char *path, int mode) {
	int r = access(path, mode);
	int e = errno;
	note("access", path, r, e);
	errno = e;
	return r;
}
static int my_getattrlist(const char *path, void *al, void *buf, size_t sz, unsigned long opt) {
	int r = getattrlist(path, al, buf, sz, (unsigned)opt);
	int e = errno;
	note("getattrlist", path, r, e);
	errno = e;
	return r;
}
static DIR *my_opendir(const char *path) {
	DIR *d = opendir(path);
	int e = errno;
	note("opendir", path, d ? 0 : -1, e);
	errno = e;
	return d;
}
static void *my_dlopen(const char *path, int mode) {
	void *h = dlopen(path, mode);
	note("dlopen", path, h ? 0 : -1, 0);
	return h;
}

INTERPOSE(my_open, open);
INTERPOSE(my_openat, openat);
INTERPOSE(my_stat, stat);
INTERPOSE(my_lstat, lstat);
INTERPOSE(my_fstatat, fstatat);
INTERPOSE(my_access, access);
INTERPOSE(my_getattrlist, getattrlist);
INTERPOSE(my_opendir, opendir);
INTERPOSE(my_dlopen, dlopen);
