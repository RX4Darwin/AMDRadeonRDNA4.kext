//
//  vmshared.cpp
//  RDNA4FB
//
//  W13 S7-lite (docs/w13-vmid.md, sections 4-5): with the boot-arg rdna4-vmshared=1 a client owns no MEC queue. Two kernel-owned queues, one
//  per MEC pipe, run every client job. They are configured as amdgpu's kernel compute rings are (gfx_v12_0_compute_mqd_init): CP_HQD_VMID 0,
//  CP_MQD_CONTROL.VMID 0, PRIV_STATE | KMD_QUEUE, every address an MC address in the compute pool. A job is
//  INDIRECT_BUFFER(vmid = the client's VMID, VA of the IB in the client's VM) followed by a ring-level RELEASE_MEM to the client's fence word,
//  the VMID coming from the IB packet (gfx_v12_0_ring_emit_ib_compute). Assumption U1 of the design: proven on this card for amdgpu's own compute
//  ring by tools/linux-replay/replay-compute.cpp (2026-10-01); the kext's own queues like this are what this mode tests.
//
//  Why: on the real card every client HQD with a non-zero VMID is never serviced (docs/w13-vmid.md s.1.1, docs/vm-client-rootcause.md), and
//  amdgpu has no gfx12 precedent for MMIO-loaded HQDs with a non-zero VMID (KFD needs MES). VMID-0 kernel queues + the IB's VMID is the model
//  amdgpu uses without MES.
//
//  Clients keep a static VMID 8-15 taken at open (the VmidPool of vmid.hpp can replace that later). A client is pinned to one shared queue so its
//  fences stay in order. Default off; the per-client-HQD path is untouched when off.
//
#include "compute.hpp"

#include <IOKit/IOLib.h>

#define SLOG(fmt, ...) IOLog("RDNA4FB: vmshared: " fmt "\n", ## __VA_ARGS__)

using namespace GfxReg;

static bool fenceAtLeast(uint32_t current, uint32_t wanted) {
	return static_cast<int32_t>(current - wanted) >= 0;
}

// The pool asks whether a fence domain (a shared queue) has reached a sequence number.
bool RDNA4Compute::poolFenceReached(void *context, uint32_t domain, uint32_t seq) {
	RDNA4Compute *self = static_cast<RDNA4Compute *>(context);
	if (domain == kGfxDomain)
		return !self->poolCpu || fenceAtLeast(*self->poolDw(kGfxFenceOffset), seq);   // the gfx ring's own fence dword (in order)
	return self->sharedFenceReached(domain, seq);
}

// The two shared queues' homes: one per MEC pipe, queue 2 (queue 0 of pipe 0 is the boot queue, (0,1) is the boot self-test's slot). They use the
// pool areas past the client slots (kVmQueueBase + 8 and 9 strides), so no client slot is touched.
bool RDNA4Compute::sharedStart(uint32_t k) {
	SharedQueue &s = sharedQ[k];
	s.pipe = k;
	s.queue = 2;
	/* Its own doorbell dwords: client slots use (0x0d + slot) * 2 (26-40) and the S1 probe queues (0x0d + 8 + area) * 2 (42 and up). Sharing 42 and 44
	 * with the probe queues at (0,1)/(1,1) made the emulator send the shared queues' doorbells to a dequeued probe HQD (hub-task-347); on the card it
	 * would make the routing depend on a dead queue's register state. */
	s.doorbell = (0x0d + 24 + k) * 2;
	s.area = kVmQueueBase + (8 + k) * kVmQueueStride;
	s.wedged = false;
	if (s.area + 0x7000 > pool.size)
		return false;
	for (uint32_t off = 0; off < 0x7000; off += 4)
		*poolDw(s.area + off) = 0;
	flushHdp();
	s.fenceCpu = poolDw(s.area + kVmFence);      // the queue's own fence word (vmshared=2), zero with the area
	s.fenceMc = poolMc(s.area + kVmFence);
	s.seq = 0;
	s.jobHead = s.jobCount = s.ringUsed = 0;
	if (!s.pm.init(poolDw(s.area + kVmPq), poolMc(s.area + kVmPq), kPqSize))
		return false;
	s.up = hqdInitFor(false, s.pipe, s.queue, 0, poolMc(s.area + kVmMqd), poolMc(s.area + kVmEop) >> 8,
	                  poolMc(s.area + kVmPq) >> 8, poolMc(s.area + kVmRptr), poolMc(s.area + kVmWptr), s.doorbell);
	queueUsed[s.pipe][s.queue] = s.up;
	SLOG("shared queue %u: MEC1 pipe %u queue %u, VMID 0, doorbell dword %u: %s", k, s.pipe, s.queue, s.doorbell,
	     s.up ? "active" : "FAILED to activate");
	return s.up;
}

