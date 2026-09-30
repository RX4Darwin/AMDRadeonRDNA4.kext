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

// The two shared queues' homes: one per MEC pipe, queue 2 (queue 0 of pipe 0 is the boot queue, (0,1) is the boot self-test's slot). They use the
// pool areas past the client slots (kVmQueueBase + 8 and 9 strides), so no client slot is touched.
bool RDNA4Compute::sharedStart(uint32_t k) {
	SharedQueue &s = sharedQ[k];
	s.pipe = k;
	s.queue = 2;
	s.doorbell = (0x0d + 8 + k) * 2;
	s.area = kVmQueueBase + (8 + k) * kVmQueueStride;
	s.wedged = false;
	if (s.area + 0x7000 > pool.size)
		return false;
	for (uint32_t off = 0; off < 0x7000; off += 4)
		*poolDw(s.area + off) = 0;
	flushHdp();
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
	SLOG("rdna4-vmshared=1: client jobs run on %u shared VMID-0 queues (IB VMID = the client's, static VMIDs 8-15)", kSharedQueues);
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

// The W6 sequence for a shared queue after a job timed out: RESET_WAVES dequeue, SQ_CMD kill of the GUILTY client's VMID only (CHECK_VMID), the
// same HQD registers again (VMID 0, the same ring), a fenced WRITE_DATA proof. Other clients' jobs queued behind the hung one are lost with the
// ring; their waits time out and recover in turn (documented: first version fails innocents rather than re-emitting them).
bool RDNA4Compute::recoverSharedQueue(uint32_t k, uint32_t guiltyVmid, const char *tag) {
	SharedQueue &s = sharedQ[k];
	if (!hangRecoveryEnabled) {
		SLOG("%s: shared queue %u recovery disabled; rdna4-hang=1 is required: the queue stays wedged", tag, k);
		s.wedged = true;
		return false;
	}
	SLOG("%s: recovering shared queue %u (guilty VMID %u) without a GPU reset", tag, k, guiltyVmid);
	logComputeQueueState(tag, s.pipe, s.queue, 0);
	grbmSelect(1, s.pipe, s.queue, 0);
	wr(IpDiscovery::HwGc, CpHqdDequeueReq, 2);   // RESET_WAVES
	wr(IpDiscovery::HwGc, SqCmd, 3u | (1u << 4) | (1u << 7) | ((guiltyVmid & 0xf) << 28));
	bool inactive = false;
	for (uint32_t us = 0; us < 100000 && !inactive; us += 10) {
		inactive = !(rdGc(CpHqdActive) & 1);
		if (!inactive)
			IODelay(10);
	}
	wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
	grbmSelect(0, 0, 0, 0);
	if (!inactive) {
		SLOG("%s: shared queue %u did not dequeue; wedged", tag, k);
		s.wedged = true;
		return false;
	}
	for (uint32_t off = 0; off < 0x7000; off += 4)
		*poolDw(s.area + off) = 0;
	flushHdp();
	const bool up = s.pm.init(poolDw(s.area + kVmPq), poolMc(s.area + kVmPq), kPqSize) &&
	                hqdInitFor(false, s.pipe, s.queue, 0, poolMc(s.area + kVmMqd), poolMc(s.area + kVmEop) >> 8,
	                           poolMc(s.area + kVmPq) >> 8, poolMc(s.area + kVmRptr), poolMc(s.area + kVmWptr), s.doorbell);
	bool proof = false;
	if (up) {
		volatile uint32_t *word = poolDw(s.area + kVmKernarg);
		*word = 0;
		flushHdp();
		uint32_t pkt[8];
		if (s.pm.emit(pkt, Pm4::writeData(pkt, poolMc(s.area + kVmKernarg), 0x600DF00D))) {
			pm4Kick(s.pm, s.doorbell, s.pm.wptr());
			for (uint32_t ms = 0; ms < 200 && *word != 0x600DF00D; ms++)
				IOSleep(1);
			proof = *word == 0x600DF00D;
		}
	}
	s.up = up;
	s.wedged = !proof;
	SLOG("%s: shared queue %u %s", tag, k, proof ? "recovered (WRITE_DATA proof landed)" : "NOT recovered: wedged");
	return proof;
}
