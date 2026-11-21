//
//  translator.cpp
//  AMDRadeonRDNA4
//
//  The Metal translator helper's request slot and user client (translator.hpp).
//

#include "translator.hpp"
#include "klog.hpp"
#include "rdna4mtlc.h"

#include <IOKit/IOMemoryDescriptor.h>


Translation gTr;

void waitForSlot() {
	while (gTr.id)
		IOLockSleep(gTr.lock, &gTr.seq, THREAD_UNINT);
}

bool waitForHelper() {
	static constexpr uint32_t kHelperWait = 20;   // seconds
	uint64_t deadline = 0;

	IOLockLock(gTr.lock);
	if (!gTr.helper && !gTr.gaveUp) {
		rdna4Log("AMDRadeonRDNA4: translator: waiting up to %u s for the helper\n", kHelperWait);
		clock_interval_to_deadline(kHelperWait, kSecondScale, &deadline);
		while (!gTr.helper && IOLockSleepDeadline(gTr.lock, &gTr.helper, deadline, THREAD_UNINT) == THREAD_AWAKENED) {
		}

		gTr.gaveUp = !gTr.helper;
	}

	const bool helper = gTr.helper;
	IOLockUnlock(gTr.lock);

	return helper;
}

class RDNA4MtlcClient : public IOUserClient {
	OSDeclareDefaultStructors(RDNA4MtlcClient)
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) APPLE_KEXT_OVERRIDE;

private:
	bool mHelper { false };   // this client is the helper (another was not connected)
	IOReturn next(IOExternalMethodArguments *a);
	IOReturn deliver(IOExternalMethodArguments *a);
};

OSDefineMetaClassAndStructors(RDNA4MtlcClient, IOUserClient)

bool RDNA4MtlcClient::start(IOService *provider) {
	if (!IOUserClient::start(provider))
		return false;
	IOLockLock(gTr.lock);
	mHelper = !gTr.helper;
	gTr.helper = true;
	gTr.gaveUp = false;
	gTr.helpers += mHelper;
	IOLockWakeup(gTr.lock, &gTr.helper, false);
	IOLockUnlock(gTr.lock);
	rdna4Log("AMDRadeonRDNA4: translator: helper %s\n", mHelper ? "connected" : "refused: one is connected already");
	return mHelper;
}

