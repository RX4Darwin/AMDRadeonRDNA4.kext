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
#include "sku.hpp"
#include "trial.hpp"
#include "vrambench.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOGraphicsTypes.h>   // header-only types for IOMacOSVideo.h
#include <IOKit/graphics/IOFramebuffer.h>     // (convertCursorImage: a virtual call, no IOGraphics symbol)
#include <IOKit/ndrvsupport/IOMacOSTypes.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include <IOKit/ndrvsupport/IONDRVLibraries.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include <mach/kmod.h>
#include <sys/sysctl.h>

#include "accelcensus.hpp"
#include "rdna4accel.hpp"
#ifdef AMDRadeonRDNA4_PVGPU
#include "pvgpu.hpp"
#include "iosurface.hpp"
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
NDRV_SAME_SIZE(VDDisplayTimingRangeRec);
NDRV_SAME_FIELD(VDDisplayTimingRangeRec, csMinFrameRate);
NDRV_SAME_FIELD(VDDisplayTimingRangeRec, csCharSizeReserved1);
NDRV_SAME_FIELD(VDDisplayTimingRangeRec, csMaxNumLinks);
NDRV_SAME_FIELD(VDDetailedTimingRec, csDisplayModeID);
NDRV_SAME_FIELD(VDDetailedTimingRec, csPixelClock);
NDRV_SAME_FIELD(VDDetailedTimingRec, csHorizontalActive);
NDRV_SAME_FIELD(VDDetailedTimingRec, csVerticalSyncConfig);
NDRV_SAME_FIELD(VDDetailedTimingRec, csNumLinks);
// Fields IOMacOSVideo.h still calls reserved: IOGraphicsTypes.h's V2 names.
static_assert(offsetof(Ndrv::VDDisplayTimingRangeRec, csSupportedPixelEncoding) == offsetof(::VDDisplayTimingRangeRec, csReserved6) &&
              offsetof(Ndrv::VDDetailedTimingRec, csPixelEncoding) == offsetof(::VDDetailedTimingRec, csReserved3) &&
              offsetof(Ndrv::VDDetailedTimingRec, csColorimetry) == offsetof(::VDDetailedTimingRec, csReserved4),
              "colour fields");
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

const char *kPathIOGraphics = "/System/Library/Extensions/IOGraphicsFamily.kext/IOGraphicsFamily";
KernelPatcher::KextInfo kextIOGraphics {
	"com.apple.iokit.IOGraphicsFamily", &kPathIOGraphics, 1, {true}, {},
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

// IOPCIFamily (boot kernel collection, loaded before Lilu): debug.rdna4_hda routes IOPCIDevice::extendedConfigRead16 in it.
const char *kPathIOPCIFamily = "/System/Library/Extensions/IOPCIFamily.kext/Contents/MacOS/IOPCIFamily";
KernelPatcher::KextInfo kextIOPCIFamily {
	"com.apple.iokit.IOPCIFamily", &kPathIOPCIFamily, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded,
};

// AppleGFXHDA (system kernel collection, loaded once the audio function is handed to it): its AMD controller's display
// matching routed, to tie each display's framebuffer first and to register the displays again (docs/audio-plan.md).
const char *kPathAppleGFXHDA = "/System/Library/Extensions/AppleGFXHDA.kext/Contents/MacOS/AppleGFXHDA";
KernelPatcher::KextInfo kextAppleGFXHDA {
	"com.apple.driver.AppleGFXHDA", &kPathAppleGFXHDA, 1, {true}, {},
	KernelPatcher::KextInfo::Unloaded,
};

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
bool pvScaledModes { true };   // rdna4-pvscaled=0: no scaled modes above a head's 3840x2160
// rdna4-pvfast=1: the boot display's paravirtual modes at each size's fastest refresh, not the one nearest 60 Hz. macOS
// picks a size there, not a refresh (its paravirtual display tells the host the size alone): one refresh a size.
bool pvFastModes { false };

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
	if (vendor == 0x1002 && (device == Sku::kNavi48 || device == Sku::kNavi48Pro || device == Sku::kNavi44))
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

// The memory behind a scaled ("looks like") mode's framebuffer. Such a frame is larger than the head, and BAR0 has no room for one: the
// console and the head slots end where the compute pool starts, at 128 MiB. WindowServer draws it through our accelerator, so nothing
// needs it in VRAM, but IOFramebuffer maps and may clear rowBytes x height of its aperture. Wired host memory takes that, handed out by
// wrapGetApertureRange (not contiguous: 96 MiB in one piece was not to be had on the rig). Write-combined as IOFramebuffer maps it, and
// zeroed, as WindowServer can map it too.
// ponytail: sized for the head's largest frame (up to 97 MiB) and kept to the next boot, as IOFramebuffer can hold its map past a mode change.
struct ScaledMemory {
	IOBufferMemoryDescriptor *buffer { nullptr };
};

bool scaledMemoryFor(ScaledMemory &s, const Modes::Mode *frames, size_t n) {
	if (s.buffer)
		return true;
	if (!n)
		return false;
	const Modes::Mode &largest = frames[n - 1];
	const uint64_t bytes = (static_cast<uint64_t>(largest.t.hActive) * 4 * largest.t.vActive + 128 + 0xfff) & ~0xfffull;
	auto *b = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,
	                                                      bytes, PAGE_SIZE);
	if (!b || !b->getBytesNoCopy()) {
		FBLOG("modes: no %llu MiB of host memory for a scaled mode's framebuffer", bytes >> 20);
		OSSafeReleaseNULL(b);
		return false;
	}
	bzero(b->getBytesNoCopy(), bytes);
	s.buffer = b;
	FBLOG("modes: scaled framebuffer: %llu MiB of host memory", bytes >> 20);
	return true;
}

Modes::Mode gBootScaled[Modes::MaxScaled] {};   // the boot display's scaled modes (PvScanout::offerScaled)
size_t gBootScaledCount { 0 };
ScaledMemory gBootScaledMemory;
bool gScaledOffered { false };                  // our accelerator serves WindowServer: it can present their frames
bool gApertureRouted { false };                 // the PVGPU aperture route is available for scaled modes

// NDRV reports the same aperture and pitch the pipe uses for this mode.
bool deviceSurfaceFor(void *ctx, const Modes::Mode &m, bool, Ndrv::Surface &out) {
	auto *dev = static_cast<RDNA4Device *>(ctx);
	if (dev->dpAperture && !dev->dpPlaneReady())
		return false;
	if (m.source == Modes::SourceScaled) {
		out = { dev->fbPhysBase, m.t.hActive * 4u, m.t.hActive, m.t.vActive };   // (the base is only reported: wrapGetApertureRange)
		return true;
	}
	if (dev->largerThanConsole(m.t)) {
		if (!dev->dpAperture)
			return false;
		out = {dev->dpSurface.physBase, dev->dpSurface.rowBytes, m.t.hActive, m.t.vActive};
		return true;
	}
	out = { dev->fbPhysBase, dev->fbRowBytes, m.t.hActive, m.t.vActive };
	return true;
}

int32_t deviceSetRefresh(void *ctx, const Modes::Mode &m, uint32_t extLines) {
	return static_cast<RDNA4Device *>(ctx)->setBootRefresh(m, extLines);
}

int32_t deviceSetHdr(void *ctx, bool on) {
	return static_cast<RDNA4Device *>(ctx)->setBootHdr(on);
}

bool gAudioOn;   // the displays' audio is wanted (rdna4-audio=1 or debug.rdna4_hda): set up again after each switch

int32_t deviceSwitchTo(void *ctx, const Modes::Mode &m, bool) {
	auto *dev = static_cast<RDNA4Device *>(ctx);
	if (m.source != Modes::SourceScaled) {
		const IOReturn ret = dev->applyMode(m);
		if (ret == kIOReturnSuccess && gAudioOn)
			(void)dev->enableBootAudio();   // a mode switch can take the stream's audio packets down with it
		return ret;
	}
	// the head stays on (or goes to) its largest mode; the present path scales the frames down to it
	const Modes::Mode *base = Modes::scaleBase(dev->modeTable, dev->modeCount);
	if (!base || !scaledMemoryFor(gBootScaledMemory, gBootScaled, gBootScaledCount))
		return kIOReturnNoMemory;
	const Edid::DetailedTiming *live = dev->liveMode();
	const IOReturn ret = live && Edid::sameTiming(*live, base->t) ? kIOReturnSuccess : dev->applyMode(*base);
	if (ret == kIOReturnSuccess && gAudioOn)
		(void)dev->enableBootAudio();   // as for a real mode (also when the pipe could not take it at attach)
	return ret;
}