// Called under rtLock when the first shared client opens (and again after a resume): SH_MEM_CONFIG/BASES for every VMID, once, like
// gfx_v12_0_constants_init, then both queues.
bool RDNA4Compute::vmSharedEnsure() {
	if (sharedInit)
		return true;
	if (!poolCpu || !gcEnsureAwake(0xfffffff0u))
		return false;
	for (uint32_t v = 1; v <= 15; v++) {
		grbmSelect(0, 0, 0, v);
		wr(IpDiscovery::HwGc, ShMemConfig, kShMemConfigDefault);
		wr(IpDiscovery::HwGc, ShMemBases, kShMemBasesDefault);
	}
	grbmSelect(0, 0, 0, 0);
	bool ok = true;
	for (uint32_t k = 0; k < kSharedQueues; k++)
		ok = sharedStart(k) && ok;
	if (!ok) {
		sharedStopAll("start failed");
		return false;
	}
	sharedInit = true;
	if (vmShared == 2) {
		vmPool.init(poolFenceReached, this, 0);
		SLOG("rdna4-vmshared=2: client jobs run on %u shared VMID-0 queues, VMIDs 1-15 are bound per job from the pool", kSharedQueues);
	} else {
		SLOG("rdna4-vmshared=1: client jobs run on %u shared VMID-0 queues (IB VMID = the client's, static VMIDs 8-15)", kSharedQueues);
	}
	return true;
}

void RDNA4Compute::sharedStopAll(const char *why) {
	for (uint32_t k = 0; k < kSharedQueues; k++) {
		SharedQueue &s = sharedQ[k];
		if (!s.up)
			continue;
		grbmSelect(1, s.pipe, s.queue, 0);
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
		bool idle = false;
		for (uint32_t us = 0; us < 100000 && !idle; us += 10) {
			idle = !(rdGc(CpHqdActive) & 1);
			if (!idle)
				IODelay(10);
		}
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
		if (!idle)
			SLOG("shared queue %u (%s): dequeue timeout (ACTIVE 0x%08x)", k, why, rdGc(CpHqdActive));
		grbmSelect(0, 0, 0, 0);
		s.up = false;
		queueUsed[s.pipe][s.queue] = false;
	}
	sharedInit = false;
}

