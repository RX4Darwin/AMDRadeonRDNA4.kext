/*
 * pvhooks.cpp
 * AMDRadeonRDNA4
 *
 * See pvhooks.hpp.
 */

#include "pvhooks.hpp"
#include "klog.hpp"
#include "pvscanout.hpp"
#include "trial.hpp"

#include <Headers/kern_api.hpp>
#include <IOKit/IOReturn.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSObject.h>
#include <sys/proc.h>

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
mach_vm_address_t orgReadEdid;   /* IODisplay::readFramebufferEDID() */
mach_vm_address_t fbOpen;   /* IOFramebuffer::open() */
mach_vm_address_t orgMatchInternal;   /* IOService::matchInternal(OSDictionary *, UInt32, UInt32 *) */
mach_vm_address_t orgServiceOpen;   /* is_io_service_open_extended */
void *ndrvVtable;   /* IONDRVFramebuffer's, once the heads are the paravirtual displays */
mach_vm_address_t enable;
uint32_t skipped;
uint32_t enabled;
uint32_t refused;
uint32_t hidden;

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
 * kDisplayWait: for its accelerator, published last (0.5 s after the
 * framebuffers, once the pipes are online), so that the paravirtual GPU is
 * whole when WindowServer goes on (wrapServiceOpen holds it here too).
 */
