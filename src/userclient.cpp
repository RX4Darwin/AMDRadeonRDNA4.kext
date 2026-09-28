//
//  userclient.cpp
//  RDNA4FB
//
//  See userclient.hpp. The selectors, their scalar/struct shapes and the
//  dispatch struct are in include/rdna4compute.h; IOUserClient's own
//  externalMethod checks the counts and sizes against kMethods before any
//  of the handlers below runs.
//

#include "userclient.hpp"
#include "compute.hpp"

#include <IOKit/IOLib.h>

OSDefineMetaClassAndStructors(RDNA4ComputeService, IOService)
OSDefineMetaClassAndStructors(RDNA4ComputeClient, IOUserClient)

// A kernel runs in VMID0 and can reach all of VRAM: only root may open.
bool RDNA4ComputeClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                      OSDictionary *properties) {
	if (!IOUserClient::initWithTask(owningTask, securityToken, type, properties))
		return false;
	if (clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return false;
	task = owningTask;
	return true;
}

bool RDNA4ComputeClient::start(IOService *provider) {
	auto *svc = OSDynamicCast(RDNA4ComputeService, provider);
	if (!svc || !svc->compute || !IOUserClient::start(provider))
		return false;
	compute = svc->compute;
	return compute->rtOpen(this) == kIOReturnSuccess;
}

IOReturn RDNA4ComputeClient::clientClose() {
	if (compute)
		compute->rtRelease(this);
	compute = nullptr;
	terminate();
	return kIOReturnSuccess;
}

IOReturn RDNA4ComputeClient::clientDied() {
	if (compute)
		compute->rtRelease(this);
	compute = nullptr;
	return IOUserClient::clientDied();
}

const IOExternalMethodDispatch RDNA4ComputeClient::kMethods[kRDNA4MethodCount] = {
	// function    scalars in  struct in                   scalars out  struct out
	{ sInfo,       0,          0,                          9,           0 },
	{ sAlloc,      1,          0,                          2,           0 },
	{ sFree,       1,          0,                          0,           0 },
	{ sWrite,      4,          0,                          0,           0 },
	{ sRead,       4,          0,                          0,           0 },
	{ sLoad,       2,          kIOUCVariableStructureSize, 8,           0 },
	{ sUnload,     1,          0,                          0,           0 },
	{ sDispatch,   0,          sizeof(RDNA4Dispatch),      1,           0 },
};

IOReturn RDNA4ComputeClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                            IOExternalMethodDispatch *, OSObject *, void *) {
	if (selector >= kRDNA4MethodCount || !compute)
		return kIOReturnUnsupported;
	return IOUserClient::externalMethod(selector, args,
	                                    const_cast<IOExternalMethodDispatch *>(&kMethods[selector]),
	                                    this, nullptr);
}

static RDNA4ComputeClient *self(OSObject *t) { return static_cast<RDNA4ComputeClient *>(t); }

IOReturn RDNA4ComputeClient::sInfo(OSObject *t, void *, IOExternalMethodArguments *a) {
	return self(t)->compute->rtInfo(t, a->scalarOutput);
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
