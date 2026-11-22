//
//  rdna4accel.cpp — our own IOGraphicsAccelerator2 (docs/native-driver-plan.md, option D), milestone 2: the family's own start.
//
//  Our kext is in the boot kernel collection; IOAcceleratorFamily2 is in the system one, which a boot-collection kext cannot link
//  to. So the base class is declared from the KDK (src/ioaccel.hpp, tools/gen/accelgen.py) and every base function our class reaches
//  is a thunk (src/ioaccel_thunks.S) through rdna4AccelThunks, filled here when the family loads. Before anything is made:
//  - every thunk's function was found;
//  - each virtual's function sits in the running vtable at the KDK's slot (else the KDK's layout is not this macOS's);
//  - the running class has the KDK's size;
//  - our metaclass's superclass link and size are where OSMetaClass keeps them (+16, +32), then the link is set to the real class.
//
//  debug.rdna4_accel=1 makes one and attaches it to the GPU (RDNA4AccelCensus::gpu); 2 starts it: the family's start, which asks for
//  an event machine, a display machine and a kernel task (our subclasses of the family's, stubs), no user clients yet. Milestone 1
//  (IOService::start only) passed on the card on 2026-11-20.
//
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/OSKextLib.h>
#include <sys/sysctl.h>

#include <IOKit/graphics/IOFramebuffer.h>

#include "accelcensus.hpp"
#ifdef AMDRadeonRDNA4_PVGPU
#include "pvgpu.hpp"
#include "pvhooks.hpp"
#include "translator.hpp"
#include "iosurface.hpp"
#include "pvscanout.hpp"
#endif
#include "rdna4compute.h"
#include "trial.hpp"
#include "userclient.hpp"
#include "ioaccel.hpp"
#include "klog.hpp"
#include "rdna4accel.hpp"


extern "C" uintptr_t rdna4AccelThunks[];
extern "C" uintptr_t rdna4AccelMissing()
{
	rdna4Log("AMDRadeonRDNA4: accel: a base function that was not found was called (returned 0)\n");
	return 0;
}

namespace {
const struct {
	uint32_t kext;   // its index in kKexts
	const char *name;
} kSymbols[] = {
#include "ioaccel_symbols.inc"
};
constexpr size_t kThunks = sizeof(kSymbols) / sizeof(kSymbols[0]);
const struct {
	uint32_t thunk;
	const char *vtable;
	uint32_t slot;
} kSlots[] = {
#include "ioaccel_slots.inc"
};

// The kexts whose classes we derive from (ioaccel_kexts.inc), each solved when it loads (familyLoaded)
struct KextName {
	const char *bundle;
	const char *path;
};
const KextName kKexts[] = {
#include "ioaccel_kexts.inc"
};
constexpr size_t kKextCount = sizeof(kKexts) / sizeof(kKexts[0]);
const char *gKextPath[kKextCount];
KernelPatcher::KextInfo gKextInfo[kKextCount];
uint32_t gSolved, gFailed;   // bit k: kKexts[k] solved and checked / found wrong

bool gReady;   // every kext's thunks filled and checked
}   // namespace

