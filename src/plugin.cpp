//
//  plugin.cpp
//  AMDRadeonRDNA4
//
//  Lilu plugin entry. On a GPU macOS has no driver for, Apple's generic
//  IONDRVFramebuffer (com.apple.iokit.IONDRVSupport) drives the GOP
//  framebuffer through its boot NDRV (IOBootNDRV). Every request that driver
//  makes of its NDRV — Initialize/Open and every csc Control/Status call —
//  goes through IONDRVFramebuffer::doDriverIO. Routing that one function lets
//  AMDRadeonRDNA4 answer the requests for the RX 9070 XT's framebuffer while Apple's
//  code keeps doing everything IOFramebuffer-side (mode caching, pixel
//  formats, aperture remapping, console, gamma, power states).
//
//  Why a Lilu plugin rather than an IOFramebuffer subclass: on macOS 11+ the
//  IOGraphicsFamily code lives only in the System kernel collection, so a kext
//  linking against it can't be injected by OpenCore (Boot KC), and the
//  /Library/Extensions route is closed to ad-hoc signed kexts on Tahoe.
//  Lilu resolves IONDRVSupport's symbols at runtime instead.
//

#include <Headers/plugin_start.hpp>
#include "klog.hpp"
#include "trial.hpp"
#include "vrambench.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOGraphicsTypes.h>   // header-only types for IOMacOSVideo.h
#include <IOKit/ndrvsupport/IOMacOSTypes.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include <IOKit/ndrvsupport/IONDRVLibraries.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include <mach/kmod.h>

#include "accelcensus.hpp"
#ifdef AMDRadeonRDNA4_PVGPU
#include "pvgpu.hpp"
#include "pvhooks.hpp"
#include "pvscanout.hpp"
#endif
#include "compute.hpp"
#include "buildrev.h"   // AMDRADEONRDNA4_BUILD, written by the Makefile
#include "device.hpp"
#include "ndrv.hpp"

uint32_t rdna4TraceLevel { 0 };

#ifdef AMDRadeonRDNA4_VM_TEST
#include "bochsvbe.hpp"
#endif

#define FBLOG(fmt, ...)  rdna4Log("AMDRadeonRDNA4: " fmt "\n", ## __VA_ARGS__)

