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
#include <sys/sysctl.h>

#include "accelcensus.hpp"
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
const char *const kSymbols[] = {
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

bool gReady;   // the thunks are filled and checked
const char *kPath = "/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/MacOS/IOAcceleratorFamily2";
KernelPatcher::KextInfo kextAccel {
	"com.apple.iokit.IOAcceleratorFamily2", &kPath, 1, { true }, {}, KernelPatcher::KextInfo::Unloaded,
};
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

// The family objects the family's start asks for. The event machine's four (its stamps and barriers in command buffers) and the
// memory map's three (the GPU page table) are for submissions through the family, which nothing makes yet: they do nothing.
class RDNA4EventMachine : public IOAccelEventMachineFast2 {
	OSDeclareDefaultStructors(RDNA4EventMachine)
public:
	uintptr_t writeStamp(int, vendevtCommandRec *, unsigned int) APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t prepareBarrier(vendevtBarrierRec *) APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t completeBarrier(vendevtBarrierRec *) APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t writeBarrierElement(vendevtBarrierRec *, int, unsigned int) APPLE_KEXT_OVERRIDE { return 0; }
};
RDNA4_ACCEL_STRUCTORS(RDNA4EventMachine, IOAccelEventMachineFast2)

class RDNA4DisplayMachine : public IOAccelDisplayMachine {
	OSDeclareDefaultStructors(RDNA4DisplayMachine)
public:
	uintptr_t displayModeWillChange() APPLE_KEXT_OVERRIDE { return 0; }
	uintptr_t displayModeDidChange() APPLE_KEXT_OVERRIDE { return 0; }
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
		if (config)
			*reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(config) + 0x28) = kMaxFramebuffers;
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
	IOAccelSysMemory *newSysMemory() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelVidMemory *newVidMemory() APPLE_KEXT_OVERRIDE { return nullptr; }
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

IOReturn RDNA4Accelerator::newUserClient(task_t, void *, UInt32 type, IOUserClient **)
{
	rdna4Log("AMDRadeonRDNA4: accel: user client of type 0x%x refused (skeleton)\n", static_cast<unsigned>(type));
	return kIOReturnUnsupported;
}

namespace {
// Our classes: their metaclass, the base's real metaclass (the symbol) and our placeholder for it, our size, the base's KDK size
struct Derived {
	const OSMetaClass *ours;
	const char *baseMeta;
	const OSMetaClass *placeholder;
	uint32_t ourSize;
	uint32_t baseSize;
};
#define RDNA4_DERIVED(cls, base, n) \
	{ &cls::gMetaClass, "__ZN" #n #base "10gMetaClassE", &base::gMetaClass, sizeof(cls), k##base##Size }
const Derived kDerived[] = {
	RDNA4_DERIVED(RDNA4Accelerator, IOGraphicsAccelerator2, 22),
	RDNA4_DERIVED(RDNA4EventMachine, IOAccelEventMachineFast2, 24),
	RDNA4_DERIVED(RDNA4DisplayMachine, IOAccelDisplayMachine, 21),
	RDNA4_DERIVED(RDNA4Task, IOAccelTask, 11),
	RDNA4_DERIVED(RDNA4MemoryMap, IOAccelMemoryMap, 16),
	RDNA4_DERIVED(RDNA4Resource, IOAccelResource2, 16),
};
constexpr size_t kDerivedCount = sizeof(kDerived) / sizeof(kDerived[0]);

// The thunks filled from the loaded family, checked; our metaclasses given the real superclasses.
void familyLoaded(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
	uint32_t missing = 0, wrongSlots = 0;
	for (size_t i = 0; i < kThunks; i++) {
		const mach_vm_address_t a = patcher.solveSymbol(index, kSymbols[i], address, size);
		if (a)
			rdna4AccelThunks[i] = a;
		else if (missing++ < 4)
			rdna4Log("AMDRadeonRDNA4: accel: %s not found\n", kSymbols[i]);
		patcher.clearError();
	}
	for (const auto &s : kSlots) {
		const mach_vm_address_t vt = patcher.solveSymbol(index, s.vtable, address, size);
		patcher.clearError();
		const uintptr_t inSlot = vt ? reinterpret_cast<const uintptr_t *>(vt + 16)[s.slot] : 0;
		if (inSlot != rdna4AccelThunks[s.thunk] && wrongSlots++ < 4)
			rdna4Log("AMDRadeonRDNA4: accel: slot %u of %s does not hold %s\n", s.slot, s.vtable, kSymbols[s.thunk]);
	}
	// each of our classes: its metaclass's superclass link and size where OSMetaClass keeps them, then the real base's metaclass
	// (the base's size is checked when one is made: here the family's static constructors have not run yet, its metaclass reads 0)
	uint32_t bad = 0;
	const OSMetaClass *real[kDerivedCount] = {};
	for (size_t i = 0; i < kDerivedCount; i++) {
		const Derived &d = kDerived[i];
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
			*reinterpret_cast<const OSMetaClass **>(reinterpret_cast<uint8_t *>(const_cast<OSMetaClass *>(kDerived[i].ours)) + 16) = real[i];
		gReady = true;
	}
	rdna4Log("AMDRadeonRDNA4: accel: IOAcceleratorFamily2: %zu thunks, %u not found, %u slots not the KDK's, %u of %zu classes not as "
	         "expected: %s\n", kThunks, missing, wrongSlots, bad, kDerivedCount,
	         gReady ? "ready (debug.rdna4_accel=1 makes one, 2 starts it, 3 its displays)" : "NOT usable");
}

RDNA4Accelerator *gAccel;   // made and attached (debug.rdna4_accel=1)
bool gStarted;              // the family's start ran (debug.rdna4_accel=2)
bool gDisplays;             // the display machine started (debug.rdna4_accel=3)

// In steps, each logged: 1 makes one (the family's own constructor runs) and attaches it under the GPU; 2 runs the family's start;
// 3 initialises and starts the display machine on the GPU (the family's: a display pipe for each framebuffer it finds).
int accelSysctl SYSCTL_HANDLER_ARGS
{
	int value = gDisplays ? 3 : gStarted ? 2 : gAccel ? 1 : 0;
	const int err = sysctl_handle_int(oidp, &value, 0, req);
	if (err || !req->newptr)
		return err;
	if (value < 1 || value > 3)
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
	if (value == 3 && !gDisplays) {
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
	return 0;
}

SYSCTL_PROC(_debug, OID_AUTO, rdna4_accel, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, accelSysctl, "I",
            "AMDRadeonRDNA4: our own IOGraphicsAccelerator2: 1 makes and attaches one, 2 runs the family's start, 3 starts the display machine (docs/native-driver-plan.md)");
}   // namespace

void RDNA4Accel::start()
{
	lilu.onKextLoadForce(&kextAccel, 1, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		if (index == kextAccel.loadIndex)
			familyLoaded(patcher, index, address, size);
	}, nullptr);
	sysctl_register_oid(&sysctl__debug_rdna4_accel);
}
