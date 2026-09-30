//
//  vmtest.cpp
//  RDNA4FB
//
//  W13 step S1 (docs/w13-vmid.md section 8.1): the diagnostic boot self-test, rdna4-vmid-test=1 (default
//  off). It runs once, after vmBootSelfTest and before any client can open, and is written so that its log
//  discriminates between the explanations of the real card's client-path failure (round 6: every client
//  dispatch times out, every client close logs "dequeue timeout (ACTIVE 0x1)", GFX activity reads 100 %
//  at idle) and answers the two assumptions design B rests on:
//    U1  an IB packet's VMID is honoured on a VMID-0 MEC queue (amdgpu's kernel compute rings rely on it);
//    U2  VMIDs 1 and 15 work like 8.
//
//  Each probe touches a slot of its own where it can, so a wedged slot cannot spoil the next one (round 3
//  showed a slot is not serviced again after its first VM fault). Probes that cannot start a shader wave run
//  first; the first probe that does and hangs stops everything else. Every probe logs one line; a failure adds
//  the queue-state dump. The summary goes to RDNA4FB,Results "vmidtest" and the log.
//
//    T0  engine survey (which HQDs are active at idle) and SH_MEM_CONFIG/BASES for VMIDs 1-15
//    T1  slot reuse, no VM involved: a VMID-0 queue on the boot self-test's own slot (0,1), twice
//    T2  the same on a slot nothing has used (1,1)
//    T3  U1/U2: a VMID-0 queue (1,3) fetching an IB in VMID 8, 1, 15 (WRITE_DATA to a page only that VMID maps)
//    T5  the vadd dispatch as an IB in VMID 8 from that queue: the first shader wave in a non-zero VMID
//    T4  a client-style queue: HQD VMID 8 on slot (1,2), ring/EOP/rptr in the client VM; WRITE_DATA; dequeue;
//        reactivate the same slot (what rtOpen/rtRelease/rtOpen do)
//    T5b the same dispatch as direct ring packets on that VMID-8 queue with the per-launch SH_MEM rewrite
//        (what rtDispatch does today)
//    T5c the incremental stream on one fresh HQD-VMID-8 queue (Anvil's H2 bisect): WRITE_DATA, +ACQUIRE_MEM, +SET_SH_REGs,
//        +DISPATCH of an s_endpgm-only kernel (no memory access), +vadd, +RELEASE_MEM, with rptr/HIT/CPC/fault logged after
//        each step; the first step that stalls is named, then sibling queues are probed (same pipe, other pipe)
//    T4c the client-style queue with HQD VMID 3 (amdgpu's gfx range) instead of 8 (KFD's)
//    T6  the dispatch with its code page mapped without EXECUTABLE: must fault, VMID 8, execute permission
//    T7  negative control: the IB packet names a VMID whose context is off: must fault with that VMID
//
#include "compute.hpp"
#include "codeobj.hpp"
#include "vadd_codeobj.h"
#include "../userspace/pm4build.h"

#include <IOKit/IOLib.h>

#define VLOG(fmt, ...) IOLog("RDNA4FB: vmidtest: " fmt "\n", ## __VA_ARGS__)

using namespace GfxReg;

// rdna4-vmid-test is a mask: 1 = the probes (vmIdTest), 2 = engine/HQD surveys at the points of the normal flow (vmIdSurvey),
// 4 = a bounded trace of the client operations (vmOpTrace). The boot plan uses 7.
uint32_t RDNA4Compute::vmIdTestMask() {
	uint32_t v = 0;
	return PE_parse_boot_argn("rdna4-vmid-test", &v, sizeof(v)) ? v & 7 : 0;
}

bool RDNA4Compute::requestedVmIdTest() {
	return (vmIdTestMask() & 1) != 0;
}

// What is busy and what every MEC HQD holds, at one point of the flow. Read-only. Anvil's H1 needs it at: before
// vmBootSelfTest, right after it, after the flip test, after clock gating, after the first client opens.
void RDNA4Compute::vmIdSurvey(const char *tag) {
	grbmSelect(0, 0, 0, 0);
	VLOG("survey %s: GRBM 0x%08x/0x%08x CPC 0x%08x/0x%08x CPF 0x%08x/0x%08x CP_STAT 0x%08x RLC_GPM 0x%08x RLC_CP_SCHED 0x%08x "
	     "MEC_RS64 0x%08x PQ_STATUS 0x%08x DB_RANGE 0x%08x..0x%08x", tag, rdGc(GrbmStatus), rdGc(GrbmStatus2),
	     rdGc(CpCpcStatus), rdGc(CpCpcBusyStat), rdGc(CpCpfStatus), rdGc(CpCpfBusyStat), rdGc(CpStat), rdGc(RlcGpmStat),
	     rdGc(RlcCpSchedulers), rdGc(CpMecRs64Cntl), rdGc(CpPqStatus), rdGc(CpMecDoorbellLower), rdGc(CpMecDoorbellUpper));
	char idle[64];
	size_t n = 0;
	idle[0] = 0;
	for (uint32_t pipe = 0; pipe < 2; pipe++) {
		for (uint32_t q = 0; q < 4; q++) {
			grbmSelect(1, pipe, q, 0);
			const uint32_t act = rdGc(CpHqdActive), vm = rdGc(CpHqdVmid), rp = rdGc(CpHqdPqRptr), wl = rdGc(CpHqdPqWptrLo),
			               wh = rdGc(CpHqdPqWptrHi), db = rdGc(CpHqdPqDoorbell), pc = rdGc(CpHqdPqControl),
			               hq = rdGc(CpHqdHqStatus0), er = rdGc(CpHqdEopRptr);
			if ((act & 1) || rp || wl || wh || (db & 0x80000000u) || hq)
				VLOG("survey %s: HQD %u/%u ACTIVE %u VMID %u rptr %u wptr 0x%x:%08x doorbell 0x%08x%s PQ_CONTROL 0x%08x (bit 15 = PQ_EMPTY "
				     "status) HQ_STATUS0 0x%08x EOP_RPTR 0x%08x", tag, pipe, q, act & 1, vm & 0xf, rp, wh, wl, db,
				     (db >> 31) ? " HIT" : "", pc, hq, er);
			else if (n + 6 < sizeof(idle))
				n += snprintf(idle + n, sizeof(idle) - n, " %u/%u", pipe, q);
		}
	}
	grbmSelect(0, 0, 0, 0);
	VLOG("survey %s: HQDs with no state at all:%s", tag, n ? idle : " none");
}

