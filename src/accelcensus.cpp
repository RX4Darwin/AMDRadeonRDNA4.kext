//
//  accelcensus.cpp
//  AMDRadeonRDNA4
//
//  See accelcensus.hpp.
//

#include "accelcensus.hpp"
#include "klog.hpp"
#ifdef AMDRadeonRDNA4_PVGPU
#include "pvgpu.hpp"
#endif

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <sys/errno.h>
#include <sys/sysctl.h>

extern "C" {
int proc_selfpid(void);
void proc_selfname(char *buf, int size);
}

OSDefineMetaClassAndStructors(RDNA4AccelCensusClient, IOUserClient)

namespace {

uint32_t gLevel { 0 };
RDNA4AccelCensus *gService { nullptr };
uint32_t gLines { 0 };
uint32_t gClients { 0 };
constexpr uint32_t kMaxLines = 2500;             // kernel log lines per boot
uint32_t gNoise { 0 };                           // generic registry-walk reads that are not logged (E1: 132 'exclave-assigned' alone)
char gRegistry[6144] {};                          // the same entries, compact, in the registry property CensusLog
uint32_t gRegistryLen { 0 };
bool gRegistryFull { false };
IOLock *gLock { nullptr };

// One census entry: the kernel log and the bounded registry buffer, prefixed with the calling process. Kernel threads (pid 0) are IOKit itself.
// `flush`: also refresh the registry property. Not from the property-read overrides: IOKit may call those with a registry property lock held,
// and setProperty takes the same lock.
void census(bool flush, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void census(bool flush, const char *fmt, ...) {
	if (gLock)
		IOLockLock(gLock);
	if (gLines >= kMaxLines) {
		if (gLines == kMaxLines)
			rdna4Log("AMDRadeonRDNA4: accelcensus: line budget (%u) used up\n", kMaxLines);
		gLines++;
		if (gLock)
			IOLockUnlock(gLock);
		return;
	}
	gLines++;
	char name[32] = {};
	proc_selfname(name, sizeof(name));
	char body[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);
	rdna4Log("AMDRadeonRDNA4: accelcensus: [%s/%d] %s\n", name, proc_selfpid(), body);
	if (!gRegistryFull) {
		char entry[240];
		const int n = snprintf(entry, sizeof(entry), "[%s/%d] %s ## ", name, proc_selfpid(), body);
		if (n > 0 && gRegistryLen + static_cast<uint32_t>(n) + 12 < sizeof(gRegistry)) {
			memcpy(gRegistry + gRegistryLen, entry, static_cast<size_t>(n));
			gRegistryLen += static_cast<uint32_t>(n);
			gRegistry[gRegistryLen] = '\0';
		} else {
			gRegistryFull = true;
			strlcat(gRegistry, "...(full)", sizeof(gRegistry));
		}
	}
	// The property is refreshed outside gCensusLock: setProperty takes a registry lock that the property-read callers may already hold.
	OSString *snapshot = flush && gService ? OSString::withCString(gRegistry) : nullptr;
	if (gLock)
		IOLockUnlock(gLock);
	if (snapshot) {
		gService->setProperty("CensusLog", snapshot);
		snapshot->release();
	}
}

bool fromUser() { return proc_selfpid() != 0; }

// Property reads that every registry walk does to every node (E1 level 1: exclave-assigned x132, IOUserServiceProperties, BSD Name,
// IOPlatform*Action). They never carry information about the accelerator and ate a fifth of the line budget; they are only counted.
bool noisyKey(const char *k) {
	return !strcmp(k, "exclave-assigned") || !strcmp(k, "IOUserServiceProperties") || !strcmp(k, "BSD Name") || !strncmp(k, "IOPlatform", 10);
}

void hexBytes(char *out, size_t cap, const void *p, size_t n) {
	out[0] = '\0';
	const uint8_t *b = static_cast<const uint8_t *>(p);
	size_t at = 0;
	for (size_t i = 0; i < n && at + 3 < cap; i++)
		at += static_cast<size_t>(snprintf(out + at, cap - at, "%02x", b[i]));
}

} // namespace

namespace {
const OSMetaClass *gClaimedMeta { nullptr };    // IOAccelerator's metaclass, found by name when the service is published
}

RDNA4AccelCensus::RDNA4AccelCensus() : IOService(gClaimedMeta) { }
RDNA4AccelCensus::~RDNA4AccelCensus() { }
const OSMetaClass *RDNA4AccelCensus::getMetaClass() const { return gClaimedMeta; }

uint32_t RDNA4AccelCensus::level() { return gLevel; }

// The properties Apple's AMD accelerators publish (docs/metal-spike.md s.1.4: a live Navi14 registry dump). The plug-in names are deliberately ones
// that exist nowhere, so that the loader's failure names what it looked for.
bool RDNA4AccelCensus::publish(IOService *provider, uint32_t level) {
	if (gService || !provider || !level)
		return false;
	const OSSymbol *name = OSSymbol::withCString("IOAccelerator");
	gClaimedMeta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
	OSSafeReleaseNULL(name);
	if (!gClaimedMeta) {
		rdna4Log("AMDRadeonRDNA4: accelcensus: IOAccelerator is not a registered class (IOGraphicsFamily not loaded?); no census\n");
		return false;
	}
	gLevel = level;
	gLock = IOLockAlloc();
	auto *svc = new RDNA4AccelCensus;
	if (!svc || !svc->init()) {
		OSSafeReleaseNULL(svc);
		rdna4Log("AMDRadeonRDNA4: accelcensus: could not create the service\n");
		return false;
	}
	svc->setName(level == 3 ? "RDNA4NativeMetal" : "RDNA4AccelCensus");
	svc->setProperty("IOUserClientClass", "RDNA4AccelCensusClient");
	svc->setProperty("IOMatchCategory", "IOAccelerator");
	if (level == 3) {   // Metal's loader: gpu_bundle_find_trusted("AMDRadeonRDNA4Metal"), the class a subclass of _MTLDevice (docs/native-metal-spike.md)
		svc->setProperty("MetalPluginName", "AMDRadeonRDNA4Metal");
		svc->setProperty("MetalPluginClassName", "RDNA4MTLDevice");
	} else {
		svc->setProperty("MetalPluginName", "RDNA4CensusMTLDriver");
		svc->setProperty("MetalPluginClassName", "RDNA4CensusMtlDevice");
		svc->setProperty("MetalStatisticsName", "RDNA4CensusStatistics");
		svc->setProperty("IOGLBundleName", "RDNA4CensusGLDriver");
		svc->setProperty("IOOCDBundleName", "RDNA4CensusGLDriver");
		svc->setProperty("GPURawCounterBundleName", "RDNA4CensusRawCounterPlugin");
		svc->setProperty("GPURawCounterPluginClassName", "RDNA4CensusRawCounterSourceGroup");
		svc->setProperty("IODVDBundleName", "RDNA4CensusVADriver");
	}
	svc->setProperty("IOAccelRevision", static_cast<uint64_t>(2), 32);
	svc->setProperty("CensusLevel", static_cast<uint64_t>(level), 32);
	svc->setProperty("CensusLog", "");
	if (!svc->attach(provider)) {
		svc->release();
		rdna4Log("AMDRadeonRDNA4: accelcensus: could not attach to the GPU\n");
		return false;
	}
	gService = svc;
	svc->registerService();
	svc->release();            // the registry keeps it
	rdna4Log("AMDRadeonRDNA4: accelcensus: IOAccelerator service published under %s (level %u: %s); no GPU action\n", provider->getName(), level,
	      level == 2 ? "every selector succeeds with zeroed outputs" : level == 3 ? "the native-Metal spike, every call refused" : "every call refused");
	return true;
}

namespace {
IOService *gNativeGpu { nullptr };   // retained once, kept for the boot
int gRoute { 0 };                    // the paravirtual accelerator's Metal plug-in: 0 Apple's, 2 our router (nobody native), 3 WindowServer native
IONotifier *gPvNotifier { nullptr };

// The native desktop (docs/native-metal-spike.md): the paravirtual accelerator, which drives the displays and so is WindowServer's, gets our
// bundle as its Metal plug-in, class RDNA4MTLRouter. The router makes our device in the processes RDNA4NativeProcesses names (and those
// started with RDNA4_NATIVE_MTL=1), Apple's paravirtual device (RDNA4AppleMetalPluginName / ClassName, its own names kept here) elsewhere.
// Metal reads these when a process makes its devices: running processes keep theirs.
void routePv(IOService *acc, int route)
{
	if (!acc->getProperty("RDNA4AppleMetalPluginName")) {
		if (OSObject *name = acc->getProperty("MetalPluginName"))
			acc->setProperty("RDNA4AppleMetalPluginName", name);
		if (OSObject *cls = acc->getProperty("MetalPluginClassName"))
			acc->setProperty("RDNA4AppleMetalPluginClassName", cls);
	}
	if (route) {
		acc->setProperty("MetalPluginName", "AMDRadeonRDNA4Metal");
		acc->setProperty("MetalPluginClassName", "RDNA4MTLRouter");
		acc->setProperty("RDNA4NativeProcesses", route == 3 ? "WindowServer" : "");
	} else {
		if (OSObject *name = acc->getProperty("RDNA4AppleMetalPluginName"))
			acc->setProperty("MetalPluginName", name);
		if (OSObject *cls = acc->getProperty("RDNA4AppleMetalPluginClassName"))
			acc->setProperty("MetalPluginClassName", cls);
		acc->removeProperty("RDNA4NativeProcesses");
	}
#ifdef AMDRadeonRDNA4_PVGPU
	pvFramesInMemory(route == 3);   // the frames are the native device's: scanned out of their memory, not out of the host's copies
#endif
	rdna4Log("AMDRadeonRDNA4: accelcensus: %s's Metal plug-in: %s\n", acc->getName(),
	         route == 3 ? "our router, WindowServer on the native device" : route ? "our router, nobody native" : "Apple's");
}

bool pvPublished(void *, void *, IOService *acc, IONotifier *)
{
	if (gRoute)
		routePv(acc, gRoute);
	return true;
}

// debug.rdna4_nativemtl: 1 publishes the native-Metal spike's service (level 3) under the GPU; 2 routes the paravirtual accelerator through
// our router, nobody native; 3 the same, WindowServer native (from its next start); 4 gives it Apple's plug-in back. Reads the route (2, 3)
// while routed, else 1 once the service is published, else 0.
int nativeMtlSysctl SYSCTL_HANDLER_ARGS
{
	int value = gRoute ?: gService && gLevel == 3 ? 1 : 0;
	const int err = sysctl_handle_int(oidp, &value, 0, req);

	if (err || !req->newptr)
		return err;
	if (value < 1 || value > 4)
		return EINVAL;
	if (value == 1)
		return gService && gLevel == 3 ? 0 : RDNA4AccelCensus::publish(gNativeGpu, 3) ? 0 : EBUSY;
	gRoute = value == 4 ? 0 : value;
	OSDictionary *match = IOService::serviceMatching("AppleParavirtAccelerator");
	IOService *acc = match ? IOService::copyMatchingService(match) : nullptr;
	OSSafeReleaseNULL(match);
	if (acc)
		routePv(acc, gRoute);
	OSSafeReleaseNULL(acc);
	return 0;
}

SYSCTL_PROC(_debug, OID_AUTO, rdna4_nativemtl, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, nativeMtlSysctl, "I",
            "AMDRadeonRDNA4: 1 publishes the native-Metal spike's IOAccelerator service; 2, 3 route the paravirtual accelerator's Metal "
            "plug-in through it (docs/native-metal-spike.md)");
}

void RDNA4AccelCensus::setProvider(IOService *gpu)
{
	if (gNativeGpu || !gpu)
		return;
	gpu->retain();
	gNativeGpu = gpu;
	sysctl_register_oid(&sysctl__debug_rdna4_nativemtl);
	// rdna4-nativews=2 or 3: route from the boot on (debug.rdna4_nativemtl's values), at the paravirtual accelerator's first publication:
	// before WindowServer makes its device. Our own service stays unpublished (the router needs none).
	int route = 0;
	if (PE_parse_boot_argn("rdna4-nativews", &route, sizeof(route)) && (route == 2 || route == 3)) {
		gRoute = route;
		OSDictionary *match = IOService::serviceMatching("AppleParavirtAccelerator");
		gPvNotifier = match ? IOService::addMatchingNotification(gIOFirstPublishNotification, match, pvPublished, nullptr) : nullptr;
		OSSafeReleaseNULL(match);
		rdna4Log("AMDRadeonRDNA4: accelcensus: rdna4-nativews=%d: the paravirtual accelerator's Metal plug-in is routed when it appears\n", route);
	}
}

IOReturn RDNA4AccelCensus::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                                        IOUserClient **handler) {
	uint32_t keys = properties ? properties->getCount() : 0;
	census(true, "newUserClient type %u, %u open properties, privileged(admin)=%d", static_cast<unsigned>(type), keys,
	       IOUserClient::clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess);
	if (properties && keys) {
		OSCollectionIterator *it = OSCollectionIterator::withCollection(properties);
		char list[160] = "";
		while (it) {
			auto *k = OSDynamicCast(OSSymbol, it->getNextObject());
			if (!k)
				break;
			if (strlcat(list, k->getCStringNoCopy(), sizeof(list)) >= sizeof(list) - 2)
				break;
			strlcat(list, " ", sizeof(list));
		}
		OSSafeReleaseNULL(it);
		census(true, "  open properties: %s", list);
	}
	return IOService::newUserClient(owningTask, securityID, type, properties, handler);
}

