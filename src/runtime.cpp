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
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>

#define RLOG(fmt, ...)  IOLog("RDNA4FB: runtime: " fmt "\n", ## __VA_ARGS__)

using namespace GfxReg;

namespace {

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

void RDNA4Compute::publishRuntime(uint32_t stage) {
	if (!initRuntimeHeap()) {
		RLOG("not published: %s", !rtLock ? "no lock" : "no room for a heap in the pool");
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
		return;
	}

	auto *svc = OSTypeAlloc(RDNA4ComputeService);
	if (!svc || !svc->init()) {
		OSSafeReleaseNULL(svc);
		RLOG("not published: could not create the service");
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
		return;
	}
	if (!svc->registerPowerManagement(env.pci)) {
		RLOG("not published: power management registration failed");
		svc->terminate();
		svc->release();
		return;
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
	const bool fragment64k = (va & 0xffff) == 0 && (mc & 0xffff) == 0 &&
		((bytes + 0xffff) & ~0xffffull) >= 0x10000;
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
		uint64_t flags = GpuVm::kValid | GpuVm::kSnooped | GpuVm::kReadable | GpuVm::kWritable;
		if (executable)
			flags |= GpuVm::kExecutable;
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
		                 GpuVm::kReadable | GpuVm::kWritable;
		if (executable)
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
	const uint32_t faultDefaults = ((1u << 14) - 1) << 10;
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
	const uint32_t request = (1u << vmid) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22) |
	                         (1u << 23) | (1u << 24); /* clear fault status */
	wr(IpDiscovery::HwGc, req, request);
	for (uint32_t us = 0; us < 100000; us += 10) {
		if (rdGc(ack) & (1u << vmid))
			return true;
		IODelay(10);
	}
	RLOG("vmid %u: VM invalidate timeout (%s), REQ 0x%08x ACK 0x%08x", vmid, tag,
	     rdGc(req), rdGc(ack));
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
	if (!initRuntimeHeap())
		return false;
	RtClient c {};
	const uint32_t vmid = 8, pipe = 0, queue = 1, doorbell = 0x1a;
	const uint32_t qoff = kVmQueueBase;
	const uint64_t qva = GpuVm::kVaStart;
	const uint64_t rva = qva + 0x1000, dataVa = qva + 0x2000, fenceVa = qva + 0x3000;
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
	const bool qMap = vmMap(c, qva, poolMc(qoff + kVmPq), kPqSize, false);
	const bool rMap = qMap && vmMap(c, rva, poolMc(qoff + kVmRptr), 0x1000, false);
	const bool dMap = rMap && vmMap(c, dataVa, poolMc(qoff + kVmWptr), 0x1000, false);
	const bool fMap = dMap && vmMap(c, fenceVa, poolMc(qoff + kVmFence), 0x1000, false);
	const bool qInit = fMap && c.pm4.init(poolDw(qoff + kVmPq), qva, kPqSize);
	if (!qInit) {
		RLOG("vm: boot page-table setup q=%d r=%d data=%d fence=%d pm4=%d",
		     qMap, rMap, dMap, fMap, qInit);
		RLOG("vm: boot page-table or queue setup failed");
		IOFree(c.tableShadow, kVmTableBytes);
		devHeap.free(table);
		return false;
	}
	trail("vm: VM context enable");
	bool context = vmContextInit(c);
	if (!context)
		RLOG("vm: boot VM context setup failed (VMID %u)", vmid);
	bool hqd = false, inactive = true, fence = false;
	if (context) {
		trail("vm: HQD activate");
		hqd = hqdInitFor(false, pipe, queue, vmid, poolMc(qoff + kVmMqd),
		                 poolMc(qoff + kVmEop) >> 8, qva >> 8, rva, rva, doorbell);
		*poolDw(qoff + kVmWptr) = 0;
		*c.fenceCpu = 0;
		flushHdp();
		uint32_t pkt[8];
		const uint32_t value = 0x564d0001;
		c.pm4.emit(pkt, Pm4::writeData(pkt, dataVa, 0x600df00d));
		c.pm4.emit(pkt, Pm4::releaseMem(pkt, fenceVa, value));
		trail("vm: queue kick");
		pm4Kick(c.pm4, doorbell, c.pm4.wptr());
		for (uint32_t us = 0; us < 200000 && !fence; us += 10) {
			fence = *c.fenceCpu == value;
			if (!fence)
				IODelay(10);
		}
		if (!fence || *poolDw(qoff + kVmWptr) != 0x600df00d)
			RLOG("vm: boot queue test failed (fence 0x%08x, data 0x%08x)",
			     *c.fenceCpu, *poolDw(qoff + kVmWptr));
	}
	if (context && (hqd || !fence)) {
		trail("vm: HQD dequeue");
		grbmSelect(1, pipe, queue, vmid);
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
	return context && hqd && fence && inactive && flushed;
}

void RDNA4Compute::logClientFault(RtClient &c, const char *tag) {
	const uint32_t status = rdGc(GcL2FaultStatusLo);
	if (!status)
		return;
	const uint64_t address = rdGc(GcL2FaultAddrLo) |
		(static_cast<uint64_t>(rdGc(GcL2FaultAddrHi)) << 32);
	RLOG("vmid %u: %s: GC hub fault status 0x%08x address 0x%llx", c.vmid, tag, status, address);
	vmInvalidate(c.vmid, "fault clear");
}

IOReturn RDNA4Compute::rtOpen(const void *owner) {
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
	uint32_t vmid = 0;
	for (vmid = 8; vmid <= 15 && vmidUsed[vmid]; vmid++) {}
	if (vmid > 15)
		return kIOReturnNoResources;
	uint32_t pipe = 0, queue = 0;
	for (; pipe < 4; pipe++) {
		for (queue = 0; queue < 8; queue++)
			if (!(pipe == 0 && queue == 0) && !queueUsed[pipe][queue])
				break;
		if (queue < 8)
			break;
	}
	if (pipe == 4)
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
	if (!vmMap(*c, qva, poolMc(qoff + kVmPq), kPqSize, false) ||
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
	return kIOReturnSuccess;
}

void RDNA4Compute::clearPresentationLocked() {
	presentActive = false;
	presentOwner = nullptr;
	presentHandle = 0;
	presentOffset = 0;
	presentStarted = 0;
	presentSurface = {};
}

IOReturn RDNA4Compute::restorePresentationLocked(const char *why) {
	if (!presentActive)
		return kIOReturnSuccess;
	const bool restored = Flip::flipTo(*this, presentSurface, presentSurface.desktop,
	                                   why ? why : "restore");
	if (restored)
		RLOG("present: restored desktop (%s)", why ? why : "requested");
	else
		RLOG("present: desktop restore failed (%s)", why ? why : "requested");
	clearPresentationLocked();
	return restored ? kIOReturnSuccess : kIOReturnNotResponding;
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
	RtClient *c = clientFor(owner);
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

IOReturn RDNA4Compute::rtSleepTest(const void *owner, uint32_t phase) {
	uint32_t enabled = 0;
	if (!PE_parse_boot_argn("rdna4-sleeptest", &enabled, sizeof(enabled)) || !enabled)
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

// Contents are undefined, as with any GPU allocation; callers write first.
IOReturn RDNA4Compute::rtAlloc(const void *owner, uint64_t bytes, uint64_t &handle, uint64_t &gpu) {
	Locked g(rtLock);
	checkPresentationTimeoutLocked();
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	RtClient *c = clientFor(owner);
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
		if (c->nextVa < b.va || !vmMap(*c, b.va, b.mc, bytes, false)) {
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
	RtClient *c = clientFor(owner);
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
	RtClient *c = clientFor(owner);
	const bool host = b->host;
	if (c && b->va)
		vmUnmap(*c, b->va, b->bytes);
	if (host && c && !vmInvalidate(c->vmid, "host unmap"))
		RLOG("vmid %u: host buffer unmap invalidation timed out", c->vmid);
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
	RtClient *c = clientFor(owner);
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
	if (RtClient *c = clientFor(owner))
		vmUnmap(*c, p->va, heap.lengthOf(p->offset));
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
	if (rtWedged)
		return kIOReturnNotResponding;
	RtClient *c = clientFor(owner);
	if (vmEnabled && !c)
		return kIOReturnNoResources;
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

	uint64_t ns = 0;
	const bool done = launch(l, "runtime", ns);
	micros = ns / 1000;
	if (!done) {
		if (c)
			c->ibOutstanding = 0;
		if (recoverComputeQueue("runtime", &l)) {
			rtWedged = false;
			RLOG("dispatch timed out after %u ms; queue recovered without a GPU reset", l.timeoutUs / 1000);
		} else {
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
	RtClient *c = clientFor(owner);
	if (!c)
		return kIOReturnNoResources;
	if (rtWedged)
		return kIOReturnNotResponding;
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
	if (c->ibOutstanding >= kMaxIbOutstanding)
		return kIOReturnBusy;

	/* The same VMID-selected shader memory state as launch(): the user IB
	 * supplies the program and resource registers, while this selector only
	 * chains it and fences it. */
	grbmSelect(0, c->pipe, c->queue, c->vmid);
	wr(IpDiscovery::HwGc, ShMemConfig, kShMemConfigDefault);
	wr(IpDiscovery::HwGc, ShMemBases, (0x2000u << 16) | 0x1000u);
	const uint32_t value = nextFence(c->fence);
	uint32_t pkt[8];
	if (!c->pm4.emit(pkt, Pm4::acquireMem(pkt, Pm4::kGcrMemSync)) ||
	    !c->pm4.emit(pkt, Pm4::indirectBufferCompute(pkt, ibVa, static_cast<uint32_t>(dwords), c->vmid)) ||
	    !c->pm4.emit(pkt, Pm4::releaseMem(pkt, c->fenceVa, value,
	                                      ihActive && c->pipe < 2)))
		return kIOReturnNoResources;
	c->fence = value;
	*c->fenceCpu = 0;
	flushHdp();
	pm4Kick(c->pm4, c->doorbell, c->pm4.wptr());
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
	RtClient *c = clientFor(owner);
	if (!c)
		return kIOReturnNoResources;
	if (rtWedged)
		return kIOReturnNotResponding;
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
		rtWedged = true;
		RLOG("IB fence %u timed out after %u ms; queue recovery failed; runtime stays wedged",
		     fence, waitMs);
	}
	return kIOReturnTimeout;
}

IOReturn RDNA4Compute::rtPresent(const void *owner, uint64_t handle, uint64_t offset,
                                 uint64_t &geometry, uint64_t &pitch) {
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
	if (presentActive && presentOwner != owner)
		return kIOReturnBusy;
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
	if (!Flip::flipTo(*this, surface, target, "present"))
		return kIOReturnNotResponding;
	presentActive = true;
	presentOwner = owner;
	presentHandle = handle;
	presentOffset = offset;
	presentStarted = mach_absolute_time();
	presentSurface = surface;
	geometry = (static_cast<uint64_t>(surface.width) & 0xffffu) |
	           ((static_cast<uint64_t>(surface.height) & 0xffffu) << 16);
	pitch = surface.pitch;
	RLOG("present: buffer 0x%llx +0x%llx, %ux%u pitch %u", handle, offset,
	     surface.width, surface.height, surface.pitch);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtRestore(const void *owner) {
	Locked g(rtLock);
	const IOReturn state = ownerStateLocked(owner);
	if (state != kIOReturnSuccess)
		return state;
	checkPresentationTimeoutLocked();
	if (!presentActive)
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
		// the old allocations; never touch a dead HQD from clientClose().
		if (early)
			*early = RtClient {};
		return;
	}
	if (presentActive && presentOwner == owner)
		(void)restorePresentationLocked("client close");
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
		/* Dequeue is deliberately polled: W1's interrupt path is not required. */
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
		vmInvalidate(c->vmid, "client close");
		vmidUsed[c->vmid] = false;
		queueUsed[c->pipe][c->queue] = false;
		IOFree(c->tableShadow, kVmTableBytes);
		devHeap.free(c->tableOffset);
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
	for (RtClient &c : clients) {
		if (!c.active || !c.vmid)
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
	if (devHeapMap) {
		IOFree(devHeapMap, devHeapMapBytes);
		devHeapMap = nullptr;
		devHeapMapBytes = 0;
	}
	heap = GpuHeap::Heap {};
	devHeap = GpuHeap::Heap {};
	hostBytesTotal = 0;
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
