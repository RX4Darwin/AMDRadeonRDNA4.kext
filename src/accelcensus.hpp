//
//  accelcensus.hpp
//  RDNA4FB
//
//  E1 of docs/metal-spike.md (hub-task-354): the impostor-IOAccelerator census. With the boot-arg rdna4-accelcensus=1 (or 2) the kext publishes one
//  IOAccelerator-class service on the GPU's IOPCIDevice that carries the registry properties Apple's AMD accelerators use to point Metal at a
//  plug-in bundle (MetalPluginName, MetalPluginClassName, ...), plus a user client that only LOGS: every open (type, caller), every property read,
//  every selector call (counts, sizes, first scalars and bytes), every memory-map request and notification-port registration.
//
//  No GPU action, no memory mapping, no hardware access: the service never touches the card. Level 1 refuses every call (kIOReturnUnsupported);
//  level 2 answers every selector with success and zeroed outputs so the caller goes further and shows the next selectors. Default off, and meant
//  for the emulator only: it must never be in a real-card boot (docs/metal-spike.md, E1).
//
//  The census lives in the kernel log (lines "RDNA4FB: accelcensus: ...") and, bounded, in the service's registry property "CensusLog".
//

#ifndef RDNA4AccelCensus_hpp
#define RDNA4AccelCensus_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>

// Why this is not a subclass of IOAccelerator: OpenCore injects our kext into the BOOT kernel collection, and IOGraphicsFamily (which defines
// IOAccelerator) is not in it (in Recovery it is in BaseSystemKernelExtensions.kc, on a full system in SystemKernelExtensions.kc), so a kext that
// links against it is never loaded (hub-task-356, measured: with that one dependency line added to a kext that otherwise loads, the serial log has 0 "RDNA4FB" lines). The
// public header shows IOAccelerator adding no data members and no virtuals of its own to IOService [INFER for the real class], so the census object is an IOService whose getMetaClass() answers with the
// metaclass of the REAL IOAccelerator, looked up by name at run time: IOServiceMatching("IOAccelerator"), OSDynamicCast and metaCast all walk
// the metaclass chain and accept it. The registry shows the class name "IOAccelerator"; the registry NAME is "RDNA4AccelCensus". Census only:
// route B needs a real subclass and will need a different way to be loaded.
class RDNA4AccelCensus : public IOService {
public:
	RDNA4AccelCensus();
	// Publish the service under `provider` (the GPU's IOPCIDevice). `level`: 1 = refuse every call, 2 = succeed with zeroed outputs. Once per boot.
	static bool publish(IOService *provider, uint32_t level);
	static uint32_t level();

	const OSMetaClass *getMetaClass() const APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	                       IOUserClient **handler) APPLE_KEXT_OVERRIDE;
	OSObject *copyProperty(const OSSymbol *aKey) const APPLE_KEXT_OVERRIDE;
	OSObject *getProperty(const OSSymbol *aKey) const APPLE_KEXT_OVERRIDE;
	bool serializeProperties(OSSerialize *s) const APPLE_KEXT_OVERRIDE;
	bool matchPropertyTable(OSDictionary *table, SInt32 *score) APPLE_KEXT_OVERRIDE;
	IOReturn message(UInt32 type, IOService *provider, void *argument) APPLE_KEXT_OVERRIDE;
	using IOService::newUserClient;
	using IOService::copyProperty;
	using IOService::getProperty;
	using IOService::matchPropertyTable;
protected:
	~RDNA4AccelCensus() APPLE_KEXT_OVERRIDE;
};

class RDNA4AccelCensusClient : public IOUserClient {
	OSDeclareDefaultStructors(RDNA4AccelCensusClient)
public:
	using IOUserClient::initWithTask;
	bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	IOReturn clientDied() APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
	                        void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon) APPLE_KEXT_OVERRIDE;
	IOReturn connectClient(IOUserClient *client) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;

private:
	uint32_t mType { 0 };
	uint32_t mId { 0 };
	int mPid { 0 };
};

#endif /* RDNA4AccelCensus_hpp */