// The local csc record mirrors (ndrv.hpp) must match the SDK exactly.
#define NDRV_SAME_SIZE(T) \
	static_assert(sizeof(Ndrv::T) == sizeof(::T), "Ndrv::" #T " size")
#define NDRV_SAME_FIELD(T, f) \
	static_assert(__builtin_offsetof(Ndrv::T, f) == __builtin_offsetof(::T, f), "Ndrv::" #T "." #f)
NDRV_SAME_SIZE(Rect);
NDRV_SAME_SIZE(VPBlock);
NDRV_SAME_FIELD(VPBlock, vpRowBytes);
NDRV_SAME_FIELD(VPBlock, vpBounds);
NDRV_SAME_FIELD(VPBlock, vpPixelType);
NDRV_SAME_FIELD(VPBlock, vpPixelSize);
NDRV_SAME_FIELD(VPBlock, vpCmpCount);
NDRV_SAME_FIELD(VPBlock, vpCmpSize);
NDRV_SAME_FIELD(VPBlock, vpPlaneBytes);
NDRV_SAME_SIZE(VDSwitchInfoRec);
NDRV_SAME_FIELD(VDSwitchInfoRec, csData);
NDRV_SAME_FIELD(VDSwitchInfoRec, csPage);
NDRV_SAME_FIELD(VDSwitchInfoRec, csBaseAddr);
NDRV_SAME_SIZE(VDTimingInfoRec);
NDRV_SAME_FIELD(VDTimingInfoRec, csTimingFormat);
NDRV_SAME_FIELD(VDTimingInfoRec, csTimingData);
NDRV_SAME_FIELD(VDTimingInfoRec, csTimingFlags);
NDRV_SAME_SIZE(VDDisplayConnectInfoRec);
NDRV_SAME_FIELD(VDDisplayConnectInfoRec, csConnectFlags);
NDRV_SAME_FIELD(VDDisplayConnectInfoRec, csDisplayComponent);
NDRV_SAME_SIZE(VDSyncInfoRec);
NDRV_SAME_SIZE(VDResolutionInfoRec);
NDRV_SAME_FIELD(VDResolutionInfoRec, csDisplayModeID);
NDRV_SAME_FIELD(VDResolutionInfoRec, csRefreshRate);
NDRV_SAME_FIELD(VDResolutionInfoRec, csMaxDepthMode);
NDRV_SAME_FIELD(VDResolutionInfoRec, csResolutionFlags);
NDRV_SAME_FIELD(VDResolutionInfoRec, csReserved);
NDRV_SAME_SIZE(VDVideoParametersInfoRec);
NDRV_SAME_FIELD(VDVideoParametersInfoRec, csDepthMode);
NDRV_SAME_FIELD(VDVideoParametersInfoRec, csVPBlockPtr);
NDRV_SAME_FIELD(VDVideoParametersInfoRec, csPageCount);
NDRV_SAME_SIZE(VDDDCBlockRec);
NDRV_SAME_FIELD(VDDDCBlockRec, ddcBlockType);
NDRV_SAME_FIELD(VDDDCBlockRec, ddcBlockData);
NDRV_SAME_SIZE(VDDetailedTimingRec);
NDRV_SAME_FIELD(VDDetailedTimingRec, csDisplayModeID);
NDRV_SAME_FIELD(VDDetailedTimingRec, csPixelClock);
NDRV_SAME_FIELD(VDDetailedTimingRec, csHorizontalActive);
NDRV_SAME_FIELD(VDDetailedTimingRec, csVerticalSyncConfig);
NDRV_SAME_FIELD(VDDetailedTimingRec, csNumLinks);
static_assert(Ndrv::cscGetCurMode == ::cscGetCurMode && Ndrv::cscGetSync == ::cscGetSync &&
              Ndrv::cscGetConnection == ::cscGetConnection &&
              Ndrv::cscGetModeTiming == ::cscGetModeTiming &&
              Ndrv::cscGetNextResolution == ::cscGetNextResolution &&
              Ndrv::cscGetVideoParameters == ::cscGetVideoParameters &&
              Ndrv::cscGetDDCBlock == ::cscGetDDCBlock &&
              Ndrv::cscGetDetailedTiming == ::cscGetDetailedTiming &&
              Ndrv::cscSupportsHardwareCursor == ::cscSupportsHardwareCursor &&
              Ndrv::cscGetHardwareCursorDrawState == ::cscGetHardwareCursorDrawState &&
              Ndrv::cscSwitchMode == ::cscSwitchMode && Ndrv::cscSetSync == ::cscSetSync &&
              Ndrv::cscSetEntries == ::cscSetEntries && Ndrv::cscSetGamma == ::cscSetGamma &&
              Ndrv::cscSetHardwareCursor == ::cscSetHardwareCursor &&
              Ndrv::cscDrawHardwareCursor == ::cscDrawHardwareCursor,
              "csc selectors");
static_assert(Ndrv::kDeclROMtables == static_cast<uint32_t>(::kDeclROMtables) &&
              Ndrv::kDepthMode1 == ::kDepthMode1 &&
              Ndrv::kDisplayModeIDFindFirst == ::kDisplayModeIDFindFirstResolution &&
              Ndrv::kDisplayModeIDNoMore == ::kDisplayModeIDNoMoreResolutions &&
              Ndrv::kDisplayModeIDInvalid == ::kDisplayModeIDInvalid &&
              Ndrv::kReportsDDCConnection == (1u << ::kReportsDDCConnection) &&
              Ndrv::kHasDDCConnection == (1u << ::kHasDDCConnection) &&
              Ndrv::kHasDirectConnection == (1u << ::kHasDirectConnection) &&
              Ndrv::kGenericLCD == ::kGenericLCD && Ndrv::kDPMSSyncOff == ::kDPMSSyncOff &&
              Ndrv::kDPMSSyncMask == ::kDPMSSyncMask &&
              Ndrv::kRGBDirectPixels == ::kIORGBDirectPixels &&
              Ndrv::kBadArgument == static_cast<int32_t>(kIOReturnBadArgument) &&
              Ndrv::kUnsupported == static_cast<int32_t>(kIOReturnUnsupported),
              "NDRV constants");
static_assert(sizeof(Ndrv::VDSetHardwareCursorRec) == sizeof(::VDSetHardwareCursorRec) &&
              sizeof(Ndrv::VDDrawHardwareCursorRec) == sizeof(::VDDrawHardwareCursorRec) &&
              sizeof(Ndrv::VDSupportsHardwareCursorRec) == sizeof(::VDSupportsHardwareCursorRec) &&
              sizeof(Ndrv::VDHardwareCursorDrawStateRec) == sizeof(::VDHardwareCursorDrawStateRec),
              "NDRV cursor records");

namespace {

// IONDRVControlParameters from IOGraphics' private IOGraphicsPrivate.h: the
// parameter block of kIONDRVControlCommand / kIONDRVStatusCommand.
struct NdrvControlParameters {
	uint8_t  reservedA[0x1a];
	uint16_t code;      // csc selector
	void    *params;    // csc record (VDSwitchInfoRec, VPBlock, ...)
	uint8_t  reservedB[0x12];
};
static_assert(__builtin_offsetof(NdrvControlParameters, code) == 0x1a, "IONDRVControlParameters.code");
static_assert(__builtin_offsetof(NdrvControlParameters, params) == 0x20, "IONDRVControlParameters.params");

const char *kPathIONDRVSupport = "/System/Library/Extensions/IONDRVSupport.kext/IONDRVSupport";

// Loaded flag: IONDRVSupport may already be loaded (System KC) by the time
// Lilu activates; Lilu then processes it as an already-loaded kext.
KernelPatcher::KextInfo kextIONDRVSupport {
	"com.apple.iokit.IONDRVSupport", &kPathIONDRVSupport, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded,
};

#ifdef AMDRadeonRDNA4_PVGPU
// The IOSurface kext (boot kernel collection, loaded before Lilu): the fake GPU's host draws into IOSurface-backed render targets with it.
const char *kPathIOSurface = "/System/Library/Extensions/IOSurface.kext/Contents/MacOS/IOSurface";
KernelPatcher::KextInfo kextIOSurface {
	"com.apple.iokit.IOSurface", &kPathIOSurface, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded,
};
#endif

mach_vm_address_t orgDoDriverIO { 0 };
mach_vm_address_t orgVslNew { 0 };
mach_vm_address_t orgVslDispose { 0 };
mach_vm_address_t orgVslDo { 0 };
mach_vm_address_t orgVslPrepareCursor { 0 };
bool traceEnabled { false };
uint32_t traceBudget { 400 };   // bounded: gamma/CLUT calls can be frequent
uint32_t computeStage { 0 };    // rdna4-compute=<stage>, see compute.hpp
uint32_t pvGpu { 0 };           // rdna4-pvgpu=1|2: M0 of docs/metal-phase-plan.md, the fake Apple paravirtual GPU nub (pvgpu.hpp); VM/emulator only
uint32_t pvDelay { 0 };         // rdna4-pvgpu-delay=<s>: publish the nub this long after plugin start instead (no GPU-side publish)
uint32_t accelCensus { 0 };     // rdna4-accelcensus=1|2: the E1 impostor-IOAccelerator census (accelcensus.hpp), emulator only
// rdna4-pvgpu-disp bit 2 (with rdna4-pvgpu): the heads are the paravirtual GPU's displays (pvscanout.hpp), and macOS is told
// that none of our framebuffers has one
bool pvHeads { false };

// What we keep for one of our framebuffers.
struct FbState {
	RDNA4Device      dev;
	Ndrv::Translator ndrv;
	RDNA4Compute     compute;
};

// Framebuffers seen so far, whether they drive one of our GPUs, and the
// state created for ours.
struct FbEntry {
	void    *fb;
	bool     ours;
	bool     tried;
	FbState *state;
};
FbEntry fbTable[8] {};
// The boot head's state, once attach made it (port 0 of the paravirtual displays).
FbState *bootState { nullptr };

// Forward declaration: the explicit open-time VBL service creation below
// goes through the same wrapper as a legacy NDRV call.
int32_t wrapVslNew(void *entryID, UInt32 type, void **service);

// The GPU behind a framebuffer: its provider, or the provider's provider.
IOPCIDevice *pciFor(IOService *provider) {
	auto *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci && provider)
		pci = OSDynamicCast(IOPCIDevice, provider->getProvider());
	return pci;
}

bool isOurDevice(IOService *provider) {
	IOPCIDevice *pci = pciFor(provider);
	if (!pci)
		return false;
	uint32_t vd = pci->configRead32(kIOPCIConfigVendorID);
	uint16_t vendor = vd & 0xffff, device = static_cast<uint16_t>(vd >> 16);
	if (vendor == 0x1002 && (device == 0x7550 || device == 0x7551))
		return true;
#ifdef AMDRadeonRDNA4_VM_TEST
	if (vendor == 0x15ad && device == 0x0405)
		return true;
#endif
	return false;
}

// The table entry for `fb`, added on first sight. nullptr only when the
// table is full, which leaves that framebuffer to Apple's code.
FbEntry *fbEntry(void *fb) {
	for (auto &e : fbTable)
		if (e.fb == fb)
			return &e;
	auto *svc = static_cast<IOService *>(fb);
	bool ours = isOurDevice(svc->getProvider());
	FBLOG("ndrv: framebuffer %p (%s) on provider %s: %s", fb, svc->getName(),
	      svc->getProvider() ? svc->getProvider()->getName() : "-",
	      ours ? "ours" : "not ours");
	for (auto &e : fbTable) {
		if (!e.fb) {
			e = { fb, ours, false, nullptr };
			return &e;
		}
	}
	FBLOG("ndrv: framebuffer table full, ignoring %p", fb);
	return nullptr;
}

// Ndrv::Backend over RDNA4Device. Every mode reuses the boot surface: modes
// never exceed the GOP framebuffer and keep its pitch (see device.hpp).
bool deviceSurfaceFor(void *ctx, const Modes::Mode &m, bool, Ndrv::Surface &out) {
	auto *dev = static_cast<RDNA4Device *>(ctx);
	if (m.t.hActive > dev->fbWidth || m.t.vActive > dev->fbHeight)
		return false;
	out = { dev->fbPhysBase, dev->fbRowBytes, m.t.hActive, m.t.vActive };
	return true;
}

int32_t deviceSwitchTo(void *ctx, const Modes::Mode &m, bool) {
	return static_cast<RDNA4Device *>(ctx)->applyMode(m);
}

void deviceSetPower(void *ctx, bool on) {
	// a framebuffer macOS has no display on for must not blank the paravirtual display it scans out
	if (pvHeads && !on)
		return;

	static_cast<RDNA4Device *>(ctx)->setDisplayPower(on);
}

void createVblankService(IOService *framebuffer) {
	uint32_t ih = 0, requested = 0;
	if (!PE_parse_boot_argn("rdna4-ih", &ih, sizeof(ih)) || ih < 2 ||
	    !PE_parse_boot_argn("rdna4-vbl", &requested, sizeof(requested)) || !requested ||
	    Ndrv::vslServicePresent() || !orgVslNew || !orgVslDo || !framebuffer)
		return;
	IOService *provider = framebuffer->getProvider();
	OSData *entry = provider ? OSDynamicCast(OSData,
		provider->getProperty(kAAPLRegEntryIDKey)) : nullptr;
	if (!entry || entry->getLength() < sizeof(RegEntryID)) {
		FBLOG("ndrv: VBL service needs provider AAPL,RegEntryID");
		return;
	}
	void *service = nullptr;
	IOReturn ret = wrapVslNew(const_cast<void *>(entry->getBytesNoCopy()),
	                          ::kVBLInterruptServiceType, &service);
	if (ret != kIOReturnSuccess)
		FBLOG("ndrv: open-time VBL service creation failed 0x%x", ret);
}

bool deviceSupportsHardwareCursor(void *ctx) {
	return static_cast<RDNA4Device *>(ctx)->supportsHardwareCursor();
}

int32_t deviceSetHardwareCursor(void *ctx, void *cursorRef) {
	return static_cast<RDNA4Device *>(ctx)->setHardwareCursor(cursorRef);
}

int32_t deviceDrawHardwareCursor(void *ctx, int32_t x, int32_t y, uint32_t visible) {
	return static_cast<RDNA4Device *>(ctx)->drawHardwareCursor(x, y, visible);
}

void deviceCursorProbe(void *ctx, const char *why) {
	static_cast<RDNA4Device *>(ctx)->cursorRegProbe(why);
}

void deviceSmuReady(void *ctx) {
	static_cast<RDNA4Device *>(ctx)->smuStarted();
}

int32_t deviceGetHardwareCursorDrawState(void *ctx,
	                                         Ndrv::VDHardwareCursorDrawStateRec &state) {
	return static_cast<RDNA4Device *>(ctx)->getHardwareCursorDrawState(state);
}

#ifdef AMDRadeonRDNA4_VM_TEST
// Size QEMU is scanning out (the last mode it took); one VM test device.
uint16_t vmWidth { 0 }, vmHeight { 0 };

// VM test device: resize QEMU's scanout, keeping the boot pitch and base.
int32_t vbeSwitchTo(void *ctx, const Modes::Mode &m, bool) {
	auto *dev = static_cast<RDNA4Device *>(ctx);
	const auto pitchPixels = static_cast<uint16_t>(dev->fbRowBytes / 4);
	if (BochsVbe::setMode(m.t.hActive, m.t.vActive, pitchPixels)) {
		vmWidth = m.t.hActive;
		vmHeight = m.t.vActive;
		FBLOG("vm: switch to %ux%u done", m.t.hActive, m.t.vActive);
		return kIOReturnSuccess;
	}
	// The failed switch reports an error and macOS keeps the old mode, so
	// the scanout has to match it again.
	bool restored = BochsVbe::setMode(vmWidth, vmHeight, pitchPixels);
	FBLOG("vm: switch to %ux%u refused by QEMU, %ux%u %s", m.t.hActive, m.t.vActive,
	      vmWidth, vmHeight, restored ? "restored" : "NOT restored");
	return kIOReturnIOError;
}
#endif

// --- rdna4-head2=1: a phantom second head (docs/second-head-ndrv.md s.7) ------
//
// A second IONDRVFramebuffer, on an IONDRVDevice nub the plugin creates. It
// has no IOBootNDRV behind it (IOBootNDRV only takes the console), so
// wrapDoDriverIO answers its Initialize/Open and every csc request. It serves
// one mode of the second sink's EDID on a spare VRAM surface behind the
// console. No display register is written and no pipe scans the surface out:
// this only shows whether macOS takes a second head.
//
// The nub has to exist before WindowServer looks for framebuffers: one that
// appeared during head 0's open was never opened (card boot of 2026-10-03).
// So it is created from the routed IONDRVFramebuffer::start, just before
// head 0 starts on the PCI device, and the two are made dependents of one
// controller (IOFBDependentID, IOFBDependentIndex 0 and 1) the way
// IONDRVSupport's own multi-head code does: IOFramebuffer::open of either
// head then opens the other.
struct Head2 {
	IOPCIDevice     *pci { nullptr };
	size_t           slot { 0 };            // its extra pipe (RDNA4Device::extra); head slot + 2 in the log
	IOService       *nub { nullptr };
	RDNA4Device     *dev { nullptr };       // once the second pipe is lit: display power goes to it
	bool             ready { false };       // fillHead2 set the mode, surface and EDID
	Ndrv::Translator ndrv;
	Ndrv::Surface    surface {};
	IODeviceMemory  *vram { nullptr };      // the surface, for getVRAMRange (wrapGetVRAMRange)
	Modes::Mode      table[Modes::MaxModes] {};
	uint8_t          edid[256] {};          // base block and one extension
	size_t           edidLen { 0 };
	uint32_t         traceBudget { 400 };   // its own: head 0 must not use up the lines
	// The device memory range that holds the console (BAR0 on the card).
	IODeviceMemory  *range { nullptr };
	uint64_t         rangeBase { 0 }, rangeLen { 0 };
	// Hot-plug (rdna4-hotplug): the display on the pipe's connector comes and goes.
	bool             connected { false };   // what macOS has been told
	void           (*connectProc)(OSObject *, void *) { nullptr };   // IOFramebuffer's connect interrupt handler
	OSObject        *connectTarget { nullptr };
	void            *connectRef { nullptr };
	IOTimerEventSource *hpdTimer { nullptr };
	bool             hpdSeen { false };     // the pin at the last poll
	uint8_t          hpdPin { 0 };          // the pin that was last seen high
	uint32_t         hpdSame { 0 };         // polls it has read that way
	uint32_t         hpdGone { 0 };         // polls it has been low with the link up
	bool             hpdBack { false };     // it came back from a second or more of low with the head connected
	// Its display can be lit at 3840x2160 once the compute bring-up has raised the display clocks: it waits
	// offline for that, then is plugged in (or after kClockWaitPolls at the size it can have).
	bool             waitClocks { false };
	uint32_t         waitPolls { 0 };
};
#define HLOG(h, fmt, ...)  FBLOG("head%u: " fmt, static_cast<unsigned>((h)->slot + 2), ## __VA_ARGS__)
// One per display beside the boot one (rdna4-heads): heads 2, 3 and 4.
Head2 *heads[RDNA4Device::kMaxExtra] {};
size_t headCount { 0 };
// rdna4-head2: 0 none, 1 phantom, 2..4 see RDNA4Device::lightSecondPipe. 4 unless
// the boot-arg says otherwise: a second display on HDMI ran on the card.
uint32_t head2Level { 0 };
// rdna4-hotplug, with rdna4-head2 >= 2: 0 off, 1 logs what the connector's HPD
// pin does, 2 (unless the boot-arg says otherwise) acts on it. With 2 the head
// exists without a monitor too, offline.
uint32_t hotplugLevel { 0 };
mach_vm_address_t orgStart { 0 };
mach_vm_address_t orgGetVRAMRange { 0 };
mach_vm_address_t orgRegisterForInterruptType { 0 };

void attach(FbEntry &e);

Head2 *headFor(void *fb) {
	for (size_t i = 0; i < headCount; i++)
		if (static_cast<IOService *>(fb)->getProvider() == heads[i]->nub)
			return heads[i];
	return nullptr;
}

// Every mode scans the one surface, from its first pixel and with its pitch.
bool head2SurfaceFor(void *ctx, const Modes::Mode &m, bool, Ndrv::Surface &out) {
	const Ndrv::Surface &s = static_cast<Head2 *>(ctx)->surface;
	if (m.t.hActive > s.width || m.t.vActive > s.height)
		return false;
	out = { s.physBase, s.rowBytes, m.t.hActive, m.t.vActive };
	return true;
}

int32_t head2SwitchTo(void *ctx, const Modes::Mode &m, bool) {
	auto *h = static_cast<Head2 *>(ctx);
	h->dev->selectExtra(h->slot);
	return h->dev->applySecondPipeMode(m);
}

void head2SetPower(void *ctx, bool on) {
	auto *h = static_cast<Head2 *>(ctx);

	if (pvHeads && !on)
		return;

	h->dev->selectExtra(h->slot);
	if (h->connected || !on)             // the link of an unplugged display stays off
		h->dev->setSecondPipePower(on);
}

// From wrapStart, before IONDRVFramebuffer starts on the PCI device.
void createHead(IOPCIDevice *pci, size_t slot) {
	auto *h = new Head2;
	// IONDRVDevice is private to IONDRVSupport: made by name, driven as an IOService.
	IOService *nub = h ? OSDynamicCast(IOService, OSMetaClass::allocClassWithName("IONDRVDevice"))
	                   : nullptr;
	if (!nub || !nub->init()) {
		FBLOG("head%zu: not created: IONDRVSupport gave no IONDRVDevice", slot + 2);
		OSSafeReleaseNULL(nub);
		delete h;
		return;
	}
	// A device-tree style "display" node: what personality 2 of IONDRVSupport
	// matches and IONDRVFramebuffer::start accepts.
	static const char kDisplay[] = "display";
	const uint64_t id = pci->getRegistryEntryID();
	nub->setName(kDisplay);
	nub->setProperty("name", const_cast<char *>(kDisplay), sizeof(kDisplay));
	nub->setProperty("device_type", const_cast<char *>(kDisplay), sizeof(kDisplay));
	nub->setProperty("IOFBDependentID", id, 64);
	nub->setProperty("IOFBDependentIndex", static_cast<unsigned long long>(slot + 1), 32);
	char location[4];
	snprintf(location, sizeof(location), "%zu", slot + 1);
	nub->setLocation(location);
	if (OSArray *memory = pci->getDeviceMemory())
		nub->setDeviceMemory(memory);   // getApertureRange maps the surface out of these
	// IONDRVFramebuffer takes its device, and IONDRVDevice its power parent,
	// from the device-tree plane.
	nub->attachToParent(pci, gIODTPlane);
	if (!nub->attach(pci)) {
		FBLOG("head%zu: not created: the nub did not attach", slot + 2);
		nub->detachFromParent(pci, gIODTPlane);
		nub->release();
		delete h;
		return;
	}
	// Head 0 copies these from its provider when it starts, right after this.
	pci->setProperty("IOFBDependentID", id, 64);
	pci->setProperty("IOFBDependentIndex", 0ull, 32);
	h->pci = pci;
	h->nub = nub;
	h->slot = slot;
	heads[headCount++] = h;   // before registerService: the framebuffer may start at once
	nub->registerService();
	HLOG(h, "nub registered, dependent %zu of 0x%llx", slot + 1, id);
}

void createHeads(IOPCIDevice *pci) {
	const size_t n = RDNA4Device::extraHeads();
	for (size_t i = 0; i < n; i++)
		createHead(pci, i);
}

// Where head h's surface may start: behind the console, in a slot of its own
// that holds the largest mode a pipe is lit at (3840x2160, 31.6 MiB) and the
// gap Ndrv::spareSurface leaves, so a head can grow without moving another.
constexpr uint64_t kHeadSlotBytes = 34ull << 20;

uint64_t surfaceFloor(const Head2 *h, const RDNA4Device &dev) {
	return dev.fbPhysBase + dev.fbLength + h->slot * kHeadSlotBytes;
}

// Once head 0's device exists: what head 2 serves.
// The modes of the sink whose EDID is in h->edid that the head can serve, and
// the one it starts in: the first, in table order (native first), whose
// surface fits. With the pipe to be lit (rdna4-head2 >= 2) that has to be
// the mode the second-pipe plan was generated for, and the table only holds
// what the pipe can be switched to.
const Modes::Mode *head2BuildTable(Head2 *h, RDNA4Device &dev, size_t &n) {
	const Edid::DetailedTiming &lit = Pipe2::config().timing;
	n = Modes::build(h->edid, h->edidLen, head2Level >= 2 ? dev.secondPipeModeLimits() : Modes::Limits {},
	                 h->table, Modes::MaxModes);
	if (head2Level >= 2) {
		size_t kept = 0;
		for (size_t i = 0; i < n; i++)
			if (dev.secondPipeModeOk(h->table[i].t))
				h->table[kept++] = h->table[i];
		n = kept;
		// A sink whose own timing of that size and rate won (a monitor's
		// CVT-RB 4K60 over CEA VIC 97) gets the plan's, which it lists too.
		const uint8_t vic = Edid::vicOf(lit);
		if (vic && Edid::hasVic(h->edid, h->edidLen, vic))
			Modes::ensure(h->table, n, Modes::MaxModes, lit, Modes::SourceVic);
	}
	for (size_t i = 0; i < n; i++) {
		const Edid::DetailedTiming &t = h->table[i].t;
		if (head2Level >= 2 && !Edid::sameTiming(t, lit))
			continue;
		if (Ndrv::spareSurface(dev.fbPhysBase, surfaceFloor(h, dev) - dev.fbPhysBase, h->rangeBase, h->rangeLen,
		                       t.hActive, t.vActive, h->surface))
			return &h->table[i];
	}
	return nullptr;
}

// WindowServer maps a framebuffer through getVRAMRange, which IONDRVFramebuffer
// only has for an IOBootNDRV: without this it gives up on the head with
// "Failed to map VRAM" (card boot of 2026-10-03, 19:46). The range is the
// largest surface the head's slot holds: the framebuffer maps it once, so a
// head lit at 3840x2160 after waiting offline with a 1080p surface showed
// only the top of the picture (the TV, 2026-11-03).
constexpr uint64_t kHeadSurfaceMaxBytes = 3840ull * 4 * 2160;
static_assert(kHeadSurfaceMaxBytes + (2ull << 20) <= kHeadSlotBytes, "a 3840x2160 surface fits a head's slot");

void head2MapSurface(Head2 *h) {
	const uint64_t bytes = static_cast<uint64_t>(h->surface.rowBytes) * h->surface.height;
	IODeviceMemory *old = h->vram;

	h->vram = IODeviceMemory::withSubRange(h->range, h->surface.physBase - h->rangeBase,
	                                       bytes > kHeadSurfaceMaxBytes ? bytes : kHeadSurfaceMaxBytes);
	OSSafeReleaseNULL(old);
	if (!h->vram)
		HLOG(h, "no VRAM range for the surface: WindowServer will not map it");
}

// Hand the table to the translator, with `mode` as the one the head is in.
void head2Init(Head2 *h, RDNA4Device &dev, const Modes::Mode *mode, size_t n) {
	Ndrv::Backend be {};
	be.ctx = h;
	be.surfaceFor = head2SurfaceFor;
	// Only a lit pipe answers cscGetSync, so only then does macOS send this
	// head display sleep (a phantom head has nothing to put to sleep).
	if (dev.xp().lit)
		be.setPower = head2SetPower;
	// Like the boot display, the other modes are offered with rdna4-modeset=1.
	const bool switching = dev.xp().lit && dev.modesetRequested;
	if (switching)
		be.switchTo = head2SwitchTo;
	h->ndrv.init(switching ? h->table : mode, switching ? n : 1, mode->id, h->edid, h->edidLen, be);
	for (size_t i = 0; switching && i < n; i++) {
		const Modes::Mode &m = h->table[i];
		HLOG(h, "mode id %u %ux%u@%u.%03u %u kHz%s%s", m.id, m.t.hActive, m.t.vActive,
		      m.refreshMilliHz / 1000, m.refreshMilliHz % 1000, m.t.pixelClockKHz, &m == mode ? " (lit)" : "",
		      Pipe2::modeKnown(m.t) ? "" : " (on the lit mode's request timing)");
	}
}

// What macOS is told of a head's display: none while the heads are the paravirtual displays.
void tellConnected(Head2 *h, bool on) {
	h->ndrv.setConnected(on && !pvHeads);
}

void fillHead(Head2 *h, RDNA4Device &dev) {
	if (h->ready)
		return;
	dev.selectExtra(h->slot);
	IOService *client = h->nub->getClient();
	HLOG(h, "framebuffer on the nub: %s", client ? client->getName() : "none started");
	h->dev = &dev;
	for (UInt32 i = 0; i < h->pci->getDeviceMemoryCount(); i++) {
		IODeviceMemory *mem = h->pci->getDeviceMemoryWithIndex(i);
		if (!mem)
			continue;
		const uint64_t base = mem->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
		if (dev.fbPhysBase >= base && dev.fbPhysBase - base < mem->getLength()) {
			h->range = mem;
			h->rangeBase = base;
			h->rangeLen = mem->getLength();
			// With the compute bring-up the surface has to end below the compute pool, which never starts
			// under RDNA4Compute::kPoolFloor.
			if (computeStage && h->rangeLen > RDNA4Compute::kPoolFloor)
				h->rangeLen = RDNA4Compute::kPoolFloor;
			break;
		}
	}
	const Edid::DetailedTiming &lit = Pipe2::config().timing;
	// A pipe to light needs a second monitor. Until one is plugged in the
	// head is there but offline (rdna4-hotplug=2): macOS shows no display.
	// For a pipe to light, a second display counts if it is on a connector
	// there is a plan for.
	const bool sink = dev.xp().edidLen && (head2Level < 2 || dev.usePlanFor(dev.xp().hpd));
	h->waitClocks = head2Level >= 2 && sink && hotplugLevel >= 2 && computeStage >= RDNA4Compute::StageGfx &&
	                !dev.uhdClocks && dev.sinkTakesUhd(dev.xp().hpd);
	const bool offline = (head2Level >= 2 && !sink && hotplugLevel >= 2) || h->waitClocks;
	const Modes::Mode *mode = nullptr;
	size_t n = 0;
	if (offline) {
		if (h->waitClocks)
			HLOG(h, "the display on HPD%u takes 3840x2160: the head waits offline for the display clocks",
			      dev.xp().hpd);
		else if (dev.xp().edidLen)
			HLOG(h, "the display on HPD%u is not served: no second-pipe plan for that connector (a "
			      "DisplayPort one is off with rdna4-head2dp=0); the head waits offline", dev.xp().hpd);
		// The one mode the pipe will be lit in, on its surface; no EDID.
		h->table[0] = Modes::Mode { 1, lit, lit.refreshMilliHz(), true, Modes::SourceBoot };
		n = 1;
		if (Ndrv::spareSurface(dev.fbPhysBase, surfaceFloor(h, dev) - dev.fbPhysBase, h->rangeBase, h->rangeLen,
		                       lit.hActive, lit.vActive, h->surface))
			mode = &h->table[0];
	} else {
		// The second sink if one answered, else a copy of the boot display's.
		const uint8_t *edid = dev.xp().edidLen ? dev.xp().edid : dev.edidData;
		h->edidLen = min(static_cast<size_t>(dev.xp().edidLen ? dev.xp().edidLen : dev.edidLen), sizeof(h->edid));
		if (h->edidLen < 128) {
			HLOG(h, "nothing to serve: no EDID");
			return;
		}
		memcpy(h->edid, edid, h->edidLen);
		// Without hot-plug a head with no monitor would be the phantom, an
		// invisible display for windows to get lost on: that is rdna4-head2=1,
		// asked for by name.
		if (head2Level >= 2 && !sink) {
			if (dev.xp().edidLen)
				HLOG(h, "nothing to serve: the second display is on HPD%u, a connector there is no "
				      "second-pipe plan for (a DisplayPort one is off with rdna4-head2dp=0)", dev.xp().hpd);
			else
				HLOG(h, "nothing to serve: no second sink answered on DDC (hot-plug is off: rdna4-hotplug=2 "
				      "waits for one, rdna4-head2=1 makes a phantom head)");
			return;
		}
		mode = head2BuildTable(h, dev, n);
	}
	if (!mode) {
		if (head2Level >= 2)
			HLOG(h, "nothing to serve: the sink has no %ux%u mode at %u kHz, the one the "
			      "second-pipe plan is for, or it does not fit behind the console", lit.hActive,
			      lit.vActive, lit.pixelClockKHz);
		else
			HLOG(h, "nothing to serve: no mode of the sink fits behind the console");
		return;
	}
	if (!offline && head2Level >= 2 && dev.isAmd)
		dev.lightSecondPipe(head2Level, h->surface.physBase);
	head2Init(h, dev, mode, n);
	h->connected = !offline;
	tellConnected(h, h->connected);
	head2MapSurface(h);
	h->ready = true;
	HLOG(h, "serving %ux%u@%u.%03u on surface 0x%llx, %s", mode->t.hActive,
	      mode->t.vActive, mode->refreshMilliHz / 1000, mode->refreshMilliHz % 1000, h->surface.physBase,
	      offline ? "offline until a display is plugged in" : dev.xp().edidLen ? "EDID of the second sink"
	              : "EDID of the boot display (no second sink)");
}

void fillHeads(RDNA4Device &dev) {
	for (size_t i = 0; i < headCount; i++)
		fillHead(heads[i], dev);
	dev.selectExtra(0);
}

// --- hot-plug of the second pipe's connector ---------------------------------
// The pin is polled: twice a second is soon enough, and it needs no interrupt
// from the card. A change is told to macOS through the handler IOFramebuffer
// registered for connect interrupts; it then asks cscGetConnection again,
// takes the framebuffer online or offline and reads the EDID and the modes.

void head2ConnectChanged(Head2 *h) {
	if (h->connectProc)
		h->connectProc(h->connectTarget, h->connectRef);
	else
		HLOG(h, "hotplug: macOS registered no connect handler: it is not told");
}

// A display on the connector: read what it is and let macOS find it. For
// another display than before the pipe goes to the mode it is lit in (and is
// lit, if this is the first display it sees), which is where a new mode table
// starts.
void head2Plugged(Head2 *h, uint8_t hpd) {
	RDNA4Device &dev = *h->dev;
	dev.selectExtra(h->slot);
	if (!dev.readSecondEdid(hpd)) {
		HLOG(h, "hotplug: HPD%u is high but there is no EDID on DDC (yet)", hpd);
		return;
	}
	// A lit pipe whose connector has another kind of sink now changes table:
	// another display than before, whatever its EDID.
	if (dev.adoptSinkKind(hpd))
		h->edidLen = 0;

	// A pipe that is not lit yet takes the plan for this connector.
	if (!dev.xp().lit && !dev.usePlanFor(hpd)) {
		HLOG(h, "hotplug: no second-pipe plan for HPD%u", hpd);
		return;
	}
	const Edid::DetailedTiming &lit = Pipe2::config().timing;
	const size_t len = min(static_cast<size_t>(dev.xp().edidLen), sizeof(h->edid));
	// The display that was there before: the link comes back in the mode it
	// was in, and macOS finds the head, its modes and their IDs as it left them.
	if (dev.xp().lit && len == h->edidLen && memcmp(h->edid, dev.xp().edid, len) == 0) {
		dev.secondPipeTo(hpd);
		if (!dev.xp().on) {
			HLOG(h, "hotplug: the link did not come back: the display stays offline");
			return;
		}
		h->connected = true;
		tellConnected(h, true);
		HLOG(h, "hotplug: the same display is back on HPD%u", Pipe2::config().hpd);
		head2ConnectChanged(h);
		return;
	}
	h->edidLen = len;
	memcpy(h->edid, dev.xp().edid, h->edidLen);
	size_t n = 0;
	const Modes::Mode *mode = head2BuildTable(h, dev, n);
	if (!mode) {
		HLOG(h, "hotplug: the display has no %ux%u mode at %u kHz, the one the pipe is lit in: left off",
		      lit.hActive, lit.vActive, lit.pixelClockKHz);
		// Not served: the next plug is not "the same display" for it.
		h->edidLen = 0;
		return;
	}
	if (!dev.xp().lit) {
		dev.lightSecondPipe(head2Level, h->surface.physBase);
	} else {
		dev.secondPipeTo(hpd);
		if (dev.xp().on && !Edid::sameTiming(dev.xp().target.now, lit))
			dev.applySecondPipeMode(*mode);
	}
	if (!dev.xp().lit || !dev.xp().on) {
		HLOG(h, "hotplug: the pipe did not come up: the display stays offline");
		return;
	}
	head2MapSurface(h);
	head2Init(h, dev, mode, n);
	h->connected = true;
	tellConnected(h, true);
	HLOG(h, "hotplug: display connected on HPD%u: %lu mode(s), EDID %lu bytes", Pipe2::config().hpd,
	      static_cast<unsigned long>(n), static_cast<unsigned long>(h->edidLen));
	head2ConnectChanged(h);
}

void head2Unplugged(Head2 *h) {
	h->dev->selectExtra(h->slot);
	h->dev->setSecondPipePower(false);
	h->connected = false;
	tellConnected(h, false);
	HLOG(h, "hotplug: display gone from HPD%u: link off", Pipe2::config().hpd);
	head2ConnectChanged(h);
}

constexpr uint32_t kHpdPollMs = 500;

// A minute for the compute bring-up to raise the display clocks.
constexpr uint32_t kClockWaitPolls = 60000 / kHpdPollMs;

// Whether a head waiting for the display clocks can be plugged in now.
bool clocksSettled(Head2 *h, const RDNA4Device &dev) {
	if (dev.uhdClocks) {
		HLOG(h, "display clocks raised: lighting the display");
		return true;
	}

	if (++h->waitPolls < kClockWaitPolls)
		return false;

	HLOG(h, "display clocks not raised after a minute: lighting the display at the size it can have");

	return true;
}

void hotplugPoll(OSObject *owner, IOTimerEventSource *timer) {
	Head2 *h = headFor(owner);
	if (!h)
		return;
	RDNA4Device &dev = *h->dev;
	dev.selectExtra(h->slot);
	// A display that is gone may come back on another connector.
	const uint8_t hpd = dev.secondSinkHpd(!h->connected);
	const bool present = hpd != 0;
	if (present != h->hpdSeen) {
		if (present)
			h->hpdPin = hpd;
		h->hpdBack = present && h->connected && h->hpdSame >= 1;
		HLOG(h, "hotplug: HPD%u went %s", h->hpdPin, present ? "high" : "low");
		h->hpdSeen = present;
		h->hpdSame = 0;
	} else {
		h->hpdSame++;
	}
	// Low only counts while the link is up: a monitor that macOS has put to
	// sleep may drop the pin, and takes a while to raise it again on waking.
	h->hpdGone = present || !dev.xp().on ? 0 : h->hpdGone + 1;
	if (h->waitClocks && clocksSettled(h, dev)) {
		h->waitClocks = false;
		if (present && !h->connected)
			head2Plugged(h, hpd);
	}
	if (hotplugLevel >= 2) {
		// A second of high, and again after 3 and 7 s if the display's EDID
		// was not there yet; three seconds of low.
		if (!h->connected && !h->waitClocks && present &&
		    (h->hpdSame == 2 || h->hpdSame == 6 || h->hpdSame == 14))
			head2Plugged(h, hpd);
		// Six seconds on DisplayPort: the test monitor drops its pin for up to
		// 3.7 s on waking from display sleep (card, 2026-10-06). A shorter
		// drop may still have cost the link: looked at a second after the pin
		// is back.
		else if (h->connected && h->hpdGone >= (Pipe2::config().dp ? 12u : 6u))
			head2Unplugged(h);
		else if (h->hpdBack && present && h->hpdSame == 2) {
			h->hpdBack = false;
			dev.secondLinkBack();
		}
	}
	timer->setTimeoutMS(kHpdPollMs);
}

// --- the boot display on DisplayPort (rdna4-dptrain) --------------------------
// The firmware keeps the boot display's link itself: with the link taken down
// under it, it had trained it again within two seconds (card, 2026-10-06),
// and the monitor comes back after a cable pull with the kext doing nothing.
// So the pin is only watched and the link's state logged when it returns; a
// second trainer on the same link would be in the firmware's way.
// rdna4-dptrain=2 still retrains once, to try the kext's own training.
struct BootDp {
	RDNA4Device        *dev { nullptr };
	IOTimerEventSource *timer { nullptr };
	bool                seen { false };   // the pin at the last poll
	bool                gone { false };   // it was low for a second since the link was last good
	uint32_t            same { 0 };       // polls it has read that way
	uint32_t            polls { 0 };
} bootDp;

void bootDpPoll(OSObject *, IOTimerEventSource *timer) {
	RDNA4Device &dev = *bootDp.dev;
	const bool present = dev.bootSinkPresent();
	if (present != bootDp.seen) {
		FBLOG("dptrain: the boot display's HPD pin went %s", present ? "high" : "low");
		bootDp.seen = present;
		bootDp.same = 0;
	} else {
		bootDp.same++;
	}
	if (!present && bootDp.same == 2)
		bootDp.gone = true;
	if (present && bootDp.gone && bootDp.same == 2) {
		bootDp.gone = false;
		dev.logBootLink("the display is back");
	}
	// rdna4-dptrain=2: once, 15 s after the framebuffer opened, with the
	// cable left alone, to try the training by itself.
	if (dev.dpTrainLevel >= 2 && ++bootDp.polls == 15000 / kHpdPollMs)
		dev.retrainBootLink("self-test");
	// ... and what the link is like once that has settled.
	if (dev.dpTrainLevel >= 2 && (bootDp.polls == 17000 / kHpdPollMs || bootDp.polls == 25000 / kHpdPollMs))
		dev.logBootLink(bootDp.polls == 17000 / kHpdPollMs ? "2 s later" : "10 s later");
	timer->setTimeoutMS(kHpdPollMs);
}

void startBootDpPoll(IOService *fb, RDNA4Device &dev) {
	IOWorkLoop *loop = fb->getWorkLoop();
	bootDp.timer = loop ? IOTimerEventSource::timerEventSource(fb, bootDpPoll) : nullptr;
	if (!bootDp.timer || loop->addEventSource(bootDp.timer) != kIOReturnSuccess) {
		FBLOG("dptrain: no timer on the boot display's work loop: off");
		OSSafeReleaseNULL(bootDp.timer);
		return;
	}
	bootDp.dev = &dev;
	bootDp.seen = dev.bootSinkPresent();
	FBLOG("dptrain: watching the boot display's HPD pin (now %s)%s", bootDp.seen ? "high" : "low",
	      dev.dpTrainLevel >= 2 ? "; self-test in 15 s" : "");
	bootDp.timer->setTimeoutMS(kHpdPollMs);
}

void startHead2Hotplug(Head2 *h, IOService *fb, void (*proc)(OSObject *, void *), OSObject *target, void *ref) {
	h->dev->selectExtra(h->slot);
	IOWorkLoop *loop = fb->getWorkLoop();
	h->hpdTimer = loop ? IOTimerEventSource::timerEventSource(fb, hotplugPoll) : nullptr;
	if (!h->hpdTimer || loop->addEventSource(h->hpdTimer) != kIOReturnSuccess) {
		HLOG(h, "hotplug: no timer on the head's work loop: off");
		OSSafeReleaseNULL(h->hpdTimer);
		return;
	}
	h->connectProc = proc;
	h->connectTarget = target;
	h->connectRef = ref;
	h->hpdPin = h->dev->secondSinkHpd();
	h->hpdSeen = h->hpdPin != 0;
	if (!h->hpdSeen)
		h->hpdPin = Pipe2::config().hpd;
	HLOG(h, "hotplug: polling HPD%u (now %s), %s; the head is %s", h->hpdPin, h->hpdSeen ? "high" : "low",
	      hotplugLevel >= 2 ? "acting on it" : "logging only (rdna4-hotplug=2 acts)",
	      h->connected ? "connected" : "offline");
	h->hpdTimer->setTimeoutMS(kHpdPollMs);
}

// IOFramebuffer registers its connect interrupt handler when it opens: by
// then it has its work loop, which is where the polls above run. There is no
// interrupt service behind an IOBootNDRV, so the registration fails as
// before; head 2's handler is kept for its poll to call.
IOReturn wrapRegisterForInterruptType(void *fb, IOSelect type, void (*proc)(OSObject *, void *),
                                      OSObject *target, void *ref, void **interruptRef) {
	const IOReturn ret = FunctionCast(wrapRegisterForInterruptType, orgRegisterForInterruptType)(
		fb, type, proc, target, ref, interruptRef);
	if (type != kIOFBConnectInterruptType)
		return ret;
	auto *service = static_cast<IOService *>(fb);
	if (Head2 *h = headFor(fb)) {
		if (hotplugLevel && h->ready && h->dev && !h->hpdTimer && head2Level >= 2 && h->dev->isAmd)
			startHead2Hotplug(h, service, proc, target, ref);
		return ret;
	}
	FbEntry *e = fbEntry(fb);
	if (e && e->ours && e->state && e->state->dev.bootDpLinkKnown && !bootDp.timer)
		startBootDpPoll(service, e->state->dev);
	return ret;
}

// Head 0's framebuffer: on the GPU itself, or on a "display" nub IONDRVSupport hangs under it (the B550 board of 2026-11:
// there, missing it left the head refused for good, its pipe scanning stale VRAM). The heads' own nubs are skipped; of
// several, the oldest: the boot display's comes first, other "display" nubs' framebuffers only once the device is up.
IOService *bootFramebuffer(IOPCIDevice *pci) {
	IOService *best = nullptr;
	auto consider = [&](OSObject *o) {
		auto *fb = OSDynamicCast(IOService, o);
		if (fb && fb->metaCast("IONDRVFramebuffer") && (!best || fb->getRegistryEntryID() < best->getRegistryEntryID()))
			best = fb;
	};
	if (OSIterator *clients = pci->getClientIterator()) {
		while (OSObject *client = clients->getNextObject()) {
			consider(client);
			auto *nub = OSDynamicCast(IOService, client);
			bool head = false;
			for (size_t i = 0; i < headCount; i++)
				head |= nub == heads[i]->nub;
			if (!nub || head)
				continue;
			if (OSIterator *fbs = nub->getClientIterator()) {
				while (OSObject *fb = fbs->getNextObject())
					consider(fb);
				fbs->release();
			}
		}
		clients->release();
	}
	return best;
}

// The stand-in for IOBootNDRV::doDriverIO.
IOReturn head2DriverIO(Head2 *h, void *contents, UInt32 commandCode, uint16_t &code) {
	if (!h->ready) {
		// macOS opened this head before head 0: build head 0's device, which
		// is where the EDID and the console geometry come from, now.
		IOService *fb0 = bootFramebuffer(h->pci);
		if (fb0)
			HLOG(h, "opened before head 0: building head 0's device through framebuffer %p on %s", fb0,
			     fb0->getProvider() ? fb0->getProvider()->getName() : "-");
		FbEntry *e0 = fb0 ? fbEntry(fb0) : nullptr;
		if (e0 && e0->ours && !e0->tried)
			attach(*e0);
		if (!h->ready)
			return kIOReturnNotReady;
	}
	if (commandCode == kIONDRVInitializeCommand || commandCode == kIONDRVOpenCommand)
		return kIOReturnSuccess;
	const bool isStatus = commandCode == kIONDRVStatusCommand;
	if ((!isStatus && commandCode != kIONDRVControlCommand) || !contents)
		return kIOReturnUnsupported;
	auto *pb = static_cast<NdrvControlParameters *>(contents);
	code = pb->code;
	int32_t r = 0;
	if (isStatus ? h->ndrv.status(code, pb->params, r) : h->ndrv.control(code, pb->params, r))
		return static_cast<IOReturn>(r);
	return static_cast<IOReturn>(Ndrv::bootReply(isStatus, code));
}

// IONDRVFramebuffer::getVRAMRange returns IOBootNDRV's console range, so
// nothing for head 2: hand out its surface instead (retained, as the original
// does).
IODeviceMemory *wrapGetVRAMRange(void *fb) {
	Head2 *h = headFor(fb);
	if (h && h->vram) {
		h->vram->retain();
		return h->vram;
	}
	return FunctionCast(wrapGetVRAMRange, orgGetVRAMRange)(fb);
}

bool wrapStart(void *fb, IOService *provider) {
	auto *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!headCount && pci && isOurDevice(pci))
		createHeads(pci);
	return FunctionCast(wrapStart, orgStart)(fb, provider);
}

// Create the state for one of our framebuffers, once. Runs after the
// original handled Open (or, if the route arrived later, before the first
// Control/Status), so IOBootNDRV already owns the console framebuffer.
void attach(FbEntry &e) {
	e.tried = true;
	auto *svc = static_cast<IOService *>(e.fb);
	IOPCIDevice *pci = pciFor(svc->getProvider());
	auto *st = new FbState;
	if (!st) {
		FBLOG("ndrv: out of memory for the device state");
		return;
	}
	RDNA4Device &dev = st->dev;
	if (!dev.init(pci, svc)) {
		FBLOG("ndrv: device init failed, leaving %p to IOBootNDRV", e.fb);
		delete st;
		return;
	}
	// A test boot's stage (rdna4-trial) is only known once NVRAM is.
	computeStage = RDNA4Compute::requestedStage();
	Ndrv::Backend be { &dev, deviceSurfaceFor, deviceSwitchTo, deviceSetPower,
	                   deviceSupportsHardwareCursor, deviceSetHardwareCursor,
	                   deviceDrawHardwareCursor, deviceGetHardwareCursorDrawState };
#ifdef AMDRadeonRDNA4_VM_TEST
	if (!dev.isAmd) {
		pci->setIOEnable(true);
		if (BochsVbe::present()) {
			be.switchTo = vbeSwitchTo;
			vmWidth = static_cast<uint16_t>(dev.fbWidth);
			vmHeight = static_cast<uint16_t>(dev.fbHeight);
			FBLOG("vm: mode switches go through the Bochs VBE interface");
		} else {
			FBLOG("vm: no Bochs VBE interface, mode switches refused");
		}
	}
#endif
	st->ndrv.init(dev.modeTable, dev.modeCount, dev.defaultModeId, dev.edidData, dev.edidLen, be);
#ifdef AMDRadeonRDNA4_PVGPU
	// A test boot's level (rdna4-trial) is only known once NVRAM is; the heads' framebuffers are told before macOS asks.
	uint32_t pvTrial = 0, pvDisp = 0;

	if (!pvGpu && Trial::option("rdna4-pvgpu", pvTrial) && pvTrial <= 2)
		pvGpu = pvTrial;

	pvHeads = pvGpu && dev.isAmd && Trial::option("rdna4-pvgpu-disp", pvDisp) && (pvDisp & 4);
	if (pvHeads) {
		st->ndrv.setConnected(false);
		FBLOG("ndrv: the heads are the paravirtual displays: no display on our framebuffers for macOS");
	}

	// Before NVRAM is up a test boot looks like a normal one: decided only once the variable could be read (or boot-args set it),
	// or the first WindowServer opened the boot framebuffer as macOS's display and died in its first frame (pvhooks.cpp).
	if (dev.isAmd && (Trial::loaded() || pvGpu))
		PvScanout::decided(pvHeads);
#endif
	e.state = st;
	bootState = st;
	FBLOG("ndrv: answering for %p: %lu mode(s), EDID %lu bytes", e.fb,
	      static_cast<unsigned long>(st->ndrv.modeCount()), static_cast<unsigned long>(dev.edidLen));
	fillHeads(dev);
	// Behind the extra heads' slots, below the compute pool (128 MiB).
	VramBench::run(dev.fbPhysBase, ((dev.fbLength + (1ull << 20) - 1) & ~((1ull << 20) - 1)) +
	                               RDNA4Device::kMaxExtra * kHeadSlotBytes + (2ull << 20));

	// E1 of docs/metal-spike.md: an IOAccelerator-shaped service that only logs what Metal asks of it. Independent of the compute bring-up.
	if (accelCensus && dev.isAmd)
		RDNA4AccelCensus::publish(pci, accelCensus);
	// M0: the fake paravirtual GPU, independent of everything else here, after the heads it may drive.
#ifdef AMDRadeonRDNA4_PVGPU
	if (pvGpu && dev.isAmd && !pvDelay)
		RDNA4PvNub::publish(pci, pvGpu);
	if (pvGpu && dev.isAmd && pvDelay)
		RDNA4PvNub::setLateParent(pci);
	if (pvGpu && dev.isAmd)
		RDNA4PvNub::setCompute(&st->compute);
#endif

	// Compute bring-up runs after the display is answered for, and only
	// when asked for: it must never be the reason the desktop is missing.
	if (computeStage && dev.isAmd) {
		RDNA4Compute::Env env { pci, svc, dev.mmioBase(), dev.mmioSize(), dev.discovery(),
		                        dev.fbPhysBase, dev.fbLength, dev.liveFramePeriodNs(),
		                        createVblankService, deviceCursorProbe, &dev };
		env.smuReady = deviceSmuReady;
		st->compute.start(env, computeStage);
	}
}

const char *commandName(UInt32 code) {
	switch (code) {
	case kIONDRVOpenCommand:       return "Open";
	case kIONDRVCloseCommand:      return "Close";
	case kIONDRVControlCommand:    return "Control";
	case kIONDRVStatusCommand:     return "Status";
	case kIONDRVInitializeCommand: return "Initialize";
	case kIONDRVFinalizeCommand:   return "Finalize";
	default:                       return "?";
	}
}

IOReturn wrapDoDriverIO(void *fb, UInt32 commandID, void *contents, UInt32 commandCode,
                        UInt32 commandKind) {
	auto org = FunctionCast(wrapDoDriverIO, orgDoDriverIO);
	FbEntry *e = fbEntry(fb);
	if (!e || !e->ours)
		return org(fb, commandID, contents, commandCode, commandKind);

	// The phantom head has no NDRV: the original would refuse every command.
	if (Head2 *h = headFor(fb)) {
		uint16_t code = 0;
		const IOReturn ret = head2DriverIO(h, contents, commandCode, code);
		const bool lifecycle = commandCode != kIONDRVControlCommand && commandCode != kIONDRVStatusCommand;
		const bool traced = !lifecycle && traceEnabled && h->traceBudget;
		if (traced)
			h->traceBudget--;
		if (lifecycle || traced)
			HLOG(h, "%s csc %u -> 0x%x", commandName(commandCode), code, ret);
		return ret;
	}

	const bool isStatus = commandCode == kIONDRVStatusCommand;
	const bool csc = (isStatus || commandCode == kIONDRVControlCommand) && contents;
	if (csc && !e->tried)
		attach(*e);

	IOReturn ret = kIOReturnSuccess;
	bool answered = false;
	uint16_t code = 0;
	if (csc) {
		auto *pb = static_cast<NdrvControlParameters *>(contents);
		code = pb->code;
		if (e->state) {
			int32_t r = 0;
			answered = isStatus ? e->state->ndrv.status(code, pb->params, r)
			                    : e->state->ndrv.control(code, pb->params, r);
			if (answered)
				ret = static_cast<IOReturn>(r);
		}
	}
	if (!answered)
		ret = org(fb, commandID, contents, commandCode, commandKind);
	if (commandCode == kIONDRVOpenCommand && ret == kIOReturnSuccess && !e->tried)
		attach(*e);

	// Mode switches are rare and the interesting part: always logged.
	if (csc && !isStatus && code == Ndrv::cscSwitchMode) {
		auto *pb = static_cast<NdrvControlParameters *>(contents);
		auto *sw = static_cast<const Ndrv::VDSwitchInfoRec *>(pb->params);
		FBLOG("ndrv: switch to mode %d -> 0x%x (%s)", sw ? sw->csData : -1, ret,
		      answered ? "rdna4" : "boot");
	}

	if (traceEnabled && traceBudget) {
		traceBudget--;
		if (csc)
			FBLOG("ndrv: %s csc %u -> 0x%x (%s)", commandName(commandCode), code, ret,
			      answered ? "rdna4" : "boot");
		else
			FBLOG("ndrv: %s -> 0x%x", commandName(commandCode), ret);
	}
	return ret;
}

// Apple IOGraphics IONDRVFramebuffer.cpp:849-890 creates and links the
// service directly; unlike doControl/doStatus at :1159-1170, VSLNew does not
// enter the controller work-loop gate.  The bring-up callback may therefore
// call it from the IH bring-up thread.  W1's deferred action calls the
// original VSLDoInterruptService, so IONDRV runs the registered IOFramebuffer
// callback exactly as it does for a real NDRV.
int32_t wrapVslNew(void *entryID, UInt32 type, void **service) {
	auto org = FunctionCast(wrapVslNew, orgVslNew);
	int32_t ret = org(entryID, type, service);
	if (ret == kIOReturnSuccess && type == ::kVBLInterruptServiceType && service && *service) {
		Ndrv::vslServiceCreated(*service,
		                        reinterpret_cast<Ndrv::VslDoInterruptService>(orgVslDo));
		FBLOG("ndrv: VBL interrupt service created (%p)", *service);
	}
	return ret;
}

int32_t wrapVslDispose(void *service) {
	auto org = FunctionCast(wrapVslDispose, orgVslDispose);
	Ndrv::vslServiceDisposed(service);
	return org(service);
}

int32_t wrapVslDo(void *service) {
	auto org = FunctionCast(wrapVslDo, orgVslDo);
	return org(service);
}

bool wrapVslPrepareCursor(void *cursorRef, void *descriptor, void *info) {
	auto org = FunctionCast(wrapVslPrepareCursor, orgVslPrepareCursor);
	return org(cursorRef, descriptor, info);
}

bool ndrvRouted = false;   // processKext ran: by Lilu's load notice, or by lateNdrvCheck

void processKext(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address,
                 size_t size) {
	if (index != kextIONDRVSupport.loadIndex || __atomic_exchange_n(&ndrvRouted, true, __ATOMIC_SEQ_CST))
		return;
	KernelPatcher::RouteRequest request {
		"__ZN17IONDRVFramebuffer10doDriverIOEjPvjj", wrapDoDriverIO, orgDoDriverIO,
	};
	if (patcher.routeMultiple(index, &request, 1, address, size))
		FBLOG("ndrv: routed IONDRVFramebuffer::doDriverIO");
	else
		FBLOG("ndrv: failed to route IONDRVFramebuffer::doDriverIO (error %d)",
		      patcher.getError());
	patcher.clearError();
	// The second display and its hot-plug are on unless turned off: lit at
	// boot, mode switching, display sleep and hot-plug ran on the card for an
	// HDMI monitor on either connector (2026-10-04..06). Not in VM test
	// builds, which have no second pipe to light.
#ifdef AMDRadeonRDNA4_VM_TEST
	uint32_t second = 0;
#else
	uint32_t second = 4;
#endif
	PE_parse_boot_argn("rdna4-head2", &second, sizeof(second));
	if (second) {
		head2Level = second;
		KernelPatcher::RouteRequest start {
			"__ZN17IONDRVFramebuffer5startEP9IOService", wrapStart, orgStart,
		};
		if (patcher.routeMultiple(index, &start, 1, address, size))
			FBLOG("head2: routed IONDRVFramebuffer::start");
		else
			FBLOG("head2: not available: no IONDRVFramebuffer::start route (error %d)",
			      patcher.getError());
		patcher.clearError();
		KernelPatcher::RouteRequest vram {
			"__ZN17IONDRVFramebuffer12getVRAMRangeEv", wrapGetVRAMRange, orgGetVRAMRange,
		};
		if (patcher.routeMultiple(index, &vram, 1, address, size))
			FBLOG("head2: routed IONDRVFramebuffer::getVRAMRange");
		else
			FBLOG("head2: no IONDRVFramebuffer::getVRAMRange route (error %d): WindowServer "
			      "will not map the second head", patcher.getError());
		patcher.clearError();
	}
	// Both kinds of hot-plug start their HPD poll when IOFramebuffer opens.
	uint32_t hotplug = second >= 2 ? 2 : 0, dptrain = 0;
	if (second >= 2)
		PE_parse_boot_argn("rdna4-hotplug", &hotplug, sizeof(hotplug));
	PE_parse_boot_argn("rdna4-dptrain", &dptrain, sizeof(dptrain));
	if (hotplug || dptrain) {
		KernelPatcher::RouteRequest connect {
			"__ZN17IONDRVFramebuffer24registerForInterruptTypeEjPFvP8OSObjectPvES1_S2_PS2_",
			wrapRegisterForInterruptType, orgRegisterForInterruptType,
		};
		if (patcher.routeMultiple(index, &connect, 1, address, size)) {
			hotplugLevel = hotplug;
			FBLOG("hotplug: routed IONDRVFramebuffer::registerForInterruptType (rdna4-hotplug=%u, "
			      "rdna4-dptrain=%u)", hotplug, dptrain);
		} else {
			FBLOG("hotplug: not available: no IONDRVFramebuffer::registerForInterruptType route (error %d)",
			      patcher.getError());
		}
		patcher.clearError();
	}
	uint32_t vbl = 0, cursor = 0;
	const bool vslRequested =
		(PE_parse_boot_argn("rdna4-vbl", &vbl, sizeof(vbl)) && vbl != 0) ||
		(PE_parse_boot_argn("rdna4-cursor", &cursor, sizeof(cursor)) && cursor != 0);
	if (!vslRequested) {
		FBLOG("ndrv: VSL routes disabled (rdna4-vbl and rdna4-cursor are off)");
		return;
	}

	KernelPatcher::RouteRequest vslNew {
		"__ZN17IONDRVFramebuffer22VSLNewInterruptServiceEPvjPP11_VSLService",
		wrapVslNew, orgVslNew,
	};
	if (!patcher.routeMultiple(index, &vslNew, 1, address, size))
		FBLOG("ndrv: VSLNewInterruptService route unavailable (error %d)", patcher.getError());
	patcher.clearError();
	KernelPatcher::RouteRequest vslDispose {
		"__ZN17IONDRVFramebuffer26VSLDisposeInterruptServiceEP11_VSLService",
		wrapVslDispose, orgVslDispose,
	};
	if (!patcher.routeMultiple(index, &vslDispose, 1, address, size))
		FBLOG("ndrv: VSLDisposeInterruptService route unavailable (error %d)", patcher.getError());
	patcher.clearError();
	KernelPatcher::RouteRequest vslDo {
		"__ZN17IONDRVFramebuffer21VSLDoInterruptServiceEP11_VSLService",
		wrapVslDo, orgVslDo,
	};
	if (!patcher.routeMultiple(index, &vslDo, 1, address, size))
		FBLOG("ndrv: VSLDoInterruptService route unavailable (error %d)", patcher.getError());
	patcher.clearError();
	KernelPatcher::RouteRequest vslPrepare {
		"__ZN17IONDRVFramebuffer33VSLPrepareCursorForHardwareCursorEPvP26IOHardwareCursorDescriptorP20IOHardwareCursorInfo",
		wrapVslPrepareCursor, orgVslPrepareCursor,
	};
	if (patcher.routeMultiple(index, &vslPrepare, 1, address, size))
		Ndrv::vslPrepareCursorInstalled(reinterpret_cast<Ndrv::VslPrepareCursor>(orgVslPrepareCursor));
	else
		FBLOG("ndrv: VSLPrepareCursor route unavailable (error %d)", patcher.getError());
	patcher.clearError();
}

// Boots where IONDRVSupport is loaded where Lilu does not see it: SimpleHDA loads it itself the moment the system kext collection is ready
// (2026-10-22: at 18 s, the framebuffer published 0.4 s later; Lilu's first load notice of the boot at 21 s). Lilu then never calls
// processKext, IONDRVFramebuffer starts without our routes and is gone, and the card is never brought up (only the firmware's picture).
// From 30 s on (by then the boot's kexts have loaded; on other boots processKext has run long before): if processKext has not run and
// IONDRVSupport is in the kernel's kext list, run it with that kext's address, then register the GPU again so that IONDRVFramebuffer matches
// again, now through our routes. Tried every 2 s until 120 s.
KernelPatcher *gPatcher = nullptr;
thread_call_t gLateNdrv = nullptr;

void lateNdrvCheck(thread_call_param_t, thread_call_param_t) {
	if (__atomic_load_n(&ndrvRouted, __ATOMIC_SEQ_CST) || !gPatcher)
		return;
	kmod_info_t *ndrv = nullptr;
	if (auto **kmods = reinterpret_cast<kmod_info_t **>(gPatcher->solveSymbol(KernelPatcher::KernelID, "_kmod")))
		for (kmod_info_t *k = *kmods; k && !ndrv; k = k->next)   // as Lilu walks it (kern_patcher.cpp processAlreadyLoadedKexts)
			if (!strncmp(k->name, kextIONDRVSupport.id, KMOD_MAX_NAME))
				ndrv = k;
	gPatcher->clearError();
	uint64_t up = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &up);
	if (!ndrv) {
		if (up < 120ull * NSEC_PER_SEC) {
			uint64_t deadline;
			clock_interval_to_deadline(2, kSecondScale, &deadline);
			thread_call_enter_delayed(gLateNdrv, deadline);
		} else {
			FBLOG("ndrv: IONDRVSupport never loaded and Lilu never reported it: the card is not brought up");
		}
		return;
	}
	FBLOG("ndrv: Lilu did not report IONDRVSupport's load (another kext loaded it); routing it now, at %llu s", up / NSEC_PER_SEC);
	// As Lilu before its load callbacks (kern_api.cpp processKextLoadCallbacks): the kext's running symbols, and its whole size (a kmod's is
	// only __TEXT since 11.0). Without this its symbols cannot be solved ("no loaded symbols buffer found", seen on the rig 2026-10-22).
	const size_t size = gPatcher->updateRunningInfo(kextIONDRVSupport.loadIndex, ndrv->address, ndrv->size);
	gPatcher->clearError();
	processKext(nullptr, *gPatcher, kextIONDRVSupport.loadIndex, ndrv->address, size);
	// The GPU again: IONDRVFramebuffer matches again (its first instance did not start without our routes).
	OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
	OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
	OSSafeReleaseNULL(match);
	while (OSObject *o = it ? it->getNextObject() : nullptr)
		if (auto *pci = OSDynamicCast(IOPCIDevice, o))
			if (pci->configRead16(kIOPCIConfigVendorID) == 0x1002 && pci->configRead8(kIOPCIConfigClassCode + 2) == 0x03) {
				FBLOG("ndrv: registering %s again for IONDRVFramebuffer", pci->getName());
				pci->registerService();
			}
	OSSafeReleaseNULL(it);
}

