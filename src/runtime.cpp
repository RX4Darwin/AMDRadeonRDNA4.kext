//
//  runtime.cpp
//  RDNA4FB
//
//  The compute runtime user space reaches through RDNA4ComputeClient
//  (userclient.cpp, include/rdna4compute.h): VRAM buffers from a heap in
//  the compute pool, code objects loaded whole, and synchronous dispatches
//  on the stage-5 MEC queue through launch(). One lock serialises it all —
//  the queue has one ring and one kernarg slot.
//
//  A dispatch whose fence never comes leaves the queue in an unknown state
//  with no reset to recover it, so the runtime then refuses further
//  dispatches (RDNA4_FLAG_WEDGED) until the next boot.
//
//  Transfers: once the DMA self-test passes (RDNA4_FLAG_DMA), Write/Read go
//  through SDMA and a pinned bounce buffer in host memory, and buffers come
//  from VRAM past the BAR (gigabytes); otherwise the CPU copies through the
//  BAR into the pool's heap, as before.
//

#include "compute.hpp"
#include "userclient.hpp"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <libkern/c++/OSObject.h>

#define RLOG(fmt, ...)  IOLog("RDNA4FB: runtime: " fmt "\n", ## __VA_ARGS__)
#define CLOG(...) RLOG(__VA_ARGS__)

using namespace GfxReg;

namespace {

bool detailedFlipTrace() {
	return rdna4TraceLevel >= 2;
}

constexpr Reg kQuiesceIhRbCntl { 0, 0x0080 };
constexpr uint32_t kQuiesceBad = 0xffffffffu;
constexpr uint32_t kQuiesceIhEnable = 1u << 0;
constexpr uint32_t kQuiesceIhWptrWriteback = 1u << 8;
constexpr uint32_t kQuiesceIhIntr = 1u << 17;

class RDNA4PresentContext : public OSObject {
	OSDeclareDefaultStructors(RDNA4PresentContext);

public:
	RDNA4Compute *compute { nullptr };
	bool init() override { return OSObject::init(); }
};

OSDefineMetaClassAndStructors(RDNA4PresentContext, OSObject);

// Handles: (generation << 16) | (slot + 1), so 0 is never one and a stale
// handle of a reused slot does not match.
uint64_t makeHandle(uint32_t slot, uint16_t gen) {
	return (static_cast<uint64_t>(gen) << 16) | (slot + 1);
}

bool slotOf(uint64_t h, uint32_t max, uint32_t &slot, uint16_t &gen) {
	const uint32_t s = static_cast<uint32_t>(h & 0xffff);
	if ((h >> 32) || !s || s > max)
		return false;
	slot = s - 1;
	gen = static_cast<uint16_t>(h >> 16);
	return true;
}

uint16_t nextGen(uint16_t g) { return static_cast<uint16_t>(g == 0xffff ? 1 : g + 1); }

bool fenceReached(uint32_t current, uint32_t wanted) {
	return static_cast<int32_t>(current - wanted) >= 0;
}

uint32_t nextFence(uint32_t previous) {
	uint32_t value = previous + 1;
	return value ? value : 1;
}

constexpr uint64_t kCopyChunk = 4ull << 20;   // wired at a time
constexpr uint32_t kCodePad = 0x100;          // zeroed past the image: instruction prefetch

struct Locked {
	IOLock *l;
	explicit Locked(IOLock *lock) : l(lock) { IOLockLock(l); }
	~Locked() { IOLockUnlock(l); }
};

// Copy between a user range and kernel memory, `length` bytes.
IOReturn userCopy(task_t task, mach_vm_address_t user, void *kernel, uint64_t length, bool fromUser) {
	for (uint64_t done = 0; done < length;) {
		const uint64_t n = length - done < kCopyChunk ? length - done : kCopyChunk;
		IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
			user + done, n, fromUser ? kIODirectionOut : kIODirectionIn, task);
		if (!md)
			return kIOReturnNoMemory;
		IOReturn r = md->prepare();
		if (r == kIOReturnSuccess) {
			uint8_t *k = static_cast<uint8_t *>(kernel) + done;
			const IOByteCount c = fromUser ? md->readBytes(0, k, n) : md->writeBytes(0, k, n);
			if (c != n)
				r = kIOReturnVMError;
			md->complete();
		}
		md->release();
		if (r != kIOReturnSuccess)
			return r;
		done += n;
	}
	return kIOReturnSuccess;
}

} // namespace

// IOKit/IOMessage.h defines the will-power-off and will-restart messages
// delivered by RootDomain.h's registerPrioritySleepWakeInterest().  This is
// the same shutdown boundary used by xnu's GPU drivers; it is deliberately
// independent of the optional rdna4-pm user-client power table.
IOReturn RDNA4Compute::systemPowerMessage(void *target, void *, UInt32 messageType,
                                           IOService *, void *, vm_size_t) {
        auto *self = static_cast<RDNA4Compute *>(target);
        if (self && (messageType == kIOMessageSystemWillPowerOff ||
                     messageType == kIOMessageSystemWillRestart))
                self->quiesceForShutdown(messageType == kIOMessageSystemWillRestart ?
                                         "system restart" : "system power off");
        return kIOReturnSuccess;
}

void RDNA4Compute::registerShutdownInterest() {
        if (shutdownInterest)
                return;
        shutdownInterest = registerPrioritySleepWakeInterest(systemPowerMessage, this);
        CLOG("quiesce: shutdown interest %s",
             shutdownInterest ? "registered" : "unavailable");
}

void RDNA4Compute::defensiveStart() {
        if (!env.pci || !env.disc)
                return;
        // Survey every producer before writing any engine register.  A cold
        // card reports reset values (including zero), which is not evidence
        // that an engine is running; only positive activity justifies the
        // defensive shutdown below.
        const uint32_t ih = rd(IpDiscovery::HwOsssys, kQuiesceIhRbCntl);
        uint32_t sdmaRb[GfxReg::kSdmaInstances] {};
        uint32_t sdmaMcu[GfxReg::kSdmaInstances] {};
        for (uint32_t instance = 0; instance < GfxReg::kSdmaInstances; instance++) {
                sdmaRb[instance] = rd(IpDiscovery::HwGc, sdma(instance, SdmaQ0RbCntl));
                sdmaMcu[instance] = rd(IpDiscovery::HwGc, sdma(instance, SdmaMcuCntl));
        }
        // Reset values are not evidence that the CP is running.  In
        // particular, do not touch GRBM_GFX_CNTL on a cold card: the RLC
        // survey must first show a live CP by one of its positive indicators.
        const bool cpCanRun =
                (sv.rlcBootload != kQuiesceBad && (sv.rlcBootload & kRlcBootComplete)) ||
                (sv.mecPc != kQuiesceBad && sv.mecPc != 0) ||
                (sv.mePc != kQuiesceBad && sv.mePc != 0) ||
                (sv.rlcCntl != kQuiesceBad && (sv.rlcCntl & kRlcEnableF32));
        if (!cpCanRun)
                CLOG("quiesce: defensive start skipped CP/HQD survey; RLC/CP is not running");
        bool hqdActive[2][4] {};   // GC 12.0.x: 2 MEC pipes x 4 queues (gfx_v12_0.c:1415-1423)
        uint32_t hqdVmid[2][4] {};
        uint32_t gfxActive = kQuiesceBad;
        uint32_t me = kQuiesceBad;
        uint32_t mec = kQuiesceBad;
        if (cpCanRun) {
                for (uint32_t pipe = 0; pipe < 2; pipe++) {
                        for (uint32_t queue = 0; queue < 4; queue++) {
                                grbmSelect(1, pipe, queue, 0);
                                const uint32_t active = rdGc(CpHqdActive);
                                hqdActive[pipe][queue] = active != kQuiesceBad && (active & 1);
                                hqdVmid[pipe][queue] = rdGc(CpHqdVmid) & 0xf;
                        }
                }
                grbmSelect(0, 0, 0, 0);
                gfxActive = rdGc(CpRbActive);
                me = rdGc(CpMeCntl);
                mec = rdGc(CpMecRs64Cntl);
        }
        const bool inheritedGfx = cpCanRun && gfxActive != kQuiesceBad && (gfxActive & 1);
        bool inheritedHqd = false;
        for (uint32_t pipe = 0; pipe < 2; pipe++)
                for (uint32_t queue = 0; queue < 4; queue++)
                        inheritedHqd |= hqdActive[pipe][queue];

        bool found = ih != kQuiesceBad &&
                     (ih & (kQuiesceIhEnable | kQuiesceIhIntr | kQuiesceIhWptrWriteback));
        for (uint32_t instance = 0; instance < GfxReg::kSdmaInstances; instance++)
                found |= sdmaRb[instance] != kQuiesceBad && (sdmaRb[instance] & kSdmaRbEnable);
        found |= inheritedHqd || inheritedGfx;

        if (ih != kQuiesceBad && (ih & (kQuiesceIhEnable | kQuiesceIhIntr | kQuiesceIhWptrWriteback))) {
                CLOG("quiesce: defensive start disabling inherited IH RB_CNTL 0x%08x", ih);
                wr(IpDiscovery::HwOsssys, kQuiesceIhRbCntl,
                   ih & ~(kQuiesceIhEnable | kQuiesceIhIntr | kQuiesceIhWptrWriteback));
        }
        for (uint32_t instance = 0; instance < GfxReg::kSdmaInstances; instance++) {
                const uint32_t rb = sdmaRb[instance];
                if (rb == kQuiesceBad || !(rb & kSdmaRbEnable))
                        continue;
                const uint32_t mcu = sdmaMcu[instance];
                CLOG("quiesce: defensive start disabling inherited SDMA%u RB 0x%08x MCU 0x%08x",
                     instance, rb, mcu);
                wr(IpDiscovery::HwGc, sdma(instance, SdmaQ0RbCntl),
                   rb & ~(kSdmaRbEnable | kSdmaRbRptrWriteback));
                wr(IpDiscovery::HwGc, sdma(instance, SdmaQ0Doorbell), 0);
                if (mcu != kQuiesceBad)
                        wr(IpDiscovery::HwGc, sdma(instance, SdmaMcuCntl), mcu | kSdmaMcuHalt);
        }
        if (cpCanRun) {
                for (uint32_t pipe = 0; pipe < 2; pipe++) {
                        for (uint32_t queue = 0; queue < 4; queue++) {
                                if (!hqdActive[pipe][queue])
                                        continue;
                                const uint32_t vmid = hqdVmid[pipe][queue];
                                grbmSelect(1, pipe, queue, vmid);
                                CLOG("quiesce: defensive start draining HQD ME1 pipe%u queue%u VMID%u",
                                     pipe, queue, vmid);
                                wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
                                for (uint32_t us = 0; us < 100000 && (rdGc(CpHqdActive) & 1); us += 10)
                                        IODelay(10);
                                if (rdGc(CpHqdActive) & 1)
                                        CLOG("quiesce: defensive HQD pipe%u queue%u VMID%u did not drain",
                                             pipe, queue, vmid);
                                wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
                        }
                }
                grbmSelect(0, 0, 0, 0);
        }
        if (inheritedGfx || inheritedHqd) {
                CLOG("quiesce: defensive start halting inherited GFX/MEC CP_RB_ACTIVE 0x%08x",
                     gfxActive);
                if (me != kQuiesceBad)
                        wr(IpDiscovery::HwGc, CpMeCntl, me | kCpMePfpHalt | kCpMeMeHalt);
                if (inheritedHqd && mec != kQuiesceBad)
                        wr(IpDiscovery::HwGc, CpMecRs64Cntl, mec | kRs64Halt);
        }
        if (found && (env.pci->configRead16(kIOPCIConfigCommand) & kIOPCICommandBusMaster)) {
                CLOG("quiesce: defensive start clearing inherited PCI bus master last");
                env.pci->setBusMasterEnable(false);
        }
        if (!found)
                CLOG("quiesce: defensive start found no inherited activity; PCI bus master untouched");
}

void RDNA4Compute::quiesceForShutdown(const char *why) {
        bool alreadyQuiesced = false;
        if (rtLock) {
                IOLockLock(rtLock);
                alreadyQuiesced = shutdownQuiesced;
                if (!alreadyQuiesced) {
                        shutdownQuiesced = true;
                        rtReady = false;
                }
                IOLockUnlock(rtLock);
        } else {
                alreadyQuiesced = shutdownQuiesced;
                shutdownQuiesced = true;
        }
        if (alreadyQuiesced)
                return;
        CLOG("quiesce: begin %s", why ? why : "shutdown");

        // Do not tear down MMIO while a bring-up stage is using it. The
        // bring-up thread checks the flag between every stage and wakes this
        // waiter when it has stopped. A stuck stage is bounded so shutdown
        // still reaches the final Bus Master gate.
        if (rtLock) {
                uint64_t span = 0;
                nanoseconds_to_absolutetime(2000000000ull, &span);
                const uint64_t deadline = mach_absolute_time() + span;
                IOLockLock(rtLock);
                while (bringupRunning && mach_absolute_time() < deadline)
                        IOLockSleepDeadline(rtLock, &bringupRunning, deadline, THREAD_UNINT);
                if (bringupRunning)
                        CLOG("quiesce: bring-up did not stop within 2 s; continuing bounded shutdown");
                IOLockUnlock(rtLock);
        }
        if (!env.pci || !env.disc) {
                stopPresentationTimer();
                CLOG("quiesce: no GPU environment; complete");
                return;
        }

        // amdgpu fini order: interrupt producers, every HQD/MEC, GFX, SDMA,
        // presentation work, then the PCI bus-master gate last. Each poll is
        // bounded so a broken engine cannot hold system restart indefinitely.
        // W27: GFXOFF is lifted before the first GC access of the quiesce (this also clears
        // the persistent flag). If it cannot be lifted every GC access below is dropped.
        bootQueueLive = false;
        gcSnapValid = false;
        gcWake(0xfffffffeu);
        if (gcState == kGcHold)
                CLOG("quiesce: GFXOFF could not be lifted; the GC quiesce steps are dropped and engines may keep running");
        if (ihActive)
                ihStop();
        const uint32_t ih = rd(IpDiscovery::HwOsssys, kQuiesceIhRbCntl);
        wr(IpDiscovery::HwOsssys, kQuiesceIhRbCntl,
           ih & ~(kQuiesceIhEnable | kQuiesceIhIntr | kQuiesceIhWptrWriteback));
        wr(IpDiscovery::HwGc, CpMe1Pipe0IntCntl,
           rd(IpDiscovery::HwGc, CpMe1Pipe0IntCntl) & ~kCpTimeStampIntEnable);
        wr(IpDiscovery::HwGc, CpMe1Pipe1IntCntl,
           rd(IpDiscovery::HwGc, CpMe1Pipe1IntCntl) & ~kCpTimeStampIntEnable);

        // Restore the desktop while the display engine still has a valid
        // source surface. Pending client presents are completed as aborted by
        // restorePresentationLocked; simply dropping them would leave the
        // user's last frame latched across restart.
        if (rtLock) {
                IOLockLock(rtLock);
                if (presentActive)
                        CLOG("quiesce: present active; restoring desktop before reset");
                (void)restorePresentationLocked("shutdown");
                IOLockUnlock(rtLock);
        }
        stopPresentationTimer();

        auto drain = [this](uint32_t pipe, uint32_t queue, uint32_t vmid) {
                grbmSelect(1, pipe, queue, vmid);
                if (!(rdGc(CpHqdActive) & 1))
                        return;
                CLOG("quiesce: dequeue HQD ME1 pipe%u queue%u VMID%u", pipe, queue, vmid);
                wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
                for (uint32_t us = 0; us < 100000 && (rdGc(CpHqdActive) & 1); us += 10)
                        IODelay(10);
                if (rdGc(CpHqdActive) & 1)
                        CLOG("quiesce: HQD pipe%u queue%u VMID%u drain timed out; continuing",
                             pipe, queue, vmid);
                wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
        };
        if (rtLock) {
                IOLockLock(rtLock);
        }
        drain(0, 0, 0);
        for (const RtClient &client : clients)
                if (client.active)
                        drain(client.pipe, client.queue, client.vmid);
        grbmSelect(0, 0, 0, 0);
        wr(IpDiscovery::HwGc, CpMecRs64Cntl,
           rd(IpDiscovery::HwGc, CpMecRs64Cntl) | kRs64Halt);
        wr(IpDiscovery::HwGc, CpMeCntl,
           rd(IpDiscovery::HwGc, CpMeCntl) | kCpMePfpHalt | kCpMeMeHalt);

        for (uint32_t instance = 0; instance < GfxReg::kSdmaInstances; instance++) {
                const auto rb = sdma(instance, SdmaQ0RbCntl);
                const auto mcu = sdma(instance, SdmaMcuCntl);
                wr(IpDiscovery::HwGc, rb, rd(IpDiscovery::HwGc, rb) &
                   ~(kSdmaRbEnable | kSdmaRbRptrWriteback));
                wr(IpDiscovery::HwGc, sdma(instance, SdmaQ0Doorbell), 0);
                wr(IpDiscovery::HwGc, mcu, rd(IpDiscovery::HwGc, mcu) | kSdmaMcuHalt);
        }
        wr(IpDiscovery::HwGc, sdma(0, SdmaCntl),
           rd(IpDiscovery::HwGc, sdma(0, SdmaCntl)) & ~1u);
        if (rtLock)
                IOLockUnlock(rtLock);

        // dmaTeardown clears the AGP aperture before releasing its memory and
        // clears PCI Command.BusMaster only here, after all producers stopped.
        dmaTeardown(why ? why : "system shutdown");
        if (env.pci && (env.pci->configRead16(kIOPCIConfigCommand) & kIOPCICommandBusMaster)) {
                CLOG("quiesce: bus master remained set; clearing it as final gate");
                env.pci->setBusMasterEnable(false);
        }
        CLOG("quiesce: complete %s", why ? why : "shutdown");
}

void RDNA4Compute::publishRuntime(uint32_t stage) {
	if (!initRuntimeHeap()) {
		RLOG("not published: %s", !rtLock ? "no lock" : "no room for a heap in the pool");
		publishResult("runtime", "FAIL runtime heap");
		return;
	}
	IOLockLock(rtLock);
	rtStage = stage;
	rtReady = true;
	IOLockUnlock(rtLock);
	if (rtService) {
		// Wake re-bring-up keeps the original IOService and its user clients;
		// only the hardware/runtime state was rebuilt.
		rtService->setProperty("HeapBytes", devHeap.size() ? devHeap.size() : heap.size(), 64);
		rtService->setProperty("DMA", dmaReady);
		if (resumePending) {
			resumed = true;
			powerSleeping = false;
		}
		RLOG("user-space runtime up again: %s", RDNA4_COMPUTE_SERVICE);
		publishResult("runtime", "PASS service ready");
		return;
	}

	auto *svc = OSTypeAlloc(RDNA4ComputeService);
	if (!svc || !svc->init()) {
		OSSafeReleaseNULL(svc);
		RLOG("not published: could not create the service");
		publishResult("runtime", "FAIL service allocation");
		return;
	}
	svc->compute = this;
	svc->setProperty("IOUserClientClass", "RDNA4ComputeClient");
	svc->setProperty("ABI", static_cast<uint64_t>(RDNA4_COMPUTE_ABI), 32);
	svc->setProperty("HeapBytes", devHeap.size() ? devHeap.size() : heap.size(), 64);
	svc->setProperty("DMA", dmaReady);
	if (!svc->attach(env.pci)) {
		svc->release();
		RLOG("not published: could not attach to the GPU");
		publishResult("runtime", "FAIL service attach");
		return;
	}
	if (requestedPowerManagement()) {
		if (!svc->registerPowerManagement(env.pci)) {
			RLOG("not published: power management registration failed");
			svc->terminate();
			svc->release();
			publishResult("runtime", "FAIL power management registration");
			return;
		}
	} else {
		RLOG("power management disabled (rdna4-pm=1 required)");
	}
	svc->registerService();
	rtService = svc;             // the registry keeps it
	svc->release();
	if (dmaReady)
		RLOG("user-space runtime up: %s, DMA transfers, %llu MiB of VRAM for buffers at MC 0x%llx",
		     RDNA4_COMPUTE_SERVICE, devHeap.size() >> 20, vramMc(devHeap.base()));
	else
		RLOG("user-space runtime up: %s, CPU transfers, heap %llu MiB at MC 0x%llx",
		     RDNA4_COMPUTE_SERVICE, heap.size() >> 20, poolMc(kHeapOffset));
	publishResult("runtime", "PASS service ready");
	if (featureAllowed("flip")) {
		if (!initPresentationTimer())
			RLOG("present async disabled: no runtime work-loop timer");
		else
			RLOG("present timer installed: rdna4-flip is enabled");
	} else {
		RLOG("present selectors/timer disabled: rdna4-flip is not enabled");
	}
}

