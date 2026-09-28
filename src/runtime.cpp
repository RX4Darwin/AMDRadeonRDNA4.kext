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

bool RDNA4Compute::vmMap(RtClient &c, uint64_t va, uint64_t mc, uint64_t bytes, bool executable) {
	if (!poolCpu || !c.tableShadow || !bytes || (va & (GpuVm::kPageBytes - 1)) ||
	    (mc & (GpuVm::kPageBytes - 1)))
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
	for (uint64_t at = va, pa = mc; at < end; at += GpuVm::kPageBytes, pa += GpuVm::kPageBytes) {
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > kVmTableBytes)
			return false;
		const uint32_t pdeIndex = GpuVm::index(at, 2);
		const uint64_t pdeOff = 0x2000 + static_cast<uint64_t>(pdeIndex) * 8;
		if (!*entry(pdeOff))
			*entry(pdeOff) = GpuVm::encodePde(c.rootMc + ptOff,
			                                GpuVm::kValid | GpuVm::kSnooped, 0);
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		if (firstPt == ~0ull)
			firstPt = ptOff;
		lastPt = ptOff;
		uint64_t flags = GpuVm::kValid | GpuVm::kSnooped | GpuVm::kReadable | GpuVm::kWritable;
		if (executable)
			flags |= GpuVm::kExecutable;
		*entry(pteOff) = GpuVm::encodePte(pa, flags, fragment64k);
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

bool RDNA4Compute::initRuntimeHeap() {
	if (!rtLock || !poolCpu || pool.size <= kHeapOffset)
		return false;
	if (heap.size())
		return true;
	/* DMA first: its self-test drives SDMA directly, before any client can. */
	if (dmaInit())
		devHeapInit();
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
	    !poolCpu || !devHeap.size() || offset & 0xfff || bytes & 0xfff)
		return false;
	memcpy(poolCpu + kVmTableStage, reinterpret_cast<uint8_t *>(c.tableShadow) + offset, bytes);
	(void)*reinterpret_cast<volatile uint32_t *>(poolCpu + kVmTableStage);
	flushHdp();
	uint32_t pkt[Sdma::kCopyDwords];
	if (!Sdma::copyLinear(pkt, poolMc(kVmTableStage), c.rootMc + offset, bytes))
		return false;
	return sdmaRun(pkt, Sdma::kCopyDwords, 2000);
}