// A shared queue after a job that did not finish, as amdgpu resets a compute queue over MMIO on this chip (mes_v12_0_reset_queue_mmio, its
// AMDGPU_RING_TYPE_COMPUTE branch, which needs no firmware scheduler): inside RLC safe mode, the queue selected, CP_HQD_DEQUEUE_REQUEST = 2
// and SPI_COMPUTE_QUEUE_RESET = 1, then wait for the queue to go inactive. Then the same HQD registers again (VMID 0, the same ring) and a
// fenced WRITE_DATA proof. Until 2026-10-07 this killed the guilty VMID's waves with SQ_CMD instead of the SPI reset; on the card the queue
// then dequeued and came up again, and its first packet never ran (docs/vm-client-rootcause.md sections 12 to 16). Other clients' jobs queued
// behind the dead one are lost with the ring; their waits time out and recover in turn.
bool RDNA4Compute::recoverSharedQueue(uint32_t k, uint32_t guiltyVmid, const char *tag) {
	SharedQueue &s = sharedQ[k];
	if (!hangRecoveryEnabled) {
		SLOG("%s: shared queue %u recovery disabled; rdna4-hang=1 is required: the queue stays wedged", tag, k);
		s.wedged = true;
		return false;
	}
	SLOG("%s: recovering shared queue %u (guilty VMID %u) without a GPU reset", tag, k, guiltyVmid);
	logComputeQueueState(tag, s.pipe, s.queue, 0);
	// One packet through the queue as it stands: true if it ran.
	auto proves = [&]() {
		volatile uint32_t *word = poolDw(s.area + kVmKernarg);
		*word = 0;
		flushHdp();
		uint32_t pkt[8];
		if (!s.pm.emit(pkt, Pm4::writeData(pkt, poolMc(s.area + kVmKernarg), 0x600DF00D)))
			return false;
		pm4Kick(s.pm, s.doorbell, s.pm.wptr());
		for (uint32_t ms = 0; ms < 200 && *word != 0x600DF00D; ms++)
			IOSleep(1);
		return *word == 0x600DF00D;
	};
	/* Only the hung waves are reset; the queue is left as it is. When recovery starts the queue itself is in order (read pointer at the
	 * write pointer, doorbell enabled); what hangs is the faulted shader's waves. VERIFIED on the card 2026-10-07 (eighth run): the proof
	 * lands within 1.1 ms on both queues and every later job runs. amdgpu's own reset (mes_v12_0_reset_queue_mmio: this write with a
	 * dequeue request, then the queue made again) was tried in the sixth and seventh run and is not here: without an MES to map the
	 * queue afterwards the remade queue never ran (docs/vm-client-rootcause.md sections 17 to 19).
	 * ponytail: a hang that is not the waves' (a command buffer the CP cannot fetch) is not recovered, the queue stays out of service
	 * and new clients go to the other one; that needs the MES or a GPU reset. */
	const bool safe = rlcSafeMode(true);
	grbmSelect(1, s.pipe, s.queue, 0);
	wr(IpDiscovery::HwGc, SpiComputeQueueReset, 1);
	grbmSelect(0, 0, 0, 0);
	rlcSafeMode(false);
	const bool proof = proves();
	SLOG("%s: shared queue %u: waves reset, the queue left as it was (RLC safe mode %s): %s", tag, k, safe ? "acknowledged" : "NOT acknowledged",
	     proof ? "it runs" : "it does not run");
	if (!proof)
		logComputeQueueState(tag, s.pipe, s.queue, 0);
	s.wedged = !proof;
	/* The proof ran behind everything that was in the ring, so the jobs that were in it are over. Their queue-fence numbers count as
	 * reached so the VMIDs they held are idle again (a job that did not run to completion never signals its client's own fence, and
	 * that wait times out), and the ring is empty. */
	*s.fenceCpu = s.seq;
	s.jobHead = s.jobCount = s.ringUsed = 0;
	flushHdp();
	SLOG("%s: shared queue %u %s", tag, k, proof ? "recovered (WRITE_DATA proof landed)" : "NOT recovered: wedged");
	return proof;
}

/* ---- rdna4-vmshared=2: VMIDs from the pool ---------------------------------------------------------------------- */

bool RDNA4Compute::sharedFenceReached(uint32_t domain, uint32_t seq) {
	if (domain >= kSharedQueues || !sharedQ[domain].fenceCpu)
		return true;
	return fenceAtLeast(*sharedQ[domain].fenceCpu, seq);
}

// Room in the 4 KiB ring of shared queue k for a job of about `dwords`: retire the jobs whose queue fence was reached, then wait (bounded) for
// more if the ring is too full. The ring carries the jobs of every client on the queue, so the producer must not run past the consumer.
bool RDNA4Compute::sharedReserve(uint32_t k, uint32_t dwords) {
	SharedQueue &s = sharedQ[k];
	const uint32_t capacity = s.pm.sizeDwords() - 16;
	for (uint32_t ms = 0; ms < 2000; ms++) {
		while (s.jobCount && fenceAtLeast(*s.fenceCpu, s.jobs[s.jobHead].seq)) {
			s.ringUsed -= s.jobs[s.jobHead].dwords;
			s.jobHead = (s.jobHead + 1) % 64;
			s.jobCount--;
		}
		if (s.ringUsed + dwords <= capacity && s.jobCount < 64)
			return true;
		IOSleep(1);
	}
	SLOG("shared queue %u: no ring space for %u dwords after 2 s (%u dwords in flight, %u jobs)", k, dwords, s.ringUsed, s.jobCount);
	return false;
}

void RDNA4Compute::sharedCommit(uint32_t k, uint32_t seq, uint32_t dwords) {
	SharedQueue &s = sharedQ[k];
	if (s.jobCount >= 64)
		return;
	const uint32_t tail = (s.jobHead + s.jobCount) % 64;
	s.jobs[tail] = SharedQueue::Job { seq, dwords };
	s.jobCount++;
	s.ringUsed += dwords;
}