RDNA4Compute::RtBuffer *RDNA4Compute::bufferFor(const void *owner, uint64_t handle) {
	uint32_t slot;
	uint16_t gen;
	if (!owner || !slotOf(handle, kMaxBuffers, slot, gen))
		return nullptr;
	RtBuffer &b = buffers[slot];
	return b.owner == owner && b.gen == gen ? &b : nullptr;
}

RDNA4Compute::RtProgram *RDNA4Compute::programFor(const void *owner, uint64_t handle) {
	uint32_t slot;
	uint16_t gen;
	if (!owner || !slotOf(handle, kMaxPrograms, slot, gen))
		return nullptr;
	RtProgram &p = programs[slot];
	return p.owner == owner && p.gen == gen ? &p : nullptr;
}

RDNA4Compute::RtClient *RDNA4Compute::clientFor(const void *owner) {
	if (!owner)
		return nullptr;
	for (RtClient &c : clients)
		if (c.owner == owner && c.active)
			return &c;
	return nullptr;
}

/* Without rdna4-vm every connection still has a client record (W9 tracks
 * aborted clients across sleep), but no page tables or queue: only paths
 * that map memory or use the client's queue ask for this one. */
RDNA4Compute::RtClient *RDNA4Compute::vmClientFor(const void *owner) {
	RtClient *c = clientFor(owner);
	return vmEnabled && c && c->tableShadow ? c : nullptr;
}

IOReturn RDNA4Compute::ownerStateLocked(const void *owner) const {
	if (!rtReady)
		return kIOReturnNotReady;
	for (const RtClient &c : clients)
		if (c.owner == owner && c.active)
			return c.aborted ? kIOReturnAborted : kIOReturnSuccess;
	return kIOReturnSuccess;
}

bool RDNA4Compute::vmMap(RtClient &c, uint64_t va, uint64_t mc, uint64_t bytes, bool executable) {
	if (!poolCpu || !c.tableShadow || !bytes || (va & (GpuVm::kPageBytes - 1)) ||
	    (mc & (GpuVm::kPageBytes - 1)))
		return false;
	uint64_t physical = 0;
	if (!gpuPhysical(mc, physical))
		return false;
	const uint64_t end = va + ((bytes + GpuVm::kPageBytes - 1) & ~(GpuVm::kPageBytes - 1));
	if (end < va || end > GpuVm::kVaEnd)
		return false;
	auto entry = [&c](uint64_t off) -> uint64_t * {
		return c.tableShadow + off / sizeof(uint64_t);
	};
	uint64_t firstPt = ~0ull, lastPt = 0;
	for (uint64_t at = va, phys = physical; at < end;
	     at += GpuVm::kPageBytes, phys += GpuVm::kPageBytes) {
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > kVmTableBytes)
			return false;
		const uint32_t pdeIndex = GpuVm::index(at, 2);
		const uint64_t pdeOff = 0x2000 + static_cast<uint64_t>(pdeIndex) * 8;
		if (!*entry(pdeOff))
			*entry(pdeOff) = GpuVm::encodePde(c.rootPhys + ptOff, GpuVm::kValid, 0);
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		if (firstPt == ~0ull)
			firstPt = ptOff;
		lastPt = ptOff;
		/* amdgpu's composition for a VRAM BO on GC 12: VALID, READABLE, WRITEABLE, IS_PTE
		 * (gart_pte_flags, amdgpu_ttm.c:1477; gmc_v12_0.c:794-796), EXECUTABLE when asked
		 * (gmc_v12_0_get_vm_pte), MTYPE NC = 0, and no SNOOPED: amdgpu_ttm_tt_pde_flags adds it for
		 * VRAM only when the BO is cached (amdgpu_ttm.c:1456-1458). */
		uint64_t flags = GpuVm::kValid | GpuVm::kReadable | GpuVm::kWritable |
		                 (vmIsPteOff ? 0 : GpuVm::kIsPte);
		/* Linux's own tables on this card (rdna4-groundtruth vm-walk.txt) have EXE, READ and
		 * WRITE on every leaf (0x...5f1 / 0x...3f1): the CP fetches the EOP buffer, the ring and
		 * IBs with EXECUTE (IV src_data[1] 0x50 = READ|EXE; round 5: PERMISSION_FAULTS 8 on the EOP
		 * page mapped R|W).  Mesa maps every BO R|W|X the same way (ac_linux_drm.c:235).
		 * rdna4-vm-exec=0 restores the old R|W-only pages (negative control). */
		if (executable || !vmExecOff)
			flags |= GpuVm::kExecutable;
		flags = (flags | vmPteSet) & ~vmPteClear;   /* W22 diagnostics only, 0 otherwise */
		/* FRAG=4 says this PTE is part of a contiguous, 64 KiB-aligned run of
		 * sixteen: only when the whole aligned block lies inside this mapping
		 * (amdgpu_vm_pte_fragment); a lone 4 KiB page that merely happens to
		 * be 64 KiB-aligned must stay FRAG=0. */
		const uint64_t block = at & ~0xffffull;
		const bool fragment64k = block >= va && block + 0x10000 <= end &&
			((phys - (at - block)) & 0xffff) == 0;
		*entry(pteOff) = GpuVm::encodePte(phys, flags, fragment64k);
	}
	/* The root, PDB1, and the PDB0 entry that points at the PT all have to
	 * reach VRAM before the queue can walk this mapping. */
	if (firstPt == ~0ull || !vmTableSync(c, 0, 0x3000))
		return false;
	for (uint64_t pt = firstPt; pt <= lastPt; pt += 0x1000)
		if (!vmTableSync(c, static_cast<uint32_t>(pt), 0x1000))
			return false;
	return true;
}

bool RDNA4Compute::vmMapHost(RtClient &c, uint64_t va, const uint64_t *pageBuses,
                             uint64_t bytes, bool executable) {
	if (!poolCpu || !c.tableShadow || !pageBuses || !bytes ||
	    (va & (GpuVm::kPageBytes - 1)))
		return false;
	const uint64_t mapped = (bytes + GpuVm::kPageBytes - 1) & ~(GpuVm::kPageBytes - 1);
	const uint64_t end = va + mapped;
	if (mapped < bytes || end < va || end > GpuVm::kVaEnd)
		return false;
	auto entry = [&c](uint64_t off) -> uint64_t * {
		return c.tableShadow + off / sizeof(uint64_t);
	};
	uint64_t firstPt = ~0ull, lastPt = 0;
	uint64_t page = 0;
	for (uint64_t at = va; at < end; at += GpuVm::kPageBytes, page++) {
		if (pageBuses[page] & (GpuVm::kPageBytes - 1))
			return false;
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > kVmTableBytes)
			return false;
		const uint32_t pdeIndex = GpuVm::index(at, 2);
		const uint64_t pdeOff = 0x2000 + static_cast<uint64_t>(pdeIndex) * 8;
		if (!*entry(pdeOff))
			*entry(pdeOff) = GpuVm::encodePde(c.rootPhys + ptOff, GpuVm::kValid, 0);
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		if (firstPt == ~0ull)
			firstPt = ptOff;
		lastPt = ptOff;
		uint64_t flags = GpuVm::kSystem | GpuVm::kSnooped | GpuVm::kValid |
		                 GpuVm::kReadable | GpuVm::kWritable | (vmIsPteOff ? 0 : GpuVm::kIsPte);
		if (executable || !vmExecOff)
			flags |= GpuVm::kExecutable;
		/* Cached GTT on gfx12.0 uses MTYPE_NC, encoded as zero. */
		*entry(pteOff) = GpuVm::encodePte(pageBuses[page], flags, false);
	}
	if (firstPt == ~0ull || !vmTableSync(c, 0, 0x3000))
		return false;
	for (uint64_t pt = firstPt; pt <= lastPt; pt += 0x1000)
		if (!vmTableSync(c, static_cast<uint32_t>(pt), 0x1000))
			return false;
	return true;
}

bool RDNA4Compute::initRuntimeHeap() {
	if (!rtLock || !poolCpu || pool.size <= kHeapOffset)
		return false;
	if (heap.size())
		return true;
	/* DMA first: its self-test drives SDMA directly, before any client can;
	 * then the interrupts (W1), off for one boot if the last one hung in
	 * "ih: ...". */
	if (dmaInit()) {
		devHeapInit();
		if (featureAllowed("ih"))
			ihInit();
	}
	heap.init(kHeapOffset, pool.size - kHeapOffset, 4096, heapMap, sizeof(heapMap));
	return heap.size() != 0;
}

void RDNA4Compute::vmUnmap(RtClient &c, uint64_t va, uint64_t bytes) {
	if (!poolCpu || !c.tableShadow || !bytes || va & (GpuVm::kPageBytes - 1))
		return;
	const uint64_t end = va + ((bytes + GpuVm::kPageBytes - 1) & ~(GpuVm::kPageBytes - 1));
	uint64_t firstPt = ~0ull, lastPt = 0;
	for (uint64_t at = va; at < end && at >= va; at += GpuVm::kPageBytes) {
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > kVmTableBytes)
			break;
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		c.tableShadow[pteOff / sizeof(uint64_t)] = 0;
		if (firstPt == ~0ull)
			firstPt = ptOff;
		lastPt = ptOff;
	}
	for (uint64_t pt = firstPt; pt != ~0ull && pt <= lastPt; pt += 0x1000)
		if (!vmTableSync(c, static_cast<uint32_t>(pt), 0x1000))
			RLOG("vmid %u: page-table unmap sync failed", c.vmid);
}

bool RDNA4Compute::vmTableSync(RtClient &c, uint32_t offset, uint32_t bytes) {
	if (!c.tableShadow || offset > kVmTableBytes || bytes > kVmTableBytes - offset ||
	    !poolCpu || !devHeap.size() || offset & 0xfff || bytes & 0xfff) {
		RLOG("vmid %u: page-table sync arguments rejected (offset 0x%x bytes 0x%x)",
		     c.vmid, offset, bytes);
		return false;
	}
	if (vmTableCpu) {
		/* W22 variant T: the table lives in the CPU-visible pool and is written
		 * by the CPU through the BAR (amdgpu_vm_cpu style), then an HDP flush. */
		memcpy(poolCpu + vmTableCpu + offset, reinterpret_cast<uint8_t *>(c.tableShadow) + offset, bytes);
		(void)*reinterpret_cast<volatile uint32_t *>(poolCpu + vmTableCpu + offset);
		flushHdp();
		return true;
	}
	memcpy(poolCpu + kVmTableStage, reinterpret_cast<uint8_t *>(c.tableShadow) + offset, bytes);
	(void)*reinterpret_cast<volatile uint32_t *>(poolCpu + kVmTableStage);
	flushHdp();
	uint32_t pkt[Sdma::kCopyDwords];
	if (!Sdma::copyLinear(pkt, poolMc(kVmTableStage), c.rootMc + offset, bytes)) {
		RLOG("vmid %u: page-table COPY_LINEAR encoding failed", c.vmid);
		return false;
	}
	if (!sdmaRun(pkt, Sdma::kCopyDwords, 2000)) {
		RLOG("vmid %u: page-table SDMA sync failed (offset 0x%x bytes 0x%x)",
		     c.vmid, offset, bytes);
		return false;
	}
	return true;
}

bool RDNA4Compute::vmContextInit(RtClient &c) {
	if (c.vmid < 1 || c.vmid > 15 || !c.rootPhys)
		return false;
	const uint32_t n = c.vmid - 1;
	// W42 (Linux ground truth): GCVM_CONTEXT1..15_CNTL read 0x03fffc07 under amdgpu: the fault-enable defaults are bits 10..25 (16 bits;
	// the header names only 10..23, bits 24-25 are reset defaults Linux keeps through its read-modify-write). Ours wrote 0x00fffc07.
	const uint32_t faultDefaults = ((1u << 16) - 1) << 10;
	const uint32_t cntl = kVmCtxEnable | (GpuVm::kDepth << 1) |
		((GpuVm::kBlockSize - 9) << 4) | faultDefaults;
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1Cntl.dword + n }, cntl);
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtBaseLo.dword + 2 * n },
	   static_cast<uint32_t>(GpuVm::encodePde(c.rootPhys, GpuVm::kValid, 0)));
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtBaseHi.dword + 2 * n },
	   static_cast<uint32_t>(GpuVm::encodePde(c.rootPhys, GpuVm::kValid, 0) >> 32));
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtStartLo.dword + 2 * n }, 0);
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtStartHi.dword + 2 * n }, 0);
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtEndLo.dword + 2 * n }, 0xffffffff);
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtEndHi.dword + 2 * n }, 0xffff);
	return rdGc(Reg { 0, GcCtx1Cntl.dword + n }) == cntl;
}

bool RDNA4Compute::vmInvalidate(uint32_t vmid, const char *tag) {
	const Reg req { 0, GcInvEng0Req.dword + kGcInvEngGart }, ack { 0, GcInvEng0Ack.dword + kGcInvEngGart };
	/* gfxhub_v12_0_get_invalidate_req: legacy flush of L2 PTEs, PDE0-2 and
	 * L1 PTEs, with CLEAR_PROTECTION_FAULT_STATUS_ADDR (bit 24) left 0.
	 * Fault status is cleared through FAULT_CNTL instead (gcFaultClear). */
	const uint32_t request = (1u << vmid) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22) |
	                         (1u << 23);
	wr(IpDiscovery::HwGc, req, request);
	for (uint32_t us = 0; us < 100000; us += 10) {
		if (rdGc(ack) & (1u << vmid))
			return true;
		IODelay(10);
	}
	RLOG("vmid %u: VM invalidate timeout (%s), REQ 0x%08x ACK 0x%08x", vmid, tag,
	     rdGc(req), rdGc(ack));
	publishResult("vm", "FAIL invalidate timeout");
	return false;
}

void RDNA4Compute::releaseHost(RtBuffer &b) {
	if (!b.host)
		return;
	if (b.hostMap) {
		b.hostMap->release();
		b.hostMap = nullptr;
	}
	if (b.hostDma) {
		(void)b.hostDma->clearMemoryDescriptor();
		b.hostDma->release();
		b.hostDma = nullptr;
	}
	if (b.hostMemory) {
		b.hostMemory->release();
		b.hostMemory = nullptr;
	}
	b.hostUser = 0;
	b.host = false;
}

void RDNA4Compute::retireIbFences(RtClient &c) {
	if (!c.fenceCpu)
		return;
	const uint32_t current = *c.fenceCpu;
	uint32_t retired = 0;
	while (retired < c.ibOutstanding && fenceReached(current, c.ibFences[retired]))
		retired++;
	if (!retired)
		return;
	for (uint32_t i = retired; i < c.ibOutstanding; i++)
		c.ibFences[i - retired] = c.ibFences[i];
	c.ibOutstanding -= retired;
}

/* Boot-time proof of one translated MEC queue.  User clients never enter this
 * path: it runs before the runtime service is published. */