void pluginStart() {
	rdna4LogInit();
	FBLOG("version " AMDRADEONRDNA4_BUILD);
	uint32_t off = 0;
	if (PE_parse_boot_argn("rdna4-off", &off, sizeof(off)) && off) {
		FBLOG("disabled by rdna4-off boot-arg");
		return;
	}
	uint32_t trace = 0;
	const bool traceArg = PE_parse_boot_argn("rdna4-trace", &trace, sizeof(trace));
	rdna4TraceLevel = traceArg ? trace : 0;
	traceEnabled = rdna4TraceLevel != 0;
#ifdef AMDRadeonRDNA4_VM_TEST
	// VM test builds trace by default (OpenCore images often pin boot-args).
	if (!traceArg) {
		rdna4TraceLevel = 1;
		traceEnabled = true;
	}
#endif
	computeStage = RDNA4Compute::requestedStage();
	if (!PE_parse_boot_argn("rdna4-accelcensus", &accelCensus, sizeof(accelCensus)) || accelCensus > 2)
		accelCensus = 0;
#ifdef AMDRadeonRDNA4_PVGPU   // built in with make PVGPU=1 only (Makefile)
	if (!PE_parse_boot_argn("rdna4-pvgpu", &pvGpu, sizeof(pvGpu)) || pvGpu > 2)
		pvGpu = 0;
	// A VM without the emulated RDNA4 device never reaches attach(). rdna4-pvgpu-delay: on the rig the default 25 s falls into the system volume's snapshot
	// remount (~26 s), where freeze-watch.sh cannot write; a later publish comes after login, where a live kernel log over ssh sees what follows.
	PE_parse_boot_argn("rdna4-pvgpu-delay", &pvDelay, sizeof(pvDelay));
	if (pvGpu)
		RDNA4PvNub::publishLater(pvGpu, pvDelay ? pvDelay * 1000 : 25000);
#endif
	Ndrv::vslInit();
	FBLOG("Lilu plugin started (trace %s, compute stage %u%s%s)", traceEnabled ? "on" : "off",
	      computeStage, accelCensus ? ", ACCEL CENSUS (emulator only)" : "", pvGpu ? ", FAKE PARAVIRT GPU (VM only)" : "");
	lilu.onKextLoadForce(&kextIONDRVSupport, 1, processKext, nullptr);
#ifdef AMDRadeonRDNA4_PVGPU
	// whether this boot has the fake GPU is only known once NVRAM is (rdna4-trial): the functions are resolved in any case
	lilu.onKextLoadForce(&kextIOSurface, 1, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		if (index != kextIOSurface.loadIndex)
			return;
		// In the order of pvgpu.cpp's gSurfaceFns.
		static const char *const names[RDNA4PvNub::kSurfaceFunctions] = {
			"__ZN13IOSurfaceRoot13lookupSurfaceEjP4task", "__ZNK9IOSurface19getMemoryDescriptorEv", "__ZNK9IOSurface14getBytesPerRowEv",
			"__ZNK9IOSurface8getWidthEv", "__ZNK9IOSurface9getHeightEv", "__ZNK9IOSurface14getPixelFormatEv", "__ZNK9IOSurface13getBaseOffsetEv",
			"__ZNK9IOSurface12getSurfaceIDEv", "__ZNK9IOSurface13getPlaneCountEv", "__ZNK9IOSurface14getPlaneOffsetEj",
			"__ZNK9IOSurface19getPlaneBytesPerRowEj", "__ZNK9IOSurface13getPlaneWidthEj", "__ZNK9IOSurface14getPlaneHeightEj"};
		mach_vm_address_t fn[RDNA4PvNub::kSurfaceFunctions];
		unsigned found = 0;
		for (unsigned k = 0; k < RDNA4PvNub::kSurfaceFunctions; k++)
			found += (fn[k] = patcher.solveSymbol(index, names[k], address, size)) != 0;
		patcher.clearError();
		if (found == RDNA4PvNub::kSurfaceFunctions)
			RDNA4PvNub::setSurfaceFunctions(fn);
		FBLOG("pvgpu: IOSurface: %u of %u functions found%s", found, RDNA4PvNub::kSurfaceFunctions,
		      found == RDNA4PvNub::kSurfaceFunctions ? "" : ": no IOSurface textures");
	}, nullptr);
	PvHooks::start();
	PvScanout::start();
