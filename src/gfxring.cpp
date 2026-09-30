//
//  gfxring.cpp
//  RDNA4FB
//
//  W3, the graphics side: the gfx ring (PFP/ME, RS64 firmware) brought up
//  the way amdgpu's gfx_v12_0 does with PSP-loaded firmware:
//
//    - the clear-state buffer (gfx12_cs_data) handed to the RLC
//      (gfx_v12_0_init_csb): the default context state the CP loads;
//    - ring 0 programmed on pipe 0, VMID0 (gfx_v12_0_cp_gfx_resume);
//    - PFP and ME unhalted (gfx_v12_0_cp_gfx_start / cp_gfx_enable);
//
//  then proven the way amdgpu tests a ring: SCRATCH_REG0 through
//  SET_UCONFIG_REG (gfx_v12_0_ring_test_ring), a WRITE_DATA and an
//  end-of-pipe RELEASE_MEM fence (ring_emit_fence), an indirect buffer
//  (ring_test_ib), and enough fenced IBs to wrap the ring several times.
//
//  Boot-arg rdna4-gfx: 2 = the write pointer goes through the gfx ring's doorbell, as amdgpu kicks it (1 is an alias of 2: the MMIO write pointer halts PFP/ME on silicon).
//  Absent = off:
//  nothing here runs. It runs after the compute stages on the bring-up
//  thread; a failure halts PFP/ME again and leaves compute as it was.
//
//  G3, once the ring is up and the runtime's device heap exists: the first
//  draw. One triangle from an NGG passthrough VS (shaders/ngg.s) and a
//  constant-colour PS (shaders/psred.s) into a 256x256 R8G8B8A8 target, from
//  the command stream in gfx12_draw.h (generated from Mesa's register
//  database; premetal/gfx12-draw-notes.md cites every register), then the
//  target is read back: 8192 pixels in rows 64..190 must be 0xFF0000FF and
//  nothing else may change.
//

#include "compute.hpp"
#include "gfx12_draw.h"
#include "linuxref.hpp"
#include "ngg_kernel.h"
#include "nggmsg_kernel.h"
#include "nggstore_kernel.h"
#include "nggvgpr_kernel.h"
#include "nggconst_kernel.h"
#include "../userspace/gfx12tricol.h"   // G4: the shared colour-triangle shaders, descriptor and check (also used by tools/linux-replay)
#include "ngg64_kernel.h"
#include "pm4.hpp"
#include "psred_kernel.h"
#include "psstore_kernel.h"

#include "amdgpu/clearstate_defs.h"
#include "amdgpu/clearstate_gfx12.h"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/c++/OSDictionary.h>
#include <stdarg.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>

#define GLOG(fmt, ...)  IOLog("RDNA4FB: gfx: " fmt "\n", ## __VA_ARGS__)

using namespace GfxReg;

uint32_t RDNA4Compute::requestedGfx() {
	uint32_t mode = 0;
	if (!PE_parse_boot_argn("rdna4-gfx", &mode, sizeof(mode)))
		return 0;
	if (mode == 1) {
		// gfx_v12_0_gfx_ring_init sets ring->use_doorbell = true (gfx_v12_0.c:990)
		// and ring_set_wptr_gfx writes the doorbell then; the MMIO branch is
		// only for a ring without one. On the real card the MMIO write
		// pointer halts PFP/ME (round 2, R2-2), so 1 now means the doorbell.
		IOLog("RDNA4FB: gfx: rdna4-gfx=1 (MMIO write pointer) halts PFP/ME on silicon; using the doorbell like amdgpu\n");
		return 2;
	}
	return mode == 2 ? mode : 0;
}

// gfx_v12_0_get_csb_buffer's layout: the cluster count, then per cluster
// its register count, its first register (absolute dword) and the values;
// RLC_CSIB_* point the RLC at it (gfx_v12_0_init_csb), length in dwords.
bool RDNA4Compute::gfxCsbInit() {
	volatile uint32_t *csb = poolDw(kGfxCsbOffset);
	uint32_t n = 1, clusters = 0;
	for (const cs_section_def *sect = gfx12_cs_data; sect->section; sect++) {
		if (sect->id != SECT_CONTEXT)
			return false;
		for (const cs_extent_def *ext = sect->section; ext->extent; ext++) {
			if ((n + 2 + ext->reg_count) * 4 > kGfxCsbMax)
				return false;
			csb[n++] = ext->reg_count;
			csb[n++] = ext->reg_index;
			for (uint32_t i = 0; i < ext->reg_count; i++)
				csb[n++] = ext->extent[i];
			clusters++;
		}
	}
	csb[0] = clusters;
	flushHdp();
	const uint64_t mc = poolMc(kGfxCsbOffset);
	wr(IpDiscovery::HwGc, RlcCsibAddrHi, static_cast<uint32_t>(mc >> 32));
	wr(IpDiscovery::HwGc, RlcCsibAddrLo, static_cast<uint32_t>(mc) & 0xfffffffc);
	wr(IpDiscovery::HwGc, RlcCsibLength, n);
	GLOG("clear-state buffer: %u clusters, %u dwords at MC 0x%llx (RLC_CSIB 0x%08x_%08x len %u)",
	     clusters, n, mc, rdGc(RlcCsibAddrHi), rdGc(RlcCsibAddrLo), rdGc(RlcCsibLength));
	return true;
}

// gfx_v12_0_cp_gfx_resume for ring 0 of pipe 0, then gfx_v12_0_cp_gfx_start.
bool RDNA4Compute::gfxRingResume() {
	if (!gfxRing.init(poolDw(kGfxRingOffset), poolMc(kGfxRingOffset), kGfxRingSize))
		return false;
	for (uint32_t off = 0; off < kGfxRingSize; off += 4)
		*poolDw(kGfxRingOffset + off) = 0;
	*poolDw(kGfxRptrOffset) = 0;
	*poolDw(kGfxWptrOffset) = 0;
	*poolDw(kGfxWptrOffset + 4) = 0;
	flushHdp();

	wr(IpDiscovery::HwGc, CpRbWptrDelay, 0);
	wr(IpDiscovery::HwGc, CpRbVmid, 0);
	grbmSelect(0, 0, 0, 0);                                 // gfx pipe 0

	uint32_t bufsz = 0;                                     // order_base_2(ring bytes / 8)
	while ((8u << bufsz) < kGfxRingSize)
		bufsz++;
	uint32_t cntl = (bufsz & 0x3f) | (((bufsz - 2) & 0x3f) << 8);
	{   // W42 (opt-in): amdgpu's MES/MQD path builds CP_GFX_HQD_CNTL from the register default, which carries MIN_AVAILSZ=3 and
		// MIN_IB_AVAILSZ=3 (bits 20-23): Linux reads CP_RB0_CNTL = 0x00f0088a. The proven ring tests ran without them, so it is off by default.
		uint32_t rbmin = 0;
		if (PE_parse_boot_argn("rdna4-gfxrbmin", &rbmin, sizeof(rbmin)) && rbmin == 1) {
			cntl |= (3u << 20) | (3u << 22);
			GLOG("ring 0: rdna4-gfxrbmin=1: CP_RB0_CNTL gets MIN_AVAILSZ=3 and MIN_IB_AVAILSZ=3 like the Linux queue (0x%08x)", cntl);
		}
	}
	wr(IpDiscovery::HwGc, CpRb0Cntl, cntl);
	wr(IpDiscovery::HwGc, CpRb0Wptr, 0);
	wr(IpDiscovery::HwGc, CpRb0WptrHi, 0);
	const uint64_t rptrMc = poolMc(kGfxRptrOffset), wptrMc = poolMc(kGfxWptrOffset);
	wr(IpDiscovery::HwGc, CpRb0RptrAddr, static_cast<uint32_t>(rptrMc) & ~3u);
	wr(IpDiscovery::HwGc, CpRb0RptrAddrHi, static_cast<uint32_t>(rptrMc >> 32) & 0xffff);
	wr(IpDiscovery::HwGc, CpRbWptrPollAddrLo, static_cast<uint32_t>(wptrMc) & ~3u);
	wr(IpDiscovery::HwGc, CpRbWptrPollAddrHi, static_cast<uint32_t>(wptrMc >> 32));
	IODelay(1000);
	wr(IpDiscovery::HwGc, CpRb0Cntl, cntl);
	const uint64_t base = gfxRing.mc() >> 8;
	wr(IpDiscovery::HwGc, CpRb0Base, static_cast<uint32_t>(base));
	wr(IpDiscovery::HwGc, CpRb0BaseHi, static_cast<uint32_t>(base >> 32));
	wr(IpDiscovery::HwGc, CpRbActive, 1);

	// gfx_v12_0_cp_gfx_set_doorbell (+ cp_set_doorbell_range's gfx half).
	uint32_t db = rdGc(CpRbDoorbellControl) & ~(0x0ffffffcu | kCpRbDoorbellEn);
	db |= ((kGfxDoorbellDword << 2) & 0x0ffffffc) | kCpRbDoorbellEn;
	wr(IpDiscovery::HwGc, CpRbDoorbellControl, db);
	wr(IpDiscovery::HwGc, CpRbDoorbellRangeLower, (kGfxDoorbellDword << 2) & kCpRbDoorbellRangeMask);
	wr(IpDiscovery::HwGc, CpRbDoorbellRangeUpper, kCpRbDoorbellRangeMask);
	grbmSelect(0, 0, 0, 0);

	wr(IpDiscovery::HwGc, CpMaxContext, kGfxMaxHwContexts - 1);
	wr(IpDiscovery::HwGc, CpDeviceId, 1);

	// gfx_v12_0_cp_gfx_enable: unhalt PFP and ME, wait for the CP to idle.
	trail("gfx: unhalt PFP/ME");
	wr(IpDiscovery::HwGc, CpMeCntl, rdGc(CpMeCntl) & ~(kCpMePfpHalt | kCpMeMeHalt));
	{   // W42 (opt-in): Linux reads CP_ME_CNTL = 0xa000 (CE_HALT clear) while ours keeps CE_HALT set; amdgpu itself never writes the bit.
		uint32_t ce = 0;
		if (PE_parse_boot_argn("rdna4-gfxce", &ce, sizeof(ce)) && ce == 1) {
			wr(IpDiscovery::HwGc, CpMeCntl, rdGc(CpMeCntl) & ~kCpMeCeHalt);
			GLOG("ring 0: rdna4-gfxce=1: CE_HALT cleared, CP_ME_CNTL 0x%08x", rdGc(CpMeCntl));
		}
	}
	uint32_t stat = 0xffffffff;
	for (uint32_t us = 0; us < 100000; us += 10) {
		stat = rdGc(CpStat);
		if (stat == 0)
			break;
		IODelay(10);
	}
	GLOG("ring 0: %u KiB at MC 0x%llx, CP_RB0_CNTL 0x%08x, %s, CP_ME_CNTL 0x%08x, CP_STAT 0x%08x%s",
	     kGfxRingSize >> 10, gfxRing.mc(), rdGc(CpRb0Cntl),
	     "doorbell", rdGc(CpMeCntl), stat,
	     stat ? " (did not idle: amdgpu reports and goes on)" : "");
	return true;
}

void RDNA4Compute::gfxKick(uint64_t wptrDwords) {
	*poolDw(kGfxWptrOffset) = static_cast<uint32_t>(wptrDwords);
	*poolDw(kGfxWptrOffset + 4) = static_cast<uint32_t>(wptrDwords >> 32);
	flushHdp();
	// gfx_v12_0_ring_set_wptr_gfx with use_doorbell (gfx_v12_0.c:4458): the
	// wptr shadow above, then the 64-bit doorbell. The MMIO CP_RB0_WPTR path is
	// gone: it halts PFP/ME on silicon (round 2, R2-2).
	{ GcAccess g(*this, 0xdb000000u | kGfxDoorbellDword); if (g.ok) doorbells[kGfxDoorbellDword / 2] = wptrDwords; }   // W27
}

bool RDNA4Compute::gfxFenceWait(uint32_t seq, uint32_t timeoutUs) {
	for (uint32_t us = 0; us < timeoutUs; us += 10) {
		if (*poolDw(kGfxFenceOffset) == seq)
			return true;
		IODelay(10);
	}
	return *poolDw(kGfxFenceOffset) == seq;
}

void RDNA4Compute::gfxStatus(const char *tag) {
	grbmSelect(0, 0, 0, 0);
	GLOG("%s: GRBM 0x%08x CP_STAT 0x%08x CP_ME_CNTL 0x%08x PFP pc 0x%x ME pc 0x%x", tag,
	     rdGc(GrbmStatus), rdGc(CpStat), rdGc(CpMeCntl), rdGc(CpPfpInstrPntr), rdGc(CpMeInstrPntr));
	GLOG("%s: RB0 rptr 0x%x (writeback 0x%x) wptr 0x%x_%08x (driver %llu), active %u, fence 0x%x "
	     "(want 0x%x), CSIB 0x%08x_%08x len %u", tag, rdGc(CpRb0Rptr), *poolDw(kGfxRptrOffset),
	     rdGc(CpRb0WptrHi), rdGc(CpRb0Wptr), static_cast<unsigned long long>(gfxRing.wptr()),
	     rdGc(CpRbActive), *poolDw(kGfxFenceOffset), gfxFence, rdGc(RlcCsibAddrHi),
	     rdGc(RlcCsibAddrLo), rdGc(RlcCsibLength));
	logGcFault(tag);
}

