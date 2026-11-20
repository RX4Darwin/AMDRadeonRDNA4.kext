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

#include "accelcensus.hpp"
#ifdef AMDRadeonRDNA4_PVGPU
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
};
RDNA4_ACCEL_STRUCTORS(RDNA4DisplayMachine, IOAccelDisplayMachine)

class RDNA4Task : public IOAccelTask {
	OSDeclareDefaultStructors(RDNA4Task)
};
RDNA4_ACCEL_STRUCTORS(RDNA4Task, IOAccelTask)

class RDNA4MemoryMap : public IOAccelMemoryMap {
	OSDeclareDefaultStructors(RDNA4MemoryMap)
public:
	uintptr_t commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t updateGPUPageTable() APPLE_KEXT_OVERRIDE { return 0; }
};
RDNA4_ACCEL_STRUCTORS(RDNA4MemoryMap, IOAccelMemoryMap)

// A resource: the family's header for a surface or buffer (a display pipe asks for one for its framebuffer). Its memory, apertures and
// paging buffer are not ours yet: nothing.
class RDNA4Resource : public IOAccelResource2 {
	OSDeclareDefaultStructors(RDNA4Resource)
public:
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

// A display pipe (one a framebuffer). For now the family's own behaviour, each transaction hook logged (the first few): what
// WindowServer's presents ask of a driver (AMD's pipe overrides these), before they go to the DCN.
class RDNA4DisplayPipe : public IOAccelDisplayPipe {
	OSDeclareDefaultStructors(RDNA4DisplayPipe)
	using super = IOAccelDisplayPipe;
	uint32_t logged = 0;
	bool note(const char *what, const void *arg)
	{
		if (logged >= 64)
			return false;
		logged++;
		rdna4Log("AMDRadeonRDNA4: accel: pipe %p: %s(%p)\n", this, what, arg);
		return true;
	}

public:
	uintptr_t validateTransaction(IOAccelDisplayPipeTransaction2 *t) APPLE_KEXT_OVERRIDE
	{
		note("validateTransaction", t);
		return super::validateTransaction(t);
	}
	uintptr_t performTransaction(IOAccelDisplayPipeTransaction2 *t) APPLE_KEXT_OVERRIDE
	{
		note("performTransaction", t);
		return super::performTransaction(t);
	}
	uintptr_t isTransactionComplete(IOAccelDisplayPipeTransaction2 *t) APPLE_KEXT_OVERRIDE { return super::isTransactionComplete(t); }
	uintptr_t submitTransaction(IOAccelDisplayPipeTransaction2 *t) APPLE_KEXT_OVERRIDE
	{
		note("submitTransaction", t);
		return super::submitTransaction(t);
	}
	uintptr_t beginTransaction(IOAccelEvent *e) APPLE_KEXT_OVERRIDE
	{
		note("beginTransaction", e);
		return super::beginTransaction(e);
	}
	uintptr_t initFramebufferResource(unsigned int i, IOAccelResource2 *r) APPLE_KEXT_OVERRIDE
	{
		note("initFramebufferResource", r);
		return super::initFramebufferResource(i, r);
	}
};
RDNA4_ACCEL_STRUCTORS(RDNA4DisplayPipe, IOAccelDisplayPipe)

// The GPU's AGDC service (AppleGraphicsControl's AppleGraphicsDeviceControl): IOPresentment maps GPUs, framebuffers and accelerators
// from these (ioPresentmentCreateAssociationMap), asking each for its vendor information and GPU capabilities; without one for our GPU
// it made no display pipes ("Unable to get AGDC information", "Unable to locate service dependencies"). Answered as Apple's paravirtual
// GPU's (AppleParavirtGPUControl::vendor_doDeviceAttribute): attribute 1, vendor information (0x2c bytes), 0x921, the GPU's
// capabilities (0xb0 bytes, its values); the rest (3: it makes its own framebuffers; 0x980; the framebuffers' own) not ours: refused.
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

