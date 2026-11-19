//
//  userclient.cpp
//  AMDRadeonRDNA4
//
//  See userclient.hpp. The selectors, their scalar/struct shapes and the
//  dispatch struct are in include/rdna4compute.h; IOUserClient's own
//  externalMethod checks the counts and sizes against kMethods before any
//  of the handlers below runs.
//

#include "userclient.hpp"
#include "klog.hpp"
#include "compute.hpp"

#include <IOKit/IOLib.h>

OSDefineMetaClassAndStructors(RDNA4ComputeService, IOService)
OSDefineMetaClassAndStructors(RDNA4ComputeClient, IOUserClient)

static IOPMPowerState kComputePowerStates[2] = {
	/* IOKit ordinals run from the lowest power state to the highest. */
	{ 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 1, kIOPMDeviceUsable | kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

IOReturn RDNA4ComputeService::setPowerState(unsigned long powerStateOrdinal,
                                             IOService *) {
	uint64_t nowNs = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
	rdna4Log("AMDRadeonRDNA4: power: setPowerState ordinal %lu at %llu ns\n",
	      powerStateOrdinal, nowNs);
	if (!compute)
		return kIOPMAckImplied;
	if (powerStateOrdinal == 0) {
		compute->powerSleepRequest();    // P7: before powerWillSleep takes rtLock, so a client wait holding it ends with Aborted
		compute->powerWillSleep();
	}
	else if (powerStateOrdinal == 1)
		compute->powerDidWake();
	return kIOPMAckImplied;
}

bool RDNA4ComputeService::registerPowerManagement(IOService *provider) {
	PMinit();
	joinPMtree(provider);
	return registerPowerDriver(this, kComputePowerStates, 2) == kIOReturnSuccess;
}

// Who may open (docs/w13-client-access.md): root, with every selector. With rdna4-client-policy=local also the user logged in at the
// console (the native Metal device's apps, docs/native-metal-spike.md), only in a GPU address space of its own (start: rtClientIsolated) and
// only with the selectors a graphics client needs (kLocalSelectors): without the VM a kernel runs in VMID0 and reaches all of VRAM.
static bool localClientsAllowed() {
	char policy[8] = {};
	return PE_parse_boot_argn("rdna4-client-policy", policy, sizeof(policy)) && !strncmp(policy, "local", sizeof(policy));
}

bool RDNA4ComputeClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                      OSDictionary *properties) {
	if (!IOUserClient::initWithTask(owningTask, securityToken, type, properties))
		return false;
	privileged = clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
	if (!privileged && !(localClientsAllowed() && clientHasPrivilege(securityToken, kIOClientPrivilegeLocalUser) == kIOReturnSuccess))
		return false;
	gate = IOLockAlloc();
	task = owningTask;

	return gate != nullptr;
}

void RDNA4ComputeClient::free() {
	if (gate)
		IOLockFree(gate);
	IOUserClient::free();
}

bool RDNA4ComputeClient::start(IOService *provider) {
	auto *svc = OSDynamicCast(RDNA4ComputeService, provider);
	rdna4Step("user client: start");
	if (!svc || !svc->compute || !IOUserClient::start(provider))
		return false;
	compute = svc->compute;
	if (compute->rtOpen(this) != kIOReturnSuccess)
		return false;
	if (!privileged && !compute->rtClientIsolated(this)) {   // a local client in VMID0 would reach all of VRAM
		rdna4Log("AMDRadeonRDNA4: user client: a client that is not root, without an address space of its own (rdna4-vm=1): refused\n");
		compute->rtRelease(this);
		compute = nullptr;
		return false;
	}
	return true;
}

// Once, after the calls in flight: they use the client's buffers, and compute stays set for them.
void RDNA4ComputeClient::release() {
	IOLockLock(gate);
	if (closed) {
		IOLockUnlock(gate);
		return;
	}

	closed = true;
	while (calls)
		IOLockSleep(gate, &calls, THREAD_UNINT);
	IOLockUnlock(gate);

	if (compute)
		compute->rtRelease(this);
}

IOReturn RDNA4ComputeClient::clientClose() {
	release();
	terminate();

	return kIOReturnSuccess;
}

IOReturn RDNA4ComputeClient::clientDied() {
	release();

	return IOUserClient::clientDied();
}

const IOExternalMethodDispatch RDNA4ComputeClient::kMethods[kRDNA4MethodCount] = {
	// function    scalars in  struct in                   scalars out  struct out
	{ sInfo,       0,          0,                          9,           0 },
	{ sAlloc,      1,          0,                          2,           0 },
	{ sFree,       1,          0,                          0,           0 },
	{ sWrite,      4,          0,                          0,           0 },
	{ sRead,       4,          0,                          0,           0 },
	{ sLoad,       2,          kIOUCVariableStructureSize, 9,           0 },
	{ sUnload,     1,          0,                          0,           0 },
	{ sDispatch,   0,          sizeof(RDNA4Dispatch),      1,           0 },
	{ sWaitVBlank, 1,          0,                          2,           0 },
	{ sPresent,    2,          0,                          2,           0 },
	{ sRestore,    0,          0,                          0,           0 },
	{ sAllocHost,  2,          0,                          3,           0 },
	{ sSensors,    0,          0,                          0,           sizeof(RDNA4Sensors) },
	{ sSleepTest,  1,          0,                          0,           0 },
	{ sQuiesce,    0,          0,                          0,           0 },
	{ sPresentAsync, 2,        0,                          1,           0 },
	{ sWaitPresent,  2,        0,                          1,           0 },
	{ sSubmitIb,   3,          0,                          1,           0 },
	{ sWaitFence,  2,          0,                          1,           0 },
	{ sSensorsEx,  0,          0,                          0,           sizeof(RDNA4SensorsEx) },
	{ sSubmitGfxIb, 3,         0,                          1,           0 },
	{ sWaitGfxFence, 2,        0,                          1,           0 },
};
static_assert(kRDNA4MethodCount == 22, "kMethods has one entry per selector");

// What a client that is not root may call: its own buffers, programs and submissions. Not the display (present, restore, vblank), the
// sensors, the sleep test or the quiesce.
static constexpr uint32_t kLocalSelectors = 1u << kRDNA4MethodInfo | 1u << kRDNA4MethodAlloc | 1u << kRDNA4MethodFree |
                                            1u << kRDNA4MethodWrite | 1u << kRDNA4MethodRead | 1u << kRDNA4MethodLoad |
                                            1u << kRDNA4MethodUnload | 1u << kRDNA4MethodDispatch | 1u << kRDNA4MethodAllocHost |
                                            1u << kRDNA4MethodSubmitIb | 1u << kRDNA4MethodWaitFence |
                                            1u << kRDNA4MethodSubmitGfxIb | 1u << kRDNA4MethodWaitGfxFence;

IOReturn RDNA4ComputeClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                            IOExternalMethodDispatch *, OSObject *, void *) {
	if (selector >= kRDNA4MethodCount || !compute)
		return kIOReturnUnsupported;
	if (!privileged && !(kLocalSelectors >> selector & 1))
		return kIOReturnNotPrivileged;

	IOLockLock(gate);
	if (closed) {
		IOLockUnlock(gate);
		return kIOReturnNotOpen;
	}
	calls++;
	IOLockUnlock(gate);

	const IOReturn r = IOUserClient::externalMethod(selector, args,
	                                                const_cast<IOExternalMethodDispatch *>(&kMethods[selector]),
	                                                this, nullptr);

	IOLockLock(gate);
	if (!--calls && closed)
		IOLockWakeup(gate, &calls, false);
	IOLockUnlock(gate);

	return r;
}

