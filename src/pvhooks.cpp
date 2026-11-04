/*
 * pvhooks.cpp
 * AMDRadeonRDNA4
 *
 * See pvhooks.hpp.
 */

#include "pvhooks.hpp"
#include "klog.hpp"

#include <Headers/kern_api.hpp>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSObject.h>

#define PLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: pvgpu: hooks: " fmt "\n", ##__VA_ARGS__)

namespace
{

const char *kPathAccel =
	"/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2";

KernelPatcher::KextInfo kextAccel {
	"com.apple.iokit.IOAcceleratorFamily2", &kPathAccel, 1, { true }, {},
	KernelPatcher::KextInfo::Unloaded,
};

constexpr const char kParavirtAccel[] = "AppleParavirtAccelerator";

mach_vm_address_t orgDisable;
mach_vm_address_t orgWait;
mach_vm_address_t enable;
uint32_t skipped;
uint32_t enabled;

bool isParavirt(void *accel)
{
	const OSMetaClass *meta = static_cast<OSObject *>(accel)->getMetaClass();
	const char *name = meta ? meta->getClassName() : nullptr;

	return name && !strcmp(name, kParavirtAccel);
}

/* IOGraphicsAccelerator2::disableAccelerator() */
void wrapDisable(void *accel)
{
	if (isParavirt(accel)) {
		if (skipped++ < 8)
			PLOG("kept the paravirt accelerator enabled through a mode change (%u)",
			     skipped);
		return;
	}

	reinterpret_cast<void (*)(void *)>(orgDisable)(accel);
}

/*
 * IOGraphicsAccelerator2::acceleratorWaitEnabled(). The accelerator starts
 * disabled and is first enabled by a mode change of its framebuffer, which
 * the offline paravirt display never has: enable it before waiting.
 */
uint64_t wrapWait(void *accel)
{
	if (enable && isParavirt(accel)) {
		reinterpret_cast<void (*)(void *)>(enable)(accel);
		if (enabled++ < 4)
			PLOG("enabled the paravirt accelerator for a waiting client (%u)", enabled);
	}

	return reinterpret_cast<uint64_t (*)(void *)>(orgWait)(accel);
}

void route(KernelPatcher &patcher, size_t index, mach_vm_address_t address,
	   size_t size, KernelPatcher::RouteRequest &request)
{
	if (patcher.routeMultiple(index, &request, 1, address, size))
		PLOG("routed %s", request.symbol);
	else
		PLOG("could not route %s (error %d)", request.symbol, patcher.getError());

	patcher.clearError();
}

void processAccel(void *, KernelPatcher &patcher, size_t index,
		  mach_vm_address_t address, size_t size)
{
	KernelPatcher::RouteRequest disable {
		"__ZN22IOGraphicsAccelerator218disableAcceleratorEv", wrapDisable,
		orgDisable,
	};
	KernelPatcher::RouteRequest wait {
		"__ZN22IOGraphicsAccelerator222acceleratorWaitEnabledEv", wrapWait,
		orgWait,
	};

	if (index != kextAccel.loadIndex)
		return;

	enable = patcher.solveSymbol(index, "__ZN22IOGraphicsAccelerator217enableAcceleratorEv",
				     address, size);
	if (!enable)
		PLOG("IOGraphicsAccelerator2::enableAccelerator not found (error %d)",
		     patcher.getError());

	patcher.clearError();
	route(patcher, index, address, size, disable);
	route(patcher, index, address, size, wait);
}

} /* namespace */

void PvHooks::start()
{
	lilu.onKextLoadForce(&kextAccel, 1, processAccel, nullptr);
}
