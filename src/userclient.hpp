//
//  userclient.hpp
//  RDNA4FB
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
	bool registerPowerManagement(IOService *provider);
	IOReturn setPowerState(unsigned long powerStateOrdinal,
	                       IOService *whatDevice) APPLE_KEXT_OVERRIDE;
};

class RDNA4ComputeClient : public IOUserClient {
	OSDeclareDefaultStructors(RDNA4ComputeClient)
public:
	using IOUserClient::initWithTask;
	bool initWithTask(task_t owningTask, void *securityToken, UInt32 type,
	                  OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	IOReturn clientDied() APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	                        IOExternalMethodDispatch *dispatch, OSObject *target,
	                        void *reference) APPLE_KEXT_OVERRIDE;

private:
	task_t        task;
	RDNA4Compute *compute;

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
	static IOReturn sPresentAsync(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sWaitPresent(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sSubmitIb(OSObject *t, void *, IOExternalMethodArguments *a);
	static IOReturn sWaitFence(OSObject *t, void *, IOExternalMethodArguments *a);
};

#endif /* RDNA4UserClient_hpp */