bool RDNA4Compute::stageGfxRing() {
	gfxClientReset();   // W12k: a fresh ring (first bring-up or after a wake): nothing is pending, not wedged, not parked
	OSDictionary *d = OSDictionary::withCapacity(8);
	auto put = [d](const char *key, uint64_t v) {
		if (OSNumber *num = d ? OSNumber::withNumber(v, 64) : nullptr) {
			d->setObject(key, num);
			num->release();
		}
	};
	auto finish = [&](bool ok, const char *what) {
		put("Mode", gfxMode);
		put("Ready", ok);
		char result[128];
		snprintf(result, sizeof(result), "%s %s", ok ? "PASS" : "FAIL",
		         ok ? "ring test" : (what ? what : "ring test"));
		publishResult("gfx", result);
		if (d) {
			env.owner->setProperty("Compute,GFX", d);
			d->release();
		}
		if (!ok) {
			gfxStatus(what);
			// Halt PFP/ME again: nothing else may run on a ring we gave up on.
			wr(IpDiscovery::HwGc, CpMeCntl, rdGc(CpMeCntl) | kCpMePfpHalt | kCpMeMeHalt);
			GLOG("%s failed: PFP/ME halted again, compute is not affected", what);
			gfxMode = 0;
		}
		return ok;
	};

	GLOG("bring-up (rdna4-gfx=%u): CP_ME_CNTL 0x%08x CP_STAT 0x%08x PFP pc 0x%x ME pc 0x%x", gfxMode,
	     rdGc(CpMeCntl), rdGc(CpStat), rdGc(CpPfpInstrPntr), rdGc(CpMeInstrPntr));
	if (!doorbells)
		return finish(false, "doorbell mode without the doorbell BAR");
	gfxGoldenInit();   // amdgpu's 3D-pipeline golden registers, before the CP starts
	gfxFaultMark("bring-up start (clears what earlier stages left)");
	gfxRs64Evidence("bring-up");
	gfxQueueEvidence("bring-up");
	trail("gfx: clear state + ring");
	if (!gfxCsbInit())
		return finish(false, "clear-state buffer");
	gfxSrmEnable();   // W37 #1: amdgpu enables the SRM right after the CSB init (gfx_v12_0_rlc_resume)
	if (!gfxRingResume())
		return finish(false, "ring setup");
	gfxFaultMark("ring setup and unhalt");

	// 1. amdgpu's ring test: SCRATCH_REG0 through SET_UCONFIG_REG.
	trail("gfx: ring test");
	uint32_t pkt[16];
	const uint32_t scratchAbs = shAbs(ScratchReg0);
	wr(IpDiscovery::HwGc, ScratchReg0, 0xCAFEDEAD);
	gfxRing.emit(pkt, Pm4::setUconfigReg(pkt, scratchAbs, 0xDEADBEEF));
	gfxKick(gfxRing.wptr());
	uint32_t scratch = 0;
	for (uint32_t us = 0; us < 100000; us += 10) {
		scratch = rdGc(ScratchReg0);
		if (scratch == 0xDEADBEEF)
			break;
		IODelay(10);
	}
	put("ScratchReg", scratch);
	GLOG("ring test: SCRATCH_REG0 0x%08x (%s)", scratch, scratch == 0xDEADBEEF ? "ok" : "not written");
	if (scratch != 0xDEADBEEF)
		return finish(false, "ring test");
	gfxFaultMark("ring test");
	if (requestedGfxProbe())
		gfxPacketProbe();   // W33 S1: one packet per submission, a fault mark after each

	// 2. A WRITE_DATA and the end-of-pipe fence amdgpu's jobs end with.
	*poolDw(kGfxTestOffset) = 0;
	*poolDw(kGfxFenceOffset) = 0;
	flushHdp();
	gfxRing.emit(pkt, Pm4::writeData(pkt, poolMc(kGfxTestOffset), 0x600DF00D));
	gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
	gfxKick(gfxRing.wptr());
	bool fenced = gfxFenceWait(gfxFence, 100000);
	const uint32_t data = *poolDw(kGfxTestOffset);
	GLOG("WRITE_DATA 0x%08x (%s), RELEASE_MEM fence %s", data, data == 0x600DF00D ? "ok" : "not written",
	     fenced ? "signalled" : "NOT signalled");
	if (!fenced || data != 0x600DF00D)
		return finish(false, "fence test");
	gfxFaultMark("WRITE_DATA + fence test");

	// 3. amdgpu's IB test: an indirect buffer with a WRITE_DATA, fenced.
	trail("gfx: IB test");
	volatile uint32_t *ib = poolDw(kGfxIbOffset);
	uint32_t ibDw = Pm4::writeData(pkt, poolMc(kGfxTestOffset + 4), 0x1B0B1B0B);
	for (uint32_t i = 0; i < ibDw; i++)
		ib[i] = pkt[i];
	*poolDw(kGfxTestOffset + 4) = 0;
	flushHdp();
	gfxRing.emit(pkt, Pm4::indirectBufferGfx(pkt, poolMc(kGfxIbOffset), ibDw, 0));
	gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
	gfxKick(gfxRing.wptr());
	fenced = gfxFenceWait(gfxFence, 100000);
	const uint32_t ibData = *poolDw(kGfxTestOffset + 4);
	GLOG("IB test: WRITE_DATA from the IB 0x%08x (%s), fence %s", ibData,
	     ibData == 0x1B0B1B0B ? "ok" : "not written", fenced ? "signalled" : "NOT signalled");
	if (!fenced || ibData != 0x1B0B1B0B)
		return finish(false, "IB test");
	gfxFaultMark("IB test");

	// 4. Fenced IBs until the ring has wrapped a few times (64-bit write
	//    pointer, never wrapped by us), each one's own value checked.
	trail("gfx: ring wrap");
	const uint32_t rounds = 3 * gfxRing.sizeDwords() / 12 + 1;   // 12 dwords per round
	uint64_t t0 = mach_absolute_time();
	uint32_t r = 0;
	for (; r < rounds; r++) {
		const uint32_t v = 0x5EED0000u + r;
		ibDw = Pm4::writeData(pkt, poolMc(kGfxTestOffset + 8), v);
		for (uint32_t i = 0; i < ibDw; i++)
			ib[i] = pkt[i];
		flushHdp();
		gfxRing.emit(pkt, Pm4::indirectBufferGfx(pkt, poolMc(kGfxIbOffset), ibDw, 0));
		gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
		gfxKick(gfxRing.wptr());
		if (!gfxFenceWait(gfxFence, 100000) || *poolDw(kGfxTestOffset + 8) != v)
			break;
	}
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	put("WrapRounds", r);
	GLOG("ring wrap: %u of %u fenced IBs right (%llu dwords through a %u-dword ring), %llu us each",
	     r, rounds, static_cast<unsigned long long>(gfxRing.wptr()), gfxRing.sizeDwords(),
	     r ? static_cast<unsigned long long>(ns / 1000 / r) : 0ull);
	if (r != rounds)
		return finish(false, "ring wrap");

	gfxFaultMark("ring wrap");
	GLOG("gfx ring ready (doorbell)");
	return finish(true, "gfx ring");
}

// gfx_v12_0_init_golden_registers (gfx_v12_0.c:3671-3690): the 3D-pipeline
// settings amdgpu writes before it enables the CP and the kext never wrote.
//   golden_settings_gc_12_0 (12.0.0 and 12.0.1): DB_MEM_CONFIG mask 0x8000 = 0x8000;
//   golden_settings_gc_12_0_rev0 (rev_id 0 only): DB_MEM_CONFIG mask 0xf = 0xf,
//   CB_HW_CONTROL_1 mask 0x03000000 = 0x03000000, GL2C_CTRL5 mask 0x70 = 0x20
// (gfx_v12_0.c:253-261). soc15_program_register_sequence: value = old & ~mask |
// (or & mask). amdgpu's rev_id is not the PCI revision byte: nbif_v6_3_1_get_rev_id
// reads STRAP_ATI_REV_ID from RCC_STRAP0_RCC_DEV0_EPF0_STRAP0 [27:24]; that is what
// decides the rev0 set here, with the PCI revision logged next to it. If the strap
// cannot be read only the unconditional set is applied. The round-3 draw (covered 0)
// ran without any of these.
void RDNA4Compute::gfxGoldenInit() {
	// rdna4-gfxgolden=0 leaves them out again: the A/B control for a card that does
	// not draw (the golden set is the fix under test in round 4).
	uint32_t useGolden = 1;
	if (PE_parse_boot_argn("rdna4-gfxgolden", &useGolden, sizeof(useGolden)) && useGolden == 0) {
		// Skipping writes nothing and undoes nothing: these registers keep an earlier
		// boot's values across a warm restart, so this is a control only after a cold
		// power cycle. Say what is in force.
		grbmSelect(0, 0, 0, 0);
		GLOG("golden: skipped by rdna4-gfxgolden=0; in force now: DB_MEM_CONFIG 0x%08x CB_HW_CONTROL_1 0x%08x "
		     "GL2C_CTRL5 0x%08x (a golden value here came from an earlier boot: power-cycle first for a clean control)",
		     rdGc(DbMemConfig), rdGc(CbHwControl1), rdGc(Gl2cCtrl5));
		return;
	}
	const uint32_t strap = rd(IpDiscovery::HwNbif, NbifStrap0);
	const uint32_t strap16 = rd(IpDiscovery::HwNbif, NbifStrap16);
	const uint32_t rev = strap == 0xffffffffu ? 0xff : (strap >> 24) & 0xf;
	const uint32_t pciRev = env.pci ? env.pci->configRead8(kIOPCIConfigRevisionID) : 0xff;
	struct Golden { const char *name; Reg reg; uint32_t mask, value; bool rev0Only; };
	const Golden table[] = {
		{ "DB_MEM_CONFIG",   DbMemConfig,   0x00008000, 0x00008000, false },
		{ "DB_MEM_CONFIG",   DbMemConfig,   0x0000000f, 0x0000000f, true },
		{ "CB_HW_CONTROL_1", CbHwControl1,  0x03000000, 0x03000000, true },
		{ "GL2C_CTRL5",      Gl2cCtrl5,     0x00000070, 0x00000020, true },
	};
	GLOG("golden: NBIF RCC_STRAP0 (0x1c) 0x%08x -> rev_id %u (PCI revision 0x%02x; STRAP16 (0x21) reads 0x%08x): %s",
	     strap, rev, pciRev, strap16,
	     rev == 0 ? "golden_settings_gc_12_0 and _rev0 apply" : "golden_settings_gc_12_0 only");
	trail("gfx: golden registers");
	grbmSelect(0, 0, 0, 0);
	uint32_t changed = 0, seen = 0;
	for (const Golden &g : table) {
		if (g.rev0Only && rev != 0)
			continue;
		const uint32_t before = rdGc(g.reg);
		if (before == 0xffffffff) {
			GLOG("golden: %s unreadable, skipped", g.name);
			continue;
		}
		const uint32_t after = (before & ~g.mask) | (g.value & g.mask);
		seen++;
		changed += after != before;
		wr(IpDiscovery::HwGc, g.reg, after);
		GLOG("golden: %s (mask 0x%08x value 0x%08x): 0x%08x -> 0x%08x, reads back 0x%08x", g.name,
		     g.mask, g.value, before, after, rdGc(g.reg));
	}
	if (seen && !changed)
		GLOG("golden: already in force from an earlier boot (nothing changed): a warm restart keeps them, so "
		     "rdna4-gfxgolden=0 cannot undo this; power-cycle for a clean control");
}

uint32_t RDNA4Compute::requestedGfxDiag() {
	uint32_t mask = 0;
	if (!PE_parse_boot_argn("rdna4-gfxdiag", &mask, sizeof(mask)))
		return 0;
	return mask & 0x1fff;   // bits 1,2,4,8,32,64,128,256,512,1024,2048,4096 = variants, 16 = run them even if the baseline passed
}

namespace {

// The registers the stream writes, by name, for the after-the-draw readback. SH
// registers are GC segment 0 (dword = header value), context registers segment 1;
// all values from gc_12_0_0_offset.h. A register that reads back different from what
// the stream wrote (see gfx12_draw.h) did not latch.
struct StateReg { const char *name; Reg reg; };
const StateReg kShState[] = {
	{ "PGM_LO_ES",  { 0, 0x1a29 } }, { "PGM_HI_ES",  { 0, 0x1a26 } },
	{ "RSRC1_GS",   { 0, 0x1a2a } }, { "RSRC2_GS",   { 0, 0x1a2b } }, { "RSRC4_GS", { 0, 0x1a28 } },
	{ "PGM_LO_PS",  { 0, 0x19a8 } }, { "PGM_HI_PS",  { 0, 0x19a9 } },
	{ "RSRC1_PS",   { 0, 0x19aa } }, { "RSRC2_PS",   { 0, 0x19ab } }, { "RSRC4_PS", { 0, 0x19a7 } },
	{ "USERDATA_PS0", { 0, 0x19ac } }, { "USERDATA_PS1", { 0, 0x19ad } },
};
const StateReg kCtxState[] = {
	{ "VGT_SHADER_STAGES_EN", { 1, 0x02a6 } }, { "VGT_PRIMITIVE_TYPE", { 1, 0x2242 } },
	{ "CB_TARGET_MASK",  { 1, 0x0214 } }, { "CB_SHADER_MASK", { 1, 0x0215 } },
	{ "CB_COLOR_CONTROL", { 1, 0x0216 } }, { "CB_COLOR0_BASE", { 1, 0x0318 } },
	{ "CB_COLOR0_ATTRIB", { 1, 0x031b } }, { "CB_COLOR0_ATTRIB2", { 1, 0x031e } },
	{ "CB_COLOR0_ATTRIB3", { 1, 0x031f } }, { "CB_COLOR0_INFO", { 1, 0x03b0 } },
	{ "SPI_SHADER_POS_FORMAT", { 1, 0x0193 } }, { "SPI_SHADER_COL_FORMAT", { 1, 0x0195 } },
	{ "SPI_PS_INPUT_ENA", { 1, 0x0197 } }, { "SPI_PS_INPUT_ADDR", { 1, 0x0198 } },
	{ "DB_SHADER_CONTROL", { 1, 0x001b } }, { "DB_RENDER_CONTROL", { 1, 0x0000 } },
	{ "PA_CL_VTE_CNTL", { 1, 0x0205 } }, { "PA_SU_SC_MODE_CNTL", { 1, 0x0207 } },
	{ "PA_SC_SCREEN_SCISSOR_TL", { 1, 0x0060 } }, { "PA_SC_SCREEN_SCISSOR_BR", { 1, 0x0061 } },
	{ "PA_SC_VPORT_SCISSOR_0_TL", { 1, 0x0094 } }, { "PA_SC_VPORT_SCISSOR_0_BR", { 1, 0x0095 } },
	{ "GE_POS_RING_BASE", { 1, 0x2268 } }, { "GE_POS_RING_SIZE", { 1, 0x2269 } },
	{ "GE_PRIM_RING_BASE", { 1, 0x226a } }, { "GE_PRIM_RING_SIZE", { 1, 0x226b } },
};

// The packet in `stream` that sets register `reg` (offset within the SET_SH_REG or
// SET_CONTEXT_REG range) and the index of its value, or -1: how the diagnostic
// variants patch one register in the IB copy without regenerating the stream.
int findStreamReg(const uint32_t *stream, uint32_t dwords, uint32_t opcode, uint32_t reg) {
	for (uint32_t i = 0; i < dwords;) {
		const uint32_t h = stream[i];
		if ((h >> 30) != 3) {
			i++;
			continue;
		}
		const uint32_t op = (h >> 8) & 0xff, count = ((h >> 16) & 0x3fff) + 1;
		if (op == opcode && i + 1 + count <= dwords) {
			const uint32_t start = stream[i + 1];
			for (uint32_t k = 0; k + 1 < count; k++)
				if (start + k == reg)
					return static_cast<int>(i + 2 + k);
		}
		i += 1 + count;
	}
	return -1;
}

constexpr uint32_t kOpSetShReg = 0x76, kOpSetContextReg = 0x69;
constexpr uint32_t kShBase = 0x19a0, kMarkerValue = 0xc0de0001;

// The state the CP itself holds, read back with COPY_DATA on the ring (rdna4-gfxprobe=1). MMIO reads
// of context registers are NOT reliable evidence: on the card the first draw's readback showed random
// values and the next draw's showed exactly what the stream had written, i.e. the MMIO view lags the
// context the CP is using. A COPY_DATA (gfx_v12_0_ring_emit_rreg, gfx_v12_0.c:4697) is executed by the
// CP in stream order, so it reports the state at that point. kind 0 = context register (SET_CONTEXT_REG
// offset = the header dword), 1 = SH register (offset = dword - kShBase), 2 = UCONFIG (dword - 0x2000).
struct ProbeReg { const char *name; uint8_t seg; uint16_t dword; uint8_t kind; uint8_t csb; };   // csb = a clear-state-extent register (W37)
constexpr uint32_t kProbeMax = 32;
const ProbeReg kProbe[] = {
	{ "VGT_SHADER_STAGES_EN", 1, 0x02a6, 0 }, { "CB_TARGET_MASK", 1, 0x0214, 0 }, { "CB_SHADER_MASK", 1, 0x0215, 0 },
	{ "CB_COLOR_CONTROL", 1, 0x0216, 0 }, { "CB_COLOR0_BASE", 1, 0x0318, 0 }, { "CB_COLOR0_ATTRIB2", 1, 0x031e, 0 },
	{ "CB_COLOR0_ATTRIB3", 1, 0x031f, 0 }, { "CB_COLOR0_INFO", 1, 0x03b0, 0 }, { "SPI_SHADER_POS_FORMAT", 1, 0x0193, 0 },
	{ "SPI_SHADER_COL_FORMAT", 1, 0x0195, 0 }, { "SPI_PS_INPUT_ENA", 1, 0x0197, 0 }, { "DB_SHADER_CONTROL", 1, 0x001b, 0 },
	{ "PA_CL_VTE_CNTL", 1, 0x0205, 0 }, { "PA_SC_SCREEN_SCISSOR_BR", 1, 0x0061, 0 }, { "PA_SC_VPORT_SCISSOR_0_BR", 1, 0x0095, 0 },
	{ "PGM_LO_ES", 0, 0x1a29, 1 }, { "PGM_LO_PS", 0, 0x19a8, 1 }, { "RSRC1_GS", 0, 0x1a2a, 1 }, { "RSRC1_PS", 0, 0x19aa, 1 },
	{ "VGT_PRIMITIVE_TYPE", 1, 0x2242, 2 }, { "GE_POS_RING_BASE", 1, 0x2268, 2 }, { "GE_PRIM_RING_BASE", 1, 0x226a, 2 },
	// W37: registers of the clear-state extents that the stream never writes (rootcause-draw.md #1). Zero in the CP's
	// view = the clear state landed (or the replay wrote it); non-zero = power-up garbage.
	{ "PA_RATE_CNTL", 1, 0x00cd, 0, 1 }, { "CONTEXT_RESERVED_REG0", 1, 0x00db, 0, 1 }, { "CONTEXT_RESERVED_REG1", 1, 0x00dc, 0, 1 },
	{ "PA_SC_CLIPRECT_0_EXT", 1, 0x00dd, 0, 1 }, { "PA_SC_BINNER_OUTPUT_TIMEOUT_CNTL", 1, 0x02ed, 0, 1 },
	{ "PA_SC_BINNER_DYNAMIC_BATCH_LIMIT", 1, 0x02ee, 0, 1 }, { "PA_SC_VPORT_0_TL", 1, 0x0040, 0, 1 },
	{ "PA_SC_RASTER_CONFIG", 1, 0x00d4, 0, 0 },
};
constexpr uint32_t kProbeCount = sizeof(kProbe) / sizeof(kProbe[0]);
static_assert(kProbeCount <= kProbeMax, "probe table larger than its buffer");

uint32_t probeOpcode(const ProbeReg &p) { return p.kind == 0 ? 0x69 : p.kind == 1 ? 0x76 : 0x79; }
uint32_t probeOffset(const ProbeReg &p) {
	return p.kind == 0 ? p.dword : p.kind == 1 ? p.dword - 0x19a0u : p.dword - 0x2000u;
}

// The first packet of `opcode` in the stream (dword index), or 0.
uint32_t findStreamPacket(const uint32_t *stream, uint32_t dwords, uint32_t opcode) {
	for (uint32_t i = 0; i < dwords;) {
		const uint32_t h = stream[i];
		if ((h >> 30) != 3) {
			i++;
			continue;
		}
		if (((h >> 8) & 0xff) == opcode)
			return i;
		i += 1 + ((h >> 16) & 0x3fff) + 1;
	}
	return 0;
}

} // namespace