#endif
	lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
		gPatcher = &patcher;
#ifdef AMDRadeonRDNA4_PVGPU
		auto **base = reinterpret_cast<uint8_t **>(patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_base"));
		auto **max = reinterpret_cast<uint8_t **>(patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_max"));

		patcher.clearError();
		if (base && max)
			RDNA4PvNub::setPhysmap(*base, *max);
		FBLOG("pvgpu: physmap %s", base && max ? "found" : "NOT found: guest memory through mappings of each page");
#endif
	});
	if ((gLateNdrv = thread_call_allocate(lateNdrvCheck, nullptr))) {   // ponytail: never freed, it lives for the boot
		uint64_t deadline;
		clock_interval_to_deadline(30, kSecondScale, &deadline);
		thread_call_enter_delayed(gLateNdrv, deadline);
	}
}

const char *bootargDebug[] { "-rdna4dbg" };

} // namespace

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	nullptr, 0,          // disable args (rdna4-off is parsed in pluginStart)
	bootargDebug, 1,     // debug args
	nullptr, 0,          // beta args
	KernelVersion::BigSur,
	KernelVersion::Tahoe,
	pluginStart,
};

#ifdef AMDRadeonRDNA4_PVGPU
// --- the heads as the paravirtual displays (pvscanout.hpp) ---------------------