// OSDefineMetaClassAndStructors(cls, base) written out, without the class's superClass pointer: OpenCore's linker patches every class
// that has one against its superclass's vtable, which must be one it linked itself; IOAcceleratorFamily2's are not (not in the boot
// collection), so it refused the whole kext (the rig booted without it, 2026-11-20). The MetaClass gets the placeholder as its
// superclass here and the real one in familyLoaded (kDerived).
#define RDNA4_ACCEL_STRUCTORS(cls, base)                                                                                                  \
	cls::MetaClass cls::gMetaClass;                                                                                                      \
	const OSMetaClass *const cls::metaClass = &cls::gMetaClass;                                                                          \
	cls::cls(const OSMetaClass *meta) : base(meta) {}                                                                                    \
	cls::~cls() {}                                                                                                                       \
	const OSMetaClass *cls::getMetaClass() const { return &gMetaClass; }                                                                 \
	cls::MetaClass::MetaClass() : OSMetaClass(#cls, &base::gMetaClass, sizeof(cls)) {}                                                   \
	OSObject *cls::MetaClass::alloc() const { return new cls; }                                                                          \
	cls::cls() : base(&gMetaClass) { gMetaClass.instanceConstructed(); }

// The family classes' key functions (src/ioaccel.hpp): their vtables are emitted here, for Base::f() calls. No base object is made here.
#define RDNA4_ACCEL_BASE(cls) \
	const OSMetaClass *cls::getMetaClass() const { return nullptr; }
RDNA4_ACCEL_BASE(IOGraphicsAccelerator2)
RDNA4_ACCEL_BASE(IOAccelTask)
RDNA4_ACCEL_BASE(IOAccelMemoryMap)
RDNA4_ACCEL_BASE(IOAccelEventMachine2)
RDNA4_ACCEL_BASE(IOAccelEventMachineFast2)
RDNA4_ACCEL_BASE(IOAccelDisplayMachine)
RDNA4_ACCEL_BASE(IOAccelResource2)
RDNA4_ACCEL_BASE(IOAccelMemory)
RDNA4_ACCEL_BASE(IOAccelSysMemory)
RDNA4_ACCEL_BASE(IOAccelVidMemory)
RDNA4_ACCEL_BASE(IOAccelDisplayPipe)
RDNA4_ACCEL_BASE(AppleGraphicsDeviceControl)

// The family objects the family's start asks for. The event machine's four (its stamps and barriers in command buffers) and the
// memory map's three (the GPU page table) are for submissions through the family, which nothing makes yet: they do nothing.
class RDNA4EventMachine : public IOAccelEventMachineFast2 {
	OSDeclareDefaultStructors(RDNA4EventMachine)
	using super = IOAccelEventMachineFast2;
	void *stamps = nullptr;   // the stamps' memory (never freed: the machine lives as long as the accelerator)

public:
	// The family's init, then the stamps' memory: as AMD's, setStampBaseAddress (each stamp's address from it); without it the family's
	// GPU progress check read through a null array 17 s into a boot with our accelerator (2026-11-20). Zeroed: nothing completes yet.
	// ponytail: 64 KiB, far more than the stamps' offsets reach; IOMallocZero, not shared with user space (getStampMemory: none)
	bool init(IOGraphicsAccelerator2 *accel, unsigned int count, int flags) APPLE_KEXT_OVERRIDE
	{
		if (!super::init(accel, count, flags))
			return false;
		if (!(stamps = IOMallocZero(kStampBytes)))
			return false;
		setStampBaseAddress(static_cast<volatile unsigned int *>(stamps));
		rdna4Log("AMDRadeonRDNA4: accel: event machine: %u stamps at %p\n", count, stamps);
		return true;
	}
	static constexpr size_t kStampBytes = 64 * 1024;

	// Every event complete: no GPU work goes through the family's command path (WindowServer's Metal work runs on the card's runtime,
	// RDNA4MTLDevice), so no stamp is ever written and a display pipe transaction's barrier event never completed: the family queued
	// WindowServer's transactions and never performed them (event_interrupt_gated, 2026-11-21). The first 24 are logged, complete by
	// their stamps or not (8 x {stamp index, -1 none; value}). ponytail: real stamps once the family's command path carries work.
	uintptr_t testEventUnlocked(IOAccelEvent *event) APPLE_KEXT_OVERRIDE
	{
		static uint32_t logged;
		const bool done = super::testEventUnlocked(event);
		if (logged < 24) {
			logged++;
			const auto *e = reinterpret_cast<const int32_t *>(event);
			rdna4Log("AMDRadeonRDNA4: accel: event %p %s by its stamps: %d:%u %d:%u %d:%u %d:%u (taken as complete)\n", event,
			         done ? "complete" : "not complete", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
		}
		return true;
	}

	uintptr_t writeStamp(int, vendevtCommandRec *, unsigned int) APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t prepareBarrier(vendevtBarrierRec *) APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t completeBarrier(vendevtBarrierRec *) APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t writeBarrierElement(vendevtBarrierRec *, int, unsigned int) APPLE_KEXT_OVERRIDE { return 0; }
};
RDNA4_ACCEL_STRUCTORS(RDNA4EventMachine, IOAccelEventMachineFast2)

class RDNA4DisplayMachine : public IOAccelDisplayMachine {
	OSDeclareDefaultStructors(RDNA4DisplayMachine)
public:
	// true: false makes the family panic ("display_mode_did_change: driver returns false", WindowServer setting a mode on an IONDRV
	// framebuffer with our pipe, twice on 2026-11-20)
	uintptr_t displayModeWillChange() APPLE_KEXT_OVERRIDE { return true; }
	uintptr_t displayModeDidChange() APPLE_KEXT_OVERRIDE { return true; }
	// (diagnostic) the framebuffer's current mode and its pixel information, as the family's init_framebuffer_resource asks them:
	// "getPixelInformation for framebuffer 1/2/5 failed" left those pipes inactive (transactionEnd: kIOReturnNotReady), 2026-11-21
	uintptr_t initFramebufferResource(unsigned int a, unsigned int b, IOFramebuffer *fb, IOAccelResource2 *r) APPLE_KEXT_OVERRIDE
	{
		static uint32_t logged;
		if (fb && logged++ < 24) {
			IODisplayModeID mode = 0;
			IOIndex depth = 0;
			IOPixelInformation info {};
			const IOReturn cur = fb->getCurrentDisplayMode(&mode, &depth);
			const IOReturn pix = cur ? cur : fb->getPixelInformation(mode, depth, kIOFBSystemAperture, &info);
			rdna4Log("AMDRadeonRDNA4: accel: display machine: framebuffer %u/%u %s 0x%llx: current mode 0x%x depth %d (0x%x), pixel info 0x%x:"
			         " %ux%u, %u bpp, %u bytes a row\n", a, b, fb->getName(), fb->getRegistryEntryID(), static_cast<unsigned>(mode),
			         static_cast<int>(depth), cur, pix, static_cast<unsigned>(info.activeWidth), static_cast<unsigned>(info.activeHeight),
			         static_cast<unsigned>(info.bitsPerPixel), static_cast<unsigned>(info.bytesPerRow));
		}
		return super::initFramebufferResource(a, b, fb, r);
	}
	// The framebuffers with a monitor first: each framebuffer found gets the next pipe index (its IOAccelIndex), and IOPresentment takes
	// a GPU's pipes as contiguous: with the lit ones at 0 and 3 it presented the second display on pipe 1, a bare framebuffer's, never
	// active (kIOReturnNotReady, 2026-11-21). Bare ones still get a pipe after them: with none, the boot display's framebuffer never got
	// a mode or its display (two test boots). (The family skips a framebuffer that already has a pipe.)
	uintptr_t found_framebuffer(IOFramebuffer *fb) APPLE_KEXT_OVERRIDE
	{
		if (!litFirst) {
			litFirst = true;
			IOService *gpu = RDNA4AccelCensus::gpu();
			IORegistryIterator *it = gpu ? IORegistryIterator::iterateOver(gpu, gIOServicePlane, kIORegistryIterateRecursively) : nullptr;
			while (IORegistryEntry *e = it ? it->getNextObject() : nullptr)
				if (e->metaCast("IOFramebuffer") && RDNA4Accel::showsFramebuffer(e)) {
					rdna4Log("AMDRadeonRDNA4: accel: display machine: framebuffer %s 0x%llx (lit) first\n", e->getName(), e->getRegistryEntryID());
					super::found_framebuffer(static_cast<IOFramebuffer *>(e));
				}
			OSSafeReleaseNULL(it);
		}
		return super::found_framebuffer(fb);
	}
	bool litFirst = false;
	using super = IOAccelDisplayMachine;
};
RDNA4_ACCEL_STRUCTORS(RDNA4DisplayMachine, IOAccelDisplayMachine)

class RDNA4Task : public IOAccelTask {
	OSDeclareDefaultStructors(RDNA4Task)
};
RDNA4_ACCEL_STRUCTORS(RDNA4Task, IOAccelTask)

class RDNA4MemoryMap : public IOAccelMemoryMap {
	OSDeclareDefaultStructors(RDNA4MemoryMap)
public:
	// bools, true when done (AMD's): nothing to put in a GPU page table yet (the display path scans out through the kext's own code);
	// false failed every framebuffer resource's prepare
	uintptr_t commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE { return true; }
	uintptr_t releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE { return true; }
	uintptr_t updateGPUPageTable() APPLE_KEXT_OVERRIDE { return true; }
};
RDNA4_ACCEL_STRUCTORS(RDNA4MemoryMap, IOAccelMemoryMap)

// A resource: the family's header for a surface or buffer (a display pipe asks for one for its framebuffer). Its memory, apertures and
// paging buffer are not ours yet: nothing.
class RDNA4Resource : public IOAccelResource2 {
	OSDeclareDefaultStructors(RDNA4Resource)
	using super = IOAccelResource2;
	uint32_t unplaced = 0;   // prepares the family could not place (below), each one undone by a complete
	bool unplaceable = false;   // the family could not place it once: it is not asked again
public:
	// Prepared even when the family cannot place it: WindowServer's IOSurfaces are VRAM resources whose memory has no size (we report
	// no device-cache VRAM), IOAccelTask::allocate cannot map 0 bytes ("failed to allocate GPU virtual address"), and every display pipe
	// transaction carried 0xe00002be and was never performed (2026-11-21). Nothing reads a GPU address of ours: the pipe presents the
	// plane by its IOSurface (performTransaction). Asked once: the size never changes, and each refusal logged two kernel lines per
	// display per frame (240 a second on the rig's two displays, 2026-11-22), which pushed everything else out of the kernel's log.
	// ponytail: real VRAM placement once the family's command path draws.
	uintptr_t prepare() APPLE_KEXT_OVERRIDE
	{
		if (unplaceable || !super::prepare()) {
			unplaceable = true;
			unplaced++;
		}
		return true;
	}
	uintptr_t complete() APPLE_KEXT_OVERRIDE
	{
		if (unplaced) {
			unplaced--;
			return 0;
		}
		return super::complete();
	}
	uintptr_t rebuildPagingBuffer() APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t getLevelOffset(unsigned char, unsigned char, int *offset) APPLE_KEXT_OVERRIDE { if (offset) *offset = 0; return 0; }
	uintptr_t getBackingLevelOffset(unsigned char, unsigned char, int *offset) APPLE_KEXT_OVERRIDE { if (offset) *offset = 0; return 0; }
	uintptr_t calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long *a, unsigned long long *b) APPLE_KEXT_OVERRIDE
	{
		if (a)
			*a = 0;
		if (b)
			*b = 0;
		return 0;
	}
	uintptr_t addToAperture() APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t removeFromAperture() APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t getApertureMemoryDescriptor(unsigned long long *) APPLE_KEXT_OVERRIDE { return 0; }
};
RDNA4_ACCEL_STRUCTORS(RDNA4Resource, IOAccelResource2)

// System memory (an app's, an IOSurface's): the family's own handles all of it.
class RDNA4SysMemory : public IOAccelSysMemory {
	OSDeclareDefaultStructors(RDNA4SysMemory)
};
RDNA4_ACCEL_STRUCTORS(RDNA4SysMemory, IOAccelSysMemory)

// Video memory: its physical pages are ours to give (the runtime's VRAM). Not yet: none.
class RDNA4VidMemory : public IOAccelVidMemory {
	OSDeclareDefaultStructors(RDNA4VidMemory)
public:
	uintptr_t getPhysicalSegment(unsigned long long, unsigned long long *length) APPLE_KEXT_OVERRIDE
	{
		if (length)
			*length = 0;
		return 0;
	}
	uintptr_t allocPhysical() APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t deallocPhysical() APPLE_KEXT_OVERRIDE { return 0; }
};
RDNA4_ACCEL_STRUCTORS(RDNA4VidMemory, IOAccelVidMemory)

// An accelerated surface: user client type 0 (the family's newUserClient asks newSurface; null was kIOReturnNoResources). WindowServer
// opens one for each display's surfaces: refused, its displays were composited in software (CompositorSW) and Core Animation's flatten
// path asserted (2026-11-21). The family's surface does the rest; the copies and swaps a driver does on the GPU are refused for now.
class RDNA4Surface : public IOAccelSurface {
	OSDeclareDefaultStructors(RDNA4Surface)
public:
	uintptr_t copyFromBuffer(int, int, int, int, unsigned int, unsigned int, IOAccelEvent *, IOAccelResource2 *, IOAccelSysMemory *, unsigned int,
	                         unsigned long long, unsigned int) APPLE_KEXT_OVERRIDE { return note("copyFromBuffer"); }
	uintptr_t copyToBuffer(int, int, int, int, unsigned int, unsigned int, IOAccelEvent *, IOAccelResource2 *, IOAccelSysMemory *, unsigned int,
	                       unsigned long long, unsigned int) APPLE_KEXT_OVERRIDE { return note("copyToBuffer"); }
	uintptr_t submitSwapCopy(IOAccelEvent *, IOAccelResource2 *, IOAccelResource2 *) APPLE_KEXT_OVERRIDE { return note("submitSwapCopy"); }
	uintptr_t shapeSurface(unsigned int, unsigned short, unsigned short) APPLE_KEXT_OVERRIDE { return note("shapeSurface"); }
	uintptr_t getDirtyBufferBitsFromPrivateModeBits(unsigned long long) APPLE_KEXT_OVERRIDE { return 0; }

private:
	uintptr_t note(const char *what)
	{
		static uint32_t logged;
		if (logged++ < 16)
			rdna4Log("AMDRadeonRDNA4: accel: surface %p: %s (refused)\n", this, what);
		return kIOReturnUnsupported;
	}
};
RDNA4_ACCEL_STRUCTORS(RDNA4Surface, IOAccelSurface)

// A 2D context: user client type 2 (new2DContext), what the IOAccelerator2D plug-in (CoreDisplay's IOPSAllocateBlitEngine, through
// the accelerator's IOCFPlugInTypes) opens for a display's blit engine; Apple's paravirtual driver keeps two in WindowServer. The blits
// themselves are refused for now (logged).
class RDNA4Context2D : public IOAccel2DContext2 {
	OSDeclareDefaultStructors(RDNA4Context2D)
public:
	uintptr_t blitCopy(IOAccelEvent *, IOAccelResource2 *, IOAccelResource2 *, IOAccel2DBlitRectStruc *, unsigned int n) APPLE_KEXT_OVERRIDE
	{
		return note("blitCopy", n);
	}
	uintptr_t blitFill(IOAccelEvent *, unsigned int, IOAccelResource2 *, IOAccel2DBlitRectStruc *, unsigned int n) APPLE_KEXT_OVERRIDE
	{
		return note("blitFill", n);
	}

private:
	uintptr_t note(const char *what, unsigned int rects)
	{
		static uint32_t logged;
		if (logged++ < 16)
			rdna4Log("AMDRadeonRDNA4: accel: 2D context %p: %s of %u rect(s) (refused)\n", this, what, rects);
		return kIOReturnUnsupported;
	}
};
RDNA4_ACCEL_STRUCTORS(RDNA4Context2D, IOAccel2DContext2)

// A display pipe (one a framebuffer). For now the family's own behaviour, each transaction hook logged (the first few): what
// WindowServer's presents ask of a driver (AMD's pipe overrides these), before they go to the DCN.
// IOAccelSysMemory::withMemoryDescriptor (static: no thunk), solved with the family (familyLoaded); our accelerator (below gAccel)
mach_vm_address_t gWithMemoryDescriptor;
mach_vm_address_t gPlaneIOSurface;   // IOAccelDisplayPipeTransaction2::getPlaneIOSurface(unsigned, unsigned) const (not virtual)
IOGraphicsAccelerator2 *ourAccelerator();
const uint8_t *gPipes[8];   // (diagnostic, debug.rdna4_accelpipes) our display pipes by framebuffer index (init)

class RDNA4DisplayPipe : public IOAccelDisplayPipe {
	OSDeclareDefaultStructors(RDNA4DisplayPipe)
	using super = IOAccelDisplayPipe;
	uint32_t logged = 0;
	// our framebuffer with the family's index `index` (its IOAccelIndex), retained, or null
	static IOFramebuffer *framebufferOf(uint32_t index)
	{
		IOService *gpu = RDNA4AccelCensus::gpu();
		IOFramebuffer *found = nullptr;
		IORegistryIterator *it = gpu ? IORegistryIterator::iterateOver(gpu, gIOServicePlane, kIORegistryIterateRecursively) : nullptr;
		while (IORegistryEntry *e = it && !found ? it->getNextObject() : nullptr) {
			auto *fb = e->metaCast("IOFramebuffer") ? static_cast<IOFramebuffer *>(e) : nullptr;   // (by name: IOGraphicsFamily is not linked)
			auto *n = fb ? OSDynamicCast(OSNumber, fb->getProperty("IOAccelIndex")) : nullptr;
			if (n && n->unsigned32BitValue() == index) {
				fb->retain();
				found = fb;
			}
		}
		OSSafeReleaseNULL(it);
		return found;
	}
	bool note(const char *what, const void *arg)
	{
		if (logged >= 64)
			return false;
		logged++;
		rdna4Log("AMDRadeonRDNA4: accel: pipe %p: %s(%p)\n", this, what, arg);
		return true;
	}

public:
	bool init(IOGraphicsAccelerator2 *accel, IOAccelDisplayMachine *machine, IOFramebuffer *fb, unsigned int index) APPLE_KEXT_OVERRIDE
	{
		if (!super::init(accel, machine, fb, index))
			return false;
		if (index < 8)
			gPipes[index] = reinterpret_cast<const uint8_t *>(this);
		return true;
	}
	uintptr_t validateTransaction(IOAccelDisplayPipeTransaction2 *t) APPLE_KEXT_OVERRIDE
	{
		note("validateTransaction", t);
		return super::validateTransaction(t);
	}
	// A transaction's first plane onto the pipe's head as the paravirtual displays' frames go (pvPresent: copied by the card into a
	// buffer the head flips to at vblank, else by the CPU). ponytail: plane 0 only, no rects.
	// The framebuffer powering off (display sleep): the family marks the pipe offline (+0x299), and transactionEnd answers every
	// transaction of WindowServer's kIOReturnOffline. CoreDisplay retried them (~1.4 a second) and never finished its display sleep:
	// WindowServer deferred its sleep ack (30 s) and IOGraphics' capability change waited for it (90 s), at every system sleep,
	// 2026-11-22. Taken instead and dropped (performTransaction) until the framebuffer is on again.
	bool displayOff = false;
	uintptr_t framebuffer_will_power_off() APPLE_KEXT_OVERRIDE
	{
		const uintptr_t r = super::framebuffer_will_power_off();
		displayOff = true;
		reinterpret_cast<uint8_t *>(this)[0x299] = 0;
		return r;
	}
	uintptr_t framebuffer_did_power_on() APPLE_KEXT_OVERRIDE
	{
		displayOff = false;
		return super::framebuffer_did_power_on();
	}
	uintptr_t performTransaction(IOAccelDisplayPipeTransaction2 *t) APPLE_KEXT_OVERRIDE
	{
		if (displayOff)
			return kIOReturnSuccess;   // nothing reaches a head that is off (and during a system sleep the card is going away)
		const uintptr_t r = super::performTransaction(t);
#ifdef AMDRadeonRDNA4_PVGPU
		using PlaneSurface = const void *(*)(const IOAccelDisplayPipeTransaction2 *, unsigned int, unsigned int);
		const void *surface = t && gPlaneIOSurface ? reinterpret_cast<PlaneSurface>(gPlaneIOSurface)(t, 0, 0) : nullptr;
		const uint32_t sid = surfaceIdOf(surface);
		if (port < 0) {   // (once: the pipe's framebuffer does not change)
			const uint32_t index = *reinterpret_cast<const uint32_t *>(reinterpret_cast<const uint8_t *>(this) + 0x284);
			if (IOFramebuffer *fb = framebufferOf(index)) {
				port = PvScanout::portOf(fb);
				fb->release();
			}
		}
		const bool shown = sid && port >= 0 && pvPresent(static_cast<uint32_t>(port), sid);
		static uint32_t presentLogged;   // (its own budget: the other hooks use up `logged`)
		if (presentLogged < 32 && (presented < 4 || !shown)) {
			presentLogged++;
			rdna4Log("AMDRadeonRDNA4: accel: pipe %p: transaction %p: IOSurface %u onto port %d: %s\n", this, t, sid, port,
			         shown ? "shown" : "NOT shown");
		}
		presented += shown;
		if (shown)
			return kIOReturnSuccess;   // (the family's own performTransaction is kIOReturnUnsupported: every transaction failed)
#endif
		return r;
	}
	int port = -1;          // the head this pipe's framebuffer is (PvScanout::portOf), once known
	uint32_t presented = 0;
	// Done once performed: the copy onto the head is (performTransaction). The family's waits for a later transaction to go live, so
	// the last one stayed pending: its 300 ms timeout. ponytail: done before the scanout shows it; the head's vblank later.
	uintptr_t isTransactionComplete(IOAccelDisplayPipeTransaction2 *) APPLE_KEXT_OVERRIDE { return true; }
	// (diagnostic) what event_interrupt_gated decides on, logged the first 48 times: the transaction's status (+0x58, given before
	// submit; pending 0xe0014042 is the only one performed), the queue (+0x238 head, +0x23c tail), the live one (+0x248: while set,
	// nothing more is performed), event interrupt armed (+0x29c), transaction interrupt (+0x2a4), +0x280/+0x299 (refuse performing)
	uintptr_t submitTransaction(IOAccelDisplayPipeTransaction2 *t) APPLE_KEXT_OVERRIDE
	{
		const uint32_t before = t ? *reinterpret_cast<const uint32_t *>(reinterpret_cast<const uint8_t *>(t) + 0x58) : 0;
		const uintptr_t r = super::submitTransaction(t);
		static uint32_t submitLogged;
		if (submitLogged < 48) {
			submitLogged++;
			const auto *b = reinterpret_cast<const uint8_t *>(this);
			rdna4Log("AMDRadeonRDNA4: accel: pipe %p: submit %p: status 0x%x -> 0x%lx; queue %d..%d live %p armed %u tint %u +0x280 %u +0x299 %u\n",
			         this, t, before, r, *reinterpret_cast<const int32_t *>(b + 0x238), *reinterpret_cast<const int32_t *>(b + 0x23c),
			         *reinterpret_cast<void *const *>(b + 0x248), *reinterpret_cast<const uint32_t *>(b + 0x29c),
			         *reinterpret_cast<const uint32_t *>(b + 0x2a4), b[0x280], b[0x299]);
		}
		return r;
	}
	uintptr_t beginTransaction(IOAccelEvent *e) APPLE_KEXT_OVERRIDE
	{
		note("beginTransaction", e);
		return super::beginTransaction(e);
	}
	// (diagnostic) the family clears the pipe's active flag after this: a pipe no longer active refuses WindowServer's transactions
	uintptr_t destroyFramebufferResource(unsigned int i, IOAccelResource2 *r) APPLE_KEXT_OVERRIDE
	{
		const auto *b = reinterpret_cast<const uint8_t *>(this);
		if (logged < 64) {
			logged++;
			rdna4Log("AMDRadeonRDNA4: accel: pipe %p (framebuffer %u): destroyFramebufferResource(%u, %p): was active %u\n", this,
			         *reinterpret_cast<const uint32_t *>(b + 0x284), i, r, b[0x298]);
		}
		return super::destroyFramebufferResource(i, r);
	}
	// The memory a framebuffer resource is: the pipe's framebuffer's scanout (its system aperture), as Apple's paravirtual pipe gives its
	// framebuffer's memory descriptor (IOAccelSysMemory::withMemoryDescriptor). The family's own returns kIOReturnUnsupported: no pipe of
	// ours was ever active, and transactionEnd refused every one of WindowServer's transactions (kIOReturnNotReady), 2026-11-21.
	uintptr_t initFramebufferResource(unsigned int i, IOAccelResource2 *r) APPLE_KEXT_OVERRIDE
	{
		const uint32_t index = *reinterpret_cast<const uint32_t *>(reinterpret_cast<const uint8_t *>(this) + 0x284);   // getFramebufferIndex
		IOFramebuffer *fb = framebufferOf(index);
		IODeviceMemory *mem = fb ? fb->getApertureRange(kIOFBSystemAperture) : nullptr;
		using WithMemory = uintptr_t (*)(IOGraphicsAccelerator2 *, IOAccelResource2 *, IOMemoryDescriptor *);
		const uintptr_t ok = mem && gWithMemoryDescriptor && ourAccelerator()
		                     ? reinterpret_cast<WithMemory>(gWithMemoryDescriptor)(ourAccelerator(), r, mem) : 0;
		// ponytail: the aperture's descriptor is kept (one a mode change): Apple's pipe hands over one its framebuffer keeps, so
		// whether withMemoryDescriptor retains it is not known; released here it could be freed under the resource
		if (!ok)
			OSSafeReleaseNULL(mem);
		OSSafeReleaseNULL(fb);
		const auto *b = reinterpret_cast<const uint8_t *>(this);   // (diagnostic) what transactionEnd tests: active +0x298, +0x282, +0x280
		if (logged < 64) {
			logged++;
			rdna4Log("AMDRadeonRDNA4: accel: pipe %p (framebuffer %u): initFramebufferResource(%u) %s: active %u, +0x280 %u +0x282 %u +0x299 %u\n",
			         this, *reinterpret_cast<const uint32_t *>(b + 0x284), i, ok ? "ok" : "FAILED", b[0x298], b[0x280], b[0x282], b[0x299]);
		}
		return ok;
	}
};
RDNA4_ACCEL_STRUCTORS(RDNA4DisplayPipe, IOAccelDisplayPipe)

// The GPU's AGDC service (AppleGraphicsControl's AppleGraphicsDeviceControl): IOPresentment maps GPUs, framebuffers and accelerators
// from these (ioPresentmentCreateAssociationMap), asking each for its vendor information and GPU capabilities; without one for our GPU
// it made no display pipes ("Unable to get AGDC information", "Unable to locate service dependencies"). Answered as Apple's paravirtual
// GPU's (AppleParavirtGPUControl::vendor_doDeviceAttribute): attribute 1, vendor information (0x2c bytes), 0x921, the GPU's
// capabilities (0xb0 bytes, its values); the rest (3: it makes its own framebuffers; 0x980; the framebuffers' own) not ours: refused.
namespace {
uint32_t accelFlags();   // (diagnostic) our accelerator's flags (+0xc78), 0 without one: defined below gAccel
}

class RDNA4DeviceControl : public AppleGraphicsDeviceControl {
	OSDeclareDefaultStructors(RDNA4DeviceControl)

	// Attribute 0x711, 0x196c bytes {u32 framebuffer (0-based, its place in 0x980's list), u32 query, answer from +8}: what CoreDisplay
	// makes its display's devices of (without it: "capabilities with no devices" and an assert). As the paravirtual framebuffer answers
	// (AppleParavirtFramebuffer::doDeviceAttribute): 1, 2, 4, 8 (cases of a table), 0x10 the mode (width, height), the rest flags.
	IOReturn framebufferQuery(const uint32_t *in, uint8_t *o)
	{
		auto u32 = [o](uint32_t at, uint32_t v) { *reinterpret_cast<uint32_t *>(o + at) = v; };
		auto u64 = [o](uint32_t at, uint64_t v) { *reinterpret_cast<uint64_t *>(o + at) = v; };
		IORegistryEntry *fb[20];
		const uint32_t n = framebuffers(fb, 20);
		uint32_t width = 0, height = 0, port = 0;
		if (in[0] < n) {   // its mode: its head's (the dependent index is the head), else the console's
			if (auto *dep = OSDynamicCast(OSNumber, fb[in[0]]->getProperty("IOFBDependentIndex")))
				port = dep->unsigned32BitValue();
#ifdef AMDRadeonRDNA4_PVGPU
			(void)PvScanout::mode(port, width, height);
#endif
			if (!width || !height) {
				auto *w = OSDynamicCast(OSNumber, fb[in[0]]->getProperty("Console,Width"));
				auto *h = OSDynamicCast(OSNumber, fb[in[0]]->getProperty("Console,Height"));
				width = w ? w->unsigned32BitValue() : 1920;
				height = h ? h->unsigned32BitValue() : 1080;
			}
		}
		for (uint32_t i = 0; i < n; i++)
			fb[i]->release();
		if (in[0] >= n)
			return kIOReturnBadArgument;
		switch (in[1]) {
		case 1:
			u32(8, 1), u32(0xc, 5), u64(0x10, 0), u32(0x18, 0), u32(0x1c, 8), u32(0x20, 0), u64(0x24, 0), u32(0x2c, 0x30300);
			u32(0x30, 0xbf800000u), u32(0x34, 0x3f800000u);   // -1.0, 1.0
			return kIOReturnSuccess;
		case 2:
			u32(8, 1), u32(0xc, 5), u64(0x10, 0), u32(0x18, 0), u32(0x1c, 0x80), u32(0x20, 0), u64(0x24, 0), u32(0x2c, 0);
			u32(0x30, 0x3f800000u), u64(0x34, 0);   // 1.0
			return kIOReturnSuccess;
		case 4:
			u32(8, 1), bzero(o + 0xc, 0x30);
			return kIOReturnSuccess;
		case 8:
			u32(8, 1), bzero(o + 0xc, 0x32c);
			return kIOReturnSuccess;
		case 0x10:   // the mode: its rectangle twice (height before width), 0x10000, in two records
			u32(8, 1), bzero(o + 0xc, 0x1c), u64(0x28, 1);
			u32(0x30, height), u32(0x34, width), u32(0x38, height), u32(0x3c, width), u32(0x40, 0x10000), bzero(o + 0x44, 0x16c);
			u64(0x1b0, 1), u32(0x1b8, height), u32(0x1bc, width), u32(0x1c0, height), u32(0x1c4, width), u32(0x1c8, 0x10000);
			bzero(o + 0x1cc, 0x16c);
			return kIOReturnSuccess;
		case 0x20: case 0x80: case 0x400: case 0x800:
			u64(8, 1), u64(0x10, 0);
			return kIOReturnSuccess;
		case 0x100:
			u32(8, 1), bzero(o + 0xc, 0x14);
			return kIOReturnSuccess;
		case 0x200: case 0x2000: case 0x4000:
			u64(8, 1);
			return kIOReturnSuccess;
		case 0x1000: case 0x8000:
			u32(8, 1), bzero(o + 0xc, 0x2c);
			return kIOReturnSuccess;
		default:
			return kIOReturnUnsupported;
		}
	}

	// the GPU's framebuffers with a display (IOFramebuffers anywhere under its provider), retained, at most cap: those WindowServer sees
	uint32_t framebuffers(IORegistryEntry **out, uint32_t cap)
	{
		uint32_t n = 0;
		IORegistryIterator *it = getProvider() ? IORegistryIterator::iterateOver(getProvider(), gIOServicePlane, kIORegistryIterateRecursively)
		                                        : nullptr;
		while (IORegistryEntry *e = it ? it->getNextObject() : nullptr)
			if (n < cap && e->metaCast("IOFramebuffer") && RDNA4Accel::showsFramebuffer(e)) {
				e->retain();
				out[n++] = e;
			}
		OSSafeReleaseNULL(it);
		auto key = [](IORegistryEntry *e) {   // by the family's framebuffer index (0x980's masks: the k-th listed is the k-th bit set)
			auto *index = OSDynamicCast(OSNumber, e->getProperty("IOAccelIndex"));
			return index ? index->unsigned32BitValue() : ~0u;
		};
		for (uint32_t i = 1; i < n; i++)
			for (uint32_t j = i; j > 0 && key(out[j]) < key(out[j - 1]); j--) {
				IORegistryEntry *t = out[j];
				out[j] = out[j - 1];
				out[j - 1] = t;
			}
		return n;
	}

public:
	IOReturn vendor_doDeviceAttribute(unsigned int attribute, unsigned long *in, unsigned long inLength, unsigned long *out,
	                                  unsigned long *outLength, IOExternalMethodArguments *) APPLE_KEXT_OVERRIDE
	{
		auto *o = reinterpret_cast<uint8_t *>(out);
		const unsigned long size = outLength ? *outLength : 0;
		IOReturn r = kIOReturnUnsupported;
		if (attribute == 1 && size == 0x2c && o) {   // version 1.0, a name of 32 bytes, vendor | device << 16, 2 (the paravirtual GPU's)
			bzero(o, 0x2c);
			*reinterpret_cast<uint32_t *>(o) = 0x10000;
			strlcpy(reinterpret_cast<char *>(o + 4), "AMD Radeon RDNA4", 32);
			*reinterpret_cast<uint32_t *>(o + 0x24) = 0x1002 | 0x7550u << 16;
			*reinterpret_cast<uint32_t *>(o + 0x28) = 2;
			r = kIOReturnSuccess;
		} else if (attribute == 3 && size == 4 && o && !inLength) {   // the framebuffer flags it may be asked to set: none (the paravirtual
			*reinterpret_cast<uint32_t *>(o) = 0;                   // GPU's 3: it makes its own framebuffers when asked; ours exist)
			r = kIOReturnSuccess;
		} else if (attribute == 0x980 && size == 0xdc && o) {   // its framebuffers, as the paravirtual GPU's: four masks of n bits (from
			IORegistryEntry *fb[20];                             // bit 1), n five times, the provider, then each framebuffer: objects,
			const uint32_t n = framebuffers(fb, 20);              // which AppleGraphicsDeviceControl's filtered_doDeviceAttribute turns
			                                                     // into registry IDs for user space (IDs here it took for objects: panic)
			// the masks: a bit for each listed framebuffer's index, the family's (IOAccelIndex, the display pipe's framebuffer index).
			// The paravirtual GPU's framebuffers are 1..n, hence its (~(~0 << n)) * 2; ours are 0, 3, 4: with its formula IOPresentment
			// presented on pipes 1 and 2, bare framebuffers' pipes, never active (transactionEnd: kIOReturnNotReady), 2026-11-21
			uint64_t mask = 0;
			for (uint32_t i = 0; i < n; i++) {
				auto *index = OSDynamicCast(OSNumber, fb[i]->getProperty("IOAccelIndex"));
				mask |= 1ull << (index ? index->unsigned32BitValue() & 63 : i + 1);
				static uint32_t logged;
				if (logged++ < 8)
					rdna4Log("AMDRadeonRDNA4: accel: AGDC 0x980: framebuffer %u: %s 0x%llx, IOAccelIndex %d (accelerator flags 0x%x)\n", i,
					         fb[i]->getName(), fb[i]->getRegistryEntryID(), index ? static_cast<int>(index->unsigned32BitValue()) : -1,
					         accelFlags());
			}
			bzero(o, 0xdc);
			for (int i = 0; i < 4; i++)
				*reinterpret_cast<uint64_t *>(o + 8 * i) = mask;
			for (int i = 0; i < 5; i++)
				*reinterpret_cast<uint32_t *>(o + 0x20 + 4 * i) = n;
			*reinterpret_cast<uint64_t *>(o + 0x34) = reinterpret_cast<uint64_t>(getProvider());
			for (uint32_t i = 0; i < n; i++) {   // (the framebuffers outlive the call: the registry holds them)
				*reinterpret_cast<uint64_t *>(o + 0x3c + 8 * i) = reinterpret_cast<uint64_t>(fb[i]);
				fb[i]->release();
			}
			r = kIOReturnSuccess;
		} else if (attribute == 0x711 && inLength == 0x196c && size == 0x196c && o && in) {   // a framebuffer's query (below)
			r = framebufferQuery(reinterpret_cast<const uint32_t *>(in), o);
		} else if (attribute == 0x925 && inLength == 0xc && size >= 0xc && o && in) {   // whether framebuffer n (1-based) shows: yes
			*reinterpret_cast<uint32_t *>(o + 8) = 1;   // (the paravirtual framebuffer's flag at +0x1e0, bit 0)
			r = kIOReturnSuccess;
		} else if (attribute == 0x921 && size == 0xb0 && o) {   // the paravirtual GPU's capabilities, as it writes them
			bzero(o, 0xb0);
			*reinterpret_cast<uint64_t *>(o + 0x08) = 1;
			*reinterpret_cast<uint64_t *>(o + 0x1c) = 0x40000000500ull;
			*reinterpret_cast<uint32_t *>(o + 0x44) = 0x500;
			*reinterpret_cast<uint64_t *>(o + 0x50) = 0x40000000000ull;
			*reinterpret_cast<uint64_t *>(o + 0x84) = 1;
			r = kIOReturnSuccess;
		}
		static uint32_t logged;
		if (logged++ < 32)
			rdna4Log("AMDRadeonRDNA4: accel: AGDC attribute 0x%x (in %lu, out %lu): 0x%x\n", attribute, inLength, size, r);
		return r;
	}
};
RDNA4_ACCEL_STRUCTORS(RDNA4DeviceControl, AppleGraphicsDeviceControl)

class RDNA4Accelerator : public IOGraphicsAccelerator2 {
	OSDeclareDefaultStructors(RDNA4Accelerator)
	using super = IOGraphicsAccelerator2;

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;

	// what every accelerator must have (IOGraphicsAccelerator2's pure virtuals). The family's start needs the device configured (the
	// kext's own: nothing to do), the config (the family's defaults), an event machine (its init by the family), a display machine
	// (kept: nothing starts it yet) and a kernel task (no GPU address ranges yet); the rest is for user clients, refused.
	void *getStampMemory(unsigned int *) APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelEventMachine2 *newEventMachine() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(RDNA4EventMachine); }
	IOAccelTask *createUserGPUTask() APPLE_KEXT_OVERRIDE { return nullptr; }
	// the family's defaults (initializeConfigStructure) but the number of framebuffers the display machine takes (+0x28, as AMD's:
	// its display count; the default took one, "trying to init too many framebuffers" for the others)
	void populateAccelConfig(IOAccelConfig *config) APPLE_KEXT_OVERRIDE
	{
		if (!config)
			return;
		*reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(config) + 0x28) = kMaxFramebuffers;
		// display pipe user clients allowed (+0x47, AMD's first store; the family's newUserClient refused type 4 without it, and
		// IOPresentment's IOAccelDisplayPipeCreate failed: "Unable to create IOAccelDisplayPipe", 2026-11-20)
		reinterpret_cast<uint8_t *>(config)[0x47] = 1;
	}
	static constexpr uint32_t kMaxFramebuffers = 8;
	bool configureDevice(IOPCIDevice *) APPLE_KEXT_OVERRIDE { return true; }
	void teardownDevice(IOPCIDevice *) APPLE_KEXT_OVERRIDE {}
	IOAccelTask *createKernelGPUTask() APPLE_KEXT_OVERRIDE;
	IOAccelDisplayMachine *newDisplayMachine() APPLE_KEXT_OVERRIDE { return displayMachine = OSTypeAlloc(RDNA4DisplayMachine); }
	RDNA4DisplayMachine *displayMachine = nullptr;   // the family keeps it (+0x378) and releases it
	IOAccelContext2 *newGLContext() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelContext2 *newCLContext() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelSurface *newSurface() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(RDNA4Surface); }
	IOAccelContext2 *new2DContext() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(RDNA4Context2D); }
	IOAccelContext2 *newVideoContext() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelSysMemory *newSysMemory() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(RDNA4SysMemory); }
	IOAccelVidMemory *newVidMemory() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(RDNA4VidMemory); }
	uintptr_t newDisplayPipe() APPLE_KEXT_OVERRIDE { return reinterpret_cast<uintptr_t>(OSTypeAlloc(RDNA4DisplayPipe)); }
	IOAccelResource2 *newResource() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(RDNA4Resource); }
	IOAccelMemoryMap *newMemoryMap() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(RDNA4MemoryMap); }
};

