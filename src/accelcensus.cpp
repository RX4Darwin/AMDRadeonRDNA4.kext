//
//  accelcensus.cpp
//  RDNA4FB
//
//  See accelcensus.hpp.
//

#include "accelcensus.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSCollectionIterator.h>

extern "C" {
int proc_selfpid(void);
void proc_selfname(char *buf, int size);
}

OSDefineMetaClassAndStructors(RDNA4AccelCensus, IOAccelerator)
OSDefineMetaClassAndStructors(RDNA4AccelCensusClient, IOUserClient)

namespace {

uint32_t gLevel { 0 };
RDNA4AccelCensus *gService { nullptr };
uint32_t gLines { 0 };
uint32_t gClients { 0 };
constexpr uint32_t kMaxLines = 600;              // kernel log lines per boot
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
			IOLog("RDNA4FB: accelcensus: line budget (%u) used up\n", kMaxLines);
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
	IOLog("RDNA4FB: accelcensus: [%s/%d] %s\n", name, proc_selfpid(), body);
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

void hexBytes(char *out, size_t cap, const void *p, size_t n) {
	out[0] = '\0';
	const uint8_t *b = static_cast<const uint8_t *>(p);
	size_t at = 0;
	for (size_t i = 0; i < n && at + 3 < cap; i++)
		at += static_cast<size_t>(snprintf(out + at, cap - at, "%02x", b[i]));
}

} // namespace

uint32_t RDNA4AccelCensus::level() { return gLevel; }

// The properties Apple's AMD accelerators publish (docs/metal-spike.md s.1.4: a live Navi14 registry dump). The plug-in names are deliberately ones
// that exist nowhere, so that the loader's failure names what it looked for.
bool RDNA4AccelCensus::publish(IOService *provider, uint32_t level) {
	if (gService || !provider || !level)
		return false;
	gLevel = level;
	gLock = IOLockAlloc();
	auto *svc = OSTypeAlloc(RDNA4AccelCensus);
	if (!svc || !svc->init()) {
		OSSafeReleaseNULL(svc);
		IOLog("RDNA4FB: accelcensus: could not create the service\n");
		return false;
	}
	svc->setProperty("IOUserClientClass", "RDNA4AccelCensusClient");
	svc->setProperty("IOMatchCategory", "IOAccelerator");
	svc->setProperty("MetalPluginName", "RDNA4CensusMTLDriver");
	svc->setProperty("MetalPluginClassName", "RDNA4CensusMtlDevice");
	svc->setProperty("MetalStatisticsName", "RDNA4CensusStatistics");
	svc->setProperty("IOGLBundleName", "RDNA4CensusGLDriver");
	svc->setProperty("IOOCDBundleName", "RDNA4CensusGLDriver");
	svc->setProperty("GPURawCounterBundleName", "RDNA4CensusRawCounterPlugin");
	svc->setProperty("GPURawCounterPluginClassName", "RDNA4CensusRawCounterSourceGroup");
	svc->setProperty("IODVDBundleName", "RDNA4CensusVADriver");
	svc->setProperty("IOAccelRevision", static_cast<uint64_t>(2), 32);
	svc->setProperty("CensusLevel", static_cast<uint64_t>(level), 32);
	svc->setProperty("CensusLog", "");
	if (!svc->attach(provider)) {
		svc->release();
		IOLog("RDNA4FB: accelcensus: could not attach to the GPU\n");
		return false;
	}
	gService = svc;
	svc->registerService();
	svc->release();            // the registry keeps it
	IOLog("RDNA4FB: accelcensus: IOAccelerator service published under %s (level %u: %s); no GPU action\n", provider->getName(), level,
	      level == 1 ? "every call refused" : "every selector succeeds with zeroed outputs");
	return true;
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
	return IOAccelerator::newUserClient(owningTask, securityID, type, properties, handler);
}

OSObject *RDNA4AccelCensus::copyProperty(const OSSymbol *aKey) const {
	OSObject *o = IOAccelerator::copyProperty(aKey);
	if (fromUser() && aKey && strcmp(aKey->getCStringNoCopy(), "CensusLog") != 0)
		census(false, "read property %s -> %s", aKey->getCStringNoCopy(), o ? "present" : "ABSENT");
	return o;
}

OSObject *RDNA4AccelCensus::getProperty(const OSSymbol *aKey) const {
	OSObject *o = IOAccelerator::getProperty(aKey);
	if (fromUser() && aKey && strcmp(aKey->getCStringNoCopy(), "CensusLog") != 0)
		census(false, "get property %s -> %s", aKey->getCStringNoCopy(), o ? "present" : "ABSENT");
	return o;
}

bool RDNA4AccelCensus::serializeProperties(OSSerialize *s) const {
	if (fromUser())
		census(false, "whole property dictionary read");
	return IOAccelerator::serializeProperties(s);
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
	return IOAccelerator::matchPropertyTable(table, score);
}

IOReturn RDNA4AccelCensus::message(UInt32 type, IOService *provider, void *argument) {
	census(true, "message type 0x%x from %s", static_cast<unsigned>(type), provider ? provider->getName() : "?");
	return IOAccelerator::message(type, provider, argument);
}

// ---- the user client --------------------------------------------------------------------------------------------------------------------

bool RDNA4AccelCensusClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) {
	if (!IOUserClient::initWithTask(owningTask, securityToken, type, properties))
		return false;
	mType = type;
	mPid = proc_selfpid();
	mId = ++gClients;
	census(true, "client #%u opened, type %u", mId, static_cast<unsigned>(type));
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
	if (RDNA4AccelCensus::level() < 2)
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
