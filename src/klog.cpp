/*
 * klog.cpp
 * AMDRadeonRDNA4
 *
 * See klog.hpp. Lines before rdna4LogInit() (none in practice: pluginStart
 * calls it first) go to IOLog only.
 */

#include "klog.hpp"

#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>

namespace
{

char gRing[kRdna4LogRing];
/* Bytes ever appended: the ring holds the last kRdna4LogRing of them. */
size_t gWritten;
IOSimpleLock *gLock;

void append(const char *s, size_t n)
{
	if (!gLock)
		return;

	IOSimpleLockLock(gLock);

	for (size_t i = 0; i < n; i++)
		gRing[(gWritten + i) % kRdna4LogRing] = s[i];

	gWritten += n;
	IOSimpleLockUnlock(gLock);
}

/* The ring in order, oldest byte first, into `copy` as one string. */
size_t snapshot(char *copy)
{
	size_t n, first;

	IOSimpleLockLock(gLock);
	n = gWritten < kRdna4LogRing ? gWritten : kRdna4LogRing;
	first = gWritten - n;

	for (size_t i = 0; i < n; i++)
		copy[i] = gRing[(first + i) % kRdna4LogRing];

	IOSimpleLockUnlock(gLock);
	copy[n] = 0;

	return n;
}

int logSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	char *copy;
	size_t n;
	int err;

	if (!rdna4Root())
		return EPERM;

	if (!gLock)
		return SYSCTL_OUT(req, "", 1);

	copy = static_cast<char *>(IOMalloc(kRdna4LogRing + 1));
	if (!copy)
		return ENOMEM;

	n = snapshot(copy);
	err = SYSCTL_OUT(req, copy, n + 1);
	IOFree(copy, kRdna4LogRing + 1);

	return err;
}

SYSCTL_PROC(_debug, OID_AUTO, rdna4_log,
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, logSysctl,
	    "A", "AMDRadeonRDNA4 log since boot");

/* At most this much a read of debug.rdna4_log_read: the lock is held while it is copied. */
constexpr size_t kReadChunk = 64 * 1024;

/*
 * debug.rdna4_log_read, for a reader following the log (rdna4-logd): given
 * the u64 offset into everything ever logged it has read up to, it gets
 * {u64 offset of the first byte returned, u64 bytes ever logged} and the
 * bytes from there, up to kReadChunk. The first offset is later than the
 * one given when the ring has dropped those bytes already.
 */
int readSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	uint64_t from = 0, head[2];
	char *buf;
	size_t n;
	int err;

	if (!rdna4Root())
		return EPERM;

	if (req->newptr) {
		err = SYSCTL_IN(req, &from, sizeof(from));
		if (err)
			return err;
	}

	if (!gLock)
		return ENOENT;

	buf = static_cast<char *>(IOMalloc(sizeof(head) + kReadChunk));
	if (!buf)
		return ENOMEM;

	IOSimpleLockLock(gLock);

	if (from + kRdna4LogRing < gWritten)
		from = gWritten - kRdna4LogRing;

	if (from > gWritten)
		from = gWritten;

	n = gWritten - from < kReadChunk ? gWritten - from : kReadChunk;
	for (size_t i = 0; i < n; i++)
		buf[sizeof(head) + i] = gRing[(from + i) % kRdna4LogRing];

	head[0] = from;
	head[1] = gWritten;
	IOSimpleLockUnlock(gLock);

	memcpy(buf, head, sizeof(head));
	err = SYSCTL_OUT(req, buf, sizeof(head) + n);
	IOFree(buf, sizeof(head) + kReadChunk);

	return err;
}

SYSCTL_PROC(_debug, OID_AUTO, rdna4_log_read,
	    CTLTYPE_OPAQUE | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, readSysctl,
	    "S", "AMDRadeonRDNA4 log from an offset (rdna4-logd)");

} /* namespace */

bool rdna4Root()
{
	return kauth_cred_issuser(kauth_cred_get());
}

void rdna4Log(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	if (n < 0)
		return;

	if (static_cast<size_t>(n) >= sizeof(line))
		n = sizeof(line) - 1;

	IOLog("%s", line);
	append(line, static_cast<size_t>(n));
}

void rdna4LogInit()
{
	if (gLock)
		return;

	gLock = IOSimpleLockAlloc();
	if (!gLock)
		return;

	sysctl_register_oid(&sysctl__debug_rdna4_log);
	sysctl_register_oid(&sysctl__debug_rdna4_log_read);
}