bool RDNA4Compute::vmBootSelfTest() {
	if (!initRuntimeHeap()) {
		publishResult("vm", "FAIL runtime heap");
		return false;
	}
	RtClient c {};
	const uint32_t vmid = 8, pipe = 0, queue = 1, doorbell = 0x1a;
	{
		/* Negative control: rdna4-vm-ispte=0 leaves bit 63 off every leaf PTE (the round 2-4
		 * encoding); the first VMID 8 access must then fault with MAPPING_ERROR (0x00800b3b). */
		uint32_t isPte = 1;
		vmIsPteOff = PE_parse_boot_argn("rdna4-vm-ispte", &isPte, sizeof(isPte)) && !isPte;
		uint32_t exec = 1;
		vmExecOff = PE_parse_boot_argn("rdna4-vm-exec", &exec, sizeof(exec)) && !exec;
		if (vmExecOff)
			RLOG("vm: rdna4-vm-exec=0: only the ring and IB pages are EXECUTABLE (negative control, "
			     "expect fault 0x00800880 on the EOP page)");
		if (vmIsPteOff)
			RLOG("vm: rdna4-vm-ispte=0: IS_PTE (bit 63) is NOT set on leaf PTEs (negative control, "
			     "expect fault 0x00800b3b)");
	}
	const uint32_t qoff = kVmQueueBase;
	/* The same client-VA layout as rtOpen: the EOP buffer, rptr report and
	 * wptr poll are addresses in the queue's VMID, as KFD programs them
	 * (kfd_mqd_manager_v12: eop_ring_buffer_address, read_ptr, write_ptr).
	 * Only the MQD stays a VMID0 MC address (hqdInitFor). */
	const uint64_t qva = GpuVm::kVaStart;
	const uint64_t eva = qva + 0x1000, rva = qva + 0x2000, wva = qva + 0x3000;
	const uint64_t dataVa = qva + 0x4000, fenceVa = qva + 0x5000;
	const uint32_t dataOff = qoff + kVmKernarg;
	uint64_t table = 0;
	if (!devHeap.size() || !devHeap.alloc(kVmTableBytes, table))
		return false;
	c.vmid = vmid;
	c.pipe = pipe;
	c.queue = queue;
	c.tableOffset = table;
	c.rootMc = vramMc(table);
	if (!gpuPhysical(c.rootMc, c.rootPhys)) {
		devHeap.free(table);
		return false;
	}
	c.tableShadow = reinterpret_cast<uint64_t *>(IOMalloc(kVmTableBytes));
	if (!c.tableShadow) {
		devHeap.free(table);
		return false;
	}
	bzero(c.tableShadow, kVmTableBytes);
	c.tableShadow[0] =
		GpuVm::encodePde(c.rootPhys + 0x1000, GpuVm::kValid, 2);
	c.tableShadow[0x1000 / sizeof(uint64_t) + GpuVm::index(GpuVm::kVaStart, 1)] =
		GpuVm::encodePde(c.rootPhys + 0x2000, GpuVm::kValid, 1);
	for (uint32_t off = 0; off < 0x7000; off += 4)
		*poolDw(qoff + off) = 0;
	c.kernargCpu = nullptr;
	c.fenceCpu = poolDw(qoff + kVmFence);
	const bool qMap = vmMap(c, qva, poolMc(qoff + kVmPq), kPqSize, true);   /* the CP fetches the ring with EXE */
	const bool eMap = qMap && vmMap(c, eva, poolMc(qoff + kVmEop), 0x1000, false);
	const bool rMap = eMap && vmMap(c, rva, poolMc(qoff + kVmRptr), 0x1000, false);
	const bool wMap = rMap && vmMap(c, wva, poolMc(qoff + kVmWptr), 0x1000, false);
	const bool dMap = wMap && vmMap(c, dataVa, poolMc(dataOff), 0x1000, false);
	const bool fMap = dMap && vmMap(c, fenceVa, poolMc(qoff + kVmFence), 0x1000, false);
	const bool qInit = fMap && c.pm4.init(poolDw(qoff + kVmPq), qva, kPqSize);
	if (!qInit) {
		RLOG("vm: boot page-table setup q=%d eop=%d r=%d w=%d data=%d fence=%d pm4=%d",
		     qMap, eMap, rMap, wMap, dMap, fMap, qInit);
		RLOG("vm: boot page-table or queue setup failed");
		IOFree(c.tableShadow, kVmTableBytes);
		devHeap.free(table);
		return false;
	}
	/* gfx_v12_0_init_compute_vmid: every KFD VMID (8-15) gets SH_MEM_CONFIG
	 * and SH_MEM_BASES (LDS_APP_BASE 1, SCRATCH_APP_BASE 2) before use.  The
	 * register is SH_MEM_BASES = 0x09e3 (gc_12_0_0_offset.h:9259); ShMemBases used to
	 * be 0x09e5 (SQ_DEBUG) in gfxregs.hpp; fixed with the emulator map. */
	/* SPI_GDBG_PER_VMID_CNTL.TRAP_EN (gfx_v12_0.c:1806-1809) is not set: there is
	 * no trap handler. */
	trail("vm: compute vmid apertures");
	for (uint32_t v = 8; v <= 15; v++) {
		grbmSelect(0, 0, 0, v);
		wr(IpDiscovery::HwGc, ShMemConfig, kShMemConfigDefault);
		wr(IpDiscovery::HwGc, ShMemBases, kShMemBasesDefault);
	}
	grbmSelect(0, 0, 0, 0);
	trail("vm: VM context enable");
	bool context = vmContextInit(c);
	if (!context)
		RLOG("vm: boot VM context setup failed (VMID %u)", vmid);
	bool hqd = false, inactive = true, fence = false, data = false, clean = false;
	uint32_t forceFail = 0;
	(void)PE_parse_boot_argn("rdna4-vm-force-fail", &forceFail, sizeof(forceFail));
	/* rdna4-vm-diag (default 0 = off): which hardware-writing diagnostics may run
	 * after the baseline queue fails.  bit0 E4 (IB in VMID 8 from a scratch VMID0
	 * queue), bit1 a (TAP_*_PHYSICAL), bit2 b (ctx0 covers the tables), bit3 c
	 * (MC-form pointers), bit4 E2 (mirror LOCAL_FB/LOCAL_SYSMEM), bit5 d (IS_PTE on
	 * leaf PTEs), bit6 e (no SNOOPED), bit7 g (EXECUTABLE), bit8 T (tables built by
	 * the CPU).  Any bit also runs the control test.  The E1 register dumps
	 * and the SDMA table readback are read-only and always on with rdna4-vm=1.
	 * The force-fail test hook only counts together with a diag mask. */
	uint32_t curPipe = pipe, curQueue = queue, curDoorbell = doorbell, curVmid = vmid;
	uint32_t diag = 0;
	(void)PE_parse_boot_argn("rdna4-vm-diag", &diag, sizeof(diag));
	diag &= 0xfff;
	if (!diag)
		forceFail = 0;

	/* W17 diagnostics: page-table readback (through SDMA: the tables live in
	 * VRAM past the BAR) next to the shadow copy.  It proves the SDMA writes
	 * landed, not how the walker interprets the entries. */
	auto inVram = [this](uint64_t mc) { return mc >= sv.fbMcBase && mc <= sv.fbMcTop; };
	auto dumpSetup = [&](const char *tag) {
		RLOG("vm: diag%s: rootMc 0x%llx rootPhys 0x%llx (FB_OFFSET 0x%x, fbMcBase 0x%llx)", tag,
		     static_cast<unsigned long long>(c.rootMc), static_cast<unsigned long long>(c.rootPhys),
		     sv.gcFbOffset, static_cast<unsigned long long>(sv.fbMcBase));
		const uint32_t n = vmid - 1;
		RLOG("vm: diag%s: CONTEXT%u CNTL 0x%08x BASE 0x%08x:%08x START 0x%08x:%08x END 0x%08x:%08x",
		     tag, vmid, rdGc(Reg { 0, GcCtx1Cntl.dword + n }),
		     rdGc(Reg { 0, GcCtx1PtBaseHi.dword + 2 * n }), rdGc(Reg { 0, GcCtx1PtBaseLo.dword + 2 * n }),
		     rdGc(Reg { 0, GcCtx1PtStartHi.dword + 2 * n }), rdGc(Reg { 0, GcCtx1PtStartLo.dword + 2 * n }),
		     rdGc(Reg { 0, GcCtx1PtEndHi.dword + 2 * n }), rdGc(Reg { 0, GcCtx1PtEndLo.dword + 2 * n }));
		RLOG("vm: diag%s: L2_CNTL 0x%08x CNTL2 0x%08x CNTL3 0x%08x CNTL4 0x%08x TLB 0x%08x FAULT_CNTL 0x%08x",
		     tag, rdGc(GcL2Cntl), rdGc(GcL2Cntl2), rdGc(GcL2Cntl3), rdGc(GcL2Cntl4),
		     rdGc(GcMxL1TlbCntl), rdGc(GcL2FaultCntl));
		RLOG("vm: diag%s: invalidate engine 17 REQ 0x%08x ACK 0x%08x SEM 0x%08x, L2_STATUS 0x%08x", tag,
		     rdGc(Reg { 0, GcInvEng0Req.dword + kGcInvEngGart }),
		     rdGc(Reg { 0, GcInvEng0Ack.dword + kGcInvEngGart }),
		     rdGc(Reg { 0, GcInvEng0Sem.dword + kGcInvEngGart }), rdGc(GcL2Status));
		RLOG("vm: diag%s: CONTEXT0 CNTL 0x%08x BASE 0x%08x:%08x START 0x%08x:%08x END 0x%08x:%08x", tag,
		     rdGc(GcCtx0Cntl), rdGc(GcCtx0PtBaseHi), rdGc(GcCtx0PtBaseLo), rdGc(GcCtx0PtStartHi),
		     rdGc(GcCtx0PtStartLo), rdGc(GcCtx0PtEndHi), rdGc(GcCtx0PtEndLo));
	};
	/* E1: both hubs' MC-window and aperture registers (vmDumpHubWindows), and
	 * the four windows E2 may mirror.  On this card the firmware (IMU/SMU) has
	 * already mirrored most of the MM hub into the GC hub by the time we run
	 * (hw log 202618 line 262); amdgpu's imu_v12_init_gfxhub_settings copies
	 * exactly these four: LOCAL_FB and LOCAL_SYSMEM start/end. */
	struct Mir { const char *name; uint32_t gc, mm; };
	static const Mir mir[] = {
		{ "LOCAL_FB_START", 0x15b1, 0x04d1 }, { "LOCAL_FB_END", 0x15b2, 0x04d2 },
		{ "LOCAL_SYSMEM_START", 0x15ae, 0x04ce }, { "LOCAL_SYSMEM_END", 0x15af, 0x04cf },
	};
	const uint32_t nMir = sizeof(mir) / sizeof(mir[0]);
	auto dumpWindows = [&](const char *tag) { vmDumpHubWindows(tag); };
	auto dumpTables = [&](const char *tag) {
		const uint32_t bytes = 0x4000;
		for (uint32_t i = 0; i < bytes / 4; i++)
			*poolDw(kVmTableStage + i * 4) = 0xdeadbeef;
		flushHdp();
		uint32_t pkt[Sdma::kCopyDwords];
		if (!Sdma::copyLinear(pkt, c.rootMc, poolMc(kVmTableStage), bytes) ||
		    !sdmaRun(pkt, Sdma::kCopyDwords, 2000)) {
			RLOG("vm: diag%s: page-table readback via SDMA failed", tag);
			logGcFault("vm: diag readback");
			return;
		}
		const uint64_t *got = reinterpret_cast<const uint64_t *>(poolCpu + kVmTableStage);
		struct Ent { const char *name; uint32_t off; };
		const Ent ents[] = {
			{ "PDB2[0]", 0 },
			{ "PDB1[idx]", 0x1000 + GpuVm::index(qva, 1) * 8 },
			{ "PDB0[idx]", 0x2000 + GpuVm::index(qva, 2) * 8 },
			{ "PT[pq]", 0x3000 + GpuVm::index(qva, 3) * 8 },
			{ "PT[eop]", 0x3000 + GpuVm::index(eva, 3) * 8 },
			{ "PT[rptr]", 0x3000 + GpuVm::index(rva, 3) * 8 },
			{ "PT[wptr]", 0x3000 + GpuVm::index(wva, 3) * 8 },
			{ "PT[data]", 0x3000 + GpuVm::index(dataVa, 3) * 8 },
			{ "PT[fence]", 0x3000 + GpuVm::index(fenceVa, 3) * 8 },
		};
		for (uint32_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
			const uint64_t want = c.tableShadow[ents[i].off / 8], have = got[ents[i].off / 8];
			RLOG("vm: diag%s: %s @+0x%x hw 0x%016llx expected 0x%016llx %s", tag, ents[i].name,
			     ents[i].off, static_cast<unsigned long long>(have),
			     static_cast<unsigned long long>(want), have == want ? "ok" : "MISMATCH");
		}
		struct Page { const char *name; uint64_t mc; };
		const Page pages[] = {
			{ "pq", poolMc(qoff + kVmPq) }, { "eop", poolMc(qoff + kVmEop) },
			{ "rptr", poolMc(qoff + kVmRptr) }, { "wptr", poolMc(qoff + kVmWptr) },
			{ "data", poolMc(dataOff) }, { "fence", poolMc(qoff + kVmFence) },
		};
		for (uint32_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++)
			RLOG("vm: diag%s: %s page MC 0x%llx is %s", tag, pages[i].name,
			     static_cast<unsigned long long>(pages[i].mc),
			     inVram(pages[i].mc) ? "VRAM" : "SYSMEM");
	};
	const uint64_t ibVa = qva + 0x6000;      /* E4's IB page (VMID 8 VA) */
	const uint32_t ibOff = dataOff;           /* same physical page as the data word, IB at +0x100 */
	auto buildTables = [&]() -> bool {
		bzero(c.tableShadow, kVmTableBytes);
		c.tableShadow[0] = GpuVm::encodePde(c.rootPhys + 0x1000, GpuVm::kValid, 2);
		c.tableShadow[0x1000 / sizeof(uint64_t) + GpuVm::index(GpuVm::kVaStart, 1)] =
			GpuVm::encodePde(c.rootPhys + 0x2000, GpuVm::kValid, 1);
		return vmMap(c, qva, poolMc(qoff + kVmPq), kPqSize, true) &&
		       vmMap(c, eva, poolMc(qoff + kVmEop), 0x1000, false) &&
		       vmMap(c, rva, poolMc(qoff + kVmRptr), 0x1000, false) &&
		       vmMap(c, wva, poolMc(qoff + kVmWptr), 0x1000, false) &&
		       vmMap(c, dataVa, poolMc(dataOff), 0x1000, false) &&
		       vmMap(c, fenceVa, poolMc(qoff + kVmFence), 0x1000, false) &&
		       vmMap(c, ibVa, poolMc(ibOff), 0x1000, true);
	};
	auto hqdStop = [&]() {
		grbmSelect(1, curPipe, curQueue, curVmid);
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
		bool idle = false;
		for (uint32_t us = 0; us < 100000; us += 10) {
			if (!(rdGc(CpHqdActive) & 1)) { idle = true; break; }
			IODelay(10);
		}
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
		if (!idle)
			RLOG("vm: boot HQD dequeue timed out (ACTIVE 0x%08x)", rdGc(CpHqdActive));
		grbmSelect(0, 0, 0, 0);
		return idle;
	};
	/* One activation + kick + wait of the test queue; the caller has set up
	 * the context.  Returns whether fence, data and a clean fault status all
	 * came out right (fence/data/clean are left set for the caller). */
	auto attempt = [&](const char *tag, bool first) -> bool {
		/* A fault latched before this test (an earlier boot, another
		 * engine) would be blamed on the queue below: show it, then clear
		 * it the way amdgpu does. */
		const uint32_t stale = rdGc(GcL2FaultStatusLo);
		if (stale)
			logGcFault("vm: boot: fault latched before the test");
		gcFaultClear();
		trail(first ? "vm: context invalidate" : "vm: variant invalidate");
		(void)vmInvalidate(curVmid, "boot context enable");
		if (!first) {
			/* A fresh queue: clear the report/poll/data/fence pages and the
			 * ring, and start the PM4 writer over. */
			for (uint32_t off = 0; off < 0x6000; off += 4)
				*poolDw(qoff + off) = 0;
			c.pm4.init(poolDw(qoff + kVmPq), qva, kPqSize);
		}
		trail(first ? "vm: HQD activate" : "vm: variant HQD activate");
		hqd = hqdInitFor(false, curPipe, curQueue, curVmid, poolMc(qoff + kVmMqd),
		                 eva >> 8, qva >> 8, rva, wva, curDoorbell);
		*poolDw(dataOff) = 0;
		*c.fenceCpu = 0;
		flushHdp();
		uint32_t pkt[8];
		const uint32_t value = 0x564d0001;
		c.pm4.emit(pkt, Pm4::writeData(pkt, dataVa, 0x600df00d));
		c.pm4.emit(pkt, Pm4::releaseMem(pkt, fenceVa, value));
		/* Anything set here came from the activation (the MQD access), not
		 * from the packets: one line makes a failure below attributable. */
		RLOG("vm: boot%s: fault status before the kick 0x%08x (stale 0x%08x, cleared)", tag,
		     rdGc(GcL2FaultStatusLo), stale);
		trail(first ? "vm: queue kick" : "vm: variant queue kick");
		pm4Kick(c.pm4, curDoorbell, c.pm4.wptr());
		fence = false;
		for (uint32_t us = 0; us < 200000 && !fence; us += 10) {
			fence = *c.fenceCpu == value;
			if (!fence)
				IODelay(10);
		}
		data = *poolDw(dataOff) == 0x600df00d;
		/* The queue's own accesses (MQD, ring, EOP, rptr report, WRITE_DATA,
		 * fence) must all translate: any GC hub fault here is a setup bug,
		 * even when the fault-default page let the fence land. */
		const uint32_t status = rdGc(GcL2FaultStatusLo);
		clean = status == 0;
		if (!fence || !data || !clean) {
			RLOG("vm: boot%s queue test failed (fence 0x%08x, data 0x%08x, fault status 0x%08x)", tag,
			     *c.fenceCpu, *poolDw(dataOff), status);
			/* One bounded dump for the real-card log: where the HQD stopped,
			 * what the CP reported, and what the doorbell holds. */
			grbmSelect(1, curPipe, curQueue, curVmid);
			{
				const uint32_t dbc = rdGc(CpHqdPqDoorbell);
				RLOG("vm: boot%s: MEC pipe %u queue %u, HQD doorbell control 0x%08x: the MEC %s the doorbell",
				     tag, curPipe, curQueue, dbc, (dbc >> 31) ? "did NOT service (HIT still set)" : "consumed");
			}
			RLOG("vm: boot%s HQD: MQD 0x%08x:%08x MQD_CONTROL 0x%08x EOP 0x%08x:%08x "
			     "rptr report 0x%08x:%08x wptr poll 0x%08x:%08x", tag,
			     rdGc(CpMqdBaseAddrHi), rdGc(CpMqdBaseAddr), rdGc(CpMqdControl),
			     rdGc(CpHqdEopBaseHi), rdGc(CpHqdEopBase), rdGc(CpHqdPqRptrReportHi),
			     rdGc(CpHqdPqRptrReport), rdGc(CpHqdPqWptrPollHi), rdGc(CpHqdPqWptrPoll));
			grbmSelect(0, 0, 0, 0);
			RLOG("vm: boot%s ring: wptr %llu dwords, rptr report in memory 0x%08x, "
			     "doorbell dword %u readback 0x%llx", tag,
			     static_cast<unsigned long long>(c.pm4.wptr()), *poolDw(qoff + kVmRptr), curDoorbell,
			     static_cast<unsigned long long>(doorbells[curDoorbell / 2]));
			logComputeQueueState("vm: boot", curPipe, curQueue, curVmid);
			return false;
		}
		return true;
	};
	if (context) {
		dumpSetup(" before kick");
		dumpWindows(" before kick");
		dumpTables(" before kick");
		/* F (bit 9): amdgpu's fault handling.  GCVM_L2_CNTL.ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY = 1
		 * (gfxhub_v12_0.c:248) and GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR = the dummy page
		 * (gfxhub_v12_0.c:186-191, amdgpu's adev->dummy_page_addr, a system page), so a
		 * faulting access is answered from the dummy page instead of stalling its engine.
		 * With the bit set the address is a SYSTEM address, so the page must be host memory
		 * that is ours: a zeroed, DMA-mapped page allocated here, scrubbed and freed after.
		 * (A VRAM page would be read as a host physical address with this bit set.) */
		IOBufferMemoryDescriptor *fpMem = nullptr;
		IODMACommand *fpDma = nullptr;
		uint32_t fpSavedL2 = 0, fpSavedLo = 0, fpSavedHi = 0;
		bool fpOn = false;
		if (diag & 512) {
			uint64_t fpBus = 0;
			bool fpOk = dmaReady && busMasterSet;
			if (fpOk) {
				fpMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
					kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 0x1000,
					0x000000fffffff000ull);
				UInt64 off = 0;
				IODMACommand::Segment64 seg {};
				UInt32 nseg = 1;
				fpDma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 40, 0,
				                                        IODMACommand::kMapped, 0, 1);
				fpOk = fpMem && fpDma && fpMem->getBytesNoCopy() &&
				       fpDma->setMemoryDescriptor(fpMem) == kIOReturnSuccess &&
				       fpDma->gen64IOVMSegments(&off, &seg, &nseg) == kIOReturnSuccess && nseg == 1 &&
				       seg.fLength >= 0x1000 && !(seg.fIOVMAddr & 0xfff);
				if (fpOk) {
					bzero(fpMem->getBytesNoCopy(), 0x1000);
					fpMem->performOperation(kIOMemoryIncoherentIOFlush, 0, 0x1000);
					fpBus = seg.fIOVMAddr;
				}
			}
			if (!fpOk) {
				RLOG("vm: F: no DMA-mapped dummy page (DMA %s); fault default page left as it was",
				     dmaReady ? "up" : "not up");
			} else {
				trail("vm: F fault default page");
				fpSavedL2 = rdGc(GcL2Cntl);
				fpSavedLo = rdGc(GcL2FaultDefaultLo);
				fpSavedHi = rdGc(GcL2FaultDefaultHi);
				wr(IpDiscovery::HwGc, GcL2FaultDefaultLo, static_cast<uint32_t>(fpBus >> 12));
				wr(IpDiscovery::HwGc, GcL2FaultDefaultHi, static_cast<uint32_t>(fpBus >> 44));
				wr(IpDiscovery::HwGc, GcL2Cntl, fpSavedL2 | kL2DefaultPageToSys);
				(void)gcHubFlush();
				fpOn = true;
				RLOG("vm: F: fault default page = system page bus 0x%llx, L2_CNTL 0x%08x -> 0x%08x "
				     "(was default addr 0x%08x:%08x)", static_cast<unsigned long long>(fpBus),
				     fpSavedL2, rdGc(GcL2Cntl), fpSavedHi, fpSavedLo);
			}
		}
		if (forceFail) {
			/* Test hook (off by default): break the context's range so the
			 * baseline faults and the diagnostics below run. */
			RLOG("vm: boot: rdna4-vm-force-fail set, END_ADDR forced to 0 for the baseline");
			wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtEndLo.dword + 2 * (vmid - 1) }, 0);
			wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtEndHi.dword + 2 * (vmid - 1) }, 0);
		}
		(void)attempt("", true);
		if (forceFail == 2 && !(fence && data && clean)) {
			/* Test hook, mode 2: the fault is "HQD side only": put the context back,
			 * so the tests below (an IB through the same tables) pass in the emulator. */
			if (hqdStop())
				hqd = false;
			(void)vmContextInit(c);
			(void)vmInvalidate(vmid, "force-fail 2");
		}
		if (!(fence && data && clean)) {
			const bool baseFence = fence, baseData = data, baseClean = clean;
			dumpWindows(" after fault");
			/* Diagnostics (only with rdna4-vm-diag).  Round 3 on the card showed that
			 * after the first VM fault the same HQD slot is not serviced again (its
			 * doorbell HIT stays set, no fault is latched), so every test below gets
			 * a FRESH queue slot on MEC pipes 1-3 (pipe 0 hosts the kernel ring and
			 * the baseline queue), and the IB probe (E4) runs first, as a scratch
			 * VMID0 queue of its own, so a fault can never wedge the kernel ring.
			 * Each test changes one thing, logs its fault status and puts the
			 * setting back.  A passing test is only reported; the runtime stays
			 * disabled this boot.  A dequeue that times out abandons the rest. */
			bool abandon = !diag;
			if (!diag) {
				RLOG("vm: baseline failed; write diagnostics are off (rdna4-vm-diag=4065 runs "
				     "F, E4, control, o, V, d, e, g, T; 30 runs a, b, c, E2)");
			} else if (!hqdStop()) {
				RLOG("vm: HQD dequeue timed out; diagnostics abandoned");
				abandon = true;
			} else {
				hqd = false;   /* stopped: nothing left to dequeue at the end */
				(void)vmInvalidate(vmid, "variant cleanup");
			}
			const uint32_t savedL4 = rdGc(GcL2Cntl4);
			const uint32_t savedC0[4] = { rdGc(GcCtx0PtStartLo), rdGc(GcCtx0PtStartHi),
			                              rdGc(GcCtx0PtEndLo), rdGc(GcCtx0PtEndHi) };
			const uint64_t savedPhys = c.rootPhys, savedMc = c.rootMc;

			bool e4Wedged = false;
			/* E4: one IB fetched in VMID 8 from a scratch VMID 0 (privileged, like
			 * amdgpu's kernel compute rings, gfx_v12_0.c:3250-3251) queue on MEC
			 * pipe 1 queue 3, slot 7's pool area (unused at boot).  Passes: tables
			 * and hub are fine and the fault is HQD-side.  Faults: hub or tables. */
			if (!abandon && (diag & 1)) {
				trail("vm: E4 IB probe");
				const uint32_t q7 = kVmQueueBase + 7 * kVmQueueStride, db = 0x32;
				for (uint32_t off = 0; off < 0x7000; off += 4)
					*poolDw(q7 + off) = 0;
				uint32_t ibPkt[8];
				const uint32_t ibDw = Pm4::writeData(ibPkt, dataVa, 0x600df00e);
				for (uint32_t i = 0; i < ibDw; i++)
					*poolDw(ibOff + 0x100 + i * 4) = ibPkt[i];
				*poolDw(dataOff) = 0;
				flushHdp();
				Pm4::Queue eq;
				const bool mapped = ibDw && vmMap(c, ibVa, poolMc(ibOff), 0x1000, true);
				const bool queueOk = mapped && eq.init(poolDw(q7 + kVmPq), poolMc(q7 + kVmPq), kPqSize);
				gcFaultClear();
				(void)vmInvalidate(vmid, "E4");
				bool e4Fence = false, e4Hqd = false;
				volatile uint32_t *e4FenceCpu = poolDw(q7 + kVmFence);
				const uint32_t seq = 0x564d0004;
				if (queueOk) {
					e4Hqd = hqdInitFor(false, 1, 3, 0, poolMc(q7 + kVmMqd), poolMc(q7 + kVmEop) >> 8,
					                   poolMc(q7 + kVmPq) >> 8, poolMc(q7 + kVmRptr),
					                   poolMc(q7 + kVmWptr), db);
					uint32_t pkt[8];
					if (e4Hqd &&
					    eq.emit(pkt, Pm4::indirectBufferCompute(pkt, ibVa + 0x100, ibDw, vmid)) &&
					    eq.emit(pkt, Pm4::releaseMem(pkt, poolMc(q7 + kVmFence), seq))) {
						pm4Kick(eq, db, eq.wptr());
						for (uint32_t us = 0; us < 200000 && !e4Fence; us += 10) {
							e4Fence = *e4FenceCpu == seq;
							if (!e4Fence)
								IODelay(10);
						}
					}
				}
				const uint32_t e4Status = rdGc(GcL2FaultStatusLo);
				RLOG("vm: E4: IB from a VMID0 queue (pipe 1 queue 3), fetched in VMID %u: mapped %d "
				     "queue %d fence %d data 0x%08x fault status 0x%08x => %s", vmid, mapped, e4Hqd,
				     e4Fence, *poolDw(dataOff), e4Status,
				     e4Fence && *poolDw(dataOff) == 0x600df00e && !e4Status
				         ? "PASS (walker and tables work; the VMID 8 HQD is the problem)"
				         : "fail (hub or tables)");
				if (e4Status)
					logGcFault("vm: E4");
				if (e4Hqd) {
					grbmSelect(1, 1, 3, 0);
					{
						const uint32_t dbc = rdGc(CpHqdPqDoorbell);
						RLOG("vm: E4: doorbell control 0x%08x: the MEC %s the doorbell", dbc,
						     (dbc >> 31) ? "did NOT service (HIT still set)" : "consumed");
					}
					wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
					bool idle = false;
					for (uint32_t us = 0; us < 100000 && !idle; us += 10) {
						idle = !(rdGc(CpHqdActive) & 1);
						if (!idle)
							IODelay(10);
					}
					wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
					grbmSelect(0, 0, 0, 0);
					if (!idle)
						RLOG("vm: E4: scratch queue did not dequeue (pipe 1 is not used again; tests move to pipe 2)");
						e4Wedged = true;
				}
				gcFaultClear();
				(void)vmInvalidate(vmid, "E4 clean");
			}

			/* The queue tests: control (nothing changed), then one change each. */
			enum { kControl, kA, kB, kC, kE2, kD, kE, kG, kT, kO, kV, kTests };
			static const uint32_t bitOf[kTests] = { 0, 2, 4, 8, 16, 32, 64, 128, 256, 1024, 2048 };
			static const char *const nameOf[kTests] = {
				" control (no change, fresh pipe)", " variant a (TAP_*_PHYSICAL=1)",
				" variant b (ctx0 covers tables)", " variant c (MC-form table pointers)",
				" variant E2 (GC windows := MM)", " variant d (OLD encoding: NO IS_PTE on leaf PTEs)",
				" variant e (SNOOPED on VRAM PTEs)", " variant g (OLD encoding: leaf PTEs not EXECUTABLE except ring/IB)",
				" variant T (tables built by the CPU in the pool)",
				" variant o (context reprogrammed in amdgpu's order + L2 invalidate)",
				" variant V (control on VMID 12)" };
			uint32_t savedWin[4] = {};
			bool passed[kTests] = {}, ran[kTests] = {};
			/* Safe tests first, hub-writing ones last.  GC 12.0.x has only MEC pipes 0-1 with
			 * queues 0-3 (gfx_v12_0.c:1415-1423; the round-4 control on "pipe 2" aliased the
			 * kernel ring): pipe 0 queue 0 is the kernel ring, queue 1 the baseline, pipe 1
			 * queue 3 E4's scratch queue; the tests take pipe 1 queues 0-2, then pipe 0
			 * queues 2-3, and reuse them round-robin when there are more tests than slots
			 * (a reused slot that took a fault may show "not serviced", the log says so). */
			static const uint32_t order[kTests] = { kControl, kO, kV, kD, kE, kG, kT, kA, kB, kC, kE2 };
			static const uint8_t slotPipe[5] = { 1, 1, 1, 0, 0 };
			static const uint8_t slotQueue[5] = { 0, 1, 2, 2, 3 };
			const uint32_t slotFirst = e4Wedged ? 3 : 0, slotCount = 5 - slotFirst;
			uint32_t slot = 0;
			for (uint32_t oi = 0; oi < kTests; oi++) {
				const uint32_t v = order[oi];
				if (abandon || (v == kControl ? !diag : !(diag & bitOf[v])))
					continue;
				const char *tag = nameOf[v];
				RLOG("vm: variant:%s", tag);
				trail("vm: variant apply");
				if (v == kE2) {
					/* Mirror only the windows that differ (APT_CNTL, CACHEABLE_DRAM_* and
					 * NB_TOP_OF_DRAM_* are never copied: the MM value is not the right
					 * one for them).  A locked register shows in the readback. */
					uint32_t written = 0;
					RLOG("vm: E2: before any write, GC LOCAL_FB 0x%08x..0x%08x LOCAL_SYSMEM 0x%08x..0x%08x; "
					     "MM LOCAL_FB 0x%08x..0x%08x LOCAL_SYSMEM 0x%08x..0x%08x",
					     rdGc(Reg { 0, mir[0].gc }), rdGc(Reg { 0, mir[1].gc }),
					     rdGc(Reg { 0, mir[2].gc }), rdGc(Reg { 0, mir[3].gc }),
					     rd(IpDiscovery::HwMmhub, Reg { 0, mir[0].mm }),
					     rd(IpDiscovery::HwMmhub, Reg { 0, mir[1].mm }),
					     rd(IpDiscovery::HwMmhub, Reg { 0, mir[2].mm }),
					     rd(IpDiscovery::HwMmhub, Reg { 0, mir[3].mm }));
					for (uint32_t i = 0; i < nMir; i++) {
						savedWin[i] = rdGc(Reg { 0, mir[i].gc });
						const uint32_t m = rd(IpDiscovery::HwMmhub, Reg { 0, mir[i].mm });
						if (m == savedWin[i])
							continue;
						wr(IpDiscovery::HwGc, Reg { 0, mir[i].gc }, m);
						written++;
						RLOG("vm: E2: %s GC 0x%08x -> 0x%08x, reads back 0x%08x", mir[i].name,
						     savedWin[i], m, rdGc(Reg { 0, mir[i].gc }));
					}
					if (!written) {
						RLOG("vm: E2: the four mirrored windows are already equal on both hubs; skipped");
						continue;
					}
					(void)gcHubFlush();
				} else if (v == kA) {
					wr(IpDiscovery::HwGc, GcL2Cntl4, savedL4 | kL2Cntl4TapPhysMask);
				} else if (v == kB) {
					/* Flat context-0 entries for the table pages: pfn -> same
					 * VRAM physical page, in the (otherwise unused) ctx0 table. */
					const uint64_t first = c.rootPhys >> 12;
					for (uint32_t i = 0; i < 8; i++) {
						const uint64_t pte = GpuVm::encodePte(c.rootPhys + i * 0x1000ull,
						                                      GpuVm::kValid | GpuVm::kReadable |
						                                      GpuVm::kWritable, false);
						*poolDw(kPtOffset + i * 8) = static_cast<uint32_t>(pte);
						*poolDw(kPtOffset + i * 8 + 4) = static_cast<uint32_t>(pte >> 32);
					}
					flushHdp();
					wr(IpDiscovery::HwGc, GcCtx0PtStartLo, static_cast<uint32_t>(first));
					wr(IpDiscovery::HwGc, GcCtx0PtStartHi, static_cast<uint32_t>(first >> 32));
					wr(IpDiscovery::HwGc, GcCtx0PtEndLo, static_cast<uint32_t>(first + 7));
					wr(IpDiscovery::HwGc, GcCtx0PtEndHi, static_cast<uint32_t>((first + 7) >> 32));
					(void)gcHubFlush();
				} else if (v == kC) {
					c.rootPhys = c.rootMc;
					if (!buildTables() || !vmContextInit(c))
						RLOG("vm: variant c: table rebuild failed");
				} else if (v == kD || v == kE || v == kG) {
					/* PTE flag variants against the default (amdgpu's) encoding, rebuilt
					 * through the normal table path.  d: the round 2-4 encoding without
					 * IS_PTE; e: SNOOPED added to VRAM PTEs; g: every leaf EXECUTABLE. */
					vmPteSet = v == kE ? GpuVm::kSnooped : 0;
					vmPteClear = v == kD ? GpuVm::kIsPte : v == kG ? GpuVm::kExecutable : 0;
					if (!buildTables())
						RLOG("vm: variant: table rebuild failed");
				} else if (v == kO) {
					/* The context in the order amdgpu ends up with: disabled, base, range,
					 * caches invalidated (gfxhub_v12_0_init_cache_regs CNTL2 pulses), then
					 * enabled (setup_vmid_config writes range and CNTL before any base is
					 * flushed in by the ring, gmc_v12_0_emit_flush_gpu_tlb). */
					const uint32_t n = curVmid - 1;
					const uint32_t cntl = kVmCtxEnable | (GpuVm::kDepth << 1) |
						((GpuVm::kBlockSize - 9) << 4) | (((1u << 16) - 1) << 10);   // W42: bits 10..25 as Linux
					wr(IpDiscovery::HwGc, Reg { 0, GcCtx1Cntl.dword + n }, 0);
					wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtBaseLo.dword + 2 * n },
					   static_cast<uint32_t>(GpuVm::encodePde(c.rootPhys, GpuVm::kValid, 0)));
					wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtBaseHi.dword + 2 * n },
					   static_cast<uint32_t>(GpuVm::encodePde(c.rootPhys, GpuVm::kValid, 0) >> 32));
					wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtStartLo.dword + 2 * n }, 0);
					wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtStartHi.dword + 2 * n }, 0);
					wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtEndLo.dword + 2 * n }, 0xffffffff);
					wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtEndHi.dword + 2 * n }, 0xf);
					wr(IpDiscovery::HwGc, GcL2Cntl2,
					   rdGc(GcL2Cntl2) | kL2InvalidateL1Tlbs | kL2InvalidateL2Cache);
					wr(IpDiscovery::HwGc, Reg { 0, GcCtx1Cntl.dword + n }, cntl);
				} else if (v == kV) {
					/* The same context and tables on another KFD VMID: is VMID 8 special? */
					curVmid = 12;
					c.vmid = curVmid;
					if (!vmContextInit(c))
						RLOG("vm: variant V: VMID %u context setup failed", curVmid);
				} else if (v == kT) {
					/* The same tables written by the CPU through the BAR (amdgpu_vm_cpu
					 * style + HDP flush) into the CPU-visible pool, no SDMA involved. */
					vmTableCpu = kVmTableCpu;
					c.rootMc = poolMc(kVmTableCpu);
					if (!gpuPhysical(c.rootMc, c.rootPhys) || !buildTables() || !vmContextInit(c))
						RLOG("vm: variant T: table build failed");
				}
				gcFaultClear();
				dumpSetup(tag);
				if (v == kC || v == kT || v == kD)
					dumpTables(tag);
				/* A fresh queue slot for every test where the ASIC has one. */
				const uint32_t si = slotFirst + slot % slotCount;
				curPipe = slotPipe[si];
				curQueue = slotQueue[si];
				curDoorbell = 0x1a + 2 * (si + 1);
				slot++;
				ran[v] = true;
				passed[v] = attempt(tag, false);
				RLOG("vm: variant:%s -> %s (fault status 0x%08x)", tag, passed[v] ? "PASS" : "fail",
				     rdGc(GcL2FaultStatusLo));
				if (!hqdStop()) {
					RLOG("vm: variant: HQD dequeue timed out; the rest is abandoned");
					abandon = true;
					hqd = true;    /* still active: the final cleanup retries this slot */
				} else {
					hqd = false;
				}
				trail("vm: variant restore");
				if (v == kE2) {
					for (uint32_t i = 0; i < nMir; i++)
						if (rdGc(Reg { 0, mir[i].gc }) != savedWin[i])
							wr(IpDiscovery::HwGc, Reg { 0, mir[i].gc }, savedWin[i]);
					(void)gcHubFlush();
				} else if (v == kA) {
					wr(IpDiscovery::HwGc, GcL2Cntl4, savedL4);
				} else if (v == kB) {
					for (uint32_t i = 0; i < 16; i++)
						*poolDw(kPtOffset + i * 4) = 0;
					flushHdp();
					wr(IpDiscovery::HwGc, GcCtx0PtStartLo, savedC0[0]);
					wr(IpDiscovery::HwGc, GcCtx0PtStartHi, savedC0[1]);
					wr(IpDiscovery::HwGc, GcCtx0PtEndLo, savedC0[2]);
					wr(IpDiscovery::HwGc, GcCtx0PtEndHi, savedC0[3]);
					(void)gcHubFlush();
				} else if (v == kV) {
					wr(IpDiscovery::HwGc, Reg { 0, GcCtx1Cntl.dword + curVmid - 1 }, 0);
					(void)vmInvalidate(curVmid, "variant V restore");
					curVmid = vmid;
					c.vmid = vmid;
				} else if (v == kC || v == kD || v == kE || v == kG || v == kT) {
					vmPteSet = vmPteClear = 0;
					vmTableCpu = 0;
					c.rootPhys = savedPhys;
					c.rootMc = savedMc;
					if (!buildTables() || !vmContextInit(c))
						RLOG("vm: variant: table restore failed");
				}
				gcFaultClear();
				(void)vmInvalidate(vmid, "variant restore");
			}
			if (diag) {
				RLOG("vm: variants (ran/PASS): control %d/%d, o %d/%d, V %d/%d, a %d/%d, b %d/%d, c %d/%d, E2 %d/%d, d %d/%d, "
				     "e %d/%d, g %d/%d, T %d/%d (baseline failed; runtime stays disabled)",
				     ran[kControl], passed[kControl], ran[kO], passed[kO], ran[kV], passed[kV], ran[kA], passed[kA], ran[kB], passed[kB],
				     ran[kC], passed[kC], ran[kE2], passed[kE2], ran[kD], passed[kD], ran[kE], passed[kE],
				     ran[kG], passed[kG], ran[kT], passed[kT]);
			}
			fence = baseFence; data = baseData; clean = baseClean;
		}
		if (fpOn) {
			trail("vm: F restore");
			wr(IpDiscovery::HwGc, GcL2Cntl, fpSavedL2);
			wr(IpDiscovery::HwGc, GcL2FaultDefaultLo, fpSavedLo);
			wr(IpDiscovery::HwGc, GcL2FaultDefaultHi, fpSavedHi);
			(void)gcHubFlush();
			RLOG("vm: F: fault default page restored (L2_CNTL 0x%08x); dummy page first dwords 0x%08x 0x%08x",
			     rdGc(GcL2Cntl), *reinterpret_cast<volatile uint32_t *>(fpMem->getBytesNoCopy()),
			     *(reinterpret_cast<volatile uint32_t *>(fpMem->getBytesNoCopy()) + 1));
		}
		/* The host page goes away only once the hub is idle (nothing can still be
		 * answered from it): GCVM_L2_STATUS L2_BUSY [0] and CONTEXT_DOMAIN_BUSY [16:1]
		 * clear, plus a short settle.  If the hub does not go idle the page is kept
		 * (deliberately leaked until reboot) rather than freed under a live engine. */
		bool fpHubIdle = true;
		if (fpOn) {
			fpHubIdle = false;
			for (uint32_t ms = 0; ms < 100 && !fpHubIdle; ms++) {
				fpHubIdle = !(rdGc(GcL2Status) & 0x1ffffu);
				if (!fpHubIdle)
					IOSleep(1);
			}
			IOSleep(5);
			RLOG("vm: F: hub %s before the dummy page is released (GCVM_L2_STATUS 0x%08x)",
			     fpHubIdle ? "idle" : "NOT idle: the page is kept", rdGc(GcL2Status));
		}
		if (fpHubIdle) {
			if (fpDma) {
				(void)fpDma->clearMemoryDescriptor();
				fpDma->release();
			}
			if (fpMem)
				fpMem->release();
		}
	}
	if (context && (hqd || !fence)) {
		trail("vm: HQD dequeue");
		if (curPipe != pipe || curQueue != queue)
			RLOG("vm: final dequeue of the last test slot (pipe %u queue %u)", curPipe, curQueue);
		grbmSelect(1, curPipe, curQueue, curVmid);
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
		inactive = false;
		for (uint32_t us = 0; us < 100000; us += 10) {
			if (!(rdGc(CpHqdActive) & 1)) { inactive = true; break; }
			IODelay(10);
		}
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
		if (!inactive)
			RLOG("vm: boot HQD dequeue timed out (ACTIVE 0x%08x)", rdGc(CpHqdActive));
		grbmSelect(0, 0, 0, 0);
	}
	bool flushed = true;
	if (context) {
		trail("vm: invalidate");
		flushed = vmInvalidate(vmid, "boot self-test");
		if (!flushed)
			RLOG("vm: boot invalidation failed (VMID %u)", vmid);
		wr(IpDiscovery::HwGc, Reg { 0, GcCtx1Cntl.dword + vmid - 1 }, 0);
	}
	IOFree(c.tableShadow, kVmTableBytes);
	devHeap.free(table);
	if (context && !clean) {
		gcFaultClear();
		scrubFaultPage();   // D15: a faulting write may have left data there
	}
	const bool ok = context && hqd && fence && data && clean && inactive && flushed;
	if (ok)
		publishResult("vm", "PASS boot self-test");
	else if (flushed)
		publishResult("vm", "FAIL boot self-test");
	return ok;
}