void waitForParavirtDisplay()
{
	static constexpr uint64_t kDisplayWait = 30ull * NSEC_PER_SEC;
	static bool waited;

	if (waited)
		return;

	waited = true;

	OSDictionary *match = IOService::serviceMatching("AppleParavirtAccelerator");
	IOService *display = match ? IOService::waitForMatchingService(match, kDisplayWait) : nullptr;

	PLOG("paravirtual displays and accelerator %s", display ? "published: refusing our framebuffers" : "NOT published within 30 s");
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

/*
 * IOService::matchInternal. CoreDisplay opens every IOFramebuffer a user-space
 * match finds, and the first WindowServer of a boot that opens only some of
 * them asserts in CGXMappedDisplayStart so that the next one tries again (its
 * state kept in /tmp/.CoreDisplayIOFBOpenInfo; the next one, seeing the same,
 * goes on): with our six refused, every boot's first WindowServer died 8.6 s
 * in and the desktop came 2 s after it (2026-11-14). While the heads are the
 * paravirtual displays, user space does not find our framebuffers at all.
 * Not matchPropertyTable, which matchPassive skips when matchInternal took
 * every key, nor matchPassive, which a match by class alone does not call
 * (copyExistingServices: instanceMatch calls matchInternal).
 */
bool wrapMatchInternal(void *service, OSDictionary *table, uint32_t options, uint32_t *done)
{
	if (ndrvVtable && *static_cast<void **>(service) == ndrvVtable && current_task() != kernel_task && PvScanout::enabled()) {
		if (hidden++ < 4)
			PLOG("framebuffer %p left out of a user-space match: the heads are the paravirtual displays", service);

		return false;
	}

	return reinterpret_cast<bool (*)(void *, OSDictionary *, uint32_t, uint32_t *)>(orgMatchInternal)(service, table, options, done);
}

// The oldest IONDRVFramebuffer, the boot display's (the heads' come later; plugin.cpp's bootFramebuffer needs the GPU, not attached
// yet when this is needed). Retained.
IOService *oldestNdrvFramebuffer()
{
	OSDictionary *match = IOService::serviceMatching(kNdrvFramebuffer);
	OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
	IOService *best = nullptr;

	while (OSObject *o = it ? it->getNextObject() : nullptr) {
		auto *fb = OSDynamicCast(IOService, o);

		if (fb && (!best || fb->getRegistryEntryID() < best->getRegistryEntryID()))
			best = fb;
	}

	if (best)
		best->retain();

	OSSafeReleaseNULL(it);
	OSSafeReleaseNULL(match);

	return best;
}

/*
 * is_io_service_open_extended, for WindowServer's first open of
 * AppleGPUWrangler: held until the paravirtual accelerator is published.
 * CoreDisplay's per-GPU path, which opens a GPU's framebuffers with their
 * display pipes, lists the GPUs when it opens the wrangler; the first
 * WindowServer of a boot starts before the paravirtual GPU is published, so
 * its list had only the card, and its other path opened the paravirtual
 * framebuffers without their pipes: software compositing, and an assert in
 * QuartzCore (2026-11-14). Held here, no IOKit lock is taken yet. With it,
 * the first WindowServer of the boot lives (the rig, 2026-11-14).
 */
int wrapServiceOpen(void *service, void *task, uint32_t type, uint64_t ndr, void *properties, uint32_t propertiesCnt, int *result,
                    void *connection)
{
	static bool held;
	char name[32] = "";

	if (!held && service && isClass(service, "AppleGPUWrangler")) {
		proc_selfname(name, sizeof(name));

		if (!strcmp(name, "WindowServer")) {
			held = true;

			// The heads are decided, and the card brought up, when the boot framebuffer is first opened: by WindowServer just after
			// this. Opened ahead here as wrapNewUserClient does for an undecided one, or the wait below would wait for itself.
			if (!PvScanout::known() && fbOpen)
				if (IOService *boot = oldestNdrvFramebuffer()) {
					const IOReturn r = reinterpret_cast<IOReturn (*)(void *)>(fbOpen)(boot);
					PLOG("opened the boot framebuffer %p ahead of WindowServer's GPU wrangler (0x%x)", boot, r);
					boot->release();
				}

			if (PvScanout::enabled()) {
				PLOG("WindowServer opens the GPU wrangler: held for the paravirtual GPU");
				waitForParavirtDisplay();
			}
		}
	}

	return reinterpret_cast<int (*)(void *, void *, uint32_t, uint64_t, void *, uint32_t, int *, void *)>(orgServiceOpen)(
		service, task, type, ndr, properties, propertiesCnt, result, connection);
}

/*
 * The guest makes up an EDID for each paravirtual display from the name and
 * modes the host gives it: Apple's vendor (APP, 0x1006), product 0, the
 * display's id (port + 1) as its serial, and a DisplayID extension with the
 * modes' timings. And the paravirtual framebuffer reports connection flags
 * of 0xaaaaaaaa, kIOConnectionBuiltIn (0x800) among them: macOS took the
 * monitors for built-in displays ("Integrierter Monitor") and made colour
 * profiles of the guest's names. The EDID's identity is made the monitor's:
 * vendor, product, serial, date, screen size, chromaticities and name from
 * its own EDID (the guest says 255 x 216 cm, which gives macOS a wrong pixel
 * density for picking the default scale);
 * the guest's timings stay, since CoreDisplay checks a display's modes
 * against its EDID (with the monitor's whole EDID it set none and showed
 * nothing). The built-in flag is cleared from the connection flags; with
 * them all cleared, every app's first Metal device open waited 15 s.
 */
void realEdid(IOService *display)
{
	OSData *made = OSDynamicCast(OSData, display->getProperty("IODisplayEDID"));
	const auto *b = made && made->getLength() >= 128 ? static_cast<const uint8_t *>(made->getBytesNoCopy()) : nullptr;

	if (!b || ((b[8] << 8) | b[9]) != 0x1006 || (b[10] | b[11] << 8))
		return;

	const uint32_t serial = b[12] | b[13] << 8 | b[14] << 16 | static_cast<uint32_t>(b[15]) << 24;
	const uint8_t *edid = nullptr;
	size_t len = 0;

	if (!serial || !PvScanout::edid(serial - 1, edid, len) || len < 128) {
		PLOG("paravirtual display %u: no EDID of its monitor: the guest's stays", serial);
		return;
	}

	uint8_t out[512];
	const size_t outLen = made->getLength() < sizeof(out) ? made->getLength() : sizeof(out);

	memcpy(out, b, outLen);           // the base block changed below; the extension (the timings) stays as it is
	memcpy(out + 8, edid + 8, 10);    // vendor, product, serial, week and year
	memcpy(out + 21, edid + 21, 2);   // screen size in cm
	memcpy(out + 25, edid + 25, 10);  // chromaticities

	// the monitor's name in the guest's name descriptor
	for (size_t from = 54; from <= 108; from += 18) {
		if (edid[from] || edid[from + 1] || edid[from + 2] || edid[from + 3] != 0xfc)
			continue;

		for (size_t to = 54; to <= 108; to += 18)
			if (!out[to] && !out[to + 1] && !out[to + 2] && out[to + 3] == 0xfc)
				memcpy(out + to + 5, edid + from + 5, 13);
	}

	uint8_t sum = 0;

	for (size_t i = 0; i < 127; i++)
		sum = static_cast<uint8_t>(sum + out[i]);

	out[127] = static_cast<uint8_t>(-sum);

	OSData *made2 = OSData::withBytes(out, static_cast<unsigned int>(outLen));
	OSNumber *vendor = OSNumber::withNumber((out[8] << 8) | out[9], 32);
	OSNumber *product = OSNumber::withNumber(out[10] | out[11] << 8, 32);
	OSNumber *number = OSNumber::withNumber(out[12] | out[13] << 8 | out[14] << 16 | static_cast<uint32_t>(out[15]) << 24, 32);

	if (made2 && vendor && product && number) {
		OSData *was = OSDynamicCast(OSData, display->getProperty("IODisplayConnectFlags"));
		uint32_t flags = 0;

		if (was && was->getLength() >= sizeof(flags)) {
			memcpy(&flags, was->getBytesNoCopy(), sizeof(flags));
			flags &= ~0x800u;   // kIOConnectionBuiltIn
			display->setProperty("IODisplayConnectFlags", static_cast<void *>(&flags), sizeof(flags));
		}
		display->setProperty("IODisplayEDID", made2);
		display->setProperty("DisplayVendorID", vendor);
		display->setProperty("DisplayProductID", product);
		display->setProperty("DisplaySerialNumber", number);
		PLOG("paravirtual display %u: the guest's EDID with its monitor's identity (vendor 0x%04x product 0x%04x), not built in",
		     serial, vendor->unsigned32BitValue(), product->unsigned32BitValue());
	}

	OSSafeReleaseNULL(made2);
	OSSafeReleaseNULL(vendor);
	OSSafeReleaseNULL(product);
	OSSafeReleaseNULL(number);
}

IOReturn wrapReadEdid(void *display)
{
	const IOReturn r = reinterpret_cast<IOReturn (*)(void *)>(orgReadEdid)(display);
	uint32_t identity = 1;   // rdna4-pvedid=0: the guest's EDID as it made it

	(void)Trial::option("rdna4-pvedid", identity);

	if (PvScanout::enabled() && identity)
		realEdid(static_cast<IOService *>(display));

	return r;
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
	KernelPatcher::RouteRequest readEdid {
		"__ZN9IODisplay19readFramebufferEDIDEv", wrapReadEdid, orgReadEdid,
	};

	if (index != kextGraphics.loadIndex)
		return;

	KernelPatcher::RouteRequest matchInternal {
		"__ZN9IOService13matchInternalEP12OSDictionaryjPj", wrapMatchInternal, orgMatchInternal,
	};

	KernelPatcher::RouteRequest serviceOpen {"_is_io_service_open_extended", wrapServiceOpen, orgServiceOpen};

	if (!patcher.routeMultiple(KernelPatcher::KernelID, &serviceOpen, 1))
		PLOG("is_io_service_open_extended not routed (error %d): WindowServer is not held for the paravirtual GPU", patcher.getError());

	patcher.clearError();

	if (!patcher.routeMultiple(KernelPatcher::KernelID, &matchInternal, 1))
		PLOG("IOService::matchInternal not routed (error %d): our framebuffers stay visible to WindowServer", patcher.getError());

	patcher.clearError();
	fbOpen = patcher.solveSymbol(index, "__ZN13IOFramebuffer4openEv", address, size);
	if (!fbOpen)
		PLOG("no IOFramebuffer::open (error %d): an undecided framebuffer is not opened ahead", patcher.getError());

	patcher.clearError();
	route(patcher, index, address, size, newUserClient);
	route(patcher, index, address, size, readEdid);
}

} /* namespace */

void PvHooks::hideFramebuffers(void *fb)
{
	if (fb && !ndrvVtable) {
		ndrvVtable = *static_cast<void **>(fb);
		PLOG("IONDRVFramebuffer left out of user-space matching%s", orgMatchInternal ? "" : " (NOT: matchInternal not routed)");
	}
}

void PvHooks::start()
{
	lilu.onKextLoadForce(&kextAccel, 1, processAccel, nullptr);
	lilu.onKextLoadForce(&kextGraphics, 1, processGraphics, nullptr);
}