uint32_t RDNA4Compute::requestedGfxProbe() {
	uint32_t v = 0;
	return PE_parse_boot_argn("rdna4-gfxprobe", &v, sizeof(v)) && v ? 1 : 0;
}

// rdna4-gfxclient=1 (default off): after a passing G3 baseline, submit a synthetic client gfx IB through the W12k path (runtime.cpp).
bool RDNA4Compute::requestedGfxClient() {
	uint32_t v = 0;
	return PE_parse_boot_argn("rdna4-gfxclient", &v, sizeof(v)) && v;
}

uint32_t RDNA4Compute::requestedGfxCol() {
	uint32_t v = 0;
	return PE_parse_boot_argn("rdna4-gfxcol", &v, sizeof(v)) && v ? 1 : 0;
}

// One COPY_DATA per probed register into the result buffer at `poolOff` (slot i = kProbe[i]).
void RDNA4Compute::gfxEmitProbe(uint32_t poolOff) {
	uint32_t pkt[16];
	for (uint32_t i = 0; i < kProbeCount; i++) {
		uint32_t byteOff = 0;
		if (!env.disc || !env.disc->regByteOffset(IpDiscovery::HwGc, 0, kProbe[i].seg, kProbe[i].dword, byteOff))
			continue;   // the slot keeps its sentinel
		gfxRing.emit(pkt, Pm4::copyDataRegToMem(pkt, byteOff / 4, poolMc(poolOff + 4 * i)));
	}
}

// Log what the CP reported and how many registers equal what the stream wrote (the IB copy `ib`).
void RDNA4Compute::gfxProbeReport(const char *label, uint32_t poolOff, volatile uint32_t *ib, uint32_t *equalOut,
                                  uint32_t *countedOut) {
	constexpr uint32_t n = sizeof(Gfx12Draw::kStream) / 4;
	char line[240], diff[300];
	uint32_t len = 0, match = 0, counted = 0, ndiff = 0, dl = 0, csbTotal = 0, csbNz = 0, cl = 0;
	char csbNames[200];
	csbNames[0] = 0;
	line[0] = diff[0] = '\0';
	for (uint32_t i = 0; i < kProbeCount; i++) {
		const uint32_t val = *poolDw(poolOff + 4 * i);
		if (kProbe[i].csb) {   // W37 #1: a clear-state register the stream never writes
			csbTotal++;
			if (val) {
				csbNz++;
				if (cl < sizeof(csbNames) - 50)
					cl += snprintf(csbNames + cl, sizeof(csbNames) - cl, "%s=0x%08x ", kProbe[i].name, val);
			}
		}
		len += snprintf(line + len, sizeof(line) - len, "%s=0x%08x ", kProbe[i].name, val);
		if (len > 150) {
			GLOG("%s: CP view: %s", label, line);
			len = 0;
			line[0] = '\0';
		}
		const int at = findStreamReg(Gfx12Draw::kStream, n, probeOpcode(kProbe[i]), probeOffset(kProbe[i]));
		if (at < 0)
			continue;
		counted++;
		const uint32_t want = ib[at];
		if (val == want) {
			match++;
		} else if (ndiff++ < 8 && dl < sizeof(diff) - 60) {
			dl += snprintf(diff + dl, sizeof(diff) - dl, "%s got 0x%08x want 0x%08x; ", kProbe[i].name, val, want);
		}
	}
	if (len)
		GLOG("%s: CP view: %s", label, line);
	GLOG("%s: CP view: %u of %u registers equal what the stream wrote%s%s", label, match, counted,
	     ndiff ? "; differ: " : "", diff);
	GLOG("%s: CP view: %u of %u clear-state registers (never written by the stream) are non-zero%s%s%s", label, csbNz, csbTotal,
	     csbNz ? ": " : " (the clear state landed or the replay wrote it)", csbNames, csbNz ? " (power-up garbage: #1)" : "");
	if (equalOut)
		*equalOut = match;
	if (countedOut)
		*countedOut = counted;
}

// W33 S1 (W31 review): which packet makes the CPG read and write VA 0? Round 4 latched GC hub fault 0x0d3d (CPG, VA 0)
// before the draw and the IH ring (which would have queued an IV per fault) only comes up after the ring tests. So the
// ring is exercised once more with ONE packet per submission, a settle time, and a fault mark after each: NOP (does the
// CP fault by merely fetching a packet?), WRITE_DATA to memory, RELEASE_MEM (the end-of-pipe fence: the draw's and the
// tests' terminator, a read+write pair recurs with every submission that carries one), ACQUIRE_MEM (the GL2 write-back
// flush the draw path issues). Runs only with rdna4-gfxprobe=1, right after the ring test; the mark before the first
// packet clears whatever the ring test left, so the first line that reports a fault names the packet.
void RDNA4Compute::gfxPacketProbe() {
	auto idle = [&]() {
		const uint32_t mask = gfxRing.sizeDwords() - 1;
		for (uint32_t us = 0; us < 20000; us += 10) {
			if ((rdGc(CpRb0Rptr) & mask) == (gfxRing.wptr() & mask))
				return true;
			IODelay(10);
		}
		return false;
	};
	uint32_t pkt[16];
	gfxFaultMark("single-packet probe start");
	for (uint32_t step = 0; step < 4; step++) {
		const char *name = "";
		bool waitFence = false, dataCheck = false;
		switch (step) {
		case 0:
			name = "NOP";
			gfxRing.emit(pkt, Pm4::nop(pkt));
			break;
		case 1:
			name = "WRITE_DATA";
			*poolDw(kGfxTestOffset + 0x10) = 0;
			flushHdp();
			gfxRing.emit(pkt, Pm4::writeData(pkt, poolMc(kGfxTestOffset + 0x10), 0x57ee1e57));
			dataCheck = true;
			break;
		case 2:
			name = "RELEASE_MEM";
			*poolDw(kGfxFenceOffset) = 0;
			flushHdp();
			gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
			waitFence = true;
			break;
		default:
			name = "ACQUIRE_MEM";
			gfxRing.emit(pkt, Pm4::acquireMem(pkt, Pm4::kGcrMemSync));
			break;
		}
		gfxKick(gfxRing.wptr());
		bool done = waitFence ? gfxFenceWait(gfxFence, 100000) : idle();
		IOSleep(2);   // a fault is raised asynchronously: let it latch before reading the status
		char tag[48];
		snprintf(tag, sizeof(tag), "single %s", name);
		GLOG("single-packet probe %s: %s, rptr 0x%x wptr %llu%s", name, done ? "ring idle / fence signalled" : "NOT idle",
		     rdGc(CpRb0Rptr), static_cast<unsigned long long>(gfxRing.wptr()),
		     dataCheck ? (*poolDw(kGfxTestOffset + 0x10) == 0x57ee1e57 ? ", data written" : ", data NOT written") : "");
		gfxFaultMark(tag);
		if (!done) {   // W35 (W33 review S1): the first unfinished step ends the probe, on ANY packet (RELEASE_MEM included)
			GLOG("single-packet probe: %s did not finish; the probe stops here (a CP that does not finish needs a cold power cycle before the next boot)", name);
			break;
		}
	}
}

// W33 S2 (W31 review): the CP-side readback is decisive only if a COPY_DATA of a context register observes a context
// write made just before it. Write sentinel A to a context register the draw stream sets itself afterwards (CB_SHADER_MASK,
// so no state is left behind), read it back on the ring, write sentinel B, read again, and read once over MMIO for
// comparison. Reads: A then B = the CP-side read tracks writes; A then A = it lags one write (the MMIO lag, so 'probe mid'
// of the FIRST draw cannot be trusted); anything else = it does not observe context writes at all.
bool RDNA4Compute::gfxSentinelCheck() {
	constexpr uint32_t kCtxOffset = 0x0215;   // CB_SHADER_MASK, set again by the draw stream
	uint32_t byteOff = 0;
	if (!env.disc || !env.disc->regByteOffset(IpDiscovery::HwGc, 0, 1, kCtxOffset, byteOff)) {
		GLOG("sentinel: cannot locate the context register, skipped");
		return true;   // nothing was submitted: the draw may go on
	}
	// W35 (W33 review NIT): the value the register holds before A is written is logged, and the sentinels are
	// chosen so that neither equals it (a warm state holding a sentinel would make the verdicts collide).
	const uint32_t before = rdGc(Reg { 1, kCtxOffset });
	uint32_t kA = 0x0a5a5a5au, kB = 0x05a5a5a5u;
	if (before == kA || before == kB) {
		kA = 0x0c3c3c3cu;
		kB = 0x03c3c3c3u;
	}
	GLOG("sentinel: CB_SHADER_MASK holds 0x%08x before the check", before);
	for (uint32_t i = 0; i < 4; i++)
		*poolDw(kGfxProbeOffset + 4 * i) = 0xdeadf00du;
	flushHdp();
	uint32_t pkt[16];
	const uint32_t vals[2] = { kA, kB };
	for (uint32_t i = 0; i < 2; i++) {
		gfxRing.emit(pkt, Pm4::setContextReg(pkt, kCtxOffset, vals[i]));
		gfxRing.emit(pkt, Pm4::copyDataRegToMem(pkt, byteOff / 4, poolMc(kGfxProbeOffset + 4 * i)));
	}
	gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
	gfxKick(gfxRing.wptr());
	const bool fenced = gfxFenceWait(gfxFence, 200000);
	const uint32_t r0 = *poolDw(kGfxProbeOffset), r1 = *poolDw(kGfxProbeOffset + 4);
	const uint32_t mmio = rdGc(Reg { 1, kCtxOffset });
	const char *verdict = !fenced                     ? "the ring did not finish"
	                    : (r0 == kA && r1 == kB)      ? "the CP-side read TRACKS context writes (probe mid is trustworthy)"
	                    : (r0 != kA && r1 == kA)      ? "the CP-side read LAGS one write like MMIO (probe mid of the first draw is stale, do not read it as 'not applied')"
	                    : (r0 == r1)                  ? "the CP-side read does NOT observe context writes (same value twice)"
	                                                  : "unexpected pattern";
	GLOG("sentinel: wrote 0x%08x then 0x%08x to CB_SHADER_MASK; CP-side reads 0x%08x then 0x%08x, MMIO after 0x%08x: %s",
	     kA, kB, r0, r1, mmio, verdict);
	gfxFaultMark("sentinel check");
	return fenced;
}

// ---------------------------------------------------------------------------
// W37 (premetal/rootcause-draw.md): the context state, the queue, and where the primitive is lost.
// ---------------------------------------------------------------------------

// W41: the VA-0 mapping is its own opt-in (rdna4-gfxva0=1), not part of rdna4-gfxprobe: it changes what the CPG can fault on,
// so it confounds the baseline draw and stays out of boots 3 and 6.
bool RDNA4Compute::requestedGfxVa0() {
	uint32_t v = 0;
	return PE_parse_boot_argn("rdna4-gfxva0", &v, sizeof(v)) && v == 1;
}

bool RDNA4Compute::requestedGfxSrm() {
	uint32_t v = 1;
	return !(PE_parse_boot_argn("rdna4-gfxsrm", &v, sizeof(v)) && v == 0);
}

bool RDNA4Compute::requestedGfxCsbReplay() {
	uint32_t v = 1;
	return !(PE_parse_boot_argn("rdna4-gfxcsb", &v, sizeof(v)) && v == 0);
}

// rootcause-draw.md #1. With PSP-loaded firmware amdgpu's gfx_v12_0_rlc_resume runs gfx_v12_0_init_csb AND
// gfx_v12_0_rlc_enable_srm (RLC_SRM_CNTL |= AUTO_INCR_ADDR | SRM_ENABLE, gfx_v12_0.c:2005-2008, called at :2106-2112).
// The kext handed the RLC the clear-state buffer and never enabled SRM, so the clear state may never have landed:
// the context registers power up as SRAM garbage (the round-4 readback VGT_SHADER_STAGES_EN=0xfd1ffe88 has bits
// where gfx12 defines no field, different each boot) and unwritten ones keep it. Same point of the bring-up as
// amdgpu: right after the CSB init. rdna4-gfxsrm=0 leaves it out (the control).
void RDNA4Compute::gfxSrmEnable() {
	const uint32_t before = rdGc(RlcSrmCntl);
	// W39 (W37 review S3b): RLC state survives a warm restart (like CG), so what the register holds at the start of this boot is
	// evidence of its own: SRM_ENABLE already set means an earlier boot's enable persisted.
	GLOG("SRM: RLC_SRM_CNTL found at start 0x%08x%s", before, (before & kRlcSrmEnable) && before != 0xffffffffu
	     ? " (SRM_ENABLE already set: persisted from an earlier boot; a rdna4-gfxsrm=0 control is void, power-cycle first)" : "");
	if (!requestedGfxSrm()) {
		GLOG("SRM: rdna4-gfxsrm=0, RLC_SRM_CNTL left at 0x%08x (control)", before);
		return;
	}
	if (before == 0xffffffffu) {
		GLOG("SRM: RLC_SRM_CNTL unreadable (0x%08x), not written", before);
		return;
	}
	wr(IpDiscovery::HwGc, RlcSrmCntl, before | kRlcSrmAutoIncr | kRlcSrmEnable);
	GLOG("SRM: RLC_SRM_CNTL 0x%08x -> 0x%08x (gfx_v12_0_rlc_enable_srm: AUTO_INCR_ADDR | SRM_ENABLE)", before,
	     rdGc(RlcSrmCntl));
}

// Belt and braces for #1: the clear-state buffer's six extents (62 registers, 74 dwords with the headers) as
// SET_CONTEXT_REG on the ring, right before the draw, with the CSB's own values (gfx12_cs_data: all zero), so the
// draw never depends on SRM. They are registers Mesa never writes (PA_RATE_CNTL, CONTEXT_RESERVED_REG0/1,
// PA_SC_BINNER_*, PA_SC_VPORT_0..15, PA_SC_CLIPRECT_n_EXT, the HiZ/HiS base and size registers); the stream's own
// later writes (SC_MEM_*, HIZ/HIS_INFO, ...) override the overlaps. rdna4-gfxcsb=0 leaves it out (the control).
void RDNA4Compute::gfxEmitCsbReplay() {
	uint32_t buf[64], extents = 0, regs = 0, dwords = 0;
	for (const cs_section_def *sect = gfx12_cs_data; sect->section; sect++) {
		if (sect->id != SECT_CONTEXT)
			continue;
		for (const cs_extent_def *ext = sect->section; ext->extent; ext++) {
			if (ext->reg_count + 2 > sizeof(buf) / sizeof(buf[0]) || ext->reg_index < 0xa000)
				continue;
			const uint32_t n = Pm4::setContextRegs(buf, ext->reg_index - 0xa000, ext->extent, ext->reg_count);
			gfxRing.emit(buf, n);
			extents++;
			regs += ext->reg_count;
			dwords += n;
		}
	}
	GLOG("CSB replay: %u extents, %u registers, %u ring dwords of SET_CONTEXT_REG before the draw", extents, regs, dwords);
}