RDNA4_ACCEL_STRUCTORS(RDNA4Accelerator, IOGraphicsAccelerator2)

// As AMD's: a task of its own with the accelerator's (the family's init: no GPU address ranges, count 0); the event machine is made by
// then (IOAccelTask::init registers with it).
IOAccelTask *RDNA4Accelerator::createKernelGPUTask()
{
	// GPU virtual address ranges, one allocator a heap (the family takes up to 16; IOAccelTask::allocate indexes them by a map's heap):
	// with none, the first framebuffer resource the display pipes prepared dereferenced a null allocator (panic, 2026-11-21). The
	// addresses are bookkeeping for now: our maps' page-table updates are no-ops, the display path scans out through the kext's code.
	// ponytail: one fixed range for every heap, 4 GiB to 512 GiB (below 4 GiB reserved: no map at 0); real GPU VM when we draw through it.
	static constexpr uint32_t kHeaps = 16;
	static constexpr IORangeScalar kVaStart = 1ull << 32, kVaEnd = 1ull << 39;
	IORangeAllocator *ranges[kHeaps] = {};
	bool rangesOk = true;
	for (uint32_t i = 0; i < kHeaps && rangesOk; i++) {
		ranges[i] = IORangeAllocator::withRange(kVaEnd - 1, 0x1000, 0, IORangeAllocator::kLocking);
		rangesOk = ranges[i] && ranges[i]->allocateRange(0, kVaStart);
	}
	RDNA4Task *t = rangesOk ? OSTypeAlloc(RDNA4Task) : nullptr;
	if (t && !t->init(this, kHeaps, ranges))
		OSSafeReleaseNULL(t);
	for (auto *r : ranges)   // (the task holds its own references)
		OSSafeReleaseNULL(r);
	rdna4Log("AMDRadeonRDNA4: accel: kernel task %s\n", t ? "made" : "NOT made");
	return t;
}