// The VMID for the client's next job. The pool decides (reuse the VMID the client still owns, else the least recently used IDLE one, never one with
// work in flight, else wait); this binds it: if it is new to the client, write its page directory into the VMID's context over MMIO, set the IH
// LUT so a fault names the client, and invalidate; if only a flush is owed (the client removed PTEs), invalidate. Safe because the pool only hands
// out a VMID none of whose jobs is still queued or running (B-V1 of docs/w13-vmid.md).
uint32_t RDNA4Compute::vmAcquire(RtClient &c) {
	Vmid::Grant g;
	Vmid::Result r = Vmid::Result::Busy;
	for (uint32_t ms = 0; ms < 5000; ms++) {
		r = vmPool.grab(reinterpret_cast<uintptr_t>(&c), c.rootPhys, c.tlbSeq, g);
		if (r != Vmid::Result::Busy)
			break;
		IOSleep(1);          // every VMID has a job in flight: the pool named the fence; jobs finish without this lock
	}
	if (r != Vmid::Result::Ok) {
		SLOG("no VMID available for client slot %d (%s)", static_cast<int>(&c - clients), r == Vmid::Result::Busy ? "all 15 busy for 5 s" : "none usable");
		return 0;
	}
	if (g.rebind) {
		/* A fault latched while the VMID was the previous owner's is the previous owner's: say so, and clear it before the VMID is re-targeted. */
		const uint32_t st = rdGc(GcL2FaultStatusLo);
		if (st && ((st >> 20) & 0xf) == g.vmid) {
			const RtClient *prev = reinterpret_cast<const RtClient *>(g.prevOwner);
			SLOG("VMID %u: fault status 0x%08x latched before its rebind belongs to its previous owner (client slot %d), cleared", g.vmid, st,
			     prev >= clients && prev < clients + kClientSlots ? static_cast<int>(prev - clients) : -1);
			gcFaultClear();
		}
		c.vmid = g.vmid;
		if (!vmContextInit(c)) {
			SLOG("VMID %u: context programming for client slot %d failed", g.vmid, static_cast<int>(&c - clients));
			uint32_t dropped = 0;
			vmPool.forget(reinterpret_cast<uintptr_t>(&c), true, dropped);
			return 0;
		}
		wr(IpDiscovery::HwOsssys, Reg { 0, g.vmid }, c.pasid);      // IH_VMID_n_LUT = PASID: a fault vector from this VMID names the client
		(void)vmInvalidate(g.vmid, "vmpool rebind");
		if (g.stolen)
			SLOG("VMID %u: rebound from client slot %d to slot %d (PASID %u)", g.vmid,
			     reinterpret_cast<const RtClient *>(g.prevOwner) >= clients ? static_cast<int>(reinterpret_cast<const RtClient *>(g.prevOwner) - clients) : -1,
			     static_cast<int>(&c - clients), c.pasid);
	} else {
		c.vmid = g.vmid;
		if (g.flush)
			(void)vmInvalidate(g.vmid, "vmpool flush");
	}
	return g.vmid;
}

// The client closes: every VMID it owns goes back to the pool with its context switched off, its IH LUT entry cleared and its TLB flushed.
void RDNA4Compute::vmReleaseVmids(RtClient &c) {
	const uintptr_t owner = reinterpret_cast<uintptr_t>(&c);
	for (uint32_t v = Vmid::kFirst; v <= Vmid::kLast; v++) {
		if (vmPool.ownerOf(v) != owner)
			continue;
		wr(IpDiscovery::HwGc, Reg { 0, GcCtx1Cntl.dword + v - 1 }, 0);
		wr(IpDiscovery::HwOsssys, Reg { 0, v }, 0);
		(void)vmInvalidate(v, "vmpool release");
	}
	uint32_t first = 0;
	vmPool.forget(owner, true, first);
	c.vmid = 0;
}

int RDNA4Compute::vmClientSlotByPasid(uint32_t pasid) const {
	if (!pasid || vmShared != 2)
		return -1;
	for (uint32_t i = 0; i < kClientSlots; i++)
		if (clients[i].active && clients[i].pasid == pasid)
			return static_cast<int>(i);
	return -1;
}

/* ---- the boot proof in shared mode ------------------------------------------------------------------------------ */

