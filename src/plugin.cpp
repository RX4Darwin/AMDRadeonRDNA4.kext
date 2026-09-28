//
//  plugin.cpp
//  RDNA4FB
//
//  Lilu plugin entry. On a GPU macOS has no driver for, Apple's generic
//  IONDRVFramebuffer (com.apple.iokit.IONDRVSupport) drives the GOP
//  framebuffer through its boot NDRV (IOBootNDRV). Every request that driver
//  makes of its NDRV — Initialize/Open and every csc Control/Status call —
//  goes through IONDRVFramebuffer::doDriverIO. Routing that one function lets
//  RDNA4FB answer the requests for the RX 9070 XT's framebuffer while Apple's
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
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOGraphicsTypes.h>   // header-only types for IOMacOSVideo.h
#include <IOKit/ndrvsupport/IOMacOSTypes.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>

#include "compute.hpp"
#include "device.hpp"
#include "ndrv.hpp"
#ifdef RDNA4FB_VM_TEST
#include "bochsvbe.hpp"
#endif

#define FBLOG(fmt, ...)  IOLog("RDNA4FB: " fmt "\n", ## __VA_ARGS__)

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
              Ndrv::cscSwitchMode == ::cscSwitchMode && Ndrv::cscSetSync == ::cscSetSync,
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

mach_vm_address_t orgDoDriverIO { 0 };
bool traceEnabled { false };
uint32_t traceBudget { 400 };   // bounded: gamma/CLUT calls can be frequent
uint32_t computeStage { 0 };    // rdna4-compute=<stage>, see compute.hpp

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
#ifdef RDNA4FB_VM_TEST
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
	static_cast<RDNA4Device *>(ctx)->setDisplayPower(on);
}

#ifdef RDNA4FB_VM_TEST
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
	Ndrv::Backend be { &dev, deviceSurfaceFor, deviceSwitchTo, deviceSetPower };
#ifdef RDNA4FB_VM_TEST
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
	e.state = st;
	FBLOG("ndrv: answering for %p: %lu mode(s), EDID %lu bytes", e.fb,
	      static_cast<unsigned long>(st->ndrv.modeCount()), static_cast<unsigned long>(dev.edidLen));

	// Compute bring-up runs after the display is answered for, and only
	// when asked for: it must never be the reason the desktop is missing.
	if (computeStage && dev.isAmd) {
		RDNA4Compute::Env env { pci, svc, dev.mmioBase(), dev.mmioSize(), dev.discovery(),
		                        dev.fbPhysBase, dev.fbLength, dev.liveFramePeriodNs() };
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

void processKext(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address,
                 size_t size) {
	if (index != kextIONDRVSupport.loadIndex)
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
}

void pluginStart() {
	uint32_t off = 0;
	if (PE_parse_boot_argn("rdna4-off", &off, sizeof(off)) && off) {
		FBLOG("disabled by rdna4-off boot-arg");
		return;
	}
	uint32_t trace = 0;
	traceEnabled = PE_parse_boot_argn("rdna4-trace", &trace, sizeof(trace)) && trace;
#ifdef RDNA4FB_VM_TEST
	// VM test builds trace by default (OpenCore images often pin boot-args).
	if (!PE_parse_boot_argn("rdna4-trace", &trace, sizeof(trace)))
		traceEnabled = true;
#endif
	computeStage = RDNA4Compute::requestedStage();
	FBLOG("Lilu plugin started (trace %s, compute stage %u)", traceEnabled ? "on" : "off",
	      computeStage);
	lilu.onKextLoadForce(&kextIONDRVSupport, 1, processKext, nullptr);
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