void RDNA4Compute::logClientFault(RtClient &c, const char *tag) {
	const uint32_t status = rdGc(GcL2FaultStatusLo);
	if (!status)
		return;
	RLOG("vmid %u: %s: GC hub fault status 0x%08x (fault VMID %u) VA 0x%llx", c.vmid, tag,
	     status, (status >> 20) & 0xf, gcFaultVa());
	gcFaultClear();
	vmInvalidate(c.vmid, "fault clear");
	scrubFaultPage();
}

// A faulting access is redirected to the one L2 fault-default page, for every
// VMID (amdgpu's dummy page works the same way). A faulting write leaves its
// data there, and the next client's faulting read would get it. Every path
// that handles a client fault scrubs the page. A write and a read faulting at
// the same moment can still meet; the page tables are the isolation, this
// closes the lingering channel.
void RDNA4Compute::scrubFaultPage() {
	for (uint32_t i = 0; i < 0x1000 / 4; i++)
		*poolDw(kScratchOffset + i * 4) = 0;
	flushHdp();
}

IOReturn RDNA4Compute::rtOpen(const void *owner) {
	const IOReturn r = rtOpenInner(owner);
	if (r != kIOReturnSuccess) {
		char why[48];
		snprintf(why, sizeof(why), "open FAILED 0x%x", r);
		vmOpTrace(why, 0, 0, 0);
	}
	return r;
}

