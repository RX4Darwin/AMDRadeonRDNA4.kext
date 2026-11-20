//
//  rdna4accel.cpp — our own IOGraphicsAccelerator2 (docs/native-driver-plan.md, option D), milestone 1: a skeleton.
//
//  Our kext is in the boot kernel collection; IOAcceleratorFamily2 is in the system one, which a boot-collection kext cannot link
//  to. So the base class is declared from the KDK (src/ioaccel.hpp, tools/gen/accelgen.py) and every base function our class reaches
//  is a thunk (src/ioaccel_thunks.S) through rdna4AccelThunks, filled here when the family loads. Before anything is made:
//  - every thunk's function was found;
//  - each virtual's function sits in the running vtable at the KDK's slot (else the KDK's layout is not this macOS's);
//  - the running class has the KDK's size;
//  - our metaclass's superclass link and size are where OSMetaClass keeps them (+16, +32), then the link is set to the real class.
//
//  debug.rdna4_accel=1 makes one, attached to the GPU (RDNA4AccelCensus::gpu) and published. Milestone 1: IOService::start only (not the family's
//  start), no user clients; the 17 functions every accelerator must have return nothing.
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

class RDNA4Accelerator : public IOGraphicsAccelerator2 {
	OSDeclareDefaultStructors(RDNA4Accelerator)
	using super = IOGraphicsAccelerator2;

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;

	// what every accelerator must have (IOGraphicsAccelerator2's pure virtuals): nothing yet
	void *getStampMemory(unsigned int *) APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelEventMachine2 *newEventMachine() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelTask *createUserGPUTask() APPLE_KEXT_OVERRIDE { return nullptr; }
	void populateAccelConfig(IOAccelConfig *) APPLE_KEXT_OVERRIDE {}
	bool configureDevice(IOPCIDevice *) APPLE_KEXT_OVERRIDE { return false; }
	void teardownDevice(IOPCIDevice *) APPLE_KEXT_OVERRIDE {}
	IOAccelTask *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelDisplayMachine2 *newDisplayMachine() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelContext2 *newGLContext() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelContext2 *newCLContext() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelSurface *newSurface() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelContext2 *new2DContext() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelContext2 *newVideoContext() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelSysMemory *newSysMemory() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelVidMemory *newVidMemory() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelResource2 *newResource() APPLE_KEXT_OVERRIDE { return nullptr; }
	IOAccelMemoryMap *newMemoryMap() APPLE_KEXT_OVERRIDE { return nullptr; }
};

// OSDefineMetaClassAndStructors(RDNA4Accelerator, IOGraphicsAccelerator2) written out, without the class's superClass pointer: OpenCore's
// linker patches every class that has one against its superclass's vtable, which must be one it linked itself; IOAcceleratorFamily2's
// is not (not in the boot collection), so it refused the whole kext (the rig booted without it, 2026-11-20). The MetaClass gets the
// placeholder as its superclass here and the real one in familyLoaded.
RDNA4Accelerator::MetaClass RDNA4Accelerator::gMetaClass;
const OSMetaClass *const RDNA4Accelerator::metaClass = &RDNA4Accelerator::gMetaClass;
RDNA4Accelerator::RDNA4Accelerator(const OSMetaClass *meta) : IOGraphicsAccelerator2(meta) {}
RDNA4Accelerator::~RDNA4Accelerator() {}
const OSMetaClass *RDNA4Accelerator::getMetaClass() const { return &gMetaClass; }
RDNA4Accelerator::MetaClass::MetaClass() : OSMetaClass("RDNA4Accelerator", &IOGraphicsAccelerator2::gMetaClass, sizeof(RDNA4Accelerator)) {}
OSObject *RDNA4Accelerator::MetaClass::alloc() const { return new RDNA4Accelerator; }
RDNA4Accelerator::RDNA4Accelerator() : IOGraphicsAccelerator2(&gMetaClass) { gMetaClass.instanceConstructed(); }

bool RDNA4Accelerator::start(IOService *provider)
{
	if (!IOService::start(provider))   // milestone 1: not the family's start (it sets the GPU up through the functions above)
		return false;
	setProperty("RDNA4Accelerator", "skeleton (milestone 1)");
	registerService();
	rdna4Log("AMDRadeonRDNA4: accel: RDNA4Accelerator published (class %s, a kind of IOGraphicsAccelerator2: %s)\n",
	         getMetaClass()->getClassName(), metaCast("IOGraphicsAccelerator2") ? "yes" : "NO");
	return true;
}