// A bounded trace of one client operation: the runtime's own view (wedged, client slot) and the HQD's (active, rptr/wptr,
// doorbell HIT) plus the latched fault. At most 64 lines per boot.
void RDNA4Compute::vmOpTrace(const char *op, uint32_t vmid, uint32_t pipe, uint32_t queue) {
	if (!vmOpTraceOn || vmOpTraceLines >= 64)
		return;
	vmOpTraceLines++;
	grbmSelect(1, pipe, queue, vmid);
	const uint32_t act = rdGc(CpHqdActive), rp = rdGc(CpHqdPqRptr), wl = rdGc(CpHqdPqWptrLo), db = rdGc(CpHqdPqDoorbell);
	grbmSelect(0, 0, 0, 0);
	VLOG("op %s: client VMID %u HQD %u/%u wedged %d: ACTIVE %u rptr %u wptr %u doorbell 0x%08x%s CPC_BUSY 0x%08x fault 0x%08x", op,
	     vmid, pipe, queue, rtWedged, act & 1, rp, wl, db, (db >> 31) ? " HIT" : "", rdGc(CpCpcBusyStat),
	     rdGc(GcL2FaultStatusLo));
}

namespace {

constexpr uint32_t kItems = 256;
constexpr uint32_t kNop = 0xffff1000u;       // the one-dword PKT3 NOP

// The vadd IB as userspace/rdna4-run.c and tools/linux-replay/replay-compute.cpp record it (the latter ran on
// the card under amdgpu's compute ring on 2026-09-30/10-01 and gave the right data).
uint32_t recordVaddIb(uint32_t *ib, uint64_t codeVa, const CodeObj::Kernel &k, uint64_t kernarg, uint32_t groups) {
	uint32_t n = 0;
	const uint32_t zero = 0, all[2] = { 0xffffffffu, 0xffffffffu };
	const uint32_t none4[4] = { 0, 0, 0, 0 }, start[3] = { 0, 0, 0 };
	const uint32_t threads[3] = { 64, 1, 1 };
	const uint32_t pgm[2] = { static_cast<uint32_t>(codeVa >> 8), static_cast<uint32_t>(codeVa >> 40) };
	const uint32_t rsrc[2] = { k.rsrc1, k.rsrc2 };
	const uint32_t rsrc3 = k.rsrc3;
	const uint32_t user[2] = { static_cast<uint32_t>(kernarg), static_cast<uint32_t>(kernarg >> 32) };
	n += rdna4_pm4_acquire_mem(ib + n, RDNA4_PM4_GCR_MEM_SYNC);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_LO, pgm, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC1, rsrc, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC3, &rsrc3, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_RESOURCE_LIM, &zero, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_TMPRING, &zero, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE0, all, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE2, all, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE4, none4, 4);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_START_X, start, 3);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_NUM_THREAD_X, threads, 3);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_USER_DATA0, user, 2);
	n += rdna4_pm4_dispatch_direct(ib + n, groups, 1, 1,
	                               RDNA4_PM4_DISPATCH_SHADER_EN | RDNA4_PM4_DISPATCH_FORCE_START0 |
	                               (k.wave32() ? RDNA4_PM4_DISPATCH_WAVE32 : 0));
	return n;
}

// The same IB in two parts, for the incremental bisect: the SET_SH_REGs, and the dispatch.
uint32_t recordVaddSetup(uint32_t *ib, uint64_t codeVa, const CodeObj::Kernel &k, uint64_t kernarg) {
	uint32_t n = 0;
	const uint32_t zero = 0, all[2] = { 0xffffffffu, 0xffffffffu };
	const uint32_t none4[4] = { 0, 0, 0, 0 }, start[3] = { 0, 0, 0 };
	const uint32_t threads[3] = { 64, 1, 1 };
	const uint32_t pgm[2] = { static_cast<uint32_t>(codeVa >> 8), static_cast<uint32_t>(codeVa >> 40) };
	const uint32_t rsrc[2] = { k.rsrc1, k.rsrc2 };
	const uint32_t rsrc3 = k.rsrc3;
	const uint32_t user[2] = { static_cast<uint32_t>(kernarg), static_cast<uint32_t>(kernarg >> 32) };
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_LO, pgm, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC1, rsrc, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC3, &rsrc3, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_RESOURCE_LIM, &zero, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_TMPRING, &zero, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE0, all, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE2, all, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE4, none4, 4);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_START_X, start, 3);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_NUM_THREAD_X, threads, 3);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_USER_DATA0, user, 2);
	return n;
}

uint32_t recordDispatch(uint32_t *ib, const CodeObj::Kernel &k, uint32_t groups) {
	return rdna4_pm4_dispatch_direct(ib, groups, 1, 1,
	                                 RDNA4_PM4_DISPATCH_SHADER_EN | RDNA4_PM4_DISPATCH_FORCE_START0 |
	                                 (k.wave32() ? RDNA4_PM4_DISPATCH_WAVE32 : 0));
}

} // namespace