IOReturn RDNA4Compute::rtOpenInner(const void *owner) {
	Locked g(rtLock);
	if (!rtReady)
		return kIOReturnNotReady;
	if (RtClient *old = clientFor(owner))
		return old->aborted ? kIOReturnAborted : kIOReturnSuccess;
	RtClient *c = nullptr;
	uint32_t slot = 0;
	for (; slot < kMaxClients; slot++)
		if (!clients[slot].active) { c = &clients[slot]; break; }
	if (!c)
		return kIOReturnNoResources;
	if (!vmEnabled) {
		*c = RtClient {};
		c->owner = owner;
		c->active = true;
		return kIOReturnSuccess;
	}
	if (vmShared)
		return rtOpenShared(owner, slot, c);
	uint32_t vmid = 0;
	for (vmid = 8; vmid <= 15 && vmidUsed[vmid]; vmid++) {}
	if (vmid > 15)
		return kIOReturnNoResources;
	/* GC 12.0.x has one MEC with 2 pipes of 4 queues (gfx_v12_0.c:1415-1423): pipe
	 * and queue numbers above that alias real slots (round 4: a queue on "pipe 2"
	 * read back the kernel ring's HQD), so at most 7 client queues exist. */
	uint32_t pipe = 0, queue = 0;
	for (; pipe < 2; pipe++) {
		for (queue = 0; queue < 4; queue++)
			if (!(pipe == 0 && queue == 0) && !queueUsed[pipe][queue])
				break;
		if (queue < 4)
			break;
	}
	if (pipe == 2)
		return kIOReturnNoResources;
	uint64_t table = 0;
	if (!devHeap.size() || !devHeap.alloc(kVmTableBytes, table))
		return kIOReturnNoMemory;
	if (kVmQueueBase + slot * kVmQueueStride + kVmKernarg + 0x1000 > pool.size) {
		devHeap.free(table);
		return kIOReturnNoMemory;
	}
	*c = RtClient {};
	c->owner = owner; c->vmid = vmid; c->pipe = pipe; c->queue = queue;
	c->tableOffset = table; c->rootMc = vramMc(table);
	if (!gpuPhysical(c->rootMc, c->rootPhys)) {
		devHeap.free(table); *c = RtClient {};
		return kIOReturnNoMemory;
	}
	c->tableShadow = reinterpret_cast<uint64_t *>(IOMalloc(kVmTableBytes));
	if (!c->tableShadow) {
		devHeap.free(table); *c = RtClient {};
		return kIOReturnNoMemory;
	}
	bzero(c->tableShadow, kVmTableBytes);
	c->tableShadow[0] =
		GpuVm::encodePde(c->rootPhys + 0x1000, GpuVm::kValid, 2);
	c->tableShadow[0x1000 / sizeof(uint64_t) + GpuVm::index(GpuVm::kVaStart, 1)] =
		GpuVm::encodePde(c->rootPhys + 0x2000, GpuVm::kValid, 1);
	const uint32_t qoff = kVmQueueBase + slot * kVmQueueStride;
	for (uint32_t off = 0; off < 0x7000; off += 4)
		*poolDw(qoff + off) = 0;
	c->kernargCpu = poolDw(qoff + kVmKernarg);
	c->queueCpu = poolDw(qoff + kVmPq);
	c->fenceCpu = poolDw(qoff + kVmFence);
	const uint64_t qva = c->nextVa; c->nextVa += 0x1000;
	const uint64_t eva = c->nextVa; c->nextVa += 0x1000;
	const uint64_t rva = c->nextVa; c->nextVa += 0x1000;
	const uint64_t wva = c->nextVa; c->nextVa += 0x1000;
	c->fenceVa = c->nextVa; c->nextVa += 0x1000;
	c->kernargVa = c->nextVa; c->nextVa += 0x1000;
	c->queueVa = qva;
	c->mqdMc = poolMc(qoff + kVmMqd);
	c->eopVa = eva;
	c->rptrVa = rva;
	c->wpollVa = wva;
	if (!vmMap(*c, qva, poolMc(qoff + kVmPq), kPqSize, true) ||
	    !vmMap(*c, eva, poolMc(qoff + kVmEop), 0x1000, false) ||
	    !vmMap(*c, rva, poolMc(qoff + kVmRptr), 0x1000, false) ||
	    !vmMap(*c, wva, poolMc(qoff + kVmWptr), 0x1000, false) ||
	    !vmMap(*c, c->fenceVa, poolMc(qoff + kVmFence), 0x1000, false) ||
	    !vmMap(*c, c->kernargVa, poolMc(qoff + kVmKernarg), 0x1000, false) ||
	    !c->pm4.init(poolDw(qoff + kVmPq), qva, kPqSize) || !vmContextInit(*c)) {
		IOFree(c->tableShadow, kVmTableBytes); devHeap.free(table); *c = RtClient {};
		return kIOReturnNoMemory;
	}
	c->doorbell = (0x0d + slot) * 2;
	/* A VMID taken by a new owner is flushed before first use (amdgpu_vmid_grab
	 * sets needs_flush, amdgpu_vm_flush emits it): no stale translations from
	 * an earlier client or an earlier boot. A timeout is logged, not fatal. */
	(void)vmInvalidate(vmid, "client context enable");
	vmidUsed[vmid] = true; queueUsed[pipe][queue] = true;
	if (!hqdInitFor(false, pipe, queue, vmid, poolMc(qoff + kVmMqd), eva >> 8, qva >> 8,
	               rva, wva, c->doorbell)) {
		vmidUsed[vmid] = false; queueUsed[pipe][queue] = false;
		IOFree(c->tableShadow, kVmTableBytes); devHeap.free(table); *c = RtClient {};
		return kIOReturnNotResponding;
	}
	c->active = true;
	RLOG("vmid %u: client queue activated MEC1 pipe %u queue %u, PDB2 MC 0x%llx physical 0x%llx, doorbell dword %u",
	     vmid, pipe, queue, c->rootMc, c->rootPhys, c->doorbell);
	vmOpTrace("open", vmid, pipe, queue);
	if (!vmSurveyClientDone) {
		vmSurveyClientDone = true;
		vmSurvey("after the first client opened");
	}
	return kIOReturnSuccess;
}

/* rdna4-vmshared=1 (vmshared.cpp): the client has a VMID and page tables but no queue. Same VA layout for the pages it keeps (kernarg, fence), plus
 * the IB page the kernel writes each dispatch into. */
IOReturn RDNA4Compute::rtOpenShared(const void *owner, uint32_t slot, RtClient *c) {
	if (!vmSharedEnsure())
		return kIOReturnNotReady;
	uint32_t vmid = 0;
	for (vmid = 8; vmid <= 15 && vmidUsed[vmid]; vmid++) {}
	if (vmid > 15)
		return kIOReturnNoResources;
	const uint32_t sq = slot & 1;
	if (sharedQ[sq].wedged || !sharedQ[sq].up)
		return kIOReturnNotResponding;
	uint64_t table = 0;
	if (!devHeap.size() || !devHeap.alloc(kVmTableBytes, table))
		return kIOReturnNoMemory;
	if (kVmQueueBase + slot * kVmQueueStride + kVmIb + 0x1000 > pool.size) {
		devHeap.free(table);
		return kIOReturnNoMemory;
	}
	*c = RtClient {};
	c->owner = owner; c->vmid = vmid; c->shared = true; c->sq = static_cast<uint8_t>(sq);
	c->pipe = sharedQ[sq].pipe; c->queue = sharedQ[sq].queue;   // for reporting; the client owns no HQD
	c->tableOffset = table; c->rootMc = vramMc(table);
	if (!gpuPhysical(c->rootMc, c->rootPhys)) {
		devHeap.free(table); *c = RtClient {};
		return kIOReturnNoMemory;
	}
	c->tableShadow = reinterpret_cast<uint64_t *>(IOMalloc(kVmTableBytes));
	if (!c->tableShadow) {
		devHeap.free(table); *c = RtClient {};
		return kIOReturnNoMemory;
	}
	bzero(c->tableShadow, kVmTableBytes);
	c->tableShadow[0] = GpuVm::encodePde(c->rootPhys + 0x1000, GpuVm::kValid, 2);
	c->tableShadow[0x1000 / sizeof(uint64_t) + GpuVm::index(GpuVm::kVaStart, 1)] =
		GpuVm::encodePde(c->rootPhys + 0x2000, GpuVm::kValid, 1);
	const uint32_t qoff = kVmQueueBase + slot * kVmQueueStride;
	c->poolOff = qoff;
	for (uint32_t off = 0; off < 0x8000; off += 4)
		*poolDw(qoff + off) = 0;
	flushHdp();
	c->kernargCpu = poolDw(qoff + kVmKernarg);
	c->fenceCpu = poolDw(qoff + kVmFence);
	/* The same VA slots as the HQD path (six pages), of which the queue's four stay unmapped, then the IB page. */
	c->nextVa += 4 * 0x1000;
	c->fenceVa = c->nextVa; c->nextVa += 0x1000;
	c->kernargVa = c->nextVa; c->nextVa += 0x1000;
	c->ibVa = c->nextVa; c->nextVa += 0x1000;
	if (!vmMap(*c, c->fenceVa, poolMc(qoff + kVmFence), 0x1000, false) ||
	    !vmMap(*c, c->kernargVa, poolMc(qoff + kVmKernarg), 0x1000, false) ||
	    !vmMap(*c, c->ibVa, poolMc(qoff + kVmIb), 0x1000, true) || !vmContextInit(*c)) {
		IOFree(c->tableShadow, kVmTableBytes); devHeap.free(table); *c = RtClient {};
		return kIOReturnNoMemory;
	}
	c->doorbell = sharedQ[sq].doorbell;
	(void)vmInvalidate(vmid, "client context enable");
	vmidUsed[vmid] = true;
	c->active = true;
	RLOG("vmid %u: shared-queue client (rdna4-vmshared) on shared queue %u (MEC1 pipe %u queue %u, VMID 0), PDB2 MC 0x%llx, IB VA 0x%llx",
	     vmid, sq, sharedQ[sq].pipe, sharedQ[sq].queue, c->rootMc, c->ibVa);
	vmOpTrace("open", vmid, c->pipe, c->queue);
	if (!vmSurveyClientDone) {
		vmSurveyClientDone = true;
		vmSurvey("after the first client opened");
	}
	return kIOReturnSuccess;
}

bool RDNA4Compute::initPresentationTimer() {
	if (presentTimer)
		return true;
	if (!env.owner || !(presentWorkLoop = env.owner->getWorkLoop()))
		return false;
	auto *context = OSTypeAlloc(RDNA4PresentContext);
	if (!context || !context->init()) {
		OSSafeReleaseNULL(context);
		presentWorkLoop = nullptr;
		return false;
	}
	context->compute = this;
	presentContext = context;
	presentTimer = IOTimerEventSource::timerEventSource(context, presentTimerAction);
	if (!presentTimer || presentWorkLoop->addEventSource(presentTimer) != kIOReturnSuccess) {
		OSSafeReleaseNULL(presentTimer);
		context->compute = nullptr;
		context->release();
		presentContext = nullptr;
		presentWorkLoop = nullptr;
		return false;
	}
	return true;
}

void RDNA4Compute::stopPresentationTimer() {
	if (presentTimer) {
		presentTimer->cancelTimeout();
		if (presentWorkLoop)
			presentWorkLoop->removeEventSource(presentTimer);
		presentTimer->release();
		presentTimer = nullptr;
	}
	if (presentContext) {
		static_cast<RDNA4PresentContext *>(presentContext)->compute = nullptr;
		presentContext->release();
		presentContext = nullptr;
	}
	presentWorkLoop = nullptr;
}

void RDNA4Compute::schedulePresentationTimer() {
	if (presentTimer) {
		presentTimer->setTimeoutMS(1);
	}
}

void RDNA4Compute::schedulePresentationRetry() {
	// This is deliberately a racy hint: a present submission arms the timer,
	// while a busy client must not turn the framebuffer work loop into a 1 kHz
	// lock-poll.  If work is still pending, retry after a bounded backoff.
	if (presentTimer && presentPending) {
		presentTimer->setTimeoutMS(8);
		if (detailedFlipTrace())
			RLOG("present timer: retry armed in 8 ms (%u pending)", presentPending);
	}
}

void RDNA4Compute::presentTimerAction(OSObject *owner, IOTimerEventSource *) {
	auto *context = static_cast<RDNA4PresentContext *>(owner);
	if (context && context->compute)
		context->compute->presentTimerTick();
}

RDNA4Compute::PresentSlot *RDNA4Compute::presentSlot(uint64_t id, const void *owner) {
	for (PresentSlot &slot : presentSlots)
		if (slot.state && slot.id == id && slot.owner == owner)
			return &slot;
	return nullptr;
}

void RDNA4Compute::completePresentLocked(PresentSlot &slot, IOReturn result, uint64_t frame) {
	if (slot.state == 1 && presentPending)
		presentPending--;
	slot.state = 2;
	slot.result = result;
	slot.frame = frame;
}

void RDNA4Compute::dropPendingPresentsLocked(IOReturn result) {
	for (PresentSlot &slot : presentSlots)
		if (slot.state == 1)
			completePresentLocked(slot, result, 0);
	if (!presentActive) {
		presentOwner = nullptr;
		presentHandle = 0;
		presentOffset = 0;
		presentSurface = {};
	}
}

void RDNA4Compute::presentTimerTick() {
	uint64_t id = 0;
	const void *owner = nullptr;
	Flip::Surface surface {};
	if (!rtLock || !IOLockTryLock(rtLock)) {
		// The framebuffer work-loop timer must never wait behind a client
		// dispatch, SDMA transfer, or restore.  Retry only while a present is
		// pending, with a backoff rather than a 1 ms lock poll.
		if (detailedFlipTrace())
			RLOG("present timer: rtLock busy; retry");
		schedulePresentationRetry();
		return;
	}
	checkPresentationTimeoutLocked();
	for (const PresentSlot &slot : presentSlots) {
		if (slot.state == 1 && (!id || slot.id < id)) {
			id = slot.id;
			owner = slot.owner;
		}
	}
	if (id)
		surface = presentSurface;
	IOLockUnlock(rtLock);
	if (!id) {
		if (detailedFlipTrace())
			RLOG("present timer: wake with no pending slot");
		return;
	}
	if (!presentNoVblankTicks && detailedFlipTrace())
		RLOG("present timer: service id %llu on OTG%u HUBP%u", id, surface.otg, surface.hubp);

	uint64_t frame = 0;
	// A one-millisecond bounded poll avoids the old 100 ms work-loop stall,
	// while still giving the emulator/card a chance to advance the frame
	// counter between timer callbacks.  Re-arming remains bounded below.
	if (!Flip::waitNextVblank(*this, surface.otg, 1, frame)) {
		if (detailedFlipTrace() &&
		    (!presentNoVblankTicks || (presentNoVblankTicks % 25) == 24))
			RLOG("present timer: id %llu no vblank in 1 ms (retry %u)", id,
			     presentNoVblankTicks + 1);
		if (!IOLockTryLock(rtLock)) {
			if (detailedFlipTrace())
				RLOG("present timer: rtLock busy after vblank miss; retry");
			schedulePresentationRetry();
			return;
		}
		PresentSlot *slot = presentSlot(id, owner);
		if (slot && slot->state == 1 && ++presentNoVblankTicks >= 100) {
			completePresentLocked(*slot, kIOReturnTimeout, 0);
			for (PresentSlot &pending : presentSlots)
				if (pending.state == 1)
					completePresentLocked(pending, kIOReturnTimeout, 0);
			if (presentActive)
				(void)restorePresentationLocked("present vblank timeout");
			else
				clearPresentationLocked();
			presentNoVblankTicks = 0;
		}
		const bool again = presentPending != 0;
		IOLockUnlock(rtLock);
		if (again)
			schedulePresentationTimer();
		return;
	}
	if (detailedFlipTrace())
		RLOG("present timer: id %llu saw vblank frame %llu; attempting latch", id, frame);

	if (!IOLockTryLock(rtLock)) {
		if (detailedFlipTrace())
			RLOG("present timer: rtLock busy after vblank; retry");
		schedulePresentationRetry();
		return;
	}
	presentNoVblankTicks = 0;
	checkPresentationTimeoutLocked();
	PresentSlot *slot = presentSlot(id, owner);
	if (slot && slot->state == 1) {
		RtBuffer *buffer = bufferFor(owner, slot->handle);
		const uint64_t bytes = Flip::surfaceBytes(surface.pitch, surface.height);
		if (!buffer || !buffer->device || (slot->offset & 255u) ||
		    slot->offset > buffer->bytes || bytes > buffer->bytes - slot->offset ||
		    slot->offset > ~0ull - buffer->mc) {
			completePresentLocked(*slot, kIOReturnAborted, 0);
		} else {
			// Arm rollback before touching the address: flipTo is allowed to
			// report failure after the user address has latched.
			presentActive = true;
			presentOwner = owner;
			presentHandle = slot->handle;
			presentOffset = slot->offset;
			presentStarted = mach_absolute_time();
			presentSurface = surface;
			if (detailedFlipTrace())
				RLOG("present timer: id %llu flip target 0x%llx", id, buffer->mc + slot->offset);
			if (!Flip::flipTo(*this, surface, buffer->mc + slot->offset, "present async",
			                  nullptr, true)) {
				completePresentLocked(*slot, kIOReturnNotResponding, 0);
				RLOG("present timer: id %llu latch failed; restoring desktop", id);
				(void)restorePresentationLocked("present async failure");
			} else {
				completePresentLocked(*slot, kIOReturnSuccess, frame);
				if (detailedFlipTrace())
					RLOG("present timer: id %llu complete at frame %llu", id, frame);
			}
		}
	}
	if (!presentActive && !presentPending)
		clearPresentationLocked();
	const bool again = presentPending != 0;
	IOLockUnlock(rtLock);
	if (again)
		schedulePresentationTimer();
}

void RDNA4Compute::clearPresentationLocked() {
	presentActive = false;
	presentOwner = nullptr;
	presentHandle = 0;
	presentOffset = 0;
	presentStarted = 0;
	presentSurface = {};
	presentNoVblankTicks = 0;
}