// The registers of the clear-state extents that the round-4 doc singles out, then every other context register
// (seg 1, dwords 0..0x3ff) that is non-zero although the draw stream never writes it: on silicon those are the
// power-up garbage (or what a context roll copied forward). '*' marks a register of the clear-state extents.
void RDNA4Compute::gfxContextDump(const char *tag) {
	struct Named { const char *name; uint32_t dword; };
	static const Named named[] = {
		{ "PA_RATE_CNTL", 0x00cd }, { "CONTEXT_RESERVED_REG0", 0x00db }, { "CONTEXT_RESERVED_REG1", 0x00dc },
		{ "PA_SC_CLIPRECT_0_EXT", 0x00dd }, { "PA_SC_BINNER_OUTPUT_TIMEOUT_CNTL", 0x02ed },
		{ "PA_SC_BINNER_DYNAMIC_BATCH_LIMIT", 0x02ee }, { "PA_SC_VPORT_0_TL", 0x0040 }, { "PA_SC_RASTER_CONFIG", 0x00d4 },
	};
	char line[240];
	uint32_t n = 0;
	line[0] = '\0';
	for (const Named &r : named) {
		n += snprintf(line + n, sizeof(line) - n, "%s=0x%08x ", r.name, rdGc(Reg { 1, r.dword }));
		if (n > 150) {
			GLOG("%s: clear-state registers: %s", tag, line);
			n = 0;
			line[0] = '\0';
		}
	}
	if (n)
		GLOG("%s: clear-state registers: %s", tag, line);
	constexpr uint32_t sn = sizeof(Gfx12Draw::kStream) / 4;
	uint32_t nonzero = 0, csbNonzero = 0, shown = 0;
	n = 0;
	line[0] = '\0';
	for (uint32_t off = 0; off < 0x400; off++) {
		if (findStreamReg(Gfx12Draw::kStream, sn, 0x69, off) >= 0)
			continue;   // the stream writes it: not garbage
		const uint32_t v = rdGc(Reg { 1, off });
		if (!v || v == 0xffffffffu)
			continue;
		bool inCsb = false;
		for (const cs_section_def *sect = gfx12_cs_data; sect->section; sect++)
			for (const cs_extent_def *ext = sect->section; ext->extent; ext++)
				if (off + 0xa000 >= ext->reg_index && off + 0xa000 < ext->reg_index + ext->reg_count)
					inCsb = true;
		nonzero++;
		csbNonzero += inCsb;
		if (shown < 48) {
			shown++;
			n += snprintf(line + n, sizeof(line) - n, "%s0x%03x=0x%08x ", inCsb ? "*" : "", off, v);
			if (n > 170) {
				GLOG("%s: context registers the stream never writes, non-zero: %s", tag, line);
				n = 0;
				line[0] = '\0';
			}
		}
	}
	if (n)
		GLOG("%s: context registers the stream never writes, non-zero: %s", tag, line);
	GLOG("%s: %u context registers the stream never writes read non-zero (%u of them in the clear-state extents)%s",
	     tag, nonzero, csbNonzero, shown < nonzero ? "; first 48 shown" : "");
}

// rootcause-draw.md #2: the registers a gfx queue would get from an MQD (gfx_v12_0_gfx_mqd_init) and the
// RS64 local base. amdgpu never runs gfx12 on a bare RB0: its kernel queue has an MQD/HQD. Read-only; the ones
// that read 0 are the candidates for what the CPG resolves at VA 0.
void RDNA4Compute::gfxQueueEvidence(const char *tag) {
	grbmSelect(0, 0, 0, 0);
	GLOG("%s: queue: CP_GFX_MQD_BASE 0x%08x_%08x CP_MQD_BASE 0x%08x_%08x CP_GFX_MQD_CONTROL 0x%08x HPD_OSPRE_FENCE 0x%08x_%08x",
	     tag, rdGc(CpGfxMqdBaseHi), rdGc(CpGfxMqdBaseLo), rdGc(CpMqdBaseAddrHi), rdGc(CpMqdBaseAddr),
	     rdGc(CpGfxMqdControl), rdGc(CpGfxHpdOspreFenceHi), rdGc(CpGfxHpdOspreFenceLo));
	GLOG("%s: queue: HQD ACTIVE 0x%08x VMID 0x%08x BASE 0x%08x_%08x CNTL 0x%08x RPTR 0x%08x RPTR_ADDR 0x%08x_%08x "
	     "WPTR 0x%08x_%08x HQ_STATUS0 0x%08x HQ_CONTROL0 0x%08x | RS64 LOCAL_BASE0_LO 0x%08x", tag,
	     rdGc(CpGfxHqdActive), rdGc(CpGfxHqdVmid), rdGc(CpGfxHqdBaseHi), rdGc(CpGfxHqdBase), rdGc(CpGfxHqdCntl),
	     rdGc(CpGfxHqdRptr), rdGc(CpGfxHqdRptrAddrHi), rdGc(CpGfxHqdRptrAddr), rdGc(CpGfxHqdWptrHi),
	     rdGc(CpGfxHqdWptr), rdGc(CpGfxHqdHqStatus0), rdGc(CpGfxHqdHqControl0), rdGc(CpGfxRs64LocalBase0Lo));
}

// rootcause-draw.md #2 test: VMID0 VA 0 (context 0, page 0: a one-page table, PTE 0 = invalid, faults to the
// shared scratch page) becomes a private zeroed VRAM page (VALID | READABLE | WRITEABLE, amdgpu_vm.h:57,67,68),
// as amdgpu's GART would be there. The CPG's stray access at VA 0 (round 4: status 0x0d3d, a read and a write
// IV pair) then lands in a page we can dump after the draw: the dwords it writes fingerprint the structure.
// rdna4-gfxva0=1 only (its own opt-in since W41: it confounds the baseline draw), right before the baseline draw, after the single-packet probe has looked at
// the faults (mapping the page makes the fault disappear).
void RDNA4Compute::gfxMapVa0() {
	for (uint32_t i = 0; i < 1024; i++)
		*poolDw(kGfxVa0Offset + 4 * i) = 0;
	const uint64_t phys = static_cast<uint64_t>(rdGc(GcFbOffset) & 0xffffff) << 24;
	const uint64_t page = phys + pool.offset + kGfxVa0Offset;
	// W39 (W37 review S1): VALID | READABLE | WRITEABLE | IS_PTE. On GC 12 a last-level entry WITHOUT IS_PTE (bit 63) is a
	// directory entry to the walker (W36), so the mapping would silently do nothing and the dump would read 'nothing wrote there'.
	const uint64_t pte = page | 0x61 | GpuVm::kIsPte;
	*poolDw(kPtOffset) = static_cast<uint32_t>(pte);
	*poolDw(kPtOffset + 4) = static_cast<uint32_t>(pte >> 32);
	flushHdp();
	const Reg req { 0, GcInvEng0Req.dword + kGcInvEngGart }, ack { 0, GcInvEng0Ack.dword + kGcInvEngGart };
	wr(IpDiscovery::HwGc, req, kInvReqVmid0);
	bool acked = false;
	for (uint32_t us = 0; us < 20000 && !acked; us += 10) {
		acked = rdGc(ack) & 1u;
		if (!acked)
			IODelay(10);
	}
	GLOG("VA 0: VMID0 page 0 mapped to a private zeroed page at MC 0x%llx (PTE 0x%016llx, IS_PTE set); GC TLB flush %s. The mapping stays for the rest of "
	     "the boot: later fault marks no longer show a VA-0 fault, so this boot is not a clean baseline for the fault status",
	     poolMc(kGfxVa0Offset), static_cast<unsigned long long>(pte),
	     acked ? "acked" : "NOT acked: the card never acks these and the ack is not required (a stale invalid entry in the TLB could still hide the mapping: read a silent VA-0 fault as 'the mapping took effect')");
}

void RDNA4Compute::gfxDumpVa0(const char *tag) {
	uint32_t nonzero = 0, shown = 0, n = 0;
	char line[240];
	line[0] = '\0';
	for (uint32_t i = 0; i < 1024; i++) {
		const uint32_t v = *poolDw(kGfxVa0Offset + 4 * i);
		if (!v)
			continue;
		nonzero++;
		if (shown < 24) {
			shown++;
			n += snprintf(line + n, sizeof(line) - n, "[%u]=0x%08x ", i, v);
			if (n > 170) {
				GLOG("%s: VA 0 page: %s", tag, line);
				n = 0;
				line[0] = '\0';
			}
		}
	}
	if (n)
		GLOG("%s: VA 0 page: %s", tag, line);
	GLOG("%s: VA 0 page: %u of 1024 dwords are non-zero%s", tag, nonzero,
	     nonzero ? " (something wrote there, if the mapping took effect: see the VA 0 line and the fault marks)" : " (nothing wrote there, if the mapping took effect)");
}

// rootcause-draw.md #4: SAMPLE_PIPELINESTAT before and after the draw (si_query.c:854-857; 14 x u64, dword
// offsets si_query_pipestat_dw_offset: PS_INVOCATIONS 0, C_PRIMITIVES 2, C_INVOCATIONS 4, VS 6, GS_INV 8, GS_PRIM 10,
// IA_PRIMITIVES 12, IA_VERTICES 14). Reading it: IA = 0: the GE dropped the draw; IA > 0 and C_INV = 0: the NGG /
// export path; C_INV > 0 and C_PRIM = 0: clip or cull; C_PRIM > 0 and PS = 0: SC / scissor / raster state (#1);
// PS > 0: pixel shaders ran. C_PRIMITIVES is supporting evidence only: on the card a good triangle reads 4, the old s_and_saveexec
// shader read 0 in 3/3 runs, but one failing draw (wave64, VGPRS 0, 0 px) read 0, 0, 4, 0, 0, 8 in six runs. The signal is PS_INVOCATIONS
// and the pixels; the "where" line below names a stage from the counters, it does not prove it.
void RDNA4Compute::gfxPstatReport(const char *label) {
	auto q = [&](uint32_t base, uint32_t idx, bool &written) {
		const uint32_t lo = *poolDw(base + 8 * idx), hi = *poolDw(base + 8 * idx + 4);
		written = lo != 0xdeadf00du;
		return (static_cast<uint64_t>(hi) << 32) | lo;
	};
	static const struct { const char *name; uint32_t idx; } stat[] = {
		{ "PS_INVOCATIONS", 0 }, { "C_PRIMITIVES", 1 }, { "C_INVOCATIONS", 2 }, { "VS_INVOCATIONS", 3 },
		{ "GS_INVOCATIONS", 4 }, { "GS_PRIMITIVES", 5 }, { "IA_PRIMITIVES", 6 }, { "IA_VERTICES", 7 },
	};
	uint64_t d[8];
	bool any = false, allWritten = true;
	char line[240];
	uint32_t n = 0;
	line[0] = '\0';
	for (uint32_t i = 0; i < 8; i++) {
		bool w0, w1;
		const uint64_t a = q(kGfxPstatPre, stat[i].idx, w0), b = q(kGfxPstatPost, stat[i].idx, w1);
		allWritten = allWritten && w0 && w1;
		d[i] = b - a;
		any = any || d[i];
		n += snprintf(line + n, sizeof(line) - n, "%s %llu (%llu->%llu) ", stat[i].name,
		              static_cast<unsigned long long>(d[i]), static_cast<unsigned long long>(a),
		              static_cast<unsigned long long>(b));
		if (n > 150) {
			GLOG("%s: pipeline statistics: %s", label, line);
			n = 0;
			line[0] = '\0';
		}
	}
	if (n)
		GLOG("%s: pipeline statistics: %s", label, line);
	for (uint32_t i = 0; i < 8; i++)
		gfxPstatLast[i] = d[i];
	const uint64_t ps = d[0], cprim = d[1], cinv = d[2], iaPrim = d[6], iaVert = d[7];
	const char *where = !allWritten ? "the SAMPLE_PIPELINESTAT events wrote nothing (counters unavailable on this path)"
	                  : (!iaVert && !iaPrim) ? "IA = 0: the GE dropped the draw (queue mode / VA 0, #2)"
	                  : !cinv ? "IA > 0 but C_INVOCATIONS = 0: lost in the NGG / export path (#4, #3)"
	                  : !cprim ? "C_INVOCATIONS > 0 but C_PRIMITIVES 0 (not reliable alone: check PS_INVOCATIONS and the pixels): maybe clipped or culled (context state, #1)"
	                  : !ps ? "C_PRIMITIVES > 0 but PS_INVOCATIONS = 0 (C_PRIMITIVES is not reliable alone): maybe lost between the clipper and the pixel shader (SC / raster state, #1)"
	                  : "pixel shaders ran";
	GLOG("%s: pipeline statistics say: %s%s", label, where, any ? "" : " (all deltas 0)");
}

// W39 (W37 review S3): the clear-state evidence BEFORE the CSB replay is emitted, once per boot, so that SRM's effect is
// attributable: with the replay on, the CP-view read after it (draw probe mid) reads 0 whether or not SRM landed the clear
// state. Boot 3 (SRM + replay) reads this before the replay; boot 6 (rdna4-gfxcsb=0) has no replay at all, so this is the
// state its draw sees. COPY_DATA on the ring plus an MMIO read of the same registers.
void RDNA4Compute::gfxClearStatePre() {
	uint32_t regs[16], k = 0;
	for (uint32_t i = 0; i < kProbeCount && k < 16; i++)
		if (kProbe[i].csb)
			regs[k++] = i;
	for (uint32_t i = 0; i < k; i++)
		*poolDw(kGfxProbeOffset + 4 * i) = 0xdeadf00du;
	flushHdp();
	uint32_t pkt[16];
	for (uint32_t i = 0; i < k; i++) {
		uint32_t byteOff = 0;
		if (env.disc && env.disc->regByteOffset(IpDiscovery::HwGc, 0, kProbe[regs[i]].seg, kProbe[regs[i]].dword, byteOff))
			gfxRing.emit(pkt, Pm4::copyDataRegToMem(pkt, byteOff / 4, poolMc(kGfxProbeOffset + 4 * i)));
	}
	gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
	gfxKick(gfxRing.wptr());
	const bool fenced = gfxFenceWait(gfxFence, 200000);
	char cp[300], mm[300];
	uint32_t cl = 0, ml = 0, cpNz = 0, mmNz = 0;
	cp[0] = mm[0] = '\0';
	for (uint32_t i = 0; i < k; i++) {
		const uint32_t v = *poolDw(kGfxProbeOffset + 4 * i);
		const uint32_t m = rdGc(Reg { kProbe[regs[i]].seg, kProbe[regs[i]].dword });
		cpNz += v != 0;
		mmNz += m != 0;
		if (cl < sizeof(cp) - 48)
			cl += snprintf(cp + cl, sizeof(cp) - cl, "%s=0x%08x ", kProbe[regs[i]].name, v);
		if (ml < sizeof(mm) - 48)
			ml += snprintf(mm + ml, sizeof(mm) - ml, "0x%08x ", m);
	}
	const uint32_t srm = rdGc(RlcSrmCntl);
	GLOG("clear state BEFORE any CSB replay (RLC_SRM_CNTL now 0x%08x%s): CP view %u of %u non-zero: %s", srm,
	     fenced ? "" : ", the ring did not finish", cpNz, k, cp);
	GLOG("clear state BEFORE any CSB replay: MMIO %u of %u non-zero: %s", mmNz, k, mm);
	GLOG("clear state BEFORE any CSB replay says: %s", cpNz == 0
	     ? "all zero: the clear state is already applied (SRM landed it, or the registers reset to 0)"
	     : "power-up garbage: the clear state did NOT land from the CSB handoff / SRM enable alone (#1: the replay or another trigger is needed)");
}

// W42 (hub-task-274): what Linux amdgpu (kernel 7.2.2, this RX 9070 XT, E:\linux\rdna4-groundtruth-{idle,vkcube}-*\gc-regs.txt)
// reads in the registers that decide VM translation, the gfx queue, the RLC and the CP, compared with ours at the same
// point. Read-only. `mask` limits the comparison to bits that are meaningful (ring size, addresses and doorbell index differ by design);
// `why` says what a known difference is. Logged once per gfx boot, right before the draw, as `linux diff: DIFF name ours 0x.. linux 0x..`.
void RDNA4Compute::gfxLinuxDiff(const char *tag) {
	grbmSelect(0, 0, 0, 0);   // VMID 0 for the banked SH_MEM_*
	uint32_t compared = 0, differ = 0;
	// One GLOG per register (W43, review BLOCKER): no accumulating buffer, so no length arithmetic to get wrong;
	// LinuxRefTable::format is bounded (host-tested with worst-case 'why' strings).
	for (const LinuxRefTable::Ref &r : LinuxRefTable::kRefs) {
		const uint32_t v = rdGc(r.reg);
		compared++;
		if ((v & r.mask) == (r.linuxValue & r.mask))
			continue;
		differ++;
		char line[LinuxRefTable::kLineMax];
		LinuxRefTable::format(line, sizeof(line), r, v);
		GLOG("%s: linux diff: %s", tag, line);
	}
	GLOG("%s: linux diff: %u registers compared with the Linux 7.2.2 capture (vkcube), %u differ (the ones listed above)", tag, compared, differ);
}

// ---------------------------------------------------------------------------
// W45: the clip / cull / viewport / scissor state of the first draw (round 5 boot 3 on the card: IA_PRIMITIVES 1, VS_INVOCATIONS 3,
// C_INVOCATIONS 1, C_PRIMITIVES 0, PS_INVOCATIONS 0, the NGG wave ran: the primitive is lost at or after the clipper; the root cause turned out to
// be the shader's EXEC, docs/linux-replay.md. C_PRIMITIVES alone is not a reliable signature, PS_INVOCATIONS and the pixels are).
// ---------------------------------------------------------------------------

// rdna4-gfxsane=0 leaves the explicit "sane clip state" block out (the A/B control).
bool RDNA4Compute::requestedGfxSane() {
	uint32_t v = 1;
	return !(PE_parse_boot_argn("rdna4-gfxsane", &v, sizeof(v)) && v == 0);
}