OSObject *RDNA4AccelCensus::copyProperty(const OSSymbol *aKey) const {
	OSObject *o = IOService::copyProperty(aKey);
	if (fromUser() && aKey && strcmp(aKey->getCStringNoCopy(), "CensusLog") != 0) {
		if (noisyKey(aKey->getCStringNoCopy()))
			__atomic_fetch_add(&gNoise, 1u, __ATOMIC_RELAXED);
		else
			census(false, "read property %s -> %s", aKey->getCStringNoCopy(), o ? "present" : "ABSENT");
	}
	return o;
}

OSObject *RDNA4AccelCensus::getProperty(const OSSymbol *aKey) const {
	OSObject *o = IOService::getProperty(aKey);
	if (fromUser() && aKey && strcmp(aKey->getCStringNoCopy(), "CensusLog") != 0) {
		if (noisyKey(aKey->getCStringNoCopy()))
			__atomic_fetch_add(&gNoise, 1u, __ATOMIC_RELAXED);
		else
			census(false, "get property %s -> %s", aKey->getCStringNoCopy(), o ? "present" : "ABSENT");
	}
	return o;
}

bool RDNA4AccelCensus::serializeProperties(OSSerialize *s) const {
	if (fromUser())
		census(false, "whole property dictionary read");
	return IOService::serializeProperties(s);
}