// Also what IOUserClient::clientDied comes to. A kMtlcNext still waiting is aborted with the process's thread.
IOReturn RDNA4MtlcClient::clientClose() {
	if (mHelper) {
		IOLockLock(gTr.lock);
		gTr.helper = false;
		IOLockUnlock(gTr.lock);
		mHelper = false;
		rdna4Log("AMDRadeonRDNA4: translator: helper gone\n");
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn RDNA4MtlcClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *, OSObject *, void *) {
	if (!mHelper)
		return kIOReturnNotOpen;
	if (selector == kMtlcNext && a->scalarInputCount == 2 && a->scalarOutputCount == 2 && a->structureOutput &&
	    a->structureOutputSize == sizeof(gTr.name))
		return next(a);
	if (selector == kMtlcDeliver && (a->scalarInputCount == 5 || a->scalarInputCount == 6) && a->scalarInput[2] <= RDNA4_MTLC_MAX_ELF)
		return deliver(a);
	return kIOReturnBadArgument;
}

IOReturn RDNA4MtlcClient::next(IOExternalMethodArguments *a) {
	IOLockLock(gTr.lock);
	while (!gTr.id || gTr.taken)
		if (IOLockSleep(gTr.lock, &gTr.id, THREAD_ABORTSAFE) != THREAD_AWAKENED) {
			IOLockUnlock(gTr.lock);
			return kIOReturnAborted;
		}
	IOReturn r = kIOReturnNoSpace;
	if (gTr.libLen <= a->scalarInput[1]) {
		IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(a->scalarInput[0], gTr.libLen, kIODirectionIn, current_task());
		r = md ? md->prepare() : kIOReturnNoMemory;
		if (r == kIOReturnSuccess) {
			r = md->writeBytes(0, gTr.lib, gTr.libLen) == gTr.libLen ? kIOReturnSuccess : kIOReturnVMError;
			md->complete();
		}
		OSSafeReleaseNULL(md);
	}
	if (r == kIOReturnSuccess) {
		a->scalarOutput[0] = gTr.id;
		a->scalarOutput[1] = gTr.libLen;
		memcpy(a->structureOutput, gTr.name, sizeof(gTr.name));
		gTr.taken = true;
	}
	IOLockUnlock(gTr.lock);
	return r;
}

IOReturn RDNA4MtlcClient::deliver(IOExternalMethodArguments *a) {
	const uint64_t *in = a->scalarInput;
	const uint32_t len = static_cast<uint32_t>(in[2]);
	auto *elf = len ? static_cast<uint8_t *>(IOMalloc(len)) : nullptr;
	if (len) {
		IOMemoryDescriptor *md = elf ? IOMemoryDescriptor::withAddressRange(in[1], len, kIODirectionOut, current_task()) : nullptr;
		IOReturn r = md ? md->prepare() : kIOReturnNoMemory;
		if (r == kIOReturnSuccess) {
			r = md->readBytes(0, elf, len) == len ? kIOReturnSuccess : kIOReturnVMError;
			md->complete();
		}
		OSSafeReleaseNULL(md);
		if (r != kIOReturnSuccess) {
			if (elf)
				IOFree(elf, len);
			return r;
		}
	}
	IOLockLock(gTr.lock);
	const bool current = in[0] && in[0] == gTr.id && gTr.taken && !gTr.done;
	if (current) {
		gTr.elf = elf;
		gTr.elfLen = len;
		gTr.buffers = static_cast<uint32_t>(in[3]);
		gTr.written = static_cast<uint32_t>(in[4]);
		gTr.texturesWritten = a->scalarInputCount > 5 ? static_cast<uint32_t>(in[5]) : 0;
		gTr.done = true;
		IOLockWakeup(gTr.lock, &gTr.done, false);
	}
	IOLockUnlock(gTr.lock);
	if (!current && elf)
		IOFree(elf, len);
	return current ? kIOReturnSuccess : kIOReturnNotFound;   // NotFound: the host stopped waiting for it
}

void translatorInit() {
	if (__atomic_load_n(&gTr.lock, __ATOMIC_ACQUIRE))
		return;
	IOLock *lock = IOLockAlloc(), *none = nullptr;   // ponytail: never freed, like the host
	if (lock && !__atomic_compare_exchange_n(&gTr.lock, &none, lock, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		IOLockFree(lock);
}

IOReturn translatorClient(IOService *service, task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) {
	if (IOUserClient::clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	if (!gTr.lock)
		return kIOReturnNotReady;   // neither the host nor our accelerator has started yet (rdna4-pvgpu-delay)
	auto *client = OSTypeAlloc(RDNA4MtlcClient);
	if (!client)
		return kIOReturnNoMemory;
	if (!client->initWithTask(owningTask, securityID, type, properties) || !client->attach(service)) {
		client->release();
		return kIOReturnNoResources;
	}
	if (!client->start(service)) {
		client->detach(service);
		client->release();
		return kIOReturnBusy;
	}
	*handler = client;
	return kIOReturnSuccess;
}

IOReturn translateForClient(const uint8_t *lib, uint32_t libLen, const char *name, uint8_t *&out, uint32_t &outLen, uint32_t &buffers)
{
	out = nullptr;
	outLen = buffers = 0;
	if (!gTr.lock || !waitForHelper()) {
		rdna4Log("AMDRadeonRDNA4: translator: \"%s\" for a runtime client: %s\n", name, gTr.lock ? "no helper (waited in vain)" : "no request slot yet");
		return kIOReturnNotReady;
	}
	IOLockLock(gTr.lock);
	waitForSlot();
	gTr.id = ++gTr.seq ? gTr.seq : ++gTr.seq;
	gTr.taken = gTr.done = false;
	gTr.lib = lib;
	gTr.libLen = libLen;
	strlcpy(gTr.name, name, sizeof(gTr.name));
	IOLockWakeup(gTr.lock, &gTr.id, false);
	uint64_t deadline = 0;
	clock_interval_to_deadline(20, kSecondScale, &deadline);   // WindowServer's largest shaders take the helper 6 s
	while (!gTr.done && IOLockSleepDeadline(gTr.lock, &gTr.done, deadline, THREAD_UNINT) == THREAD_AWAKENED) {
	}
	const bool done = gTr.done;
	out = done ? gTr.elf : nullptr;
	outLen = done ? gTr.elfLen : 0;
	buffers = done ? gTr.buffers : 0;
	if (!done && gTr.elf)
		IOFree(gTr.elf, gTr.elfLen);
	gTr.id = 0;
	gTr.lib = nullptr;
	gTr.elf = nullptr;
	IOLockWakeup(gTr.lock, &gTr.seq, false);
	IOLockUnlock(gTr.lock);
	if (!done)
		return kIOReturnTimeout;
	return out ? kIOReturnSuccess : kIOReturnUnsupported;   // delivered empty: the helper could not translate it
}