void deviceSetPower(void *ctx, bool on) {
	// a framebuffer macOS has no display on for must not blank the paravirtual display it scans out
	if (pvHeads && !on)
		return;

	static_cast<RDNA4Device *>(ctx)->setDisplayPower(on);
	if (on && gAudioOn)
		(void)static_cast<RDNA4Device *>(ctx)->enableBootAudio();
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
	return static_cast<RDNA4Device *>(ctx)->ndrvHardwareCursor();
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

void coldStartHeads(RDNA4Device &dev);

void deviceSmuReady(void *ctx) {
	RDNA4Device &dev = *static_cast<RDNA4Device *>(ctx);

	dev.breadcrumb("smuReady: display clocks");
	dev.smuStarted();
	dev.breadcrumb("smuReady: cold start");
	coldStartHeads(dev);   // rdna4-dcninit=1 only
	dev.breadcrumb("smuReady: done");
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
	Modes::Mode      scaled[Modes::MaxScaled] {};   // its "looks like" frames (offerScaledOn)
	size_t           scaledCount { 0 };
	ScaledMemory     scaledMemory;
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
	bool             coldPending { false };   // rdna4-coldboot=1: its pipe lit by coldStartHeads, not at start
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
mach_vm_address_t orgGetApertureRange { 0 };
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
	if (m.source == Modes::SourceScaled) {
		out = { static_cast<Head2 *>(ctx)->surface.physBase, m.t.hActive * 4u, m.t.hActive, m.t.vActive };   // (only reported: wrapGetApertureRange)
		return true;
	}
	const Ndrv::Surface &s = static_cast<Head2 *>(ctx)->surface;
	if (m.t.hActive > s.width || m.t.vActive > s.height)
		return false;
	out = { s.physBase, s.rowBytes, m.t.hActive, m.t.vActive };
	return true;
}

int32_t head2SwitchTo(void *ctx, const Modes::Mode &m, bool) {
	auto *h = static_cast<Head2 *>(ctx);
	const Modes::Mode *run = &m;
	if (m.source == Modes::SourceScaled) {   // the head on its largest mode; the present path scales the frames down to it
		run = Modes::scaleBase(h->table, h->ndrv.modeCount());
		if (!run || !scaledMemoryFor(h->scaledMemory, h->scaled, h->scaledCount))
			return kIOReturnNoMemory;
	}
	RDNA4Device::ExtraHold hold(*h->dev);
	h->dev->selectExtra(h->slot);
	const IOReturn ret = h->dev->applySecondPipeMode(*run);
	if (ret == kIOReturnSuccess && gAudioOn)
		(void)h->dev->enableExtraAudio();   // as the boot display's (deviceSwitchTo)
	return ret;
}

int32_t head2SetRefresh(void *ctx, const Modes::Mode &m, uint32_t extLines) {
	auto *h = static_cast<Head2 *>(ctx);
	RDNA4Device::ExtraHold hold(*h->dev);
	h->dev->selectExtra(h->slot);
	return h->dev->setSecondPipeRefresh(m, extLines);
}

void head2SetPower(void *ctx, bool on) {
	auto *h = static_cast<Head2 *>(ctx);

	if (pvHeads && !on)
		return;

	RDNA4Device::ExtraHold hold(*h->dev);
	h->dev->selectExtra(h->slot);
	if (h->connected || !on)             // the link of an unplugged display stays off
		h->dev->setSecondPipePower(on);
	if (on && h->connected && gAudioOn)
		(void)h->dev->enableExtraAudio();
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
	return dev.fbPhysBase + dev.displaySpan() + h->slot * kHeadSlotBytes;
}

// rdna4-vrr=1: offer variable refresh (docs/freesync-plan.md).
bool vrrWanted() {
	uint32_t vrr = 0;
	return Trial::bootArg("rdna4-vrr", vrr) && vrr;
}

// rdna4-hdr=1 and a sink whose EDID takes PQ.
bool hdrWanted(const uint8_t *edid, size_t edidLen) {
	uint32_t hdr = 0;
	return Trial::bootArg("rdna4-hdr", hdr) && hdr && Edid::takesPq(edid, edidLen);
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
		for (size_t i = 0; i < n; i++) {
			const auto &t = h->table[i].t;
			Ndrv::Surface available {};
			if (dev.secondPipeModeOk(t) &&
			    Ndrv::spareSurface(dev.fbPhysBase, surfaceFloor(h, dev) - dev.fbPhysBase, h->rangeBase, h->rangeLen,
			                       max(t.hActive, lit.hActive), max(t.vActive, lit.vActive), available))
				h->table[kept++] = h->table[i];
		}
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

	const uint64_t offset = h->surface.physBase - h->rangeBase;
	const uint64_t length = Ndrv::headMappingLength(offset, h->rangeLen, bytes, kHeadSurfaceMaxBytes);
	h->vram = length ? IODeviceMemory::withSubRange(h->range, offset, length) : nullptr;
	OSSafeReleaseNULL(old);
	if (!h->vram)
		HLOG(h, "no VRAM range for the surface: WindowServer will not map it");
}

// The head's "looks like" frames, for a head whose surface is its largest mode (what they are scaled into).
void offerScaledOn(Head2 *h) {
	const size_t n = h->ndrv.modeCount();
	const Modes::Mode *base = gApertureRouted && pvScaledModes && !pvHeads && n > 1 ? Modes::scaleBase(h->table, n) : nullptr;
	h->scaledCount = base && base->t.hActive == h->surface.width && base->t.vActive == h->surface.height
	                 ? Modes::buildScaled(h->table, n, h->scaled, Modes::MaxScaled) : 0;
	h->ndrv.setScaled(h->scaled, h->scaledCount);
	if (h->scaledCount)
		HLOG(h, "%zu scaled modes offered, up to %ux%u", h->scaledCount, h->scaled[h->scaledCount - 1].t.hActive,
		     h->scaled[h->scaledCount - 1].t.vActive);
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
	if (switching) {
		be.switchTo = head2SwitchTo;
		be.setRefresh = head2SetRefresh;
	}
	h->ndrv.init(switching ? h->table : mode, switching ? n : 1, mode->id, h->edid, h->edidLen, be);
	h->ndrv.adaptiveSync = vrrWanted();
	h->ndrv.hdr = hdrWanted(h->edid, h->edidLen);
	if (gScaledOffered)
		offerScaledOn(h);
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
			if (computeStage)
				h->rangeLen = min(h->rangeLen, RDNA4Compute::kPoolFloor);
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
	// After the cold-boot reset nothing can light a pipe before the SMU and DMUB run: coldStartHeads does.
	const bool lights = !offline && head2Level >= 2 && dev.isAmd;
	h->coldPending = lights && dev.coldBoot;
	if (lights && !dev.coldBoot)
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

// With the heads as the paravirtual displays (rdna4-boot4k=0 turns it off): the boot display, which the firmware lit behind a passive DP++
// adapter at what it carries (2560x1440), is taken over by the first head with no display of its own whose pipe has a table
// for its connector: once the display clocks are up the firmware's stream goes down (RDNA4Device::releaseBootPipe) and that
// head lights the connector at 3840x2160, as the port-0 paravirtual display. The head on the second connector does the same.
Head2 *pvBootHead { nullptr };

void pickBootHead(RDNA4Device &dev) {
	uint32_t on = 1;

	(void)Trial::option("rdna4-boot4k", on);

	if (!pvHeads || head2Level < 2 || hotplugLevel < 2 || !on)
		return;

	for (size_t i = 0; i < headCount; i++) {
		Head2 *h = heads[i];

		if (h->connected || h->waitClocks || h->slot >= RDNA4Device::kMaxExtra ||   // a free head may serve a placeholder
		    !dev.bootUhdPipe(dev.extra[h->slot].pipe))
			continue;

		h->waitClocks = true;
		pvBootHead = h;
		HLOG(h, "boot4k: takes the boot display over at 3840x2160 once the display clocks are up");
		return;
	}

	FBLOG("boot4k: no head can take the boot display over (no free head with a 3840x2160 table for its connector, or "
	      "the sink does not take it through the adapter)");
}

void head2Plugged(Head2 *h, uint8_t hpd);

// The boot display's connector to pvBootHead: the firmware's stream down, then the head plugged in on it.
void takeBootDisplay(Head2 *h, RDNA4Device &dev) {
	if (!dev.releaseBootPipe()) {
		HLOG(h, "boot4k: the boot display stays the firmware's");
		return;
	}

	const uint8_t hpd = dev.secondSinkHpd(true);

	if (!hpd) {
		HLOG(h, "boot4k: the boot display's pin is not offered to the head");
		return;
	}

	head2Plugged(h, hpd);
	h->ready = h->connected;
	HLOG(h, "boot4k: the boot display %s", h->connected ? "is the head's now" : "did NOT come up on the head");
}

// rdna4-dcninit=1: the cold-start test, once the kext's DMUB firmware runs and the clocks are raised; the extra
// pipes it put to sleep lit again by their heads' plans, as at boot.
void coldStartHeads(RDNA4Device &dev) {
	RDNA4Device::ExtraHold hold(dev);
	uint32_t relight = 0;

	(void)dev.coldStart(relight);
	for (size_t i = 0; i < headCount; i++) {
		Head2 *h = heads[i];

		if (!(relight & 1u << h->slot) && !h->coldPending)
			continue;
		const bool pending = h->coldPending;
		h->coldPending = false;
		dev.selectExtra(h->slot);
		char step[48];
		snprintf(step, sizeof(step), "cold start: head %zu lit again", h->slot + 2);
		dev.breadcrumb(step);
		dev.extraAuxPadsAfterPowerLoss();
		dev.lightSecondPipe(head2Level, h->surface.physBase);
		// A head the cold boot left dark at start (fillHead) set up as a lit one: its modes, switching and power.
		size_t n = 0;
		const Modes::Mode *mode = pending && dev.xp().lit ? head2BuildTable(h, dev, n) : nullptr;
		if (mode)
			head2Init(h, dev, mode, n);
		HLOG(h, "dcninit: lit again: %s%s; surface %ux%u at 0x%llx", dev.xp().lit && dev.xp().on ? "on" : "NOT on",
		      mode ? ", its modes set up" : "", h->surface.width, h->surface.height, h->surface.physBase);
	}
	dev.selectExtra(0);
	dev.coldStartPending = false;   // the second pipes' power requests act again
}

void fillHeads(RDNA4Device &dev) {
	RDNA4Device::ExtraHold hold(dev);
	for (size_t i = 0; i < headCount; i++)
		fillHead(heads[i], dev);
	pickBootHead(dev);
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
	RDNA4Device::ExtraHold hold(dev);
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
	RDNA4Device::ExtraHold hold(*h->dev);
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
	RDNA4Device::ExtraHold hold(dev);
	dev.selectExtra(h->slot);
	// A display that is gone may come back on another connector.
	const uint8_t hpd = dev.secondSinkHpd(!h->connected);
	const bool present = hpd != 0;
	if (present != h->hpdSeen) {
		if (present)
			h->hpdPin = hpd;
		// Any drop, even one shorter than a poll: a monitor switched off and on drops its pin for under 0.5 s and may
		// come back without the link or the DPCD settings it had (card, 2026-11-26).
		h->hpdBack = present && h->connected;
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
		if (h == pvBootHead)
			takeBootDisplay(h, dev);
		else if (present && !h->connected)
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
	bool                returned { false };   // it came back less than 7 s ago (variable refresh's DPCD check)
	uint32_t            same { 0 };       // polls it has read that way
	uint32_t            polls { 0 };
} bootDp;

void bootDpPoll(OSObject *, IOTimerEventSource *timer) {
	RDNA4Device &dev = *bootDp.dev;
	const bool present = dev.bootSinkPresent();
	if (present != bootDp.seen) {
		FBLOG("dptrain: the boot display's HPD pin went %s", present ? "high" : "low");
		bootDp.returned = present;
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
	// Variable refresh: the firmware trains the link again after the pin returns, which rewrites DPCD 0x107 (within 2 s
	// on the card); its ignore-MSA bit is looked at 1, 3 and 7 s after any return, however short the drop.
	if (present && (bootDp.same == 2 || bootDp.same == 6 || bootDp.same == 14) && bootDp.returned)
		dev.bootKeepMsaIgnore();
	if (bootDp.same == 14)
		bootDp.returned = false;
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
	RDNA4Device::ExtraHold hold(*h->dev);
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
	if (e && e->ours && e->state && e->state->dev.bootDpLinkKnown && !e->state->dev.coldBoot && !bootDp.timer)
		startBootDpPoll(service, e->state->dev);
	return ret;
}

#ifdef AMDRadeonRDNA4_PVGPU
// Our framebuffers' hardware cursor while our own accelerator serves WindowServer: the heads' cursor planes (PvScanout::cursorArgb,
// cursorMove) as AppleParavirtFramebuffer does it, which WindowServer takes: the image converted for a 128x128 32-bit cursor (its
// descriptor byte for byte; at 64x64 WindowServer drew the pointer itself), cropped onto the 64x64 plane. IOFramebuffer's first call
// is its test (convertCursorImage records the descriptor in IOFBCursorInfo and returns false). Otherwise IONDRV's own.
mach_vm_address_t orgSetCursorImage { 0 }, orgSetCursorState { 0 }, orgFbGetAttribute { 0 };
mach_vm_address_t orgFbSetAttribute { 0 }, orgFbTimingInfo { 0 }, orgFbGetConnAttribute { 0 };

// rdna4-vrrprobe=1 (docs/freesync-plan.md step 0): the boot display says it can refresh variably ('vrr?', and its modes' timing
// carries the vertical blanking extension down to the EDID's lowest rate), and what WindowServer then asks and sets is logged.
// Nothing in the display hardware changes.
bool gVrrProbe;
bool vrrProbeFb(void *fb) { return gVrrProbe && PvScanout::portOf(static_cast<IOService *>(fb)) == 0; }
void vrrLog(const char *what, IOSelect a, uintptr_t v) {
	static uint32_t lines;
	if (lines++ < 200)
		FBLOG("vrrprobe: %s '%c%c%c%c' (0x%08x) = 0x%lx", what, (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff, a,
		      static_cast<unsigned long>(v));
}
constexpr uint32_t kCursorAnnounced = 128;
uint32_t gCursorHot[8][2];   // each head's image's hot spot: IOFramebuffer moves the image's corner, cursorMove takes the hot spot
bool gAccelCursor;           // rdna4-accel=4, read where the routes are made

int cursorPort(void *fb)
{
	// (the boot-arg, not RDNA4Accel::framebuffersDecided: IOFramebuffer tests the boot display's cursor at its first open, ~7 s before
	// WindowServer is let go; WindowServer takes a hardware cursor only when every display has one)
	const int port = gAccelCursor ? PvScanout::portOf(static_cast<IOService *>(fb)) : -1;
	return port >= 0 && PvScanout::cursorReady(static_cast<uint32_t>(port)) ? port : -1;
}

IOReturn wrapSetCursorImage(void *fb, void *image)
{
	const int port = cursorPort(fb);
	if (port < 0)
		return FunctionCast(wrapSetCursorImage, orgSetCursorImage)(fb, image);

	IOHardwareCursorDescriptor desc {};
	desc.majorVersion = 1;   // (AppleParavirtFramebuffer's IOFBCursorInfo)
	desc.minorVersion = 1;
	desc.width = desc.height = kCursorAnnounced;
	desc.bitDepth = 32;
	desc.supportedSpecialEncodings = 4;
	desc.specialEncodings[1] = 0x00ffffff;
	auto *pixels = static_cast<uint32_t *>(IOMalloc(kCursorAnnounced * kCursorAnnounced * 4));
	IOHardwareCursorInfo info {};
	info.majorVersion = kHardwareCursorInfoMajorVersion;
	info.minorVersion = kHardwareCursorInfoMinorVersion;
	info.hardwareCursorData = reinterpret_cast<UInt8 *>(pixels);
	const bool ok = pixels && static_cast<IOFramebuffer *>(fb)->convertCursorImage(image, &desc, &info) &&
	                // (rows packed tightly, the image's width a row: as AppleParavirtFramebuffer's glyphs, "32 bytes a row" for 8x8; read at
	                // the descriptor's 128 they were a block of garbage on both displays, 2026-11-21)
	                PvScanout::cursorArgb(static_cast<uint32_t>(port), pixels, info.cursorWidth, info.cursorHeight, info.cursorWidth,
	                                      info.cursorHotSpotX, info.cursorHotSpotY);
	if (ok && port < 8) {
		gCursorHot[port][0] = info.cursorHotSpotX;
		gCursorHot[port][1] = info.cursorHotSpotY;
	}
	if (!ok)   // IOFramebuffer draws this one itself: the plane's image before must not stay beside it (two pointers)
		(void)PvScanout::cursorMove(static_cast<uint32_t>(port), 0, 0, false);
	static uint32_t logged;
	if (logged++ < 6)
		FBLOG("cursor: our accelerator's: port %d: image %ux%u, hot spot %u,%u: %s", port, info.cursorWidth, info.cursorHeight,
		      info.cursorHotSpotX, info.cursorHotSpotY, ok ? "on the head's plane" : "not converted (IOFramebuffer's test, or too large)");
	if (pixels)
		IOFree(pixels, kCursorAnnounced * kCursorAnnounced * 4);
	return ok ? kIOReturnSuccess : kIOReturnUnsupported;
}

// IOFramebuffer::open asks kIOHardwareCursorAttribute before it uses a hardware cursor at all (+0x19e); IONDRV asks the NDRV, which
// does not have one for each head.
IOReturn wrapFbGetAttribute(void *fb, IOSelect attribute, uintptr_t *value)
{
	if (attribute == kIOHardwareCursorAttribute && value && cursorPort(fb) >= 0) {
		*value = 1;
		return kIOReturnSuccess;
	}
	if (vrrProbeFb(fb) && attribute == kIOFBVariableRefreshRate && value) {
		*value = 1;
		vrrLog("getAttribute", attribute, 1);
		return kIOReturnSuccess;
	}
	const IOReturn r = FunctionCast(wrapFbGetAttribute, orgFbGetAttribute)(fb, attribute, value);
	if (vrrProbeFb(fb))
		vrrLog(r == kIOReturnSuccess ? "getAttribute" : "getAttribute (failed)", attribute, value ? *value : 0);
	return r;
}

IOReturn wrapFbSetAttribute(void *fb, IOSelect attribute, uintptr_t value)
{
	if (vrrProbeFb(fb))
		vrrLog("setAttribute", attribute, value);
	if (vrrProbeFb(fb) && attribute == kIOFBVariableRefreshRate)
		return kIOReturnSuccess;
	return FunctionCast(wrapFbSetAttribute, orgFbSetAttribute)(fb, attribute, value);
}

IOReturn wrapFbGetConnAttribute(void *fb, IOIndex connect, IOSelect attribute, uintptr_t *value)
{
	const IOReturn r = FunctionCast(wrapFbGetConnAttribute, orgFbGetConnAttribute)(fb, connect, attribute, value);
	if (vrrProbeFb(fb))
		vrrLog(r == kIOReturnSuccess ? "getAttributeForConnection" : "getAttributeForConnection (failed)", attribute,
		       value ? *value : 0);
	return r;
}

// The mode's timing with the vertical blanking that takes it down to the display's lowest rate (EDID range limits, 48 Hz when
// not known): Adaptive-Sync for IOGraphics (IODetailedTimingInformationV2::verticalBlankingExtension, 0 = none).
IOReturn wrapFbTimingInfo(void *fb, IODisplayModeID mode, IOTimingInformation *info)
{
	const IOReturn r = FunctionCast(wrapFbTimingInfo, orgFbTimingInfo)(fb, mode, info);
	if (r != kIOReturnSuccess || !info || !vrrProbeFb(fb) || !(info->flags & kIODetailedTimingValid))
		return r;
	IODetailedTimingInformationV2 &t = info->detailedInfo.v2;
	const uint64_t total = static_cast<uint64_t>(t.horizontalActive + t.horizontalBlanking) * (t.verticalActive + t.verticalBlanking);
	const uint32_t hz = total ? static_cast<uint32_t>(t.pixelClock / total) : 0, minHz = 48;
	if (hz > minHz) {
		t.verticalBlankingExtension = (t.verticalActive + t.verticalBlanking) * hz / minHz - (t.verticalActive + t.verticalBlanking);
		static uint32_t lines;
		if (lines++ < 40)
			FBLOG("vrrprobe: mode 0x%x %ux%u at %u Hz: vertical blanking extension %u lines (down to %u Hz)", mode,
			      t.horizontalActive, t.verticalActive, hz, t.verticalBlankingExtension, minHz);
	}
	return r;
}

IOReturn wrapSetCursorState(void *fb, SInt32 x, SInt32 y, bool visible)
{
	const int port = cursorPort(fb);
	if (port < 0)
		return FunctionCast(wrapSetCursorState, orgSetCursorState)(fb, x, y, visible);
	const uint32_t *hot = port < 8 ? gCursorHot[port] : gCursorHot[0];
	const uint64_t from = mach_absolute_time();
	const bool ok = PvScanout::cursorMove(static_cast<uint32_t>(port), x + static_cast<int32_t>(hot[0]), y + static_cast<int32_t>(hot[1]),
	                                      visible);
	// (diagnostic, the pointer lagged) how many moves and how long each took, the lock wait included: a line every 10 s with moves
	static uint64_t windowAt, moves, worst, total;
	uint64_t now = mach_absolute_time(), ns = 0;
	absolutetime_to_nanoseconds(now - from, &ns);
	moves++;
	total += ns;
	worst = ns > worst ? ns : worst;
	uint64_t sinceNs = 0;
	absolutetime_to_nanoseconds(now - windowAt, &sinceNs);
	if (sinceNs >= 10000000000ull) {
		if (windowAt)
			FBLOG("cursor: our accelerator's: %llu moves in %llu s, %llu us on average, the longest %llu us", moves, sinceNs / 1000000000ull,
			      total / moves / 1000, worst / 1000);
		windowAt = now;
		moves = total = worst = 0;
	}
	return ok ? kIOReturnSuccess : kIOReturnUnsupported;
}
#endif

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
	if (isStatus ? h->ndrv.status(code, pb->params, r) : h->ndrv.control(code, pb->params, r)) {
		if (isStatus && code == Ndrv::cscGetTimingRanges) {
			auto *g = static_cast<const Ndrv::VDDisplayTimingRangeRec *>(pb->params);
			HLOG(h, "ndrv: timing range %llu-%llu Hz, %u modes -> 0x%x", g->csMinPixelClock, g->csMaxPixelClock,
			     static_cast<unsigned>(h->ndrv.modeCount()), r);
		}
		if (!isStatus && code == Ndrv::cscSetDetailedTiming) {
			auto *t = static_cast<const Ndrv::VDDetailedTimingRec *>(pb->params);
			if (t->csReserved2)   // a variable-refresh timing
				HLOG(h, "ndrv: variable timing 0x%x %ux%u, %u lines of extension -> 0x%x", t->csDisplayModeAlias,
				     t->csHorizontalActive, t->csVerticalActive, t->csReserved2, r);
			if (t->csBitsPerColorComponent || t->csColorimetry || t->csDynamicRange)
				HLOG(h, "ndrv: timing 0x%x colour: encoding 0x%x, bpc 0x%x, colorimetry 0x%x, range 0x%x",
				     t->csDisplayModeAlias, t->csPixelEncoding, t->csBitsPerColorComponent, t->csColorimetry,
				     t->csDynamicRange);
		}
		return static_cast<IOReturn>(r);
	}
	return static_cast<IOReturn>(Ndrv::bootReply(isStatus, code));
}

// IONDRVFramebuffer::getVRAMRange returns IOBootNDRV's console range, so
// nothing for head 2: hand out its surface instead (retained, as the original
// does).
IODeviceMemory *wrapGetVRAMRange(void *fb) {
	for (const auto &entry : fbTable)
		if (entry.fb == fb && entry.state && entry.state->dev.dpVram) {
			entry.state->dev.dpVram->retain();
			return entry.state->dev.dpVram;
		}
	Head2 *h = headFor(fb);
	if (h && h->vram) {
		h->vram->retain();
		return h->vram;
	}
	return FunctionCast(wrapGetVRAMRange, orgGetVRAMRange)(fb);
}

// In a scaled mode the framebuffer's memory is the host buffer (ScaledMemory): the original would cut rowBytes x height out of BAR0, over
// the other heads and the compute pool. Retained, as the original's is; a descriptor passed as IODeviceMemory, as the original does.
IODeviceMemory *wrapGetApertureRange(void *fb, IOPixelAperture aperture) {
	if (aperture == kIOFBSystemAperture) {
		for (const auto &entry : fbTable)
			if (entry.fb == fb && entry.state && entry.state->dev.activeDpMode()) {
				entry.state->dev.dpAperture->retain();
				return entry.state->dev.dpAperture;
			}
		uint32_t width, height;
		ScaledMemory *memory = nullptr;
		if (Head2 *h = headFor(fb)) {
			if (h->ndrv.scaledFrame(width, height))
				memory = &h->scaledMemory;
		} else if (bootState && bootState->ndrv.scaledFrame(width, height)) {
			for (const auto &e : fbTable)
				if (e.fb == fb && e.state == bootState)
					memory = &gBootScaledMemory;
		}
		if (memory && memory->buffer) {
			memory->buffer->retain();
			return reinterpret_cast<IODeviceMemory *>(memory->buffer);
		}
	}
	return FunctionCast(wrapGetApertureRange, orgGetApertureRange)(fb, aperture);
}

// Resolve at IONDRVSupport load. Extra heads need the VRAM route and PVGPU needs the
// aperture route for scaled frames. A plain build can defer the other routes until a
// larger surface has actually been mapped, before its modes are published to NDRV.
KernelPatcher *gPatcher = nullptr;
struct RangeRoute {
	mach_vm_address_t target {};
	bool installed {};
} vramRange, apertureRange;
IOLock *rangeRouteLock {};
bool installRangeRoute(KernelPatcher::RouteRequest request, RangeRoute &route, const char *name) {
	if (route.installed)
		return true;
	request.from = route.target;
	route.installed = route.target && gPatcher->routeMultiple(kextIONDRVSupport.loadIndex, &request, 1);
	FBLOG("modes: IONDRVFramebuffer::%s %s", name, route.installed ? "routed" : "route unavailable");
	gPatcher->clearError();
	return route.installed;
}

bool installVramRange() {
	return installRangeRoute({nullptr, wrapGetVRAMRange, orgGetVRAMRange}, vramRange, "getVRAMRange");
}

bool installApertureRange() {
	return installRangeRoute({nullptr, wrapGetApertureRange, orgGetApertureRange}, apertureRange, "getApertureRange");
}

bool ensureDpSurfaceRanges() {
	// Only deferred routing needs a lock; both existing routes may already be in.
	if (!rangeRouteLock)
		return vramRange.installed && apertureRange.installed;
	IOLockLock(rangeRouteLock);
	const bool ready = installVramRange() && installApertureRange();
	IOLockUnlock(rangeRouteLock);
	return ready;
}

bool wrapStart(void *fb, IOService *provider) {
	auto *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!headCount && pci && isOurDevice(pci))
		createHeads(pci);
	return FunctionCast(wrapStart, orgStart)(fb, provider);
}

// Step 0 of docs/audio-plan.md: hand the card's HDMI/DP audio function (function 1 beside the GPU) to Apple's
// AppleGFXHDAEGController. The AMDRadeonRDNA4HDA personality in Info.plist matches it once it is marked rdna4-hda, and
// the controller's probe takes only the AMD functions in its table (AB28, Navi 21's, among them, not Navi 48's AB40), so
// IOPCIDevice::extendedConfigRead16 (what it reads them with) is routed to read AB28 as its device ID, and the codec's
// function group is chosen by the device-id property, which is set to AB28 too. At run time only
// (`sudo sysctl debug.rdna4_hda=1`), so a boot never repeats what it did.
IOPCIDevice *gHdaGpu, *gHdaDevice;
RDNA4Device *gHdaDev;   // the boot display's device: its audio goes on first (step 1)
mach_vm_address_t orgExtConfigRead16 { 0 };
constexpr uint16_t kHdaSpoofId = 0xab28;

UInt16 wrapExtConfigRead16(IOPCIDevice *that, IOByteCount offset) {
	const UInt16 value = FunctionCast(wrapExtConfigRead16, orgExtConfigRead16)(that, offset);
	return that == gHdaDevice && offset == kIOPCIConfigDeviceID ? kHdaSpoofId : value;
}

// AppleGFXHDA ties the audio function to a framebuffer by properties (its AppleDisplay match handler): the same
// hda-gfx on both, and the framebuffer's connector-type (kConnectorDisplayPort 0x400, HDMI 0x800). Both rig monitors
// are on DisplayPort. ponytail: every framebuffer under the GPU is called DisplayPort; take it from the head's
// connector when an HDMI display is to have audio.
bool tieFramebufferToHda(IORegistryEntry *e) {
	// audio-codec-info: byte 0 the codec's address, byte 1 non-zero, byte 2 the pin's node ID: the pin of the display's
	// stream encoder's Azalia endpoint (enableAudio), AMD's codec's pins being 0x03, 0x05, 0x07, 0x09 for endpoints 0-3.
	// A framebuffer without a lit DisplayPort display gets none (no output in Sound settings for it).
	uint8_t dig = 0xff;
	if (!strcmp(e->getName(), ".Display_boot")) {
		dig = gHdaDev ? gHdaDev->bootDig() : 0xff;
	} else if (Head2 *h = headFor(e)) {
		if (h->dev && h->connected) {
			RDNA4Device::ExtraHold hold(*h->dev);
			h->dev->selectExtra(h->slot);
			dig = h->dev->extraDig();
		}
	}
	if (dig >= 4)
		return false;
	uint8_t dp[4] = { 0x00, 0x04, 0x00, 0x00 };
	uint8_t dpSignal[4] = { 0x10, 0x00, 0x00, 0x00 };   // kIOFBAVSignalTypeDP (av-signal-type, read on each framebuffer event)
	uint8_t codecInfo[4] = { 0x00, 0x01, static_cast<uint8_t>(0x03 + 2 * dig), 0x00 };
	uint8_t pipeNo[4] = { static_cast<uint8_t>(dig + 1), 0, 0, 0 };
	e->setProperty("hda-gfx", "onboard-1");
	e->setProperty("connector-type", dp, sizeof(dp));
	e->setProperty("av-signal-type", dpSignal, sizeof(dpSignal));
	e->setProperty("audio-codec-info", codecInfo, sizeof(codecInfo));
	e->setProperty("AAPL,DisplayPipe", pipeNo, sizeof(pipeNo));
	return true;
}

void tieFramebuffersToHda(IOPCIDevice *gpu) {
	IORegistryIterator *it = IORegistryIterator::iterateOver(gpu, gIOServicePlane, kIORegistryIterateRecursively);
	if (!it)
		return;
	unsigned n = 0;
	while (IORegistryEntry *e = it->getNextObject())
		if (e->metaCast("IOFramebuffer") && tieFramebufferToHda(e))   // (by name: the kext does not link IOGraphics)
			n++;
	it->release();
	FBLOG("audio: %u framebuffers tied to the audio function", n);
}

// AppleGFXHDA asks for AppleDisplays after ours exist and drops the ones IOKit hands it before its notifier is stored
// (docs/audio-plan.md step 2): register them again once it has asked.
void registerDisplaysAgain(IOPCIDevice *gpu) {
	IORegistryIterator *it = gpu ? IORegistryIterator::iterateOver(gpu, gIOServicePlane, kIORegistryIterateRecursively) : nullptr;
	unsigned n = 0;
	while (IORegistryEntry *e = it ? it->getNextObject() : nullptr)
		if (e->metaCast("AppleDisplay")) {
			static_cast<IOService *>(e)->registerService();
			n++;
		}
	OSSafeReleaseNULL(it);
	FBLOG("audio: %u AppleDisplays registered again", n);
}

void offerHdaToApple(IOPCIDevice *gpu) {
	IORegistryEntry *bus = gpu ? gpu->getParentEntry(gIOServicePlane) : nullptr;
	OSIterator *it = bus ? bus->getChildIterator(gIOServicePlane) : nullptr;
	if (!it)
		return;
	while (OSObject *o = it->getNextObject()) {
		auto *d = OSDynamicCast(IOPCIDevice, o);
		if (!d || d == gpu || d->getFunctionNumber() != 1 || d->configRead16(kIOPCIConfigVendorID) != 0x1002)
			continue;
		const uint16_t id = d->configRead16(kIOPCIConfigDeviceID);
		if (!orgExtConfigRead16 && gPatcher) {
			KernelPatcher::RouteRequest read {"__ZN11IOPCIDevice20extendedConfigRead16Ey", wrapExtConfigRead16, orgExtConfigRead16};
			if (!gPatcher->routeMultiple(kextIOPCIFamily.loadIndex, &read, 1))
				orgExtConfigRead16 = 0;
			gPatcher->clearError();
		}
		if (!orgExtConfigRead16) {
			FBLOG("audio: IOPCIDevice::extendedConfigRead16 not routed: HDA function %04x left alone", id);
			break;
		}
		gHdaDevice = d;
		uint8_t spoofed[4] = { kHdaSpoofId & 0xff, kHdaSpoofId >> 8, 0, 0 };
		d->setProperty("device-id", spoofed, sizeof(spoofed));
		d->setProperty("hda-gfx", "onboard-1");
		tieFramebuffersToHda(gpu);
		d->setProperty("rdna4-hda", kOSBooleanTrue);
		d->registerService();
		FBLOG("audio: HDA function %04x at function 1 offered to AppleGFXHDAEGController as %04x", id, kHdaSpoofId);
		break;
	}
	it->release();
}

// The routed AppleGFXHDA functions: their arguments as pointers (they take pointers or integers, at most four), their
// result as it comes back (a bool's in the low byte).
using GfxHdaFn = uint64_t (*)(void *, void *, void *, void *);
enum { kGfxMatchedEG, kGfxLocateEG, kGfxEngineDP, kGfxActivate, kGfxFns };
mach_vm_address_t orgGfxHda[kGfxFns] {};

const char *entryName(void *e) {
	auto *r = static_cast<IORegistryEntry *>(e);
	return r ? r->getName() : "(none)";
}

uint64_t wrapGfxMatchedEG(void *self, void *ref, void *service, void *notifier) {
	// the display's framebuffer gets the properties the handler reads (one made after debug.rdna4_hda or boot ran, too)
	for (IORegistryEntry *e = static_cast<IORegistryEntry *>(service); e; e = e->getParentEntry(gIOServicePlane))
		if (e->metaCast("IOFramebuffer")) {
			tieFramebufferToHda(e);
			break;
		}
	const uint64_t r = FunctionCast(wrapGfxMatchedEG, orgGfxHda[kGfxMatchedEG])(self, ref, service, notifier);
	FBLOG("audio: AppleGFXHDA EG gfxMatchedHandler(%s) -> 0x%llx", entryName(service), r);
	return r;
}
uint64_t wrapGfxLocateEG(void *self, void *a, void *b, void *c) {
	const uint64_t r = FunctionCast(wrapGfxLocateEG, orgGfxHda[kGfxLocateEG])(self, a, b, c);
	FBLOG("audio: AppleGFXHDA EG locateAssociatedGraphicsController -> 0x%llx", r);
	if (!static_cast<uint32_t>(r))
		registerDisplaysAgain(gHdaGpu);
	return r;
}
uint64_t wrapGfxEngineDP(void *self, void *paths, void *index, void *head) {
	const uint64_t r = FunctionCast(wrapGfxEngineDP, orgGfxHda[kGfxEngineDP])(self, paths, index, head);
	FBLOG("audio: AppleGFXHDA createAudioOutputEngineDP(%llu) -> 0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(index)), r);
	return r;
}
uint64_t wrapGfxActivate(void *self, void *a, void *b, void *c) {
	const uint64_t r = FunctionCast(wrapGfxActivate, orgGfxHda[kGfxActivate])(self, a, b, c);
	FBLOG("audio: AppleGFXHDA activateAudioEngines -> 0x%llx", r);
	return r;
}

void processGfxHda(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (index != kextAppleGFXHDA.loadIndex)
		return;
	KernelPatcher::RouteRequest routes[] {
		{"__ZN23AppleGFXHDAEGController17gfxMatchedHandlerEPvP9IOServiceP10IONotifier", wrapGfxMatchedEG, orgGfxHda[kGfxMatchedEG]},
		{"__ZN23AppleGFXHDAEGController34locateAssociatedGraphicsControllerEv", wrapGfxLocateEG, orgGfxHda[kGfxLocateEG]},
		{"__ZN17AppleGFXHDADriver25createAudioOutputEngineDPEP7OSArrayiP27HDAGFXHeadAssociationStruct", wrapGfxEngineDP, orgGfxHda[kGfxEngineDP]},
		{"__ZN17AppleGFXHDADriver20activateAudioEnginesEv", wrapGfxActivate, orgGfxHda[kGfxActivate]},
	};
	const bool ok = patcher.routeMultiple(index, routes, kGfxFns, address, size);
	FBLOG("audio: AppleGFXHDA loaded: its display matching %s", ok ? "routed" : "NOT routed");
	patcher.clearError();
}

// The boot display's audio set up and the audio function handed to AppleGFXHDA (rdna4-audio=1, or debug.rdna4_hda=1).
void startAudio() {
	gAudioOn = true;   // (a display that cannot take it now gets it at its next switch or power-on)
	if (gHdaDev)
		(void)gHdaDev->enableBootAudio();
	for (size_t i = 0; i < headCount; i++)
		if (Head2 *h = heads[i]; h->dev && h->connected) {
			RDNA4Device::ExtraHold hold(*h->dev);
			h->dev->selectExtra(h->slot);
			(void)h->dev->enableExtraAudio();
		}
	offerHdaToApple(gHdaGpu);
}

int hdaSysctl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	int value = 0;
	const int err = sysctl_handle_int(oidp, &value, 0, req);
	if (err || !req->newptr)
		return err;
	if ((value != 1 && value != 2) || !gHdaGpu)
		return EINVAL;
	if (value == 2) {   // the displays matched again by hand (done by itself once AppleGFXHDA has asked for them)
		registerDisplaysAgain(gHdaGpu);
		return 0;
	}
	startAudio();
	return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, rdna4_hda, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, hdaSysctl, "I",
	    "AMDRadeonRDNA4: write 1 to hand the card's HDMI/DP audio function to AppleGFXHDA (docs/audio-plan.md)");

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
	if (!gHdaGpu) {
		gHdaGpu = pci;
		gHdaDev = &dev;
		sysctl_register_oid(&sysctl__debug_rdna4_hda);
		uint32_t audio = 0;   // rdna4-audio=1 (docs/audio-plan.md): the boot display's audio from boot on
		if (Trial::bootArg("rdna4-audio", audio) && audio)
			startAudio();
	}
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
	// The boot display can advertise its fixed as well as variable timings past the first EDID extension.
	const bool vrr = vrrWanted();
	if (vrr && be.switchTo == deviceSwitchTo)
		be.setRefresh = deviceSetRefresh;
	if (be.switchTo == deviceSwitchTo)
		be.setHdr = deviceSetHdr;
	uint32_t guest = 0, bench = 0, cold = 0, dsc = 0;
	if (!pvGpu && !(Trial::option("rdna4-pvgpu", guest) && guest) &&
	    !(Trial::option("rdna4-vrambench", bench) && bench) && !(Trial::option("rdna4-coldboot", cold) && cold) &&
	    !(Trial::option("rdna4-dscboot", dsc) && dsc))
		dev.prepareDpSurface(RDNA4Device::extraHeads() * kHeadSlotBytes + (2ull << 20),
		                     computeStage ? RDNA4Compute::kPoolFloor : ~0ull, computeStage >= RDNA4Compute::StageGfx,
		                     ensureDpSurfaceRanges);
	else
		FBLOG("modes: no larger DP surface: pvgpu %u, vrambench %u, coldboot %u, dscboot %u",
		      pvGpu ? pvGpu : guest, bench, cold, dsc);
	st->ndrv.init(dev.modeTable, dev.modeCount, dev.defaultModeId, dev.edidAll, dev.edidAllLen, be);
	st->ndrv.adaptiveSync = vrr;
	st->ndrv.hdr = hdrWanted(dev.edidAll, dev.edidAllLen);
#ifdef AMDRadeonRDNA4_PVGPU
	// A test boot's level (rdna4-trial) is only known once NVRAM is; the heads' framebuffers are told before macOS asks.
	uint32_t pvTrial = 0, pvDisp = 0;

	if (!pvGpu && Trial::option("rdna4-pvgpu", pvTrial) && pvTrial <= 2)
		pvGpu = pvTrial;

	pvHeads = pvGpu && dev.isAmd && Trial::option("rdna4-pvgpu-disp", pvDisp) && (pvDisp & 4);

	uint32_t scaled = 1;

	(void)Trial::option("rdna4-pvscaled", scaled);
	pvScaledModes = scaled != 0;

	uint32_t fast = 0;

	(void)Trial::option("rdna4-pvfast", fast);
	pvFastModes = fast != 0;

	if (pvHeads) {
		st->ndrv.setConnected(false);
		FBLOG("ndrv: the heads are the paravirtual displays: no display on our framebuffers for macOS");
	}

	// Before NVRAM is up a test boot looks like a normal one: decided only once the variable could be read (or boot-args set it),
	// or the first WindowServer opened the boot framebuffer as macOS's display and died in its first frame (pvhooks.cpp).
	if (dev.isAmd && (Trial::loaded() || pvGpu))
		PvScanout::decided(pvHeads);

	if (pvHeads)
		PvHooks::hideFramebuffers(e.fb);
#endif
	e.state = st;
	bootState = st;
	FBLOG("ndrv: answering for %p: %lu mode(s), cached EDID %lu bytes", e.fb,
	      static_cast<unsigned long>(st->ndrv.modeCount()), static_cast<unsigned long>(dev.edidAllLen));
	fillHeads(dev);
	// Behind the extra heads' slots, below the compute pool (128 MiB).
	VramBench::run(dev.fbPhysBase, ((dev.displaySpan() + (1ull << 20) - 1) & ~((1ull << 20) - 1)) +
	                               RDNA4Device::kMaxExtra * kHeadSlotBytes + (2ull << 20));

	// E1 of docs/metal-spike.md: an IOAccelerator-shaped service that only logs what Metal asks of it. Independent of the compute bring-up.
	if (accelCensus && dev.isAmd)
		RDNA4AccelCensus::publish(pci, accelCensus);
	if (dev.isAmd)   // the native-Metal spike: published later by debug.rdna4_nativemtl=1, never at boot
		RDNA4AccelCensus::setProvider(pci);
	// M0: the fake paravirtual GPU, independent of everything else here, after the heads it may drive.
#ifdef AMDRadeonRDNA4_PVGPU
	if (pvGpu && dev.isAmd && !pvDelay)
		RDNA4PvNub::publish(pci, pvGpu);
	if (pvGpu && dev.isAmd && pvDelay)
		RDNA4PvNub::setLateParent(pci);
	if (dev.isAmd)   // (the host's; without rdna4-pvgpu our accelerator's display host, pvPresent)
		RDNA4PvNub::setCompute(&st->compute);
#endif

	// Compute bring-up runs after the display is answered for, and only
	// when asked for: it must never be the reason the desktop is missing.
	if (computeStage && dev.isAmd) {
		RDNA4Compute::Env env { pci, svc, dev.mmioBase(), dev.mmioSize(), dev.discovery(),
		                        dev.fbPhysBase, dev.displayReserve ? dev.displayReserve : dev.fbLength, dev.liveFramePeriodNs(),
		                        createVblankService, deviceCursorProbe, &dev };
		env.smuReady = deviceSmuReady;
		dev.compute = &st->compute;   // its sleep and wake from the device's notifiers (rdna4-syssleep without rdna4-pm)
		env.vbios = dev.vbiosBytes(env.vbiosSize);
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

bool sysSleepLog = false;   // rdna4-syssleep: the framebuffers' power calls logged

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

	if (csc && isStatus && code == Ndrv::cscGetTimingRanges) {
		auto *g = static_cast<const Ndrv::VDDisplayTimingRangeRec *>(static_cast<NdrvControlParameters *>(contents)->params);
		FBLOG("ndrv: timing range %llu-%llu Hz, %u modes -> 0x%x (%s)", g->csMinPixelClock, g->csMaxPixelClock,
		      e->state ? static_cast<unsigned>(e->state->ndrv.modeCount()) : 0u, ret, answered ? "rdna4" : "boot");
	}
	if (csc && !isStatus && code == Ndrv::cscSetDetailedTiming) {
		auto *t = static_cast<const Ndrv::VDDetailedTimingRec *>(static_cast<NdrvControlParameters *>(contents)->params);
		if (t->csReserved2)   // a variable-refresh timing
			FBLOG("ndrv: variable timing 0x%x %ux%u, %u lines of extension -> 0x%x (%s)", t->csDisplayModeAlias,
			      t->csHorizontalActive, t->csVerticalActive, t->csReserved2, ret, answered ? "rdna4" : "boot");
		if (t->csBitsPerColorComponent || t->csColorimetry || t->csDynamicRange)
			FBLOG("ndrv: timing 0x%x colour: encoding 0x%x, bpc 0x%x, colorimetry 0x%x, range 0x%x",
			      t->csDisplayModeAlias, t->csPixelEncoding, t->csBitsPerColorComponent, t->csColorimetry,
			      t->csDynamicRange);
	}
	// Mode switches are rare and the interesting part: always logged.
	if (csc && !isStatus && code == Ndrv::cscSwitchMode) {
		auto *pb = static_cast<NdrvControlParameters *>(contents);
		auto *sw = static_cast<const Ndrv::VDSwitchInfoRec *>(pb->params);
		FBLOG("ndrv: switch to mode %d -> 0x%x (%s)", sw ? sw->csData : -1, ret,
		      answered ? "rdna4" : "boot");
	}

	// The power calls (cscGetPowerState / cscSetPowerState, 25) and the NDRV's lifecycle, always: what system sleep
	// asks of the framebuffer (rdna4-syssleep, roadmap item 5).
	const bool lifecycle = !csc && commandCode != kIONDRVControlCommand && commandCode != kIONDRVStatusCommand;
	if (sysSleepLog && ((csc && code == 25) || lifecycle)) {
		const auto *ps = csc ? static_cast<const VDPowerStateRec *>(static_cast<NdrvControlParameters *>(contents)->params) : nullptr;
		FBLOG("power: %p %s%s -> 0x%x (%s)%s state %u flags 0x%x", fb, commandName(commandCode), csc ? " csc 25 (power state)" : "",
		      ret, answered ? "rdna4" : "boot", ps ? "," : "", ps ? ps->powerState : 0, ps ? ps->powerFlags : 0);
	} else if (traceEnabled && traceBudget) {
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

// rdna4-syssleep=1 (roadmap item 5, stage 1): system sleep no longer refused for the card. IONDRVFramebuffer::initForPM
// sets kIOPMPreventSystemSleep in its power states whenever its NDRV is IOBootNDRV (as ours always is), or the NDRV's
// cscGetPowerState fails or lacks kPowerStateSleepCanPowerOff (IONDRVSupport, Tahoe 26.6.2, disassembled 2026-11-15);
// a service whose power state carries that flag is one of IOPMrootDomain's "system sleep preventers". For our
// framebuffers the flag is taken out of the table they register; what sleep and wake then do is what stage 1 looks at.
constexpr IOPMPowerFlags kPreventSystemSleep = 0x0010;   // kIOPMPreventSystemSleep (IOPM.h, private flags)
mach_vm_address_t orgRegisterPowerDriver;

IOReturn wrapRegisterPowerDriver(IOService *that, IOService *driver, IOPMPowerState *states, unsigned long n) {
	auto org = FunctionCast(wrapRegisterPowerDriver, orgRegisterPowerDriver);
	if (!that || !states || !n || n > 8 || !that->metaCast("IONDRVFramebuffer") ||
	    !(isOurDevice(that->getProvider()) || headFor(that)))
		return org(that, driver, states, n);
	IOPMPowerState copy[8];   // registerPowerDriver copies the table itself (an OSData of it)
	uint32_t cleared = 0;
	for (unsigned long i = 0; i < n; i++) {
		copy[i] = states[i];
		if (copy[i].capabilityFlags & kPreventSystemSleep) {
			copy[i].capabilityFlags &= ~kPreventSystemSleep;
			cleared |= 1u << i;
		}
	}
	FBLOG("power: %s %p: %lu power state(s), kIOPMPreventSystemSleep taken out of 0x%x: system sleep allowed",
	      that->getName(), that, n, cleared);
	return org(that, driver, copy, n);
}

// Our framebuffers' slots in their IOFBController. On boards whose BIOS names the card's PCI device VGA rather than display,
// IONDRVSupport's expander runs (the IONDRVFramebuffer on the PCI device that is not name-matched): it numbers every
// device-tree child of type display 0, 1, ... (our head nubs, over createHead's index) and wraps each in an IONDRVDevice of its
// own, so six framebuffers share IOFBDependentIndex 0..2, and some are never opened. IOFramebuffer::open puts a framebuffer
// into its controller's table at its index (+0x18, a pointer per slot), and IOFBController::checkPowerWork walks that table
// as a list that ends at the first empty slot (IOGraphicsFamily, Tahoe 26.6.2, disassembled 2026-11-15). A framebuffer that
// lost its slot, or stands behind an empty one, never has a power change done: each waited out its 45 s acknowledgement
// timeout, at boot, at every sleep and every wake. So each of our framebuffers opens into the first free slot: the table
// holds the open framebuffers without a gap. (The never opened ones acknowledge power changes at once.)
mach_vm_address_t orgFbOpen;

// A framebuffer's IOFBController (IOFramebuffer::__private at +0x1c8, the controller its first field), or null.
uint8_t *fbController(IOService *fb) {
	auto *priv = fb ? *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(fb) + 0x1c8) : nullptr;
	return priv ? *reinterpret_cast<uint8_t **>(priv) : nullptr;
}

// The controller's 'wsup' bit of framebuffer `fb` cleared (see wrapFbOpen): for one WindowServer never opens. Returns the controller's
// bits before, or -1 without a controller or slot.
int clearServerUpBit(IOService *fb) {
	uint8_t *ctl = fb && fb->metaCast("IONDRVFramebuffer") ? fbController(fb) : nullptr;
	if (!ctl)
		return -1;
	auto *const *table = reinterpret_cast<IOService *const *>(ctl + 0x18);
	for (uint32_t slot = 0; slot < 8; slot++)
		if (table[slot] == fb) {
			const uint8_t was = ctl[0x1b4];
			__atomic_fetch_and(ctl + 0x1b4, static_cast<uint8_t>(~(1u << slot)), __ATOMIC_SEQ_CST);
			return was;
		}
	return -1;
}

// The paravirtual GPU's framebuffer (Apple's driver, on our nub).
bool isParavirtFb(IOService *fb) {
	const OSMetaClass *meta = fb ? fb->getMetaClass() : nullptr;
	return meta && !strcmp(meta->getClassName(), "AppleParavirtFramebuffer");
}

// The controller of one of our card's IONDRVFramebuffers, or null for any other framebuffer.
uint8_t *ourFbController(IOService *fb) {
	return fb && fb->metaCast("IONDRVFramebuffer") && isOurDevice(fb->getProvider()) ? fbController(fb) : nullptr;
}

IOReturn wrapFbOpen(IOService *fb) {
	if (uint8_t *ctl = ourFbController(fb)) {
		auto *index = OSDynamicCast(OSNumber, fb->getProperty("IOFBDependentIndex"));
		if (index) {
			auto *const *table = reinterpret_cast<IOService *const *>(ctl + 0x18);
			uint32_t slot = 0;
			while (slot < 32 && table[slot] && table[slot] != fb)
				slot++;
			if (slot < 32 && slot != index->unsigned32BitValue()) {
				FBLOG("ndrv: %s %llx opens as dependent %u, the first free slot (was %u)", fb->getName(),
				      fb->getRegistryEntryID(), slot, index->unsigned32BitValue());
				fb->setProperty("IOFBDependentIndex", static_cast<unsigned long long>(slot), 32);
			}
		}
	}
	const IOReturn r = FunctionCast(wrapFbOpen, orgFbOpen)(fb);
	// A framebuffer whose open failed (the expander's wrappers of our head nubs: IOBootNDRV refuses a nub index other than
	// 0, and we do not answer for them) is dead, but open had already put it into its controller's table. It is never in
	// gAllFramebuffers, so never told a server state: it stays "on" (sent/acked 1/1), the controller's state keeps bit
	// 0x40, and IOFramebuffer::systemWork then (a) holds the system sleep's acknowledgement until gIOGNotifyTO (90 s) and
	// (b) at the wake skips telling WindowServer the displays are on: no desktop (card, 2026-11-15, trace of 10000056f).
	// So it leaves the table again, and the next framebuffer to open takes its slot.
	uint8_t *ctl = ourFbController(fb);
	if (r != kIOReturnSuccess && ctl) {
		auto **table = reinterpret_cast<IOService **>(ctl + 0x18);
		for (uint32_t slot = 0; slot < 32; slot++)
			if (table[slot] == fb) {
				table[slot] = nullptr;
				if (slot < 8)
					ctl[0x1b4] &= static_cast<uint8_t>(~(1u << slot));
				// ponytail: a gap left before live slots would end the controller's walks early; open fills slots in
				// order, so the failed one is the last taken unless a dependent opened inside it (logged)
				const bool gap = slot + 1 < 32 && table[slot + 1];
				FBLOG("ndrv: %s %llx did not open (0x%x): out of its controller's slot %u again%s", fb->getName(),
				      fb->getRegistryEntryID(), r, slot, gap ? " -- LEAVES A GAP before live slots" : "");
			}
	}
	// The controller's byte at +0x1b4 has a bit per framebuffer opened but not yet told by WindowServer that it is up
	// (extGetAttribute 'wsup', through the framebuffer's user client; nothing else clears it). While a bit is set, the
	// controller's state has bit 0x8 and IOFramebuffer::systemWork holds back the servers' notification of a system sleep
	// until gIOGNotifyTO (90 s) runs out. With the heads as the paravirtual displays our framebuffers are hidden from
	// WindowServer, which never comes for them: their bits are cleared here (pvHeads is decided while the boot one opens).
	if (pvHeads && ctl && ctl[0x1b4]) {
		FBLOG("ndrv: %s %llx: controller's framebuffers 0x%02x marked up for WindowServer (it never opens them)",
		      fb->getName(), fb->getRegistryEntryID(), ctl[0x1b4]);
		ctl[0x1b4] = 0;
	}
	// On the native desktop (rdna4-accel=4) the bare ones are hidden from WindowServer the same way (RDNA4Accel::isBareFramebuffer):
	// one opened after that was decided is marked up here, the ones before when it is (ndrvServerNeverOpens).
	if (ctl && r == kIOReturnSuccess && RDNA4Accel::framebuffersDecided() && RDNA4Accel::isBareFramebuffer(fb))
		ndrvServerNeverOpens(fb);
	return r;
}

// IOFramebuffer::notifyServer(state) tells a framebuffer's server (WindowServer) of a system power change. One without a
// server port, as ours always are with the heads as the paravirtual displays, has its server state set at once, but its
// controller is not marked: IOFBController::computeState keeps returning its cached state (recomputed only when +0x1b6 or
// +0x19c is set), with bit 0x40 (a server still on) from before the sleep. IOFramebuffer::systemWork then holds the system
// sleep's acknowledgement until gIOGNotifyTO, 90 s (card, 2026-11-15: our controller's state stayed 0x44 for the whole wait
// while the paravirtual one, marked by WindowServer's acknowledgement, changed within a second). So the controller is
// marked as didWork(1) marks it (didWork itself wants the controller's gate, which notifyServer has let go of by now) and
// systemWork is run again, to recompute.
mach_vm_address_t orgFbNotifyServer, fbStartThread;

void wrapFbNotifyServer(IOService *fb, uint8_t state) {
	FunctionCast(wrapFbNotifyServer, orgFbNotifyServer)(fb, state);
	uint8_t *ctl = ourFbController(fb);
	if (!ctl || !fbStartThread)
		return;
	auto *msg = *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(fb) + 0x1a0);   // the server message; +0x8 its port
	if (!(msg && *reinterpret_cast<void **>(msg + 0x8))) {
		__atomic_fetch_or(ctl + 0x1b6, static_cast<uint8_t>(1), __ATOMIC_SEQ_CST);
		reinterpret_cast<void (*)(bool)>(fbStartThread)(false);
	}
}

// IOFramebuffer::_extEntry, the gate every user-client call of a framebuffer passes, waits (sleepGate) while the framebuffer
// is not on (this+0x198 bit 13, cleared for every framebuffer at a system sleep and set again by handleEvent(4), the wake a
// framebuffer's driver delivers on its power-on). In a dark wake WindowServer's main thread asks the paravirtual framebuffer for
// its mode (extGetCurrentDisplayMode) and waits there; at the full wake it cannot acknowledge the wake's notification, and the
// watchdog kills WindowServer and then panics (card, 2026-11-15, its spin report and Kernel-...-194833.panic). The paravirtual
// framebuffer has no hardware a sleep has to keep calls away from (our host answers at any time), so it is let through: marked
// on before the check (the next sleep clears the bit again).
// Then the wake is delivered as IONDRVSupport delivers it for its own framebuffers on their power-on: handleEvent(4, 1)
// (kIOFBNotifyDidWake: restoreFramebuffer, the framebuffer marked on, its waiters woken, notification 4 to its clients).
// The paravirtual framebuffer has no power states, so nothing else delivers it: its display pipe, powered off at the sleep
// (IOAccelDisplayPipe::framebuffer_will_power_off), stayed off, and every frame WindowServer submitted was refused
// ("Fallback GPU Display Pipe transaction submission failed"; card, 2026-11-15). handleEvent wants the controller's gate,
// which _extEntry has taken when it returns (it is let go in _extExit).
mach_vm_address_t orgExtEntry, fbHandleEvent;

IOReturn wrapExtEntry(IOService *fb, bool system, bool allowOffline, uint32_t apiBit, const char *where) {
	bool woken = false;
	if (isParavirtFb(fb)) {
		auto *flags = reinterpret_cast<uint8_t *>(fb) + 0x199;   // the high byte of this+0x198
		if (!(*flags & 0x20)) {
			__atomic_fetch_or(flags, static_cast<uint8_t>(0x20), __ATOMIC_SEQ_CST);
			woken = true;
		}
	}
	const IOReturn r = FunctionCast(wrapExtEntry, orgExtEntry)(fb, system, allowOffline, apiBit, where);
	if (woken) {
		IOReturn w = kIOReturnUnsupported;
		// handleEvent acts only on the thread its controller has at +0x188 (set by checkPowerWork around a framebuffer's
		// power work, where IONDRVSupport's calls come from): set here the same way, and put back.
		uint8_t *ctl = fbController(fb);
		if (r == kIOReturnSuccess && fbHandleEvent && ctl) {
			auto *owner = reinterpret_cast<thread_t *>(ctl + 0x188);
			const thread_t was = *owner;
			*owner = current_thread();
			w = reinterpret_cast<IOReturn (*)(IOService *, int32_t, void *)>(fbHandleEvent)(fb, 4, reinterpret_cast<void *>(1));
			*owner = was;
		}
		FBLOG("pvgpu: %s %llx: a call while it is off for a sleep (%s): let through, marked on, the wake delivered (0x%x/0x%x)",
		      fb->getName(), fb->getRegistryEntryID(), where ? where : "?", r, w);
	}
	return r;
}

// The paravirtual framebuffer joins the power tree (under the card's PCI device) without power states: its AppleDisplay can
// never change its power, so WindowServer's display sleep never completes, and WindowServer holds back its answer to the
// system sleep ("Display sleep not yet completed, defer ack of IOPM sleep") until powerd gives up after 30 s ("WindowServer
// timed out(30000 ms)"; card, 2026-11-15). With rdna4-syssleep it is given the two states a framebuffer has, as IONDRVSupport
// gives ours (IONDRVFramebuffer::initForPM), for its AppleDisplay to follow; its changes are acknowledged at once
// (wrapFbSetPowerState).
mach_vm_address_t orgFbStart;

bool wrapFbStart(IOService *fb, IOService *provider) {
	const bool started = FunctionCast(wrapFbStart, orgFbStart)(fb, provider);
	if (started && sysSleepLog && isParavirtFb(fb)) {
		static IOPMPowerState states[2] = {
			{ kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
			{ kIOPMPowerStateVersion1, kIOPMDeviceUsable | kIOPMPowerOn | kIOPMMaxPerformance, kIOPMPowerOn, kIOPMPowerOn,
			  0, 0, 0, 0, 0, 0, 0, 0 },
		};
		const IOReturn r = fb->registerPowerDriver(fb, states, 2);
		FBLOG("pvgpu: %s %llx: power states registered (0x%x): its display can sleep and wake", fb->getName(),
		      fb->getRegistryEntryID(), r);
	}
	return started;
}

// Its power changes are only for its AppleDisplay to follow: acknowledged at once, without IOFramebuffer's power work (which
// it never had before it had states). With two paravirtual displays that work never ran for one of them, and each of its
// changes waited out the 45 s acknowledgement timeout, at boot, sleep and wake (card, 2026-11-15).
mach_vm_address_t orgFbSetPowerState;

IOReturn wrapFbSetPowerState(IOService *fb, unsigned long state, IOService *device) {
	if (sysSleepLog && isParavirtFb(fb))
		return kIOPMAckImplied;
	return FunctionCast(wrapFbSetPowerState, orgFbSetPowerState)(fb, state, device);
}

// WebKit's GPU process may read /Library/GPUBundles but its sandbox (com.apple.WebKit.GPUProcess.sb) denies
// file-map-executable there: Apple's own GPU drivers live in /System/Library/Extensions, which it allows, so Apple never
// needs the exec-map of a third-party bundle. The result is that the GPU process can read our Metal driver but not run it,
// and WebGL/Metal in Safari fall back or fail. This turns a *denied* executable mmap into an allow, only for a WebKit
// process mapping a file inside our own bundle; every other mmap decision is left to the system policy. It is free on the
// common path (the vnode/path/name work runs only after the policy already said no). Opt out with rdna4-webkitgpu=0.
// Resolved by solveSymbol at patcher load, called through these: fg_get_vnode is not a KPI export, so calling it directly
// would leave an unresolved kernel symbol and stop the kext loading. proc_best_name gives p_name when set (the full
// "com.apple.WebKit.GPU", not p_comm's 16 chars).
void *(*gFgGetVnode)(void *fg) = nullptr;                 // the fileglob's vnode, or null for a non-vnode file
int (*gVnGetpath)(void *vp, char *buf, int *len) = nullptr;
const char *(*gProcBestName)(void *proc) = nullptr;
void *(*gCurrentProc)(void) = nullptr;

mach_vm_address_t orgMacFileCheckMmap;

int wrapMacFileCheckMmap(void *cred, void *fg, int prot, int flags, unsigned long long offset, int *maxprot) {
	const int r = FunctionCast(wrapMacFileCheckMmap, orgMacFileCheckMmap)(cred, fg, prot, flags, offset, maxprot);
	if (r == 0 || !fg)   // allowed already, or no file: the overwhelmingly common path, no further work
		return r;
	void *vp = gFgGetVnode(fg);
	if (!vp)
		return r;
	char path[1024];
	int len = sizeof(path);
	if (gVnGetpath(vp, path, &len) != 0)
		return r;
	static const char kBundle[] = "/Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle/";
	if (strncmp(path, kBundle, sizeof(kBundle) - 1) != 0)
		return r;
	const char *name = gProcBestName(gCurrentProc());
	if (!name || strncmp(name, "com.apple.WebKit", 16) != 0)   // WebKit's GPU (and other WebKit) processes only
		return r;
	static uint32_t logged = 0;
	if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 4)
		FBLOG("sandbox: %s may map %s (the sandbox had denied the executable mapping)", name, path);
	if (maxprot)
		*maxprot |= 0x4;   // VM_PROT_EXECUTE: the policy cleared it; the map is allowed to be executable
	return 0;
}

void processGraphics(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (index != kextIOGraphics.loadIndex)
		return;
	KernelPatcher::RouteRequest start[] {
		{"__ZN13IOFramebuffer5startEP9IOService", wrapFbStart, orgFbStart},
		{"__ZN13IOFramebuffer13setPowerStateEmP9IOService", wrapFbSetPowerState, orgFbSetPowerState},
	};
	if (!patcher.routeMultiple(index, start, 2, address, size))
		FBLOG("ndrv: IOFramebuffer::start/setPowerState not routed (error %d): the paravirtual display keeps a sleep waiting 30 s",
		      patcher.getError());
	patcher.clearError();
	KernelPatcher::RouteRequest open {"__ZN13IOFramebuffer4openEv", wrapFbOpen, orgFbOpen};
	if (!patcher.routeMultiple(index, &open, 1, address, size))
		FBLOG("ndrv: IOFramebuffer::open not routed (error %d): framebuffers can lose their power work", patcher.getError());
	patcher.clearError();
	fbHandleEvent = patcher.solveSymbol(index, "__ZN13IOFramebuffer11handleEventEiPv", address, size);
	patcher.clearError();
	fbStartThread = patcher.solveSymbol(index, "__ZN13IOFramebuffer11startThreadEb", address, size);
	patcher.clearError();
	KernelPatcher::RouteRequest extEntry {"__ZN13IOFramebuffer9_extEntryEbbjPKc", wrapExtEntry, orgExtEntry};
	if (!patcher.routeMultiple(index, &extEntry, 1, address, size))
		FBLOG("ndrv: IOFramebuffer::_extEntry not routed (error %d): a dark wake can leave WindowServer waiting", patcher.getError());
	patcher.clearError();
	KernelPatcher::RouteRequest notify {"__ZN13IOFramebuffer12notifyServerEh", wrapFbNotifyServer, orgFbNotifyServer};
	if (!fbStartThread || !patcher.routeMultiple(index, &notify, 1, address, size))
		FBLOG("ndrv: IOFramebuffer::notifyServer not routed (error %d): a system sleep waits 90 s for our framebuffers",
		      patcher.getError());
	patcher.clearError();
}

bool ndrvRouted = false;   // processKext ran: by Lilu's load notice, or by lateNdrvCheck
bool lateRouting = false;  // processKext is lateNdrvCheck's (the rdna4-ndrvdelay test hook leaves it be)
// (test) rdna4-ndrvdelay=<ms>: Lilu's notice of IONDRVSupport acted on that much later, on a thread call: the boot framebuffer then starts
// before our routes, as when another kext loads IONDRVSupport ahead of the notice (the race lateNdrvCheck has to win).
struct DelayedRoute { KernelPatcher *patcher; size_t index; mach_vm_address_t address; size_t size; } gDelayedRoute;
thread_call_t gDelayedRouteCall = nullptr;
void processKext(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
void delayedRoute(thread_call_param_t, thread_call_param_t) {
	FBLOG("ndrv: (test) rdna4-ndrvdelay: routing IONDRVSupport now");
	processKext(nullptr, *gDelayedRoute.patcher, gDelayedRoute.index, gDelayedRoute.address, gDelayedRoute.size);
}

void processKext(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address,
                 size_t size) {
	uint32_t delayMs = 0;
	if (index == kextIONDRVSupport.loadIndex && !gDelayedRouteCall && !lateRouting &&
	    PE_parse_boot_argn("rdna4-ndrvdelay", &delayMs, sizeof(delayMs)) && delayMs &&
	    (gDelayedRouteCall = thread_call_allocate(delayedRoute, nullptr))) {   // (never freed: once a boot)
		gDelayedRoute = { &patcher, index, address, size };
		uint64_t deadline;
		clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gDelayedRouteCall, deadline);
		FBLOG("ndrv: (test) rdna4-ndrvdelay=%u: IONDRVSupport routed %u ms late", delayMs, delayMs);
		return;
	}
	if (index != kextIONDRVSupport.loadIndex || __atomic_exchange_n(&ndrvRouted, true, __ATOMIC_SEQ_CST))
		return;
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
	vramRange.target = patcher.solveSymbol(index, "__ZN17IONDRVFramebuffer12getVRAMRangeEv", address, size);
	patcher.clearError();
	apertureRange.target = patcher.solveSymbol(index, "__ZN17IONDRVFramebuffer16getApertureRangeEi", address, size);
	patcher.clearError();
	if (second)
		installVramRange();
#ifdef AMDRadeonRDNA4_PVGPU
	gApertureRouted = installApertureRange();
#endif
	if (!vramRange.installed || !apertureRange.installed) {
		rangeRouteLock = IOLockAlloc();
		if (!rangeRouteLock)
			FBLOG("modes: cannot allocate the lock for deferred framebuffer range routes");
	}
	KernelPatcher::RouteRequest request {
		"__ZN17IONDRVFramebuffer10doDriverIOEjPvjj", wrapDoDriverIO, orgDoDriverIO,
	};
	if (patcher.routeMultiple(index, &request, 1, address, size))
		FBLOG("ndrv: routed IONDRVFramebuffer::doDriverIO");
	else
		FBLOG("ndrv: failed to route IONDRVFramebuffer::doDriverIO (error %d)",
		      patcher.getError());
	patcher.clearError();
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
	}
#ifdef AMDRadeonRDNA4_PVGPU
	{   // (pass-through unless our accelerator serves WindowServer)
		uint32_t accel = 0;
		gAccelCursor = Trial::bootArg("rdna4-accel", accel) && accel == 4;
		KernelPatcher::RouteRequest cursor[] {
			{ "__ZN17IONDRVFramebuffer14setCursorImageEPv", wrapSetCursorImage, orgSetCursorImage },
			{ "__ZN17IONDRVFramebuffer14setCursorStateEiib", wrapSetCursorState, orgSetCursorState },
			{ "__ZN17IONDRVFramebuffer12getAttributeEjPm", wrapFbGetAttribute, orgFbGetAttribute },
		};
		if (!patcher.routeMultiple(index, cursor, 3, address, size))
			FBLOG("cursor: no IONDRVFramebuffer::setCursorImage/setCursorState/getAttribute routes (error %d)", patcher.getError());
		patcher.clearError();
		uint32_t vrr = 0;
		if (Trial::bootArg("rdna4-vrrprobe", vrr) && vrr) {
			KernelPatcher::RouteRequest probe[] {
				{ "__ZN17IONDRVFramebuffer12setAttributeEjm", wrapFbSetAttribute, orgFbSetAttribute },
				{ "__ZN17IONDRVFramebuffer25getAttributeForConnectionEijPm", wrapFbGetConnAttribute, orgFbGetConnAttribute },
				{ "__ZN17IONDRVFramebuffer27getTimingInfoForDisplayModeEiP19IOTimingInformation", wrapFbTimingInfo, orgFbTimingInfo },
			};
			gVrrProbe = patcher.routeMultiple(index, probe, 3, address, size);
			FBLOG("vrrprobe: IONDRVFramebuffer routes %s (error %d)", gVrrProbe ? "in" : "NOT in", patcher.getError());
			patcher.clearError();
		}
	}
#endif
	// Both kinds of hot-plug start their HPD poll when IOFramebuffer opens.
	uint32_t hotplug = second >= 2 ? 2 : 0, dptrain = 0;
	if (second >= 2)
		(void)Trial::option("rdna4-hotplug", hotplug);
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

// Boots where IONDRVSupport is loaded where Lilu does not see it in time: SimpleHDA loads it itself the moment the system kext collection is
// ready (2026-10-22: at 18 s, the framebuffer published 0.4 s later; Lilu's first load notice of the boot at 21 s). Then IONDRVFramebuffer
// starts without our routes and is gone, and the card is never brought up (only the firmware's picture): lateNdrvCheck below.
thread_call_t gLateNdrv = nullptr;
// IONDRVSupport routed from the kernel's kext list when Lilu has not told us of its load: polled every 20 ms from 1 s on (every 2 s from 60
// s, until 120 s), so the routes are in before its framebuffers match. SimpleHDA loads IONDRVSupport itself inside the catalog hook and
// holds its personalities until its own linking is done (~0.5 s on the rig); Lilu's notice came after the boot framebuffer had started in
// some boots, which used up the boot display's NDRV for good: the card never came up. Matching the framebuffers again afterwards (the
// framebuffers terminated, the display nubs or the GPU registered again) did not bring it back on the card (2026-11-21): be in time.
// Card: with Lilu's notice held back 3 s (rdna4-ndrvdelay=3000) routed at 9.8 s from the list, the card up as on a normal boot.
void lateNdrvCheck(thread_call_param_t, thread_call_param_t) {
	uint64_t up = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &up);
	auto again = [](uint64_t ms) {
		uint64_t deadline;
		clock_interval_to_deadline(static_cast<uint32_t>(ms), kMillisecondScale, &deadline);
		thread_call_enter_delayed(gLateNdrv, deadline);
	};
	if (__atomic_load_n(&ndrvRouted, __ATOMIC_SEQ_CST))   // Lilu told us in time
		return;
	static kmod_info_t **kmods;
	if (!kmods && gPatcher) {
		kmods = reinterpret_cast<kmod_info_t **>(gPatcher->solveSymbol(KernelPatcher::KernelID, "_kmod"));
		gPatcher->clearError();
	}
	kmod_info_t *ndrv = nullptr;
	for (kmod_info_t *k = kmods ? *kmods : nullptr; k && !ndrv; k = k->next)   // as Lilu walks it (kern_patcher.cpp processAlreadyLoadedKexts)
		if (!strncmp(k->name, kextIONDRVSupport.id, KMOD_MAX_NAME))
			ndrv = k;
	static uint32_t seen;   // polls that found it: Lilu's own notice gets one more poll, and the load its end
	if (!ndrv || ++seen < 2) {
		if (up < 120ull * NSEC_PER_SEC)
			again(up < 60ull * NSEC_PER_SEC ? 20 : 2000);
		else
			FBLOG("ndrv: IONDRVSupport never loaded and Lilu never reported it: the card is not brought up");
		return;
	}
	FBLOG("ndrv: Lilu has not reported IONDRVSupport's load (another kext loaded it); routing it from the kext list at %llu ms",
	      up / NSEC_PER_MSEC);
	// As Lilu before its load callbacks (kern_api.cpp processKextLoadCallbacks): the kext's running symbols, and its whole size (a kmod's is
	// only __TEXT since 11.0). Without this its symbols cannot be solved ("no loaded symbols buffer found", seen on the rig 2026-10-22).
	const size_t size = gPatcher->updateRunningInfo(kextIONDRVSupport.loadIndex, ndrv->address, ndrv->size);
	gPatcher->clearError();
	lateRouting = true;
	processKext(nullptr, *gPatcher, kextIONDRVSupport.loadIndex, ndrv->address, size);
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
	(void)Trial::bootArg("rdna4-pvgpu-delay", pvDelay);
	if (pvGpu)
		RDNA4PvNub::publishLater(pvGpu, pvDelay ? pvDelay * 1000 : 25000);
#endif
	Ndrv::vslInit();
	FBLOG("Lilu plugin started (trace %s, compute stage %u%s%s)", traceEnabled ? "on" : "off",
	      computeStage, accelCensus ? ", ACCEL CENSUS (emulator only)" : "", pvGpu ? ", FAKE PARAVIRT GPU (VM only)" : "");
	lilu.onKextLoadForce(&kextIONDRVSupport, 1, processKext, nullptr);
	lilu.onKextLoadForce(&kextIOGraphics, 1, processGraphics, nullptr);
	lilu.onKextLoadForce(&kextIOPCIFamily, 1, [](void *, KernelPatcher &, size_t, mach_vm_address_t, size_t) {}, nullptr);   // (its index, for debug.rdna4_hda)
	lilu.onKextLoadForce(&kextAppleGFXHDA, 1, processGfxHda, nullptr);   // (loads only after debug.rdna4_hda=1)
	RDNA4Accel::start();   // our own accelerator: nothing published unless debug.rdna4_accel=1 (rdna4accel.cpp)
#ifdef AMDRadeonRDNA4_PVGPU
	// whether this boot has the fake GPU is only known once NVRAM is (rdna4-trial): the functions are resolved in any case
	lilu.onKextLoadForce(&kextIOSurface, 1, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		if (index != kextIOSurface.loadIndex)
			return;
		// In the order of iosurface.hpp's IOSurfaceFunctions.
		static const char *const names[kSurfaceFunctions] = {
			"__ZN13IOSurfaceRoot13lookupSurfaceEjP4task", "__ZNK9IOSurface19getMemoryDescriptorEv", "__ZNK9IOSurface14getBytesPerRowEv",
			"__ZNK9IOSurface8getWidthEv", "__ZNK9IOSurface9getHeightEv", "__ZNK9IOSurface14getPixelFormatEv", "__ZNK9IOSurface13getBaseOffsetEv",
			"__ZNK9IOSurface12getSurfaceIDEv", "__ZNK9IOSurface13getPlaneCountEv", "__ZNK9IOSurface14getPlaneOffsetEj",
			"__ZNK9IOSurface19getPlaneBytesPerRowEj", "__ZNK9IOSurface13getPlaneWidthEj", "__ZNK9IOSurface14getPlaneHeightEj",
			"__ZNK9IOSurface11getLockSeedEv", "__ZNK9IOSurface13getUnlockSeedEv"};
		mach_vm_address_t fn[kSurfaceFunctions];
		unsigned found = 0;
		for (unsigned k = 0; k < kSurfaceFunctions; k++)
			found += (fn[k] = patcher.solveSymbol(index, names[k], address, size)) != 0;
		patcher.clearError();
		if (found == kSurfaceFunctions)
			setSurfaceFunctions(fn);
		FBLOG("pvgpu: IOSurface: %u of %u functions found%s", found, kSurfaceFunctions,
		      found == kSurfaceFunctions ? "" : ": no IOSurface textures");
	}, nullptr);
	PvHooks::start();
	PvScanout::start();
#endif
	lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
		gPatcher = &patcher;
		uint32_t webkit = 1;   // default on; rdna4-webkitgpu=0 opts out
		if (!Trial::bootArg("rdna4-webkitgpu", webkit) || webkit) {
			gFgGetVnode = reinterpret_cast<decltype(gFgGetVnode)>(patcher.solveSymbol(KernelPatcher::KernelID, "_fg_get_vnode"));
			gVnGetpath = reinterpret_cast<decltype(gVnGetpath)>(patcher.solveSymbol(KernelPatcher::KernelID, "_vn_getpath"));
			gProcBestName = reinterpret_cast<decltype(gProcBestName)>(patcher.solveSymbol(KernelPatcher::KernelID, "_proc_best_name"));
			gCurrentProc = reinterpret_cast<decltype(gCurrentProc)>(patcher.solveSymbol(KernelPatcher::KernelID, "_current_proc"));
			patcher.clearError();
			if (gFgGetVnode && gVnGetpath && gProcBestName && gCurrentProc) {
				KernelPatcher::RouteRequest wk {
					"_mac_file_check_mmap", wrapMacFileCheckMmap, orgMacFileCheckMmap,
				};
				const bool routed = patcher.routeMultiple(KernelPatcher::KernelID, &wk, 1);
				FBLOG("sandbox: mac_file_check_mmap %s: WebKit's GPU process may run /Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle",
				      routed ? "routed" : "NOT routed");
				patcher.clearError();
			} else {
				FBLOG("sandbox: not hooking mac_file_check_mmap: a helper symbol was not found (WebKit GPU map left to the system)");
			}
		}
		if (RDNA4Device::systemSleepWanted()) {
			KernelPatcher::RouteRequest power {
				"__ZN9IOService19registerPowerDriverEPS_P14IOPMPowerStatem", wrapRegisterPowerDriver,
				orgRegisterPowerDriver,
			};
			const bool routed = patcher.routeMultiple(KernelPatcher::KernelID, &power, 1);
			sysSleepLog = true;
			FBLOG("power: rdna4-syssleep: IOService::registerPowerDriver %s", routed ? "routed" : "NOT routed");
			patcher.clearError();
		}
#ifdef AMDRadeonRDNA4_PVGPU
		auto **base = reinterpret_cast<uint8_t **>(patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_base"));
		auto **max = reinterpret_cast<uint8_t **>(patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_max"));

		patcher.clearError();
		if (base && max)
			RDNA4PvNub::setPhysmap(*base, *max);
		FBLOG("pvgpu: physmap %s", base && max ? "found" : "NOT found: guest memory through mappings of each page");
		pvgpu::routeGlPages(patcher);
#endif
	});
	if ((gLateNdrv = thread_call_allocate(lateNdrvCheck, nullptr))) {   // ponytail: never freed, it lives for the boot
		uint64_t deadline;
		clock_interval_to_deadline(1, kSecondScale, &deadline);   // (the poll: lateNdrvCheck)
		thread_call_enter_delayed(gLateNdrv, deadline);
	}
}

const char *bootargDebug[] { "-rdna4dbg" };

} // namespace

