/*
 * pvjob.cpp
 * AMDRadeonRDNA4
 *
 * See pvjob.hpp.
 */

#include "pvjob.hpp"
#include "pvexec.hpp"
#include "pvsurface.hpp"
#include "klog.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>

#define JLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: pvgpu: job: " fmt "\n", ##__VA_ARGS__)

namespace
{

enum class State : uint32_t {
	Idle,       /* no job */
	Pending,    /* run() posted one, the service has not taken it */
	Taken,      /* the service is on it */
	Done,       /* the service ended it; run() collects the status */
};

/* A service that asked for work within this long is taken to be there. */
constexpr uint64_t kServiceAliveMs = 5000;
constexpr size_t kChunk = 4096;

IOLock *gLock;
State gState = State::Idle;
RDNA4PvJob gJob;
uint32_t gNextId = 1;
uint32_t gStatus;
uint64_t gLastAskMs;

uint64_t nowMs()
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);

	return ns / 1000000ull;
}

uint64_t deadlineIn(uint32_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, kMillisecondScale, &deadline);

	return deadline;
}

bool lock()
{
	if (!gLock)
		gLock = IOLockAlloc();

	if (!gLock)
		return false;

	IOLockLock(gLock);

	return true;
}

bool serviceAlive()
{
	return gLastAskMs && nowMs() - gLastAskMs < kServiceAliveMs;
}

} /* namespace */

bool PvJob::run(RDNA4PvJob &job, uint32_t timeoutMs)
{
	const uint64_t deadline = deadlineIn(timeoutMs);
	bool ok;

	if (!lock())
		return false;

	if (gState != State::Idle || !serviceAlive()) {
		IOLockUnlock(gLock);
		return false;
	}

	job.id = gNextId++;
	gJob = job;
	gState = State::Pending;
	IOLockWakeup(gLock, &gState, false);

	while (gState != State::Done) {
		if (IOLockSleepDeadline(gLock, &gState, deadline, THREAD_UNINT) == THREAD_TIMED_OUT)
			break;
	}

	ok = gState == State::Done && gStatus == 0;
	if (gState != State::Done)
		JLOG("job %u: no answer in %u ms", job.id, timeoutMs);

	gState = State::Idle;
	IOLockUnlock(gLock);

	return ok;
}

IOReturn PvJob::next(uint32_t timeoutMs, RDNA4PvJob &out)
{
	const uint64_t deadline = deadlineIn(timeoutMs);

	if (!lock())
		return kIOReturnNoMemory;

	gLastAskMs = nowMs();

	while (gState != State::Pending) {
		const int r = IOLockSleepDeadline(gLock, &gState, deadline, THREAD_ABORTSAFE);

		if (r == THREAD_TIMED_OUT || r == THREAD_INTERRUPTED)
			break;
	}

	if (gState != State::Pending) {
		IOLockUnlock(gLock);
		return kIOReturnTimeout;
	}

	out = gJob;
	gState = State::Taken;
	gLastAskMs = nowMs();
	IOLockUnlock(gLock);

	return kIOReturnSuccess;
}

IOReturn PvJob::done(uint32_t id, uint32_t status)
{
	if (!lock())
		return kIOReturnNoMemory;

	if (gState != State::Taken || gJob.id != id) {
		IOLockUnlock(gLock);
		return kIOReturnNotFound;
	}

	gStatus = status;
	gState = State::Done;
	gLastAskMs = nowMs();
	IOLockWakeup(gLock, &gState, false);
	IOLockUnlock(gLock);

	return kIOReturnSuccess;
}

/* A chunk of the client's memory or of an IOSurface. */
static bool guest(uint32_t task, uint64_t va, uint8_t *chunk, size_t n, uint32_t flags)
{
	const bool write = flags & RDNA4_PVACCESS_WRITE;

	if (flags & RDNA4_PVACCESS_SURFACE)
		return PvSurface::access(task, va, chunk, n, write);

	return write ? PvExec::writeGpu(task, va, chunk, n) : PvExec::readGpu(task, va, chunk, n);
}

IOReturn PvJob::access(uint32_t task, uint64_t va, task_t userTask, uint64_t user, uint64_t len, uint32_t flags)
{
	const bool write = flags & RDNA4_PVACCESS_WRITE;
	IOMemoryDescriptor *md;
	uint8_t *chunk;
	IOReturn ret = kIOReturnSuccess;

	if (!len || len > RDNA4_PVJOB_MAX_CODE * 64ull)
		return kIOReturnBadArgument;

	md = IOMemoryDescriptor::withAddressRange(user, len, write ? kIODirectionOut : kIODirectionIn, userTask);
	if (!md)
		return kIOReturnNoMemory;

	chunk = static_cast<uint8_t *>(IOMalloc(kChunk));
	if (!chunk || md->prepare() != kIOReturnSuccess) {
		if (chunk)
			IOFree(chunk, kChunk);
		md->release();
		return kIOReturnVMError;
	}

	for (uint64_t off = 0; off < len && ret == kIOReturnSuccess; off += kChunk) {
		const size_t n = len - off < kChunk ? static_cast<size_t>(len - off) : kChunk;

		if (write) {
			if (md->readBytes(off, chunk, n) != n || !guest(task, va + off, chunk, n, flags))
				ret = kIOReturnIOError;
		} else {
			if (!guest(task, va + off, chunk, n, flags) || md->writeBytes(off, chunk, n) != n)
				ret = kIOReturnIOError;
		}
	}

	md->complete();
	md->release();
	IOFree(chunk, kChunk);

	return ret;
}
