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
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOGraphicsTypes.h>   // header-only types for IOMacOSVideo.h
#include <IOKit/ndrvsupport/IOMacOSTypes.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include <IOKit/ndrvsupport/IONDRVLibraries.h>
#include <pexpert/pexpert.h>

#include "accelcensus.hpp"
#include "pvgpu.hpp"
#include "compute.hpp"
#include "device.hpp"
#include "ndrv.hpp"

uint32_t rdna4TraceLevel { 0 };

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

mach_vm_address_t orgDoDriverIO { 0 };
mach_vm_address_t orgVslNew { 0 };
mach_vm_address_t orgVslDispose { 0 };
mach_vm_address_t orgVslDo { 0 };
mach_vm_address_t orgVslPrepareCursor { 0 };
bool traceEnabled { false };
uint32_t traceBudget { 400 };   // bounded: gamma/CLUT calls can be frequent
uint32_t computeStage { 0 };    // rdna4-compute=<stage>, see compute.hpp
uint32_t pvGpu { 0 };           // rdna4-pvgpu=1|2: M0 of docs/metal-phase-plan.md, the fake Apple paravirtual GPU nub (pvgpu.hpp); VM/emulator only
uint32_t accelCensus { 0 };     // rdna4-accelcensus=1|2: the E1 impostor-IOAccelerator census (accelcensus.hpp), emulator only

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

int32_t deviceGetHardwareCursorDrawState(void *ctx,
	                                         Ndrv::VDHardwareCursorDrawStateRec &state) {
	return static_cast<RDNA4Device *>(ctx)->getHardwareCursorDrawState(state);
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
};
Head2 *head2 { nullptr };
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

bool isHead2(void *fb) {
	return head2 && static_cast<IOService *>(fb)->getProvider() == head2->nub;
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
	return static_cast<Head2 *>(ctx)->dev->applySecondPipeMode(m);
}

void head2SetPower(void *ctx, bool on) {
	auto *h = static_cast<Head2 *>(ctx);
	if (h->connected || !on)             // the link of an unplugged display stays off
		h->dev->setSecondPipePower(on);
}

