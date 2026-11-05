/*
 * pvhooks.cpp
 * AMDRadeonRDNA4
 *
 * See pvhooks.hpp.
 */

#include "pvhooks.hpp"
#include "klog.hpp"
#include "pvscanout.hpp"

#include <Headers/kern_api.hpp>
#include <IOKit/IOReturn.h>
#include <IOKit/IOService.h>
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

const char *kPathGraphics = "/System/Library/Extensions/IOGraphicsFamily.kext/IOGraphicsFamily";

KernelPatcher::KextInfo kextGraphics {
	"com.apple.iokit.IOGraphicsFamily", &kPathGraphics, 1, { true }, {},
	KernelPatcher::KextInfo::Unloaded,
};

constexpr const char kParavirtAccel[] = "AppleParavirtAccelerator";
constexpr const char kNdrvFramebuffer[] = "IONDRVFramebuffer";

mach_vm_address_t orgDisable;
mach_vm_address_t orgWait;
mach_vm_address_t orgNewUserClient;
mach_vm_address_t fbOpen;   /* IOFramebuffer::open() */
mach_vm_address_t enable;
uint32_t skipped;
uint32_t enabled;
uint32_t refused;

bool isClass(void *object, const char *want)
{
	const OSMetaClass *meta = static_cast<OSObject *>(object)->getMetaClass();
	const char *name = meta ? meta->getClassName() : nullptr;

	return name && !strcmp(name, want);
}

bool isParavirt(void *accel)
{
	return isClass(accel, kParavirtAccel);
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

/*
 * IOFramebuffer::newUserClient(task_t, void *, UInt32, IOUserClient **).
 * While the heads are the paravirtual displays, our framebuffers have no
 * display for macOS and IOFramebuffer gives them its 1x1 offline mode.
 * WindowServer still opened them and copied its 4K boot screen into that
 * one page of mapping (SIGBUS in CGXReleaseDisplayDeviceSurface). The first
 * open of the boot framebuffer is what has it answered (the NDRV's Open,
 * plugin.cpp attach) and the heads decided: the first WindowServer of a boot
 * got it before the decision and died in its first frame, and loginwindow
 * does not log in automatically again after that. Refused undecided, the card
 * was never brought up. So an undecided framebuffer is opened here first
 * (IOFramebuffer::open takes its gates itself), and the client then refused.
 */
/*
 * CoreDisplay asserts when none of a GPU's framebuffers opens and there is
 * no other display yet: the first WindowServer of a boot, refused before the
 * paravirtual display was published, died in CGXMappedDisplayStart (its
 * restart then found it). The first refusal waits for that display, at most
 * kDisplayWait.
 */
void waitForParavirtDisplay()
{
	static constexpr uint64_t kDisplayWait = 30ull * NSEC_PER_SEC;
	static bool waited;

	if (waited)
		return;

	waited = true;

	OSDictionary *match = IOService::serviceMatching("AppleParavirtFramebuffer");
	IOService *display = match ? IOService::waitForMatchingService(match, kDisplayWait) : nullptr;

	PLOG("paravirtual display %s", display ? "published: refusing our framebuffers" : "NOT published within 30 s");
	OSSafeReleaseNULL(display);
	OSSafeReleaseNULL(match);
}

IOReturn wrapNewUserClient(void *fb, void *task, void *security, uint32_t type, void **handler)
{
	if (isClass(fb, kNdrvFramebuffer) && !PvScanout::known() && fbOpen) {
		const IOReturn r = reinterpret_cast<IOReturn (*)(void *)>(fbOpen)(fb);

		PLOG("opened framebuffer %p ahead of its first client (0x%x): the heads %s", fb, r,
		     !PvScanout::known() ? "still undecided" : PvScanout::enabled() ? "are the paravirtual displays" : "are macOS's displays");
	}

	if (isClass(fb, kNdrvFramebuffer) && PvScanout::enabled()) {
		waitForParavirtDisplay();

		if (refused++ < 8)
			PLOG("refused user client type %u of framebuffer %p: the heads are the paravirtual displays",
			     type, fb);

		return kIOReturnUnsupported;
	}

	return reinterpret_cast<IOReturn (*)(void *, void *, void *, uint32_t, void **)>(orgNewUserClient)(
		fb, task, security, type, handler);
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

void processGraphics(void *, KernelPatcher &patcher, size_t index,
		     mach_vm_address_t address, size_t size)
{
	KernelPatcher::RouteRequest newUserClient {
		"__ZN13IOFramebuffer13newUserClientEP4taskPvjPP12IOUserClient",
		wrapNewUserClient, orgNewUserClient,
	};

	if (index != kextGraphics.loadIndex)
		return;

	fbOpen = patcher.solveSymbol(index, "__ZN13IOFramebuffer4openEv", address, size);
	if (!fbOpen)
		PLOG("no IOFramebuffer::open (error %d): an undecided framebuffer is not opened ahead", patcher.getError());

	patcher.clearError();
	route(patcher, index, address, size, newUserClient);
}

} /* namespace */

void PvHooks::start()
{
	lilu.onKextLoadForce(&kextAccel, 1, processAccel, nullptr);
	lilu.onKextLoadForce(&kextGraphics, 1, processGraphics, nullptr);
}