bool RDNA4Accelerator::start(IOService *provider)
{
	rdna4Log("AMDRadeonRDNA4: accel: the family's start\n");
	const bool ok = super::start(provider);
	rdna4Log("AMDRadeonRDNA4: accel: the family's start %s (class %s, registered %s)\n", ok ? "succeeded" : "FAILED", getMetaClass()->getClassName(),
	         isInactive() ? "-" : getProperty("IOAccelRevision") ? "with its properties" : "without its properties");
	if (ok)
		setProperty("RDNA4Accelerator", "milestone 2: the family's start");
	return ok;
}

void RDNA4Accelerator::stop(IOService *provider)
{
	super::stop(provider);
}

static bool gFamilyClients;   // the family's own user clients (display pipes, devices, shared, ...) handed out: rdna4-accel=4 only

// Our runtime's client (the native Metal device opens it through its accelerator, as through the paravirtual one: pvhooks.cpp); the
// family's own types only when this boot's WindowServer is to use us (rdna4-accel=4), else refused.
IOReturn RDNA4Accelerator::newUserClient(task_t task, void *security, UInt32 type, IOUserClient **handler)
{
	if (type == RDNA4_NATIVE_UC_TYPE)
		return rdna4AcceleratorClient(this, task, security, handler);
	const IOReturn r = gFamilyClients ? super::newUserClient(task, security, type, handler) : kIOReturnUnsupported;
	static uint32_t logged;
	if (logged++ < 64) {
		char name[32] = "";
		proc_selfname(name, sizeof(name));
		rdna4Log("AMDRadeonRDNA4: accel: user client of type 0x%x for %s: %s: 0x%x\n", static_cast<unsigned>(type), name,
		         gFamilyClients ? "the family's" : "refused", r);
	}
	return r;
}