bool PvScanout::enabled()
{
	return pvHeads;
}

// The extra heads by port (port - 1), numbered by the first count().
const Head2 *pvPorts[RDNA4Device::kMaxExtra] {};
// The size each of them is lit in, or will be: what its paravirtual display is announced with.
uint32_t pvPortWidth[RDNA4Device::kMaxExtra] {}, pvPortHeight[RDNA4Device::kMaxExtra] {};
uint32_t pvPortCount { 0 };
bool pvPortsNumbered { false };

uint32_t PvScanout::count()
{
	if (!pvPortsNumbered) {
		for (size_t i = 0; i < headCount; i++) {
			const Head2 *h = heads[i];

			if (!h->connected && !h->waitClocks)
				continue;

			// a head waiting for the display clocks is lit at 3840x2160 once they are up (fillHead)
			pvPortWidth[pvPortCount] = h->waitClocks ? 3840 : h->surface.width;
			pvPortHeight[pvPortCount] = h->waitClocks ? 2160 : h->surface.height;
			pvPorts[pvPortCount++] = h;
		}

		pvPortsNumbered = true;
	}

	return 1 + pvPortCount;
}

bool PvScanout::mode(uint32_t port, uint32_t &width, uint32_t &height)
{
	Head head;

	if (PvScanout::head(port, head)) {
		width = head.width;
		height = head.height;

		return true;
	}

	if (port == 0 || port > pvPortCount)
		return false;

	width = pvPortWidth[port - 1];
	height = pvPortHeight[port - 1];

	return true;
}

bool PvScanout::head(uint32_t port, Head &out)
{
	if (port == 0) {
		if (!bootState)
			return false;

		const RDNA4Device &dev = bootState->dev;

		out = { dev.fbPhysBase, dev.fbRowBytes, dev.fbWidth, dev.fbHeight };

		return true;
	}

	if (port > pvPortCount)
		return false;

	const Head2 *h = pvPorts[port - 1];

	if (!h->ready || !h->connected)
		return false;

	out = { h->surface.physBase, h->surface.rowBytes, h->surface.width, h->surface.height };

	return true;
}
#endif