// From wrapStart, before IONDRVFramebuffer starts on the PCI device.
void createHead2(IOPCIDevice *pci) {
	auto *h = new Head2;
	// IONDRVDevice is private to IONDRVSupport: made by name, driven as an IOService.
	IOService *nub = h ? OSDynamicCast(IOService, OSMetaClass::allocClassWithName("IONDRVDevice"))
	                   : nullptr;
	if (!nub || !nub->init()) {
		FBLOG("head2: not created: IONDRVSupport gave no IONDRVDevice");
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
	nub->setProperty("IOFBDependentIndex", 1ull, 32);
	nub->setLocation("1");
	if (OSArray *memory = pci->getDeviceMemory())
		nub->setDeviceMemory(memory);   // getApertureRange maps the surface out of these
	// IONDRVFramebuffer takes its device, and IONDRVDevice its power parent,
	// from the device-tree plane.
	nub->attachToParent(pci, gIODTPlane);
	if (!nub->attach(pci)) {
		FBLOG("head2: not created: the nub did not attach");
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
	head2 = h;   // before registerService: the framebuffer may start at once
	nub->registerService();
	FBLOG("head2: nub registered, dependent 1 of 0x%llx", id);
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
	}
	for (size_t i = 0; i < n; i++) {
		const Edid::DetailedTiming &t = h->table[i].t;
		if (head2Level >= 2 && !Edid::sameTiming(t, lit))
			continue;
		if (Ndrv::spareSurface(dev.fbPhysBase, dev.fbLength, h->rangeBase, h->rangeLen, t.hActive, t.vActive,
		                       h->surface))
			return &h->table[i];
	}
	return nullptr;
}

// Hand the table to the translator, with `mode` as the one the head is in.
void head2Init(Head2 *h, RDNA4Device &dev, const Modes::Mode *mode, size_t n) {
	Ndrv::Backend be {};
	be.ctx = h;
	be.surfaceFor = head2SurfaceFor;
	// Only a lit pipe answers cscGetSync, so only then does macOS send this
	// head display sleep (a phantom head has nothing to put to sleep).
	if (dev.pipe2Lit)
		be.setPower = head2SetPower;
	// Like the boot display, the other modes are offered with rdna4-modeset=1.
	const bool switching = dev.pipe2Lit && dev.modesetRequested;
	if (switching)
		be.switchTo = head2SwitchTo;
	h->ndrv.init(switching ? h->table : mode, switching ? n : 1, mode->id, h->edid, h->edidLen, be);
	for (size_t i = 0; switching && i < n; i++) {
		const Modes::Mode &m = h->table[i];
		FBLOG("head2: mode id %u %ux%u@%u.%03u %u kHz%s%s", m.id, m.t.hActive, m.t.vActive,
		      m.refreshMilliHz / 1000, m.refreshMilliHz % 1000, m.t.pixelClockKHz, &m == mode ? " (lit)" : "",
		      Pipe2::modeKnown(m.t) ? "" : " (on the lit mode's request timing)");
	}
}

void fillHead2(RDNA4Device &dev) {
	Head2 *h = head2;
	if (!h || h->ready)
		return;
	IOService *client = h->nub->getClient();
	FBLOG("head2: framebuffer on the nub: %s", client ? client->getName() : "none started");
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
	const bool sink = dev.edid2Len && (head2Level < 2 || dev.usePlanFor(dev.edid2Hpd));
	const bool offline = head2Level >= 2 && !sink && hotplugLevel >= 2;
	const Modes::Mode *mode = nullptr;
	size_t n = 0;
	if (offline) {
		if (dev.edid2Len)
			FBLOG("head2: the display on HPD%u is not served: no second-pipe plan for that connector (a "
			      "DisplayPort one needs rdna4-head2dp=1); the head waits offline", dev.edid2Hpd);
		// The one mode the pipe will be lit in, on its surface; no EDID.
		h->table[0] = Modes::Mode { 1, lit, lit.refreshMilliHz(), true, Modes::SourceBoot };
		n = 1;
		if (Ndrv::spareSurface(dev.fbPhysBase, dev.fbLength, h->rangeBase, h->rangeLen, lit.hActive, lit.vActive,
		                       h->surface))
			mode = &h->table[0];
	} else {
		// The second sink if one answered, else a copy of the boot display's.
		const uint8_t *edid = dev.edid2Len ? dev.edid2Data : dev.edidData;
		h->edidLen = min(static_cast<size_t>(dev.edid2Len ? dev.edid2Len : dev.edidLen), sizeof(h->edid));
		if (h->edidLen < 128) {
			FBLOG("head2: nothing to serve: no EDID");
			return;
		}
		memcpy(h->edid, edid, h->edidLen);
		// Without hot-plug a head with no monitor would be the phantom, an
		// invisible display for windows to get lost on: that is rdna4-head2=1,
		// asked for by name.
		if (head2Level >= 2 && !sink) {
			if (dev.edid2Len)
				FBLOG("head2: nothing to serve: the second display is on HPD%u, a connector there is no "
				      "second-pipe plan for (a DisplayPort one needs rdna4-head2dp=1)", dev.edid2Hpd);
			else
				FBLOG("head2: nothing to serve: no second sink answered on DDC (hot-plug is off: rdna4-hotplug=2 "
				      "waits for one, rdna4-head2=1 makes a phantom head)");
			return;
		}
		mode = head2BuildTable(h, dev, n);
	}
	if (!mode) {
		if (head2Level >= 2)
			FBLOG("head2: nothing to serve: the sink has no %ux%u mode at %u kHz, the one the "
			      "second-pipe plan is for, or it does not fit behind the console", lit.hActive,
			      lit.vActive, lit.pixelClockKHz);
		else
			FBLOG("head2: nothing to serve: no mode of the sink fits behind the console");
		return;
	}
	if (!offline && head2Level >= 2 && dev.isAmd)
		dev.lightSecondPipe(head2Level, h->surface.physBase);
	head2Init(h, dev, mode, n);
	h->connected = !offline;
	h->ndrv.setConnected(h->connected);
	// WindowServer maps a framebuffer through getVRAMRange, which IONDRVFramebuffer
	// only has for an IOBootNDRV: without this it gives up on the head with
	// "Failed to map VRAM" (card boot of 2026-10-03, 19:46).
	h->vram = IODeviceMemory::withSubRange(h->range, h->surface.physBase - h->rangeBase,
	                                       static_cast<uint64_t>(h->surface.rowBytes) * h->surface.height);
	if (!h->vram)
		FBLOG("head2: no VRAM range for the surface: WindowServer will not map it");
	h->ready = true;
	FBLOG("head2: serving %ux%u@%u.%03u on surface 0x%llx, %s", mode->t.hActive,
	      mode->t.vActive, mode->refreshMilliHz / 1000, mode->refreshMilliHz % 1000, h->surface.physBase,
	      offline ? "offline until a display is plugged in" : dev.edid2Len ? "EDID of the second sink"
	              : "EDID of the boot display (no second sink)");
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
		FBLOG("hotplug: macOS registered no connect handler on head 2: it is not told");
}

// A display on the connector: read what it is and let macOS find it. For
// another display than before the pipe goes to the mode it is lit in (and is
// lit, if this is the first display it sees), which is where a new mode table
// starts.
void head2Plugged(Head2 *h, uint8_t hpd) {
	RDNA4Device &dev = *h->dev;
	if (!dev.readSecondEdid(hpd)) {
		FBLOG("hotplug: HPD%u is high but there is no EDID on DDC (yet)", hpd);
		return;
	}
	// A pipe that is not lit yet takes the plan for this connector.
	if (!dev.pipe2Lit && !dev.usePlanFor(hpd)) {
		FBLOG("hotplug: no second-pipe plan for HPD%u", hpd);
		return;
	}
	const Edid::DetailedTiming &lit = Pipe2::config().timing;
	const size_t len = min(static_cast<size_t>(dev.edid2Len), sizeof(h->edid));
	// The display that was there before: the link comes back in the mode it
	// was in, and macOS finds the head, its modes and their IDs as it left them.
	if (dev.pipe2Lit && len == h->edidLen && memcmp(h->edid, dev.edid2Data, len) == 0) {
		dev.secondPipeTo(hpd);
		if (!dev.pipe2On) {
			FBLOG("hotplug: the link did not come back: the display stays offline");
			return;
		}
		h->connected = true;
		h->ndrv.setConnected(true);
		FBLOG("hotplug: the same display is back on HPD%u", Pipe2::config().hpd);
		head2ConnectChanged(h);
		return;
	}
	h->edidLen = len;
	memcpy(h->edid, dev.edid2Data, h->edidLen);
	size_t n = 0;
	const Modes::Mode *mode = head2BuildTable(h, dev, n);
	if (!mode) {
		FBLOG("hotplug: the display has no %ux%u mode at %u kHz, the one the pipe is lit in: left off",
		      lit.hActive, lit.vActive, lit.pixelClockKHz);
		return;
	}
	if (!dev.pipe2Lit) {
		dev.lightSecondPipe(head2Level, h->surface.physBase);
	} else {
		dev.secondPipeTo(hpd);
		if (dev.pipe2On && !Edid::sameTiming(dev.pipe2Target.now, lit))
			dev.applySecondPipeMode(*mode);
	}
	if (!dev.pipe2Lit || !dev.pipe2On) {
		FBLOG("hotplug: the pipe did not come up: the display stays offline");
		return;
	}
	head2Init(h, dev, mode, n);
	h->connected = true;
	h->ndrv.setConnected(true);
	FBLOG("hotplug: display connected on HPD%u: %lu mode(s), EDID %lu bytes", Pipe2::config().hpd,
	      static_cast<unsigned long>(n), static_cast<unsigned long>(h->edidLen));
	head2ConnectChanged(h);
}

void head2Unplugged(Head2 *h) {
	h->dev->setSecondPipePower(false);
	h->connected = false;
	h->ndrv.setConnected(false);
	FBLOG("hotplug: display gone from HPD%u: link off", Pipe2::config().hpd);
	head2ConnectChanged(h);
}

constexpr uint32_t kHpdPollMs = 500;

void hotplugPoll(OSObject *, IOTimerEventSource *timer) {
	Head2 *h = head2;
	RDNA4Device &dev = *h->dev;
	// A display that is gone may come back on another connector.
	const uint8_t hpd = dev.secondSinkHpd(!h->connected);
	const bool present = hpd != 0;
	if (present != h->hpdSeen) {
		if (present)
			h->hpdPin = hpd;
		h->hpdBack = present && h->connected && h->hpdSame >= 1;
		FBLOG("hotplug: HPD%u went %s", h->hpdPin, present ? "high" : "low");
		h->hpdSeen = present;
		h->hpdSame = 0;
	} else {
		h->hpdSame++;
	}
	// Low only counts while the link is up: a monitor that macOS has put to
	// sleep may drop the pin, and takes a while to raise it again on waking.
	h->hpdGone = present || !dev.pipe2On ? 0 : h->hpdGone + 1;
	if (hotplugLevel >= 2) {
		// A second of high, and again after 3 and 7 s if the display's EDID
		// was not there yet; three seconds of low.
		if (!h->connected && present && (h->hpdSame == 2 || h->hpdSame == 6 || h->hpdSame == 14))
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

void startHead2Hotplug(IOService *fb, void (*proc)(OSObject *, void *), OSObject *target, void *ref) {
	Head2 *h = head2;
	IOWorkLoop *loop = fb->getWorkLoop();
	h->hpdTimer = loop ? IOTimerEventSource::timerEventSource(fb, hotplugPoll) : nullptr;
	if (!h->hpdTimer || loop->addEventSource(h->hpdTimer) != kIOReturnSuccess) {
		FBLOG("hotplug: no timer on head 2's work loop: off");
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
	FBLOG("hotplug: polling HPD%u (now %s), %s; head 2 is %s", h->hpdPin, h->hpdSeen ? "high" : "low",
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
	if (isHead2(fb)) {
		Head2 *h = head2;
		if (hotplugLevel && h->ready && h->dev && !h->hpdTimer && head2Level >= 2 && h->dev->isAmd)
			startHead2Hotplug(service, proc, target, ref);
		return ret;
	}
	FbEntry *e = fbEntry(fb);
	if (e && e->ours && e->state && e->state->dev.bootDpLinkKnown && !bootDp.timer)
		startBootDpPoll(service, e->state->dev);
	return ret;
}

// The stand-in for IOBootNDRV::doDriverIO.
IOReturn head2DriverIO(void *contents, UInt32 commandCode, uint16_t &code) {
	if (!head2->ready) {
		// macOS opened this head before head 0: build head 0's device, which
		// is where the EDID and the console geometry come from, now.
		IOService *fb0 = nullptr;
		if (OSIterator *clients = head2->pci->getClientIterator()) {
			while (OSObject *client = clients->getNextObject())
				if (client->metaCast("IONDRVFramebuffer")) {
					fb0 = static_cast<IOService *>(client);
					break;
				}
			clients->release();
		}
		FbEntry *e0 = fb0 ? fbEntry(fb0) : nullptr;
		if (e0 && e0->ours && !e0->tried)
			attach(*e0);
		if (!head2->ready)
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
	if (isStatus ? head2->ndrv.status(code, pb->params, r) : head2->ndrv.control(code, pb->params, r))
		return static_cast<IOReturn>(r);
	return static_cast<IOReturn>(Ndrv::bootReply(isStatus, code));
}

// IONDRVFramebuffer::getVRAMRange returns IOBootNDRV's console range, so
// nothing for head 2: hand out its surface instead (retained, as the original
// does).
IODeviceMemory *wrapGetVRAMRange(void *fb) {
	if (isHead2(fb) && head2->vram) {
		head2->vram->retain();
		return head2->vram;
	}
	return FunctionCast(wrapGetVRAMRange, orgGetVRAMRange)(fb);
}

bool wrapStart(void *fb, IOService *provider) {
	auto *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!head2 && pci && isOurDevice(pci))
		createHead2(pci);
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
	Ndrv::Backend be { &dev, deviceSurfaceFor, deviceSwitchTo, deviceSetPower,
	                   deviceSupportsHardwareCursor, deviceSetHardwareCursor,
	                   deviceDrawHardwareCursor, deviceGetHardwareCursorDrawState };
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
	fillHead2(dev);

	// E1 of docs/metal-spike.md: an IOAccelerator-shaped service that only logs what Metal asks of it. Independent of the compute bring-up.
	if (accelCensus && dev.isAmd)
		RDNA4AccelCensus::publish(pci, accelCensus);
	// M0: the fake paravirtual GPU, independent of everything else here.
	if (pvGpu && dev.isAmd)
		RDNA4PvNub::publish(pci, pvGpu);

	// Compute bring-up runs after the display is answered for, and only
	// when asked for: it must never be the reason the desktop is missing.
	if (computeStage && dev.isAmd) {
		RDNA4Compute::Env env { pci, svc, dev.mmioBase(), dev.mmioSize(), dev.discovery(),
		                        dev.fbPhysBase, dev.fbLength, dev.liveFramePeriodNs(),
		                        createVblankService, deviceCursorProbe, &dev };
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
	if (isHead2(fb)) {
		uint16_t code = 0;
		const IOReturn ret = head2DriverIO(contents, commandCode, code);
		const bool lifecycle = commandCode != kIONDRVControlCommand && commandCode != kIONDRVStatusCommand;
		const bool traced = !lifecycle && traceEnabled && head2->traceBudget;
		if (traced)
			head2->traceBudget--;
		if (lifecycle || traced)
			FBLOG("head2: %s csc %u -> 0x%x", commandName(commandCode), code, ret);
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
	// The second display and its hot-plug are on unless turned off: lit at
	// boot, mode switching, display sleep and hot-plug ran on the card for an
	// HDMI monitor on either connector (2026-10-04..06). Not in VM test
	// builds, which have no second pipe to light.
#ifdef RDNA4FB_VM_TEST
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

void pluginStart() {
	uint32_t off = 0;
	if (PE_parse_boot_argn("rdna4-off", &off, sizeof(off)) && off) {
		FBLOG("disabled by rdna4-off boot-arg");
		return;
	}
	uint32_t trace = 0;
	const bool traceArg = PE_parse_boot_argn("rdna4-trace", &trace, sizeof(trace));
	rdna4TraceLevel = traceArg ? trace : 0;
	traceEnabled = rdna4TraceLevel != 0;
#ifdef RDNA4FB_VM_TEST
	// VM test builds trace by default (OpenCore images often pin boot-args).
	if (!traceArg) {
		rdna4TraceLevel = 1;
		traceEnabled = true;
	}
#endif
	computeStage = RDNA4Compute::requestedStage();
	if (!PE_parse_boot_argn("rdna4-accelcensus", &accelCensus, sizeof(accelCensus)) || accelCensus > 2)
		accelCensus = 0;
	if (!PE_parse_boot_argn("rdna4-pvgpu", &pvGpu, sizeof(pvGpu)) || pvGpu > 2)
		pvGpu = 0;
	if (pvGpu)
		RDNA4PvNub::publishLater(pvGpu, 25000);   // a VM without the emulated RDNA4 device never reaches attach()
	Ndrv::vslInit();
	FBLOG("Lilu plugin started (trace %s, compute stage %u%s%s)", traceEnabled ? "on" : "off",
	      computeStage, accelCensus ? ", ACCEL CENSUS (emulator only)" : "", pvGpu ? ", FAKE PARAVIRT GPU (VM only)" : "");
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
