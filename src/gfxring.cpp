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
#include "ngg_kernel.h"
#include "nggmsg_kernel.h"
#include "pm4.hpp"
#include "psred_kernel.h"
#include "psstore_kernel.h"

#include "amdgpu/clearstate_defs.h"
#include "amdgpu/clearstate_gfx12.h"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/c++/OSDictionary.h>
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
	const uint32_t cntl = (bufsz & 0x3f) | (((bufsz - 2) & 0x3f) << 8);
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
	trail("gfx: clear state + ring");
	if (!gfxCsbInit())
		return finish(false, "clear-state buffer");
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
	return mask & 0x1f;   // bits 1,2,4,8 = variants, 16 = run them even if the baseline passed
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
struct ProbeReg { const char *name; uint8_t seg; uint16_t dword; uint8_t kind; };
constexpr uint32_t kProbeMax = 32;
const ProbeReg kProbe[] = {
	{ "VGT_SHADER_STAGES_EN", 1, 0x02a6, 0 }, { "CB_TARGET_MASK", 1, 0x0214, 0 }, { "CB_SHADER_MASK", 1, 0x0215, 0 },
	{ "CB_COLOR_CONTROL", 1, 0x0216, 0 }, { "CB_COLOR0_BASE", 1, 0x0318, 0 }, { "CB_COLOR0_ATTRIB2", 1, 0x031e, 0 },
	{ "CB_COLOR0_ATTRIB3", 1, 0x031f, 0 }, { "CB_COLOR0_INFO", 1, 0x03b0, 0 }, { "SPI_SHADER_POS_FORMAT", 1, 0x0193, 0 },
	{ "SPI_SHADER_COL_FORMAT", 1, 0x0195, 0 }, { "SPI_PS_INPUT_ENA", 1, 0x0197, 0 }, { "DB_SHADER_CONTROL", 1, 0x001b, 0 },
	{ "PA_CL_VTE_CNTL", 1, 0x0205, 0 }, { "PA_SC_SCREEN_SCISSOR_BR", 1, 0x0061, 0 }, { "PA_SC_VPORT_SCISSOR_0_BR", 1, 0x0095, 0 },
	{ "PGM_LO_ES", 0, 0x1a29, 1 }, { "PGM_LO_PS", 0, 0x19a8, 1 }, { "RSRC1_GS", 0, 0x1a2a, 1 }, { "RSRC1_PS", 0, 0x19aa, 1 },
	{ "VGT_PRIMITIVE_TYPE", 1, 0x2242, 2 }, { "GE_POS_RING_BASE", 1, 0x2268, 2 }, { "GE_PRIM_RING_BASE", 1, 0x226a, 2 },
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
void RDNA4Compute::gfxProbeReport(const char *label, uint32_t poolOff, volatile uint32_t *ib) {
	constexpr uint32_t n = sizeof(Gfx12Draw::kStream) / 4;
	char line[240], diff[300];
	uint32_t len = 0, match = 0, counted = 0, ndiff = 0, dl = 0;
	line[0] = diff[0] = '\0';
	for (uint32_t i = 0; i < kProbeCount; i++) {
		const uint32_t val = *poolDw(poolOff + 4 * i);
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
}

// The GC hub fault status, logged and cleared, so the log says which bring-up step first makes the
// CP read an unmapped address (round 4: CPG read VA 0, status 0x0d3d, already before the first draw).
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
// DC_BASE0/1; zero there would be a CPG read of VA 0.
void RDNA4Compute::gfxRs64Evidence(const char *tag) {
	grbmSelect(0, 0, 0, 0);
	GLOG("%s: RS64 DC_BASE0 0x%08x_%08x DC_BASE1 0x%08x_%08x DC_BASE_CNTL 0x%08x | PFP IC_BASE 0x%08x_%08x cntl 0x%08x | "
	     "ME IC_BASE 0x%08x_%08x cntl 0x%08x | INSTR_PNTR0/1 0x%x/0x%x", tag,
	     rdGc(CpRs64DcBase0Hi), rdGc(CpRs64DcBase0Lo), rdGc(CpRs64DcBase1Hi), rdGc(CpRs64DcBase1Lo),
	     rdGc(CpRs64DcBaseCntl), rdGc(CpPfpIcBaseHi), rdGc(CpPfpIcBaseLo), rdGc(CpPfpIcBaseCntl),
	     rdGc(CpMeIcBaseHi), rdGc(CpMeIcBaseLo), rdGc(CpMeIcBaseCntl), rdGc(CpGfxRs64InstrPntr0),
	     rdGc(CpGfxRs64InstrPntr1));
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
	else
		place(kGfxVsOffset, kNggKernel, sizeof(kNggKernel) / 4);
	if (variant & 8)
		place(kGfxPsOffset, kPsstoreKernel, sizeof(kPsstoreKernel) / 4);
	else
		place(kGfxPsOffset, kPsredKernel, sizeof(kPsredKernel) / 4);

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

	// The target, cleared; the fences and the marker, zero.
	for (uint32_t i = 0; i < kWidth * kHeight; i++)
		*poolDw(kGfxTargetOffset + 4 * i) = 0;
	*poolDw(kGfxDrawFenceOffset) = 0;
	*poolDw(kGfxMarkerOffset) = 0;
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
	gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
	uint64_t t0 = mach_absolute_time();
	gfxKick(gfxRing.wptr());
	r.ringDone = gfxFenceWait(gfxFence, 500000);
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	r.ns = ns;
	r.drawFence = *poolDw(kGfxDrawFenceOffset);
	gfxEvidence(label, true);
	if (split && r.ringDone) {
		char what[40];
		snprintf(what, sizeof(what), "%s probe mid", label);
		gfxProbeReport(what, kGfxProbeOffset, ib);
		snprintf(what, sizeof(what), "%s probe post", label);
		gfxProbeReport(what, kGfxProbePost, ib);
	}
	gfxFaultMark(label);
	if (!r.ringDone || r.drawFence != 1) {
		GLOG("%s: did not finish (ring fence %s, draw fence 0x%x)", label,
		     r.ringDone ? "ok" : "NOT signalled", r.drawFence);
		gfxStatus("draw");
		return false;
	}

	gfxCountTarget(r);
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
	gfxEvidence("before draw", false);
	const bool ran = gfxDrawRun("draw", 0, va, base);
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

	// The diagnostic ladder (rdna4-gfxdiag=<mask>, default off): the same stream with
	// one open question changed at a time, each logged with the same evidence, so that
	// round 4 sees which stage of the pipeline works. Only the baseline decides PASS.
	const uint32_t diag = requestedGfxDiag();
	if (diag && (!base.ok || (diag & 16))) {
		char summary[200];
		summary[0] = '\0';
		size_t used = 0;
		// By information and risk: the marker store (8) first, then the register-only variants
		// (2, 1), and the GS_ALLOC_REQ shader (4) last: it can hang the NGG pipeline and there is
		// no reset (W23 review S3), so boot 12 runs it alone (rdna4-gfxdiag=4). After a "hang/"
		// result the next boot needs a cold power cycle: the GC state survives a warm restart.
		static const uint32_t order[4] = { 8, 2, 1, 4 };
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
			if (!did)
				break;   // a variant that did not finish leaves the ring in an unknown state
		}
		GLOG("diag ladder: baseline %u px; variants (bit:pixels) %s", base.covered, summary);
		env.owner->setProperty("Compute,GFXDiag", summary);
	}
	return base.ok;
}