void RDNA4Compute::vmIdTest() {
	VLOG("rdna4-vmid-test=1: S1 diagnostic (docs/w13-vmid.md s.8.1); one line per probe, a dump on failure");
	trail("vmidtest: start");
	if (!initRuntimeHeap() || !devHeap.size() || !poolCpu) {
		VLOG("skipped: the runtime heap/DMA is not up");
		publishResult("vmidtest", "SKIPPED no runtime heap");
		return;
	}

	/* ---- the vadd image ------------------------------------------------------------------ */
	CodeObj::Kernel kern {};
	CodeObj::Image img {};
	const char *why = nullptr;
	if (!CodeObj::findKernel(kVaddCodeObject, sizeof(kVaddCodeObject), "vadd", kern, &why) ||
	    !CodeObj::parseImage(kVaddCodeObject, sizeof(kVaddCodeObject), img, &why) ||
	    img.size > 3 * 0x1000 || !kern.wantsKernargPtr() || kern.userSgprCount() != 2) {
		VLOG("skipped: the vadd code object is not usable here (%s)", why ? why : "shape");
		publishResult("vmidtest", "SKIPPED vadd image");
		return;
	}

	if (kVmQueueBase + 7 * kVmQueueStride + 0xa000 > pool.size) {
		VLOG("skipped: the pool is too small for the test areas");
		publishResult("vmidtest", "SKIPPED pool size");
		return;
	}

	/* ---- layout ---------------------------------------------------------------------------
	 * Queue areas 0..5 are the pool slots a real client would use (none is open at boot): MQD, EOP, PQ, rptr,
	 * wptr, fence, and a data page each (kVm*). Area 7 holds the IB, data, kernarg, a, b, c and code pages. */
	auto qbase = [](uint32_t k) { return kVmQueueBase + k * kVmQueueStride; };
	const uint32_t dp = qbase(7);
	auto va = [](uint32_t page) { return GpuVm::kVaStart + 0x1000ull * page; };
	enum { kPgIb = 0, kPgData = 1, kPgKernarg = 2, kPgA = 3, kPgB = 4, kPgC = 5, kPgCode = 6, kPgCodeNoExec = 10,
	       kPgEnd = 13, kPgQPq = 16, kPgQEop = 17, kPgQRptr = 18, kPgQWptr = 19, kPgQFence = 20 };

	char summary[160] = {};
	size_t sumLen = 0;
	auto note = [&](const char *name, char code) {
		if (sumLen + 8 < sizeof(summary))
			sumLen += snprintf(summary + sumLen, sizeof(summary) - sumLen, "%s%s=%c", sumLen ? " " : "", name, code);
	};
	auto faultText = [&](char *buf, size_t n) {
		const uint32_t st = rdGc(GcL2FaultStatusLo);
		if (!st)
			snprintf(buf, n, "none");
		else
			snprintf(buf, n, "0x%08x (VMID %u CID 0x%x PERM 0x%x WALKER %u MAP %u RW %u) VA 0x%llx", st,
			         (st >> 20) & 0xf, (st >> 9) & 0x1ff, (st >> 4) & 0xf, (st >> 1) & 7, (st >> 8) & 1,
			         (st >> 18) & 1, static_cast<unsigned long long>(gcFaultVa()));
	};

	/* ---- T0: what is busy at idle, and SH_MEM for every VMID --------------------------------- */
	trail("vmidtest: T0 survey");
	gfxPmSurvey("vmidtest start");
	trail("vmidtest: T0 SH_MEM");
	for (uint32_t v = 1; v <= 15; v++) {
		grbmSelect(0, 0, 0, v);
		wr(IpDiscovery::HwGc, ShMemConfig, kShMemConfigDefault);
		wr(IpDiscovery::HwGc, ShMemBases, kShMemBasesDefault);
	}
	{
		char line[240];
		size_t n = 0;
		uint32_t bad = 0;
		for (uint32_t v = 1; v <= 15; v++) {
			grbmSelect(0, 0, 0, v);
			const uint32_t cfg = rdGc(ShMemConfig), bases = rdGc(ShMemBases);
			bad += cfg != kShMemConfigDefault || bases != kShMemBasesDefault;
			if (v == 1 || v == 8 || v == 15 || cfg != kShMemConfigDefault || bases != kShMemBasesDefault)
				n += snprintf(line + n, sizeof(line) - n, " v%u %08x/%08x", v, cfg, bases);
		}
		grbmSelect(0, 0, 0, 0);
		VLOG("T0 SH_MEM_CONFIG/BASES written for VMIDs 1-15, read back (config/bases):%s; %u of 15 differ from what was written%s",
		     line, bad, bad ? " <-- the banked register does not hold per VMID" : "");
		note("T0", bad ? 'D' : 'P');
		/* amdgpu sets SPI_GDBG_PER_VMID_CNTL.TRAP_EN only for the KFD VMIDs 8-15 (gfx_v12_0.c:1795-1799); the kext sets it for none. */
		n = 0;
		for (uint32_t v = 1; v <= 15; v++) {
			grbmSelect(0, 0, 0, v);
			n += snprintf(line + n, sizeof(line) - n, " v%u %x", v, rdGc(Reg { 0, 0x1f72 }));
		}
		grbmSelect(0, 0, 0, 0);
		VLOG("T0 SPI_GDBG_PER_VMID_CNTL per VMID:%s", line);
	}
	vmIdSurvey("vmidtest start");

	/* ---- tables (one set; the context of each VMID under test points at it) ------------------ */
	uint64_t table = 0;
	if (!devHeap.alloc(kVmTableBytes, table)) {
		publishResult("vmidtest", "SKIPPED no table memory");
		return;
	}
	RtClient c {};
	c.tableOffset = table;
	c.rootMc = vramMc(table);
	c.tableShadow = reinterpret_cast<uint64_t *>(IOMalloc(kVmTableBytes));
	if (!c.tableShadow || !gpuPhysical(c.rootMc, c.rootPhys)) {
		if (c.tableShadow)
			IOFree(c.tableShadow, kVmTableBytes);
		devHeap.free(table);
		publishResult("vmidtest", "SKIPPED tables");
		return;
	}
	bzero(c.tableShadow, kVmTableBytes);
	c.tableShadow[0] = GpuVm::encodePde(c.rootPhys + 0x1000, GpuVm::kValid, 2);
	c.tableShadow[0x1000 / sizeof(uint64_t) + GpuVm::index(GpuVm::kVaStart, 1)] =
		GpuVm::encodePde(c.rootPhys + 0x2000, GpuVm::kValid, 1);

	/* Data the probes use (CPU writes through the BAR, HDP flushed). */
	for (uint32_t off = 0; off < 0xa000; off += 4)
		*poolDw(dp + off) = 0;
	/* The s_endpgm-only kernel of T5c: a wave that does nothing and touches no memory. */
	for (uint32_t i = 0; i < 64; i++)
		*poolDw(dp + 0x9000 + 4 * i) = i ? 0xbf9f0000u : 0xbf810000u;   // s_endpgm, then s_code_end padding
	uint8_t *codeCpu = poolCpu + dp + 0x6000;
	for (uint32_t i = 0; i < img.count; i++) {
		const auto &s = img.seg[i];
		if (s.vaddr + s.fileSize > 3 * 0x1000 || s.fileOffset + s.fileSize > sizeof(kVaddCodeObject)) {
			VLOG("skipped: vadd segment does not fit");
			IOFree(c.tableShadow, kVmTableBytes);
			devHeap.free(table);
			publishResult("vmidtest", "SKIPPED image layout");
			return;
		}
		memcpy(codeCpu + s.vaddr, kVaddCodeObject + s.fileOffset, s.fileSize);
	}
	auto fillVadd = [&]() {
		for (uint32_t i = 0; i < kItems; i++) {
			*poolDw(dp + 0x3000 + 4 * i) = i * 7u + 1u;          // a
			*poolDw(dp + 0x4000 + 4 * i) = (i ^ 0x55u);          // b
			*poolDw(dp + 0x5000 + 4 * i) = 0xdeadbeefu;          // c
		}
		flushHdp();
	};
	fillVadd();
	const uint64_t kernarg[3] = { va(kPgA), va(kPgB), va(kPgC) };
	for (uint32_t i = 0; i < 6; i++)
		*poolDw(dp + 0x2000 + 4 * i) = reinterpret_cast<const uint32_t *>(kernarg)[i];
	flushHdp();
	auto vaddOk = [&]() {
		uint32_t bad = 0;
		for (uint32_t i = 0; i < kItems; i++)
			bad += *poolDw(dp + 0x5000 + 4 * i) != (i * 7u + 1u) + 3u * (i ^ 0x55u);
		return bad;
	};

	const uint64_t codeVa = va(kPgCode) + kern.entryVa;
	const uint64_t codeVaNoExec = va(kPgCodeNoExec) + kern.entryVa;
	const uint64_t codeVaEnd = va(kPgEnd);
	bool mapped = vmMap(c, va(kPgIb), poolMc(dp), 0x1000, true) &&
	              vmMap(c, va(kPgData), poolMc(dp + 0x1000), 0x1000, false) &&
	              vmMap(c, va(kPgKernarg), poolMc(dp + 0x2000), 0x1000, false) &&
	              vmMap(c, va(kPgA), poolMc(dp + 0x3000), 0x1000, false) &&
	              vmMap(c, va(kPgB), poolMc(dp + 0x4000), 0x1000, false) &&
	              vmMap(c, va(kPgC), poolMc(dp + 0x5000), 0x1000, false);
	for (uint32_t p = 0; mapped && p < 3; p++)
		mapped = vmMap(c, va(kPgCode + p), poolMc(dp + 0x6000 + 0x1000 * p), 0x1000, true);
	mapped = mapped && vmMap(c, va(kPgEnd), poolMc(dp + 0x9000), 0x1000, true);
	{   /* T6's alias of the same code pages WITHOUT the EXECUTABLE bit: leaves are only non-executable with vmExecOff. */
		const bool savedExecOff = vmExecOff;
		vmExecOff = true;
		for (uint32_t p = 0; mapped && p < 3; p++)
			mapped = vmMap(c, va(kPgCodeNoExec + p), poolMc(dp + 0x6000 + 0x1000 * p), 0x1000, false);
		vmExecOff = savedExecOff;
	}
	/* The client-style queue of T4/T5b lives in area 3: its ring, EOP, rptr report, wptr poll and fence are
	 * VAs in the VM, exactly as rtOpen maps them; only the MQD stays an MC address. */
	const uint32_t a3 = qbase(3);
	mapped = mapped && vmMap(c, va(kPgQPq), poolMc(a3 + kVmPq), kPqSize, true) &&
	         vmMap(c, va(kPgQEop), poolMc(a3 + kVmEop), 0x1000, false) &&
	         vmMap(c, va(kPgQRptr), poolMc(a3 + kVmRptr), 0x1000, false) &&
	         vmMap(c, va(kPgQWptr), poolMc(a3 + kVmWptr), 0x1000, false) &&
	         vmMap(c, va(kPgQFence), poolMc(a3 + kVmFence), 0x1000, false);
	if (!mapped) {
		VLOG("skipped: page-table setup failed");
		IOFree(c.tableShadow, kVmTableBytes);
		devHeap.free(table);
		publishResult("vmidtest", "SKIPPED page tables");
		return;
	}

	auto ctxOn = [&](uint32_t v) {
		c.vmid = v;
		const bool ok = vmContextInit(c);
		gcFaultClear();
		(void)vmInvalidate(v, "vmidtest context on");
		return ok;
	};
	auto ctxOff = [&](uint32_t v) {
		wr(IpDiscovery::HwGc, Reg { 0, GcCtx1Cntl.dword + v - 1 }, 0);
		(void)vmInvalidate(v, "vmidtest context off");
	};

	/* ---- queue helpers ------------------------------------------------------------------------ */
	struct Q { uint32_t area, pipe, queue, db; Pm4::Queue pm; bool up; };
	auto mkq = [&](uint32_t area, uint32_t pipe, uint32_t queue) {
		Q q {};
		q.area = area; q.pipe = pipe; q.queue = queue; q.db = (0x0d + 8 + area) * 2; q.up = false;
		return q;
	};
	auto hqdLine = [&](const char *what, const Q &q, uint32_t vmid) {
		grbmSelect(1, q.pipe, q.queue, vmid);
		const uint32_t active = rdGc(CpHqdActive), dbc = rdGc(CpHqdPqDoorbell), rptr = rdGc(CpHqdPqRptr),
		               wptrLo = rdGc(CpHqdPqWptrLo), hv = rdGc(CpHqdVmid);
		grbmSelect(0, 0, 0, 0);
		VLOG("%s: HQD %u/%u ACTIVE %u HQD_VMID %u rptr %u wptr %u doorbell control 0x%08x (%s)", what, q.pipe, q.queue,
		     active & 1, hv & 0xf, rptr, wptrLo, dbc, (dbc >> 31) ? "HIT still set: the MEC has not consumed the doorbell" : "consumed");
	};
	auto hqdDequeue = [&](const char *what, Q &q, uint32_t vmid, bool kill) {
		if (!q.up)
			return true;
		grbmSelect(1, q.pipe, q.queue, vmid);
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
		bool idle = false;
		for (uint32_t us = 0; us < 100000 && !idle; us += 10) {
			idle = !(rdGc(CpHqdActive) & 1);
			if (!idle)
				IODelay(10);
		}
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
		bool killed = false;
		if (!idle && kill) {
			/* The W6 sequence (recoverComputeQueue): RESET_WAVES dequeue + SQ_CMD kill of this VMID's waves. */
			grbmSelect(1, q.pipe, q.queue, vmid);
			wr(IpDiscovery::HwGc, CpHqdDequeueReq, 2);
			wr(IpDiscovery::HwGc, SqCmd, 3u | (1u << 4) | (1u << 7) | ((vmid & 0xf) << 28));
			for (uint32_t us = 0; us < 100000 && !idle; us += 10) {
				idle = !(rdGc(CpHqdActive) & 1);
				if (!idle)
					IODelay(10);
			}
			wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
			killed = true;
		}
		const uint32_t active = rdGc(CpHqdActive);
		grbmSelect(0, 0, 0, 0);
		VLOG("%s: dequeue HQD %u/%u: %s (ACTIVE 0x%08x%s)", what, q.pipe, q.queue, idle ? "ok" : "TIMED OUT", active,
		     killed ? ", after RESET_WAVES + SQ_CMD kill" : "");
		if (idle)
			q.up = false;
		return idle;
	};
	auto waitFence = [&](volatile uint32_t *f, uint32_t seq, uint32_t ms) {
		for (uint32_t us = 0; us < ms * 1000 && *f != seq; us += 10)
			IODelay(10);
		return *f == seq;
	};
	auto dumpFail = [&](const char *what, const Q &q, uint32_t vmid) {
		hqdLine(what, q, vmid);
		logComputeQueueState(what, q.pipe, q.queue, vmid);
	};
	/* A VMID-0 (kernel) queue: every address is an MC address in the pool, like amdgpu's compute rings. */
	auto kqStart = [&](Q &q) {
		const uint32_t b = qbase(q.area);
		for (uint32_t off = 0; off < 0x7000; off += 4)
			*poolDw(b + off) = 0;
		flushHdp();
		if (!q.pm.init(poolDw(b + kVmPq), poolMc(b + kVmPq), kPqSize))
			return false;
		gcFaultClear();
		q.up = hqdInitFor(false, q.pipe, q.queue, 0, poolMc(b + kVmMqd), poolMc(b + kVmEop) >> 8,
		                  poolMc(b + kVmPq) >> 8, poolMc(b + kVmRptr), poolMc(b + kVmWptr), q.db);
		return q.up;
	};
	auto emitAll = [&](Pm4::Queue &pm, const uint32_t *dw, uint32_t n) {
		for (uint32_t i = 0; i < n;) {
			const uint32_t chunk = n - i > 8 ? 8 : n - i;      // emit in small pieces: Queue::emit wraps per call
			if (!pm.emit(dw + i, chunk))
				return false;
			i += chunk;
		}
		return true;
	};
	auto ibPad = [&](uint32_t *ib, uint32_t n) {
		while (n & 7)
			ib[n++] = kNop;
		return n;
	};
	auto putIb = [&](const uint32_t *ib, uint32_t n) {       // the IB page the kernel queue's jobs fetch in the VM
		for (uint32_t i = 0; i < n; i++)
			*poolDw(dp + 4 * i) = ib[i];
		flushHdp();
	};
	uint32_t seq = 0x564d1000;
	/* One job on a VMID-0 queue: INDIRECT_BUFFER(vmid, VA of the IB page) then a ring-level RELEASE_MEM fence at a
	 * kernel address (amdgpu_fence_emit does the same). */
	auto kqJob = [&](Q &q, uint32_t vmid, uint32_t ibDwords, uint32_t timeoutMs) {
		const uint32_t b = qbase(q.area);
		volatile uint32_t *fence = poolDw(b + kVmFence);
		const uint32_t s = ++seq;
		uint32_t pkt[8];
		const bool built = q.pm.emit(pkt, Pm4::acquireMem(pkt, Pm4::kGcrMemSync)) &&
		                   q.pm.emit(pkt, Pm4::indirectBufferCompute(pkt, va(kPgIb), ibDwords, vmid)) &&
		                   q.pm.emit(pkt, Pm4::releaseMem(pkt, poolMc(b + kVmFence), s));
		if (!built)
			return false;
		flushHdp();
		pm4Kick(q.pm, q.db, q.pm.wptr());
		return waitFence(fence, s, timeoutMs);
	};

	bool shaderHang = false;
	uint32_t ib[96];

	/* ---- T1: slot reuse with no VM anywhere ---------------------------------------------------- */
	auto plainProbe = [&](const char *name, Q &q, uint32_t pat) {
		trail(name);
		const uint32_t b = qbase(q.area);
		const bool up = kqStart(q);
		*poolDw(b + kVmKernarg) = 0;
		flushHdp();
		uint32_t pkt[8];
		const uint32_t s = ++seq;
		bool fence = false;
		if (up && q.pm.emit(pkt, Pm4::writeData(pkt, poolMc(b + kVmKernarg), pat)) &&
		    q.pm.emit(pkt, Pm4::releaseMem(pkt, poolMc(b + kVmFence), s))) {
			flushHdp();
			pm4Kick(q.pm, q.db, q.pm.wptr());
			fence = waitFence(poolDw(b + kVmFence), s, 200);
		}
		const bool data = *poolDw(b + kVmKernarg) == pat;
		char ft[96];
		faultText(ft, sizeof(ft));
		VLOG("%s: VMID-0 queue on %u/%u: activated %d, fence %d, data %d, fault %s => %s", name, q.pipe, q.queue, up,
		     fence, data, ft, up && fence && data ? "PASS" : "FAIL");
		if (!(up && fence && data))
			dumpFail(name, q, 0);
		const bool idle = hqdDequeue(name, q, 0, false);
		return up && fence && data && idle;
	};
	Q q01 = mkq(0, 0, 1), q11 = mkq(1, 1, 1);
	note("T1a", plainProbe("T1a slot (0,1) first use after the boot test", q01, 0x7e570001) ? 'P' : 'F');
	note("T1b", plainProbe("T1b slot (0,1) reactivated", q01, 0x7e570002) ? 'P' : 'F');
	note("T2", plainProbe("T2 never-used slot (1,1)", q11, 0x7e570003) ? 'P' : 'F');

	/* ---- T3: U1 / U2: VMID-0 queue, IB in VMID n ----------------------------------------------- */
	Q q13 = mkq(2, 1, 3);
	bool t3Up = false, t3Vmid8 = false;
	{
		trail("vmidtest: T3 queue");
		t3Up = kqStart(q13);
		VLOG("T3: VMID-0 kernel queue on 1/3 activated %d (PRIV_STATE|KMD_QUEUE, CP_HQD_VMID 0, MQD VMID 0)", t3Up);
		static const uint32_t order[3] = { 8, 1, 15 };
		for (uint32_t i = 0; t3Up && i < 3; i++) {
			const uint32_t n = order[i];
			char name[16];
			snprintf(name, sizeof(name), "T3/%u", n);
			trail("vmidtest: T3 IB");
			const bool ctx = ctxOn(n);
			const uint32_t pat = 0x7e570100 + n;
			*poolDw(dp + 0x1000) = 0;
			uint32_t w = Pm4::writeData(ib, va(kPgData), pat);
			w = ibPad(ib, w);
			putIb(ib, w);
			const bool fence = kqJob(q13, n, w, 200);
			const bool data = *poolDw(dp + 0x1000) == pat;
			char ft[96];
			faultText(ft, sizeof(ft));
			VLOG("T3 U1%s: VMID-0 queue, IB packet VMID %u (context %s): fence %d, data %d, fault %s => %s", n == 8 ? "" : "/U2", n,
			     ctx ? "on" : "FAILED to enable", fence, data, ft, fence && data && !rdGc(GcL2FaultStatusLo) ? "PASS" : "FAIL");
			const bool ok = fence && data && !rdGc(GcL2FaultStatusLo);
			note(name, ok ? 'P' : 'F');
			if (n == 8)
				t3Vmid8 = ok;
			if (!ok) {
				dumpFail(name, q13, 0);
				gcFaultClear();
				if (!fence)
					break;       // the queue is not serviced: the other VMIDs cannot be tested through it
			}
			gcFaultClear();
		}
	}

	/* ---- T5: the vadd dispatch as an IB in VMID 8 ---------------------------------------------- */
	if (t3Up && t3Vmid8 && q13.up) {
		trail("vmidtest: T5 vadd IB");
		ctxOn(8);
		fillVadd();
		uint32_t w = recordVaddIb(ib, codeVa, kern, va(kPgKernarg), kItems / 64);
		w = ibPad(ib, w);
		putIb(ib, w);
		const bool fence = kqJob(q13, 8, w, 500);
		const uint32_t bad = fence ? vaddOk() : kItems;
		char ft[96];
		faultText(ft, sizeof(ft));
		VLOG("T5: vadd as an IB in VMID 8 from the VMID-0 queue: fence %d, %u of %u results wrong, fault %s => %s "
		     "(the first shader wave in a non-zero VMID)", fence, bad, kItems, ft, fence && !bad ? "PASS" : "FAIL");
		note("T5", fence && !bad ? 'P' : (fence ? 'F' : 'H'));
		if (!fence) {
			shaderHang = true;
			dumpFail("T5", q13, 0);
			gfxPmSurvey("vmidtest T5 hang");
			(void)hqdDequeue("T5", q13, 8, true);
		}
		gcFaultClear();
	} else {
		VLOG("T5 skipped: T3 (IB in VMID 8 from a VMID-0 queue) did not pass");
		note("T5", 'S');
	}
	(void)hqdDequeue("T3/T5", q13, 0, true);

	/* ---- T4: a client-style queue: HQD VMID 8, queue addresses in the VM ------------------------ */
	Q q12 = mkq(3, 1, 2);
	auto clientStart = [&](uint32_t vmid) {
		for (uint32_t off = 0; off < 0x7000; off += 4)
			*poolDw(a3 + off) = 0;
		flushHdp();
		if (!q12.pm.init(poolDw(a3 + kVmPq), va(kPgQPq), kPqSize))
			return false;
		ctxOn(vmid);
		gcFaultClear();
		q12.up = hqdInitFor(false, q12.pipe, q12.queue, vmid, poolMc(a3 + kVmMqd), va(kPgQEop) >> 8, va(kPgQPq) >> 8,
		                    va(kPgQRptr), va(kPgQWptr), q12.db);
		return q12.up;
	};
	auto clientWriteData = [&](const char *name, uint32_t pat, uint32_t vmid) {
		trail(name);
		const bool up = clientStart(vmid);
		*poolDw(dp + 0x1000) = 0;
		flushHdp();
		uint32_t pkt[8];
		const uint32_t s = ++seq;
		bool fence = false;
		if (up && q12.pm.emit(pkt, Pm4::writeData(pkt, va(kPgData), pat)) &&
		    q12.pm.emit(pkt, Pm4::releaseMem(pkt, va(kPgQFence), s))) {
			flushHdp();
			pm4Kick(q12.pm, q12.db, q12.pm.wptr());
			fence = waitFence(poolDw(a3 + kVmFence), s, 200);
		}
		const bool data = *poolDw(dp + 0x1000) == pat;
		char ft[96];
		faultText(ft, sizeof(ft));
		VLOG("%s: client-style queue 1/2, HQD VMID %u: activated %d, fence %d, data %d, fault %s => %s", name, vmid, up, fence,
		     data, ft, up && fence && data ? "PASS" : "FAIL");
		if (!(up && fence && data))
			dumpFail(name, q12, vmid);
		return up && fence && data;
	};
	const bool t4a = clientWriteData("T4a first activation (WRITE_DATA)", 0x7e570201, 8);
	note("T4a", t4a ? 'P' : 'F');
	const bool t4aIdle = hqdDequeue("T4a", q12, 8, true);
	note("T4a-deq", t4aIdle ? 'P' : 'F');
	const bool t4b = clientWriteData("T4b reactivated after the dequeue", 0x7e570202, 8);
	note("T4b", t4b ? 'P' : 'F');
	(void)hqdDequeue("T4b", q12, 8, true);
	vmIdSurvey("vmidtest after T4b");
	/* T4c: the same queue with HQD VMID 3 (amdgpu's own VMIDs are 1-7, KFD's 8-15). */
	const bool t4c = clientWriteData("T4c HQD VMID 3 (the gfx range)", 0x7e570203, 3);
	note("T4c", t4c ? 'P' : 'F');
	(void)hqdDequeue("T4c", q12, 3, true);

	/* ---- T5c: the incremental stream on a fresh client-style queue ---------------------------------- */
	bool t5cDone = false;
	if (!shaderHang && t4b) {
		trail("vmidtest: T5c incremental");
		const bool up = clientStart(8);
		fillVadd();
		*poolDw(dp + 0x1000) = 0;
		flushHdp();
		uint32_t part[96];
		char stepName[24] = {};
		int stalled = 0;
		uint32_t stallSeq = 0;
		auto stepWait = [&](const char *what, uint32_t ms) {
			const uint32_t want = static_cast<uint32_t>(q12.pm.wptr());
			uint32_t rp = 0;
			for (uint32_t us = 0; us < ms * 1000; us += 10) {
				grbmSelect(1, q12.pipe, q12.queue, 8);
				rp = rdGc(CpHqdPqRptr);
				grbmSelect(0, 0, 0, 0);
				if (rp == want)
					break;
				IODelay(10);
			}
			grbmSelect(1, q12.pipe, q12.queue, 8);
			const uint32_t dbc = rdGc(CpHqdPqDoorbell);
			grbmSelect(0, 0, 0, 0);
			char ft[96];
			faultText(ft, sizeof(ft));
			VLOG("T5c %s: rptr %u of %u %s, doorbell HIT %d, CPC_BUSY 0x%08x, fault %s", what, rp, want,
			     rp == want ? "(consumed)" : "<-- STALLED", dbc >> 31, rdGc(CpCpcBusyStat), ft);
			return rp == want;
		};
		auto stepSubmit = [&](const uint32_t *dw, uint32_t n) {
			if (!emitAll(q12.pm, dw, n))
				return false;
			flushHdp();
			pm4Kick(q12.pm, q12.db, q12.pm.wptr());
			return true;
		};
		uint32_t pkt[8];
		for (int step = 1; up && step <= 6 && !stalled; step++) {
			uint32_t n = 0;
			switch (step) {
			case 1: n = Pm4::writeData(part, va(kPgData), 0x7e570501); snprintf(stepName, sizeof(stepName), "s1 WRITE_DATA"); break;
			case 2: n = Pm4::acquireMem(part, Pm4::kGcrMemSync); snprintf(stepName, sizeof(stepName), "s2 +ACQUIRE_MEM"); break;
			case 3: n = recordVaddSetup(part, codeVaEnd, kern, va(kPgKernarg)); snprintf(stepName, sizeof(stepName), "s3 +SET_SH_REGs"); break;
			case 4: n = recordDispatch(part, kern, 1); snprintf(stepName, sizeof(stepName), "s4 +DISPATCH s_endpgm"); break;
			case 5:
				n = recordVaddSetup(part, codeVa, kern, va(kPgKernarg));
				n += recordDispatch(part + n, kern, kItems / 64);
				snprintf(stepName, sizeof(stepName), "s5 +vadd dispatch");
				break;
			default:
				stallSeq = ++seq;
				n = Pm4::releaseMem(part, va(kPgQFence), stallSeq);
				snprintf(stepName, sizeof(stepName), "s6 +RELEASE_MEM");
				break;
			}
			if (!stepSubmit(part, n)) {
				stalled = step;
				break;
			}
			if (!stepWait(stepName, step >= 4 ? 500 : 200))
				stalled = step;
		}
		(void)pkt;
		const bool fence = up && !stalled && waitFence(poolDw(a3 + kVmFence), stallSeq, 200);
		const uint32_t bad = fence ? vaddOk() : kItems;
		VLOG("T5c: incremental stream on a fresh HQD-VMID-8 queue => %s", !up ? "queue did not activate"
		     : stalled ? "STALLED at the step above" : fence && !bad ? "PASS: every step consumed, the vadd result is right"
		     : "steps consumed but fence/result wrong");
		if (stalled) {
			char nm[24];
			snprintf(nm, sizeof(nm), "T5c@s%d", stalled);
			note(nm, stalled >= 4 ? 'H' : 'F');
			if (stalled >= 4)
				shaderHang = true;
			dumpFail("T5c", q12, 8);
			vmIdSurvey("vmidtest T5c stall");
			gfxPmSurvey("vmidtest T5c stall");
			/* Siblings: is it this queue, this pipe, or the whole MEC? A plain VMID-0 WRITE_DATA on another queue of the same
			 * pipe and on the other pipe, while the stalled queue stays active. */
			note("sibP", plainProbe("T5c-sib same pipe (1,1)", q11, 0x7e570511) ? 'P' : 'F');
			Q q02s = mkq(4, 0, 2);
			note("sibO", plainProbe("T5c-sib other pipe (0,2)", q02s, 0x7e570512) ? 'P' : 'F');
		} else {
			note("T5c", fence && !bad ? 'P' : 'F');
			t5cDone = fence && !bad;
		}
		(void)hqdDequeue("T5c", q12, 8, true);
		gcFaultClear();
	} else {
		VLOG("T5c skipped: %s", shaderHang ? "a shader probe already hung" : "T4b did not pass");
		note("T5c", 'S');
	}

	/* ---- T5b: rtDispatch's stream: direct ring packets on the VMID-8 queue ---------------------- */
	if (!shaderHang && t4b && t5cDone) {
		trail("vmidtest: T5b vadd direct");
		const bool up = clientStart(8);
		fillVadd();
		/* launch(): select the VMID, rewrite SH_MEM_CONFIG/BASES, then emit the stream into the queue. */
		grbmSelect(0, q12.pipe, q12.queue, 8);
		wr(IpDiscovery::HwGc, ShMemConfig, kShMemConfigDefault);
		wr(IpDiscovery::HwGc, ShMemBases, kShMemBasesDefault);
		grbmSelect(0, 0, 0, 0);
		uint32_t w = recordVaddIb(ib, codeVa, kern, va(kPgKernarg), kItems / 64);
		uint32_t pkt[8];
		const uint32_t s = ++seq;
		bool fence = false;
		if (up && emitAll(q12.pm, ib, w) && q12.pm.emit(pkt, Pm4::releaseMem(pkt, va(kPgQFence), s))) {
			flushHdp();
			pm4Kick(q12.pm, q12.db, q12.pm.wptr());
			fence = waitFence(poolDw(a3 + kVmFence), s, 500);
		}
		const uint32_t bad = fence ? vaddOk() : kItems;
		char ft[96];
		faultText(ft, sizeof(ft));
		VLOG("T5b: vadd as direct ring packets on the HQD-VMID-8 queue (what rtDispatch does): activated %d, fence %d, %u of %u "
		     "results wrong, fault %s => %s", up, fence, bad, kItems, ft, up && fence && !bad ? "PASS" : "FAIL");
		note("T5b", fence && !bad ? 'P' : (fence ? 'F' : 'H'));
		if (!fence) {
			shaderHang = true;
			dumpFail("T5b", q12, 8);
			gfxPmSurvey("vmidtest T5b hang");
		}
		(void)hqdDequeue("T5b", q12, 8, true);
		gcFaultClear();
	} else {
		VLOG("T5b skipped: %s", shaderHang ? "a shader probe already hung" : !t4b ? "T4b did not pass" : "T5c did not pass (it is the same stream, bisected)");
		note("T5b", 'S');
	}

	/* ---- T6: code page without EXECUTABLE ------------------------------------------------------- */
	Q q02 = mkq(4, 0, 2), q03 = mkq(5, 0, 3);
	if (!shaderHang && t3Vmid8) {
		trail("vmidtest: T6 no-exec code");
		ctxOn(8);
		fillVadd();
		const bool up = kqStart(q02);
		uint32_t w = recordVaddIb(ib, codeVaNoExec, kern, va(kPgKernarg), kItems / 64);
		w = ibPad(ib, w);
		putIb(ib, w);
		const bool fence = up && kqJob(q02, 8, w, 300);
		const uint32_t bad = vaddOk();
		const uint32_t st = rdGc(GcL2FaultStatusLo);
		char ft[96];
		faultText(ft, sizeof(ft));
		const bool execFault = (st & 0x80) && ((st >> 20) & 0xf) == 8;
		VLOG("T6: code page mapped WITHOUT EXECUTABLE: fence %d, %u of %u results wrong, fault %s => %s", fence, bad, kItems, ft,
		     execFault && bad == kItems ? "PASS (the wave's instruction fetch is translated by VMID 8's tables and refused)"
		                                : "FAIL or ambiguous");
		note("T6", execFault && bad == kItems ? 'P' : 'F');
		if (!fence)
			dumpFail("T6", q02, 0);
		(void)hqdDequeue("T6", q02, 8, true);
		gcFaultClear();
		scrubFaultPage();
	} else {
		VLOG("T6 skipped: %s", shaderHang ? "a shader probe already hung" : "T3 did not pass");
		note("T6", 'S');
	}

	/* ---- T7: negative control: the IB packet names a VMID whose context is off ------------------ */
	if (!shaderHang && t3Vmid8) {
		trail("vmidtest: T7 wrong VMID");
		ctxOff(9);
		const bool up = kqStart(q03);
		uint32_t w = Pm4::writeData(ib, va(kPgData), 0x7e570709);
		w = ibPad(ib, w);
		*poolDw(dp + 0x1000) = 0;
		putIb(ib, w);
		const bool fence = up && kqJob(q03, 9, w, 300);
		const bool data = *poolDw(dp + 0x1000) == 0x7e570709;
		const uint32_t st = rdGc(GcL2FaultStatusLo);
		char ft[96];
		faultText(ft, sizeof(ft));
		const bool vmidOk = st && ((st >> 20) & 0xf) == 9 && !data;
		VLOG("T7: IB packet VMID 9 (context off): fence %d, data %d, fault %s => %s", fence, data, ft,
		     vmidOk ? "PASS (the packet's VMID field decides which tables the IB is fetched through)"
		            : data ? "FAIL: the IB RAN although VMID 9 has no tables: the packet's VMID is not what decides (U1 false)"
		                   : "FAIL or ambiguous");
		note("T7", vmidOk ? 'P' : 'F');
		if (!vmidOk)
			dumpFail("T7", q03, 0);
		(void)hqdDequeue("T7", q03, 0, true);
		gcFaultClear();
		scrubFaultPage();
	} else {
		VLOG("T7 skipped: %s", shaderHang ? "a shader probe already hung" : "T3 did not pass");
		note("T7", 'S');
	}

	/* ---- clean up: nothing of the test stays programmed ------------------------------------------ */
	trail("vmidtest: cleanup");
	(void)hqdDequeue("end", q01, 0, true);
	(void)hqdDequeue("end", q11, 0, true);
	(void)hqdDequeue("end", q13, 0, true);
	(void)hqdDequeue("end", q12, 8, true);
	(void)hqdDequeue("end", q02, 0, true);
	(void)hqdDequeue("end", q03, 0, true);
	for (uint32_t v = 1; v <= 15; v++) {
		if (v == 1 || v == 8 || v == 15)
			ctxOff(v);
	}
	bzero(c.tableShadow, kVmTableBytes);
	if (!vmTableSync(c, 0, kVmTableBytes))
		VLOG("cleanup: page-table clear failed");
	IOFree(c.tableShadow, kVmTableBytes);
	devHeap.free(table);
	gcFaultClear();
	scrubFaultPage();
	vmIdSurvey("vmidtest end");
	gfxPmSurvey("vmidtest end");
	VLOG("summary: %s (P pass, F fail, H shader hang, S skipped, D readback differs)", summary);
	publishResult("vmidtest", summary);
	trail("vmidtest: done");
}