// What vmBootSelfTest proves, by the route clients take in shared mode and without ever building a queue inside a client address space: a
// client like any other (its tables, its address-space context, a place on a shared VMID-0 queue), and two command buffers in its address
// space that each write two words through its tables, each followed by the client's fence. Runs on the bring-up thread before the runtime is
// published, so it opens the client's slot itself. A job that does not finish gets the shared queue's own recovery (rdna4-hang=1), and the
// address spaces stay off for the boot.
//
// Why not the old test in this mode: on the card every boot that built a queue in a non-zero address space, the old boot test included, left
// the GPU at full clock before any client existed, and no client queue was ever serviced afterwards (docs/vm-client-rootcause.md, S-A to S-C).
// Boots that never built one (2026-10-06, docs/vulkan-port.md) ran work in address space 8 from the graphics ring and the copy engine.
bool RDNA4Compute::vmSharedBootTest() {
	static char token;
	const void *owner = &token;
	if (!initRuntimeHeap()) {
		publishResult("vm", "FAIL runtime heap");
		return false;
	}
	char result[176];
	snprintf(result, sizeof(result), "FAIL shared-queue client setup");
	bool pass = false;
	IOLockLock(rtLock);
	uint32_t slot = 0;
	while (slot < clientCap() && clients[slot].active)
		slot++;
	RtClient *c = slot < clientCap() ? &clients[slot] : nullptr;
	const IOReturn opened = !c ? kIOReturnNoResources
	                           : vmShared == 2 ? rtOpenPooled(owner, slot, c) : rtOpenShared(owner, slot, c);
	if (opened != kIOReturnSuccess) {
		SLOG("boot test: no client (0x%x)", opened);
	} else if (c->ibVa && c->kernargCpu && c->fenceCpu) {
		volatile uint32_t *ib = poolDw(c->poolOff + kVmIb);
		const uint32_t first[2] = { 0x600DF00Du, 0x57EE1E57u }, second[2] = { 0x0DDBA11Du, 0x5EC0FFEEu };
		uint64_t us[2] = { 0, 0 };
		bool done[2] = { false, false }, data[2] = { false, false };
		IOReturn sent[2] = { kIOReturnNotReady, kIOReturnNotReady };
		uint32_t vmid = 0;
		for (uint32_t job = 0; job < 2 && (job == 0 || (done[0] && data[0])); job++) {
			const uint32_t *values = job ? second : first;
			uint32_t pkt[16], n = 0;
			n += Pm4::writeData(pkt + n, c->kernargVa, values[0]);
			n += Pm4::writeData(pkt + n, c->kernargVa + 4, values[1]);
			for (uint32_t i = 0; i < n; i++)
				ib[i] = pkt[i];
			c->kernargCpu[0] = c->kernargCpu[1] = 0;
			flushHdp();
			uint64_t fence = 0;
			sent[job] = submitIbLocked(*c, c->ibVa, n, fence);
			vmid = c->vmid;                      // mode 2 binds one at the submit
			if (sent[job] != kIOReturnSuccess)
				break;
			const uint64_t t0 = mach_absolute_time();
			for (uint32_t ms = 0; ms < 2000 && !(done[job] = fenceAtLeast(*c->fenceCpu, static_cast<uint32_t>(fence))); ms++)
				IOSleep(1);
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &us[job]);
			us[job] /= 1000;
			data[job] = c->kernargCpu[0] == values[0] && c->kernargCpu[1] == values[1];
			if (!done[job]) {
				logComputeQueueState("vm shared boot test", sharedQ[c->sq].pipe, sharedQ[c->sq].queue, 0);
				c->ibOutstanding = 0;
				(void)recoverSharedQueue(c->sq, c->vmid, "boot test");
			}
		}
		retireIbFences(*c);
		logClientFault(*c, "vm shared boot test");
		pass = done[0] && data[0] && done[1] && data[1];
		SLOG("boot test: address space %u from shared queue %u (MEC1 pipe %u queue %u, VMID 0): job 1 submit 0x%x fence %s data %s (%llu us), "
		     "job 2 submit 0x%x fence %s data %s (%llu us): %s", vmid, c->sq, sharedQ[c->sq].pipe, sharedQ[c->sq].queue, sent[0],
		     done[0] ? "reached" : "NOT reached", data[0] ? "ok" : "WRONG", us[0], sent[1], done[1] ? "reached" : "NOT reached",
		     data[1] ? "ok" : "WRONG", us[1], pass ? "PASS" : "FAIL");
		snprintf(result, sizeof(result), "%s shared queue: two command buffers in address space %u from a VMID-0 kernel queue, "
		         "written through the client's tables (%llu / %llu us)", pass ? "PASS" : "FAIL", vmid, us[0], us[1]);
	}
	IOLockUnlock(rtLock);
	if (opened == kIOReturnSuccess)
		rtRelease(owner);
	publishResult("vm", result);
	return pass;
}