// The front-end registers the draw stream (Mesa's gfx12 preamble + one triangle) leaves at whatever the context holds: the implicit
// viewport scissor rectangles (PA_SC_VPORT_0..15_TL/BR), the user clip planes, the programmable near-clip Z, the four cliprects, the
// scissors of viewports 1..15, the polygon offset, stereo, line stipple, tessellation and streamout offsets. On the card the context
// SRAM powers up with garbage and the CSB/SRM/replay only zero the CSB extents (62 registers); everything else the stream does not write
// (335 registers) stays garbage. Each one is gated off by the stream's own state for this draw (verify-draw.md: implicit scissor
// disabled, HiZ off, ZCLIP_PROG_NEAR off, CLIPRECT_RULE 0xffff, one viewport, no UCP, no poly offset), but "gated off" is an
// argument, not a measurement, so they are all written explicitly before the draw to their neutral value: rectangles are the full
// screen (0,0)..(0xffff,0xffff), everything else 0. Emitted right before the draw IB, after the CSB replay; the stream's own writes
// (Mesa's values) come later and win for the overlaps. rdna4-gfxsane=0 leaves it out.
void RDNA4Compute::gfxEmitSaneClip() {
	uint32_t buf[64], vals[40];
	uint32_t packets = 0, regs = 0, dwords = 0;
	auto put = [&](uint32_t offset, uint32_t count, bool rect) {
		for (uint32_t i = 0; i < count; i++)
			vals[i] = rect ? ((i & 1) ? 0xffffffffu : 0u) : 0u;   // rect: TL = 0, BR = 0xffffffff
		const uint32_t n = Pm4::setContextRegs(buf, offset, vals, count);
		gfxRing.emit(buf, n);
		packets++;
		regs += count;
		dwords += n;
	};
	put(0x040, 32, true);    // PA_SC_VPORT_0..15_TL/BR: the implicit viewport scissor rectangles
	put(0x084, 8, true);     // PA_SC_CLIPRECT_0..3_TL/BR
	put(0x096, 30, true);    // PA_SC_VPORT_SCISSOR_1..15_TL/BR (viewport 0's is the stream's)
	put(0x0b4, 24, false);   // PA_CL_UCP_0..5_X/Y/Z/W
	put(0x0cc, 1, false);    // PA_CL_PROG_NEAR_CLIP_Z
	put(0x0dd, 4, false);    // PA_SC_CLIPRECT_0..3_EXT
	put(0x2de, 6, false);    // PA_SU_POLY_OFFSET_DB_FMT_CNTL, _CLAMP, FRONT/BACK SCALE/OFFSET
	put(0x211, 1, false);    // PA_STATE_STEREO_X
	put(0x283, 1, false);    // PA_SC_LINE_STIPPLE
	put(0x291, 1, false);    // PA_SC_LINE_STIPPLE_RESET
	put(0x2a9, 1, false);    // VGT_TF_PARAM
	put(0x2cb, 2, false);    // VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE, _VERTEX_STRIDE
	put(0x2d6, 1, false);    // VGT_LS_HS_CONFIG
	GLOG("sane clip state: %u packets, %u registers, %u ring dwords before the draw (rectangles full screen, the rest 0)",
	     packets, regs, dwords);
}

namespace {

// The clip / cull / viewport / scissor / front-end registers read back through the CP right after the draw (COPY_DATA on the ring):
// what the CP holds for the draw that was just dropped. dword = the register in GC segment 1 (context: the context offset;
// uconfig: 0x2000 + the uconfig offset). The stream's own value (or the sane block's neutral value) is the expectation.
struct ClipReg { const char *name; uint16_t dword; uint32_t sane; bool hasSane; };
const ClipReg kClip[] = {
	{ "PA_CL_CLIP_CNTL", 0x204, 0, false }, { "PA_CL_VTE_CNTL", 0x205, 0, false }, { "PA_CL_VS_OUT_CNTL", 0x206, 0, false },
	{ "PA_SU_SC_MODE_CNTL", 0x207, 0, false }, { "PA_CL_NANINF_CNTL", 0x208, 0, false },
	{ "PA_SU_PRIM_FILTER_CNTL", 0x20b, 0, false }, { "PA_SU_SMALL_PRIM_FILTER_CNTL", 0x20c, 0, false },
	{ "PA_CL_NGG_CNTL", 0x20e, 0, false }, { "PA_CL_VRS_CNTL", 0x212, 0, false },
	{ "PA_SC_MODE_CNTL_0", 0x292, 0, false }, { "PA_SC_MODE_CNTL_1", 0x293, 0, false }, { "PA_SU_VTX_CNTL", 0x2f9, 0, false },
	{ "PA_CL_GB_VERT_CLIP_ADJ", 0x10b, 0, false }, { "PA_CL_GB_VERT_DISC_ADJ", 0x10c, 0, false },
	{ "PA_CL_GB_HORZ_CLIP_ADJ", 0x10d, 0, false }, { "PA_CL_GB_HORZ_DISC_ADJ", 0x10e, 0, false },
	{ "PA_CL_VPORT_XSCALE", 0x10f, 0, false }, { "PA_CL_VPORT_XOFFSET", 0x110, 0, false },
	{ "PA_CL_VPORT_YSCALE", 0x111, 0, false }, { "PA_CL_VPORT_YOFFSET", 0x112, 0, false },
	{ "PA_CL_VPORT_ZSCALE", 0x113, 0, false }, { "PA_CL_VPORT_ZOFFSET", 0x114, 0, false },
	{ "PA_SC_VPORT_ZMIN_0", 0x115, 0, false }, { "PA_SC_VPORT_ZMAX_0", 0x116, 0, false },
	{ "PA_SC_VPORT_SCISSOR_0_TL", 0x094, 0, false }, { "PA_SC_VPORT_SCISSOR_0_BR", 0x095, 0, false },
	{ "PA_SC_SCREEN_SCISSOR_TL", 0x060, 0, false }, { "PA_SC_SCREEN_SCISSOR_BR", 0x061, 0, false },
	{ "PA_SC_GENERIC_SCISSOR_TL", 0x090, 0, false }, { "PA_SC_GENERIC_SCISSOR_BR", 0x091, 0, false },
	{ "PA_SC_WINDOW_SCISSOR_TL", 0x081, 0, false }, { "PA_SC_WINDOW_SCISSOR_BR", 0x082, 0, false },
	{ "PA_SC_WINDOW_OFFSET", 0x080, 0, false }, { "PA_SC_CLIPRECT_RULE", 0x083, 0, false },
	{ "PA_SU_HARDWARE_SCREEN_OFFSET", 0x08d, 0, false }, { "PA_SC_EDGERULE", 0x08c, 0, false },
	{ "PA_SC_VPORT_0_TL", 0x040, 0, false }, { "PA_SC_VPORT_0_BR", 0x041, 0xffffffffu, true },
	{ "PA_SC_CLIPRECT_0_TL", 0x084, 0, true }, { "PA_SC_CLIPRECT_0_BR", 0x085, 0xffffffffu, true },
	{ "PA_CL_UCP_0_X", 0x0b4, 0, true }, { "PA_CL_PROG_NEAR_CLIP_Z", 0x0cc, 0, true },
	{ "PA_SU_POLY_OFFSET_DB_FMT_CNTL", 0x2de, 0, true },
	{ "PA_SC_BINNER_CNTL_0", 0x311, 0, false }, { "PA_SC_NGG_MODE_CNTL", 0x314, 0, false },
	{ "GE_MAX_OUTPUT_PER_SUBGROUP", 0x1ff, 0, false }, { "VGT_GS_MAX_VERT_OUT", 0x2ce, 0, false },
	{ "GE_NGG_SUBGRP_CNTL", 0x2d3, 0, false },
	{ "GE_CNTL", 0x225b, 0, false }, { "GE_STEREO_CNTL", 0x225f, 0, false }, { "VGT_GS_OUT_PRIM_TYPE", 0x2266, 0, false },
	{ "VGT_PRIMITIVEID_EN", 0x2262, 0, false },
};
constexpr uint32_t kClipCount = sizeof(kClip) / sizeof(kClip[0]);
static_assert(kClipCount <= 64, "clip probe table larger than its buffer");

uint32_t clipOpcode(const ClipReg &c) { return c.dword >= 0x2000 ? 0x79 : 0x69; }
uint32_t clipOffset(const ClipReg &c) { return c.dword >= 0x2000 ? c.dword - 0x2000u : c.dword; }

} // namespace

// One COPY_DATA per register into the buffer at `poolOff` (slot i = kClip[i]); emitted after the draw IB.
void RDNA4Compute::gfxEmitClipProbe(uint32_t poolOff) {
	uint32_t pkt[16];
	for (uint32_t i = 0; i < kClipCount; i++) {
		uint32_t byteOff = 0;
		if (!env.disc || !env.disc->regByteOffset(IpDiscovery::HwGc, 0, 1, kClip[i].dword, byteOff))
			continue;   // the slot keeps its sentinel
		gfxRing.emit(pkt, Pm4::copyDataRegToMem(pkt, byteOff / 4, poolMc(poolOff + 4 * i)));
	}
}

// What the CP holds for the dropped draw: every register, the ones that differ from what the stream (or the sane block) wrote, and
// the kill/cull switches spelled out.
void RDNA4Compute::gfxClipReport(const char *label, uint32_t poolOff, volatile uint32_t *ib) {
	constexpr uint32_t n = sizeof(Gfx12Draw::kStream) / 4;
	char line[240], diff[300];
	uint32_t len = 0, equal = 0, compared = 0, dl = 0, ndiff = 0;
	line[0] = diff[0] = '\0';
	uint32_t clipCntl = 0, modeCntl = 0, primFilter = 0, smallFilter = 0, vte = 0;
	for (uint32_t i = 0; i < kClipCount; i++) {
		const uint32_t val = *poolDw(poolOff + 4 * i);
		const uint32_t dw = kClip[i].dword;
		if (dw == 0x204) clipCntl = val;
		if (dw == 0x207) modeCntl = val;
		if (dw == 0x20b) primFilter = val;
		if (dw == 0x20c) smallFilter = val;
		if (dw == 0x205) vte = val;
		len += snprintf(line + len, sizeof(line) - len, "%s=0x%08x ", kClip[i].name, val);
		if (len > 150) {
			GLOG("%s: clip state (CP view after the draw): %s", label, line);
			len = 0;
			line[0] = '\0';
		}
		const int at = findStreamReg(Gfx12Draw::kStream, n, clipOpcode(kClip[i]), clipOffset(kClip[i]));
		if (at < 0 && !kClip[i].hasSane)
			continue;
		const uint32_t want = at >= 0 ? ib[at] : kClip[i].sane;
		compared++;
		if (val == want) {
			equal++;
		} else if (ndiff++ < 8 && dl < sizeof(diff) - 70) {
			dl += snprintf(diff + dl, sizeof(diff) - dl, "%s got 0x%08x want 0x%08x; ", kClip[i].name, val, want);
		}
	}
	if (len)
		GLOG("%s: clip state (CP view after the draw): %s", label, line);
	GLOG("%s: clip state: %u of %u registers equal what the stream / the sane block wrote%s%s", label, equal, compared,
	     ndiff ? "; differ: " : "", diff);
	gfxClipEq = equal;
	gfxClipCmp = compared;
	gfxClipSw = ((clipCntl >> 16) & 1) | (((clipCntl >> 21) & 1) << 1) | (((clipCntl >> 22) & 1) << 2) | ((modeCntl & 1) << 3) |
	            (((modeCntl >> 1) & 1) << 4) | ((primFilter & 1) << 5);
	// The switches that drop a primitive in the clipper / setup, spelled out (bit numbers from gfx12.json).
	GLOG("%s: clip switches: CLIP_DISABLE %u VTX_KILL_OR %u DX_RASTERIZATION_KILL %u DX_CLIP_SPACE_DEF %u ZCLIP_NEAR_DISABLE %u "
	     "ZCLIP_FAR_DISABLE %u | CULL_FRONT %u CULL_BACK %u FACE %u POLY_MODE %u | VTE scale/offset enables 0x%02x W0_FMT %u | "
	     "prim filter TRI_DISABLE %u small-prim filter %s%s", label,
	     (clipCntl >> 16) & 1, (clipCntl >> 21) & 1, (clipCntl >> 22) & 1, (clipCntl >> 19) & 1, (clipCntl >> 26) & 1,
	     (clipCntl >> 27) & 1, modeCntl & 1, (modeCntl >> 1) & 1, (modeCntl >> 2) & 1, (modeCntl >> 3) & 3, vte & 0x3f,
	     (vte >> 10) & 1, primFilter & 1, (smallFilter & 1) ? "on" : "off", (smallFilter & 2) ? " (triangles exempt)" : "");
}

// ---------------------------------------------------------------------------
// W46: persistent verdicts and the parked pipe (round 6 boot 3: the kernel log window had already moved on when the diagnostic script ran,
// so none of the `gfx:` lines survived; only the registry property Compute,GFXDiag did).
// ---------------------------------------------------------------------------

// Append one entry to the registry property Compute,GFXVerdict (the same route as Compute,GFXDiag): it survives the kernel log wrapping and
// a late script run. The buffer is bounded; a full buffer ends in "...(full)". Entries are short (about 120 characters per draw).
void RDNA4Compute::gfxVerdictAdd(const char *fmt, ...) {
	char entry[220];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(entry, sizeof(entry), fmt, ap);
	va_end(ap);
	const size_t n = strlen(entry);
	if (gfxVerdictFull || gfxVerdictLen + n + 4 + sizeof("...(full)") > sizeof(gfxVerdict)) {
		if (!gfxVerdictFull) {
			gfxVerdictFull = true;
			strlcat(gfxVerdict, "...(full)", sizeof(gfxVerdict));
			if (env.owner)
				env.owner->setProperty("Compute,GFXVerdict", gfxVerdict);
		}
		return;
	}
	memcpy(gfxVerdict + gfxVerdictLen, entry, n);
	gfxVerdictLen += static_cast<uint32_t>(n);
	memcpy(gfxVerdict + gfxVerdictLen, " ## ", 4);
	gfxVerdictLen += 4;
	gfxVerdict[gfxVerdictLen] = '\0';
	if (env.owner)
		env.owner->setProperty("Compute,GFXVerdict", gfxVerdict);
}

// The one-line summary of a draw that the property keeps: the pipeline counters (IA prims/verts, VS, C_INV, C_PRIM, PS), the pixels, the
// clip-state readback (registers equal to what was written / compared, and the kill-cull-filter switches as a bit mask: 1 CLIP_DISABLE,
// 2 VTX_KILL_OR, 4 DX_RASTERIZATION_KILL, 8 CULL_FRONT, 16 CULL_BACK, 32 TRIANGLE prim filter disabled), the NGG marker and, for the VGPR
// variant (512), whether VertexID was the lane index and the positions were what the shader intends, with lane 0's v0.
void RDNA4Compute::gfxVerdictDraw(const char *label, uint32_t variant, const GfxDrawResult &r) {
	const uint64_t *d = gfxPstatLast;
	char extra[80];
	extra[0] = '\0';
	if (variant & 32)
		snprintf(extra, sizeof(extra), " mk %s", r.nggMarker == 0xc0de0002u ? "ran" : "NO");
	else if (variant & 512)
		snprintf(extra, sizeof(extra), " vid %s pos %s v0 %08x", gfxNggVid ? "lane" : "NOT", gfxNggPos ? "ok" : "NOT", gfxNggV0);
	gfxVerdictAdd("%s: ia %llu/%llu vs %llu ci %llu cp %llu ps %llu px %u ring %s clip %u/%u sw %x%s", label,
	              static_cast<unsigned long long>(d[6]), static_cast<unsigned long long>(d[7]),
	              static_cast<unsigned long long>(d[3]), static_cast<unsigned long long>(d[2]),
	              static_cast<unsigned long long>(d[1]), static_cast<unsigned long long>(d[0]), r.covered,
	              r.ringDone ? "ok" : "HUNG", gfxClipEq, gfxClipCmp, gfxClipSw, extra);
}

bool RDNA4Compute::requestedGfxPark() {
	uint32_t v = 1;
	return requestedGfxProbe() && !(PE_parse_boot_argn("rdna4-gfxpark", &v, sizeof(v)) && v == 0);
}