static RDNA4ComputeClient *self(OSObject *t) { return static_cast<RDNA4ComputeClient *>(t); }

IOReturn RDNA4ComputeClient::sInfo(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtInfo(t, a->scalarOutput);
}

IOReturn RDNA4ComputeClient::sSensors(OSObject *t, void *, IOExternalMethodArguments *a) {
	if (!a->structureOutput || a->structureOutputSize != sizeof(RDNA4Sensors))
		return kIOReturnBadArgument;
	return self(t)->compute->rtSensors(t, *static_cast<RDNA4Sensors *>(a->structureOutput));
}

IOReturn RDNA4ComputeClient::sSensorsEx(OSObject *t, void *, IOExternalMethodArguments *a) {
	if (!a->structureOutput || a->structureOutputSize != sizeof(RDNA4SensorsEx))
		return kIOReturnBadArgument;
	return self(t)->compute->rtSensorsEx(t, *static_cast<RDNA4SensorsEx *>(a->structureOutput));
}

IOReturn RDNA4ComputeClient::sSleepTest(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtSleepTest(t, static_cast<uint32_t>(a->scalarInput[0]));
}

IOReturn RDNA4ComputeClient::sQuiesce(OSObject *t, void *, IOExternalMethodArguments *) {
	return self(t)->compute->rtQuiesce(t);
}

