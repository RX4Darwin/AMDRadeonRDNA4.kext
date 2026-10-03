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
	bool             ready { false };       // fillHead2 set the mode, surface and EDID
	Ndrv::Translator ndrv;
	Ndrv::Surface    surface {};
	Modes::Mode      table[Modes::MaxModes] {};
	uint8_t          edid[128] {};
	uint32_t         traceBudget { 400 };   // its own: head 0 must not use up the lines
};
Head2 *head2 { nullptr };
mach_vm_address_t orgStart { 0 };

void attach(FbEntry &e);

bool isHead2(void *fb) {
	return head2 && static_cast<IOService *>(fb)->getProvider() == head2->nub;
}

bool head2SurfaceFor(void *ctx, const Modes::Mode &, bool, Ndrv::Surface &out) {
	out = static_cast<Head2 *>(ctx)->surface;
	return true;
}

// From wrapStart, before IONDRVFramebuffer starts on the PCI device.
void createHead2(IOPCIDevice *pci) {
	if (computeStage) {   // the compute pool takes the VRAM behind the console
		FBLOG("head2: not created: rdna4-compute is set");
		return;
	}
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
void fillHead2(const RDNA4Device &dev) {
	Head2 *h = head2;
	if (!h || h->ready)
		return;
	IOService *client = h->nub->getClient();
	FBLOG("head2: framebuffer on the nub: %s", client ? client->getName() : "none started");
	if (!dev.edid2Len && dev.edidLen < 128) {
		FBLOG("head2: nothing to serve: no EDID");
		return;
	}
	// The device memory range that holds the console (BAR0 on the card).
	uint64_t rangeBase = 0, rangeLen = 0;
	for (UInt32 i = 0; i < h->pci->getDeviceMemoryCount(); i++) {
		IODeviceMemory *mem = h->pci->getDeviceMemoryWithIndex(i);
		if (!mem)
			continue;
		const uint64_t base = mem->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
		if (dev.fbPhysBase >= base && dev.fbPhysBase - base < mem->getLength()) {
			rangeBase = base;
			rangeLen = mem->getLength();
			break;
		}
	}
	// The second sink if one answered, else a copy of the boot display's.
	memcpy(h->edid, dev.edid2Len ? dev.edid2Data : dev.edidData, sizeof(h->edid));
	// The first mode, in table order (native first), whose surface fits.
	const size_t n = Modes::build(h->edid, sizeof(h->edid), Modes::Limits {}, h->table, Modes::MaxModes);
	const Modes::Mode *mode = nullptr;
	for (size_t i = 0; i < n && !mode; i++)
		if (Ndrv::spareSurface(dev.fbPhysBase, dev.fbLength, rangeBase, rangeLen,
		                       h->table[i].t.hActive, h->table[i].t.vActive, h->surface))
			mode = &h->table[i];
	if (!mode) {
		FBLOG("head2: nothing to serve: no mode of the sink fits behind the console");
		return;
	}
	Ndrv::Backend be {};
	be.ctx = h;
	be.surfaceFor = head2SurfaceFor;
	h->ndrv.init(mode, 1, mode->id, h->edid, sizeof(h->edid), be);
	h->ready = true;
	FBLOG("head2: serving %ux%u@%u.%03u on surface 0x%llx, EDID of %s", mode->t.hActive,
	      mode->t.vActive, mode->refreshMilliHz / 1000, mode->refreshMilliHz % 1000,
	      h->surface.physBase, dev.edid2Len ? "the second sink" : "the boot display (no second sink)");
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
	uint32_t second = 0;
	if (PE_parse_boot_argn("rdna4-head2", &second, sizeof(second)) && second) {
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