// After a probe boot's draws (and ladder) the gfx pipe is left running: PFP/ME unhalted with the ring idle, and the SMU reported GFX activity
// 100 % / 81 W idle with later runtime clients timing out (dequeue timeout, ACTIVE 1). CP_STAT and GRBM_STATUS read idle after every
// draw in the round-6 log, so nothing is wedged, but the microengines keep polling and GFXOFF is disallowed while gfxMode is set. Park
// it: wait (bounded) for the CP to idle, halt PFP and ME exactly like the failure path of the ring bring-up does (gfx_v12_0_cp_gfx_enable(false)),
// and log the SMU's GFX activity / power before and after. rdna4-gfxpark=0 leaves the pipe running (probe boots only).
void RDNA4Compute::gfxPark() {
	if (!requestedGfxPark() || !gfxMode)
		return;
	RDNA4SensorsEx before {}, after {};
	const bool haveBefore = readSensorsEx(before);
	grbmSelect(0, 0, 0, 0);
	uint32_t stat = 0xffffffffu;
	for (uint32_t us = 0; us < 50000; us += 10) {
		stat = rdGc(CpStat);
		if (stat == 0)
			break;
		IODelay(10);
	}
	const uint32_t cntl0 = rdGc(CpMeCntl);
	wr(IpDiscovery::HwGc, CpMeCntl, cntl0 | kCpMePfpHalt | kCpMeMeHalt);
	gfxParked = true;   // W12k: client gfx IBs are refused while the microengines are halted (probe boots; rdna4-gfxpark=0 leaves them running)
	IOSleep(20);
	const uint32_t cntl1 = rdGc(CpMeCntl);
	GLOG("park: after the draws CP_STAT 0x%08x%s, PFP/ME halted: CP_ME_CNTL 0x%08x -> 0x%08x, GRBM 0x%08x, RB0 rptr 0x%x wptr 0x%x",
	     stat, stat ? " (did not idle)" : "", cntl0, cntl1, rdGc(GrbmStatus), rdGc(CpRb0Rptr), rdGc(CpRb0Wptr));
	IOSleep(200);   // the SMU averages: give the metrics a moment to see the pipe idle
	const bool haveAfter = readSensorsEx(after);
	GLOG("park: SMU GFX activity %u%% -> %u%%, socket power %u W -> %u W, GFXCLK %u -> %u MHz (0 = not read)",
	     haveBefore ? before.gfxActivity : 0, haveAfter ? after.gfxActivity : 0, haveBefore ? before.socketPowerW : 0,
	     haveAfter ? after.socketPowerW : 0, haveBefore ? before.currGfxclkMHz : 0, haveAfter ? after.currGfxclkMHz : 0);
	gfxVerdictAdd("park: cp_stat %x me_cntl %x->%x gfx %u->%u%% pwr %u->%uW", stat, cntl0, cntl1, haveBefore ? before.gfxActivity : 0,
	              haveAfter ? after.gfxActivity : 0, haveBefore ? before.socketPowerW : 0, haveAfter ? after.socketPowerW : 0);
}

// The GC hub fault status, logged and cleared, so the log says which bring-up step first makes the
// CP read an unmapped address (round 4: CPG read VA 0, status 0x0d3d, already before the first draw).
// It runs on EVERY rdna4-gfx=2 boot (W33 S3, decided with the lead: W31 review S3), not only with
// rdna4-gfxprobe=1, and it clears the status when it finds one, like amdgpu does, so on such boots the
// existing 'before draw' fault line reports what happened since the last mark, not what any earlier step left.
void RDNA4Compute::gfxFaultMark(const char *tag) {
	const uint32_t status = rdGc(GcL2FaultStatusLo);
	if (status && status != 0xffffffffu) {
		GLOG("fault after %s: GC hub fault status 0x%08x (VMID %u, CID 0x%x, %s), VA 0x%llx", tag, status,
		     (status >> 20) & 0xf, (status >> 9) & 0x1ff, (status >> 18) & 1 ? "write" : "read", gcFaultVa());
		gcFaultClear();
	} else {
		GLOG("no fault after %s", tag);
	}
}

// The RS64 PFP/ME data-cache and instruction-cache bases: the CP reads its stack and data through
// DC_BASE0/1; zero there would be a CPG read of VA 0. The registers are per ME0 pipe (amdgpu writes them
// under soc24_grbm_select for each pipe), so both pipes are read (pipe 1 is disabled in CP_ME_CNTL: a zero
// there is informative only next to pipe 0). Read-only: on a PSP/autoload boot the loader sets them, and
// they must not be written from here without the address of the firmware's data image (W31 review).
// Like gfxFaultMark this runs on EVERY rdna4-gfx=2 boot (W33 S3), not only with rdna4-gfxprobe.
void RDNA4Compute::gfxRs64Evidence(const char *tag) {
	for (uint32_t pipe = 0; pipe < 2; pipe++) {
		grbmSelect(0, pipe, 0, 0);
		GLOG("%s: pipe %u RS64 DC_BASE0 0x%08x_%08x DC_BASE1 0x%08x_%08x DC_BASE_CNTL 0x%08x | PFP IC_BASE 0x%08x_%08x cntl 0x%08x | "
		     "ME IC_BASE 0x%08x_%08x cntl 0x%08x | INSTR_PNTR0/1 0x%x/0x%x", tag, pipe,
		     rdGc(CpRs64DcBase0Hi), rdGc(CpRs64DcBase0Lo), rdGc(CpRs64DcBase1Hi), rdGc(CpRs64DcBase1Lo),
		     rdGc(CpRs64DcBaseCntl), rdGc(CpPfpIcBaseHi), rdGc(CpPfpIcBaseLo), rdGc(CpPfpIcBaseCntl),
		     rdGc(CpMeIcBaseHi), rdGc(CpMeIcBaseLo), rdGc(CpMeIcBaseCntl), rdGc(CpGfxRs64InstrPntr0),
		     rdGc(CpGfxRs64InstrPntr1));
	}
	grbmSelect(0, 0, 0, 0);
}

// What round 4 needs to see: the engine status right after the draw, and the pipeline
// state as the hardware holds it (context and SH registers read back over MMIO).
void RDNA4Compute::gfxEvidence(const char *tag, bool state) {
	grbmSelect(0, 0, 0, 0);
	GLOG("%s: GRBM 0x%08x GRBM2 0x%08x SE0-3 0x%08x/0x%08x/0x%08x/0x%08x SPI_BUSY 0x%08x", tag,
	     rdGc(GrbmStatus), rdGc(GrbmStatus2), rdGc(GrbmStatusSe0), rdGc(GrbmStatusSe1),
	     rdGc(GrbmStatusSe2), rdGc(GrbmStatusSe3), rdGc(SpiDebugBusy));
	GLOG("%s: CP_STAT 0x%08x RB0 rptr 0x%x wptr 0x%x (driver %llu), draw fence 0x%x, ring fence 0x%x/%u",
	     tag, rdGc(CpStat), rdGc(CpRb0Rptr), rdGc(CpRb0Wptr),
	     static_cast<unsigned long long>(gfxRing.wptr()), *poolDw(kGfxDrawFenceOffset),
	     *poolDw(kGfxFenceOffset), gfxFence);
	logGcFault(tag);
	if (!state)
		return;
	char line[240];
	uint32_t n = 0;
	auto flush = [&](const char *what) {
		if (n) {
			GLOG("%s: %s: %s", tag, what, line);
			n = 0;
		}
	};
	line[0] = '\0';
	for (const StateReg &r : kShState) {
		n += snprintf(line + n, sizeof(line) - n, "%s=0x%08x ", r.name, rdGc(r.reg));
		if (n > 150)
			flush("SH");
	}
	flush("SH");
	for (const StateReg &r : kCtxState) {
		n += snprintf(line + n, sizeof(line) - n, "%s=0x%08x ", r.name, rdGc(r.reg));
		if (n > 150)
			flush("CTX");
	}
	flush("CTX");
}

// Count the target: pixels equal to the expected colour, other non-zero pixels, and
// the bounds of the covered ones (the stage-G3 check).
void RDNA4Compute::gfxCountTarget(GfxDrawResult &r) {
	using namespace Gfx12Draw;
	r.covered = r.other = 0;
	r.minX = kWidth; r.maxX = 0; r.minY = kHeight; r.maxY = 0;
	r.row64[0] = kWidth; r.row64[1] = 0; r.row190[0] = kWidth; r.row190[1] = 0;
	r.firstNonZero = 0xffffffffu;
	for (uint32_t y = 0; y < kHeight; y++) {
		for (uint32_t x = 0; x < kWidth; x++) {
			const uint32_t p = *poolDw(kGfxTargetOffset + 4 * (y * kWidth + x));
			if (p == kCoveredRgba) {
				r.covered++;
				r.minX = x < r.minX ? x : r.minX;
				r.maxX = x > r.maxX ? x : r.maxX;
				r.minY = y < r.minY ? y : r.minY;
				r.maxY = y > r.maxY ? y : r.maxY;
				uint32_t *span = y == 64 ? r.row64 : y == 190 ? r.row190 : nullptr;
				if (span) {
					span[0] = x < span[0] ? x : span[0];
					span[1] = x > span[1] ? x : span[1];
				}
			} else if (p) {
				r.other++;
				if (r.firstNonZero == 0xffffffffu)
					r.firstNonZero = y * kWidth + x;
			}
		}
	}
	// notes 3.9: 8192 pixels, rows 64..190, row 64 = x 64..191, row 190 = x 127..128.
	r.ok = r.covered == kCoveredPixels && !r.other && r.minY == 64 && r.maxY == 190 &&
	       r.row64[0] == 64 && r.row64[1] == 191 && r.row190[0] == 127 && r.row190[1] == 128;
}