void RDNA4Accelerator::stop(IOService *provider)
{
	IOService::stop(provider);
}

IOReturn RDNA4Accelerator::newUserClient(task_t, void *, UInt32 type, IOUserClient **)
{
	rdna4Log("AMDRadeonRDNA4: accel: user client of type 0x%x refused (skeleton)\n", static_cast<unsigned>(type));
	return kIOReturnUnsupported;
}

namespace {
// The thunks filled from the loaded family, checked; our metaclass given the real superclass.
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
	auto *real = reinterpret_cast<const OSMetaClass *>(patcher.solveSymbol(index, "__ZN22IOGraphicsAccelerator210gMetaClassE", address, size));
	patcher.clearError();
	auto *ours = reinterpret_cast<uint8_t *>(&RDNA4Accelerator::gMetaClass);
	auto **link = reinterpret_cast<const OSMetaClass **>(ours + 16);
	const uint32_t ourSize = *reinterpret_cast<const uint32_t *>(ours + 32);
	const bool layout = *link == &IOGraphicsAccelerator2::gMetaClass && ourSize == sizeof(RDNA4Accelerator);
	// (the class's size is checked when one is made: here the family's static constructors have not run yet, its metaclass reads 0)
	if (!missing && !wrongSlots && layout && real) {
		*link = real;
		gReady = true;
	}
	rdna4Log("AMDRadeonRDNA4: accel: IOAcceleratorFamily2: %zu thunks, %u not found, %u slots not the KDK's; the class %s, "
	         "our metaclass %s: %s\n", kThunks, missing, wrongSlots, real ? "found" : "NOT found", layout ? "as expected" : "NOT as expected",
	         gReady ? "ready (debug.rdna4_accel=1 publishes one)" : "NOT usable");
}

RDNA4Accelerator *gAccel;

int accelSysctl SYSCTL_HANDLER_ARGS
{
	int value = gAccel ? 1 : 0;
	const int err = sysctl_handle_int(oidp, &value, 0, req);
	if (err || !req->newptr)
		return err;
	if (value != 1)
		return EINVAL;
	if (gAccel)
		return 0;
	// (accelcensus.cpp's: an extern of its variable, which is in an anonymous namespace, linked as an unresolved lookup that OpenCore
	// bound to junk: reading it panicked the rig, 2026-11-20)
	IOService *gpu = RDNA4AccelCensus::gpu();
	if (!gReady || !gpu)
		return ENODEV;
	const OSMetaClass *base = RDNA4Accelerator::gMetaClass.getSuperClass();
	if (base->getClassSize() != kIOGraphicsAccelerator2Size) {
		rdna4Log("AMDRadeonRDNA4: accel: IOGraphicsAccelerator2 is 0x%x bytes here, the KDK's 0x%zx: not made\n", base->getClassSize(),
		         static_cast<size_t>(kIOGraphicsAccelerator2Size));
		return ENODEV;
	}
	auto *a = OSTypeAlloc(RDNA4Accelerator);
	if (!a || !a->init()) {
		OSSafeReleaseNULL(a);
		return ENOMEM;
	}
	a->setName("RDNA4Accelerator");
	if (!a->attach(gpu)) {
		a->release();
		return EBUSY;
	}
	if (!a->start(gpu)) {
		a->detach(gpu);
		a->release();
		return EIO;
	}
	gAccel = a;
	return 0;
}

SYSCTL_PROC(_debug, OID_AUTO, rdna4_accel, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, accelSysctl, "I",
            "AMDRadeonRDNA4: 1 publishes our own IOGraphicsAccelerator2 (skeleton; HUNG the rig on 2026-11-20, docs/native-driver-plan.md)");
}   // namespace

void RDNA4Accel::start()
{
	lilu.onKextLoadForce(&kextAccel, 1, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		if (index == kextAccel.loadIndex)
			familyLoaded(patcher, index, address, size);
	}, nullptr);
	sysctl_register_oid(&sysctl__debug_rdna4_accel);
}