namespace {
// Our classes: their metaclass, the base's real metaclass (the symbol) and our placeholder for it, our size, the base's KDK size
struct Derived {
	const OSMetaClass *ours;
	const char *baseMeta;
	const OSMetaClass *placeholder;
	uint32_t ourSize;
	uint32_t baseSize;
	uint32_t kext;   // the base's kext (kKexts)
};
#define RDNA4_DERIVED(cls, base, n) RDNA4_DERIVED_IN(cls, base, n, 0)
#define RDNA4_DERIVED_IN(cls, base, n, kext) \
	{ &cls::gMetaClass, "__ZN" #n #base "10gMetaClassE", &base::gMetaClass, sizeof(cls), k##base##Size, kext }
const Derived kDerived[] = {
	RDNA4_DERIVED(RDNA4Accelerator, IOGraphicsAccelerator2, 22),
	RDNA4_DERIVED(RDNA4EventMachine, IOAccelEventMachineFast2, 24),
	RDNA4_DERIVED(RDNA4DisplayMachine, IOAccelDisplayMachine, 21),
	RDNA4_DERIVED(RDNA4Task, IOAccelTask, 11),
	RDNA4_DERIVED(RDNA4MemoryMap, IOAccelMemoryMap, 16),
	RDNA4_DERIVED(RDNA4Resource, IOAccelResource2, 16),
	RDNA4_DERIVED(RDNA4SysMemory, IOAccelSysMemory, 16),
	RDNA4_DERIVED(RDNA4VidMemory, IOAccelVidMemory, 16),
	RDNA4_DERIVED(RDNA4Surface, IOAccelSurface, 14),
	RDNA4_DERIVED(RDNA4Context2D, IOAccel2DContext2, 17),
	RDNA4_DERIVED(RDNA4DisplayPipe, IOAccelDisplayPipe, 18),
	RDNA4_DERIVED_IN(RDNA4DeviceControl, AppleGraphicsDeviceControl, 26, 1),
};
constexpr size_t kDerivedCount = sizeof(kDerived) / sizeof(kDerived[0]);

// Kext `kext`'s thunks filled, checked; our metaclasses whose base is in it given the real superclasses.
// IOGraphicsAccelerator2::enableAccelerator (not virtual: no thunk), solved with the family
mach_vm_address_t gEnableAccelerator;

void familyLoaded(uint32_t kext, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
	if (kext == 0) {
		gEnableAccelerator = patcher.solveSymbol(index, "__ZN22IOGraphicsAccelerator217enableAcceleratorEv", address, size);
		patcher.clearError();
		gPlaneIOSurface = patcher.solveSymbol(index, "__ZNK30IOAccelDisplayPipeTransaction217getPlaneIOSurfaceEjj", address, size);
		patcher.clearError();
		gWithMemoryDescriptor = patcher.solveSymbol(index,
		                                            "__ZN16IOAccelSysMemory20withMemoryDescriptorEP22IOGraphicsAccelerator2P16IOAccelResource2P18IOMemoryDescriptor",
		                                            address, size);
		patcher.clearError();
	}
	uint32_t missing = 0, wrongSlots = 0, thunks = 0, folded = 0;
	for (size_t i = 0; i < kThunks; i++) {
		if (kSymbols[i].kext != kext)
			continue;
		thunks++;
		const mach_vm_address_t a = patcher.solveSymbol(index, kSymbols[i].name, address, size);
		if (a)
			rdna4AccelThunks[i] = a;
		else if (missing++ < 4)
			rdna4Log("AMDRadeonRDNA4: accel: %s not found\n", kSymbols[i].name);
		patcher.clearError();
	}
	for (const auto &s : kSlots) {
		if (kSymbols[s.thunk].kext != kext)
			continue;
		const mach_vm_address_t vt = patcher.solveSymbol(index, s.vtable, address, size);
		patcher.clearError();
		const uintptr_t inSlot = vt ? reinterpret_cast<const uintptr_t *>(vt + 16)[s.slot] : 0;
		// the same code at another address: the kernel collection folded identical functions (small getters; the symbol names one copy,
		// the vtable holds another): the vtable's copy is called
		size_t body = 32;   // the bytes compared: up to the symbol's first ret when one is that near (empty and return-0 bodies)
		for (size_t k = 0; rdna4AccelThunks[s.thunk] && k < 32; k++)
			if (reinterpret_cast<const uint8_t *>(rdna4AccelThunks[s.thunk])[k] == 0xc3) {
				body = k + 1;
				break;
			}
		if (inSlot && rdna4AccelThunks[s.thunk] && inSlot != rdna4AccelThunks[s.thunk] &&
		    !memcmp(reinterpret_cast<const void *>(inSlot), reinterpret_cast<const void *>(rdna4AccelThunks[s.thunk]), body)) {
			rdna4AccelThunks[s.thunk] = inSlot;
			folded++;
			continue;
		}
		if (inSlot != rdna4AccelThunks[s.thunk] && wrongSlots++ < 12) {
			int found = -1;   // where the function is instead (a few slots either way: this macOS's layout moved)
			for (int d = -8; vt && d <= 8 && found < 0; d++)
				if (reinterpret_cast<const uintptr_t *>(vt + 16)[static_cast<int>(s.slot) + d] == rdna4AccelThunks[s.thunk])
					found = static_cast<int>(s.slot) + d;
			const uint8_t *b = reinterpret_cast<const uint8_t *>(inSlot);   // what it holds instead
			rdna4Log("AMDRadeonRDNA4: accel: slot %u of %s does not hold %s (it is in slot %d; the slot's code %02x %02x %02x %02x)\n", s.slot,
			         s.vtable, kSymbols[s.thunk].name, found, b ? b[0] : 0, b ? b[1] : 0, b ? b[2] : 0, b ? b[3] : 0);
		}
	}
	// each of our classes: its metaclass's superclass link and size where OSMetaClass keeps them, then the real base's metaclass
	// (the base's size is checked when one is made: here the family's static constructors have not run yet, its metaclass reads 0)
	uint32_t bad = 0;
	const OSMetaClass *real[kDerivedCount] = {};
	for (size_t i = 0; i < kDerivedCount; i++) {
		const Derived &d = kDerived[i];
		if (d.kext != kext)
			continue;
		real[i] = reinterpret_cast<const OSMetaClass *>(patcher.solveSymbol(index, d.baseMeta, address, size));
		patcher.clearError();
		auto *ours = reinterpret_cast<const uint8_t *>(d.ours);
		const bool layout = *reinterpret_cast<const OSMetaClass *const *>(ours + 16) == d.placeholder &&
		                    *reinterpret_cast<const uint32_t *>(ours + 32) == d.ourSize;
		if (!layout || !real[i]) {
			bad++;
			rdna4Log("AMDRadeonRDNA4: accel: %s: %s\n", d.baseMeta, real[i] ? "our metaclass NOT as expected" : "NOT found");
		}
	}
	if (!missing && !wrongSlots && !bad) {
		for (size_t i = 0; i < kDerivedCount; i++)
			if (kDerived[i].kext == kext)
				*reinterpret_cast<const OSMetaClass **>(reinterpret_cast<uint8_t *>(const_cast<OSMetaClass *>(kDerived[i].ours)) + 16) = real[i];
		gSolved |= 1u << kext;
	} else {
		gFailed |= 1u << kext;
	}
	gReady = gSolved == (1u << kKextCount) - 1 && !gFailed;
	rdna4Log("AMDRadeonRDNA4: accel: %s: %u thunks, %u not found, %u slots not the KDK's (%u folded copies), %u classes not as expected: %s\n",
	         kKexts[kext].bundle, thunks, missing, wrongSlots, folded, bad,
	         gReady ? "ready (debug.rdna4_accel=1 makes one, 2 starts it, 3 its displays)" : gFailed ? "NOT usable" : "solved");
}

RDNA4Accelerator *gAccel;   // made and attached (debug.rdna4_accel=1)
uint32_t accelFlags() { return gAccel ? *reinterpret_cast<const uint32_t *>(reinterpret_cast<const uint8_t *>(gAccel) + 0xc78) : 0; }
bool gStarted;              // the family's start ran (debug.rdna4_accel=2)
bool gDisplays;             // the display machine started (debug.rdna4_accel=3)

// In steps, each logged: 1 makes one (the family's own constructor runs) and attaches it under the GPU; 2 runs the family's start;
// 3 initialises and starts the display machine on the GPU (the family's: a display pipe for each framebuffer it finds); 4 (a test boot
// whose WindowServer is to use us: rdna4-accel=4 with the paravirtual GPU off) also names our Metal bundle, hands out the family's
// user clients and registers the accelerator.
int accelTo(int value)
{
	if (value < 1 || value > 4)
		return EINVAL;
	// (accelcensus.cpp's: an extern of its variable, which is in an anonymous namespace, linked as an unresolved lookup that OpenCore
	// bound to junk: reading it panicked the rig, 2026-11-20)
	IOService *gpu = RDNA4AccelCensus::gpu();
	if (!gReady || !gpu)
		return ENODEV;
	if (!gAccel) {
		for (const Derived &d : kDerived) {
			const OSMetaClass *base = d.ours->getSuperClass();
			if (base->getClassSize() != d.baseSize) {
				rdna4Log("AMDRadeonRDNA4: accel: %s is 0x%x bytes here, the KDK's 0x%x: not made\n", base->getClassName(), base->getClassSize(),
				         d.baseSize);
				return ENODEV;
			}
		}
		rdna4Log("AMDRadeonRDNA4: accel: making one (the family's constructor)\n");
		auto *a = OSTypeAlloc(RDNA4Accelerator);
		rdna4Log("AMDRadeonRDNA4: accel: made %p; init\n", a);
		if (!a || !a->init()) {
			OSSafeReleaseNULL(a);
			return ENOMEM;
		}
		a->setName("RDNA4Accelerator");
		if (!a->attach(gpu)) {
			a->release();
			return EBUSY;
		}
		gAccel = a;
		rdna4Log("AMDRadeonRDNA4: accel: attached under %s (not started): a kind of IOGraphicsAccelerator2: %s\n", gpu->getName(),
		         a->metaCast("IOGraphicsAccelerator2") ? "yes" : "NO");
	}
	if (value >= 2 && !gStarted) {
		rdna4Log("AMDRadeonRDNA4: accel: starting\n");
		if (!gAccel->start(gpu))
			return EIO;
		gStarted = true;
	}
	if (value >= 3 && !gDisplays) {
		RDNA4DisplayMachine *dm = gAccel->displayMachine;
		if (!dm)
			return ENODEV;
		// (init and start return nothing: what rax held, the lock's address: not a result; the framebuffers' IOAccelIndex tell)
		rdna4Log("AMDRadeonRDNA4: accel: display machine init and start on the GPU\n");
		dm->init(gAccel);
		dm->start(reinterpret_cast<IOPCIDevice *>(gpu));   // (gpu is the GPU's IOPCIDevice)
		rdna4Log("AMDRadeonRDNA4: accel: display machine started\n");
		gDisplays = true;
	}
	if (value == 4 && !gFamilyClients) {
		gAccel->setProperty("MetalPluginName", "AMDRadeonRDNA4MTLDriver");   // as the native spike's service (accelcensus.cpp, level 3)
		gAccel->setProperty("MetalPluginClassName", "RDNA4MTLDevice");
		// as Apple's paravirtual accelerator (its AcceleratorProperties): CoreDisplay presents through display pipe transactions when
		// these say so; without them it took the older surface path
		gAccel->setProperty("IOMatchCategory", "IOAccelerator");
		// the paravirtual accelerator's personality: CoreDisplay makes each display's 2D blit engine through the plug-in these name
		// (IOPSAllocateBlitEngine: IOCreatePlugInInterfaceForService on the accelerator); without one a display stayed unaccelerated
		if (OSDictionary *plugins = OSDictionary::withCapacity(1)) {
			if (OSString *plugin = OSString::withCString("IOAccelerator2D.plugin")) {
				plugins->setObject("ACCF0000-0000-0000-0000-000a2789904e", plugin);
				plugin->release();
			}
			gAccel->setProperty("IOCFPlugInTypes", plugins);
			plugins->release();
		}
		// OpenGL on our Metal device (Apple's GL-on-Metal renderer; debug.rdna4_accelgl switches it at run time). rdna4-accelgl=0: not named
		uint32_t gl = 0;
		if (Trial::bootArg("rdna4-accelgl", gl) && gl)
			gAccel->setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
		gAccel->setProperty("IOAccelRevision", 2ull, 32);
		gAccel->setProperty("AccelCaps", 7ull, 32);
		if (OSDictionary *caps = OSDictionary::withCapacity(2)) {
			caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
			caps->setObject("TransactionsSupported", kOSBooleanTrue);
			gAccel->setProperty("IOAccelDisplayPipeCapabilities", caps);
			caps->release();
		}
		gFamilyClients = true;
		// the GPU's AGDC service, for IOPresentment (RDNA4DeviceControl)
		auto *agdc = OSTypeAlloc(RDNA4DeviceControl);
		const bool agdcUp = agdc && agdc->init() && agdc->attach(gpu) && agdc->start(gpu);
		if (agdcUp)
			agdc->registerService();
		rdna4Log("AMDRadeonRDNA4: accel: AGDC service %s\n", agdcUp ? "started" : "NOT started");
		OSSafeReleaseNULL(agdc);   // (the registry keeps a started one)
#ifdef AMDRadeonRDNA4_PVGPU
		translatorInit();   // the Metal device's shaders go through the translator helper (none without the paravirtual host)
#endif
		// enabled, as a driver does once its hardware is up (the accelerator's flag 0x2 at +0xc78): IOAccelDisplayPipeUserClient2's
		// transactionEnd refused every one of WindowServer's display pipe transactions without it (kIOReturnNotReady, 229 a test boot),
		// and its displays' surfaces were composited in software (CDDisplay::begin_update: CompositorSW), 2026-11-21
		if (gEnableAccelerator)
			reinterpret_cast<void (*)(IOGraphicsAccelerator2 *)>(gEnableAccelerator)(gAccel);
		rdna4Log("AMDRadeonRDNA4: accel: enableAccelerator %s: flags +0xc78 0x%x\n", gEnableAccelerator ? "called" : "NOT found",
		         *reinterpret_cast<const uint32_t *>(reinterpret_cast<const uint8_t *>(gAccel) + 0xc78));
		gAccel->registerService();
		rdna4Log("AMDRadeonRDNA4: accel: registered for WindowServer: our Metal bundle, the family's user clients\n");
	}
	return 0;
}

void guardSet(const char *what);

int accelSysctl SYSCTL_HANDLER_ARGS
{
	int value = gFamilyClients ? 4 : gDisplays ? 3 : gStarted ? 2 : gAccel ? 1 : 0;
	const int err = sysctl_handle_int(oidp, &value, 0, req);
	if (err || !req->newptr)
		return err;
	if (value == 0) {   // the boot-arg's guard cleared: the next boot tries again
		guardSet("cleared");
		return 0;
	}
	return accelTo(value);
}

// The boot-arg's guard (rdna4-accel in config.plist survives a crash, a test boot's option does not): "pending" while an attempt runs,
// "survived" once it has lasted kGuardSeconds; a boot that finds "pending" skips the attempt (the last one hung or panicked).
// debug.rdna4_accel=0 clears it. Written as trial.cpp writes its variable (IORegistryEntry's own calls on /options).
const char *kGuardKey = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-accel-guard";
constexpr uint32_t kGuardSeconds = 120;

bool guardIs(const char *what)
{
	IORegistryEntry *nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
	OSObject *v = nvram ? nvram->copyProperty(kGuardKey) : nullptr;
	char text[32] = "";
	if (auto *str = OSDynamicCast(OSString, v))
		strlcpy(text, str->getCStringNoCopy(), sizeof(text));
	else if (auto *d = OSDynamicCast(OSData, v))
		strlcpy(text, static_cast<const char *>(d->getBytesNoCopy()), MIN(sizeof(text), d->getLength() + 1));
	OSSafeReleaseNULL(v);
	OSSafeReleaseNULL(nvram);
	return !strcmp(text, what);
}

void guardSet(const char *what)
{
	IORegistryEntry *nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
	const OSSymbol *key = OSSymbol::withCString(kGuardKey);
	OSString *value = OSString::withCString(what);
	OSDictionary *sync = OSDictionary::withCapacity(1);
	OSString *yes = OSString::withCString("1");
	if (nvram && key && value)
		nvram->setProperty(key, value);
	if (nvram && sync && yes && sync->setObject("IONVRAM-FORCESYNCNOW-PROPERTY", yes))
		nvram->setProperties(sync);
	OSSafeReleaseNULL(yes);
	OSSafeReleaseNULL(sync);
	OSSafeReleaseNULL(value);
	OSSafeReleaseNULL(key);
	OSSafeReleaseNULL(nvram);
}

// rdna4-accel (a boot-arg, or a test boot's: rdna4-trial "armed rdna4-accel=4"): the steps at boot, before WindowServer if it can,
// once the family is solved, the GPU known and its first framebuffer up (the rest of the boot heads a moment later).
// The boot thread's end (whatever the outcome): what WindowServer's first wrangler open waits for (RDNA4Accel::waitForWindowServer)
bool gBootDone;
void bootDone()
{
	gBootDone = true;
}

void accelAtBoot(void *, wait_result_t)
{
	for (int ms = 0; ms < 60000 && !RDNA4AccelCensus::gpu(); ms += 100)
		IOSleep(100);
	// asked only now: at the plugin's start NVRAM is not there yet and a test boot's options read as none (2026-11-20)
	uint32_t want = 0;
	if (!Trial::option("rdna4-accel", want) || want < 1 || want > 4) {
		bootDone();
		thread_terminate(current_thread());
		return;
	}
	if (guardIs("pending")) {
		rdna4Log("AMDRadeonRDNA4: accel: rdna4-accel=%u: the last attempt did not survive %u s (guard \"pending\"): skipped; "
		         "debug.rdna4_accel=0 clears the guard\n", want, kGuardSeconds);
		bootDone();
		thread_terminate(current_thread());
		return;
	}
	guardSet("pending");
	// Without the paravirtual GPU nothing loads the family (our kext cannot depend on it): loaded here, solved when it comes
	for (const KextName &k : kKexts) {
		const OSReturn loaded = OSKextLoadKextWithIdentifier(k.bundle);
		rdna4Log("AMDRadeonRDNA4: accel: loading %s: 0x%x\n", k.bundle, loaded);
	}
	for (int ms = 0; ms < 30000 && !gReady; ms += 100)
		IOSleep(100);
	OSDictionary *match = IOService::serviceMatching("IONDRVFramebuffer");
	IOService *fb = match ? IOService::waitForMatchingService(match, 60ull * NSEC_PER_SEC) : nullptr;
	OSSafeReleaseNULL(match);
	OSSafeReleaseNULL(fb);
	IOSleep(2000);
	const int err = accelTo(static_cast<int>(want));
	rdna4Log("AMDRadeonRDNA4: accel: rdna4-accel=%u at boot: %s (%d)\n", want, err ? "FAILED" : "done", err);
	bootDone();
	IOSleep(kGuardSeconds * 1000);
	guardSet("survived");
	rdna4Log("AMDRadeonRDNA4: accel: rdna4-accel=%u survived %u s: guard cleared\n", want, kGuardSeconds);
	thread_terminate(current_thread());
}

// (diagnostic) each display pipe's state as transactionEnd tests it, one line a pipe: index, transactions begun (+0x288), active
// (+0x298), terminated (+0x280), WSAA defer (+0x282), +0x299, the queue (+0x238 head, +0x23c tail), live (+0x248), and its VBLs:
// the framebuffer interrupt it registered (+0xd8, displayModeDidChange), VBL interrupt on (+0x2a0), enableVBLInterrupt count (+0x2a8)
int accelPipesSysctl SYSCTL_HANDLER_ARGS
{
	char text[1536];   // (8 lines of at most ~170 characters)
	size_t at = 0;
	text[0] = 0;
	for (uint32_t i = 0; i < 8 && at < sizeof(text); i++)
		if (const uint8_t *b = gPipes[i]) {
			auto u32 = [b](uint32_t off) { return *reinterpret_cast<const uint32_t *>(b + off); };
			at += static_cast<size_t>(snprintf(text + at, sizeof(text) - at,
			                                   "pipe %u %p: begun %u active %u term %u defer %u 299 %u queue %d..%d live %p vbl ref %p on %u enables %u\n", i, b,
			                                   u32(0x288), b[0x298], b[0x280], b[0x282], b[0x299], static_cast<int32_t>(u32(0x238)),
			                                   static_cast<int32_t>(u32(0x23c)), *reinterpret_cast<void *const *>(b + 0x248),
			                                   *reinterpret_cast<void *const *>(b + 0xd8), u32(0x2a0), u32(0x2a8)));
		}
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
// OpenGL on our accelerator, for processes started after it is set: AppleMetalOpenGLRenderer named in IOGLBundleName (OpenGL.framework loads
// a hardware renderer only for an accelerator that names one), running on our Metal device. Named from the boot (accelTo, rdna4-accelgl,
// default 1); naming it from the boot froze the rig once (af85142), before the Metal bundle had its own GL drawables.
int accelGlSysctl SYSCTL_HANDLER_ARGS
{
	int value = gAccel && gAccel->getProperty("IOGLBundleName") ? 1 : 0;
	const int err = sysctl_handle_int(oidp, &value, 0, req);
	if (err || !req->newptr)
		return err;
	if (!gAccel || !gFamilyClients)
		return ENODEV;
	if (value)
		gAccel->setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
	else
		gAccel->removeProperty("IOGLBundleName");
	rdna4Log("AMDRadeonRDNA4: accel: OpenGL renderer %s\n", value ? "named (AppleMetalOpenGLRenderer)" : "no longer named");
	return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, rdna4_accelgl, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, accelGlSysctl, "I",
            "AMDRadeonRDNA4: 1 names Apple's GL-on-Metal renderer on our accelerator (OpenGL on our Metal device), 0 not");
SYSCTL_PROC(_debug, OID_AUTO, rdna4_accelpipes, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, accelPipesSysctl, "A",
            "AMDRadeonRDNA4: our display pipes' states (diagnostic)");
SYSCTL_PROC(_debug, OID_AUTO, rdna4_accel, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, accelSysctl, "I",
            "AMDRadeonRDNA4: our own IOGraphicsAccelerator2: 1 makes and attaches one, 2 runs the family's start, 3 starts the display machine (docs/native-driver-plan.md)");
}   // namespace

IOGraphicsAccelerator2 *ourAccelerator() { return gAccel; }

bool RDNA4Accel::isOurs(const void *accel) { return accel && accel == gAccel; }

namespace {
// Whether framebuffer `fb` has a monitor, as the kext knows its heads (plugin.cpp). Not an IODisplayConnect under it: those came
// more than 10 s late in some boots (2026-11-21), all six framebuffers were then hidden and WindowServer ran without displays.
bool isLit(IOService *fb)
{
#ifdef AMDRadeonRDNA4_PVGPU
	return PvScanout::framebufferLit(fb);
#else
	(void)fb;
	return true;
#endif
}

// Our framebuffers without a monitor, by registry ID: decided once, when WindowServer is let go (waitForWindowServer).
uint64_t gBare[8];
uint32_t gBareCount;
bool gBareKnown;

// The IOFramebuffers under the GPU, each passed to `f` until it returns false
template <typename F> void eachFramebuffer(F f)
{
	IOService *gpu = RDNA4AccelCensus::gpu();
	IORegistryIterator *it = gpu ? IORegistryIterator::iterateOver(gpu, gIOServicePlane, kIORegistryIterateRecursively) : nullptr;
	while (IORegistryEntry *e = it ? it->getNextObject() : nullptr)
		if (e->metaCast("IOFramebuffer") && !f(e))
			break;
	OSSafeReleaseNULL(it);
}
}   // namespace

namespace {
// A field of a serialized AppleGPUWrangler_GPU ("<key>acc</key><integer size=\"64\">0x...</integer>"), 0 when absent
uint64_t serializedNumber(const char *xml, const char *key)
{
	char want[32];
	snprintf(want, sizeof(want), "<key>%s</key>", key);
	const char *at = xml ? strstr(xml, want) : nullptr;
	at = at ? strstr(at, "<integer") : nullptr;
	at = at ? strchr(at, '>') : nullptr;
	if (!at)
		return 0;
	at++;
	const bool hex = at[0] == '0' && (at[1] == 'x' || at[1] == 'X');
	uint64_t v = 0;
	for (at += hex ? 2 : 0;; at++) {
		const char c = *at;
		const int d = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0)
			return v;
		v = v * (hex ? 16 : 10) + static_cast<uint64_t>(d);
	}
}

// Whether AppleGPUWrangler's GPU list (its debug-gpus: AppleGPUWrangler_GPU objects, each serialized as {"pci" = the GPU's registry ID,
// "acc" = its accelerator's}) has our accelerator for our GPU
bool wranglerHasUs()
{
	IOService *gpu = RDNA4AccelCensus::gpu();
	OSDictionary *match = IOService::serviceMatching("AppleGPUWrangler");
	IOService *w = match ? IOService::copyMatchingService(match) : nullptr;
	OSSafeReleaseNULL(match);
	bool has = false;
	auto *gpus = w ? OSDynamicCast(OSArray, w->getProperty("debug-gpus")) : nullptr;
	for (unsigned i = 0; gpu && gAccel && gpus && i < gpus->getCount() && !has; i++) {
		OSSerialize *ser = OSSerialize::withCapacity(1024);
		const OSObject *g = gpus->getObject(i);
		if (ser && g && g->serialize(ser))
			has = serializedNumber(ser->text(), "pci") == gpu->getRegistryEntryID() &&
			      serializedNumber(ser->text(), "acc") == gAccel->getRegistryEntryID();
		OSSafeReleaseNULL(ser);
	}
	OSSafeReleaseNULL(w);
	return has;
}
}   // namespace

namespace {
// An IODisplayConnect under framebuffer `fb`: IOFramebuffer's display, made when it found a monitor
bool hasDisplayConnect(IORegistryEntry *fb)
{
	bool has = false;
	if (OSIterator *it = fb->getChildIterator(gIOServicePlane)) {
		while (OSObject *o = it->getNextObject())
			has |= OSDynamicCast(IORegistryEntry, o) && static_cast<IORegistryEntry *>(o)->metaCast("IODisplayConnect");
		it->release();
	}
	return has;
}
}   // namespace

bool RDNA4Accel::framebuffersDecided() { return __atomic_load_n(&gBareKnown, __ATOMIC_ACQUIRE); }

bool RDNA4Accel::isBareFramebuffer(IORegistryEntry *fb)
{
	const uint64_t id = __atomic_load_n(&gBareKnown, __ATOMIC_ACQUIRE) ? fb->getRegistryEntryID() : 0;
	for (uint32_t i = 0; id && i < gBareCount; i++)
		if (gBare[i] == id)
			return true;
	return false;
}

bool RDNA4Accel::hidesFromMatch(IORegistryEntry *service, OSDictionary *)
{
	// Only the bare ones: a match by class must find the lit ones too. CoreDisplay's GetSystemGPUVendors takes the GPU vendors from
	// the IOFramebuffers a class match finds, and turns IOPresentment off when there are none ("GetSystemGPUVendors: 0 0"): hidden from
	// class matches, the lit ones left WindowServer on the framebuffers' legacy path, composited in software (2026-11-21).
	return __atomic_load_n(&gBareKnown, __ATOMIC_ACQUIRE) && isBareFramebuffer(service);
}

bool RDNA4Accel::namesAsAmd(OSObject *object)
{
	auto *e = __atomic_load_n(&gBareKnown, __ATOMIC_ACQUIRE) ? OSDynamicCast(IORegistryEntry, object) : nullptr;
	return e && e->metaCast("IONDRVFramebuffer") && !isBareFramebuffer(e);
}

bool RDNA4Accel::showsFramebuffer(IORegistryEntry *fb)
{
	auto *svc = OSDynamicCast(IOService, fb);
	return __atomic_load_n(&gBareKnown, __ATOMIC_ACQUIRE) ? !isBareFramebuffer(fb) : svc && isLit(svc);
}

// WindowServer's first open of the GPU wrangler (pvhooks.cpp wrapServiceOpen): with rdna4-accel=4 as a boot-arg, held until the boot
// thread has made and registered the accelerator (or given up), at most 60 s: WindowServer's wrangler lists each GPU's accelerator
// when it opens, and it opened before ours existed ("accel 0x0", 2026-11-20). (A test boot's option is not known this early.)
// Then the framebuffers without a monitor are the bare ones (hidden from WindowServer).
void RDNA4Accel::waitForWindowServer()
{
	uint32_t want = 0;
	if (!Trial::bootArg("rdna4-accel", want) || want != 4)
		return;
	rdna4Log("AMDRadeonRDNA4: accel: WindowServer opens the GPU wrangler: held until the accelerator is registered\n");
	for (int ms = 0; ms < 60000 && !gBootDone; ms += 50)
		IOSleep(50);
	// the GPU wrangler takes our accelerator as the GPU's asynchronously (its own work loop, on our registerService): until its
	// debug-gpus names it, WindowServer's CoreDisplay::GPU gets accelerator 0 for each framebuffer and composites in software
	uint32_t waited = 0;
	for (; gFamilyClients && !wranglerHasUs() && waited < 5000; waited += 50)
		IOSleep(50);
	// and the card's runtime (RDNA4ComputeService, runtime.cpp): WindowServer's Metal device opens it at once, and when it was not up yet
	// ("data was not found") CoreDisplay aborted and WindowServer restarted, three times in a boot of 2026-11-21 (let go at 14.8 s)
	bool runtime = false;
	if (gFamilyClients) {
		OSDictionary *match = IOService::serviceMatching(RDNA4_COMPUTE_SERVICE);
		IOService *svc = match ? IOService::waitForMatchingService(match, 30ull * kSecondScale) : nullptr;
		runtime = svc;
		OSSafeReleaseNULL(svc);
		OSSafeReleaseNULL(match);
	}
	// and each lit framebuffer's IODisplayConnect (its monitor as IOFramebuffer knows it): IOFramebuffer::open makes them. One that
	// WindowServer opened first got mode 0 and no display for half a minute (the Samsung, runs 19 and 29 of 2026-11-21: the boot
	// framebuffer opened ahead, pvhooks.cpp, is the oldest IONDRV one, there a monitorless display nub's): opened here first.
	uint32_t connectMs = 0, missing = 0;
	for (; gFamilyClients && connectMs <= 15000; connectMs += 100) {
		missing = 0;
		eachFramebuffer([&](IORegistryEntry *e) {
			auto *fb = OSDynamicCast(IOService, e);
			if (fb && isLit(fb) && !hasDisplayConnect(e)) {
				missing++;
#ifdef AMDRadeonRDNA4_PVGPU
				if (!connectMs)
					rdna4Log("AMDRadeonRDNA4: accel: framebuffer %s 0x%llx: lit, no IODisplayConnect yet: opened (0x%x)\n", e->getName(),
					         e->getRegistryEntryID(), PvHooks::openFramebuffer(fb));
#endif
			}
			return true;
		});
		if (!missing)
			break;
		IOSleep(100);
	}
	if (gFamilyClients)
		rdna4Log("AMDRadeonRDNA4: accel: the lit framebuffers' IODisplayConnects: %s after %u ms\n", missing ? "NOT all there" : "all there",
		         connectMs);
	uint32_t lit = 0;
	if (gFamilyClients) {
		eachFramebuffer([&](IORegistryEntry *e) {
			auto *fb = OSDynamicCast(IOService, e);
			const bool on = fb && isLit(fb);
			lit += on;
			if (e->metaCast("IONDRVFramebuffer") && !on && gBareCount < 8)
				gBare[gBareCount++] = e->getRegistryEntryID();
			rdna4Log("AMDRadeonRDNA4: accel: framebuffer %s 0x%llx: %s\n", e->getName(), e->getRegistryEntryID(), on ? "lit" : "bare");
			return true;
		});
		__atomic_store_n(&gBareKnown, true, __ATOMIC_RELEASE);
	}
	rdna4Log("AMDRadeonRDNA4: accel: WindowServer let go (%s): the wrangler %s us after %u ms; %u framebuffer(s) lit, %u bare hidden; "
	         "runtime %s\n", gFamilyClients ? "registered" : "NOT registered", wranglerHasUs() ? "has" : "does NOT have", waited, lit, gBareCount,
	         runtime ? "up" : "NOT up after 30 s");
}

void RDNA4Accel::start()
{
	for (size_t k = 0; k < kKextCount; k++) {
		gKextPath[k] = kKexts[k].path;
		gKextInfo[k] = { kKexts[k].bundle, &gKextPath[k], 1, { true }, {}, KernelPatcher::KextInfo::Unloaded };
	}
	lilu.onKextLoadForce(gKextInfo, kKextCount, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		for (uint32_t k = 0; k < kKextCount; k++)
			if (index == gKextInfo[k].loadIndex)
				familyLoaded(k, patcher, index, address, size);
	}, nullptr);
	sysctl_register_oid(&sysctl__debug_rdna4_accel);
	sysctl_register_oid(&sysctl__debug_rdna4_accelpipes);
	sysctl_register_oid(&sysctl__debug_rdna4_accelgl);
	thread_t th;   // (it waits for the GPU, then reads rdna4-accel)
	if (kernel_thread_start(accelAtBoot, nullptr, &th) == KERN_SUCCESS)
		thread_deallocate(th);
}