// rdna4accel.cpp, as WindowServer is let go: framebuffer `fb` is bare and hidden from it, so WindowServer never tells its controller
// it is up ('wsup'); without this the controller's state kept bit 0x8 and every system sleep waited 90 s for it (gIOGNotifyTO:
// "PM priority notification timeout (com.apple.iokit.IOGraphicsFamily)", 2026-11-22).
void ndrvServerNeverOpens(IOService *fb) {
	const int was = clearServerUpBit(fb);
	if (was >= 0)
		FBLOG("ndrv: %s %llx (bare, hidden from WindowServer): marked up for WindowServer (controller's bits 0x%02x before)",
		      fb->getName(), fb->getRegistryEntryID(), was);
}

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

bool PvScanout::framebufferLit(IOService *fb)
{
	if (Head2 *h = headFor(fb))
		return h->connected || h->waitClocks;
	for (const auto &e : fbTable)
		if (e.fb == fb)
			return e.state && e.state == bootState;
	return false;
}

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

			if ((!h->connected && !h->waitClocks) || h == pvBootHead)   // the boot head's port is 0
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

int PvScanout::portOf(IOService *fb)
{
	(void)PvScanout::count();   // (numbers the ports)
	if (Head2 *h = headFor(fb)) {
		for (uint32_t i = 0; i < pvPortCount; i++)
			if (pvPorts[i] == h)
				return static_cast<int>(i) + 1;
		return -1;
	}
	for (const auto &e : fbTable)
		if (e.fb == fb)
			return e.state && e.state == bootState ? 0 : -1;
	return -1;
}