IOReturn RDNA4Compute::restorePresentationLocked(const char *why) {
	dropPendingPresentsLocked(kIOReturnAborted);
	if (!presentActive) {
		clearPresentationLocked();
		return kIOReturnSuccess;
	}
	const bool restored = Flip::flipTo(*this, presentSurface, presentSurface.desktop,
	                                   why ? why : "restore");
	if (restored) {
		RLOG("present: restored desktop (%s)", why ? why : "requested");
		clearPresentationLocked();
		return kIOReturnSuccess;
	}
	RLOG("present: desktop restore failed (%s)", why ? why : "requested");
	// flipTo can fail after the user address has latched. Keep the state
	// armed so a later client call retries the 30 s auto-restore.
	presentStarted = mach_absolute_time();
	return kIOReturnNotResponding;
}

void RDNA4Compute::checkPresentationTimeoutLocked() {
	if (!presentActive)
		return;
	uint64_t span = 0;
	nanoseconds_to_absolutetime(30000000000ull, &span);
	if (mach_absolute_time() - presentStarted >= span)
		(void)restorePresentationLocked("timeout");
}

IOReturn RDNA4Compute::rtInfo(const void *owner, uint64_t out[9]) {
	Locked g(rtLock);
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	checkPresentationTimeoutLocked();
	out[0] = RDNA4_COMPUTE_ABI;
	out[1] = rtStage;
	out[2] = (rtReady ? RDNA4_FLAG_READY : 0) | (rtWedged ? RDNA4_FLAG_WEDGED : 0) |
	         (dmaReady ? RDNA4_FLAG_DMA : 0);
	const bool dev = dmaReady && devHeap.size();          // where buffers come from
	const GpuHeap::Heap &h = dev ? devHeap : heap;
	out[3] = h.size();
	out[4] = h.freeBytes();
	out[5] = dev ? vramMc(devHeap.base()) : poolMc(kHeapOffset);
	RtClient *c = vmClientFor(owner);
	out[6] = c ? c->vmid : 0;
	out[7] = c ? c->pipe : 0;
	out[8] = c ? c->queue : 0;
	if (vmEnabled && c)
		out[2] |= RDNA4_FLAG_VM;
	if (resumed)
		out[2] |= RDNA4_FLAG_RESUMED;
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtSensors(const void *owner, RDNA4Sensors &out) {
	Locked g(rtLock);
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	return readSensors(out) ? kIOReturnSuccess : kIOReturnNotResponding;
}

IOReturn RDNA4Compute::rtSensorsEx(const void *owner, RDNA4SensorsEx &out) {
	Locked g(rtLock);
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	return readSensorsEx(out) ? kIOReturnSuccess : kIOReturnNotResponding;
}

IOReturn RDNA4Compute::rtSleepTest(const void *owner, uint32_t phase) {
	uint32_t enabled = 0;
	if (!requestedPowerManagement() ||
	    !PE_parse_boot_argn("rdna4-sleeptest", &enabled, sizeof(enabled)) || !enabled)
		return kIOReturnUnsupported;
	if (phase != 1 && phase != 2)
		return kIOReturnBadArgument;
	/* IOUserClient::initWithTask already requires kIOClientPrivilegeAdministrator;
	 * this selector is therefore root-only along with the rest of this service. */
	{
		Locked g(rtLock);
		if (!clientFor(owner))
			return kIOReturnNotFound;
	}
	if (phase == 1) {
		RLOG("power: debug sleep selector phase 1");
		powerWillSleep();
	} else {
		RLOG("power: debug sleep selector phase 2");
		powerDidWake();
	}
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtQuiesce(const void *owner) {
	/* The user client is administrator-only.  This selector deliberately calls
	 * the same path as the root-domain shutdown/restart notification so that a
	 * monitor reset can compare a stopped card with an inherited live card. */
	uint32_t enabled = 0;
	if (!PE_parse_boot_argn("rdna4-quiesce-test", &enabled, sizeof(enabled)) || !enabled)
		return kIOReturnUnsupported;
	{
		Locked g(rtLock);
		if (!clientFor(owner))
			return kIOReturnNotFound;
	}
	quiesceForShutdown("debug quiesce");
	return kIOReturnSuccess;
}

// Contents are undefined, as with any GPU allocation; callers write first.
IOReturn RDNA4Compute::rtAlloc(const void *owner, uint64_t bytes, uint64_t &handle, uint64_t &gpu) {
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	RtClient *c = vmClientFor(owner);
	if (vmEnabled && !c)
		return kIOReturnNoResources;
	const bool dev = dmaReady && devHeap.size();
	GpuHeap::Heap &h = dev ? devHeap : heap;
	if (!bytes || bytes > h.size())
		return kIOReturnBadArgument;
	uint32_t slot = 0;
	while (slot < kMaxBuffers && buffers[slot].owner)
		slot++;
	if (slot == kMaxBuffers)
		return kIOReturnNoResources;
	uint64_t off;
	if (!h.alloc(bytes, off))
		return kIOReturnNoMemory;
	RtBuffer &b = buffers[slot];
	const uint16_t generation = nextGen(b.gen);
	b = RtBuffer {};
	b.owner = owner;
	b.offset = off;
	b.bytes = bytes;
	b.mc = dev ? vramMc(off) : poolMc(off);
	b.gen = generation;
	b.device = dev;
	if (c) {
		b.va = (c->nextVa + 0xffff) & ~0xffffull;
		c->nextVa = b.va + ((bytes + 0xffff) & ~0xffffull);
		if (c->nextVa < b.va || !vmMap(*c, b.va, b.mc, bytes, true)) {   /* IBs live in user buffers: R|W|X like Mesa (ac_linux_drm.c:235) */
			h.free(b.offset); b.owner = nullptr;
			return kIOReturnNoMemory;
		}
	}
	handle = makeHandle(slot, b.gen);
	gpu = c ? b.va : b.mc;
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtAllocHost(const void *owner, task_t task, uint64_t bytes,
                                   uint64_t flags, uint64_t &handle, uint64_t &gpu,
                                   uint64_t &user) {
	Locked g(rtLock);
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	if (!vmEnabled)
		return kIOReturnUnsupported;
	RtClient *c = vmClientFor(owner);
	if (!c || !dmaReady || !busMasterSet)
		return kIOReturnUnsupported;
	if (!bytes || bytes > kHostMaxBuffer || (flags & ~static_cast<uint64_t>(RDNA4_HOST_EXECUTABLE)))
		return bytes > kHostMaxBuffer ? kIOReturnNoResources : kIOReturnBadArgument;
	const uint64_t rounded = (bytes + GpuVm::kPageBytes - 1) &
	                         ~(GpuVm::kPageBytes - 1);
	if (rounded < bytes || rounded > kHostMaxBuffer ||
	    rounded > kHostMaxClient - c->hostBytes ||
	    rounded > kHostMaxTotal - hostBytesTotal)
		return kIOReturnNoResources;
	uint32_t slot = 0;
	while (slot < kMaxBuffers && buffers[slot].owner)
		slot++;
	if (slot == kMaxBuffers)
		return kIOReturnNoResources;

	IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithOptions(
		kernel_task, kIODirectionInOut, rounded, GpuVm::kPageBytes);
	if (!memory)
		return kIOReturnNoMemory;
	IOMemoryMap *map = memory->createMappingInTask(task, 0, kIOMapAnywhere);
	if (!map) {
		memory->release();
		return kIOReturnNoMemory;
	}
	const uint64_t pageCount = rounded / GpuVm::kPageBytes;
	IODMACommand *dma = IODMACommand::withSpecification(
		kIODMACommandOutputHost64, 48, 0, IODMACommand::kMapped, 0, GpuVm::kPageBytes);
	if (!dma) {
		map->release();
		memory->release();
		return kIOReturnNoMemory;
	}
	const uint64_t segBytes = pageCount * sizeof(IODMACommand::Segment64);
	IODMACommand::Segment64 *segments =
		static_cast<IODMACommand::Segment64 *>(IOMalloc(segBytes));
	uint64_t *pageBuses = static_cast<uint64_t *>(IOMalloc(pageCount * sizeof(uint64_t)));
	UInt64 dmaOffset = 0;
	UInt32 segmentCount = static_cast<UInt32>(pageCount);
	bool mapped = segments && pageBuses &&
	             dma->setMemoryDescriptor(memory) == kIOReturnSuccess &&
	             dma->gen64IOVMSegments(&dmaOffset, segments, &segmentCount) == kIOReturnSuccess;
	uint64_t page = 0;
	if (mapped) {
		for (UInt32 i = 0; i < segmentCount && mapped; i++) {
			const uint64_t address = segments[i].fIOVMAddr;
			const uint64_t length = segments[i].fLength;
			if ((address & (GpuVm::kPageBytes - 1)) ||
			    (length & (GpuVm::kPageBytes - 1)) || !length) {
				mapped = false;
				break;
			}
			for (uint64_t at = 0; at < length; at += GpuVm::kPageBytes)
				pageBuses[page++] = address + at;
		}
		mapped = mapped && page == pageCount;
	}
	if (segments)
		IOFree(segments, segBytes);
	if (!mapped) {
		(void)dma->clearMemoryDescriptor();
		dma->release();
		if (pageBuses)
			IOFree(pageBuses, pageCount * sizeof(uint64_t));
		map->release();
		memory->release();
		return kIOReturnNoResources;
	}

	RtBuffer &b = buffers[slot];
	const uint16_t generation = nextGen(b.gen);
	b = RtBuffer {};
	b.owner = owner;
	b.bytes = bytes;
	b.va = (c->nextVa + 0xffff) & ~0xffffull;
	b.gen = generation;
	b.host = true;
	b.hostMemory = memory;
	b.hostDma = dma;
	b.hostMap = map;
	b.hostUser = map->getAddress();
	if (!b.hostUser ||
	    b.va + rounded < b.va || b.va + rounded > GpuVm::kVaEnd ||
	    !vmMapHost(*c, b.va, pageBuses, rounded,
                (flags & RDNA4_HOST_EXECUTABLE) != 0)) {
		if (pageBuses)
			IOFree(pageBuses, pageCount * sizeof(uint64_t));
		vmUnmap(*c, b.va, rounded);
		releaseHost(b);
		b.owner = nullptr;
		return kIOReturnNoMemory;
	}
	if (pageBuses)
		IOFree(pageBuses, pageCount * sizeof(uint64_t));
	if (!vmInvalidate(c->vmid, "host map")) {
		vmUnmap(*c, b.va, rounded);
		releaseHost(b);
		b.owner = nullptr;
		return kIOReturnNotResponding;
	}
	c->nextVa = b.va + ((rounded + 0xffff) & ~0xffffull);
	c->hostBytes += rounded;
	hostBytesTotal += rounded;
	handle = makeHandle(slot, b.gen);
	gpu = b.va;
	user = b.hostUser;
	RLOG("vmid %u: host buffer %llu bytes mapped at GPU VA 0x%llx, user 0x%llx, %llu pages",
	     c->vmid, bytes, b.va, b.hostUser, pageCount);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtFree(const void *owner, uint64_t handle) {
	Locked g(rtLock);
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	checkPresentationTimeoutLocked();
	RtBuffer *b = bufferFor(owner, handle);
	if (!b)
		return kIOReturnBadArgument;
	IOReturn restore = kIOReturnSuccess;
	if (presentActive && presentOwner == owner && presentHandle == handle)
		restore = restorePresentationLocked("buffer free");
	RtClient *c = vmClientFor(owner);
	const bool host = b->host;
	if (c && b->va)
		vmUnmap(*c, b->va, b->bytes);
	/* Every unmap is followed by an invalidation before the memory goes back to its heap
	 * (amdgpu sets tlb_seq on a cleared PTE and flushes before the VMID's next job,
	 * amdgpu_vm.c:1272). Device buffers used to skip it: a cached translation could then
	 * still reach VRAM that the heap hands to another client. Inferred from the code, not
	 * measured on the card. Failure handling is the host path's: log and go on. */
	if (c && (b->va || host) && !vmInvalidate(c->vmid, host ? "host unmap" : "device unmap"))
		RLOG("vmid %u: %s buffer unmap invalidation timed out", c->vmid, host ? "host" : "device");
	if (host) {
		if (c) {
			const uint64_t rounded = (b->bytes + GpuVm::kPageBytes - 1) &
			                         ~(GpuVm::kPageBytes - 1);
			if (c->hostBytes >= rounded)
				c->hostBytes -= rounded;
			if (hostBytesTotal >= rounded)
				hostBytesTotal -= rounded;
		}
		RLOG("vmid %u: host buffer handle 0x%llx unmapped", c ? c->vmid : 0, handle);
		releaseHost(*b);
	} else {
		(b->device ? devHeap : heap).free(b->offset);
	}
	b->owner = nullptr;
	return restore;
}

IOReturn RDNA4Compute::rtCopy(const void *owner, uint64_t handle, uint64_t offset, task_t task,
                              mach_vm_address_t user, uint64_t length, bool toGpu) {
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	RtBuffer *b = bufferFor(owner, handle);
	if (!b || offset > b->bytes || length > b->bytes - offset)
		return kIOReturnBadArgument;
	if (b->host)
		return kIOReturnUnsupported;
	if (!length)
		return kIOReturnSuccess;
	if (dmaReady)
		return dmaCopy(task, user, b->mc + offset, length, toGpu);
	if (b->device)
		return kIOReturnNotReady;
	IOReturn r = userCopy(task, user, poolCpu + b->offset + offset, length, toGpu);
	if (toGpu)
		flushHdp();
	return r;
}

IOReturn RDNA4Compute::rtLoad(const void *owner, task_t task, mach_vm_address_t elf, uint64_t length,
                              const char *name, uint64_t out[8]) {
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	RtClient *c = vmClientFor(owner);
	if (vmEnabled && !c)
		return kIOReturnNoResources;
	if (!length || length > RDNA4_MAX_CODE_OBJECT)
		return kIOReturnBadArgument;
	uint32_t slot = 0;
	while (slot < kMaxPrograms && programs[slot].owner)
		slot++;
	if (slot == kMaxPrograms)
		return kIOReturnNoResources;

	auto *file = static_cast<uint8_t *>(IOMalloc(length));
	if (!file)
		return kIOReturnNoMemory;
	IOReturn r = userCopy(task, elf, file, length, true);
	CodeObj::Kernel k {};
	CodeObj::Image img {};
	const char *why = nullptr;
	uint64_t off = 0;
	if (r != kIOReturnSuccess) {
		why = "could not read the file from the caller";
	} else if (!CodeObj::findKernel(file, length, name, k, &why) ||
	           !CodeObj::parseImage(file, length, img, &why)) {
		r = kIOReturnNotFound;
	} else if (!kernelFits(k, &why)) {
		r = kIOReturnUnsupported;
	} else if ((k.entryVa & 0xff) || k.entryVa >= img.size) {
		why = "its code is not 256-byte aligned inside the image";
		r = kIOReturnUnsupported;
	} else if (!heap.alloc(img.size + kCodePad, off)) {
		why = "no room in the heap";
		r = kIOReturnNoMemory;
	} else {
		uint8_t *dst = poolCpu + off;
		bzero(dst, static_cast<size_t>(img.size + kCodePad));
		for (uint32_t i = 0; i < img.count; i++)
			memcpy(dst + img.seg[i].vaddr, file + img.seg[i].fileOffset,
			       static_cast<size_t>(img.seg[i].fileSize));
		flushHdp();
	}
	IOFree(file, length);
	if (r != kIOReturnSuccess) {
		RLOG("load of \"%s\" refused: %s", name, why ? why : "?");
		return r;
	}

	RtProgram &p = programs[slot];
	p = { owner, off, 0, k, nextGen(p.gen) };
	if (c) {
		p.va = (c->nextVa + 0xfff) & ~0xfffull;
		c->nextVa = p.va + ((img.size + kCodePad + 0xffff) & ~0xffffull);
		if (c->nextVa < p.va || !vmMap(*c, p.va, poolMc(off), img.size + kCodePad, true)) {
			heap.free(off); p.owner = nullptr;
			return kIOReturnNoMemory;
		}
	}
	out[0] = makeHandle(slot, p.gen);
	out[1] = k.kernargSize;
	out[2] = img.size;
	out[3] = k.rsrc1;
	out[4] = k.rsrc2;
	out[5] = k.rsrc3;
	out[6] = k.properties;
	out[7] = k.groupSegmentSize;
	out[8] = (c ? p.va : poolMc(off)) + k.entryVa;
	RLOG("loaded \"%s\": %llu-byte image at %s 0x%llx, entry +0x%llx, %u bytes of kernargs, "
	     "%u of LDS", name, img.size, c ? "VA" : "MC", c ? p.va : poolMc(off), k.entryVa,
	     k.kernargSize, k.groupSegmentSize);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtUnload(const void *owner, uint64_t program) {
	Locked g(rtLock);
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	checkPresentationTimeoutLocked();
	RtProgram *p = programFor(owner, program);
	if (!p)
		return kIOReturnBadArgument;
	if (RtClient *c = vmClientFor(owner)) {
		vmUnmap(*c, p->va, heap.lengthOf(p->offset));
		/* As rtFree: invalidate before the code pages return to the heap. */
		if (!vmInvalidate(c->vmid, "program unmap"))
			RLOG("vmid %u: program unmap invalidation timed out", c->vmid);
	}
	heap.free(p->offset);
	p->owner = nullptr;
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtDispatch(const void *owner, const RDNA4Dispatch &d, uint64_t &micros) {
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	RtClient *c = vmClientFor(owner);
	if (rtWedged && !(c && c->shared)) {
		vmOpTrace("dispatch REFUSED: rtWedged", c ? c->vmid : 0, c ? c->pipe : 0, c ? c->queue : 0);
		return kIOReturnNotResponding;
	}
	if (vmEnabled && !c)
		return kIOReturnNoResources;
	if (c)
		retireIbFences(*c);
	RtProgram *p = programFor(owner, d.program);
	if (!p || d.kernargBytes > RDNA4_MAX_KERNARG || d.timeoutMs > RDNA4_MAX_TIMEOUT_MS ||
	    d.dynamicLdsBytes > RDNA4_MAX_LDS ||
	    p->k.groupSegmentSize + d.dynamicLdsBytes > RDNA4_MAX_LDS)
		return kIOReturnBadArgument;
	uint64_t items = 1;
	for (int i = 0; i < 3; i++) {
		if (!d.groups[i] || !d.groupSize[i] ||
		    static_cast<uint64_t>(d.groups[i]) * d.groupSize[i] > 0xffffffffull)
			return kIOReturnBadArgument;
		items *= d.groupSize[i];
	}
	if (items > 1024)
		return kIOReturnBadArgument;

	// The kernarg block: the caller's bytes, zero up to the kernel's size
	// (hidden arguments the caller did not fill).
	const CodeObj::Kernel &k = p->k;
	uint32_t bytes = k.kernargSize > d.kernargBytes ? k.kernargSize : d.kernargBytes;
	bytes = (bytes + 3) & ~3u;
	volatile uint32_t *kernargCpu = c ? c->kernargCpu : poolDw(kKernargOffset);
	for (uint32_t off = 0; off < bytes; off += 4)
		kernargCpu[off / 4] = 0;
	if (d.kernargBytes) {
		for (uint32_t off = 0; off < d.kernargBytes; off++)
			reinterpret_cast<volatile uint8_t *>(kernargCpu)[off] = d.kernargs[off];
	}

	const uint64_t kernarg = c ? c->kernargVa : poolMc(kKernargOffset);
	const uint32_t user[2] = { static_cast<uint32_t>(kernarg), static_cast<uint32_t>(kernarg >> 32) };
	Launch l {};
	l.code = (c ? p->va : poolMc(p->offset)) + k.entryVa;
	l.rsrc1 = k.rsrc1;
	l.rsrc2 = k.rsrc2;
	l.rsrc3 = k.rsrc3;
	l.user = user;
	l.userCount = k.userSgprCount();
	for (int i = 0; i < 3; i++) {
		l.groups[i] = d.groups[i];
		l.groupSize[i] = d.groupSize[i];
	}
	l.wave32 = k.wave32();
	l.timeoutUs = (d.timeoutMs ? d.timeoutMs : 1000) * 1000;
	l.ldsBytes = k.groupSegmentSize + d.dynamicLdsBytes;
	l.useInterrupt = ihActive;
	l.preserveFence = c && c->ibOutstanding != 0;
	l.queue = c ? &c->pm4 : nullptr;
	l.vmid = c ? c->vmid : 0;
	l.pipe = c ? c->pipe : 0;
	l.queueId = c ? c->queue : 0;
	l.fenceValue = c ? nextFence(c->fence) : 0;
	if (c)
		c->fence = l.fenceValue;
	l.doorbell = c ? c->doorbell : 0;
	l.fenceAddress = c ? c->fenceVa : 0;
	l.fenceCpu = c ? c->fenceCpu : nullptr;
	l.queueCpu = c ? c->queueCpu : nullptr;
	l.queueAddress = c ? c->queueVa : 0;
	l.recoveryMqd = c ? c->mqdMc : 0;
	l.recoveryEop = c ? c->eopVa >> 8 : 0;
	l.recoveryRptr = c ? c->rptrVa : 0;
	l.recoveryWpoll = c ? c->wpollVa : 0;
	l.recoveryProofAddress = c ? c->fenceVa : 0;
	l.recoveryProofCpu = c ? c->fenceCpu : nullptr;
	if (c && c->shared) {
		/* rdna4-vmshared: the job runs on the client's shared VMID-0 queue as INDIRECT_BUFFER(vmid) + a ring-level fence to the client's
		 * fence word (an MC address). The packets are written into the client's IB page. */
		SharedQueue &sq = sharedQ[c->sq];
		if (sq.wedged || !sq.up) {
			vmOpTrace("dispatch REFUSED: shared queue wedged/down", c->vmid, sq.pipe, sq.queue);
			return kIOReturnNotResponding;
		}
		l.queue = &sq.pm;
		l.pipe = sq.pipe;
		l.queueId = sq.queue;
		l.doorbell = sq.doorbell;
		l.fenceAddress = poolMc(c->poolOff + kVmFence);
		l.ibCpu = poolDw(c->poolOff + kVmIb);
		l.ibVa = c->ibVa;
		l.ibVmid = vmidForSubmit(*c);
		l.queueCpu = nullptr;
	}

	uint64_t ns = 0;
	if (c)
		vmOpTrace("dispatch entry", l.vmid, l.pipe, l.queueId);
	const bool done = launch(l, "runtime", ns);
	micros = ns / 1000;
	if (c)
		vmOpTrace(done ? "dispatch done" : "dispatch TIMED OUT", l.vmid, l.pipe, l.queueId);
	if (!done) {
		if (c)
			c->ibOutstanding = 0;
		if (c && c->shared) {
			if (recoverSharedQueue(c->sq, c->vmid, "runtime")) {
				RLOG("dispatch timed out after %u ms; shared queue recovered without a GPU reset", l.timeoutUs / 1000);
			} else {
				RLOG("dispatch timed out after %u ms: shared queue %u recovery failed; that queue stays wedged", l.timeoutUs / 1000, c->sq);
			}
			return kIOReturnTimeout;
		}
		if (recoverComputeQueue("runtime", &l)) {
			rtWedged = false;
			RLOG("dispatch timed out after %u ms; queue recovered without a GPU reset", l.timeoutUs / 1000);
		} else {
			vmOpTrace("rtWedged SET by a dispatch timeout (recovery failed)", l.vmid, l.pipe, l.queueId);
			rtWedged = true;
			RLOG("dispatch timed out after %u ms: queue recovery failed; runtime stays wedged",
			     l.timeoutUs / 1000);
		}
		return kIOReturnTimeout;
	}
	if (c) {
		retireIbFences(*c);
		logClientFault(*c, "dispatch");
	}
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtSubmitIb(const void *owner, uint64_t ibVa, uint64_t dwords,
                                  uint64_t flags, uint64_t &fence) {
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	if (!vmEnabled)
		return kIOReturnUnsupported;
	RtClient *c = vmClientFor(owner);
	if (!c)
		return kIOReturnNoResources;
	if (rtWedged && !c->shared) {
		vmOpTrace("submitib REFUSED: rtWedged", c->vmid, c->pipe, c->queue);
		return kIOReturnNotResponding;
	}
	if (c->shared && (sharedQ[c->sq].wedged || !sharedQ[c->sq].up)) {
		vmOpTrace("submitib REFUSED: shared queue wedged/down", c->vmid, c->pipe, c->queue);
		return kIOReturnNotResponding;
	}
	if (!ibVa || (ibVa & 3) || !dwords || dwords > (1u << 20) || flags)
		return kIOReturnBadArgument;
	const uint64_t ibBytes = dwords * 4;
	if (ibBytes / 4 != dwords)
		return kIOReturnBadArgument;
	RtBuffer *containing = nullptr;
	for (RtBuffer &b : buffers) {
		if (b.owner != owner || (!b.device && !b.host) || !b.va || ibVa < b.va)
			continue;
		const uint64_t offset = ibVa - b.va;
		if (offset <= b.bytes && ibBytes <= b.bytes - offset) {
			containing = &b;
			break;
		}
	}
	if (!containing)
		return kIOReturnBadArgument;
	retireIbFences(*c);
	/* A shared queue's ring (4 KiB) carries the jobs of up to four clients: 8 outstanding jobs of 20 dwords each keep it far from full. */
	if (c->ibOutstanding >= (c->shared ? 8u : kMaxIbOutstanding))
		return kIOReturnBusy;

	/* The same VMID-selected shader memory state as launch(): the user IB
	 * supplies the program and resource registers, while this selector only
	 * chains it and fences it. (Shared mode: SH_MEM was written for every VMID at the queues' start.) */
	if (!c->shared) {
		grbmSelect(0, c->pipe, c->queue, c->vmid);
		wr(IpDiscovery::HwGc, ShMemConfig, kShMemConfigDefault);
		wr(IpDiscovery::HwGc, ShMemBases, kShMemBasesDefault);
	}
	const uint32_t value = nextFence(c->fence);
	uint32_t pkt[8];
	Pm4::Queue &ring = c->shared ? sharedQ[c->sq].pm : c->pm4;
	/* Shared: the fence is a ring-level write to the client's fence word (MC address); else a write through the client's VM. */
	const uint64_t fenceAt = c->shared ? poolMc(c->poolOff + kVmFence) : c->fenceVa;
	if (!ring.emit(pkt, Pm4::acquireMem(pkt, Pm4::kGcrMemSync)) ||
	    !ring.emit(pkt, Pm4::indirectBufferCompute(pkt, ibVa, static_cast<uint32_t>(dwords), vmidForSubmit(*c))) ||
	    !ring.emit(pkt, Pm4::releaseMem(pkt, fenceAt, value,
	                                    ihActive && c->pipe < 2)))
		return kIOReturnNoResources;
	c->fence = value;
	flushHdp();
	pm4Kick(ring, c->doorbell, ring.wptr());
	vmOpTrace("submitib kicked", c->vmid, c->pipe, c->queue);
	c->ibFences[c->ibOutstanding++] = value;
	fence = value;
	RLOG("vmid %u: submitted unprivileged compute IB VA 0x%llx, %u dwords, fence %u",
	     c->vmid, ibVa, static_cast<uint32_t>(dwords), value);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtWaitFence(const void *owner, uint32_t fence, uint32_t timeoutMs,
                                   uint64_t &ns) {
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	if (!vmEnabled)
		return kIOReturnUnsupported;
	RtClient *c = vmClientFor(owner);
	if (!c)
		return kIOReturnNoResources;
	if (rtWedged && !c->shared) {
		vmOpTrace("waitfence REFUSED: rtWedged", c->vmid, c->pipe, c->queue);
		return kIOReturnNotResponding;
	}
	if (timeoutMs > RDNA4_MAX_TIMEOUT_MS ||
	    static_cast<int32_t>(fence - c->fence) > 0)
		return kIOReturnBadArgument;
	const uint32_t waitMs = timeoutMs ? timeoutMs : 1000;
	bool done = false;
	if (ihActive && c->pipe < 2) {
		done = ihWaitFence(c->fenceCpu, fence, waitMs, true, "IB", ns);
	} else {
		const uint64_t t0 = mach_absolute_time();
		uint64_t span = 0;
		nanoseconds_to_absolutetime(static_cast<uint64_t>(waitMs) * 1000000, &span);
		for (uint32_t polls = 0;; polls++) {
			done = fenceReached(*c->fenceCpu, fence);
			if (done || mach_absolute_time() - t0 > span)
				break;
			if (polls < 200)
				IODelay(10);
			else
				IOSleep(1);
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	}
	if (done) {
		retireIbFences(*c);
		logClientFault(*c, "IB wait");
		return kIOReturnSuccess;
	}

	if (c->shared) {
		/* The shared queue's recovery kills this client's waves only (SQ_CMD CHECK_VMID) and re-initialises the queue. */
		const bool ok = recoverSharedQueue(c->sq, c->vmid, "IB");
		c->ibOutstanding = 0;
		if (ok) {
			*c->fenceCpu = 0;
			flushHdp();
		}
		RLOG("IB fence %u timed out after %u ms; shared queue %u %s", fence, waitMs, c->sq,
		     ok ? "recovered without a GPU reset" : "recovery failed; that queue stays wedged");
		return kIOReturnTimeout;
	}
	Launch l {};
	l.queue = &c->pm4;
	l.vmid = c->vmid;
	l.pipe = c->pipe;
	l.queueId = c->queue;
	l.doorbell = c->doorbell;
	l.queueCpu = c->queueCpu;
	l.queueAddress = c->queueVa;
	l.recoveryMqd = c->mqdMc;
	l.recoveryEop = c->eopVa >> 8;
	l.recoveryRptr = c->rptrVa;
	l.recoveryWpoll = c->wpollVa;
	l.recoveryProofAddress = c->fenceVa;
	l.recoveryProofCpu = c->fenceCpu;
	const bool recovered = recoverComputeQueue("IB", &l);
	c->ibOutstanding = 0;
	if (recovered) {
		/* W6's proof WRITE_DATA is not a client fence value. */
		*c->fenceCpu = 0;
		flushHdp();
		rtWedged = false;
		RLOG("IB fence %u timed out after %u ms; queue recovered without a GPU reset",
		     fence, waitMs);
	} else {
		vmOpTrace("rtWedged SET by an IB wait timeout (recovery failed)", c->vmid, c->pipe, c->queue);
		rtWedged = true;
		RLOG("IB fence %u timed out after %u ms; queue recovery failed; runtime stays wedged",
		     fence, waitMs);
	}
	return kIOReturnTimeout;
}

IOReturn RDNA4Compute::rtPresent(const void *owner, uint64_t handle, uint64_t offset,
                                 uint64_t &geometry, uint64_t &pitch) {
	if (!featureAllowed("flip"))
		return kIOReturnUnsupported;
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;

	Flip::Surface surface {};
	if (!handle) {
		if (!Flip::findPipe(*this, surface))
			return kIOReturnNotReady;
		geometry = (static_cast<uint64_t>(surface.width) & 0xffffu) |
		           ((static_cast<uint64_t>(surface.height) & 0xffffu) << 16);
		pitch = surface.pitch;
		return kIOReturnSuccess;
	}
	if (presentOwner && presentOwner != owner)
		return kIOReturnBusy;
	dropPendingPresentsLocked(kIOReturnAborted);
	RtBuffer *b = bufferFor(owner, handle);
	if (!b || !b->device || (offset & 255u))
		return kIOReturnBadArgument;
	if (presentActive) {
		surface = presentSurface;
	} else if (!Flip::findPipe(*this, surface)) {
		return kIOReturnNotReady;
	}
	const uint64_t bytes = Flip::surfaceBytes(surface.pitch, surface.height);
	if (!bytes || offset > b->bytes || bytes > b->bytes - offset || offset > ~0ull - b->mc)
		return kIOReturnBadArgument;
	const uint64_t target = b->mc + offset;
	// Keep a rollback target armed before the first address write.  flipTo
	// can fail after the user address has latched, in which case the desktop
	// must be restored immediately and remain eligible for auto-restore.
	presentActive = true;
	presentOwner = owner;
	presentHandle = handle;
	presentOffset = offset;
	presentStarted = mach_absolute_time();
	presentSurface = surface;
	if (!Flip::flipTo(*this, surface, target, "present")) {
		(void)restorePresentationLocked("present failure");
		return kIOReturnNotResponding;
	}
	geometry = (static_cast<uint64_t>(surface.width) & 0xffffu) |
	           ((static_cast<uint64_t>(surface.height) & 0xffffu) << 16);
	pitch = surface.pitch;
	RLOG("present: buffer 0x%llx +0x%llx, %ux%u pitch %u", handle, offset,
	     surface.width, surface.height, surface.pitch);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtPresentAsync(const void *owner, uint64_t handle, uint64_t offset,
                                      uint64_t &presentId) {
	if (!featureAllowed("flip"))
		return kIOReturnUnsupported;
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	if (!rtReady)
		return kIOReturnNotReady;
	if (!presentTimer)
		return kIOReturnNotReady;
	if (presentOwner && presentOwner != owner)
		return kIOReturnBusy;
	if (presentPending >= 2)
		return kIOReturnBusy;
	RtBuffer *b = bufferFor(owner, handle);
	if (!b || !b->device || (offset & 255u))
		return kIOReturnBadArgument;

	Flip::Surface surface = presentSurface;
	if (!presentOwner && !Flip::findPipe(*this, surface))
		return kIOReturnNotReady;
	const uint64_t bytes = Flip::surfaceBytes(surface.pitch, surface.height);
	if (!bytes || offset > b->bytes || bytes > b->bytes - offset || offset > ~0ull - b->mc)
		return kIOReturnBadArgument;
	PresentSlot *slot = nullptr;
	for (PresentSlot &candidate : presentSlots)
		if (!candidate.state) {
			slot = &candidate;
			break;
		}
	if (!slot)
		return kIOReturnBusy;
	if (!presentOwner) {
		presentOwner = owner;
		presentSurface = surface;
	}
	presentId = nextPresentId++;
	if (!presentId)
		presentId = nextPresentId++;
	*slot = PresentSlot { owner, handle, offset, presentId, 0, kIOReturnSuccess, 1 };
	presentPending++;
	schedulePresentationTimer();
	if (detailedFlipTrace())
		RLOG("present async: queued id %llu buffer 0x%llx +0x%llx (%u/%u pending), timer armed 1 ms",
		     presentId, handle, offset, presentPending, 2u);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtWaitPresent(const void *owner, uint64_t presentId, uint32_t timeoutMs,
                                     uint64_t &frame) {
	if (!featureAllowed("flip"))
		return kIOReturnUnsupported;
	if (!presentId || timeoutMs > RDNA4_MAX_TIMEOUT_MS)
		return kIOReturnBadArgument;
	if (!timeoutMs)
		timeoutMs = 1000;
	uint64_t span = 0;
	nanoseconds_to_absolutetime(static_cast<uint64_t>(timeoutMs) * 1000000ull, &span);
	const uint64_t start = mach_absolute_time();
	for (;;) {
		{
			Locked g(rtLock);
			checkPresentationTimeoutLocked();
			PresentSlot *slot = presentSlot(presentId, owner);
			if (!slot)
				return kIOReturnBadArgument;
			if (slot->state == 2) {
				const IOReturn result = slot->result;
				frame = slot->frame;
				*slot = PresentSlot {};
				return result;
			}
		}
		if (mach_absolute_time() - start >= span)
			return kIOReturnTimeout;
		IOSleep(1);
	}
}

IOReturn RDNA4Compute::rtRestore(const void *owner) {
	Locked g(rtLock);
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	checkPresentationTimeoutLocked();
	if (!presentOwner)
		return kIOReturnSuccess;
	if (presentOwner != owner)
		return kIOReturnBusy;
	return restorePresentationLocked("restore");
}

void RDNA4Compute::rtRelease(const void *owner) {
	if (!rtLock || !owner)
		return;
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	RtClient *early = clientFor(owner);
	if (powerSleeping || (early && early->aborted)) {
		// Hardware is off or has already been rebuilt. The resume cleanup owns
		// the old allocations (and dropped the pending presents); never touch
		// a dead HQD from clientClose(). Only forget this owner's slots.
		for (PresentSlot &slot : presentSlots)
			if (slot.owner == owner)
				slot = PresentSlot {};
		if (early)
			*early = RtClient {};
		return;
	}
	// Pending async presents count too: restore drops them even when nothing
	// has been flipped yet.
	if (presentOwner == owner)
		(void)restorePresentationLocked("client close");
	for (PresentSlot &slot : presentSlots)
		if (slot.owner == owner)
			slot = PresentSlot {};
	RtClient *c = clientFor(owner);
	uint32_t nb = 0, np = 0;
	bool hostUnmapped = false;
	for (RtBuffer &b : buffers) {
		if (b.owner == owner) {
			if (c)
				vmUnmap(*c, b.va, b.bytes);
			if (b.host) {
				hostUnmapped = true;
			} else {
				(b.device ? devHeap : heap).free(b.offset);
				b.owner = nullptr;
			}
			nb++;
		}
	}
	for (RtProgram &p : programs) {
		if (p.owner == owner) {
			if (c)
				vmUnmap(*c, p.va, heap.lengthOf(p.offset));
			heap.free(p.offset);
			p.owner = nullptr;
			np++;
		}
	}
	if (c && hostUnmapped && !vmInvalidate(c->vmid, "host unmap"))
		RLOG("vmid %u: host buffer cleanup invalidation timed out", c->vmid);
	for (RtBuffer &b : buffers) {
		if (b.owner == owner && b.host) {
			const uint64_t rounded = (b.bytes + GpuVm::kPageBytes - 1) &
			                         ~(GpuVm::kPageBytes - 1);
			if (c && c->hostBytes >= rounded)
				c->hostBytes -= rounded;
			if (hostBytesTotal >= rounded)
				hostBytesTotal -= rounded;
			RLOG("vmid %u: host buffer unmapped during client close", c ? c->vmid : 0);
			releaseHost(b);
			b.owner = nullptr;
		}
	}
	if (nb || np)
		RLOG("client closed: freed %u buffer(s), %u program(s)", nb, np);
	if (c && vmEnabled) {
		if (c->shared) {
			/* No HQD to dequeue. Jobs still in flight on the shared queue must finish before the tables go: wait (bounded) for
			 * the client's fence, and recover the queue (kill this VMID's waves) if they do not. */
			vmOpTrace("release (shared)", c->vmid, c->pipe, c->queue);
			if (c->ibOutstanding) {
				const uint32_t want = c->fence;
				for (uint32_t ms = 0; ms < 500 && !fenceReached(*c->fenceCpu, want); ms++)
					IOSleep(1);
				if (!fenceReached(*c->fenceCpu, want))
					(void)recoverSharedQueue(c->sq, c->vmid, "client close");
			}
			wr(IpDiscovery::HwGc, Reg { 0, GcCtx1Cntl.dword + c->vmid - 1 }, 0);   // the context goes off before its tables do
		} else {
		/* Dequeue is deliberately polled: W1's interrupt path is not required. */
		vmOpTrace("release before dequeue", c->vmid, c->pipe, c->queue);
		grbmSelect(1, c->pipe, c->queue, c->vmid);
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
		bool inactive = false;
		for (uint32_t us = 0; us < 100000; us += 10) {
			if (!(rdGc(CpHqdActive) & 1)) { inactive = true; break; }
			IODelay(10);
		}
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
		if (!inactive)
			RLOG("vmid %u: queue MEC1 pipe %u queue %u dequeue timeout (ACTIVE 0x%08x)",
			     c->vmid, c->pipe, c->queue, rdGc(CpHqdActive));
		grbmSelect(0, 0, 0, 0);
		vmOpTrace(inactive ? "release after dequeue" : "release after dequeue TIMEOUT", c->vmid, c->pipe, c->queue);
		}
		/* The table allocation is reused by the next client.  Unmapping each
		 * live object leaves untouched PDEs/PTEs behind, so clear the complete
		 * image before releasing the VMID or its backing VRAM. */
		bzero(c->tableShadow, kVmTableBytes);
		if (!vmTableSync(*c, 0, kVmTableBytes))
			RLOG("vmid %u: page-table teardown clear failed", c->vmid);
		vmInvalidate(c->vmid, "client close");
		vmidUsed[c->vmid] = false;
		if (!c->shared)
			queueUsed[c->pipe][c->queue] = false;
		IOFree(c->tableShadow, kVmTableBytes);
		devHeap.free(c->tableOffset);
		if (c->shared)
			RLOG("vmid %u: shared-queue client closed, freed its page tables", c->vmid);
		else
			RLOG("vmid %u: client closed, freed MEC1 pipe %u queue %u and page tables",
			     c->vmid, c->pipe, c->queue);
		*c = RtClient {};
	} else if (c) {
		*c = RtClient {};
	}
}

void RDNA4Compute::powerWillSleep() {
	if (!rtLock)
		return;
	Locked g(rtLock);
	if (powerSleeping)
		return;
	rtReady = false;
	powerSleeping = true;
	resumePending = false;
	RLOG("power: quiesce begin (runtime not ready)");
	// Calls serialize on rtLock, so no dispatch or SDMA fence can still be
	// executing here. Drain every HQD with the same bounded poll used by W6.
	auto drainQueue = [this](uint32_t pipe, uint32_t queue, uint32_t vmid,
	                     const Launch *recovery) {
		grbmSelect(1, pipe, queue, vmid);
		if (!(rdGc(CpHqdActive) & 1)) {
			grbmSelect(0, 0, 0, 0);
			return true;
		}
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
		bool inactive = false;
		for (uint32_t us = 0; us < 100000; us += 10) {
			if (!(rdGc(CpHqdActive) & 1)) { inactive = true; break; }
			IODelay(10);
		}
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
		if (!inactive && recovery) {
			RLOG("power: HQD %u/%u VMID %u timed out; invoking W6 recovery", pipe, queue, vmid);
			inactive = recoverComputeQueue("sleep", recovery);
		}
		if (!inactive)
			RLOG("power: HQD %u/%u VMID %u did not drain within the bound", pipe, queue, vmid);
		grbmSelect(0, 0, 0, 0);
		return inactive;
	};
	(void)drainQueue(0, 0, 0, nullptr);
	/* rdna4-vmshared: the shared queues are the only HQDs the clients use; they are re-created at the next open after the wake. */
	if (vmShared && sharedInit)
		sharedStopAll("system sleep");
	for (RtClient &c : clients) {
		if (!c.active || !c.vmid || c.shared)
			continue;
		Launch recovery {};
		recovery.queue = &c.pm4;
		recovery.queueCpu = c.queueCpu;
		recovery.queueAddress = c.queueVa;
		recovery.vmid = c.vmid;
		recovery.pipe = c.pipe;
		recovery.queueId = c.queue;
		recovery.doorbell = c.doorbell;
		recovery.recoveryMqd = c.mqdMc;
		recovery.recoveryEop = c.eopVa;
		recovery.recoveryRptr = c.rptrVa;
		recovery.recoveryWpoll = c.wpollVa;
		recovery.recoveryProofAddress = c.fenceVa;
		recovery.recoveryProofCpu = c.fenceCpu;
		(void)drainQueue(c.pipe, c.queue, c.vmid, &recovery);
	}
	if (ihActive)
		ihStop();
	// The flip implementation has no independent timer in this worktree;
	// IH DCN teardown is the hook that stops its vblank/pflip activity.
	RLOG("power: IH disabled, flip timer hook stopped");
	wr(IpDiscovery::HwGc, CpMeCntl, rdGc(CpMeCntl) | kCpMePfpHalt | kCpMeMeHalt);
	wr(IpDiscovery::HwGc, CpMecRs64Cntl, rdGc(CpMecRs64Cntl) | kRs64Halt);
	dmaTeardown("system sleep");
	RLOG("power: quiesce complete; compute engines halted");
}

void RDNA4Compute::resetRuntimeForResume() {
	if (!rtLock)
		return;
	Locked g(rtLock);
	for (RtBuffer &b : buffers) {
		if (!b.owner)
			continue;
		if (b.host)
			releaseHost(b);
		else if (b.device)
			devHeap.free(b.offset);
		else
			heap.free(b.offset);
		b = RtBuffer {};
	}
	for (RtProgram &p : programs) {
		if (p.owner) {
			heap.free(p.offset);
			p = RtProgram {};
		}
	}
	for (RtClient &c : clients) {
		if (!c.active)
			continue;
		if (c.tableShadow) {
			IOFree(c.tableShadow, kVmTableBytes);
			c.tableShadow = nullptr;
		}
		c.aborted = true;
		c.tableOffset = c.rootMc = c.rootPhys = 0;
		c.queueCpu = nullptr;
		c.kernargCpu = c.fenceCpu = nullptr;
	}
	bzero(vmidUsed, sizeof(vmidUsed));
	bzero(queueUsed, sizeof(queueUsed));
	for (SharedQueue &q : sharedQ)
		q.up = q.wedged = false;
	sharedInit = false;
	if (devHeapMap) {
		IOFree(devHeapMap, devHeapMapBytes);
		devHeapMap = nullptr;
		devHeapMapBytes = 0;
	}
	heap = GpuHeap::Heap {};
	devHeap = GpuHeap::Heap {};
	hostBytesTotal = 0;
	// Queued async presents point at buffers that are gone: fail them now, or
	// the present timer would flip the rebuilt display to a stale address.
	dropPendingPresentsLocked(kIOReturnAborted);
	presentActive = false;
	presentOwner = nullptr;
	rtWedged = false;
	rtReady = false;
	RLOG("power: old runtime buffers, heaps and VM contexts discarded; client objects retained as aborted");
}

// ---------------------------------------------------------------------------
// DMA: SDMA between VRAM and a pinned bounce buffer in host memory
// ---------------------------------------------------------------------------

// One packet on SDMA0 queue 0 with a FENCE after it; waits for the fence.
bool RDNA4Compute::sdmaRun(const uint32_t *pkt, uint32_t dwords, uint32_t timeoutMs) {
	uint32_t fence[Sdma::kFenceDwords];
	uint32_t trap[Sdma::kTrapDwords];
	const uint32_t value = ++sdmaFence;
	Sdma::fence(fence, poolMc(kSdmaFenceOffset), value);
	Sdma::trap(trap);
	if (!sdmaRing.emit(pkt, dwords) || !sdmaRing.emit(fence, Sdma::kFenceDwords) ||
	    !sdmaRing.emit(trap, Sdma::kTrapDwords))
		return false;
	sdmaKick(sdmaRing.wptr());
	if (ihActive) {
		uint64_t ns = 0;
		const bool done = ihWaitFence(poolDw(kSdmaFenceOffset), value, timeoutMs, false, "dma", ns);
		if (!done)
			RLOG("dma: SDMA queue remains wedged; this gfx12 path has no source-backed no-MES queue reset");
		return done;
	}
	uint64_t t0 = mach_absolute_time(), span = 0;
	nanoseconds_to_absolutetime(static_cast<uint64_t>(timeoutMs) * 1000000, &span);
	for (uint32_t polls = 0;; polls++) {
		if (*poolDw(kSdmaFenceOffset) == value)
			return true;
		if (mach_absolute_time() - t0 > span)
			break;
		if (polls < 200)
			IODelay(10);
		else
			IOSleep(1);
	}
	RLOG("dma: SDMA fence %u never came (0x%08x)", value, *poolDw(kSdmaFenceOffset));
	// The current gfx12 amdgpu source exposes SDMA reset through MES. There is
	// no source-backed no-MES queue reset sequence for this bring-up path, so a
	// timed-out SDMA queue remains wedged rather than risking the display.
	RLOG("dma: SDMA queue remains wedged; no source-backed no-MES queue reset");
	logGcFault("dma");
	return false;
}

// One COPY_LINEAR between VRAM and the bounce buffer, through the AGP
// aperture (bus address b is MC agpStart + b).
bool RDNA4Compute::bounceCopy(uint64_t vram, uint32_t hostOffset, uint32_t bytes, bool toGpu) {
	if (!bytes || hostOffset > kBounceBytes || bytes > kBounceBytes - hostOffset)
		return false;
	const uint64_t host = agpStart + bounceBus + hostOffset;
	uint32_t pkt[Sdma::kCopyDwords];
	Sdma::copyLinear(pkt, toGpu ? host : vram, toGpu ? vram : host, bytes);
	return sdmaRun(pkt, Sdma::kCopyDwords, 2000);
}

bool RDNA4Compute::dmaInit() {
	if (!poolCpu || !sdmaRing.sizeBytes() || !sv.fbMcTop) {
		RLOG("dma: not set up: %s", !sdmaRing.sizeBytes() ? "no SDMA queue" : "no compute pool");
		return false;
	}
	// 1. Pinned (buffer memory is wired), physically contiguous, 40-bit.
	bounce = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kBounceBytes,
		0x000000fffffff000ull);
	if (!bounce) {
		dmaTeardown("no contiguous 16 MiB bounce buffer");
		return false;
	}
	bounceVa = static_cast<uint8_t *>(bounce->getBytesNoCopy());

	// 2. The address the device uses for it: an IOMapper's, if the system
	//    has one, else the physical one. One segment, or no DMA.
	bounceDma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 40, 0,
	                                            IODMACommand::kMapped, 0, 1);
	UInt64 off = 0;
	IODMACommand::Segment64 seg {};
	UInt32 nseg = 1;
	if (!bounceVa || !bounceDma || bounceDma->setMemoryDescriptor(bounce) != kIOReturnSuccess ||
	    bounceDma->gen64IOVMSegments(&off, &seg, &nseg) != kIOReturnSuccess || nseg != 1 ||
	    seg.fLength < kBounceBytes) {
		dmaTeardown("the bounce buffer has no single DMA segment");
		return false;
	}
	bounceBus = seg.fIOVMAddr;

	// 3. amdgpu's AGP layout (amdgpu_gmc_agp_location, amdgpu_gmc_agp_addr,
	//    gfxhub_v12_0_init_system_aperture_regs): AGP_BASE 0, the aperture
	//    16 GiB-aligned right after the FB, bus address b at agpStart + b,
	//    and the system aperture extended over it. It reaches bus addresses
	//    up to the bounce buffer's end, not all of memory.
	agpStart = (sv.fbMcTop + (16ull << 30)) & ~((16ull << 30) - 1);
	const uint64_t agpEnd = agpStart + bounceBus + kBounceBytes - 1;
	wr(IpDiscovery::HwGc, GcAgpBase, 0);
	wr(IpDiscovery::HwGc, GcAgpBot, static_cast<uint32_t>(agpStart >> 24));
	wr(IpDiscovery::HwGc, GcAgpTop, static_cast<uint32_t>(agpEnd >> 24));
	wr(IpDiscovery::HwGc, GcSysApertureHigh, static_cast<uint32_t>(agpEnd >> 18));
	gcHubFlush();

	// 4. The GPU may master the bus from here on (the display never needs to).
	busMasterWas = env.pci->setBusMasterEnable(true);
	busMasterSet = true;

	// 5. Self-test, reads first: until the GPU has fetched a known pattern
	//    from the bounce buffer, a wrong mapping costs a mismatch, never a
	//    write into memory that is not ours.
	constexpr uint32_t n = 64 << 10;
	auto pat1 = [](uint32_t i) { return 0xB0A7C0DEu ^ (i * 2654435761u); };
	auto pat2 = [](uint32_t i) { return 0x5EEDF00Du + i * 7; };
	uint32_t *host = reinterpret_cast<uint32_t *>(bounceVa);
	for (uint32_t i = 0; i < 3 * n / 4; i++)
		host[i] = i < n / 4 ? pat1(i) : 0;
	bounce->performOperation(kIOMemoryIncoherentIOFlush, 0, 3 * n);
	for (uint32_t i = 0; i < 2 * n / 4; i++)
		*poolDw(kDmaTestOffset + 4 * i) = i < n / 4 ? 0 : pat2(i - n / 4);
	flushHdp();
	bool ok = bounceCopy(poolMc(kDmaTestOffset), 0, n, true);
	uint32_t bad = 0;
	for (uint32_t i = 0; ok && i < n / 4; i++)
		bad += *poolDw(kDmaTestOffset + 4 * i) != pat1(i);
	if (!ok || bad) {
		dmaTeardown(!ok ? "the host->VRAM test copy never finished"
		                : "the host->VRAM test copy delivered wrong data (read only; nothing written)");
		return false;
	}
	// 6. Then writes: VRAM into the buffer's second 64 KiB; the third must
	//    stay untouched.
	ok = bounceCopy(poolMc(kDmaTestOffset + n), n, n, false);
	bounce->performOperation(kIOMemoryIncoherentIOFlush, n, 2 * n);
	for (uint32_t i = 0; ok && i < n / 4; i++)
		bad += host[n / 4 + i] != pat2(i) || host[n / 2 + i] != 0;
	if (!ok || bad) {
		dmaTeardown(!ok ? "the VRAM->host test copy never finished"
		                : "the VRAM->host test copy delivered wrong data");
		return false;
	}

	// 7. Speed, for the log: the whole buffer each way, into the pool's
	//    heap area (not handed out yet).
	uint64_t t0 = mach_absolute_time(), ns1 = 0, ns2 = 0;
	ok = bounceCopy(poolMc(kHeapOffset), 0, kBounceBytes, true);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns1);
	t0 = mach_absolute_time();
	ok = ok && bounceCopy(poolMc(kHeapOffset), 0, kBounceBytes, false);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns2);
	if (!ok) {
		dmaTeardown("a 16 MiB test copy never finished");
		return false;
	}
	dmaReady = true;
	RLOG("dma: on — bounce buffer 16 MiB at bus 0x%llx, AGP aperture MC 0x%llx..0x%llx; self-test "
	     "ok; SDMA host->VRAM %llu MB/s, VRAM->host %llu MB/s", bounceBus, agpStart, agpEnd,
	     ns1 ? kBounceBytes * 1000ull / ns1 : 0, ns2 ? kBounceBytes * 1000ull / ns2 : 0);
	return true;
}

void RDNA4Compute::dmaTeardown(const char *why) {
	RLOG("dma: off — %s; transfers stay on the CPU through the BAR", why);
	dmaReady = false;
	wr(IpDiscovery::HwGc, GcAgpBase, 0);
	wr(IpDiscovery::HwGc, GcAgpBot, 0xffffff);                 // aperture closed
	wr(IpDiscovery::HwGc, GcAgpTop, 0);
	wr(IpDiscovery::HwGc, GcSysApertureHigh, static_cast<uint32_t>(sv.fbMcTop >> 18));
	if (busMasterSet) {
		env.pci->setBusMasterEnable(busMasterWas);
		busMasterSet = false;
	}
	if (bounceDma) {
		bounceDma->clearMemoryDescriptor();
		bounceDma->release();
		bounceDma = nullptr;
	}
	OSSafeReleaseNULL(bounce);
	bounceVa = nullptr;
	bounceBus = 0;
}

// VRAM past the BAR and the compute pool: from the pool's end up to half
// of the card's VRAM (at most 8 GiB), always below the DMUB memory the
// display keeps near the top; the firmware's reservations (TMR, discovery)
// sit above that too.
void RDNA4Compute::devHeapInit() {
	const uint64_t base = pool.offset + pool.size;
	uint64_t top = static_cast<uint64_t>(sv.vramMiB) << 19;
	if (top > (8ull << 30))
		top = 8ull << 30;
	if (dmubVram && dmubVram < top)
		top = dmubVram;
	top &= ~static_cast<uint64_t>(kDevHeapGranule - 1);
	if (top <= base + (64ull << 20)) {
		RLOG("dma: no VRAM past the BAR for buffers (top 0x%llx); buffers stay in the pool", top);
		return;
	}
	const uint64_t granules = (top - base) / kDevHeapGranule;
	devHeapMap = static_cast<uint8_t *>(IOMalloc(granules));
	if (!devHeapMap) {
		RLOG("dma: no memory for the device heap's map; buffers stay in the pool");
		return;
	}
	devHeapMapBytes = static_cast<uint32_t>(granules);
	devHeap.init(base, top - base, kDevHeapGranule, devHeapMap, devHeapMapBytes);
	RLOG("dma: buffers from VRAM+0x%llx..+0x%llx (%llu MiB, past the BAR)", base, top,
	     (top - base) >> 20);
}

// Write/Read through the bounce buffer, 16 MiB at a time. The buffer is
// cacheable host memory and AGP accesses do not snoop, so the CPU caches are
// flushed around each copy: before the GPU reads it, and before and after
// the GPU writes it (no dirty line may land on the data later, and none
// fetched meanwhile may be read).
IOReturn RDNA4Compute::dmaCopy(task_t task, mach_vm_address_t user, uint64_t mc, uint64_t length,
                               bool toGpu) {
	for (uint64_t done = 0; done < length;) {
		const uint32_t n = static_cast<uint32_t>(length - done < kBounceBytes ? length - done
		                                                                        : kBounceBytes);
		IOReturn r;
		if (toGpu) {
			if ((r = userCopy(task, user + done, bounceVa, n, true)) != kIOReturnSuccess)
				return r;
			bounce->performOperation(kIOMemoryIncoherentIOFlush, 0, n);
			if (!bounceCopy(mc + done, 0, n, true))
				return kIOReturnIOError;
		} else {
			bounce->performOperation(kIOMemoryIncoherentIOFlush, 0, n);
			if (!bounceCopy(mc + done, 0, n, false))
				return kIOReturnIOError;
			bounce->performOperation(kIOMemoryIncoherentIOFlush, 0, n);
			if ((r = userCopy(task, user + done, bounceVa, n, false)) != kIOReturnSuccess)
				return r;
		}
		done += n;
	}
	return kIOReturnSuccess;
}

/* E1: the D1 registers of both hubs, read-only and compact.  GC 0x15a0-0x15b3
 * (GCMC_VM_NB_*, FB_OFFSET, default page, CACHEABLE_DRAM, LOCAL_SYSMEM, APT_CNTL,
 * LOCAL_FB) and 0x1614-0x161b (FB location, AGP, system aperture, L1 TLB) against
 * MM 0x04c0-0x04d3 and 0x0554-0x055b (gc_12_0_0_offset.h, mmhub_4_1_0_offset.h);
 * the two blocks are 0x10e0 apart. */
void RDNA4Compute::vmDumpHubWindows(const char *tag) {
	if (!poolCpu)
		return;
	static const struct { uint32_t gc, mm, n; } blocks[] = { { 0x15a0, 0x04c0, 20 }, { 0x1614, 0x0554, 8 } };
	for (uint32_t b = 0; b < 2; b++) {
		uint32_t g[20], m[20], differ = 0;
		for (uint32_t i = 0; i < blocks[b].n; i++) {
			/* GCMC_VM_SHARED_VIRT_RESET_REQ (GC 0x15ab, MM 0x04cb) is reset-named:
			 * never read it. */
			if (b == 0 && i == 11) {
				g[i] = m[i] = 0;
				continue;
			}
			g[i] = rdGc(GfxReg::Reg { 0, blocks[b].gc + i });
			m[i] = rd(IpDiscovery::HwMmhub, GfxReg::Reg { 0, blocks[b].mm + i });
			if (g[i] != m[i])
				differ |= 1u << i;
		}
		for (uint32_t half = 0; half < blocks[b].n; half += 10) {
			const uint32_t n = blocks[b].n - half < 10 ? blocks[b].n - half : 10;
			char gl[128], ml[128];
			uint32_t gp = 0, mp = 0;
			for (uint32_t i = 0; i < n; i++) {
				gp += snprintf(gl + gp, sizeof(gl) - gp, " %08x", g[half + i]);
				mp += snprintf(ml + mp, sizeof(ml) - mp, " %08x", m[half + i]);
			}
			RLOG("vm: E1%s: GC[%04x+%u]%s", tag, blocks[b].gc + half, half, gl);
			RLOG("vm: E1%s: MM[%04x+%u]%s", tag, blocks[b].mm + half, half, ml);
		}
		RLOG("vm: E1%s: block %04x differs at index mask 0x%05x (bit i = dword base+i)", tag,
		     blocks[b].gc, differ);
	}
}