// One draw of the stream into the cleared target, with the diagnostic variant
// `variant` (a bit set of rdna4-gfxdiag: 1 = one user SGPR on the NGG stage, 2 =
// INST_PREF_SIZE like Mesa, 4 = the GS_ALLOC_REQ NGG shader, 8 = a pixel shader that
// stores a marker to memory first). 0 is the plain stream. `va` are the addresses
// the stream's relocations take.
bool RDNA4Compute::gfxDrawRun(const char *label, uint32_t variant, const uint64_t *va, GfxDrawResult &r) {
	using namespace Gfx12Draw;
	memset(&r, 0, sizeof(r));

	// Shaders, each followed by s_code_end padding for the SQ's prefetch.
	auto place = [&](uint32_t at, const uint32_t *code, uint32_t dwords) {
		for (uint32_t i = 0; i < 0x100; i++)
			*poolDw(at + 4 * i) = i < dwords ? code[i] : 0xbf9f0000u;   // s_code_end
	};
	if (variant & 4)
		place(kGfxVsOffset, kNggmsgKernel, sizeof(kNggmsgKernel) / 4);
	else if (variant & 32)   // W37 #4: the NGG shader that stores a marker before anything else
		place(kGfxVsOffset, kNggstoreKernel, sizeof(kNggstoreKernel) / 4);
	else if (variant & 512)   // W45: the NGG shader that stores what it gets and computes, per lane
		place(kGfxVsOffset, kNggvgprKernel, sizeof(kNggvgprKernel) / 4);
	else if (variant & 1024)   // W46: the NGG shader that takes nothing from its VGPR inputs (constant primitive, lane-id vertices)
		place(kGfxVsOffset, kNggconstKernel, sizeof(kNggconstKernel) / 4);
	else if (variant & 4096)   // the wave64 fallback: ngg.s as a wave64 NGG shader, the way RADV runs its NGG (GS) stage (PS stays wave32)
		place(kGfxVsOffset, kNgg64Kernel, sizeof(kNgg64Kernel) / 4);
	else
		place(kGfxVsOffset, kNggKernel, sizeof(kNggKernel) / 4);
	if (variant & 32) {   // the two literal dwords of its s_mov_b32 sN, literal carry the marker address
		const uint64_t mark = poolMc(kGfxNggMarkOffset);
		for (uint32_t i = 0; i < sizeof(kNggstoreKernel) / 4; i++) {
			if (*poolDw(kGfxVsOffset + 4 * i) == 0xdead0001u)
				*poolDw(kGfxVsOffset + 4 * i) = static_cast<uint32_t>(mark);
			else if (*poolDw(kGfxVsOffset + 4 * i) == 0xdead0002u)
				*poolDw(kGfxVsOffset + 4 * i) = static_cast<uint32_t>(mark >> 32);
		}
	}
	if (variant & 8)
		place(kGfxPsOffset, kPsstoreKernel, sizeof(kPsstoreKernel) / 4);
	else
		place(kGfxPsOffset, kPsredKernel, sizeof(kPsredKernel) / 4);

	if (variant & 512) {   // the eight literal dwords of nggvgpr.s carry the four array bases (0x80 bytes apart)
		for (uint32_t i = 0; i < sizeof(kNggvgprKernel) / 4; i++) {
			const uint32_t lit = *poolDw(kGfxVsOffset + 4 * i);
			if (lit >= 0xdead0001u && lit <= 0xdead0008u) {
				const uint32_t k = lit - 0xdead0001u;
				const uint64_t base = poolMc(kGfxVgprOffset + 0x80 * (k / 2));
				*poolDw(kGfxVsOffset + 4 * i) = (k & 1) ? static_cast<uint32_t>(base >> 32) : static_cast<uint32_t>(base);
			}
		}
	}
	// The command stream, its addresses filled in.
	constexpr uint32_t n = sizeof(kStream) / 4;
	volatile uint32_t *ib = poolDw(kGfxIbOffset);
	for (uint32_t i = 0; i < n; i++)
		ib[i] = kStream[i];
	for (const Reloc &rl : kRelocs)
		ib[rl.dword] = static_cast<uint32_t>((va[rl.sym] >> rl.shift) & rl.mask);
	auto patch = [&](uint32_t opcode, uint32_t reg, uint32_t mask, uint32_t value, const char *what) {
		const int at = findStreamReg(kStream, n, opcode, reg);
		if (at < 0) {
			GLOG("%s: variant patch %s: register not in the stream", label, what);
			return;
		}
		const uint32_t old = ib[at];
		ib[at] = (old & ~mask) | (value & mask);
		GLOG("%s: variant patch %s: 0x%08x -> 0x%08x", label, what, old, ib[at]);
	};
	if (variant & 1)   // RSRC2_GS.USER_SGPR = 1 (open question 4: Mesa always has user SGPRs)
		patch(kOpSetShReg, 0x1a2b - kShBase, 0x0000003e, 1u << 1, "SPI_SHADER_PGM_RSRC2_GS.USER_SGPR=1");
	if (variant & 2) { // INST_PREF_SIZE like Mesa (open question 5): VS 3, PS 2 units of 128 B
		patch(kOpSetShReg, 0x1a28 - kShBase, 0x7f800000, 3u << 23, "SPI_SHADER_PGM_RSRC4_GS.INST_PREF_SIZE=3");
		patch(kOpSetShReg, 0x19a7 - kShBase, 0x00ff0000, 2u << 16, "SPI_SHADER_PGM_RSRC4_PS.INST_PREF_SIZE=2");
	}
	if (variant & 4)   // VGT_SHADER_STAGES_EN.PRIMGEN_PASSTHRU_NO_MSG = 0 (the shader sends GS_ALLOC_REQ)
		patch(kOpSetContextReg, 0x02a6, 0x04000000, 0, "VGT_SHADER_STAGES_EN.PRIMGEN_PASSTHRU_NO_MSG=0");
	if (variant & 8)   // two user SGPRs on the PS carry the marker address
		patch(kOpSetShReg, 0x19ab - kShBase, 0x0000003e, 2u << 1, "SPI_SHADER_PGM_RSRC2_PS.USER_SGPR=2");
	if (variant & 64) {   // W41: VGT_PRIMITIVE_TYPE through SET_UCONFIG_REG_INDEX (0x7a) with index 1, as RADV (radv_cmd_buffer.c:4685; ac_cmdbuf.h ac_cmdbuf_set_ucfg_reg_idx: offset | idx << 28)
		const int at = findStreamReg(kStream, n, 0x79, 0x242);
		if (at < 2 || (((ib[at - 2] >> 16) & 0x3fff) != 1)) {
			GLOG("%s: variant patch VGT_PRIMITIVE_TYPE index 1: not a single-register SET_UCONFIG_REG in the stream", label);
		} else {
			const uint32_t oldHdr = ib[at - 2], oldOff = ib[at - 1];
			ib[at - 2] = (oldHdr & ~0x0000ff00u) | (0x7au << 8);
			ib[at - 1] = oldOff | (1u << 28);
			GLOG("%s: variant patch VGT_PRIMITIVE_TYPE via SET_UCONFIG_REG_INDEX index 1: header 0x%08x -> 0x%08x, offset 0x%08x -> 0x%08x",
			     label, oldHdr, ib[at - 2], oldOff, ib[at - 1]);
		}
	}
	// The variant bits are applied one per draw (the ladder runs a single bit at a time): 4096 must not be combined with the wave32
	// marker shaders (32 / 512 / 1024), whose VGPRS=1 patch it would overwrite with 2 while clearing GS_W32_EN.
	if (variant & (32 | 512 | 1024))   // the marker shader uses v8/v9: 10 VGPRs = 2 granules of 8 (VGPRS field = granules - 1)
		patch(kOpSetShReg, 0x1a2a - kShBase, 0x0000003f, 1, "SPI_SHADER_PGM_RSRC1_GS.VGPRS=1");

	if (variant & 4096) {   // wave64 (proven on the card under Linux: tools/linux-replay REPLAY_VARIANT=4096, 8192 px)
		patch(kOpSetContextReg, 0x02a6, 0x00400000, 0, "VGT_SHADER_STAGES_EN.GS_W32_EN=0 (wave64 NGG)");
		patch(kOpSetShReg, 0x1a2a - kShBase, 0x0000003f, 2, "SPI_SHADER_PGM_RSRC1_GS.VGPRS=2 (12 VGPRs: wave64 granules of 4)");
	}
	if (variant & 2048)   // W46: GE_PRIM_RING_SIZE without Mesa's GL2 hints (SCOPE, PAF/PAB_TEMPORAL, FORCE_SE_SCOPE, PAB_NOFILL): MEM_SIZE only
		patch(0x79, 0x26b, 0xffffffff, 0x000007fe, "GE_PRIM_RING_SIZE = MEM_SIZE only (no GL2 hints)");
	if (variant & 128)   // W45: PA_CL_CLIP_CNTL.CLIP_DISABLE: if the primitive survives, the clipper was dropping it
		patch(kOpSetContextReg, 0x204, 0x00010000, 0x00010000, "PA_CL_CLIP_CNTL.CLIP_DISABLE=1");
	if (variant & 256) {   // W45: the primitive filters off: PA_SU_PRIM_FILTER_CNTL disable bits 0-3, small-prim filter off
		patch(kOpSetContextReg, 0x20b, 0x0000000f, 0x0000000f, "PA_SU_PRIM_FILTER_CNTL.*_FILTER_DISABLE=1");
		patch(kOpSetContextReg, 0x20c, 0x00000001, 0x00000000, "PA_SU_SMALL_PRIM_FILTER_CNTL.SMALL_PRIM_FILTER_ENABLE=0");
	}
	// The target, cleared; the fences and the marker, zero.
	for (uint32_t i = 0; i < kWidth * kHeight; i++)
		*poolDw(kGfxTargetOffset + 4 * i) = 0;
	*poolDw(kGfxDrawFenceOffset) = 0;
	*poolDw(kGfxMarkerOffset) = 0;
	for (uint32_t i = 0; i < 3; i++)
		*poolDw(kGfxNggMarkOffset + 4 * i) = 0;
	for (uint32_t i = 0; i < 128; i++)   // W45: the per-lane VGPR arrays (variant 512) and the clip-state readback slots
		*poolDw(kGfxVgprOffset + 4 * i) = 0;
	for (uint32_t i = 0; i < 64; i++)
		*poolDw(kGfxClipOffset + 4 * i) = 0xdeadf00du;
	flushHdp();
	GLOG("%s: %u-dword stream at MC 0x%llx, VS 0x%llx PS 0x%llx, target 0x%llx, rings 0x%llx "
	     "(%llu MiB)%s", label, n, poolMc(kGfxIbOffset), va[kVs], va[kPs], va[kCb], va[kAttrRing],
	     kRingBytes >> 20, variant ? "" : " (baseline)");

	trail(variant ? "gfx: draw variant" : "gfx: first draw");
	uint32_t pkt[16];
	if (variant & 8) {   // the marker's address goes to SPI_SHADER_USER_DATA_PS_0/1 first
		const uint64_t marker = poolMc(kGfxMarkerOffset);
		const uint32_t ud[2] = { static_cast<uint32_t>(marker), static_cast<uint32_t>(marker >> 32) };
		gfxRing.emit(pkt, Pm4::setShReg(pkt, 0x2c00 + (0x19ac - kShBase), ud, 2));
	}
	if (requestedGfxCsbReplay())
		gfxEmitCsbReplay();   // W37 #1: the draw never depends on SRM having applied the clear state
	if (requestedGfxSane())
		gfxEmitSaneClip();   // W45: every clip/cull/viewport/scissor register the stream leaves at power-up garbage, explicitly neutral
	const bool pstat = requestedGfxProbe();
	if (pstat) {   // W37 #4: pipeline statistics around the draw (both buffers pre-filled with a sentinel)
		for (uint32_t i = 0; i < 28; i++) {
			*poolDw(kGfxPstatPre + 4 * i) = 0xdeadf00du;
			*poolDw(kGfxPstatPost + 4 * i) = 0xdeadf00du;
		}
		flushHdp();
		gfxRing.emit(pkt, Pm4::eventWrite(pkt, 0x19));   // PIPELINESTAT_START (event 25)
		gfxRing.emit(pkt, Pm4::eventWriteAddr(pkt, 0x1e | (2u << 8), poolMc(kGfxPstatPre)));   // SAMPLE_PIPELINESTAT (event 30, index 2)
	}
	// rdna4-gfxprobe=1: split the stream at NUM_INSTANCES (state | draw) and read the CP's own view of the
	// state between the two halves and again after the draw (COPY_DATA on the ring, in stream order).
	const uint32_t split = requestedGfxProbe() ? findStreamPacket(kStream, n, 0x2f) : 0;
	if (split) {
		for (uint32_t i = 0; i < 2 * kProbeMax; i++)
			*poolDw(kGfxProbeOffset + 4 * i) = 0xdeadf00du;
		flushHdp();
		gfxRing.emit(pkt, Pm4::indirectBufferGfx(pkt, poolMc(kGfxIbOffset), split, 0));
		gfxEmitProbe(kGfxProbeOffset);
		gfxRing.emit(pkt, Pm4::indirectBufferGfx(pkt, poolMc(kGfxIbOffset) + 4ull * split, n - split, 0));
		gfxEmitProbe(kGfxProbePost);
	} else {
		gfxRing.emit(pkt, Pm4::indirectBufferGfx(pkt, poolMc(kGfxIbOffset), n, 0));
	}
	if (split)
		gfxEmitClipProbe(kGfxClipOffset);   // W45: the clip state as the CP holds it right after the draw
	if (pstat)
		gfxRing.emit(pkt, Pm4::eventWriteAddr(pkt, 0x1e | (2u << 8), poolMc(kGfxPstatPost)));
	gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
	uint64_t t0 = mach_absolute_time();
	gfxKick(gfxRing.wptr());
	r.ringDone = gfxFenceWait(gfxFence, 500000);
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	r.ns = ns;
	r.drawFence = *poolDw(kGfxDrawFenceOffset);
	// W41 (verify-draw.md 7): the FIRST lines after a draw are the pipeline-statistics verdict and the NGG marker, then the evidence.
	if (pstat && r.ringDone) {
		gfxPstatReport(label);
		r.cInv = static_cast<uint32_t>(gfxPstatLast[2]);
		r.cPrim = static_cast<uint32_t>(gfxPstatLast[1]);
		r.ps = static_cast<uint32_t>(gfxPstatLast[0]);
	}
	if (variant & 32) {   // W37 #4: did the NGG wave launch at all?
		r.nggMarker = *poolDw(kGfxNggMarkOffset);
		r.nggS2 = *poolDw(kGfxNggMarkOffset + 4);
		r.nggS3 = *poolDw(kGfxNggMarkOffset + 8);
		GLOG("%s: NGG marker 0x%08x (%s), s2 (gs_tg_info) 0x%08x, s3 (merged_wave_info) 0x%08x", label, r.nggMarker,
		     r.nggMarker == 0xc0de0002u ? "the NGG wave ran and stored to memory" : "NOT written: the NGG wave did not run",
		     r.nggS2, r.nggS3);
	}
	if (variant & 512) {   // W45: what the NGG wave got (v0, v3) and computed (x, y), lane by lane
		// Only lane 0 holds the packed primitive in v0 (0x040a0300 on the card); v0 of lanes 1-3 is whatever those lanes hold (garbage by
		// design, the wave has one primitive). x/y are stored under the 3-lane position exec, so lane 3 of x/y is the cleared slot (stale).
		uint32_t v0[4], v3[4], px[4], py[4];
		for (uint32_t l = 0; l < 4; l++) {
			v0[l] = *poolDw(kGfxVgprOffset + 4 * l);
			v3[l] = *poolDw(kGfxVgprOffset + 0x80 + 4 * l);
			px[l] = *poolDw(kGfxVgprOffset + 0x100 + 4 * l);
			py[l] = *poolDw(kGfxVgprOffset + 0x180 + 4 * l);
		}
		GLOG("%s: NGG VGPRs per lane: v0 0x%08x 0x%08x 0x%08x 0x%08x | v3 (VertexID) %u %u %u %u | x 0x%08x 0x%08x 0x%08x 0x%08x | "
		     "y 0x%08x 0x%08x 0x%08x 0x%08x", label, v0[0], v0[1], v0[2], v0[3], v3[0], v3[1], v3[2], v3[3], px[0], px[1],
		     px[2], px[3], py[0], py[1], py[2], py[3]);
		const bool vidOk = v3[0] == 0 && v3[1] == 1 && v3[2] == 2;
		const bool posOk = px[0] == 0xbf000000u && px[1] == 0x3f000000u && px[2] == 0 && py[0] == 0xbf000000u &&
		                   py[1] == 0xbf000000u && py[2] == 0x3f000000u;
		gfxNggVid = vidOk;
		gfxNggPos = posOk;
		gfxNggV0 = v0[0];
		GLOG("%s: NGG inputs say: VertexID %s the lane index (v3 = 0,1,2), the exported positions %s what ngg.s intends "
		     "((-0.5,-0.5) (0.5,-0.5) (0,0.5))", label, vidOk ? "IS" : "is NOT", posOk ? "are" : "are NOT");
	}
	if (requestedGfxProbe() && !variant)
		GLOG("%s: NGG marker: not part of the clean baseline (ngg.s); ladder variant 32 (marker) runs next if the draw is empty", label);
	if (split && r.ringDone) {   // W41 (verify-draw.md 5, A1): VGT_PRIMITIVE_TYPE reads 0 over MMIO on the card while the stream writes 4
		for (uint32_t i = 0; i < kProbeCount; i++) {
			if (strcmp(kProbe[i].name, "VGT_PRIMITIVE_TYPE") != 0)
				continue;
			const int at = findStreamReg(kStream, n, probeOpcode(kProbe[i]), probeOffset(kProbe[i]));
			GLOG("%s: VGT_PRIMITIVE_TYPE: CP view mid 0x%08x post 0x%08x, the stream wrote 0x%08x (%s), MMIO 0x%08x", label,
			     *poolDw(kGfxProbeOffset + 4 * i), *poolDw(kGfxProbePost + 4 * i), at >= 0 ? ib[at] : 0u,
			     (variant & 64) ? "SET_UCONFIG_REG_INDEX index 1, like RADV" : "plain SET_UCONFIG_REG, like radeonsi",
			     rdGc(Reg { kProbe[i].seg, kProbe[i].dword }));
		}
	}
	gfxEvidence(label, true);
	if (split && r.ringDone) {
		char what[40];
		snprintf(what, sizeof(what), "%s probe mid", label);
		r.probeSeen = true;
		gfxProbeReport(what, kGfxProbeOffset, ib, &r.probeEqual, &r.probeCounted);
		snprintf(what, sizeof(what), "%s probe post", label);
		gfxProbeReport(what, kGfxProbePost, ib);
	}
	if (split && r.ringDone)
		gfxClipReport(label, kGfxClipOffset, ib);   // W45
	static uint32_t ctxDumps = 0;   // W37 review NIT: the 1024-register dump only for the baseline and the first variant
	if (requestedGfxProbe() && r.ringDone && ctxDumps++ < 2)
		gfxContextDump(label);   // W37 #1: what the clear-state registers and their neighbours hold now
	gfxFaultMark(label);
	if (!r.ringDone || r.drawFence != 1) {
		GLOG("%s: did not finish (ring fence %s, draw fence 0x%x)", label,
		     r.ringDone ? "ok" : "NOT signalled", r.drawFence);
		gfxVerdictDraw(label, variant, r);
		gfxStatus("draw");
		return false;
	}

	gfxCountTarget(r);
	gfxVerdictDraw(label, variant, r);   // W46: the persistent one-line summary of this draw
	r.marker = *poolDw(kGfxMarkerOffset);
	if (variant & 8)
		GLOG("%s: pixel-shader marker 0x%08x (%s)", label, r.marker,
		     r.marker == kMarkerValue ? "the PS ran and stored to memory" : "NOT written");
	GLOG("%s: %s in %llu us: %u pixels 0x%08x (want %u), %u others (first at %d); bounds x %u..%u y %u..%u, "
	     "row 64 x %u..%u, row 190 x %u..%u", label, r.ok ? "THE TRIANGLE IS RIGHT" : "wrong image",
	     r.ns / 1000, r.covered, kCoveredRgba, kCoveredPixels, r.other,
	     r.firstNonZero == 0xffffffffu ? -1 : static_cast<int>(r.firstNonZero), r.minX, r.maxX, r.minY,
	     r.maxY, r.row64[0], r.row64[1], r.row190[0], r.row190[1]);

	if (!r.ok && r.covered == 0 && !r.other) {
		// Nothing landed. Two cheap questions before blaming the pipeline: is the write
		// merely late, and is it stuck in a cache? Look again after 2 ms, then force a
		// full GL2 write-back/invalidate (the gfx_v12_0_emit_mem_sync GCR bits) and a fresh
		// end-of-pipe fence, and look a third time.
		IOSleep(2);
		GfxDrawResult again;
		gfxCountTarget(again);
		gfxRing.emit(pkt, Pm4::acquireMem(pkt, Pm4::kGcrMemSync));
		gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
		gfxKick(gfxRing.wptr());
		const bool flushed = gfxFenceWait(gfxFence, 500000);
		GfxDrawResult flush;
		gfxCountTarget(flush);
		GLOG("%s: target still empty: after 2 ms %u px/%u other; after a full ACQUIRE_MEM GL2 write-back "
		     "(fence %s) %u px/%u other", label, again.covered, again.other, flushed ? "ok" : "NOT signalled",
		     flush.covered, flush.other);
		gfxEvidence("after flush", false);
	}
	return true;
}

// The persistent one-line summary of the G4 draw (registry property Compute,GFXVerdict, the same route and "##" separator as gfxVerdictDraw): the
// pipeline counters of the draw (probe boots; zeros otherwise), the pixels, the ring state, the largest channel error in channel units and the
// centroid pixel. No clip-state readback: G4 does not run the clip probe.
void RDNA4Compute::gfxVerdictCol(const GfxColResult &r) {
	const uint64_t *d = gfxPstatLast;
	gfxVerdictAdd("col: ia %llu/%llu vs %llu ci %llu cp %llu ps %llu px %u ring %s err %u.%02u sum %u cen %08x%s",
	              static_cast<unsigned long long>(d[6]), static_cast<unsigned long long>(d[7]),
	              static_cast<unsigned long long>(d[3]), static_cast<unsigned long long>(d[2]),
	              static_cast<unsigned long long>(d[1]), static_cast<unsigned long long>(d[0]), r.covered,
	              r.ringDone ? "ok" : "HUNG", r.maxChannelErr256 * 100 / 256 / 100, r.maxChannelErr256 * 100 / 256 % 100, r.maxSumErr,
	              r.centroid, r.ok ? " RIGHT" : "");
}

