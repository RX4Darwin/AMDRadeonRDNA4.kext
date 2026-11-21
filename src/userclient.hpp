//
//  userclient.hpp
//  AMDRadeonRDNA4
//
//  The user-space door to the compute runtime (include/rdna4compute.h):
//
//    RDNA4ComputeService  a nub attached to the GPU's IOPCIDevice once the
//                         bring-up can dispatch; IOUserClientClass names the
//                         client, so IOServiceOpen creates one per connection.
//    RDNA4ComputeClient   checks each call's arguments and hands it to
//                         RDNA4Compute's rt* methods; root only.
//

#ifndef RDNA4UserClient_hpp
#define RDNA4UserClient_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>

#include "rdna4compute.h"

class RDNA4Compute;

class RDNA4ComputeService : public IOService {
	OSDeclareDefaultStructors(RDNA4ComputeService)
public:
	RDNA4Compute *compute;
	// A connection of type 'N48N' is the Vulkan interface's (src/n48nkext.cpp); any other is the compute client.
	using IOService::newUserClient;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	                       IOUserClient **handler) APPLE_KEXT_OVERRIDE;
	bool registerPowerManagement(IOService *provider);
	IOReturn setPowerState(unsigned long powerStateOrdinal,
	                       IOService *whatDevice) APPLE_KEXT_OVERRIDE;
};

// rdna4-client-policy: the console user may open the runtime's clients (the default), not root alone (userclient.cpp)
bool localClientsAllowed();

// The published service (the runtime's), for clients made through the paravirtual accelerator (rdna4AcceleratorClient).
extern RDNA4ComputeService *gComputeService;
// A compute client for `task`, attached to the IOAccelerator `accel` it opened with type RDNA4_NATIVE_UC_TYPE (pvhooks.cpp).
IOReturn rdna4AcceleratorClient(IOService *accel, task_t task, void *security, IOUserClient **handler);

class RDNA4ComputeClient : public IOUserClient {
	OSDeclareDefaultStructors(RDNA4ComputeClient)
public:
	using IOUserClient::initWithTask;
	bool initWithTask(task_t owningTask, void *securityToken, UInt32 type,
	                  OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	IOReturn clientDied() APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	                        IOExternalMethodDispatch *dispatch, OSObject *target,
	                        void *reference) APPLE_KEXT_OVERRIDE;

private:
	task_t        task;
	RDNA4Compute *compute;
	IOLock       *gate;     // guards calls and closed
	uint32_t      calls;    // externalMethod calls in flight
	bool          closed;   // released: no more calls
	bool          privileged;   // root: every selector; else (rdna4-client-policy=local) the ones a graphics client needs, in its own VM

	void release();

	static const IOExternalMethodDispatch kMethods[kRDNA4MethodCount];
	static IOReturn sInfo(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sAlloc(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sFree(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sWrite(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sRead(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sLoad(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sUnload(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sDispatch(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sWaitVBlank(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sPresent(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sRestore(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sAllocHost(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sSensors(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sSleepTest(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sQuiesce(OSObject *t, void *, IOExternalMethodArguments *);
	static IOReturn sPresentAsync(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sWaitPresent(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sSubmitIb(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sWaitFence(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sSubmitGfxIb(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sWaitGfxFence(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sImportHost(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sTranslate(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sPresentSource(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sPvNext(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sPvAccess(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sPvDone(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sSensorsEx(OSObject *t, void *, IOExternalMethodArguments *a);
};

// The class clients get (runtime.cpp's IOUserClientClass): RDNA4ComputeClient under a name WindowServer's sandbox lets it open
// (com.apple.WindowServer.sb allows iokit-open of classes prefixed IOAccel), for the native desktop (docs/native-metal-spike.md).
class IOAccelRDNA4ComputeClient : public RDNA4ComputeClient {
	OSDeclareDefaultStructors(IOAccelRDNA4ComputeClient)
};

#endif /* RDNA4UserClient_hpp */
