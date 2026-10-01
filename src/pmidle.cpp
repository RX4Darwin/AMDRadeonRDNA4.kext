//
//  pmidle.cpp
//
//  Power workstream items that do not depend on the VM client blocker (docs/power-gfx.md, plan P2 and P7):
//
//  P2  idle accounting, boot-arg rdna4-gfxidle=1 (default off). Pure software: a counter of synchronous client operations (IdleUse, taken
//      under rtLock around the runtime selectors that use the GPU), the outstanding client fences (compute IBs and gfx IBs, retired by a 50 ms
//      poll of the fence dwords in host memory, NOT by a caller's wait), and "nothing used the GPU for 100 ms" (amdgpu's delay,
//      amdgpu_gfx.c AMDGPU_GFX_OFF_DELAY) make the runtime idle. A transition is one log line and the registry property Compute,GFXIdle.
//      It sends no SMU message and touches no GC register: it only makes the idle state observable, so that a later step can depend on it.
//
//  P7  sleep/wake hardening. sleepRequested is set by the power callback before it takes rtLock; waits that hold rtLock poll it and return
//      kIOReturnAborted at once (the timeout they replace would have wedged the gfx ring). gfxSleepDrain gives client gfx IBs 100 ms to
//      finish before PFP/ME are halted and then drops them without wedging anything (the wake re-initialises the ring). The wake skips the
//      bring-up *tests* (G3/G4 draws, the gfx client self-test, the flip test) unless rdna4-resume-tests=1.
//

#include "compute.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <libkern/c++/OSObject.h>
#include <pexpert/pexpert.h>

#define ILOG(fmt, ...)  IOLog("RDNA4FB: idle: " fmt "\n", ## __VA_ARGS__)
#define PLOG(fmt, ...)  IOLog("RDNA4FB: power: " fmt "\n", ## __VA_ARGS__)