IOReturn RDNA4ComputeClient::sAlloc(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtAlloc(t, a->scalarInput[0], a->scalarOutput[0], a->scalarOutput[1]);
}

IOReturn RDNA4ComputeClient::sFree(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtFree(t, a->scalarInput[0]);
}

IOReturn RDNA4ComputeClient::sWrite(OSObject *t, void *, IOExternalMethodArguments *a) {
	const uint64_t *in = a->scalarInput;
	return self(t)->compute->rtCopy(t, in[0], in[1], self(t)->task, in[2], in[3], true);
}

IOReturn RDNA4ComputeClient::sRead(OSObject *t, void *, IOExternalMethodArguments *a) {
	const uint64_t *in = a->scalarInput;
	return self(t)->compute->rtCopy(t, in[0], in[1], self(t)->task, in[2], in[3], false);
}

IOReturn RDNA4ComputeClient::sLoad(OSObject *t, void *, IOExternalMethodArguments *a) {
	// The kernel name: 1..RDNA4_MAX_NAME bytes, NUL-terminated.
	const uint32_t n = a->structureInputSize;
	const auto *s = static_cast<const char *>(a->structureInput);
	if (!s || !n || n > RDNA4_MAX_NAME || s[n - 1] != 0)
		return kIOReturnBadArgument;
	char name[RDNA4_MAX_NAME];
	memcpy(name, s, n);
	return self(t)->compute->rtLoad(t, self(t)->task, a->scalarInput[0], a->scalarInput[1], name,
	                                a->scalarOutput);
}

IOReturn RDNA4ComputeClient::sUnload(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtUnload(t, a->scalarInput[0]);
}

IOReturn RDNA4ComputeClient::sDispatch(OSObject *t, void *, IOExternalMethodArguments *a) {
	if (!a->structureInput || a->structureInputSize != sizeof(RDNA4Dispatch))
		return kIOReturnBadArgument;
	return self(t)->compute->rtDispatch(t, *static_cast<const RDNA4Dispatch *>(a->structureInput),
	                                    a->scalarOutput[0]);
}

IOReturn RDNA4ComputeClient::sWaitVBlank(OSObject *t, void *, IOExternalMethodArguments *a) {
	uint64_t count = 0, timeNs = 0;
	if (!self(t)->compute->ihWaitVblank(Pipe::kNone, static_cast<uint32_t>(a->scalarInput[0]), count,
	                                    timeNs))
		return kIOReturnTimeout;
	a->scalarOutput[0] = count;
	a->scalarOutput[1] = timeNs;
	return kIOReturnSuccess;
}

IOReturn RDNA4ComputeClient::sPresent(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtPresent(t, a->scalarInput[0], a->scalarInput[1],
	                                   a->scalarOutput[0], a->scalarOutput[1]);
}

IOReturn RDNA4ComputeClient::sRestore(OSObject *t, void *, IOExternalMethodArguments *) {
	return self(t)->compute->rtRestore(t);
}

IOReturn RDNA4ComputeClient::sAllocHost(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtAllocHost(t, self(t)->task, a->scalarInput[0],
	                                     a->scalarInput[1], a->scalarOutput[0],
	                                     a->scalarOutput[1], a->scalarOutput[2]);
}

IOReturn RDNA4ComputeClient::sPresentAsync(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtPresentAsync(t, a->scalarInput[0], a->scalarInput[1],
	                                       a->scalarOutput[0]);
}

IOReturn RDNA4ComputeClient::sWaitPresent(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtWaitPresent(t, a->scalarInput[0],
	                                      static_cast<uint32_t>(a->scalarInput[1]),
	                                      a->scalarOutput[0]);
}

IOReturn RDNA4ComputeClient::sSubmitIb(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtSubmitIb(t, a->scalarInput[0], a->scalarInput[1],
	                                    a->scalarInput[2], a->scalarOutput[0]);
}

IOReturn RDNA4ComputeClient::sWaitFence(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtWaitFence(t, static_cast<uint32_t>(a->scalarInput[0]),
	                                     static_cast<uint32_t>(a->scalarInput[1]),
	                                     a->scalarOutput[0]);
}

IOReturn RDNA4ComputeClient::sSubmitGfxIb(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtSubmitGfxIb(t, a->scalarInput[0], a->scalarInput[1],
	                                       a->scalarInput[2], a->scalarOutput[0]);
}

IOReturn RDNA4ComputeClient::sWaitGfxFence(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtWaitGfxFence(t, static_cast<uint32_t>(a->scalarInput[0]),
	                                        static_cast<uint32_t>(a->scalarInput[1]),
	                                        a->scalarOutput[0]);
}
