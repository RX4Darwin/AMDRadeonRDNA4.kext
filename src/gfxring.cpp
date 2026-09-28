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
//  Boot-arg rdna4-gfx: 1 = the write pointer goes to CP_RB0_WPTR (MMIO),
//  2 = through the gfx ring's doorbell, as amdgpu kicks it. Absent = off:
//  nothing here runs. It runs after the compute stages on the bring-up
//  thread; a failure halts PFP/ME again and leaves compute as it was.
//

#include "compute.hpp"
#include "pm4.hpp"

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
	return mode == 1 || mode == 2 ? mode : 0;
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
	if (gfxMode == 2) {
		db |= ((kGfxDoorbellDword << 2) & 0x0ffffffc) | kCpRbDoorbellEn;
		wr(IpDiscovery::HwGc, CpRbDoorbellControl, db);
		wr(IpDiscovery::HwGc, CpRbDoorbellRangeLower, (kGfxDoorbellDword << 2) & kCpRbDoorbellRangeMask);
		wr(IpDiscovery::HwGc, CpRbDoorbellRangeUpper, kCpRbDoorbellRangeMask);
	} else {
		wr(IpDiscovery::HwGc, CpRbDoorbellControl, db);
	}
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
	     gfxMode == 2 ? "doorbell" : "MMIO write pointer", rdGc(CpMeCntl), stat,
	     stat ? " (did not idle: amdgpu reports and goes on)" : "");
	return true;
}

void RDNA4Compute::gfxKick(uint64_t wptrDwords) {
	*poolDw(kGfxWptrOffset) = static_cast<uint32_t>(wptrDwords);
	*poolDw(kGfxWptrOffset + 4) = static_cast<uint32_t>(wptrDwords >> 32);
	flushHdp();
	if (gfxMode == 2) {
		doorbells[kGfxDoorbellDword / 2] = wptrDwords;
	} else {
		grbmSelect(0, 0, 0, 0);
		wr(IpDiscovery::HwGc, CpRb0WptrHi, static_cast<uint32_t>(wptrDwords >> 32));
		wr(IpDiscovery::HwGc, CpRb0Wptr, static_cast<uint32_t>(wptrDwords));
	}
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
	if (gfxMode == 2 && !doorbells)
		return finish(false, "doorbell mode without the doorbell BAR");
	trail("gfx: clear state + ring");
	if (!gfxCsbInit())
		return finish(false, "clear-state buffer");
	if (!gfxRingResume())
		return finish(false, "ring setup");

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

	GLOG("gfx ring ready (%s)", gfxMode == 2 ? "doorbell" : "MMIO write pointer");
	return finish(true, "gfx ring");
}