namespace {

constexpr uint32_t kIdleDelayMs = 100;       // amdgpu's GFXOFF delay: the definition of idle
constexpr uint32_t kIdlePollMs = 50;

class RDNA4IdleContext : public OSObject {
	OSDeclareDefaultStructors(RDNA4IdleContext);

public:
	RDNA4Compute *compute { nullptr };
	bool init() override { return OSObject::init(); }
};

OSDefineMetaClassAndStructors(RDNA4IdleContext, OSObject);

uint64_t nowNs() {
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

}  // namespace

bool RDNA4Compute::requestedGfxIdle() {
	uint32_t v = 0;
	return PE_parse_boot_argn("rdna4-gfxidle", &v, sizeof(v)) && v != 0;
}

// Default ON: it only changes what happens while a sleep is being requested (the wait returns Aborted instead of running to its timeout,
// and every selector of an old connection returns Aborted after the wake anyway). rdna4-sleepabort=0 restores the old behaviour.
bool RDNA4Compute::requestedSleepAbort() {
	uint32_t v = 1;
	return !(PE_parse_boot_argn("rdna4-sleepabort", &v, sizeof(v)) && v == 0);
}

bool RDNA4Compute::requestedResumeTests() {
	uint32_t v = 0;
	return PE_parse_boot_argn("rdna4-resume-tests", &v, sizeof(v)) && v != 0;
}

// ---------------------------------------------------------------------------
// P2: idle accounting
// ---------------------------------------------------------------------------

void RDNA4Compute::idleReport(const char *why) {
	if (!env.owner)
		return;
	const uint64_t now = nowNs();
	char line[200];
	snprintf(line, sizeof(line), "%s for %llu ms (%s); total busy %llu ms idle %llu ms, %u transitions; last use: %s",
	         idleBusy ? "busy" : "idle", (now - idleStateAbs) / 1000000ull, why,
	         (idleBusyNs + (idleBusy ? now - idleStateAbs : 0)) / 1000000ull,
	         (idleIdleNs + (idleBusy ? 0 : now - idleStateAbs)) / 1000000ull, idleTransitions, idleLastWhat[0] ? idleLastWhat : "none");
	env.owner->setProperty("Compute,GFXIdle", line);
}

// A use of the GPU by a client (rtLock held): the accounting is busy again.
void RDNA4Compute::idleTouchLocked(const char *what) {
	const uint64_t now = nowNs();
	idleLastUseAbs = now;
	if (what)
		strlcpy(idleLastWhat, what, sizeof(idleLastWhat));
	if (idleBusy)
		return;
	idleIdleNs += now - idleStateAbs;
	if (idleTransitions < 64 || !(idleTransitions & 63))
		ILOG("idle -> busy after %llu ms idle (%s)", (now - idleStateAbs) / 1000000ull, idleLastWhat);
	idleBusy = true;
	idleStateAbs = now;
	idleTransitions++;
	if (idleTransitions <= 32 || !(idleTransitions & 7))
		idleReport("idle -> busy");
	if (idleTimer)
		idleTimer->setTimeoutMS(kIdlePollMs);
}

void RDNA4Compute::idleBegin(const char *what) {
	if (!idleOn)
		return;
	idleSync++;
	idleTouchLocked(what);
}

void RDNA4Compute::idleEnd() {
	if (!idleOn)
		return;
	if (idleSync)
		idleSync--;
	idleTouchLocked(nullptr);
}

// rtLock held. The fences are retired here, by the poll: a client that submits and never waits must not keep the runtime busy for ever.
void RDNA4Compute::idleEvaluateLocked() {
	bool outstanding = gfxClientPending != 0;
	for (RtClient &c : clients) {
		if (!c.active || c.aborted)
			continue;
		retireIbFences(c);
		gfxClientRetire(c);
		outstanding = outstanding || c.ibOutstanding || c.gfxOutstanding;
	}
	const uint64_t now = nowNs();
	const bool recent = now - idleLastUseAbs < static_cast<uint64_t>(kIdleDelayMs) * 1000000ull;
	const bool busyNow = !rtReady || bringupRunning || powerSleeping || idleSync || outstanding || recent;
	if (busyNow || !idleBusy)
		return;
	idleBusyNs += now - idleStateAbs;
	if (idleTransitions < 64 || !(idleTransitions & 63))
		ILOG("busy -> idle after %llu ms busy (last use: %s, no operation in progress, no fence outstanding, %u ms since the last use)",
		     (now - idleStateAbs) / 1000000ull, idleLastWhat[0] ? idleLastWhat : "bring-up", kIdleDelayMs);
	idleBusy = false;
	idleStateAbs = now;
	idleTransitions++;
	if (idleTransitions <= 32 || !(idleTransitions & 7))
		idleReport("busy -> idle");
}

void RDNA4Compute::idleTick() {
	if (!idleOn || !idleTimer)
		return;
	if (!rtLock || !IOLockTryLock(rtLock)) {
		idleTimer->setTimeoutMS(kIdlePollMs);       // never wait behind a client operation
		return;
	}
	idleEvaluateLocked();
	const bool again = idleBusy;
	IOLockUnlock(rtLock);
	if (again)
		idleTimer->setTimeoutMS(kIdlePollMs);
}

void RDNA4Compute::idleTimerAction(OSObject *owner, IOTimerEventSource *) {
	auto *context = static_cast<RDNA4IdleContext *>(owner);
	if (context && context->compute)
		context->compute->idleTick();
}

// Called once bring-up has published the runtime (and again after a wake): create the timer if needed, and count from now.
void RDNA4Compute::idleStart() {
	idleOn = requestedGfxIdle();
	if (!idleOn)
		return;
	const uint64_t now = nowNs();
	if (!idleTimer) {
		if (!env.owner || !(idleWorkLoop = env.owner->getWorkLoop())) {
			ILOG("accounting disabled: no work loop for the poll timer");
			idleOn = false;
			return;
		}
		auto *context = OSTypeAlloc(RDNA4IdleContext);
		if (!context || !context->init()) {
			OSSafeReleaseNULL(context);
			idleWorkLoop = nullptr;
			idleOn = false;
			return;
		}
		context->compute = this;
		idleContext = context;
		idleTimer = IOTimerEventSource::timerEventSource(context, idleTimerAction);
		if (!idleTimer || idleWorkLoop->addEventSource(idleTimer) != kIOReturnSuccess) {
			OSSafeReleaseNULL(idleTimer);
			context->compute = nullptr;
			context->release();
			idleContext = nullptr;
			idleWorkLoop = nullptr;
			idleOn = false;
			ILOG("accounting disabled: the poll timer could not be installed");
			return;
		}
	}
	if (rtLock)
		IOLockLock(rtLock);
	idleBusy = true;
	idleSync = 0;
	idleLastUseAbs = idleStateAbs = now;
	idleLastWhat[0] = '\0';
	if (rtLock)
		IOLockUnlock(rtLock);
	ILOG("accounting on (rdna4-gfxidle=1): idle = no client operation, no outstanding client fence and %u ms without use; software only, no SMU message; "
	     "registry property Compute,GFXIdle", kIdleDelayMs);
	idleTimer->setTimeoutMS(kIdlePollMs);
}

void RDNA4Compute::idleStop() {
	if (idleTimer) {
		idleTimer->cancelTimeout();
		if (idleWorkLoop)
			idleWorkLoop->removeEventSource(idleTimer);
		idleTimer->release();
		idleTimer = nullptr;
	}
	if (idleContext) {
		static_cast<RDNA4IdleContext *>(idleContext)->compute = nullptr;
		idleContext->release();
		idleContext = nullptr;
	}
	idleWorkLoop = nullptr;
}

// ---------------------------------------------------------------------------
// P7: sleep/wake hardening
// ---------------------------------------------------------------------------

// The very first thing a power callback does, BEFORE powerWillSleep takes rtLock: a client waiting inside a runtime call holds rtLock for up to
// RDNA4_MAX_TIMEOUT_MS (10 s), so the sleep handler would sit behind it, and a wait that then timed out would wedge the gfx ring.
void RDNA4Compute::powerSleepRequest() {
	sleepAbortOn = requestedSleepAbort();
	__atomic_store_n(&sleepRequested, 1u, __ATOMIC_RELEASE);
	idleStop();   // P2: no poll timer across the sleep. NOT under rtLock: removing an event source from the work loop can wait for the work-loop thread, and
	              // that thread may be inside an action that blocks on rtLock (stopPresentationTimer is likewise only ever called without it).
	PLOG("sleep requested: %s", sleepAbortOn ? "client waits that hold the runtime lock now return Aborted (rdna4-sleepabort=0 disables)"
	                                          : "rdna4-sleepabort=0: waits run to their own timeout");
}

void RDNA4Compute::powerSleepClear() {
	__atomic_store_n(&sleepRequested, 0u, __ATOMIC_RELEASE);
}

// powerWillSleep, rtLock held, before PFP/ME are halted. New client gfx submissions are already refused (gfxClientReady: powerSleeping).
void RDNA4Compute::gfxSleepDrain() {
	const uint32_t pending = gfxClientPending;
	if (!gfxMode || !pending) {
		PLOG("gfx: no client gfx IB in flight at sleep");
		return;
	}
	PLOG("gfx: %u client gfx IB(s) in flight at sleep; waiting up to 100 ms", pending);
	for (uint32_t ms = 0; ms < 100; ms++) {
		uint32_t left = 0;
		for (RtClient &c : clients) {
			if (!c.active || c.aborted)
				continue;
			gfxClientRetire(c);
			left += c.gfxOutstanding;
		}
		if (!left && !gfxClientPending) {
			PLOG("gfx: client gfx IBs finished after %u ms", ms);
			return;
		}
		IOSleep(1);
	}
	// Dropped, not wedged: the ring is about to be reset by the wake's bring-up anyway, and the old connections only ever see Aborted.
	uint32_t dropped = 0;
	for (RtClient &c : clients) {
		dropped += c.gfxOutstanding;
		c.gfxOutstanding = 0;
	}
	gfxClientPending = 0;
	PLOG("gfx: %u client gfx IB(s) did not finish within 100 ms; dropped (their fences never signal; the connections are Aborted after the wake)",
	     dropped);
}