// G4: one draw of the colour triangle (src/gfx12_draw_col.h, shaders/nggcol.s + pscol.s) into the cleared target, exactly the way gfxDrawRun
// draws G3: the same stream discipline (relocations, CSB replay, sane clip state, pipeline statistics on probe boots, one RELEASE_MEM fence,
// the same timeout and "did not finish" handling), then the colour check shared with tools/linux-replay (userspace/gfx12tricol.h). `va` are
// the relocation addresses (same order as G3's: VS, PS, target, three rings, fence); va[kColAttrRing] is also the address nggcol.s's attribute
// ring descriptor carries. The G3 baseline must have passed (the caller checks), so the ring is healthy and the shared rings/target are in use
// by nothing else.
bool RDNA4Compute::gfxColDrawRun(const char *label, const uint64_t *va, GfxColResult &r) {
	using namespace Gfx12DrawCol;
	static_assert(kColRingBytes == Gfx12Draw::kRingBytes && kColWidth == Gfx12Draw::kWidth && kColHeight == Gfx12Draw::kHeight,
	              "G4 shares the GE ring block and the target with G3");
	memset(&r, 0, sizeof(r));

	// Shaders in their own slots, each followed by s_code_end padding; the NGG shader's descriptor literals carry the attribute ring's address.
	for (uint32_t i = 0; i < RDNA4_TRICOL_SHADER_PAD; i++) {
		*poolDw(kGfxColVsOffset + 4 * i) = rdna4_tricol_vs_dword(i, va[kColAttrRing]);
		*poolDw(kGfxColPsOffset + 4 * i) = rdna4_tricol_ps_dword(i);
	}
	// The command stream, its addresses filled in.
	constexpr uint32_t n = sizeof(kColStream) / 4;
	volatile uint32_t *ib = poolDw(kGfxIbOffset);
	for (uint32_t i = 0; i < n; i++)
		ib[i] = kColStream[i];
	for (const ColReloc &rl : kColRelocs)
		ib[rl.dword] = static_cast<uint32_t>((va[rl.sym] >> rl.shift) & rl.mask);
	// The target, cleared; the fence, zero.
	for (uint32_t i = 0; i < kColWidth * kColHeight; i++)
		*poolDw(kGfxTargetOffset + 4 * i) = 0;
	*poolDw(kGfxDrawFenceOffset) = 0;
	flushHdp();
	GLOG("%s: %u-dword stream at MC 0x%llx, VS 0x%llx PS 0x%llx, target 0x%llx, rings 0x%llx (%llu MiB), attribute ring descriptor "
	     "0x%08x 0x%08x 0x%08x 0x%08x", label, n, poolMc(kGfxIbOffset), va[kColVs], va[kColPs], va[kColCb], va[kColAttrRing],
	     kColRingBytes >> 20, static_cast<uint32_t>(va[kColAttrRing]),
	     static_cast<uint32_t>((va[kColAttrRing] >> 32) & 0xffffu) | 0xc0000000u | (16u << 16), static_cast<uint32_t>(kColRingBytes), 0x0043ffacu);

	trail("gfx: colour draw");
	uint32_t pkt[16];
	if (requestedGfxCsbReplay())
		gfxEmitCsbReplay();   // the draw never depends on SRM having applied the clear state (as G3)
	if (requestedGfxSane())
		gfxEmitSaneClip();    // every clip/cull/viewport/scissor register the stream leaves at power-up garbage, explicitly neutral (as G3)
	const bool pstat = requestedGfxProbe();
	if (pstat) {   // pipeline statistics around the draw (both buffers pre-filled with a sentinel), as G3
		for (uint32_t i = 0; i < 28; i++) {
			*poolDw(kGfxPstatPre + 4 * i) = 0xdeadf00du;
			*poolDw(kGfxPstatPost + 4 * i) = 0xdeadf00du;
		}
		flushHdp();
		gfxRing.emit(pkt, Pm4::eventWrite(pkt, 0x19));   // PIPELINESTAT_START
		gfxRing.emit(pkt, Pm4::eventWriteAddr(pkt, 0x1e | (2u << 8), poolMc(kGfxPstatPre)));   // SAMPLE_PIPELINESTAT
	}
	gfxRing.emit(pkt, Pm4::indirectBufferGfx(pkt, poolMc(kGfxIbOffset), n, 0));
	if (pstat)
		gfxRing.emit(pkt, Pm4::eventWriteAddr(pkt, 0x1e | (2u << 8), poolMc(kGfxPstatPost)));
	gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
	uint64_t t0 = mach_absolute_time();
	gfxKick(gfxRing.wptr());
	r.ringDone = gfxFenceWait(gfxFence, 500000);
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	r.ns = ns;
	r.drawFence = *poolDw(kGfxDrawFenceOffset);
	if (pstat && r.ringDone) {
		gfxPstatReport(label);
		r.cInv = static_cast<uint32_t>(gfxPstatLast[2]);
		r.cPrim = static_cast<uint32_t>(gfxPstatLast[1]);
		r.ps = static_cast<uint32_t>(gfxPstatLast[0]);
	}
	gfxEvidence(label, true);
	gfxFaultMark(label);
	if (!r.ringDone || r.drawFence != 1) {
		GLOG("%s: did not finish (ring fence %s, draw fence 0x%x)", label, r.ringDone ? "ok" : "NOT signalled", r.drawFence);
		gfxVerdictCol(r);
		gfxStatus("colour draw");
		return false;
	}

	// The check: the same function tools/linux-replay (REPLAY_DRAW=col) runs on the Linux side.
	auto check = [&](GfxColResult &out) {
		rdna4_tricol_result cr;
		out.ok = rdna4_tricol_check(const_cast<const uint32_t *>(poolDw(kGfxTargetOffset)), &cr) != 0;
		out.covered = cr.covered; out.badAlpha = cr.badAlpha; out.notDominant = cr.notDominant;
		out.maxChannelErr256 = cr.maxChannelErr256; out.maxSumErr = cr.maxSumErr;
		out.minX = cr.minX; out.maxX = cr.maxX; out.minY = cr.minY; out.maxY = cr.maxY;
		out.near0 = cr.near0; out.near1 = cr.near1; out.near2 = cr.near2; out.centroid = cr.centroid;
	};
	check(r);
	gfxVerdictCol(r);
	GLOG("%s: %s in %llu us: %u pixels (want %u), bounds x %u..%u y %u..%u; max channel error %u.%02u (tolerance %u), max |R+G+B-255| %u, "
	     "%u bad alpha, %u not dominated by the nearest vertex's colour; near v0 (66,65) 0x%08x, near v1 (189,65) 0x%08x, near v2 (128,188) "
	     "0x%08x, centroid (128,106) 0x%08x (want ~85,85,85)", label, r.ok ? "THE COLOUR TRIANGLE IS RIGHT" : "wrong colour image",
	     r.ns / 1000, r.covered, kColCoveredPixels, r.minX, r.maxX, r.minY, r.maxY, r.maxChannelErr256 * 100 / 256 / 100,
	     r.maxChannelErr256 * 100 / 256 % 100, kColChannelTolerance, r.maxSumErr, r.badAlpha, r.notDominant, r.near0, r.near1, r.near2,
	     r.centroid);

	if (!r.ok && r.covered == 0) {   // nothing landed: the same two cheap questions as G3 (late write? stuck in a cache?)
		IOSleep(2);
		GfxColResult again;
		check(again);
		gfxRing.emit(pkt, Pm4::acquireMem(pkt, Pm4::kGcrMemSync));
		gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
		gfxKick(gfxRing.wptr());
		const bool flushed = gfxFenceWait(gfxFence, 500000);
		GfxColResult flush;
		check(flush);
		GLOG("%s: target still empty: after 2 ms %u px; after a full ACQUIRE_MEM GL2 write-back (fence %s) %u px", label, again.covered,
		     flushed ? "ok" : "NOT signalled", flush.covered);
		gfxEvidence("after col flush", false);
	}
	return true;
}

// G3: the first draw. Needs the gfx ring up and the device heap (the GE
// rings, 10.5 MiB, live there); everything else is in the pool.
bool RDNA4Compute::stageGfxDraw() {
	using namespace Gfx12Draw;
	if (!gfxMode || !dmaReady || !devHeap.size() || !rtLock) {
		GLOG("draw: skipped (%s)", !gfxMode ? "no gfx ring" : "no device heap");
		publishResult("gfx", "SKIPPED draw prerequisites");
		return false;
	}
	// The GE rings are sized for kMaxSe shader engines (they scale with its
	// square): check the card's count first. The count is the IP discovery
	// gc_info one: gfx_v12_0 uses max_shader_engines and max_backends_per_se,
	// which amdgpu_discovery_get_gc_info fills from that table
	// (amdgpu_discovery.c:2016,2020). The decoded GB_ADDR_CONFIG field is not
	// used as the SE count (gfx_v12_0.c:3648 decodes num_se from it and never
	// reads it back); the card's 0x08200545 reads 16 through it
	// (NUM_SHADER_ENGINES, gc_12_0_0_sh_mask.h:25752), so it is only logged.
	const uint32_t gbAddr = rdGc(GbAddrConfig);
	uint32_t ses = 0, rbPerSe = 0, gcVer = 0;
	const bool haveGc = env.disc && env.disc->gcInfo(ses, rbPerSe, &gcVer);
	GLOG("draw: GB_ADDR_CONFIG 0x%08x, IP discovery gc_info %u.%u: %s%u shader engines, %u RBs per SE",
	     gbAddr, gcVer >> 16, gcVer & 0xffff, haveGc ? "" : "absent, ", ses, rbPerSe);
	if (!haveGc || !ses || ses > kMaxSe) {
		GLOG("draw: the GE rings are sized for %u shader engines; skipped", kMaxSe);
		publishResult("gfx", "SKIPPED unsupported shader-engine count");
		return false;
	}

	// Rings: 64 KiB-granular bases; Mesa aligns the whole block to 2 MiB.
	constexpr uint64_t kAlign = 2ull << 20;
	uint64_t off = 0;
	IOLockLock(rtLock);
	const bool got = devHeap.alloc(kRingBytes + kAlign, off);
	IOLockUnlock(rtLock);
	if (!got) {
		GLOG("draw: no %llu MiB for the GE rings in the device heap", (kRingBytes + kAlign) >> 20);
		publishResult("gfx", "FAIL draw device heap allocation");
		return false;
	}
	gfxRings = off;
	const uint64_t ringVa = (vramMc(off) + kAlign - 1) & ~(kAlign - 1);
	const uint64_t va[7] = {
		poolMc(kGfxVsOffset), poolMc(kGfxPsOffset), poolMc(kGfxTargetOffset), ringVa,
		ringVa + kAttrRingBytes, ringVa + kAttrRingBytes + kPosRingBytes, poolMc(kGfxDrawFenceOffset),
	};

	GfxDrawResult base;
	gfxRs64Evidence("before draw");
	gfxQueueEvidence("before draw");
	gfxLinuxDiff("before draw");   // W42
	if (requestedGfxVa0())
		gfxMapVa0();   // W37 #2: after the single-packet probe has looked at the faults; the CPG's stray VA-0 access lands in a page we dump
	gfxEvidence("before draw", false);
	if (requestedGfxProbe())
		if (!gfxSentinelCheck()) {   // W33 S2: does the CP-side readback see a context write? (decides how to read 'probe mid')
			GLOG("draw: skipped, the sentinel submission did not finish (the ring is not healthy; power-cycle before the next boot)");
			publishResult("gfx", "FAIL sentinel did not finish");
			return false;
		}
	if (requestedGfxProbe())
		gfxClearStatePre();   // W39 S3: before the first CSB replay
	const bool ran = gfxDrawRun("draw", 0, va, base);
	if (requestedGfxVa0())
		gfxDumpVa0("after the baseline draw");   // W37 #2: what the CP wrote at VA 0 during the draw
	if (!ran) {
		publishResult("gfx", "FAIL draw fence");
		return false;
	}
	char result[160];
	snprintf(result, sizeof(result), "%s %s, draw %u px in %llu us",
	         base.ok ? "PASS" : "FAIL", base.ok ? "ring test, THE TRIANGLE IS RIGHT" : "draw image",
	         base.covered, static_cast<unsigned long long>(base.ns / 1000));
	publishResult("gfx", result);
	env.owner->setProperty("Compute,GFXDrawPixels", static_cast<uint64_t>(base.covered), 32);

	// G4 (rdna4-gfxcol=1, default off): the colour triangle, only after a passing G3 baseline and after its result is published. It runs BEFORE the
	// ladder, which runs only when the baseline failed (or rdna4-gfxdiag has bit 16): the ring is healthy here, and a ladder variant that hangs
	// cannot pre-empt it. A G4 draw that does not finish leaves the ring in an unknown state, so the ladder is skipped after it.
	bool colHung = false;
	if (requestedGfxCol()) {
		if (!base.ok) {
			GLOG("col: skipped, the G3 baseline did not pass");
			publishResult("gfx-col", "SKIPPED G3 baseline did not pass");
		} else {
			const uint64_t vaCol[7] = {
				poolMc(kGfxColVsOffset), poolMc(kGfxColPsOffset), poolMc(kGfxTargetOffset), ringVa,
				ringVa + kAttrRingBytes, ringVa + kAttrRingBytes + kPosRingBytes, poolMc(kGfxDrawFenceOffset),
			};
			GfxColResult col;
			if (!gfxColDrawRun("col", vaCol, col)) {
				colHung = true;
				publishResult("gfx-col", "FAIL colour draw fence");
			} else {
				char colResult[192];
				snprintf(colResult, sizeof(colResult), "%s %s, draw %u px, max channel error %u.%02u, centroid 0x%08x in %llu us",
				         col.ok ? "PASS" : "FAIL", col.ok ? "colour triangle, THE COLOUR TRIANGLE IS RIGHT" : "colour image", col.covered,
				         col.maxChannelErr256 * 100 / 256 / 100, col.maxChannelErr256 * 100 / 256 % 100, col.centroid,
				         static_cast<unsigned long long>(col.ns / 1000));
				publishResult("gfx-col", colResult);
				env.owner->setProperty("Compute,GFXColPixels", static_cast<uint64_t>(col.covered), 32);
			}
		}
	}

	// The diagnostic ladder (rdna4-gfxdiag=<mask>, default off): the same stream with
	// one open question changed at a time, each logged with the same evidence, so that
	// round 4 sees which stage of the pipeline works. Only the baseline decides PASS.
	const uint32_t diag = requestedGfxDiag() | (requestedGfxDiag() && requestedGfxProbe() ? (32u | 64u | 128u | 256u | 512u | 1024u | 2048u | 4096u) : 0u);   // W37: the NGG marker joins the ladder of a probe boot
	if (diag && colHung)
		GLOG("diag ladder: skipped, the G4 colour draw did not finish and left the ring in an unknown state");
	if (diag && !colHung && (!base.ok || (diag & 16))) {
		char summary[400], probes[240];
		summary[0] = probes[0] = '\0';
		size_t used = 0, pused = 0;
		if (base.probeSeen)   // W33 S2: 'probe mid' equal/compared, baseline next to every variant
			pused += snprintf(probes, sizeof(probes), "baseline %u/%u", base.probeEqual, base.probeCounted);
		// By information and risk: the marker store (8) first, then the register-only variants
		// (2, 1), and the GS_ALLOC_REQ shader (4) last: it can hang the NGG pipeline and there is
		// no reset (W23 review S3), so boot 12 runs it alone (rdna4-gfxdiag=4). After a "hang/"
		// result the next boot needs a cold power cycle: the GC state survives a warm restart.
		// W45: the clip variants (128 clip off, 256 primitive filters off) and the NGG inputs (512) right after the marker: round 5 showed the
		// primitive reaches the clipper (C_INVOCATIONS 1) and C_PRIMITIVES reads 0 (supporting evidence only: pixels and PS_INVOCATIONS are the signal).
		// W46: round 6 on the card: cprim read 0 with CLIP_DISABLE (128) and the filters off (256) as well, so the two new variants come first: 1024 takes
		// nothing from the wave's VGPR inputs, 2048 removes the GE ring's GL2 hints.
		// The wave64 fallback (4096) right after the marker: RADV's NGG (GS) half for this draw, the other wave size (RADV also runs the PS as
		// wave64, PS_W32_EN = 0; 4096 keeps the PS wave32), proven on the card under Linux; if the wave32 baseline is empty on macOS but 4096
		// draws, the difference is the GS wave size there.
		static const uint32_t order[12] = { 32, 4096, 1024, 2048, 512, 128, 256, 64, 8, 2, 1, 4 };   // 32 (NGG marker: did the wave launch?) is one extra store, no more risk than 8
		for (uint32_t bit : order) {
			if (!(diag & bit))
				continue;
			char label[16];
			snprintf(label, sizeof(label), "diag %u", bit);
			GfxDrawResult r;
			const bool did = gfxDrawRun(label, bit, va, r);
			used += snprintf(summary + used, sizeof(summary) - used, "%s%u:%s%u", used ? " " : "", bit,
			                 did ? "" : "hang/", did ? r.covered : 0);
			if (bit == 8 && did)
				used += snprintf(summary + used, sizeof(summary) - used, "/marker %s",
				                 r.marker == kMarkerValue ? "yes" : "no");
			if ((bit == 32 || bit == 128 || bit == 256 || bit == 512 || bit == 1024 || bit == 2048 || bit == 4096) && did && r.probeSeen)
				used += snprintf(summary + used, sizeof(summary) - used, "/cprim %u", r.cPrim);
			if (bit == 64 && did)
				used += snprintf(summary + used, sizeof(summary) - used, "/primtype-idx");
			if (bit == 32 && did)
				used += snprintf(summary + used, sizeof(summary) - used, "/ngg %s",
				                 r.nggMarker == 0xc0de0002u ? "ran" : "NOT run");
			if (r.probeSeen && pused + 24 < sizeof(probes))
				pused += snprintf(probes + pused, sizeof(probes) - pused, " %u:%u/%u", bit, r.probeEqual, r.probeCounted);
			if (!did)
				break;   // a variant that did not finish leaves the ring in an unknown state
		}
		GLOG("diag ladder: baseline %u px; variants (bit:pixels) %s", base.covered, summary);
		if (base.probeSeen)   // a first read that lags shows here: the baseline below its variants
			GLOG("probe consistency (mid, equal/compared): %s%s", probes,
			     base.probeEqual < base.probeCounted ? " (baseline below its variants: the CP-side read lags the first draw too; "
			                                           "trust 'probe post' and the sentinel line)" : "");
		env.owner->setProperty("Compute,GFXDiag", summary);
	}
	if (requestedGfxVa0())
		gfxDumpVa0("after all draws");   // W37 #2
	gfxQueueEvidence("after draw");
	return base.ok;
}