bool PvScanout::mode(uint32_t port, uint32_t &width, uint32_t &height)
{
	Head head;

	if (port == 0 && pvBootHead && !pvBootHead->connected) {   // lit at 3840x2160 once the display clocks are up
		width = 3840;
		height = 2160;

		return true;
	}

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

// A head's paravirtual display port: 0 the boot display's (pvBootHead when it took the boot display over), then the extra heads.
Head2 *pvHead(uint32_t port)
{
	if (port == 0)
		return pvBootHead;

	return port <= pvPortCount ? const_cast<Head2 *>(pvPorts[port - 1]) : nullptr;
}

// The mode of a size nearest 60 Hz in `table`.
const Modes::Mode *nearest60(const Modes::Mode *table, size_t n, uint32_t width, uint32_t height)
{
	const Modes::Mode *best = nullptr;

	for (size_t i = 0; i < n; i++) {
		const Modes::Mode &m = table[i];
		const uint32_t off = m.refreshMilliHz > 60000 ? m.refreshMilliHz - 60000 : 60000 - m.refreshMilliHz;

		if (m.t.hActive != width || m.t.vActive != height)
			continue;

		if (!best || off < (best->refreshMilliHz > 60000 ? best->refreshMilliHz - 60000 : 60000 - best->refreshMilliHz))
			best = &m;
	}

	return best;
}

// The mode of that size with the highest refresh (the boot display's, rdna4-pvfast=1), or nullptr.
const Modes::Mode *fastest(const Modes::Mode *table, size_t n, uint32_t width, uint32_t height)
{
	const Modes::Mode *best = nullptr;

	for (size_t i = 0; i < n; i++)
		if (table[i].t.hActive == width && table[i].t.vActive == height &&
		    (!best || table[i].refreshMilliHz > best->refreshMilliHz))
			best = &table[i];

	return best;
}

// Port 0 when no head took the boot display over (a DisplayPort boot display: boot4k takes only an HDMI one behind an adapter): the boot
// framebuffer's own modes (its NDRV table, at most the console's size, scanned from the console's memory at its pitch) and the size it
// runs in now; its mode switches go through its mode-set engine (RDNA4Device::applyMode: on DisplayPort the stream retimed, the link as
// trained).
bool bootModes(const Modes::Mode *&table, size_t &n, uint32_t &width, uint32_t &height)
{
	if (pvBootHead || !bootState || !bootState->dev.modeCount)
		return false;

	const RDNA4Device &dev = bootState->dev;
	uint32_t liveW, liveH;
	const auto *large = dev.activeDpMode();
	const bool live = dev.liveSize(liveW, liveH) && liveW && liveW <= dev.fbWidth && liveH && liveH <= dev.fbHeight;

	table = dev.modeTable;
	n = dev.modeCount;
	width = large ? large->t.hActive : live ? liveW : dev.fbWidth;
	height = large ? large->t.vActive : live ? liveH : dev.fbHeight;
	return true;
}

uint32_t PvScanout::modes(uint32_t port, Mode *out, uint32_t cap)
{
	Head2 *h = pvHead(port);
	static Modes::Mode built[Modes::MaxModes];
	const Modes::Mode *table = nullptr;
	size_t n = 0;
	uint32_t litW = 0, litH = 0, count = 0;

	if (!cap || (port == 0 && !h && !bootModes(table, n, litW, litH)) || (port && !h) || !bootState)
		return 0;

	if (!h) {
		// the boot framebuffer's (bootModes)
	} else if (h->connected && h->ndrv.modeCount() > 1) {
		table = h->table;
		n = h->ndrv.modeCount();
		litW = h->surface.width;
		litH = h->surface.height;
	} else if (h == pvBootHead || h->waitClocks) {   // not lit yet: its monitor's, as a head's pipe can run them, from 3840x2160
		const uint8_t *edid = h->edidLen ? h->edid : nullptr;
		size_t len = h->edidLen;

		if (!edid && !PvScanout::edid(port, edid, len))
			return 0;

		// what a head lit at 3840x2160 takes (the limits of whichever table is chosen now may be another connector's)
		Modes::Limits lim {};

		lim.maxHActive = 3840;
		lim.maxVActive = 2160;
		lim.maxPixelClockKHz = ModeSet::kMaxTmdsKHz;
		n = Modes::build(edid, len, lim, built, Modes::MaxModes);
		table = built;
		litW = 3840;
		litH = 2160;
	}

	// The size it is lit in, then the others from the largest down, each once, as many as fit; a pipe not lit yet is lit at
	// 3840x2160@60 (through a DP++ adapter in 4:2:0, which the limits for the other modes leave out).
	Mode all[Modes::MaxModes + 7];
	uint32_t total = 0;

	if (!nearest60(table, n, litW, litH))
		all[total++] = { litW, litH, 60000 };

	// Above 3840x2160, sizes no pipe runs: macOS's HiDPI "looks like 2048x1152" to "3360x1890" draw in twice those, which the host
	// scales down to the largest mode (presentOnCard). The steps macOS offers a 4K monitor on Apple's framebuffer of a 6600 XT, which
	// it makes from that framebuffer's scaler; the paravirtual one's modes are only those of this list.
	uint32_t bigW = litW, bigH = litH;

	for (size_t i = 0; i < n; i++)
		if (static_cast<uint64_t>(table[i].t.hActive) * table[i].t.vActive > static_cast<uint64_t>(bigW) * bigH) {
			bigW = table[i].t.hActive;
			bigH = table[i].t.vActive;
		}

	for (size_t i = 0; bigW >= 3840 && pvScaledModes && i < Modes::MaxScaled; i++)
		all[total++] = { 2 * bigW * Modes::ScaledSteps[i][0] / Modes::ScaledSteps[i][1],
		                 2 * bigH * Modes::ScaledSteps[i][0] / Modes::ScaledSteps[i][1], 60000 };

	const bool fast = !h && pvFastModes;   // the boot display's (port 0)

	for (size_t i = 0; i < n && total < Modes::MaxModes + 7; i++) {
		const Modes::Mode *best = (fast ? fastest : nearest60)(table, n, table[i].t.hActive, table[i].t.vActive);
		bool seen = false;

		for (uint32_t k = 0; k < total; k++)
			seen = seen || (all[k].width == best->t.hActive && all[k].height == best->t.vActive);

		if (!seen)
			all[total++] = { best->t.hActive, best->t.vActive, best->refreshMilliHz };
	}

	for (uint32_t i = 0; i < total; i++)
		for (uint32_t k = i + 1; k < total; k++) {
			const bool kLit = all[k].width == litW && all[k].height == litH, iLit = all[i].width == litW && all[i].height == litH;
			const bool larger = static_cast<uint64_t>(all[k].width) * all[k].height > static_cast<uint64_t>(all[i].width) * all[i].height;

			if (!iLit && (kLit || larger)) {
				const Mode t = all[i];

				all[i] = all[k];
				all[k] = t;
			}
		}

	// From the largest down, not a size that is half of a monitor's mode kept: macOS offers that size as the HiDPI mode of the larger one
	// already and listed this one again as "(niedrige Auflösung)" (1920x1080 beside 3840x2160). Half of a scaled size above the monitor's
	// stays: it is a mode of the monitor's own (2560x1440 beside 5120x2880 went, and with it macOS's 1280x720 HiDPI). The lit one stays.
	for (uint32_t i = 0; i < total && count < cap; i++) {
		bool half = false;

		for (uint32_t k = 0; k < count && !half; k++)
			half = out[k].width <= bigW && out[k].height <= bigH && out[k].width == 2 * all[i].width && out[k].height == 2 * all[i].height;

		if (!half || (all[i].width == litW && all[i].height == litH))
			out[count++] = all[i];
	}

	return count;
}

bool PvScanout::largest(uint32_t port, uint32_t &width, uint32_t &height)
{
	Head2 *h = pvHead(port);
	const Modes::Mode *table = h ? h->table : nullptr;
	size_t n = h && h->connected ? h->ndrv.modeCount() : 0;
	uint32_t liveW, liveH;

	if (port == 0 && !h && !bootModes(table, n, liveW, liveH))
		n = 0;

	width = height = 0;

	for (size_t i = 0; i < n; i++) {
		const Edid::DetailedTiming &t = table[i].t;

		if (static_cast<uint64_t>(t.hActive) * t.vActive > static_cast<uint64_t>(width) * height) {
			width = t.hActive;
			height = t.vActive;
		}
	}

	return width && height;
}

// Once our accelerator serves WindowServer (rdna4accel.cpp, before WindowServer is let go): only it can present a frame larger than the head.
void PvScanout::offerScaled()
{
	gScaledOffered = true;
	if (gApertureRouted && bootState && !pvBootHead && !pvHeads && pvScaledModes && bootState->dev.modesetRequested) {
		const RDNA4Device &dev = bootState->dev;
		const Modes::Mode *base = Modes::scaleBase(dev.modeTable, dev.modeCount);

		gBootScaledCount = base && base->t.hActive <= dev.fbWidth && base->t.vActive <= dev.fbHeight
		                   ? Modes::buildScaled(dev.modeTable, dev.modeCount, gBootScaled, Modes::MaxScaled) : 0;
		bootState->ndrv.setScaled(gBootScaled, gBootScaledCount);
		if (gBootScaledCount)
			FBLOG("modes: boot display: %zu scaled modes offered, up to %ux%u", gBootScaledCount,
			      gBootScaled[gBootScaledCount - 1].t.hActive, gBootScaled[gBootScaledCount - 1].t.vActive);
	}
	for (size_t i = 0; i < headCount; i++)
		offerScaledOn(heads[i]);
}

bool PvScanout::scaledFrame(uint32_t port, uint32_t &width, uint32_t &height)
{
	if (Head2 *extra = pvHead(port))
		return extra->ndrv.scaledFrame(width, height);

	return port == 0 && bootState && bootState->ndrv.scaledFrame(width, height);
}

bool PvScanout::wantsMode(uint32_t port, uint32_t width, uint32_t height)
{
	const Modes::Mode *table = nullptr;
	size_t n = 0;
	uint32_t liveW, liveH;

	if (port || pvHead(0) || !bootModes(table, n, liveW, liveH))   // the boot display's: the heads light their own
		return false;

	const Modes::Mode *m = (pvFastModes ? fastest : nearest60)(table, n, width, height);
	const Edid::DetailedTiming *live = bootState->dev.liveMode();

	return m && live && Modes::match(m, 1, *live) < 0;
}

bool PvScanout::setMode(uint32_t port, uint32_t width, uint32_t height)
{
	Head2 *h = pvHead(port);
	const Modes::Mode *table = nullptr;
	size_t n = 0;
	uint32_t liveW, liveH;

	if (port == 0 && !h && bootModes(table, n, liveW, liveH)) {
		const Modes::Mode *m = (pvFastModes ? fastest : nearest60)(table, n, width, height);
		const IOReturn r = m ? bootState->dev.applyMode(*m) : kIOReturnUnsupported;

		FBLOG("pvgpu: port 0: the boot display switched to %ux%u for macOS's mode: %s", width, height,
		      !m ? "no such mode" : r == kIOReturnSuccess ? "done" : "FAILED");
		return r == kIOReturnSuccess;
	}

	const Modes::Mode *m = h && h->connected ? nearest60(h->table, h->ndrv.modeCount(), width, height) : nullptr;

	if (!m) {
		FBLOG("pvgpu: port %u: no mode of %ux%u to switch its head to", port, width, height);
		return false;
	}

	RDNA4Device::ExtraHold hold(*h->dev);
	h->dev->selectExtra(h->slot);

	const IOReturn r = h->dev->applySecondPipeMode(*m);

	HLOG(h, "port %u: switched to %ux%u@%u.%03u for macOS's mode: %s", port, width, height, m->refreshMilliHz / 1000,
	     m->refreshMilliHz % 1000, r == kIOReturnSuccess ? "done" : "FAILED");

	if (r != kIOReturnSuccess)
		return false;

	h->surface.width = width;
	h->surface.height = height;

	return true;
}

bool PvScanout::edid(uint32_t port, const uint8_t *&data, size_t &len)
{
	if (port == 0 && pvBootHead && pvBootHead->edidLen) {
		data = pvBootHead->edid;
		len = pvBootHead->edidLen;

		return true;
	}

	if (port == 0) {
		if (!bootState || !bootState->dev.edidLen)
			return false;

		data = bootState->dev.edidData;
		len = bootState->dev.edidLen;

		return true;
	}

	if (port > pvPortCount)
		return false;

	const Head2 *h = pvPorts[port - 1];

	if (h->edidLen) {
		data = h->edid;
		len = h->edidLen;

		return true;
	}

	// a head waiting for the display clocks carries no EDID of its own: its extra pipe read the sink's
	if (!bootState || h->slot >= RDNA4Device::kMaxExtra || !bootState->dev.extra[h->slot].edidLen)
		return false;

	data = bootState->dev.extra[h->slot].edid;
	len = bootState->dev.extra[h->slot].edidLen;

	return true;
}

// A head's hardware cursor: the boot head's on the boot device's own pipe (not a placeholder head standing in for it), an extra head's on
// its pipe, the device's cursor calls pointed at it (setCursorPipe) under its ExtraHold; one sprite for all. WindowServer takes a hardware
// cursor only when every display has one: with port 0's alone it went on drawing the pointer into the frames (2026-11-18).
static RDNA4Device *pvCursorDevice(uint32_t port, Head2 *&extra)
{
	extra = port ? pvHead(port) : nullptr;

	if (port == 0)
		return !pvBootHead && bootState && bootState->dev.supportsHardwareCursor() ? &bootState->dev : nullptr;

	return extra && extra->connected && extra->dev && extra->dev->supportsHardwareCursor() ? extra->dev : nullptr;
}

bool PvScanout::cursorReady(uint32_t port)
{
	Head2 *extra;

	return pvCursorDevice(port, extra) != nullptr;
}

// `call` with the device's cursor calls on the head's pipe.
template <typename Call>
static bool pvOnCursorPipe(uint32_t port, Call call)
{
	Head2 *extra;
	RDNA4Device *dev = pvCursorDevice(port, extra);

	if (!dev)
		return false;

	// held for the boot head as well: the device's cursor state is one for both heads, and our accelerator's framebuffers call from
	// their own threads at once (the paravirtual host from its one work loop): a boot-head move in the middle of the second head's
	// image went to the wrong pipe
	RDNA4Device::ExtraHold hold(*dev);
	if (!extra)
		return call(*dev);

	dev->selectExtra(extra->slot);
	dev->setCursorPipe(dev->xp().pipe);
	const bool ok = call(*dev);
	dev->setCursorPipe(0xff);

	return ok;
}

bool PvScanout::cursorImage(uint32_t port, const uint32_t *argb, uint32_t width, uint32_t height, uint32_t pitch, uint32_t hotX, uint32_t hotY)
{
	return pvOnCursorPipe(port, [&](RDNA4Device &dev) {
		return dev.setCursorPixels(argb, width, height, pitch, hotX, hotY) == kIOReturnSuccess;
	});
}

// A cursor image larger than the plane (macOS's are announced at 128x128): what it covers (alpha above 0) and its hot spot onto the
// plane, when that fits.
bool PvScanout::cursorArgb(uint32_t port, const uint32_t *argb, uint32_t width, uint32_t height, uint32_t pitch, uint32_t hotX, uint32_t hotY)
{
	if (!argb || !width || !height)
		return false;

	uint32_t x0 = hotX < width ? hotX : width - 1, y0 = hotY < height ? hotY : height - 1, x1 = x0, y1 = y0;

	for (uint32_t y = 0; y < height; y++)
		for (uint32_t x = 0; x < width; x++)
			if (argb[y * pitch + x] >> 24) {
				x0 = x < x0 ? x : x0;
				x1 = x > x1 ? x : x1;
				y0 = y < y0 ? y : y0;
				y1 = y > y1 ? y : y1;
			}

	return cursorImage(port, argb + y0 * pitch + x0, x1 - x0 + 1, y1 - y0 + 1, pitch, hotX - x0, hotY - y0);
}

// x, y: the hot spot's position (what the guest's page holds); drawHardwareCursor takes the image's top-left corner.
bool PvScanout::cursorMove(uint32_t port, int32_t x, int32_t y, bool visible)
{
	uint32_t frameW, frameH;
	Head lit;

	if (scaledFrame(port, frameW, frameH) && head(port, lit)) {   // in a scaled mode macOS's position is on the frame, the plane's on the head
		x = static_cast<int32_t>(static_cast<int64_t>(x) * lit.width / frameW);
		y = static_cast<int32_t>(static_cast<int64_t>(y) * lit.height / frameH);
	}

	return pvOnCursorPipe(port, [&](RDNA4Device &dev) {
		return dev.drawHardwareCursor(x - static_cast<int32_t>(dev.cursorHotX()), y - static_cast<int32_t>(dev.cursorHotY()), visible) ==
		       kIOReturnSuccess;
	});
}

bool PvScanout::fixedFrontBuffer(uint32_t port) {
	return port == 0 && !pvBootHead && bootState && bootState->dev.dpVram;
}

bool PvScanout::head(uint32_t port, Head &out)
{
	if (port == 0 && pvBootHead) {
		const Head2 *h = pvBootHead;

		if (!h->connected)
			return false;

		out = { h->surface.physBase, h->surface.rowBytes, h->surface.width, h->surface.height };

		return true;
	}

	if (port == 0) {
		if (!bootState)
			return false;

		// Use the committed scanout geometry for native presents, including the first frame after a switch.
		const RDNA4Device &dev = bootState->dev;
		if (const auto *large = dev.activeDpMode()) {
			out = {dev.dpSurface.physBase, dev.dpSurface.rowBytes, large->t.hActive, large->t.vActive};
			return true;
		}
		const Modes::Mode *table;
		size_t n;
		uint32_t width = dev.fbWidth, height = dev.fbHeight;

		(void)bootModes(table, n, width, height);
		out = { dev.fbPhysBase, dev.fbRowBytes, width, height };

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
