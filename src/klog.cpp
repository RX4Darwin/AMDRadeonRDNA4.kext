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

} /* namespace */

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
	if (gLock)
		sysctl_register_oid(&sysctl__debug_rdna4_log);
}
