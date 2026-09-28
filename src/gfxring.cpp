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
#include "pm4.hpp"
#include "psred_kernel.h"

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
	// square): check the card's count first.
	const uint32_t gbAddr = rdGc(GbAddrConfig);
	const uint32_t ses = 1u << ((gbAddr >> 19) & 0xf);
	GLOG("draw: GB_ADDR_CONFIG 0x%08x: %u shader engines, %u RBs per SE, %u pipes", gbAddr, ses,
	     1u << ((gbAddr >> 26) & 3), 1u << (gbAddr & 7));
	if (gbAddr == 0xffffffff || ses > kMaxSe) {
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

	// Shaders, each followed by s_code_end padding for the SQ's prefetch.
	auto place = [&](uint32_t at, const uint32_t *code, uint32_t dwords) {
		for (uint32_t i = 0; i < 0x100; i++)
			*poolDw(at + 4 * i) = i < dwords ? code[i] : 0xbf9f0000u;   // s_code_end
	};
	place(kGfxVsOffset, kNggKernel, sizeof(kNggKernel) / 4);
	place(kGfxPsOffset, kPsredKernel, sizeof(kPsredKernel) / 4);

	// The command stream, its addresses filled in.
	constexpr uint32_t n = sizeof(kStream) / 4;
	volatile uint32_t *ib = poolDw(kGfxIbOffset);
	for (uint32_t i = 0; i < n; i++)
		ib[i] = kStream[i];
	for (const Reloc &r : kRelocs)
		ib[r.dword] = static_cast<uint32_t>((va[r.sym] >> r.shift) & r.mask);

	// The target, cleared; the fences, zero.
	for (uint32_t i = 0; i < kWidth * kHeight; i++)
		*poolDw(kGfxTargetOffset + 4 * i) = 0;
	*poolDw(kGfxDrawFenceOffset) = 0;
	flushHdp();
	GLOG("draw: %u-dword stream at MC 0x%llx, VS 0x%llx PS 0x%llx, target 0x%llx, rings 0x%llx "
	     "(%llu MiB)", n, poolMc(kGfxIbOffset), va[kVs], va[kPs], va[kCb], ringVa, kRingBytes >> 20);

	trail("gfx: first draw");
	uint32_t pkt[16];
	gfxRing.emit(pkt, Pm4::indirectBufferGfx(pkt, poolMc(kGfxIbOffset), n, 0));
	gfxRing.emit(pkt, Pm4::releaseMem(pkt, poolMc(kGfxFenceOffset), ++gfxFence));
	uint64_t t0 = mach_absolute_time();
	gfxKick(gfxRing.wptr());
	const bool ringDone = gfxFenceWait(gfxFence, 500000);
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	const uint32_t drawFence = *poolDw(kGfxDrawFenceOffset);
	if (!ringDone || drawFence != 1) {
		GLOG("draw: did not finish (ring fence %s, draw fence 0x%x)", ringDone ? "ok" : "NOT signalled",
		     drawFence);
		gfxStatus("draw");
		publishResult("gfx", "FAIL draw fence");
		return false;
	}

	// Read back: count the covered pixels, their bounds, and anything else
	// that changed.
	uint32_t covered = 0, other = 0, minX = kWidth, maxX = 0, minY = kHeight, maxY = 0;
	uint32_t row64[2] = { kWidth, 0 }, row190[2] = { kWidth, 0 };
	for (uint32_t y = 0; y < kHeight; y++) {
		for (uint32_t x = 0; x < kWidth; x++) {
			const uint32_t p = *poolDw(kGfxTargetOffset + 4 * (y * kWidth + x));
			if (p == kCoveredRgba) {
				covered++;
				minX = x < minX ? x : minX;
				maxX = x > maxX ? x : maxX;
				minY = y < minY ? y : minY;
				maxY = y > maxY ? y : maxY;
				uint32_t *span = y == 64 ? row64 : y == 190 ? row190 : nullptr;
				if (span) {
					span[0] = x < span[0] ? x : span[0];
					span[1] = x > span[1] ? x : span[1];
				}
			} else if (p) {
				other++;
			}
		}
	}
	// notes 3.9: 8192 pixels, rows 64..190, row 64 = x 64..191, row 190 = x 127..128.
	const bool ok = covered == kCoveredPixels && !other && minY == 64 && maxY == 190 &&
	                row64[0] == 64 && row64[1] == 191 && row190[0] == 127 && row190[1] == 128;
	GLOG("draw: %s in %llu us: %u pixels 0x%08x (want %u), %u others; bounds x %u..%u y %u..%u, "
	     "row 64 x %u..%u, row 190 x %u..%u", ok ? "THE TRIANGLE IS RIGHT" : "wrong image",
	     ns / 1000, covered, kCoveredRgba, kCoveredPixels, other, minX, maxX, minY, maxY, row64[0],
	     row64[1], row190[0], row190[1]);
	char result[160];
	snprintf(result, sizeof(result), "%s %s, draw %u px in %llu us",
	         ok ? "PASS" : "FAIL", ok ? "ring test, THE TRIANGLE IS RIGHT" : "draw image",
	         covered, static_cast<unsigned long long>(ns / 1000));
	publishResult("gfx", result);
	env.owner->setProperty("Compute,GFXDrawPixels", static_cast<uint64_t>(covered), 32);
	return ok;
}