bool RDNA4AccelCensus::matchPropertyTable(OSDictionary *table, SInt32 *score) {
	if (fromUser() && table) {
		char list[160] = "";
		OSCollectionIterator *it = OSCollectionIterator::withCollection(table);
		while (it) {
			auto *k = OSDynamicCast(OSSymbol, it->getNextObject());
			if (!k)
				break;
			if (strlcat(list, k->getCStringNoCopy(), sizeof(list)) >= sizeof(list) - 2)
				break;
			strlcat(list, " ", sizeof(list));
		}
		OSSafeReleaseNULL(it);
		census(false, "matching dictionary keys: %s", list);
	}
	return IOService::matchPropertyTable(table, score);
}

IOReturn RDNA4AccelCensus::message(UInt32 type, IOService *provider, void *argument) {
	census(true, "message type 0x%x from %s", static_cast<unsigned>(type), provider ? provider->getName() : "?");
	return IOService::message(type, provider, argument);
}

// ---- the user client --------------------------------------------------------------------------------------------------------------------

bool RDNA4AccelCensusClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) {
	if (!IOUserClient::initWithTask(owningTask, securityToken, type, properties))
		return false;
	mType = type;
	mPid = proc_selfpid();
	mId = ++gClients;
	census(true, "client #%u opened, type %u (%u generic property reads not logged so far)", mId, static_cast<unsigned>(type),
	       __atomic_load_n(&gNoise, __ATOMIC_RELAXED));
	return true;
}