	// the GPU's framebuffers (IOFramebuffers anywhere under its provider), retained, at most cap
	uint32_t framebuffers(IORegistryEntry **out, uint32_t cap)
	{
		uint32_t n = 0;
		IORegistryIterator *it = getProvider() ? IORegistryIterator::iterateOver(getProvider(), gIOServicePlane, kIORegistryIterateRecursively)
		                                        : nullptr;
		while (IORegistryEntry *e = it ? it->getNextObject() : nullptr)
			if (n < cap && e->metaCast("IOFramebuffer")) {
				e->retain();
				out[n++] = e;
			}
		OSSafeReleaseNULL(it);
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
			const uint64_t mask = static_cast<uint64_t>(~(0xffffffffu << n)) * 2;
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
	IOAccelSurface *newSurface() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelContext2 *new2DContext() APPLE_KEXT_OVERRIDE { return nullptr; }
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
	RDNA4Task *t = OSTypeAlloc(RDNA4Task);
	if (t && !t->init(this, 0, nullptr))
		OSSafeReleaseNULL(t);
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
	static uint32_t logged;
	if (logged++ < 64) {
		char name[32] = "";
		proc_selfname(name, sizeof(name));
		rdna4Log("AMDRadeonRDNA4: accel: user client of type 0x%x for %s: %s\n", static_cast<unsigned>(type), name,
		         gFamilyClients ? "the family's" : "refused");
	}
	return gFamilyClients ? super::newUserClient(task, security, type, handler) : kIOReturnUnsupported;
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
	RDNA4_DERIVED(RDNA4DisplayPipe, IOAccelDisplayPipe, 18),
	RDNA4_DERIVED_IN(RDNA4DeviceControl, AppleGraphicsDeviceControl, 26, 1),
};
constexpr size_t kDerivedCount = sizeof(kDerived) / sizeof(kDerived[0]);

// Kext `kext`'s thunks filled, checked; our metaclasses whose base is in it given the real superclasses.
void familyLoaded(uint32_t kext, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
	uint32_t missing = 0, wrongSlots = 0, thunks = 0;
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
		if (inSlot != rdna4AccelThunks[s.thunk] && wrongSlots++ < 4)
			rdna4Log("AMDRadeonRDNA4: accel: slot %u of %s does not hold %s\n", s.slot, s.vtable, kSymbols[s.thunk].name);
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
	rdna4Log("AMDRadeonRDNA4: accel: %s: %u thunks, %u not found, %u slots not the KDK's, %u classes not as expected: %s\n",
	         kKexts[kext].bundle, thunks, missing, wrongSlots, bad,
	         gReady ? "ready (debug.rdna4_accel=1 makes one, 2 starts it, 3 its displays)" : gFailed ? "NOT usable" : "solved");
}

RDNA4Accelerator *gAccel;   // made and attached (debug.rdna4_accel=1)
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
		gAccel->setProperty("MetalPluginName", "AMDRadeonRDNA4Metal");   // as the native spike's service (accelcensus.cpp, level 3)
		gAccel->setProperty("MetalPluginClassName", "RDNA4MTLDevice");
		// as Apple's paravirtual accelerator (its AcceleratorProperties): CoreDisplay presents through display pipe transactions when
		// these say so; without them it took the older surface path
		gAccel->setProperty("IOMatchCategory", "IOAccelerator");
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

SYSCTL_PROC(_debug, OID_AUTO, rdna4_accel, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, accelSysctl, "I",
            "AMDRadeonRDNA4: our own IOGraphicsAccelerator2: 1 makes and attaches one, 2 runs the family's start, 3 starts the display machine (docs/native-driver-plan.md)");
}   // namespace

// WindowServer's first open of the GPU wrangler (pvhooks.cpp wrapServiceOpen): with rdna4-accel=4 as a boot-arg, held until the boot
// thread has made and registered the accelerator (or given up), at most 60 s: WindowServer's wrangler lists each GPU's accelerator
// when it opens, and it opened before ours existed ("accel 0x0", 2026-11-20). (A test boot's option is not known this early.)
void RDNA4Accel::waitForWindowServer()
{
	uint32_t want = 0;
	if (!PE_parse_boot_argn("rdna4-accel", &want, sizeof(want)) || want != 4)
		return;
	rdna4Log("AMDRadeonRDNA4: accel: WindowServer opens the GPU wrangler: held until the accelerator is registered\n");
	for (int ms = 0; ms < 60000 && !gBootDone; ms += 50)
		IOSleep(50);
	rdna4Log("AMDRadeonRDNA4: accel: WindowServer let go (%s)\n", gFamilyClients ? "registered" : "NOT registered");
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
	thread_t th;   // (it waits for the GPU, then reads rdna4-accel)
	if (kernel_thread_start(accelAtBoot, nullptr, &th) == KERN_SUCCESS)
		thread_deallocate(th);
}