bool RDNA4Compute::vmContextInit(RtClient &c) {
	if (c.vmid < 1 || c.vmid > 15 || !c.rootMc)
		return false;
	const uint32_t n = c.vmid - 1;
	const uint32_t faultDefaults = ((1u << 14) - 1) << 10;
	const uint32_t cntl = kVmCtxEnable | (GpuVm::kDepth << 1) |
		((GpuVm::kBlockSize - 9) << 4) | faultDefaults;
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1Cntl.dword + n }, cntl);
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtBaseLo.dword + 2 * n },
	   static_cast<uint32_t>(GpuVm::encodePde(c.rootMc, GpuVm::kValid | GpuVm::kSnooped, 0)));
	wr(IpDiscovery::HwGc, Reg { 0, GcCtx1PtBaseHi.dword + 2 * n },
	   static_cast<uint32_t>(GpuVm::encodePde(c.rootMc, GpuVm::kValid | GpuVm::kSnooped, 0) >> 32));
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
	c.tableShadow = reinterpret_cast<uint64_t *>(IOMalloc(kVmTableBytes));
	if (!c.tableShadow) {
		devHeap.free(table);
		return false;
	}
	bzero(c.tableShadow, kVmTableBytes);
	c.tableShadow[0] =
		GpuVm::encodePde(c.rootMc + 0x1000, GpuVm::kValid | GpuVm::kSnooped, 2);
	c.tableShadow[0x1000 / sizeof(uint64_t) + 4] =
		GpuVm::encodePde(c.rootMc + 0x2000, GpuVm::kValid | GpuVm::kSnooped, 1);
	for (uint32_t off = 0; off < 0x5000; off += 4)
		*poolDw(qoff + off) = 0;
	c.kernargCpu = nullptr;
	c.fenceCpu = poolDw(qoff + kVmFence);
	if (!vmMap(c, qva, poolMc(qoff + kVmPq), kPqSize, false) ||
	    !vmMap(c, rva, poolMc(qoff + kVmRptr), 0x1000, false) ||
	    !vmMap(c, dataVa, poolMc(qoff + kVmWptr), 0x1000, false) ||
	    !vmMap(c, fenceVa, poolMc(qoff + kVmFence), 0x1000, false) ||
	    !c.pm4.init(poolDw(qoff + kVmPq), qva, kPqSize)) {
		IOFree(c.tableShadow, kVmTableBytes);
		devHeap.free(table);
		return false;
	}
	trail("vm: VM context enable");
	bool context = vmContextInit(c);
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
		grbmSelect(0, 0, 0, 0);
	}
	bool flushed = true;
	if (context) {
		trail("vm: invalidate");
		flushed = vmInvalidate(vmid, "boot self-test");
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
	if (!vmEnabled)
		return kIOReturnSuccess;
	Locked g(rtLock);
	if (clientFor(owner))
		return kIOReturnSuccess;
	RtClient *c = nullptr;
	uint32_t slot = 0;
	for (; slot < kMaxClients; slot++)
		if (!clients[slot].active) { c = &clients[slot]; break; }
	if (!c)
		return kIOReturnNoResources;
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
	c->tableShadow = reinterpret_cast<uint64_t *>(IOMalloc(kVmTableBytes));
	if (!c->tableShadow) {
		devHeap.free(table); *c = RtClient {};
		return kIOReturnNoMemory;
	}
	bzero(c->tableShadow, kVmTableBytes);
	c->tableShadow[0] =
		GpuVm::encodePde(c->rootMc + 0x1000, GpuVm::kValid | GpuVm::kSnooped, 2);
	c->tableShadow[0x1000 / sizeof(uint64_t) + 4] =
		GpuVm::encodePde(c->rootMc + 0x2000, GpuVm::kValid | GpuVm::kSnooped, 1);
	const uint32_t qoff = kVmQueueBase + slot * kVmQueueStride;
	for (uint32_t off = 0; off < 0x5000; off += 4)
		*poolDw(qoff + off) = 0;
	c->kernargCpu = poolDw(qoff + kVmKernarg);
	c->fenceCpu = poolDw(qoff + kVmFence);
	const uint64_t qva = c->nextVa; c->nextVa += 0x1000;
	const uint64_t eva = c->nextVa; c->nextVa += 0x1000;
	const uint64_t rva = c->nextVa; c->nextVa += 0x1000;
	const uint64_t wva = c->nextVa; c->nextVa += 0x1000;
	c->fenceVa = c->nextVa; c->nextVa += 0x1000;
	c->kernargVa = c->nextVa; c->nextVa += 0x1000;
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
	RLOG("vmid %u: client queue activated MEC1 pipe %u queue %u, PDB2 MC 0x%llx, doorbell dword %u",
	     vmid, pipe, queue, c->rootMc, c->doorbell);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtInfo(const void *owner, uint64_t out[9]) {
	Locked g(rtLock);
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
	if (c)
		out[2] |= RDNA4_FLAG_VM;
	return kIOReturnSuccess;
}

// Contents are undefined, as with any GPU allocation; callers write first.
IOReturn RDNA4Compute::rtAlloc(const void *owner, uint64_t bytes, uint64_t &handle, uint64_t &gpu) {
	Locked g(rtLock);
	if (!rtReady)
		return kIOReturnNotReady;
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
	b = { owner, off, bytes, dev ? vramMc(off) : poolMc(off), 0, nextGen(b.gen), dev };
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

IOReturn RDNA4Compute::rtFree(const void *owner, uint64_t handle) {
	Locked g(rtLock);
	RtBuffer *b = bufferFor(owner, handle);
	if (!b)
		return kIOReturnBadArgument;
	if (RtClient *c = clientFor(owner))
		vmUnmap(*c, b->va, b->bytes);
	(b->device ? devHeap : heap).free(b->offset);
	b->owner = nullptr;
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtCopy(const void *owner, uint64_t handle, uint64_t offset, task_t task,
                              mach_vm_address_t user, uint64_t length, bool toGpu) {
	Locked g(rtLock);
	if (!rtReady)
		return kIOReturnNotReady;
	RtBuffer *b = bufferFor(owner, handle);
	if (!b || offset > b->bytes || length > b->bytes - offset)
		return kIOReturnBadArgument;
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
	if (!rtReady)
		return kIOReturnNotReady;
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
	RLOG("loaded \"%s\": %llu-byte image at %s 0x%llx, entry +0x%llx, %u bytes of kernargs, "
	     "%u of LDS", name, img.size, c ? "VA" : "MC", c ? p.va : poolMc(off), k.entryVa,
	     k.kernargSize, k.groupSegmentSize);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtUnload(const void *owner, uint64_t program) {
	Locked g(rtLock);
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
	if (!rtReady)
		return kIOReturnNotReady;
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
	l.queue = c ? &c->pm4 : nullptr;
	l.vmid = c ? c->vmid : 0;
	l.pipe = c ? c->pipe : 0;
	l.queueId = c ? c->queue : 0;
	l.fenceValue = c ? ++c->fence : 0;
	l.doorbell = c ? c->doorbell : 0;
	l.fenceAddress = c ? c->fenceVa : 0;
	l.fenceCpu = c ? c->fenceCpu : nullptr;

	uint64_t ns = 0;
	const bool done = launch(l, "runtime", ns);
	micros = ns / 1000;
	if (!done) {
		rtWedged = true;
		RLOG("dispatch timed out after %u ms: the queue is considered hung; no more dispatches "
		     "until reboot", l.timeoutUs / 1000);
		return kIOReturnTimeout;
	}
	if (c)
		logClientFault(*c, "dispatch");
	return kIOReturnSuccess;
}

void RDNA4Compute::rtRelease(const void *owner) {
	if (!rtLock || !owner)
		return;
	Locked g(rtLock);
	RtClient *c = clientFor(owner);
	uint32_t nb = 0, np = 0;
	for (RtBuffer &b : buffers) {
		if (b.owner == owner) {
			if (c)
				vmUnmap(*c, b.va, b.bytes);
			(b.device ? devHeap : heap).free(b.offset);
			b.owner = nullptr;
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
	if (nb || np)
		RLOG("client closed: freed %u buffer(s), %u program(s)", nb, np);
	if (c) {
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
	}
}

// ---------------------------------------------------------------------------
// DMA: SDMA between VRAM and a pinned bounce buffer in host memory
// ---------------------------------------------------------------------------

// One packet on SDMA0 queue 0 with a FENCE after it; waits for the fence.
bool RDNA4Compute::sdmaRun(const uint32_t *pkt, uint32_t dwords, uint32_t timeoutMs) {
	uint32_t fence[Sdma::kFenceDwords];
	const uint32_t value = ++sdmaFence;
	Sdma::fence(fence, poolMc(kSdmaFenceOffset), value);
	if (!sdmaRing.emit(pkt, dwords) || !sdmaRing.emit(fence, Sdma::kFenceDwords))
		return false;
	sdmaKick(sdmaRing.wptr());
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