bool RDNA4AccelCensusClient::start(IOService *provider) {
	return IOUserClient::start(provider);
}

IOReturn RDNA4AccelCensusClient::clientClose() {
	census(true, "client #%u (type %u) closed", mId, mType);
	terminate();
	return kIOReturnSuccess;
}

IOReturn RDNA4AccelCensusClient::clientDied() {
	census(true, "client #%u (type %u) died", mId, mType);
	return IOUserClient::clientDied();
}

void RDNA4AccelCensusClient::free() {
	IOUserClient::free();
}

IOReturn RDNA4AccelCensusClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                                                OSObject *, void *) {
	if (!args) {
		census(true, "client #%u type %u selector %u with no arguments", mId, mType, selector);
		return kIOReturnBadArgument;
	}
	char bytes[72] = "";
	if (args->structureInput && args->structureInputSize)
		hexBytes(bytes, sizeof(bytes), args->structureInput, args->structureInputSize < 32 ? args->structureInputSize : 32);
	census(true, "client #%u type %u selector %u: scalars in %u [%llx %llx %llx %llx], struct in %u%s%s%s, scalars out %u, struct out %u%s%s",
	       mId, mType, selector, args->scalarInputCount,
	       args->scalarInputCount > 0 && args->scalarInput ? args->scalarInput[0] : 0ull,
	       args->scalarInputCount > 1 && args->scalarInput ? args->scalarInput[1] : 0ull,
	       args->scalarInputCount > 2 && args->scalarInput ? args->scalarInput[2] : 0ull,
	       args->scalarInputCount > 3 && args->scalarInput ? args->scalarInput[3] : 0ull,
	       args->structureInputSize, args->structureInputDescriptor ? " (descriptor)" : "", bytes[0] ? " bytes " : "", bytes,
	       args->scalarOutputCount, args->structureOutputSize, args->structureOutputDescriptor ? " (descriptor)" : "",
	       args->asyncWakePort ? ", async" : "");
	if (RDNA4AccelCensus::level() != 2)   // 1 and the native spike's 3 refuse
		return kIOReturnUnsupported;
	// Level 2: succeed with zeros, never touch anything.
	if (args->scalarOutput)
		for (uint32_t i = 0; i < args->scalarOutputCount; i++)
			args->scalarOutput[i] = 0;
	if (args->structureOutput && args->structureOutputSize)
		bzero(args->structureOutput, args->structureOutputSize);
	return kIOReturnSuccess;
}

IOReturn RDNA4AccelCensusClient::clientMemoryForType(UInt32 type, IOOptionBits *, IOMemoryDescriptor **memory) {
	census(true, "client #%u type %u clientMemoryForType(%u): refused", mId, mType, static_cast<unsigned>(type));
	if (memory)
		*memory = nullptr;
	return kIOReturnUnsupported;
}

IOReturn RDNA4AccelCensusClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon) {
	census(true, "client #%u type %u registerNotificationPort(port 0x%lx, type %u, refCon 0x%x): refused", mId, mType, reinterpret_cast<unsigned long>(port),
	       static_cast<unsigned>(type), static_cast<unsigned>(refCon));
	return kIOReturnUnsupported;
}

IOReturn RDNA4AccelCensusClient::connectClient(IOUserClient *client) {
	census(true, "client #%u type %u connectClient(%s)", mId, mType, client ? client->getMetaClass()->getClassName() : "null");
	return kIOReturnUnsupported;
}

IOReturn RDNA4AccelCensusClient::setProperties(OSObject *properties) {
	census(true, "client #%u type %u setProperties(%s)", mId, mType, properties ? properties->getMetaClass()->getClassName() : "null");
	return kIOReturnUnsupported;
}
