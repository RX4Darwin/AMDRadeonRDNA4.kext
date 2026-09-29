//
//  compute.cpp
//  RDNA4FB
//
//  See compute.hpp. Stage 1 (survey) only reads registers. Stage 2 is the
//  first to write: PSP mailbox/ring registers, the HDP flush, and the compute
//  pool in VRAM — never DCN, the MM hub or the scanout.
//

#include "codeobj.hpp"
#include "compute.hpp"
#include "flip.hpp"
#include "pm4.hpp"
#include "probe_kernel.h"
#include "sdma.hpp"
#include "smu_metrics.h"
#include "vadd_codeobj.h"

#include <kern/clock.h>

#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

#define CLOG(fmt, ...)  IOLog("RDNA4FB: compute: " fmt "\n", ## __VA_ARGS__)

// Lilu's vendor GUID; `nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trail`
// reads it back.
static const char *kTrailKey = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trail";
// IONVRAM's "commit now, unthrottled" request key (IOKitKeys.h,
// kIONVRAMForceSyncNowPropertyKey), handled by IODTNVRAM::setProperties.
static const char *kNvramForceSync = "IONVRAM-FORCESYNCNOW-PROPERTY";

using namespace GfxReg;

#ifndef RDNA4FB_NO_FIRMWARE
// src/fwblobs.S: linux-firmware blobs embedded at build time.
extern "C" {
extern const uint8_t rdna4_fw_psp_sos[], rdna4_fw_psp_sos_end[];
extern const uint8_t rdna4_fw_smu[], rdna4_fw_smu_end[];
extern const uint8_t rdna4_fw_sdma[], rdna4_fw_sdma_end[];
extern const uint8_t rdna4_fw_pfp[], rdna4_fw_pfp_end[];
extern const uint8_t rdna4_fw_me[], rdna4_fw_me_end[];
extern const uint8_t rdna4_fw_mec[], rdna4_fw_mec_end[];
extern const uint8_t rdna4_fw_mes[], rdna4_fw_mes_end[];
extern const uint8_t rdna4_fw_imu[], rdna4_fw_imu_end[];
extern const uint8_t rdna4_fw_rlc[], rdna4_fw_rlc_end[];
}
#define FW_BLOB(n) AmdFw::Blob { rdna4_fw_##n, static_cast<uint32_t>(rdna4_fw_##n##_end - rdna4_fw_##n) }
#endif

namespace {
constexpr uint32_t kBad = 0xFFFFFFFF;
constexpr uint64_t kMiB = 1ull << 20;

// RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE (NBIF 6.3.1 seg 2): usable VRAM in MiB.
constexpr Reg NbifMemSize { 2, 0x00c3 };

// Display memory the GOP set up (DCN 4.1, seg 2; the same registers the
// display side uses): the DMUB mailbox (REGION4, MC address) and each
// HUBP's primary surface (MC address, per-pipe stride 0xdc).
constexpr Reg DmcubRegion4Offset     { 2, 0x0196 };
constexpr Reg DmcubRegion4OffsetHigh { 2, 0x0197 };
constexpr uint32_t kHubpSurfaceLo = 0x060a, kHubpSurfaceHi = 0x060b, kHubpStride = 0xdc;

const char *onOff(bool v) { return v ? "yes" : "no"; }

// A VMx FB aperture (MC address >> 24 in [23:0]) is set when it spans at
// least the VRAM the card reports. Zeroed registers read as a 16 MiB range
// at 0, which is not a configured aperture.
bool fbApertureSet(uint32_t base, uint32_t top, uint32_t vramMiB) {
	if (base == kBad || top == kBad)
		return false;
	base &= 0xffffff;
	top &= 0xffffff;
	if (top < base)
		return false;
	uint64_t spanMiB = (static_cast<uint64_t>(top - base) + 1) * 16;
	return vramMiB != kBad && vramMiB && spanMiB >= vramMiB;
}
} // namespace

void RDNA4Compute::publishResult(const char *feature, const char *value) {
	if (!feature || !value || !env.owner || !resultLock)
		return;
	IOLockLock(resultLock);
	OSDictionary *next = resultDictionary
		? OSDictionary::withDictionary(resultDictionary, 12)
		: OSDictionary::withCapacity(12);
	if (next) {
		if (OSString *s = OSString::withCString(value)) {
			next->setObject(feature, s);
			s->release();
			env.owner->setProperty("RDNA4FB,Results", next);
			if (resultDictionary)
				resultDictionary->release();
			resultDictionary = next;
		} else {
			next->release();
		}
	}
	IOLockUnlock(resultLock);
}

uint32_t RDNA4Compute::requestedStage() {
	uint32_t stage = 0;
	if (!PE_parse_boot_argn("rdna4-compute", &stage, sizeof(stage)))
		return StageOff;
	return stage > StageKernel ? StageKernel : stage;
}

bool RDNA4Compute::requestedVm() {
	uint32_t enabled = 0;
	return PE_parse_boot_argn("rdna4-vm", &enabled, sizeof(enabled)) && enabled != 0;
}

bool RDNA4Compute::requestedPowerManagement() {
	uint32_t enabled = 0;
	return PE_parse_boot_argn("rdna4-pm", &enabled, sizeof(enabled)) && enabled != 0;
}

uint32_t RDNA4Compute::rd(uint16_t hwId, const Reg &r) const {
	uint32_t off;
	if (!env.mmio || !env.disc || !env.disc->regByteOffset(hwId, 0, r.seg, r.dword, off) ||
	    off + 4 > env.mmioSize)
		return kBad;
	if (hwId == IpDiscovery::HwGc) {
		GcAccess g(*this, (static_cast<uint32_t>(r.seg) << 16) | r.dword);   // W27: never touch a GC block that GFXOFF powered down
		if (!g.ok)
			return kBad;
		return env.mmio[off / 4];
	}
	return env.mmio[off / 4];
}

void RDNA4Compute::wr(uint16_t hwId, const Reg &r, uint32_t value) {
	uint32_t off;
	if (!env.mmio || !env.disc || !env.disc->regByteOffset(hwId, 0, r.seg, r.dword, off) ||
	    off + 4 > env.mmioSize)
		return;
	if (hwId == IpDiscovery::HwGc) {
		GcAccess g(*this, (static_cast<uint32_t>(r.seg) << 16) | r.dword);   // W27: see rd()
		if (g.ok)
			env.mmio[off / 4] = value;
		return;
	}
	env.mmio[off / 4] = value;
}

// ---------------------------------------------------------------------------
// Stage 1: survey
// ---------------------------------------------------------------------------

bool RDNA4Compute::survey() {
	sv = Survey {};

	IpDiscovery::IpEntry gc {};
	if (!env.disc->findIp(IpDiscovery::HwGc, 0, gc)) {
		CLOG("survey: no GC block in the IP discovery table");
		return false;
	}
	sv.gcMajor = gc.major;
	sv.gcMinor = gc.minor;
	sv.gcRev = gc.revision;

	// GC first. All-ones from GRBM_STATUS means the block does not answer
	// (powered down or out of the BAR5 window): skip the rest of GC then.
	sv.grbmStatus = rdGc(GrbmStatus);
	sv.gcReadable = sv.grbmStatus != kBad;
	if (sv.gcReadable) {
		sv.grbmStatus2 = rdGc(GrbmStatus2);
		sv.cpStat      = rdGc(CpStat);
		sv.cpcStatus   = rdGc(CpCpcStatus);
		sv.cpfStatus   = rdGc(CpCpfStatus);
		sv.cpMeCntl    = rdGc(CpMeCntl);
		sv.mecCntl     = rdGc(CpMecRs64Cntl);
		sv.mesCntl     = rdGc(CpMesCntl);
		sv.pfpPc       = rdGc(CpPfpInstrPntr);
		sv.mePc        = rdGc(CpMeInstrPntr);
		sv.mecPc       = rdGc(CpMecRs64InstrPntr);
		sv.rlcCntl     = rdGc(RlcCntl);
		sv.rlcStat     = rdGc(RlcStat);
		sv.rlcGpmStat  = rdGc(RlcGpmStat);
		sv.rlcBootload = rdGc(RlcBootloadStatus);
		sv.imuCoreCtrl = rdGc(ImuCoreCtrl);
		sv.imuGfxReset = rdGc(ImuGfxResetCtrl);
		for (uint32_t i = 0; i < kSdmaInstances; i++) {
			sv.sdmaMcuCntl[i]  = rdGc(sdma(i, SdmaMcuCntl));
			sv.sdmaStatus[i]   = rdGc(sdma(i, SdmaStatusReg));
			sv.sdmaUcodeRev[i] = rdGc(sdma(i, SdmaUcodeRev));
			sv.sdmaRbCntl[i]   = rdGc(sdma(i, SdmaQ0RbCntl));
		}
		sv.gcFbBase   = rdGc(GcFbLocationBase);
		sv.gcFbTop    = rdGc(GcFbLocationTop);
		sv.gcFbOffset = rdGc(GcFbOffset);
		sv.gcAgpBase  = rdGc(GcAgpBase);
		sv.gcAgpBot   = rdGc(GcAgpBot);
		sv.gcAgpTop   = rdGc(GcAgpTop);
		sv.gcSysLow   = rdGc(GcSysApertureLow);
		sv.gcL2Cntl   = rdGc(GcL2Cntl);
		sv.gcCtx0Cntl = rdGc(GcCtx0Cntl);
		sv.gcPtLo     = rdGc(GcCtx0PtBaseLo);
		sv.gcPtHi     = rdGc(GcCtx0PtBaseHi);
	}

	sv.mmFbBase   = rd(IpDiscovery::HwMmhub, MmFbLocationBase);
	sv.mmFbTop    = rd(IpDiscovery::HwMmhub, MmFbLocationTop);
	sv.mmFbOffset = rd(IpDiscovery::HwMmhub, MmFbOffset);
	sv.mmAgpBase  = rd(IpDiscovery::HwMmhub, MmAgpBase);
	sv.mmAgpBot   = rd(IpDiscovery::HwMmhub, MmAgpBot);
	sv.mmAgpTop   = rd(IpDiscovery::HwMmhub, MmAgpTop);
	sv.mmSysLow   = rd(IpDiscovery::HwMmhub, MmSysApertureLow);
	sv.mmSysHigh  = rd(IpDiscovery::HwMmhub, MmSysApertureHigh);
	sv.mmL2Cntl   = rd(IpDiscovery::HwMmhub, MmL2Cntl);
	sv.mmCtx0Cntl = rd(IpDiscovery::HwMmhub, MmCtx0Cntl);
	sv.mmPtLo     = rd(IpDiscovery::HwMmhub, MmCtx0PtBaseLo);
	sv.mmPtHi     = rd(IpDiscovery::HwMmhub, MmCtx0PtBaseHi);

	sv.pspBoot = rd(IpDiscovery::HwMp0, PspBootStatus);
	sv.pspRing = rd(IpDiscovery::HwMp0, PspRingStatus);
	sv.pspSos  = rd(IpDiscovery::HwMp0, PspSosVersion);

	sv.vramMiB = rd(IpDiscovery::HwNbif, NbifMemSize);
	sv.hdpMemFlushRemap = rd(IpDiscovery::HwNbif, NbifRemapHdpMemFlush);
	sv.hdpRegFlushRemap = rd(IpDiscovery::HwNbif, NbifRemapHdpRegFlush);

	// The VRAM MC range the MM hub (display, PSP) was set up with. The
	// field is the MC address in 16 MiB units.
	if (fbApertureSet(sv.mmFbBase, sv.mmFbTop, sv.vramMiB)) {
		sv.fbMcBase = static_cast<uint64_t>(sv.mmFbBase & 0xffffff) << 24;
		sv.fbMcTop  = (static_cast<uint64_t>(sv.mmFbTop & 0xffffff) << 24) | 0xffffff;
	}

	sv.scanoutOffset = ~0ull;
	if (IODeviceMemory *bar0 = env.pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)) {
		sv.bar0Phys = bar0->getPhysicalAddress();
		sv.bar0Size = bar0->getLength();
		if (env.scanoutPhys >= sv.bar0Phys && env.scanoutPhys < sv.bar0Phys + sv.bar0Size)
			sv.scanoutOffset = env.scanoutPhys - sv.bar0Phys;
	}
	if (IODeviceMemory *bar2 = env.pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2))
		sv.bar2Size = bar2->getLength();
	return true;
}

void RDNA4Compute::logSurvey() const {
	CLOG("GC %u.%u.%u, registers %s", sv.gcMajor, sv.gcMinor, sv.gcRev,
	     sv.gcReadable ? "readable" : "NOT readable (GRBM_STATUS all-ones)");
	uint32_t gcSe = 0, gcRb = 0, gcVer = 0;
	if (env.disc && env.disc->gcInfo(gcSe, gcRb, &gcVer))
		CLOG("IP discovery gc_info %u.%u: %u shader engines, %u RBs per SE", gcVer >> 16, gcVer & 0xffff, gcSe, gcRb);
	else
		CLOG("IP discovery gc_info: absent or not a GC table");
	if (sv.gcReadable) {
		CLOG("grbm status=0x%08x status2=0x%08x cp_stat=0x%08x cpc=0x%08x cpf=0x%08x",
		     sv.grbmStatus, sv.grbmStatus2, sv.cpStat, sv.cpcStatus, sv.cpfStatus);
		CLOG("cp me_cntl=0x%08x mec_rs64_cntl=0x%08x mes_cntl=0x%08x pc pfp=0x%x me=0x%x mec=0x%x",
		     sv.cpMeCntl, sv.mecCntl, sv.mesCntl, sv.pfpPc, sv.mePc, sv.mecPc);
		CLOG("rlc cntl=0x%08x stat=0x%08x gpm_stat=0x%08x bootload=0x%08x",
		     sv.rlcCntl, sv.rlcStat, sv.rlcGpmStat, sv.rlcBootload);
		CLOG("imu core_ctrl=0x%08x gfx_reset_ctrl=0x%08x", sv.imuCoreCtrl, sv.imuGfxReset);
		for (uint32_t i = 0; i < kSdmaInstances; i++)
			CLOG("sdma%u mcu_cntl=0x%08x status=0x%08x ucode_rev=0x%08x rb_cntl=0x%08x", i,
			     sv.sdmaMcuCntl[i], sv.sdmaStatus[i], sv.sdmaUcodeRev[i], sv.sdmaRbCntl[i]);
		CLOG("gchub fb base=0x%08x top=0x%08x offset=0x%08x agp base=0x%08x bot=0x%08x top=0x%08x "
		     "sys_low=0x%08x", sv.gcFbBase, sv.gcFbTop, sv.gcFbOffset, sv.gcAgpBase, sv.gcAgpBot,
		     sv.gcAgpTop, sv.gcSysLow);
		CLOG("gchub l2_cntl=0x%08x ctx0_cntl=0x%08x ctx0_pt=0x%08x%08x", sv.gcL2Cntl,
		     sv.gcCtx0Cntl, sv.gcPtHi, sv.gcPtLo);
	}
	CLOG("mmhub fb base=0x%08x top=0x%08x offset=0x%08x agp base=0x%08x bot=0x%08x top=0x%08x "
	     "sys 0x%08x..0x%08x", sv.mmFbBase, sv.mmFbTop, sv.mmFbOffset, sv.mmAgpBase, sv.mmAgpBot,
	     sv.mmAgpTop, sv.mmSysLow, sv.mmSysHigh);
	CLOG("mmhub l2_cntl=0x%08x ctx0_cntl=0x%08x ctx0_pt=0x%08x%08x", sv.mmL2Cntl, sv.mmCtx0Cntl,
	     sv.mmPtHi, sv.mmPtLo);
	CLOG("psp boot=0x%08x ring=0x%08x sos=0x%08x", sv.pspBoot, sv.pspRing, sv.pspSos);
	CLOG("hdp flush remap mem=0x%08x reg=0x%08x", sv.hdpMemFlushRemap, sv.hdpRegFlushRemap);
	CLOG("memory: VRAM %u MiB, MC 0x%llx..0x%llx, BAR0 0x%llx (%llu MiB), BAR2 %llu KiB, "
	     "scanout at VRAM+0x%llx (%llu KiB)", sv.vramMiB, sv.fbMcBase, sv.fbMcTop, sv.bar0Phys,
	     sv.bar0Size / kMiB, sv.bar2Size / 1024, sv.scanoutOffset, env.scanoutLength / 1024);

	// What the numbers mean for the next stages.
	const bool pspAlive = sv.pspSos != 0 && sv.pspSos != kBad;
	CLOG("verdict: PSP sOS %s, bootloader %s, KM ring %s", pspAlive ? "RUNNING" : "not running",
	     (sv.pspBoot != kBad && (sv.pspBoot & 0x80000000u)) ? "ready" : "not ready",
	     (sv.pspRing != kBad && (sv.pspRing & 0x80000000u)) ? "created" : "not created");
	if (sv.gcReadable) {
		const bool rlcOn   = sv.rlcCntl & kRlcEnableF32;
		const bool rlcDone = sv.rlcBootload & kRlcBootComplete;
		const bool imuOn   = !(sv.imuCoreCtrl & kImuCoreReset);
		const bool gfxOut  = (sv.imuGfxReset & kImuGfxOutOfReset) == kImuGfxOutOfReset;
		// A clear halt bit only says the core is not stopped; firmware in it
		// is shown by RLC bootload and SDMA ucode-init, not by these.
		CLOG("verdict: GFX: IMU %s, GFX domains %s, RLC %s (bootload %s), PFP %s, ME %s, "
		     "MEC %s (pipes 0x%x), MES %s",
		     imuOn ? "out of reset" : "held in reset", gfxOut ? "out of reset" : "in reset",
		     rlcOn ? "enabled" : "disabled", rlcDone ? "complete" : "not complete",
		     (sv.cpMeCntl & kCpMePfpHalt) ? "halted" : "not halted",
		     (sv.cpMeCntl & kCpMeMeHalt) ? "halted" : "not halted",
		     (sv.mecCntl & kRs64Halt) ? "halted" : "not halted",
		     (sv.mecCntl >> kRs64PipeActiveShift) & 0xf,
		     (sv.mesCntl & kRs64Halt) ? "halted" : "not halted");
		for (uint32_t i = 0; i < kSdmaInstances; i++)
			CLOG("verdict: SDMA%u: MCU %s, ucode init %s, queue0 %s", i,
			     (sv.sdmaMcuCntl[i] & kSdmaMcuHalt) ? "halted" : "not halted",
			     (sv.sdmaStatus[i] & kSdmaUcodeInitDone) ? "done" : "not done",
			     (sv.sdmaRbCntl[i] & kSdmaRbEnable) ? "enabled" : "disabled");
		CLOG("verdict: GC hub FB aperture %s, L2 %s, VMID0 %s",
		     fbApertureSet(sv.gcFbBase, sv.gcFbTop, sv.vramMiB) ? "set" : "NOT set",
		     onOff(sv.gcL2Cntl & kVmL2Enable), onOff(sv.gcCtx0Cntl & kVmCtxEnable));
	}
	CLOG("verdict: MM hub FB aperture %s, L2 %s, VMID0 %s",
	     sv.fbMcTop ? "set" : "NOT set", onOff(sv.mmL2Cntl & kVmL2Enable),
	     onOff(sv.mmCtx0Cntl & kVmCtxEnable));
	if (pool.valid)
		CLOG("pool: VRAM+0x%llx, %llu MiB (MC 0x%llx) reserved for compute", pool.offset,
		     pool.size / kMiB, pool.mcAddress);
	else
		CLOG("pool: no VRAM window found for compute");
}

void RDNA4Compute::publishSurvey() const {
	OSDictionary *d = OSDictionary::withCapacity(40);
	if (!d)
		return;
	auto put = [d](const char *key, uint64_t v, uint32_t bits) {
		if (OSNumber *n = OSNumber::withNumber(v, bits)) {
			d->setObject(key, n);
			n->release();
		}
	};
	put("GCVersion", (sv.gcMajor << 16) | (sv.gcMinor << 8) | sv.gcRev, 32);
	put("GRBM_STATUS", sv.grbmStatus, 32);
	put("GRBM_STATUS2", sv.grbmStatus2, 32);
	put("CP_ME_CNTL", sv.cpMeCntl, 32);
	put("CP_MEC_RS64_CNTL", sv.mecCntl, 32);
	put("CP_MES_CNTL", sv.mesCntl, 32);
	put("RLC_CNTL", sv.rlcCntl, 32);
	put("RLC_BOOTLOAD_STATUS", sv.rlcBootload, 32);
	put("IMU_CORE_CTRL", sv.imuCoreCtrl, 32);
	put("IMU_GFX_RESET_CTRL", sv.imuGfxReset, 32);
	put("SDMA0_MCU_CNTL", sv.sdmaMcuCntl[0], 32);
	put("SDMA0_STATUS", sv.sdmaStatus[0], 32);
	put("SDMA1_MCU_CNTL", sv.sdmaMcuCntl[1], 32);
	put("SDMA1_STATUS", sv.sdmaStatus[1], 32);
	put("GCMC_FB_BASE", sv.gcFbBase, 32);
	put("GCMC_FB_TOP", sv.gcFbTop, 32);
	put("GCVM_L2_CNTL", sv.gcL2Cntl, 32);
	put("GCVM_CTX0_CNTL", sv.gcCtx0Cntl, 32);
	put("MMMC_FB_BASE", sv.mmFbBase, 32);
	put("MMMC_FB_TOP", sv.mmFbTop, 32);
	put("MMVM_L2_CNTL", sv.mmL2Cntl, 32);
	put("MMVM_CTX0_CNTL", sv.mmCtx0Cntl, 32);
	put("PSP_SOS", sv.pspSos, 32);
	put("PSP_BOOT", sv.pspBoot, 32);
	put("HDP_MEM_FLUSH_REMAP", sv.hdpMemFlushRemap, 32);
	put("VRAM_MiB", sv.vramMiB, 32);
	put("ScanoutOffset", sv.scanoutOffset, 64);
	put("PoolOffset", pool.valid ? pool.offset : 0, 64);
	put("PoolSize", pool.valid ? pool.size : 0, 64);
	env.owner->setProperty("Compute,Survey", d);
	d->release();
}

// Pick the compute window: 64 MiB in the CPU-visible aperture, 16 MiB
// aligned and past the scanout, never below 128 MiB (the GOP and the
// pre-OS console live at the bottom of VRAM). Reserved only in the sense
// that the display path never touches it; nothing is written in stage 1.
void RDNA4Compute::choosePool() {
	pool = Pool {};
	if (!sv.bar0Size || !sv.fbMcTop || sv.scanoutOffset == ~0ull)
		return;
	// The bring-up needs 64 MiB (its layout ends below kHeapOffset); the rest
	// of BAR0, up to 128 MiB in all, becomes the user-space heap.
	constexpr uint64_t kAlign = 16 * kMiB, kNeed = 64 * kMiB, kMost = 128 * kMiB;
	constexpr uint64_t kFloor = 128 * kMiB;
	uint64_t start = (sv.scanoutOffset + env.scanoutLength + kAlign - 1) & ~(kAlign - 1);
	if (start < kFloor)
		start = kFloor;
	if (start + kNeed > sv.bar0Size)
		return;
	const uint64_t room = (sv.bar0Size - start) & ~(kAlign - 1);
	pool.offset = start;
	pool.size = room < kMost ? room : kMost;
	pool.mcAddress = sv.fbMcBase + start;
	pool.valid = pool.mcAddress + pool.size - 1 <= sv.fbMcTop;

	// Nothing the display depends on may live in the pool: the compute
	// stages overwrite it. Seen on the RX 9070 XT, the DMUB memory is at the
	// top of VRAM and the one lit surface at VRAM+0; if a card differs, it
	// runs without compute rather than without a display.
	auto clash = [this](const char *what, uint64_t mc) {
		if (!mc || mc == ~0ull || (mc & 0xffffffffull) == kBad || mc < sv.fbMcBase)
			return false;
		const uint64_t off = mc - sv.fbMcBase;
		const bool in = off >= pool.offset && off < pool.offset + pool.size;
		CLOG("display memory: %s at VRAM+0x%llx%s", what, off, in ? " — INSIDE the compute pool" : "");
		return in;
	};
	const uint64_t dmubMc = rd(IpDiscovery::HwDmu, DmcubRegion4Offset) |
	                        (static_cast<uint64_t>(rd(IpDiscovery::HwDmu, DmcubRegion4OffsetHigh)) << 32);
	if ((dmubMc & 0xffffffffull) != kBad && dmubMc > sv.fbMcBase)
		dmubVram = dmubMc - sv.fbMcBase;           // the device heap stays below it
	bool hit = clash("DMUB mailbox (region4)", dmubMc);
	for (uint32_t i = 0; i < 4; i++) {
		const char *names[4] = { "HUBP0 surface", "HUBP1 surface", "HUBP2 surface", "HUBP3 surface" };
		const uint32_t lo = rd(IpDiscovery::HwDmu, Reg { 2, kHubpSurfaceLo + i * kHubpStride });
		const uint32_t hi = rd(IpDiscovery::HwDmu, Reg { 2, kHubpSurfaceHi + i * kHubpStride });
		if (lo != kBad && hi != kBad)
			hit |= clash(names[i], lo | (static_cast<uint64_t>(hi & 0xffff) << 32));
	}
	if (hit && pool.valid) {
		CLOG("pool: display memory lies in VRAM+0x%llx..+0x%llx; compute disabled to keep the "
		     "display safe", pool.offset, pool.offset + pool.size);
		pool.valid = false;
	}
}

// ---------------------------------------------------------------------------

uint32_t RDNA4Compute::start(const Env &e, uint32_t stage) {
	env = e;
	uint32_t hang = 0;
	hangRecoveryEnabled = PE_parse_boot_argn("rdna4-hang", &hang, sizeof(hang)) && hang != 0;
	CLOG("queue hang recovery %s (rdna4-hang=%u)", hangRecoveryEnabled ? "enabled" : "disabled", hang);
	vmEnabled = requestedVm();
	if (stage == StageOff)
		return StageOff;
	if (!env.pci || !env.owner || !env.mmio || !env.disc || !env.disc->isValid()) {
		CLOG("no MMIO or IP discovery table, compute bring-up skipped");
		return StageOff;
	}
	CLOG("bring-up to stage %u requested", stage);
	if (!rtLock)
		rtLock = IOLockAlloc();
	if (!smuLock)
		smuLock = IOLockAlloc();
	if (!gcLock)
		gcLock = IOLockAlloc();
        if (!resultLock)
                resultLock = IOLockAlloc();
        if (rtLock) {
                IOLockLock(rtLock);
                shutdownQuiesced = false;
                bringupRunning = false;
                IOLockUnlock(rtLock);
        }
        registerShutdownInterest();
        publishResult("runtime", "SKIPPED bring-up in progress");

	gfxOffPreflight(true);
	if (gcState == kGcHold)
		return StageOff;
	if (!survey())
		return StageOff;
	// A warm restart can leave the GOP's engine state live. Survey first so a
	// cold card's reset values never cause writes into engines still in reset.
	defensiveStart();
	choosePool();
	logSurvey();
	publishSurvey();

	if (stage >= StagePsp) {
		target = stage;
		thread_t th = nullptr;
		if (kernel_thread_start(threadMain, this, &th) == KERN_SUCCESS) {
			thread_deallocate(th);
			CLOG("stages 2..%u continue on the bring-up thread", stage);
		} else {
			CLOG("could not start the bring-up thread; stopping after the survey");
		}
	}
	return StageSurvey;
}

// ---------------------------------------------------------------------------
// NVRAM breadcrumbs
// ---------------------------------------------------------------------------

// Only IORegistryEntry's own virtuals are called on the NVRAM entry: they
// keep their vtable slots across releases. IODTNVRAM-specific virtuals
// (sync, safeToSync) moved on Tahoe — calling them with an older SDK's
// layout panics (seen in the VM through Lilu's NVStorage::sync).
void RDNA4Compute::trail(const char *step) {
	IORegistryEntry *nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!nvram)
		return;
	const OSSymbol *key = OSSymbol::withCString(kTrailKey);
	OSString *value = OSString::withCString(step);
	OSDictionary *sync = OSDictionary::withCapacity(1);
	OSString *yes = OSString::withCString("1");
	if (key && value)
		nvram->setProperty(key, value);
	if (sync && yes && sync->setObject(kNvramForceSync, yes))
		nvram->setProperties(sync);
	OSSafeReleaseNULL(yes);
	OSSafeReleaseNULL(sync);
	OSSafeReleaseNULL(value);
	OSSafeReleaseNULL(key);
	nvram->release();
}

bool RDNA4Compute::logPreviousTrail(char *text, size_t size) {
	text[0] = 0;
	IORegistryEntry *nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!nvram)
		return false;
	if (OSObject *prev = nvram->copyProperty(kTrailKey)) {
		if (auto *s = OSDynamicCast(OSString, prev))
			strlcpy(text, s->getCStringNoCopy(), size);
		else if (auto *d = OSDynamicCast(OSData, prev)) {
			const size_t n = d->getLength() < size - 1 ? d->getLength() : size - 1;
			memcpy(text, d->getBytesNoCopy(), n);
			text[n] = 0;
		}
		prev->release();
	}
	nvram->release();
	if (!text[0]) {
		CLOG("no bring-up trail from a previous boot");
		return false;
	}
	CLOG("previous boot's bring-up ended at: %s", text);
	// Every way a bring-up ends on its own leaves one of these; anything
	// else is the step that was running when that boot died.
	return strncmp(text, "finished", 8) && strncmp(text, "stopped", 7) &&
	       strncmp(text, "skipped", 7);
}

// ---------------------------------------------------------------------------
// Bring-up thread
// ---------------------------------------------------------------------------

void RDNA4Compute::threadMain(void *arg, wait_result_t) {
	// Let the desktop come up first: nothing below is on its path.
	IOSleep(5000);
	static_cast<RDNA4Compute *>(arg)->runStages();
	thread_terminate(current_thread());
}

void RDNA4Compute::resumeMain(void *arg, wait_result_t) {
	auto *self = static_cast<RDNA4Compute *>(arg);
	CLOG("power: wake received; re-bring-up scheduled on the bring-up thread");
	if (!self->beginBringup()) {
		CLOG("power: wake bring-up cancelled by shutdown");
		thread_terminate(current_thread());
		return;
	}
	const bool stopped = !self->bringupStepAllowed("resume reset");
	if (!stopped)
	self->resetRuntimeForResume();
	if (!stopped)
		self->runStages();
	self->endBringup();
	if (self->rtLock) {
		IOLockLock(self->rtLock);
		self->resumePending = false;
		if (!self->rtReady)
			self->powerSleeping = false;
		IOLockUnlock(self->rtLock);
	}
	thread_terminate(current_thread());
}

void RDNA4Compute::powerDidWake() {
	if (!rtLock)
		return;
	IOLockLock(rtLock);
	if (!powerSleeping || resumePending) {
		IOLockUnlock(rtLock);
		return;
	}
	resumePending = true;
	IOLockUnlock(rtLock);
	thread_t th = nullptr;
	if (kernel_thread_start(resumeMain, this, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	} else {
		IOLockLock(rtLock);
		resumePending = false;
		IOLockUnlock(rtLock);
		CLOG("power: could not start the wake bring-up thread; runtime stays not ready");
	}
}

bool RDNA4Compute::beginBringup() {
	if (!rtLock)
		return false;
	IOLockLock(rtLock);
	const bool allowed = !shutdownQuiesced;
	if (allowed)
		bringupRunning = true;
	IOLockUnlock(rtLock);
	return allowed;
}

void RDNA4Compute::endBringup() {
	if (!rtLock)
		return;
	IOLockLock(rtLock);
	bringupRunning = false;
	IOLockWakeup(rtLock, &bringupRunning, false);
	IOLockUnlock(rtLock);
}

bool RDNA4Compute::bringupStepAllowed(const char *step) {
	if (!rtLock)
		return false;
	IOLockLock(rtLock);
	const bool allowed = !shutdownQuiesced;
	IOLockUnlock(rtLock);
        if (!allowed) {
                CLOG("bring-up stopped before %s: shutdown is quiesced", step ? step : "next step");
                trail("stopped: shutdown");
        }
        return allowed;
}

void RDNA4Compute::runStages() {
	bool ownsBringup = false;
	if (rtLock) {
		IOLockLock(rtLock);
		if (shutdownQuiesced) {
			IOLockUnlock(rtLock);
			CLOG("bring-up stopped before stages: shutdown is quiesced");
			return;
		}
		if (!bringupRunning) {
			bringupRunning = true;
			ownsBringup = true;
		}
		IOLockUnlock(rtLock);
	}
	struct BringupGuard {
		RDNA4Compute *self;
		bool owns;
		~BringupGuard() { if (owns) self->endBringup(); }
	} guard { this, ownsBringup };
	// W27: a bring-up (or a re-bring-up after sleep) starts with GFX powered.
	gcWake(0xffffffffu);
	gfxOffPreflight(false);
	if (gcState == kGcHold) {
		CLOG("gfxoff: bring-up skipped: GFXOFF could not be lifted");
		return;
	}
	if (!bringupStepAllowed("stage start"))
		return;
	// Read before this boot writes its own. Not at attach: that is before
	// the EFI NVRAM driver has published the stored variables.
	char prev[96];
	bool hung = logPreviousTrail(prev, sizeof(prev));
	uint32_t done = StageSurvey;
	char note[96];
	static const char *const kFeatures[] = { "gfx", "ih", "vm", "flip", "pm" };
	for (size_t i = 0; hung && i < sizeof(kFeatures) / sizeof(kFeatures[0]); i++) {
		const size_t n = strlen(kFeatures[i]);
		if (!strncmp(prev, kFeatures[i], n) && prev[n] == ':') {
			// A feature past the stages hung: the stages were fine, so only
			// that feature is left out, once.
			strlcpy(hungFeature, kFeatures[i], sizeof(hungFeature));
			// A hang between the cap probe and its restore leaves the GFXCLK
			// soft max at 1001 MHz in the SMU across a warm reboot.
			pmCapPending = !strncmp(prev, "pm: cap", 7) || !strncmp(prev, "pm: gfxcap", 10);
			CLOG("the previous boot died during \"%s\": %s is off this boot, everything else runs "
			     "(the next boot tries it again)", prev, hungFeature);
			env.owner->setProperty("Compute,PreviousHang", prev);
			hung = false;
		}
	}
	if (hung) {
		// Running the same steps again would most likely hang this boot too,
		// before anyone can collect a log. Skip once; the trail this leaves
		// is a normal ending, so the boot after this one tries again.
		CLOG("the previous boot died during \"%s\"; stages 2+ are skipped this boot so it stays "
		     "up (the next boot tries again)", prev);
		snprintf(note, sizeof(note), "skipped: previous boot hung at %s", prev);
		trail(note);
		env.owner->setProperty("Compute,PreviousHang", prev);
		env.owner->setProperty("Compute,Stage", static_cast<uint64_t>(done), 32);
		return;
	}
	// W24: engine busy survey at each stage, only with rdna4-gfxpm bit 16.
	// A GFXCLK soft max survives warm reboots in the SMU (rdna4-gfxcap, the pm
	// cap probe): remind whenever a pm boot-arg is present.
	uint32_t capArg = 0, cgArg = 0;
	if (requestedGfxPm() || requestedGfxCap(capArg) || !gfxCgIsDefault() || requestedGfxOff())
		CLOG("pm: reminder: a GFXCLK soft max set by an earlier boot (rdna4-gfxcap, or a cap probe that was not "
		     "restored) and clock gating written by rdna4-gfxcg stay in force across warm reboots; "
		     "rdna4-gfxcap=0 (boot 13 in set-boot.sh), rdna4-gfxcg=0 or a cold power cycle lifts them");
	const bool pmSurveyOn = (requestedGfxPm() & kPmSurvey) && featureAllowed("pm");
	auto survey = [&](const char *tag) {
		if (pmSurveyOn && done >= StageGfx && bringupStepAllowed("pm survey"))
			gfxPmSurvey(tag);
	};
	auto stop = [&](const char *what) {
		CLOG("%s failed; stopping, the display is not affected", what);
		snprintf(note, sizeof(note), "stopped: %s failed (no hang)", what);
		trail(note);
		env.owner->setProperty("Compute,Stage", static_cast<uint64_t>(done), 32);
	};
	if (target >= StagePsp) {
		if (!bringupStepAllowed("stage 2 (psp)")) return;
		if (!stagePsp())
			return stop("stage 2 (psp)");
		done = StagePsp;
	}
	if (target >= StageGfx) {
		if (!bringupStepAllowed("stage 3 (gfx)")) return;
		if (!stageGfx())
			return stop("stage 3 (gfx)");
		done = StageGfx;
		survey("after stage 3 (gfx up)");
	}
	if (target >= StageSdma) {
		if (!bringupStepAllowed("stage 4 (sdma)")) return;
		if (!stageSdma())
			return stop("stage 4 (sdma)");
		done = StageSdma;
		survey("after stage 4 (sdma)");
	}
	if (target >= StageCompute) {
		if (!bringupStepAllowed("stage 5 (compute)")) return;
		if (!stageCompute())
			return stop("stage 5 (compute)");
		done = StageCompute;
		survey("after stage 5 (compute queue)");
	}
	if (target >= StageDispatch) {
		if (!bringupStepAllowed("stage 6 (dispatch)")) return;
		if (!stageDispatch())
			return stop("stage 6 (dispatch)");
		done = StageDispatch;
		survey("after stage 6 (dispatch)");
	}
	if (target >= StageKernel) {
		if (!bringupStepAllowed("stage 7 (kernel)")) return;
		if (!stageKernel())
			return stop("stage 7 (kernel)");
		done = StageKernel;
		survey("after stage 7 (kernel)");
	}
	if (vmEnabled && done >= StageKernel) {
		if (!bringupStepAllowed("VM self-test"))
			return;
		if (!featureAllowed("vm") || !vmBootSelfTest()) {
		vmEnabled = false;
		CLOG("vm: boot self-test failed; per-client GPU VM disabled");
		}
	}
	// W3: the gfx ring, when asked for. A failure only turns it off again.
	bool gfxOk = false;
	const uint32_t gfxAsked = done >= StageKernel ? requestedGfx() : 0;
	if (gfxAsked && featureAllowed("gfx")) {
		if (!bringupStepAllowed("gfx ring")) return;
		gfxMode = gfxAsked;
		gfxOk = stageGfxRing();
		survey("after the gfx ring");
	}
	// W19: the GFX power-management experiment, only when asked for. It runs
	// while trails are still allowed and leaves its own "pm: ..." steps.
	if (pmCapPending && done >= StageGfx)
		gfxPmRecoverCap();
	const uint32_t pmAsked = done >= StageGfx ? requestedGfxPm() : 0;
	if ((pmAsked & 15) && featureAllowed("pm")) {
		if (!bringupStepAllowed("pm experiment")) return;
		gfxPmExperiment(pmAsked);
	}
	survey("end of bring-up");
	uint32_t capMHz = 0;
	if (done >= StageGfx && featureAllowed("pm") && requestedGfxCap(capMHz)) {
		if (!bringupStepAllowed("gfxcap")) return;
		gfxCapApply(capMHz);
	}
	env.owner->setProperty("Compute,Stage", static_cast<uint64_t>(done), 32);
	CLOG("bring-up finished at stage %u", done);
	// The runtime and what it starts leave their own "<feature>: ..." steps
	// in the trail, so the trail's normal ending comes after them.
	if (done >= StageDispatch)
		if (!bringupStepAllowed("runtime publish")) return;
	if (done >= StageDispatch)
		publishRuntime(done);
	// W5: the page-flip test, once the runtime's DMA and device heap exist.
	if (done >= StageKernel && featureAllowed("flip"))
		if (!bringupStepAllowed("flip")) return;
	if (done >= StageKernel && featureAllowed("flip"))
		Flip::run(*this);
	// G3: the first draw, once the runtime's device heap holds its rings.
	if (!bringupStepAllowed("gfx draw"))
		return;
	const bool drew = gfxOk && stageGfxDraw();
	// W27: clock gating, then GFXOFF, are the last things bring-up does, after every
	// self-test. Nothing after gfxOffProbe() may touch the GC domain except through
	// GcAccess, which wakes GFX first.
	uint32_t cgMask = 0;
	if (done >= (gfxCgIsDefault() ? StageKernel : StageGfx) && featureAllowed("pm") && requestedGfxCg(cgMask)) {
		if (!bringupStepAllowed("clock gating")) return;
		gfxCgApply(cgMask);
	}
	if (done >= StageGfx && featureAllowed("pm") && (requestedGfxOff() || gfxOffHook())) {
		if (!bringupStepAllowed("gfxoff")) return;
		gfxOffProbe();
	}
	snprintf(note, sizeof(note), "finished at stage %u%s%s%s", done,
	         !gfxAsked ? "" : drew ? ", gfx draw right" : gfxOk ? ", gfx ring up" : ", gfx ring off",
	         hungFeature[0] ? ", skipped after a hang: " : "", hungFeature);
	trail(note);
}

bool RDNA4Compute::featureAllowed(const char *name) const {
	if (hungFeature[0] && !strcmp(hungFeature, name))
		return false;
	if (!strcmp(name, "flip")) {
		uint32_t requested = 0;
		return PE_parse_boot_argn("rdna4-flip", &requested, sizeof(requested)) && requested != 0;
	}
	return true;
}

// The pool, uncached: the PSP reads what we write there and writes fences
// we poll, so neither side may see a stale cache line.
bool RDNA4Compute::mapPool() {
	if (poolCpu)
		return true;
	IODeviceMemory *bar0 = env.pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!bar0 || !pool.valid)
		return false;
	IODeviceMemory *sub = IODeviceMemory::withSubRange(bar0, pool.offset, pool.size);
	if (!sub)
		return false;
	poolMap = sub->map(kIOMapInhibitCache);
	sub->release();
	if (!poolMap)
		return false;
	poolCpu = reinterpret_cast<uint8_t *>(poolMap->getVirtualAddress());
	return poolCpu != nullptr;
}

// CPU writes to VRAM go through the HDP; a write to the remapped
// HDP_MEM_COHERENCY_FLUSH_CNTL (NBIF remap, set up by the VBIOS) pushes them
// out before the PSP or an engine reads the memory. Read back to post it.
void RDNA4Compute::flushHdp() {
	const uint32_t off = sv.hdpMemFlushRemap;
	if (!off || off == kBad || off + 4 > env.mmioSize)
		return;
	env.mmio[off / 4] = 0;
	// amdgpu_hdp_generic_flush posts the write with a read of the NBIF
	// memory size register, not of the flush register.
	(void)rd(IpDiscovery::HwNbif, NbifMemSize);
}

uint32_t RDNA4Compute::pspRead(void *ctx, uint32_t dword) {
	return static_cast<RDNA4Compute *>(ctx)->rd(IpDiscovery::HwMp0, Reg { 0, dword });
}

void RDNA4Compute::pspWrite(void *ctx, uint32_t dword, uint32_t value) {
	static_cast<RDNA4Compute *>(ctx)->wr(IpDiscovery::HwMp0, Reg { 0, dword }, value);
}

void RDNA4Compute::pspDelay(void *, uint32_t us) {
	if (us >= 1000)
		IOSleep(us / 1000);
	else
		IODelay(us);
}

void RDNA4Compute::pspFlush(void *ctx) {
	static_cast<RDNA4Compute *>(ctx)->flushHdp();
}

uint32_t RDNA4Compute::smuSend(uint32_t msg, uint32_t param, uint32_t &ret, uint32_t timeoutMs) {
	if (smuLock)
		IOLockLock(smuLock);           // one message in the mailbox at a time (W27 gcWake can run on any thread)
	wr(IpDiscovery::HwMp1, SmuResp, 0);
	wr(IpDiscovery::HwMp1, SmuArg, param);
	wr(IpDiscovery::HwMp1, SmuMsg, msg);
	uint32_t resp = 0;
	for (uint32_t ms = 0; ms <= timeoutMs; ms++) {
		resp = rd(IpDiscovery::HwMp1, SmuResp);
		if (resp != 0 && resp != kBad)
			break;
		IOSleep(1);
	}
	ret = rd(IpDiscovery::HwMp1, SmuArg);
	if (smuLock)
		IOLockUnlock(smuLock);
	return resp == kBad ? 0 : resp;
}

bool RDNA4Compute::readSensors(RDNA4Sensors &out) {
	if (!poolCpu || kSmuTableOffset + 0x1000 > pool.size)
		return false;
	uint32_t ret = 0;
	// TABLE_SMU_METRICS is table 5 in smu14_driver_if_v14_0.h. Table 0 is
	// PPTABLE and is not the telemetry transfer requested by this reader.
	if (smuSend(kSmuMsgGetMetricsTable, 5, ret, 100) != kSmuRespOk)
		return false;
	/* smu14_driver_if_v14_0.h, SmuMetrics_t: CurrClock[11], the average
	 * clocks, then MetricsCounter, voltage/current arrays, power, temperatures,
	 * and fan tach. Keep this compact reader in sync with that public table
	 * layout instead of copying the whole firmware ABI into our user ABI. */
	const uint8_t *table = poolCpu + kSmuTableOffset;
	auto u16 = [table](uint32_t off) -> uint32_t {
		return *reinterpret_cast<const volatile uint16_t *>(table + off);
	};
	constexpr uint32_t kAverageGfxPost = RDNA4_SMU_METRICS_AVG_GFXCLK_POST_DS;
	constexpr uint32_t kAverageMemPost = RDNA4_SMU_METRICS_AVG_MEMCLK_POST_DS;
	constexpr uint32_t kSocketPower = RDNA4_SMU_METRICS_AVG_SOCKET_POWER;
	constexpr uint32_t kTemperatures = RDNA4_SMU_METRICS_AVG_TEMPERATURE;
	constexpr uint32_t kFanRpm = RDNA4_SMU_METRICS_AVG_FAN_RPM;
	out.edgeTempC = u16(kTemperatures + 0 * 2);
	out.hotspotTempC = u16(kTemperatures + 1 * 2);
	out.gfxClockMHz = u16(kAverageGfxPost);
	out.memoryClockMHz = u16(kAverageMemPost);
	out.socketPowerW = u16(kSocketPower);
	out.fanRpm = u16(kFanRpm);
	return true;
}

// The power-management view of the same table: instantaneous GFX clock, the
// activity counters and the VDD_GFX rail, plus MetricsCounter to prove the
// table is live. amdgpu invalidates the HDP read path after
// TransferTableSmu2Dram (smu_cmn_update_table_read_arg, smu_cmn.c:1147-1150),
// but hdp_v7_0_funcs (hdp_v7_0.c:128-132) has no invalidate_hdp and soc24 sets
// none, so on this ASIC that call is a no-op. Our mapping is uncached
// (kIOMapInhibitCache), which leaves only a stale table to rule out: poison
// MetricsCounter first, so a table the SMU did not rewrite reads back as poison
// instead of an old sample.
bool RDNA4Compute::readSensorsEx(RDNA4SensorsEx &out) {
	if (!poolCpu || kSmuTableOffset + 0x1000 > pool.size)
		return false;
	uint8_t *table = poolCpu + kSmuTableOffset;
	constexpr uint32_t kPoison = 0xFFFFFFFFu;
	*reinterpret_cast<volatile uint32_t *>(table + RDNA4_SMU_METRICS_COUNTER) = kPoison;
	flushHdp();
	uint32_t ret = 0;
	if (smuSend(kSmuMsgGetMetricsTable, 5, ret, 100) != kSmuRespOk)
		return false;
	flushHdp();
	auto u8 = [table](uint32_t off) -> uint32_t {
		return *reinterpret_cast<const volatile uint8_t *>(table + off);
	};
	auto u16 = [table](uint32_t off) -> uint32_t {
		return *reinterpret_cast<const volatile uint16_t *>(table + off);
	};
	auto u32 = [table](uint32_t off) -> uint32_t {
		return *reinterpret_cast<const volatile uint32_t *>(table + off);
	};
	memset(&out, 0, sizeof(out));
	out.metricsCounter = u32(RDNA4_SMU_METRICS_COUNTER);
	if (out.metricsCounter != kPoison)
		out.flags |= RDNA4_SENSORS_EX_LIVE;
	out.currGfxclkMHz = u32(RDNA4_SMU_METRICS_CURR_CLOCK + RDNA4_SMU_METRICS_PPCLK_GFXCLK * 4);
	out.avgGfxclkPreDsMHz = u16(RDNA4_SMU_METRICS_AVG_GFXCLK_PRE_DS);
	out.avgGfxclkPostDsMHz = u16(RDNA4_SMU_METRICS_AVG_GFXCLK_POST_DS);
	out.gfxActivity = u16(RDNA4_SMU_METRICS_AVG_GFX_ACTIVITY);
	out.uclkActivity = u16(RDNA4_SMU_METRICS_AVG_UCLK_ACTIVITY);
	out.vddGfxMv = u16(RDNA4_SMU_METRICS_AVG_VOLTAGE + RDNA4_SMU_METRICS_SVI_VDD_GFX * 2);
	out.vddGfxCurrentA = u16(RDNA4_SMU_METRICS_AVG_CURRENT + RDNA4_SMU_METRICS_SVI_VDD_GFX * 2);
	out.socketPowerW = u16(RDNA4_SMU_METRICS_AVG_SOCKET_POWER);
	out.hotspotTempC = u16(RDNA4_SMU_METRICS_AVG_TEMPERATURE + 1 * 2);
	for (uint32_t i = 0; i < RDNA4_SMU_METRICS_THROTTLER_COUNT; i++) {
		const uint32_t pct = u8(RDNA4_SMU_METRICS_THROTTLING_PCT + i);
		out.throttlingPercent[i] = static_cast<uint8_t>(pct);
		if (pct)
			out.throttlingMask |= 1u << i;
	}
	return true;
}

// ---------------------------------------------------------------------------
// W19: GFX power-management experiment (rdna4-gfxpm), off unless asked for
// ---------------------------------------------------------------------------
//
// r2-sensors-review.md: on the card, idle in Recovery, the SMU reports about
// 300 W and 3.2 GHz on GFX. amdgpu sends these after
// EnableAllSmuFeatures that this kext never does: SetWorkloadMask (the boot-up
// default profile, smu_bump_power_profile_mode, amdgpu_smu.c:2394-2417 ->
// smu_v14_0_2_set_power_profile_mode, smu_v14_0_2_ppt.c:1824-1876), and
// AllowGfxOff. (amdgpu writes no soft limits at the AUTO level, because
// dpm_level == level, amdgpu_smu.c:2462; bit 2 below is an experiment beyond
// amdgpu that writes what its automatic branch would,
// smu_v14_0_set_soft_freq_limited_range, smu_v14_0.c:1019-1050.) SetWorkloadMask
// also skips amdgpu's smu_v14_0_deep_sleep_control(true), which is fine: the DS
// features already run under the PMFW's own set. Each bit is followed by a
// sampled readout so the real-card log shows which one moves
// GFXCLK/activity/power. GFXOFF is deliberately not offered: with MMIO,
// doorbell and queue access from this kext it would power GFX down under us.
//
//   rdna4-gfxpm=8   sample only: log the metrics, change nothing
//   rdna4-gfxpm=1   + SetWorkloadMask(WORKLOAD_PPLIB_DEFAULT_BIT)
//   rdna4-gfxpm=2   + SetSoftMax/MinByFreq(GFXCLK) back to automatic
//   rdna4-gfxpm=4   + probe: cap GFXCLK soft max at 1000 MHz, sample, restore auto
//
// Every message has a bounded wait; an answer other than OK stops the
// experiment, and the state it left is logged.
uint32_t RDNA4Compute::requestedGfxPm() {
	uint32_t mask = 0;
	if (!PE_parse_boot_argn("rdna4-gfxpm", &mask, sizeof(mask)))
		return 0;
	return mask & (kPmSampleOnly | kPmWorkload | kPmSoftAuto | kPmCapProbe | kPmSurvey);
}

// rdna4-gfxcap=<MHz>: a user-selectable GFXCLK soft maximum through the
// SetSoftMaxByFreq path the round-3 cap probe proved on the card (1000 MHz:
// 46 W instead of 310 W). Off unless present. The SMU keeps the limit across
// warm reboots, so dropping the boot-arg does not lift it: use
// rdna4-gfxcap=0 (soft max back to automatic) or a cold power cycle.
bool RDNA4Compute::requestedGfxCap(uint32_t &mhz) {
	uint32_t v = 0;
	if (!PE_parse_boot_argn("rdna4-gfxcap", &v, sizeof(v)))
		return false;
	mhz = v;
	return true;
}

void RDNA4Compute::gfxCapApply(uint32_t mhz) {
	if (!poolCpu)
		return;
	trail("pm: gfxcap");
	if (mhz == 0) {
		if (gfxPmRestoreAuto("gfxcap=0: GFXCLK soft max -> automatic"))
			gfxPmSample("after gfxcap=0 (soft max automatic)");
		return;
	}
	if (mhz < kGfxCapMinMHz || mhz > kGfxCapMaxMHz) {
		CLOG("pm: rdna4-gfxcap=%u ignored (accepted: 0, or %u..%u MHz)", mhz, kGfxCapMinMHz, kGfxCapMaxMHz);
		return;
	}
	// SMU_V14_SOFT_FREQ_ROUND(max) is max + 1 (smu_v14_0.h:54).
	if (!gfxPmSoftLimits(mhz + 1, kPmNoMin, "gfxcap: GFXCLK soft max")) {
		CLOG("pm: rdna4-gfxcap=%u refused by the SMU; no cap in force", mhz);
		return;
	}
	CLOG("pm: rdna4-gfxcap=%u MHz in force (kept across warm reboots; rdna4-gfxcap=0 or a cold power cycle lifts it)", mhz);
	gfxPmSample("with rdna4-gfxcap");
}

// ---------------------------------------------------------------------------
// W27 (review B1): the guard must survive the kext's own lifetime
// ---------------------------------------------------------------------------
//
// A new kext instance starts with gcState == On. If the previous boot left GFXOFF
// allowed and the ASIC (which survives a warm restart: golden registers, the GFXCLK
// cap and live engines all do) kept it, the stage-1 survey would read a powered-down
// block. Two layers:
//   1. the shutdown quiesce lifts GFXOFF before it touches GC (runtime.cpp), which
//      also clears the flag below;
//   2. a persistent NVRAM flag, written right before AllowGfxOff and cleared after a
//      successful DisallowGfxOff, is read at the next start; when it is set the kext
//      sends DisallowGfxOff before the first GC read (gfxOffPreflight). The SMU mailbox
//      is on MP1 (SmuMsg/SmuArg/SmuResp = MP1 C2PMSG_66/82/90, gfxregs.hpp), not GC, so
//      it is reachable while GC is powered down. If the SMU does not answer, kGcHold:
//      every GC access of this boot is dropped and the bring-up is skipped.
// Caveat, found by testing: the NVRAM entry may not be published yet when the kext
// starts ("Not at attach: that is before the EFI NVRAM driver has published the stored
// variables", runStages). gfxOffPreflight logs whether the flag was readable at
// attach; runStages repeats the check once NVRAM is up (after the 5 s wait) so a late
// flag still lifts GFXOFF before stage 2, but the stage-1 reads cannot be protected
// then, and the log says so.
static const char *kGfxOffKey = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-gfxoff";

bool RDNA4Compute::gfxOffFlagGet() {
	bool set = false;
	IORegistryEntry *nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!nvram)
		return false;
	if (OSObject *v = nvram->copyProperty(kGfxOffKey)) {
		if (auto *s = OSDynamicCast(OSString, v))
			set = s->getLength() && s->getCStringNoCopy()[0] == '1';
		else if (auto *d = OSDynamicCast(OSData, v))
			set = d->getLength() && static_cast<const char *>(d->getBytesNoCopy())[0] == '1';
		v->release();
	}
	nvram->release();
	return set;
}

bool RDNA4Compute::gfxOffFlagSet(bool set) {
	IORegistryEntry *nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!nvram)
		return false;
	const OSSymbol *key = OSSymbol::withCString(kGfxOffKey);
	OSString *value = OSString::withCString(set ? "1" : "0");
	OSDictionary *sync = OSDictionary::withCapacity(1);
	OSString *yes = OSString::withCString("1");
	if (key && value)
		nvram->setProperty(key, value);
	if (sync && yes && sync->setObject(kNvramForceSync, yes))
		nvram->setProperties(sync);
	OSSafeReleaseNULL(yes);
	OSSafeReleaseNULL(sync);
	OSSafeReleaseNULL(value);
	OSSafeReleaseNULL(key);
	nvram->release();
	return gfxOffFlagGet() == set;              // read it back: a flag that did not stick protects nothing
}

// Called before the first GC read of a start (attach == true) and again once NVRAM is
// published (attach == false, from runStages).
void RDNA4Compute::gfxOffPreflight(bool attach) {
	// Test hook (VM only): the guest NVRAM does not survive between vm-test runs, so
	// rdna4-gfxoff=3 pretends the flag was found set; 2 leaves it set and reads it back.
	const bool pretend = gfxOffHook() == 3;
	const bool flag = gfxOffFlagGet() || pretend;
	CLOG("gfxoff: %s: previous-boot GFXOFF flag %s%s%s", attach ? "attach" : "bring-up start",
	     flag ? "SET" : "clear", pretend ? " (TEST HOOK rdna4-gfxoff=3 pretends it)" : "",
	     attach ? " (NVRAM may not be published yet at attach)" : "");
	if (!flag || gcState == kGcHold)
		return;
	CLOG("gfxoff: the previous boot may have left GFXOFF allowed: DisallowGfxOff before any GC access%s",
	     attach ? "" : " (too late for the stage-1 survey reads, which were unguarded)");
	uint32_t ret = 0, resp = 0;
	for (int attempt = 0; attempt < 2 && resp != kSmuRespOk; attempt++)
		resp = smuSend(kSmuMsgDisallowGfxOff, 0, ret, 100);
	if (resp == kSmuRespOk) {
		gfxOffFlagSet(false);
		CLOG("gfxoff: DisallowGfxOff -> 0x%02x; flag cleared", resp);
		return;
	}
	gcState = kGcHold;
	CLOG("gfxoff: the SMU did not answer DisallowGfxOff (0x%02x): every GC register and doorbell access is "
	     "dropped for the rest of this boot and the bring-up is skipped", resp);
}

// ---------------------------------------------------------------------------
// W27: GFXOFF guard. Every access to the GC power domain goes through it.
// ---------------------------------------------------------------------------
//
// Why this exists. With rdna4-gfxoff=1 the kext asks the SMU to allow GFXOFF once,
// at the very end of bring-up (gfxOffAllow). The RLC/PMFW may then power the GC
// block down whenever it likes. Any MMIO or doorbell access to a powered-off GC
// block can hang the machine (amdgpu keeps GFXOFF disallowed around every direct
// GC register access it does: amdgpu_gfx_off_ctrl(adev, false), e.g.
// gfx_v12_0.c:5074,5247,5283, and only allows it again 100 ms after the last user,
// amdgpu_gfx.c:925-985).
//
// The guarantee. There are exactly three ways this kext reaches the GC domain:
//   1. rd()/wr() with hwId == HwGc (every GC, RLC, CP, SDMA, GCVM register; the
//      IH, flip, sensors, PSP and SMU code use other hwIds and never touch GC);
//   2. the four doorbell writes (compute, VM queue, SDMA, gfx ring);
//   3. nothing else: env.mmio is dereferenced only in rd()/wr()/flushHdp() (HDP is
//      NBIF), verified by grep.
// Both 1 and 2 construct a GcAccess first. GcAccess implements a small protocol:
//   - gcBusy counts GC accessors inside their access; gcState is On (GFX powered,
//     the state of every boot until the Allow), Allowing, or Off (GFXOFF allowed).
//   - An accessor increments gcBusy, then checks gcState. If it is not On it
//     decrements, calls gcWake() (DisallowGfxOff through the SMU, which returns once
//     GFX is powered, retried once) and retries. It proceeds only with gcState == On
//     observed after its increment.
//   - gfxOffAllow() sets gcState = Allowing, issues a full barrier, waits (bounded)
//     for gcBusy == 0 and only then sends AllowGfxOff. Accessors that raced in
//     after the state change see it and wait on gcLock; accessors that were already
//     inside are waited for. If anything is still inside after the bound, Allow is
//     not sent.
//   - The wake is sticky: once any GC access happens, GFXOFF stays disallowed until
//     the next boot (or the next bring-up after sleep, which begins with gcWake()).
//     So GFXOFF only ever covers the idle window between the end of bring-up and
//     the first GC access; the rdna4-run info/sensors path is SMU-only and does not
//     end it, the selftest, bench, IH stop, quiesce, flip-with-GC and dispatch do.
//   - smuSend is serialized with smuLock so that a wake from any thread cannot
//     interleave with another SMU message.
// Not covered on purpose: interrupt context (nothing there touches GC; IH uses
// OSSSYS and DMU registers only) and a failed wake (logged; the next GC access
// would still be attempted, which is the risk of the whole feature).
RDNA4Compute::GcAccess::GcAccess(const RDNA4Compute &comp, uint32_t what) : c(comp) {
	if (c.gcOwner == current_thread())
		return;                     // the waker restoring GC state: it holds gcLock, GC is up
	for (int tries = 0;; tries++) {
		OSIncrementAtomic(&c.gcBusy);
		if (c.gcState == kGcOn) {
			counted = true;
			return;
		}
		OSDecrementAtomic(&c.gcBusy);
		// A failed wake leaves kGcHold: never proceed to a block that may be powered down.
		if (c.gcState == kGcHold || tries >= 2) {
			ok = false;
			return;
		}
		c.gcWake(what);
	}
}

RDNA4Compute::GcAccess::~GcAccess() {
	if (counted)
		OSDecrementAtomic(&c.gcBusy);
}

void RDNA4Compute::gcWake(uint32_t what) const {
	RDNA4Compute *self = const_cast<RDNA4Compute *>(this);
	if (!gcLock)
		return;
	IOLockLock(gcLock);
	if (gcState == kGcOff || gcState == kGcAllowing) {
		uint32_t ret = 0, resp = 0;
		for (int attempt = 0; attempt < 2 && resp != kSmuRespOk; attempt++)
			resp = self->smuSend(kSmuMsgDisallowGfxOff, 0, ret, 100);
		const char *who = what == 0xffffffffu ? "the start of a bring-up" : what == 0xfffffffeu ? "the shutdown quiesce" :
		                  (what >> 24) == 0xdb ? "a doorbell write, dword" : "a GC register access, seg<<16|dword";
		if (resp == kSmuRespOk) {
			CLOG("gfxoff: GFXOFF ended by %s 0x%x (thread %p): DisallowGfxOff -> 0x%02x; GFXOFF stays "
			     "disallowed until the next boot", who, what & 0xffffff, current_thread(), resp);
			// GC state does not survive the power-down: wait for the RLC to finish restoring, then
			// bring the boot HQD back, before any other thread can use GC (gcState is still not On).
			gcOwner = current_thread();
			self->gfxOffAfterWake(what);
			gcOwner = nullptr;
			gcState = kGcOn;
			gfxOffFlagSet(false);
		} else {
			gcState = kGcHold;
			CLOG("gfxoff: WAKE FAILED for %s 0x%x (DisallowGfxOff -> 0x%02x): every GC register and doorbell "
			     "access is dropped for the rest of this boot", who, what & 0xffffff, resp);
		}
	}
	IOLockUnlock(gcLock);
}

// After DisallowGfxOff is acknowledged, GC has been powered back up but its state is not the state
// we left. Round 4 on the card: the first dispatch after the wake timed out with CP_HQD_ACTIVE 0
// and PQ base/doorbell/rptr/wptr all zero (the directly programmed MMIO HQD is lost across the
// GFXOFF power-down: amdgpu's queues survive because the CP restores them from their MQD in
// memory, which a plain-MMIO HQD does not have), and only the hang recovery brought it back.
// amdgpu itself does not poll anything after the DisallowGfxOff answer (smu_v14_0_gfx_off_control
// is synchronous, amdgpu_gfx.c:925-985; gfx_v12_0.c has no RLC_GPM_STAT poll); we additionally
//   1. wait (bounded 200 ms) until RLC_GPM_STAT reports GFX powered and no save/restore or WGP
//      power-up in progress, and log what it said;
//   2. if the boot queue's HQD is no longer active, program it again (the same reinit
//      recoverComputeQueue uses) and prove it with a fenced WRITE_DATA.
// Runs on the waking thread with gcOwner set (its GC accesses bypass the guard).
void RDNA4Compute::gfxOffAfterWake(uint32_t what) {
	if (what == 0xfffffffeu)
		return;                                     // shutdown quiesce: nothing to restore
	uint32_t stat = 0;
	uint32_t us = 0;
	const uint32_t busyMask = kGpmSavingRegs | kGpmRestoringRegs | kGpmGfx3dChanging | kGpmCmpChanging |
	                          kGpmStaticWgpUp | kGpmDynWgpUp;
	for (; us < 200000; us += 100) {
		stat = rdGc(RlcGpmStat);
		if (stat != kBad && (stat & kGpmGfxPowerStatus) && !(stat & busyMask))
			break;
		IODelay(100);
	}
	CLOG("gfxoff: after the wake RLC_GPM_STAT 0x%08x after %u us (%s)", stat, us,
	     us >= 200000 ? "NOT settled within 200 ms, continuing" : "settled");
	if (!bootQueueLive)
		return;
	grbmSelect(1, 0, 0, 0);
	const uint32_t active = rdGc(CpHqdActive);
	grbmSelect(0, 0, 0, 0);
	if (active != kBad && (active & 1)) {
		CLOG("gfxoff: the boot HQD survived the power-down (CP_HQD_ACTIVE 0x%08x)", active);
		return;
	}
	CLOG("gfxoff: the boot HQD was lost in the power-down (CP_HQD_ACTIVE 0x%08x): programming it again", active);
	logComputeQueueState("gfxoff wake", 0, 0, 0);
	if (pm4Queue.init(poolDw(kPqOffset), poolMc(kPqOffset), kPqSize) && hqdInit(hqdMode == 2) &&
	    queueWriteTest("gfxoff wake"))
		CLOG("gfxoff: the boot HQD is back (fenced WRITE_DATA landed)");
	else
		CLOG("gfxoff: the boot HQD could not be restored; the first dispatch will need the hang recovery");
}

bool RDNA4Compute::gfxOffAllow() {
	if (!gcLock)
		return false;
	bool ok = false;
	IOLockLock(gcLock);
	gcState = kGcAllowing;
	OSAddAtomic(0, &gcBusy);      // a locked instruction: full barrier on x86, state write before the busy read
	uint32_t waited = 0;
	while (gcBusy != 0 && waited < 50) {
		IOSleep(1);
		waited++;
	}
	if (gcBusy != 0) {
		CLOG("gfxoff: %d GC accessors still inside after 50 ms; AllowGfxOff not sent", static_cast<int>(gcBusy));
		gcState = kGcOn;
	} else {
		uint32_t ret = 0;
		// Persist "GFXOFF may be allowed" BEFORE the message: the next boot reads it and
		// sends DisallowGfxOff before it touches GC, even after a crash or a hard reset.
		if (!gfxOffFlagSet(true)) {
			// S5: without the flag the next boot cannot know; do not allow GFXOFF at all.
			CLOG("gfxoff: the persistent flag could not be written (read-back is not 1); AllowGfxOff NOT sent");
			gcState = kGcOn;
			IOLockUnlock(gcLock);
			return false;
		}
		const uint32_t resp = smuSend(kSmuMsgAllowGfxOff, 0, ret, 100);
		CLOG("gfxoff: AllowGfxOff -> 0x%02x", resp);
		if (resp == kSmuRespOk) {
			gcState = kGcOff;
			ok = true;
		} else if (resp == 0) {
			// S6: a timeout is ambiguous, the SMU may still have processed the message. Treat it as
			// possibly allowed: keep the flag set and go through the guard as if allowed.
			CLOG("gfxoff: AllowGfxOff timed out: treated as possibly allowed (flag kept, the guard stays armed)");
			gcState = kGcOff;
			ok = true;
		} else {
			gcState = kGcOn;
			gfxOffFlagSet(false);
		}
	}
	IOLockUnlock(gcLock);
	return ok;
}

// The end-of-bring-up GFXOFF probe: allow, wait, sample the SMU table only (no GC
// register read: that would wake it), and leave GFXOFF allowed. Trail steps are
// written here because the bring-up thread is still running; the final "finished"
// trail follows, so the trail never ends on "pm: gfxoff".
void RDNA4Compute::gfxOffProbe() {
	if (!poolCpu)
		return;
	const uint32_t hook = gfxOffHook();
	if (hook == 3)
		return;                                 // test hook: preflight only, nothing is allowed
	if (hook == 2) {
		// Test hook (rdna4-gfxoff=2, VM only): behave as if a boot had allowed GFXOFF and then died
		// before waking it: the persistent flag stays set, nothing is allowed. The next start must
		// send DisallowGfxOff before its first GC read.
		gfxOffFlagSet(true);
		CLOG("gfxoff: TEST HOOK rdna4-gfxoff=2: persistent flag left set (read back: %s), AllowGfxOff NOT sent",
		     gfxOffFlagGet() ? "SET" : "clear");
		return;
	}
	trail("pm: gfxoff allow");
	if (!gfxOffAllow()) {
		CLOG("gfxoff: not allowed; GFX stays powered");
		return;
	}
	IOSleep(1500);
	if (gcState != kGcOff)
		CLOG("gfxoff: another thread's GC access already ended GFXOFF during the wait; the sample below is "
		     "not a GFXOFF reading");
	RDNA4SensorsEx s;
	if (readSensorsEx(s))
		CLOG("gfxoff: 1.5 s after AllowGfxOff (SMU only, no GC access): avg GFXCLK pre-DS %u post-DS %u MHz, "
		     "GFX activity %u %%, VDD_GFX %u mV, socket %u W, hotspot %u C, MetricsCounter %u (%s); "
		     "GFXOFF stays allowed until the first GC access", s.avgGfxclkPreDsMHz, s.avgGfxclkPostDsMHz,
		     s.gfxActivity, s.vddGfxMv, s.socketPowerW, s.hotspotTempC, s.metricsCounter,
		     (s.flags & RDNA4_SENSORS_EX_LIVE) ? "live" : "STALE");
	else
		CLOG("gfxoff: metrics query failed after AllowGfxOff");
}

bool RDNA4Compute::runsUnderHypervisor() {
	uint32_t a = 1, b = 0, c = 0, d = 0;
	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
	return (c >> 31) & 1;
}

// rdna4-gfxoff=2 and =3 are VM test hooks. On real hardware (no hypervisor) they are ignored
// and logged, so a stray value can neither pretend a flag nor leave one behind.
uint32_t RDNA4Compute::gfxOffHook() {
	const uint32_t v = gfxOffMode();
	if (v != 2 && v != 3)
		return 0;
	if (!runsUnderHypervisor()) {
		CLOG("gfxoff: TEST HOOK rdna4-gfxoff=%u ignored: this is not an emulated device", v);
		return 0;
	}
	return v;
}

uint32_t RDNA4Compute::gfxOffMode() {
	uint32_t v = 0;
	if (!PE_parse_boot_argn("rdna4-gfxoff", &v, sizeof(v)))
		return 0;
	return v;
}

// The real AllowGfxOff needs exactly rdna4-gfxoff=1; any other value (a typo, 10) is ignored.
bool RDNA4Compute::requestedGfxOff() {
	return gfxOffMode() == 1;
}

// ---------------------------------------------------------------------------
// W27: clock gating (rdna4-gfxcg=<mask>), gfx_v12_0_update_gfx_clock_gating
// ---------------------------------------------------------------------------
//
// amdgpu enables GFX clock gating at the end of device init
// (amdgpu_device_ip_late_init -> amdgpu_device_set_cg_state(GATE),
// amdgpu_device.c:2772 -> gfx_v12_0_set_clockgating_state, gfx_v12_0.c:4370-4384 ->
// gfx_v12_0_update_gfx_clock_gating, gfx_v12_0.c:4342-4366). The cg_flags for
// GC 12.0.1 are CGCG, CGLS, MGCG, 3D CGCG, 3D CGLS, REPEATER_FGCG, FGCG and PERF_CLK
// (soc24.c:369-381), so every branch below is the enabled one. Steps, one bit each
// in the boot-arg (rdna4-gfxcg=15 is all of them, the order amdgpu uses; 0 is
// the enable=false mirror):
//   1 coarse   gfx_v12_0_update_coarse_grain_clock_gating, gfx_v12_0.c:4151-4247
//   2 medium   gfx_v12_0_update_medium_grain_clock_gating, gfx_v12_0.c:4268-4300
//   4 fine     repeater FGCG :4302, SRAM FGCG :4323, perf clock :4055
//   8 gui-idle gfx_v12_0_enable_gui_idle_interrupt, gfx_v12_0.c:1909-1935
// The whole update runs inside RLC safe mode like amdgpu's
// (amdgpu_gfx_rlc_enter_safe_mode, amdgpu_rlc.c:38-53; set/unset_safe_mode,
// gfx_v12_0.c:4029-4047), each register is read-modify-write and written only if
// the value changes (amdgpu's "def != data"), and every wait is bounded.

// gfx_v12_0_set_safe_mode / unset_safe_mode: RLC_SAFE_MODE CMD | (1 << MESSAGE),
// wait for CMD to clear. Returns false on timeout (the exit is still sent).
bool RDNA4Compute::rlcSafeMode(bool enter) {
	if (!enter) {
		wr(IpDiscovery::HwGc, RlcSafeMode, kRlcSafeModeCmd);
		return true;
	}
	wr(IpDiscovery::HwGc, RlcSafeMode, kRlcSafeModeCmd | (1u << kRlcSafeModeMsgShift));
	for (uint32_t us = 0; us < 20000; us += 10) {
		if (!(rdGc(RlcSafeMode) & kRlcSafeModeCmd))
			return true;
		IODelay(10);
	}
	return false;
}

// def = RREG32; data = (def & ~clear) | set; if (def != data) WREG32.
void RDNA4Compute::gfxCgRmw(const GfxReg::Reg &r, const char *name, uint32_t clear, uint32_t set) {
	const uint32_t def = rdGc(r);
	const uint32_t data = (def & ~clear) | set;
	if (def != data)
		wr(IpDiscovery::HwGc, r, data);
	CLOG("cg: %s 0x%08x -> 0x%08x%s", name, def, data, def == data ? " (unchanged)" : "");
}

void RDNA4Compute::gfxCgCoarse(bool enable) {
	if (enable) {
		trail("pm: cg coarse");
		// unset the CGCG, CGLS and 3D override bits (:4166-4176)
		gfxCgRmw(RlcCgttMgcgOverride, "RLC_CGTT_MGCG_OVERRIDE", kCgOvrCgcg | kCgOvrCgls | kCgOvr3d, 0);
		// enable the CGCG FSM (0x0000363F, :4179-4193)
		gfxCgRmw(RlcCgcgCglsCtrl, "RLC_CGCG_CGLS_CTRL", kCgIdleThresholdMask | kCgRepDelayMask,
		         (0x36u << kCgIdleThresholdShift) | kCgEn | (0xFu << kCgRepDelayShift) | kCglsEn);
		// RLC_CGCG_CGLS_CTRL_3D (:4198-4214)
		gfxCgRmw(RlcCgcgCglsCtrl3d, "RLC_CGCG_CGLS_CTRL_3D", kCgIdleThresholdMask | kCgRepDelayMask,
		         (0x36u << kCgIdleThresholdShift) | kCgEn | (0xFu << kCgRepDelayShift) | kCglsEn);
		// IDLE_POLL_COUNT(0x00900100) (:4217-4226)
		gfxCgRmw(CpRbWptrPollCntl, "CP_RB_WPTR_POLL_CNTL", 0xFFFFFFFFu, (0x0090u << 16) | 0x0100u);
		// CP_INT_CNTL busy/empty/idle interrupts, written unconditionally (:4228-4233)
		const uint32_t cpInt = rdGc(CpIntCntl) | kCpIntGuiIdleBits;
		wr(IpDiscovery::HwGc, CpIntCntl, cpInt);
		CLOG("cg: CP_INT_CNTL -> 0x%08x", cpInt);
		// SDMAn_RLC_CGCG_CTRL.CGCG_INT_ENABLE (:4235-4247)
		const uint32_t s0 = rdGc(Sdma0RlcCgcgCtrl) | kSdmaCgcgIntEnable;
		wr(IpDiscovery::HwGc, Sdma0RlcCgcgCtrl, s0);
		const uint32_t s1 = rdGc(Sdma1RlcCgcgCtrl) | kSdmaCgcgIntEnable;
		wr(IpDiscovery::HwGc, Sdma1RlcCgcgCtrl, s1);
		CLOG("cg: SDMA0/1_RLC_CGCG_CTRL -> 0x%08x/0x%08x", s0, s1);
	} else {
		// the enable=false branch (:4249-4265)
		gfxCgRmw(RlcCgcgCglsCtrl, "RLC_CGCG_CGLS_CTRL", kCgEn | kCglsEn, 0);
		gfxCgRmw(RlcCgcgCglsCtrl3d, "RLC_CGCG_CGLS_CTRL_3D", kCgEn | kCglsEn, 0);
	}
}

void RDNA4Compute::gfxCgMedium(bool enable) {
	trail("pm: cg medium");
	const uint32_t mgcg = kCgOvrGrbmSclk | kCgOvrRlcSclk | kCgOvrMgcg;
	// enable clears the three overrides (:4277-4287); disable sets them (:4290-4298)
	gfxCgRmw(RlcCgttMgcgOverride, "RLC_CGTT_MGCG_OVERRIDE", enable ? mgcg : 0, enable ? 0 : mgcg);
}

void RDNA4Compute::gfxCgFine(bool enable) {
	trail("pm: cg fine");
	const uint32_t rep = kCgOvrRepeaterFgcg | kCgOvrRlcRepeaterFgcg;
	gfxCgRmw(RlcCgttMgcgOverride, "RLC_CGTT_MGCG_OVERRIDE (repeater FGCG)", enable ? rep : 0, enable ? 0 : rep);
	gfxCgRmw(RlcCgttMgcgOverride, "RLC_CGTT_MGCG_OVERRIDE (SRAM FGCG)", enable ? kCgOvrFgcg : 0,
	         enable ? 0 : kCgOvrFgcg);
	gfxCgRmw(RlcCgttMgcgOverride, "RLC_CGTT_MGCG_OVERRIDE (perf clock)", enable ? kCgOvrPerfmon : 0,
	         enable ? 0 : kCgOvrPerfmon);
}

// gfx_v12_0_enable_gui_idle_interrupt for ME0 pipe 0 only (gfx_v12_0_get_cpg_int_cntl
// returns CP_INT_CNTL_RING0 for pipe 0 and 0 otherwise, gfx.c:1874-1886).
void RDNA4Compute::gfxCgGuiIdle(bool enable) {
	trail("pm: cg gui idle");
	const uint32_t v = enable ? (rdGc(CpIntCntlRing0) | kCpIntGuiIdleBits) : (rdGc(CpIntCntlRing0) & ~kCpIntGuiIdleBits);
	wr(IpDiscovery::HwGc, CpIntCntlRing0, v);
	CLOG("cg: CP_INT_CNTL_RING0 -> 0x%08x", v);
}

// Clock gating is on by default, as in amdgpu (late init, amdgpu_device.c:2772): the round-4 card
// went from 2541 MHz / 100 % / 131 W to 803 MHz / 3 % / 20 W with it. rdna4-gfxcg=<mask> selects
// steps, rdna4-gfxcg=0 writes amdgpu's disable branch.
bool RDNA4Compute::gfxCgIsDefault() {
	uint32_t v = 0;
	return !PE_parse_boot_argn("rdna4-gfxcg", &v, sizeof(v));
}

bool RDNA4Compute::requestedGfxCg(uint32_t &mask) {
	uint32_t v = 0;
	if (!PE_parse_boot_argn("rdna4-gfxcg", &v, sizeof(v))) {
		mask = kCgStepCoarse | kCgStepMedium | kCgStepFine | kCgStepGuiIdle;
		return true;
	}
	mask = v & (kCgStepCoarse | kCgStepMedium | kCgStepFine | kCgStepGuiIdle);
	return true;
}

// Applied at the end of bring-up (after the boot self-tests), bounded and logged
// with a survey and SMU samples before and after. mask 0 = the disable mirror.
void RDNA4Compute::gfxCgApply(uint32_t mask) {
	const bool enable = mask != 0;
	CLOG("cg: rdna4-gfxcg=0x%x%s: %s (%s%s%s%s); persists in the RLC across warm reboots, "
	     "rdna4-gfxcg=0 undoes it", mask, gfxCgIsDefault() ? " (default)" : "",
	     enable ? "enable clock gating" : "disable clock gating",
	     (mask & kCgStepCoarse) ? "coarse " : "", (mask & kCgStepMedium) ? "medium " : "",
	     (mask & kCgStepFine) ? "fine " : "", (mask & kCgStepGuiIdle) ? "gui-idle" : "");
	if (!(rdGc(RlcCntl) & kRlcEnableF32)) {
		CLOG("cg: RLC is not running (RLC_CNTL 0x%08x); clock gating skipped", rdGc(RlcCntl));
		return;
	}
	gfxPmSurvey("cg before");
	gfxPmSample("cg before");
	trail("pm: cg enter safe mode");
	const bool safe = rlcSafeMode(true);
	if (!safe) {
		CLOG("cg: RLC safe mode did not acknowledge (RLC_SAFE_MODE 0x%08x); nothing written", rdGc(RlcSafeMode));
	} else {
		if (enable) {
			if (mask & kCgStepCoarse)
				gfxCgCoarse(true);
			if (mask & kCgStepMedium)
				gfxCgMedium(true);
			if (mask & kCgStepFine)
				gfxCgFine(true);
			if (mask & kCgStepGuiIdle)
				gfxCgGuiIdle(true);
		} else {
			trail("pm: cg disable");
			gfxCgCoarse(false);
			gfxCgMedium(false);
			gfxCgFine(false);
			gfxCgGuiIdle(false);
		}
	}
	trail("pm: cg exit safe mode");
	rlcSafeMode(false);
	gfxPmSample("cg after (+0.3 s)");
	IOSleep(700);
	gfxPmSample("cg after (+1.3 s)");
	gfxPmSurvey("cg after");
	CLOG("cg: finished");
}

// W24: which engine is busy? Read-only survey, logged at each bring-up stage.
// Registers (gc_12_0_0_sh_mask.h): GRBM_STATUS bit 31 GUI_ACTIVE, 29 CP_BUSY,
// 27 ANY_ACTIVE, 22 SPI_BUSY, 11-13 SC/DB/CB_CLEAN; GRBM_STATUS2 RLC/CPC/CPF/CPG/
// SDMA/EA/UTCL2 busy; GRBM_STATUS_SE0..3 per shader engine; CP_STAT/CP_BUSY_STAT,
// CP_CPC_STATUS/BUSY_STAT, CP_CPF_STATUS/BUSY_STAT; RLC_STAT/GPM_STAT/SAFE_MODE;
// CP_MES_CNTL, CP_ME_CNTL, CP_MEC_RS64_CNTL (halt/active); CP_HQD_ACTIVE for every
// MEC pipe/queue and CP_GFX_HQD_ACTIVE for the gfx queues (gfx_v12_0_kiq/kcq
// setup); RLC_CGTT_MGCG_OVERRIDE / RLC_CGCG_CGLS_CTRL for the gating state
// (gfx_v12_0_update_coarse_grain_clock_gating, gfx_v12_0.c:4151-4268). All reads.
void RDNA4Compute::gfxPmSurvey(const char *tag) {
	trail("pm: survey");
	CLOG("pm: survey %s: GRBM 0x%08x GRBM2 0x%08x SE0-3 0x%08x/0x%08x/0x%08x/0x%08x", tag,
	     rdGc(GrbmStatus), rdGc(GrbmStatus2), rdGc(GrbmStatusSe0), rdGc(GrbmStatusSe1),
	     rdGc(GrbmStatusSe2), rdGc(GrbmStatusSe3));
	CLOG("pm: survey %s: CP_STAT 0x%08x CP_BUSY 0x%08x CPC_STAT 0x%08x CPC_BUSY 0x%08x CPF_STAT 0x%08x "
	     "CPF_BUSY 0x%08x", tag, rdGc(CpStat), rdGc(CpBusyStat), rdGc(CpCpcStatus), rdGc(CpCpcBusyStat),
	     rdGc(CpCpfStatus), rdGc(CpCpfBusyStat));
	CLOG("pm: survey %s: RLC_CNTL 0x%08x RLC_STAT 0x%08x RLC_GPM_STAT 0x%08x RLC_SAFE_MODE 0x%08x "
	     "MES_CNTL 0x%08x ME_CNTL 0x%08x MEC_RS64_CNTL 0x%08x SDMA0 0x%08x SDMA1 0x%08x", tag,
	     rdGc(RlcCntl), rdGc(RlcStat), rdGc(RlcGpmStat), rdGc(RlcSafeMode), rdGc(CpMesCntl),
	     rdGc(CpMeCntl), rdGc(CpMecRs64Cntl), rdGc(sdma(0, SdmaStatusReg)), rdGc(sdma(1, SdmaStatusReg)));
	CLOG("pm: survey %s: RLC_CGTT_MGCG_OVERRIDE 0x%08x RLC_CGCG_CGLS_CTRL 0x%08x (CGCG_EN bit0, CGLS_EN bit1; "
	     "override bits set = gating held off)", tag, rdGc(RlcCgttMgcgOverride), rdGc(RlcCgcgCglsCtrl));
	char mec[200], gfx[64];
	size_t nm = 0, ng = 0;
	uint32_t activeMec = 0, activeGfx = 0;
	mec[0] = gfx[0] = 0;
	// amdgpu's geometry for GC 12.0.0/12.0.1: one MEC with 2 pipes x 4 queues
	// (gfx_v12_0_sw_init, gfx_v12_0.c:1416-1424), one ME with 1 pipe x 8 queues.
	// A read of 0xffffffff means the bank is not implemented, never "active".
	uint32_t notImplemented = 0;
	for (uint32_t pipe = 0; pipe < 2; pipe++) {
		for (uint32_t q = 0; q < 4; q++) {
			grbmSelect(1, pipe, q, 0);
			const uint32_t v = rdGc(CpHqdActive);
			if (v == kBad)
				notImplemented++;
			else if (v & 1) {
				activeMec++;
				if (nm + 8 < sizeof(mec))
					nm += snprintf(mec + nm, sizeof(mec) - nm, " %u/%u", pipe, q);
			}
		}
	}
	for (uint32_t pipe = 0; pipe < 1; pipe++) {
		for (uint32_t q = 0; q < 8; q++) {
			grbmSelect(0, pipe, q, 0);
			const uint32_t v = rdGc(CpGfxHqdActive);
			if (v == kBad)
				notImplemented++;
			else if (v & 1) {
				activeGfx++;
				if (ng + 8 < sizeof(gfx))
					ng += snprintf(gfx + ng, sizeof(gfx) - ng, " %u/%u", pipe, q);
			}
		}
	}
	grbmSelect(0, 0, 0, 0);
	CLOG("pm: survey %s: active HQDs: MEC(pipe/queue) %u:%s; gfx(pipe/queue) %u:%s; %u bank reads 0xffffffff "
	     "(not implemented)", tag, activeMec, activeMec ? mec : " none", activeGfx, activeGfx ? gfx : " none",
	     notImplemented);
	RDNA4SensorsEx s;
	if (poolCpu && readSensorsEx(s))
		CLOG("pm: survey %s: SMU avg GFXCLK pre-DS %u post-DS %u MHz, GFX activity %u %%, socket %u W, "
		     "hotspot %u C, MetricsCounter %u (%s)", tag, s.avgGfxclkPreDsMHz, s.avgGfxclkPostDsMHz,
		     s.gfxActivity, s.socketPowerW, s.hotspotTempC, s.metricsCounter,
		     (s.flags & RDNA4_SENSORS_EX_LIVE) ? "live" : "STALE");
}

// Let the PMFW average for a moment, then log what the metrics table says.
void RDNA4Compute::gfxPmSample(const char *tag) {
	IOSleep(300);
	RDNA4SensorsEx s;
	if (!readSensorsEx(s)) {
		CLOG("pm: %s: metrics query failed", tag);
		return;
	}
	CLOG("pm: %s: avg GFXCLK pre-DS %u post-DS %u MHz (CurrClock %u, AvgCurrent %u A: not trusted on IF 0x33), "
	     "GFX activity %u %%, UCLK activity %u %%, VDD_GFX %u mV, socket %u W, hotspot %u C, "
	     "MetricsCounter %u (%s), throttle mask 0x%x, GRBM 0x%08x CP_STAT 0x%08x",
	     tag, s.avgGfxclkPreDsMHz, s.avgGfxclkPostDsMHz, s.currGfxclkMHz, s.vddGfxCurrentA, s.gfxActivity,
	     s.uclkActivity, s.vddGfxMv, s.socketPowerW, s.hotspotTempC,
	     s.metricsCounter, (s.flags & RDNA4_SENSORS_EX_LIVE) ? "live" : "STALE", s.throttlingMask,
	     rdGc(GrbmStatus), rdGc(CpStat));
}

// smu_v14_0_set_soft_freq_limited_range: param = (PPCLK_GFXCLK << 16) | value.
bool RDNA4Compute::gfxPmSoftLimits(uint32_t maxParam, uint32_t minParam, const char *what) {
	uint32_t ret = 0;
	const uint32_t rMax = smuSend(kSmuMsgSetSoftMaxByFreq, maxParam, ret, 100);
	const uint32_t rMin = minParam == kPmNoMin ? kSmuRespOk : smuSend(kSmuMsgSetSoftMinByFreq, minParam, ret, 100);
	CLOG("pm: %s: SetSoftMaxByFreq(0x%08x) -> 0x%02x, SetSoftMinByFreq(0x%08x) -> 0x%02x", what,
	     maxParam, rMax, minParam == kPmNoMin ? 0 : minParam, rMin);
	return rMax == kSmuRespOk && rMin == kSmuRespOk;
}

// The GFXCLK soft max back to automatic, once and then once more if refused.
bool RDNA4Compute::gfxPmRestoreAuto(const char *why) {
	for (int attempt = 1; attempt <= 2; attempt++) {
		if (gfxPmSoftLimits(kPmSoftMaxAuto, kPmNoMin, why))
			return true;
		CLOG("pm: %s: restore attempt %d was refused", why, attempt);
	}
	return false;
}

// The previous boot died inside "pm: cap probe" or "pm: cap restore": the SMU
// may still hold the 1001 MHz soft max (a warm reboot keeps it). Lift it once.
void RDNA4Compute::gfxPmRecoverCap() {
	trail("pm: cap recover");
	if (gfxPmRestoreAuto("previous boot died in a cap step: GFXCLK soft max -> automatic"))
		CLOG("pm: the GFXCLK soft max was lifted");
	else
		CLOG("pm: WARNING: the GFXCLK soft max could not be lifted; compute may stay limited to about "
		     "1000 MHz until a cold power cycle");
}

void RDNA4Compute::gfxPmExperiment(uint32_t mask) {
	CLOG("pm: rdna4-gfxpm=0x%x: %s%s%s%s", mask, (mask & kPmSampleOnly) ? "sample " : "",
	     (mask & kPmWorkload) ? "workload " : "", (mask & kPmSoftAuto) ? "soft-limits-auto " : "",
	     (mask & kPmCapProbe) ? "cap-probe" : "");
	if (!poolCpu) {
		CLOG("pm: no compute pool; experiment skipped");
		return;
	}
	trail("pm: baseline sample");
	gfxPmSample("baseline (nothing changed)");
	if (mask & kPmSurvey)
		gfxPmSurvey("pm baseline");
	uint32_t ret = 0;
	if (mask & kPmWorkload) {
		trail("pm: SetWorkloadMask");
		const uint32_t resp = smuSend(kSmuMsgSetWorkloadMask, 1u << kWorkloadPplibDefaultBit, ret, 100);
		CLOG("pm: SetWorkloadMask(WORKLOAD_PPLIB_DEFAULT) -> 0x%02x", resp);
		if (resp != kSmuRespOk) {
			CLOG("pm: workload mask refused; experiment stopped");
			return;
		}
		gfxPmSample("after SetWorkloadMask(DEFAULT)");
	}
	if (mask & kPmSoftAuto) {
		trail("pm: soft limits auto");
		if (!gfxPmSoftLimits(kPmSoftMaxAuto, 0, "GFXCLK soft limits -> automatic")) {
			CLOG("pm: soft limits refused; experiment stopped");
			return;
		}
		gfxPmSample("after GFXCLK soft limits automatic");
	}
	if (mask & kPmCapProbe) {
		trail("pm: cap probe");
		// SMU_V14_SOFT_FREQ_ROUND(max) is max + 1 (smu_v14_0.h:54).
		const bool capped = gfxPmSoftLimits(kPmProbeMHz + 1, kPmNoMin, "probe: GFXCLK soft max 1000 MHz");
		if (capped)
			gfxPmSample("with GFXCLK soft max 1000 MHz");
		trail("pm: cap restore");
		if (gfxPmRestoreAuto("probe: GFXCLK soft max -> automatic"))
			gfxPmSample("after the cap is lifted");
		else
			CLOG("pm: WARNING: the GFXCLK soft max could not be restored (2 attempts); compute stays limited "
			     "to about 1000 MHz, possibly across warm reboots (the next boot retries once); "
			     "a cold power cycle clears it");
	}
	CLOG("pm: experiment finished");
}

// ---------------------------------------------------------------------------
// Stage 2: PSP secure OS, GPCOM ring, SMU firmware
// ---------------------------------------------------------------------------

bool RDNA4Compute::stagePsp() {
#ifdef RDNA4FB_NO_FIRMWARE
	CLOG("psp: this build carries no firmware (firmware/amdgpu/ was empty at build time)");
	return false;
#else
	OSDictionary *d = OSDictionary::withCapacity(12);
	auto put = [d](const char *key, uint64_t v) {
		if (OSNumber *n = d ? OSNumber::withNumber(v, 64) : nullptr) {
			d->setObject(key, n);
			n->release();
		}
	};
	auto publish = [this, d]() {
		if (d) {
			env.owner->setProperty("Compute,PSP", d);
			d->release();
		}
	};

	if (!pool.valid || !sv.hdpMemFlushRemap || sv.hdpMemFlushRemap == kBad) {
		CLOG("psp: no compute pool or no HDP flush path; not touching the PSP");
		publish();
		return false;
	}
	if (!mapPool()) {
		CLOG("psp: could not map the compute pool");
		publish();
		return false;
	}
	CLOG("psp: pool mapped at VRAM+0x%llx (MC 0x%llx), first dwords %08x %08x %08x %08x",
	     pool.offset, pool.mcAddress, reinterpret_cast<volatile uint32_t *>(poolCpu)[0],
	     reinterpret_cast<volatile uint32_t *>(poolCpu)[1],
	     reinterpret_cast<volatile uint32_t *>(poolCpu)[2],
	     reinterpret_cast<volatile uint32_t *>(poolCpu)[3]);

	AmdFw::PspPackage pkg;
	AmdFw::Blob smu;
	const size_t sosLen = static_cast<size_t>(rdna4_fw_psp_sos_end - rdna4_fw_psp_sos);
	const size_t smuLen = static_cast<size_t>(rdna4_fw_smu_end - rdna4_fw_smu);
	if (!AmdFw::parsePsp(rdna4_fw_psp_sos, sosLen, pkg) ||
	    !AmdFw::payload(rdna4_fw_smu, smuLen, smu)) {
		CLOG("psp: embedded firmware does not parse (sos %lu, smu %lu bytes)",
		     static_cast<unsigned long>(sosLen), static_cast<unsigned long>(smuLen));
		publish();
		return false;
	}
	CLOG("psp: firmware sOS 0x%08x (%u parts), SMU %u bytes", pkg.version[AmdFw::PspSos],
	     pkg.count, smu.size);

	Psp::Bus bus { this, pspRead, pspWrite, pspDelay, pspFlush };
	Psp::Window win { poolCpu, pool.mcAddress, static_cast<uint32_t>(pool.size) };
	if (!psp.init(bus, win)) {
		CLOG("psp: window rejected (MC 0x%llx, %llu bytes)", pool.mcAddress, pool.size);
		publish();
		return false;
	}

	// 1. The secure OS, through the bootloader.
	trail("s2: PSP bootloader (sOS components)");
	uint32_t loaded = 0;
	Psp::Result r = psp.loadSos(pkg, loaded);
	CLOG("psp: bootloader: %s (0x%08x), %u component(s) loaded", r.what, r.value, loaded);
	put("BootloaderLoaded", loaded);
	put("SOSVersion", psp.sosVersion());
	if (!r.ok) {
		publish();
		return false;
	}

	// 2. The kernel-mode command ring.
	trail("s2: GPCOM ring create");
	r = psp.createRing();
	CLOG("psp: %s (C2PMSG_64=0x%08x)", r.what, r.value);
	put("RingCreated", r.ok);
	if (!r.ok) {
		publish();
		return false;
	}

	// 3. LOAD_TOC: a first command that changes nothing we depend on, and
	//    tells how much TMR this firmware set needs (the boot-time TMR the
	//    bootloader set up is used; no SETUP_TMR on this ASIC).
	trail("s2: LOAD_TOC");
	uint32_t tmr = 0;
	r = psp.loadToc(pkg.part[AmdFw::PspToc], tmr);
	CLOG("psp: %s status 0x%x, TMR size 0x%x", r.what, r.value, tmr);
	put("TOCStatus", r.value);
	put("TMRSize", tmr);
	if (!r.ok) {
		publish();
		return false;
	}

	// 4. The SMU (power management) firmware.
	trail("s2: LOAD_IP_FW SMU");
	Psp::Response resp;
	r = psp.loadIpFw(smu, Psp::FwSmu, resp);
	CLOG("psp: LOAD_IP_FW(SMU) %s status 0x%x, TMR address 0x%08x%08x", r.what, r.value,
	     resp.fwAddrHi, resp.fwAddrLo);
	put("SMULoadStatus", r.value);
	if (!r.ok) {
		publish();
		return false;
	}

	// 5. Proof: the SMU now answers its mailbox (today it does not).
	trail("s2: SMU ping");
	// The PMFW posts a nonzero response once it takes messages
	// (__smu_cmn_poll_stat before the first send); then one message, polled
	// — resending into a booting mailbox can leave it out of step.
	uint32_t ret = 0, answer = 0;
	for (uint32_t ms = 0; ms < 3000; ms += 10) {
		const uint32_t r0 = rd(IpDiscovery::HwMp1, SmuResp);
		if (r0 != 0 && r0 != kBad)
			break;
		IOSleep(10);
	}
	answer = smuSend(kSmuMsgTest, 0xC0FFEE, ret, 2000);
	if (answer != 1) {
		CLOG("psp: SMU still silent after loading its firmware (resp 0x%x)", answer);
		put("SMUAlive", 0);
		publish();
		return false;
	}
	uint32_t ver = 0;
	smuSend(kSmuMsgGetVersion, 0, ver, 100);
	CLOG("psp: SMU answers: TestMessage ok, PMFW version 0x%08x (%u.%u.%u)", ver,
	     (ver >> 16) & 0xff, (ver >> 8) & 0xff, ver & 0xff);
	put("SMUAlive", 1);
	put("SMUVersion", ver);
	publish();
	return true;
#endif
}

// ---------------------------------------------------------------------------
// Stage 3: GC firmware into the TMR, RLC autoload
// ---------------------------------------------------------------------------

bool RDNA4Compute::stageGfx() {
#ifdef RDNA4FB_NO_FIRMWARE
	return false;
#else
	OSDictionary *d = OSDictionary::withCapacity(12);
	auto put = [d](const char *key, uint64_t v) {
		if (OSNumber *n = d ? OSNumber::withNumber(v, 64) : nullptr) {
			d->setObject(key, n);
			n->release();
		}
	};
	auto publish = [this, d]() {
		if (d) {
			env.owner->setProperty("Compute,GFX", d);
			d->release();
		}
	};

	const AmdFw::GfxBlobs blobs { FW_BLOB(sdma), FW_BLOB(pfp), FW_BLOB(me), FW_BLOB(mec),
	                              FW_BLOB(mes), FW_BLOB(imu), FW_BLOB(rlc) };
	AmdFw::GfxImage img[AmdFw::kMaxGfxImages];
	const char *why = nullptr;
	const uint32_t n = AmdFw::buildGfxImages(blobs, img, AmdFw::kMaxGfxImages, &why);
	if (!n) {
		CLOG("gfx: embedded GC firmware does not parse (%s)", why ? why : "?");
		publish();
		return false;
	}
	CLOG("gfx: %u firmware images for the RLC autoload", n);

	// 1. Everything into the TMR, in amdgpu's order; RLC_G is last.
	for (uint32_t i = 0; i < n; i++) {
		Psp::Response resp;
		char step[48];
		snprintf(step, sizeof(step), "s3: LOAD_IP_FW %s", img[i].name);
		trail(step);
		Psp::Result r = psp.loadIpFw(img[i].payload, img[i].pspType, resp);
		CLOG("gfx: LOAD_IP_FW %-21s type %2u %7u bytes -> %s (0x%x)", img[i].name,
		     img[i].pspType, img[i].payload.size, r.what, r.value);
		if (!r.ok) {
			put("FailedImage", i);
			put("FailedStatus", r.value);
			publish();
			return false;
		}
	}
	put("ImagesLoaded", n);

	// 2. Let the RLC boot GFX from the TMR.
	trail("s3: AUTOLOAD_RLC");
	Psp::Result r = psp.autoloadRlc();
	CLOG("gfx: %s (0x%x)", r.what, r.value);
	put("AutoloadStatus", r.value);
	if (!r.ok) {
		publish();
		return false;
	}

	// 3. amdgpu's smu_hw_init runs here, before the GFX block waits: on this
	//    dGPU nothing else releases the IMU (IMU_CORE_CTRL 0x9 = CRESET |
	//    DRESET, seen on the card with the autoload armed and no SMU setup).
	//    First offer every feature except those that retune memory, fabric
	//    or display clocks under the GOP's live scanout, and GFXOFF (it would
	//    power GFX down under us); widen only if GFX does not come up.
	CLOG("gfx: after AUTOLOAD_RLC: IMU core 0x%08x, GFX reset 0x%08x", rdGc(ImuCoreCtrl),
	     rdGc(ImuGfxResetCtrl));
	static const uint32_t kHeldBack[] = { kSmuFeatDpmUclk, kSmuFeatDpmFclk, kSmuFeatDpmDcn,
	                                      kSmuFeatVmempScaling, kSmuFeatVddioMemScaling,
	                                      kSmuFeatDsFclk, kSmuFeatDsDcfclk, kSmuFeatDsUclk,
	                                      kSmuFeatGfxoff, kSmuFeatDfCstate, kSmuFeatAthubMmhubPg };
	uint64_t displaySafe = ~0ull;
	for (uint32_t bit : kHeldBack)
		displaySafe &= ~(1ull << bit);
	trail("s3: SMU features (display-safe mask)");
	uint64_t running = smuEnableFeatures(displaySafe, 0);
	put("SmuFeatures", running);

	// 4. gfx_v12_0_wait_for_rlc_autoload_complete: CP idle and the RLC
	//    reports its bootload complete.
	trail("s3: wait for RLC bootload");
	uint32_t cpStat = kBad, boot = kBad;
	bool complete = waitRlcAutoload(2000, cpStat, boot);
	if (!complete) {
		// The GFX power domain alone (amdgpu's enable_gfx_features, proven
		// on 14.0.2; this card's MP1 is 14.0.3), then every feature.
		CLOG("gfx: not up with the display-safe features; IMU core 0x%08x, GFX reset 0x%08x",
		     rdGc(ImuCoreCtrl), rdGc(ImuGfxResetCtrl));
		trail("s3: SMU EnableAllSmuFeatures(GFX)");
		uint32_t ret = 0;
		CLOG("gfx: EnableAllSmuFeatures(PWR_GFX) -> 0x%02x",
		     smuSend(kSmuMsgEnableAllFeatures, kSmuPwrDomainGfx, ret, 2000));
		complete = waitRlcAutoload(1000, cpStat, boot);
	}
	if (!complete) {
		trail("s3: SMU features (full mask, fallback)");
		running = smuEnableFeatures(~0ull, 0);
		put("SmuFeatures", running);
		complete = waitRlcAutoload(2000, cpStat, boot);
	}
	put("CP_STAT", cpStat);
	put("RLC_BOOTLOAD_STATUS", boot);
	if (!complete) {
		CLOG("gfx: RLC autoload did not complete (CP_STAT 0x%08x, bootload 0x%08x)", cpStat, boot);
		// How far GFX got: IMU out of reset? GFX domains released? RLC on?
		CLOG("gfx: IMU core 0x%08x, GFX reset 0x%08x, RLC_CNTL 0x%08x, RLC_STAT 0x%08x, GRBM 0x%08x",
		     rdGc(ImuCoreCtrl), rdGc(ImuGfxResetCtrl), rdGc(RlcCntl), rdGc(RlcStat),
		     rdGc(GrbmStatus));
		put("IMU_CORE_CTRL", rdGc(ImuCoreCtrl));
		put("IMU_GFX_RESET_CTRL", rdGc(ImuGfxResetCtrl));
		publish();
		return false;
	}

	// What GFX looks like now, next to the stage 1 survey.
	const uint32_t imu = rdGc(ImuCoreCtrl), gfxReset = rdGc(ImuGfxResetCtrl);
	const uint32_t rlc = rdGc(RlcCntl), grbm = rdGc(GrbmStatus), mec = rdGc(CpMecRs64Cntl);
	const uint32_t sdma0 = rdGc(sdma(0, SdmaStatusReg)), sdma1 = rdGc(sdma(1, SdmaStatusReg));
	CLOG("gfx: RLC autoload complete: bootload 0x%08x, IMU core 0x%08x, GFX reset 0x%08x, "
	     "RLC_CNTL 0x%08x, GRBM 0x%08x, MEC 0x%08x", boot, imu, gfxReset, rlc, grbm, mec);
	CLOG("gfx: SDMA0 status 0x%08x (ucode init %s), SDMA1 status 0x%08x (ucode init %s)", sdma0,
	     (sdma0 & kSdmaUcodeInitDone) ? "done" : "not done", sdma1,
	     (sdma1 & kSdmaUcodeInitDone) ? "done" : "not done");
	put("IMU_CORE_CTRL", imu);
	put("IMU_GFX_RESET_CTRL", gfxReset);
	put("RLC_CNTL", rlc);
	put("SDMA0_STATUS", sdma0);
	put("SDMA1_STATUS", sdma1);

	// The GC hub read all-zero while GFX was in reset (stage 1). Now that GC
	// is up, whether the VBIOS programmed its FB aperture decides how much
	// of it stage 4 must set up itself.
	const uint32_t fbBase = rdGc(GcFbLocationBase), fbTop = rdGc(GcFbLocationTop);
	CLOG("gfx: GC hub now: fb base 0x%08x top 0x%08x offset 0x%08x, agp 0x%08x/0x%08x/0x%08x, "
	     "sys_low 0x%08x, l2 0x%08x, ctx0 0x%08x, l1 tlb 0x%08x", fbBase, fbTop,
	     rdGc(GcFbOffset), rdGc(GcAgpBase), rdGc(GcAgpBot), rdGc(GcAgpTop),
	     rdGc(GcSysApertureLow), rdGc(GcL2Cntl), rdGc(GcCtx0Cntl), rdGc(GcMxL1TlbCntl));
	if (requestedVm())
		vmDumpHubWindows(" after SMU enable");   // W17 E1, read-only, before gcHubInit
	put("GCMC_FB_BASE", fbBase);
	put("GCMC_FB_TOP", fbTop);
	publish();
	return true;
#endif
}

bool RDNA4Compute::waitRlcAutoload(uint32_t ms, uint32_t &cpStat, uint32_t &boot) {
	for (uint32_t t = 0; t < ms; t++) {
		cpStat = rdGc(CpStat);
		boot = rdGc(RlcBootloadStatus);
		if (cpStat == 0 && boot != kBad && (boot & kRlcBootComplete))
			return true;
		IOSleep(1);
	}
	return false;
}

// smu_smc_hw_setup's messages up to system_features_control (smu_v14_0_2):
// driver table location, RunDcBtc, the allowed mask (refused with SCPM on:
// the PMFW then runs the pptable's own feature set), EnableAllSmuFeatures.
uint64_t RDNA4Compute::smuEnableFeatures(uint64_t allowed, uint32_t domain) {
	uint32_t ret = 0, resp;
	auto running = [this]() -> uint64_t {
		uint32_t lo = 0, hi = 0;
		if (smuSend(kSmuMsgGetRunningFeaturesLow, 0, lo, 100) != kSmuRespOk ||
		    smuSend(kSmuMsgGetRunningFeaturesHigh, 0, hi, 100) != kSmuRespOk)
			return 0;
		return lo | (static_cast<uint64_t>(hi) << 32);
	};

	resp = smuSend(kSmuMsgGetDriverIfVersion, 0, ret, 100);
	CLOG("smu: driver interface version 0x%x (resp 0x%02x; amdgpu expects 0x2e)", ret, resp);
	CLOG("smu: running features before: 0x%016llx", running());

	const uint64_t table = poolMc(kSmuTableOffset);
	for (uint32_t off = 0; off < 0x10000; off += 4)
		*poolDw(kSmuTableOffset + off) = 0;
	flushHdp();
	const uint32_t rh = smuSend(kSmuMsgSetDriverDramAddrHigh, static_cast<uint32_t>(table >> 32), ret, 100);
	const uint32_t rl = smuSend(kSmuMsgSetDriverDramAddrLow, static_cast<uint32_t>(table), ret, 100);
	CLOG("smu: driver table at MC 0x%llx (resp 0x%02x/0x%02x)", table, rh, rl);

	resp = smuSend(kSmuMsgRunDcBtc, 0, ret, 2000);
	CLOG("smu: RunDcBtc -> 0x%02x", resp);

	const uint32_t mh = smuSend(kSmuMsgSetAllowedMaskHigh, static_cast<uint32_t>(allowed >> 32), ret, 100);
	const uint32_t ml = smuSend(kSmuMsgSetAllowedMaskLow, static_cast<uint32_t>(allowed), ret, 100);
	CLOG("smu: allowed features 0x%016llx -> 0x%02x/0x%02x%s", allowed, mh, ml,
	     mh == kSmuRespRejectedPrereq ? " (refused: SCPM, the pptable decides)" : "");

	resp = smuSend(kSmuMsgEnableAllFeatures, domain, ret, 2000);
	CLOG("smu: EnableAllSmuFeatures(%u) -> 0x%02x", domain, resp);
	// GFXOFF stays disallowed (amdgpu's initial gfx_off_req_count is 1).
	resp = smuSend(kSmuMsgDisallowGfxOff, 0, ret, 100);

	const uint64_t now = running();
	auto on = [now](uint32_t bit) { return (now >> bit) & 1 ? "on" : "off"; };
	CLOG("smu: running features after: 0x%016llx — GFX_IMU %s, GFXCLK DPM %s, UCLK DPM %s, "
	     "FCLK DPM %s, DCN DPM %s, GFXOFF %s, FW_CTF %s, fan %s (DisallowGfxOff 0x%02x)", now,
	     on(kSmuFeatGfxImu), on(kSmuFeatDpmGfxclk), on(kSmuFeatDpmUclk), on(kSmuFeatDpmFclk),
	     on(kSmuFeatDpmDcn), on(kSmuFeatGfxoff), on(kSmuFeatFwCtf), on(kSmuFeatFanControl), resp);
	return now;
}

// ---------------------------------------------------------------------------
// Stage 4: GC hub + SDMA
// ---------------------------------------------------------------------------

// gfxhub_v12_0_gart_enable, reduced to VMID0 with no GART in use: VRAM is
// reached through the FB aperture / system aperture, the context-0 page
// table covers one unused page. The MM hub (display, PSP) is not touched.
bool RDNA4Compute::gcHubInit() {
	if (!sv.fbMcTop)
		return false;
	const uint32_t mmBase = sv.mmFbBase & 0xffffff, mmTop = sv.mmFbTop & 0xffffff;

	// FB aperture: amdgpu takes the MM hub's as the truth for both hubs
	// (gmc_v12_0_vram_gtt_location); make the GC hub's match it, offset too.
	uint32_t gcBase = rdGc(GcFbLocationBase) & 0xffffff, gcTop = rdGc(GcFbLocationTop) & 0xffffff;
	if (gcBase != mmBase || gcTop != mmTop) {
		CLOG("sdma: GC hub FB aperture 0x%x..0x%x; set to the MM hub's 0x%x..0x%x", gcBase, gcTop,
		     mmBase, mmTop);
		wr(IpDiscovery::HwGc, GcFbLocationBase, mmBase);
		wr(IpDiscovery::HwGc, GcFbLocationTop, mmTop);
	}
	const uint32_t fbOffset = sv.mmFbOffset & 0xffffff;
	wr(IpDiscovery::HwGc, GcFbOffset, fbOffset);
	sv.gcFbOffset = fbOffset;
	// Page-table and default-page addresses are physical (VRAM offset plus
	// FB_OFFSET), as gmc_v12_0_get_vm_pde makes them, not MC addresses.
	const uint64_t phys = static_cast<uint64_t>(fbOffset) << 24;

	// Context 0: a flat table over one page nobody uses (MC page 0).
	for (uint32_t i = 0; i < 0x1000 / 4; i++) {
		*poolDw(kPtOffset + i * 4) = 0;
		*poolDw(kScratchOffset + i * 4) = 0;
	}
	flushHdp();
	const uint64_t pt = phys + pool.offset + kPtOffset;
	wr(IpDiscovery::HwGc, GcCtx0PtBaseLo, static_cast<uint32_t>(pt) | 1);
	wr(IpDiscovery::HwGc, GcCtx0PtBaseHi, static_cast<uint32_t>(pt >> 32));
	wr(IpDiscovery::HwGc, GcCtx0PtStartLo, 0);
	wr(IpDiscovery::HwGc, GcCtx0PtStartHi, 0);
	wr(IpDiscovery::HwGc, GcCtx0PtEndLo, 0);
	wr(IpDiscovery::HwGc, GcCtx0PtEndHi, 0);

	// System aperture = the FB range, no AGP; defaults point at a scratch page.
	wr(IpDiscovery::HwGc, GcAgpBase, 0);
	wr(IpDiscovery::HwGc, GcAgpBot, 0xffffff);
	wr(IpDiscovery::HwGc, GcAgpTop, 0);
	wr(IpDiscovery::HwGc, GcSysApertureLow, static_cast<uint32_t>(sv.fbMcBase >> 18));
	wr(IpDiscovery::HwGc, GcSysApertureHigh, static_cast<uint32_t>(sv.fbMcTop >> 18));
	const uint64_t scratchPhys = phys + pool.offset + kScratchOffset;
	wr(IpDiscovery::HwGc, GcSysDefaultLsb, static_cast<uint32_t>(scratchPhys >> 12));
	wr(IpDiscovery::HwGc, GcSysDefaultMsb, static_cast<uint32_t>(scratchPhys >> 44));
	wr(IpDiscovery::HwGc, GcL2FaultDefaultLo, static_cast<uint32_t>(scratchPhys >> 12));
	wr(IpDiscovery::HwGc, GcL2FaultDefaultHi, static_cast<uint32_t>(scratchPhys >> 44));
	wr(IpDiscovery::HwGc, GcL2FaultCntl2, rdGc(GcL2FaultCntl2) | kL2FaultRetryRead);
	// A bad address then faults to the scratch page (logged below on a
	// timeout) instead of halting the engine that issued it.
	wr(IpDiscovery::HwGc, GcL2FaultCntl,
	   (rdGc(GcL2FaultCntl) | kL2FaultEnableDefaults) & ~kL2FaultCrashBits);
	wr(IpDiscovery::HwGc, CpDebug, rdGc(CpDebug) | kCpDebugUtcl1ErrorHaltDisable);

	// L1 TLB.
	uint32_t v = rdGc(GcMxL1TlbCntl);
	v &= ~(kL1TlbSysAccess3 | kL1TlbSysUnmapped | kL1TlbEcoMask | kL1TlbMtypeMask);
	v |= kL1TlbEnable | kL1TlbSysAccess3 | kL1TlbAdvDriver | kL1TlbMtypeUc;
	wr(IpDiscovery::HwGc, GcMxL1TlbCntl, v);

	// L2 cache.
	// No default page: amdgpu points faults at its dummy page in system
	// memory; with none here, a stray access must not reach a host address.
	v = rdGc(GcL2Cntl);
	v &= ~(kL2FragmentProcessing | kL2Pde0TagGenMode | kL2PdeFaultClassify | (3u << 19) |
	       kL2IdentityFragMask | kL2DefaultPageToSys);
	v |= kL2EnableCache | kL2Ctx1IdentityAccess;
	wr(IpDiscovery::HwGc, GcL2Cntl, v);
	wr(IpDiscovery::HwGc, GcL2Cntl2, rdGc(GcL2Cntl2) | kL2InvalidateL1Tlbs | kL2InvalidateL2Cache);
	wr(IpDiscovery::HwGc, GcL2Cntl3,
	   (kL2Cntl3Default & ~(kL2Cntl3BankMask | kL2Cntl3BigKMask)) | 9 | (6u << 15));
	wr(IpDiscovery::HwGc, GcL2Cntl4, kL2Cntl4Default & ~kL2Cntl4TapPhysMask);
	wr(IpDiscovery::HwGc, GcL2Cntl5, kL2Cntl5Default & ~kL2Cntl5SmallKMask);

	// VMID0 on, flat.
	v = rdGc(GcCtx0Cntl);
	v = (v & ~(kCtxDepthMask | kCtxRetryPermFault)) | kVmCtxEnable;
	wr(IpDiscovery::HwGc, GcCtx0Cntl, v);

	// Identity aperture off; every invalidation engine covers all addresses.
	wr(IpDiscovery::HwGc, GcIdentLowLo, 0xffffffff);
	wr(IpDiscovery::HwGc, GcIdentLowHi, 0xf);
	wr(IpDiscovery::HwGc, GcIdentHighLo, 0);
	wr(IpDiscovery::HwGc, GcIdentHighHi, 0);
	wr(IpDiscovery::HwGc, GcIdentOffsetLo, 0);
	wr(IpDiscovery::HwGc, GcIdentOffsetHi, 0);
	for (uint32_t e = 0; e < kGcInvEngines; e++) {
		wr(IpDiscovery::HwGc, Reg { 0, GcInvEng0RangeLo.dword + 2 * e }, 0xffffffff);
		wr(IpDiscovery::HwGc, Reg { 0, GcInvEng0RangeHi.dword + 2 * e }, 0x1f);
	}
	flushHdp();
	// A flush that is never acknowledged (seen on the card, 2026-09-28) is
	// logged, not fatal: nothing here is translated — VMID0 reaches VRAM
	// through the FB/system aperture — and stage 4's WRITE decides.
	if (!gcHubFlush())
		CLOG("sdma: continuing without the flush: VMID0 accesses bypass translation");
	return true;
}

void RDNA4Compute::logGcFault(const char *tag) {
	const uint32_t status = rdGc(GcL2FaultStatusLo);
	CLOG("%s: GC hub fault status 0x%08x%s (VMID %u, CID 0x%x, %s), VA 0x%llx", tag, status,
	     status ? "" : " (no fault)", (status >> 20) & 0xf, (status >> 9) & 0x1ff,
	     (status >> 18) & 1 ? "write" : "read", gcFaultVa());
}

// sdma_v7_0_start on bare metal: every MCU is unhalted before any queue
// register is written; the RS64 firmware's init (UCODE_INIT_DONE) may reset
// the queue context, so the queue is programmed after it.
bool RDNA4Compute::sdmaStartMcus() {
	for (uint32_t i = 0; i < kSdmaInstances; i++)
		wr(IpDiscovery::HwGc, sdma(i, SdmaMcuCntl),
		   rdGc(sdma(i, SdmaMcuCntl)) & ~(kSdmaMcuHalt | kSdmaMcuReset));
	uint32_t s0 = 0;
	for (uint32_t ms = 0; ms < 100; ms++) {
		s0 = rdGc(sdma(0, SdmaStatusReg));
		if (s0 != kBad && (s0 & kSdmaUcodeInitDone))
			break;
		IOSleep(1);
	}
	const uint32_t s1 = rdGc(sdma(1, SdmaStatusReg));
	CLOG("sdma: MCUs unhalted: SDMA0 status 0x%08x (ucode init %s), SDMA1 status 0x%08x", s0,
	     (s0 & kSdmaUcodeInitDone) ? "done" : "NOT done", s1);
	return s0 != kBad && (s0 & kSdmaUcodeInitDone);
}

// gmc_v12_0_flush_vm_hub for VMID0 on the GC hub (engine 17, no semaphore).
bool RDNA4Compute::gcHubFlush() {
	const Reg req { 0, GcInvEng0Req.dword + kGcInvEngGart }, ack { 0, GcInvEng0Ack.dword + kGcInvEngGart };
	wr(IpDiscovery::HwGc, req, kInvReqVmid0);
	for (uint32_t us = 0; us < 100000; us += 10) {
		if (rdGc(ack) & 1)
			return true;
		IODelay(10);
	}
	CLOG("sdma: GC hub TLB flush not acknowledged: REQ 0x%08x ACK 0x%08x SEM 0x%08x, L2 status "
	     "0x%08x cntl 0x%08x, ctx0 0x%08x, IMU core 0x%08x GFX reset 0x%08x RLC 0x%08x",
	     rdGc(req), rdGc(ack), rdGc(Reg { 0, GcInvEng0Sem.dword + kGcInvEngGart }),
	     rdGc(GcL2Status), rdGc(GcL2Cntl), rdGc(GcCtx0Cntl), rdGc(ImuCoreCtrl),
	     rdGc(ImuGfxResetCtrl), rdGc(RlcCntl));
	// Engine 0, for comparison: a hub that acks nothing, or just engine 17?
	const Reg req0 { 0, GcInvEng0Req.dword }, ack0 { 0, GcInvEng0Ack.dword };
	wr(IpDiscovery::HwGc, req0, kInvReqVmid0);
	uint32_t a0 = 0;
	for (uint32_t us = 0; us < 10000 && !(a0 & 1); us += 10) {
		a0 = rdGc(ack0);
		IODelay(10);
	}
	CLOG("sdma: engine 0 flush: ACK 0x%08x (%s)", a0, (a0 & 1) ? "acknowledged" : "not acknowledged");
	return false;
}

// gfx_v12_0_config_gfx_rs64: amdgpu runs it after the autoload wait and
// before the GC hub is enabled and flushed — every RS64 microengine gets its
// start address from its firmware header and a pipe-reset pulse; nothing is
// unhalted here (the MEC is, in stage 5).
void RDNA4Compute::cpConfigRs64() {
#ifndef RDNA4FB_NO_FIRMWARE
	auto start = [](const AmdFw::Blob &b, uint32_t &lo, uint32_t &hi) {
		if (b.size < 60)
			return false;
		auto le32 = [&b](uint32_t off) {
			return static_cast<uint32_t>(b.data[off]) | (static_cast<uint32_t>(b.data[off + 1]) << 8) |
			       (static_cast<uint32_t>(b.data[off + 2]) << 16) |
			       (static_cast<uint32_t>(b.data[off + 3]) << 24);
		};
		lo = le32(52);                     // gfx_firmware_header_v2_0 ucode_start_addr_lo
		hi = le32(56);
		return true;
	};
	uint32_t lo = 0, hi = 0;
	if (start(FW_BLOB(pfp), lo, hi)) {
		for (uint32_t pipe = 0; pipe < 2; pipe++) {
			grbmSelect(0, pipe, 0, 0);
			wr(IpDiscovery::HwGc, CpPfpPrgrmStart, (hi << 30) | (lo >> 2));
			wr(IpDiscovery::HwGc, CpPfpPrgrmStartHi, hi >> 2);
		}
		grbmSelect(0, 0, 0, 0);
		const uint32_t v = rdGc(CpMeCntl);
		wr(IpDiscovery::HwGc, CpMeCntl, v | kCpMePfpPipeReset);
		wr(IpDiscovery::HwGc, CpMeCntl, v & ~kCpMePfpPipeReset);
	}
	if (start(FW_BLOB(me), lo, hi)) {
		for (uint32_t pipe = 0; pipe < 2; pipe++) {
			grbmSelect(0, pipe, 0, 0);
			wr(IpDiscovery::HwGc, CpMePrgrmStart, (hi << 30) | (lo >> 2));
			wr(IpDiscovery::HwGc, CpMePrgrmStartHi, hi >> 2);
		}
		grbmSelect(0, 0, 0, 0);
		const uint32_t v = rdGc(CpMeCntl);
		wr(IpDiscovery::HwGc, CpMeCntl, v | kCpMeMePipeReset);
		wr(IpDiscovery::HwGc, CpMeCntl, v & ~kCpMeMePipeReset);
	}
	if (start(FW_BLOB(mec), lo, hi)) {
		for (uint32_t pipe = 0; pipe < 4; pipe++) {
			grbmSelect(1, pipe, 0, 0);
			wr(IpDiscovery::HwGc, CpMecPrgrmStart, (lo >> 2) | (hi << 30));
			wr(IpDiscovery::HwGc, CpMecPrgrmStartHi, hi >> 2);
		}
		grbmSelect(0, 0, 0, 0);
		const uint32_t v = rdGc(CpMecRs64Cntl);
		wr(IpDiscovery::HwGc, CpMecRs64Cntl, v | kMecPipeResetMask);
		wr(IpDiscovery::HwGc, CpMecRs64Cntl, v & ~kMecPipeResetMask);
	}
	CLOG("gfx: RS64 microengines configured: CP_ME_CNTL 0x%08x CP_MEC_RS64_CNTL 0x%08x",
	     rdGc(CpMeCntl), rdGc(CpMecRs64Cntl));
#endif
}

// sdma_v7_0_gfx_resume_instance for SDMA0 queue 0, without a doorbell: the
// write pointer is posted by register (and mirrored where the MCU polls it).
bool RDNA4Compute::sdmaQueueInit() {
	const uint64_t ring = poolMc(kSdmaRingOffset);
	const uint32_t sizeLog2 = 10;   // 4 KiB = 1024 dwords
	uint32_t rb = rdGc(sdma(0, SdmaQ0RbCntl));
	rb = (rb & ~(kSdmaRbSizeMask | kSdmaRbEnable)) | (sizeLog2 << kSdmaRbSizeShift) | kSdmaRbPriv;
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbCntl), rb);
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbRptr), 0);
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbRptrHi), 0);
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbWptr), 0);
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbWptrHi), 0);

	const uint64_t wpoll = poolMc(kSdmaWptrOffset), rwb = poolMc(kSdmaRptrOffset);
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0WptrPollLo), static_cast<uint32_t>(wpoll));
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0WptrPollHi), static_cast<uint32_t>(wpoll >> 32));
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RptrAddrHi), static_cast<uint32_t>(rwb >> 32));
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RptrAddrLo), static_cast<uint32_t>(rwb) & ~3u);

	rb = (rb | kSdmaRbRptrWriteback | kSdmaRbMcuWptrPoll) & ~kSdmaRbWptrPoll;
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbBase), static_cast<uint32_t>(ring >> 8));
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbBaseHi), static_cast<uint32_t>(ring >> 40));

	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0MinorPtrUpd), 1);
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbWptr), 0);
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbWptrHi), 0);
	// sdma_v7_0 always uses a doorbell (use_doorbell = true): SDMA0 on
	// dword kSdmaDoorbellDword, routed by NBIF S2A entry 2.
	const uint32_t db = rdGc(sdma(0, SdmaQ0Doorbell));
	if (sdmaDoorbell) {
		wr(IpDiscovery::HwGc, sdma(0, SdmaQ0Doorbell), db | kSdmaDoorbellEnable);
		wr(IpDiscovery::HwGc, sdma(0, SdmaQ0DoorbellOffset),
		   (rdGc(sdma(0, SdmaQ0DoorbellOffset)) & ~(0x3ffffffu << 2)) | (kSdmaDoorbellDword << 2));
		wr(IpDiscovery::HwNbif, NbifS2aDoorbell2,
		   (rd(IpDiscovery::HwNbif, NbifS2aDoorbell2) & ~kS2aDoorbell2Mask) | kS2aDoorbell2Sdma);
	} else {
		wr(IpDiscovery::HwGc, sdma(0, SdmaQ0Doorbell), db & ~kSdmaDoorbellEnable);
	}
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0MinorPtrUpd), 0);

	wr(IpDiscovery::HwGc, sdma(0, SdmaWatchdogCntl),
	   (rdGc(sdma(0, SdmaWatchdogCntl)) & ~kSdmaWatchdogHangMask) | 1);
	wr(IpDiscovery::HwGc, sdma(0, SdmaUtcl1Cntl),
	   (rdGc(sdma(0, SdmaUtcl1Cntl)) & ~(kSdmaUtcl1RespMask | kSdmaUtcl1RedoMask)) | (3u << 9) | 9);
	wr(IpDiscovery::HwGc, sdma(0, SdmaUtcl1Page),
	   (rdGc(sdma(0, SdmaUtcl1Page)) & kSdmaUtcl1PagePolicyKeep) | kSdmaUtcl1PagePolicy);
	wr(IpDiscovery::HwGc, sdma(0, SdmaMcuCntl),
	   rdGc(sdma(0, SdmaMcuCntl)) & ~(kSdmaMcuHalt | kSdmaMcuReset));

	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbCntl), rb | kSdmaRbEnable);
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0IbCntl), rdGc(sdma(0, SdmaQ0IbCntl)) | kSdmaIbEnable);
	return true;
}

// sdma_v7_0_ring_set_wptr: the wptr copy, then the 64-bit doorbell (bytes);
// without a doorbell, the RB_WPTR registers (its non-doorbell branch).
void RDNA4Compute::sdmaKick(uint64_t wptrBytes) {
	*poolDw(kSdmaWptrOffset) = static_cast<uint32_t>(wptrBytes);
	*poolDw(kSdmaWptrOffset + 4) = static_cast<uint32_t>(wptrBytes >> 32);
	flushHdp();
	if (sdmaDoorbell && doorbells) {
		{ GcAccess g(*this, 0xdb000000u | kSdmaDoorbellDword); if (g.ok) doorbells[kSdmaDoorbellDword / 2] = wptrBytes; }   // W27
		return;
	}
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbWptr), static_cast<uint32_t>(wptrBytes));
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbWptrHi), static_cast<uint32_t>(wptrBytes >> 32));
}

bool RDNA4Compute::stageSdma() {
	OSDictionary *d = OSDictionary::withCapacity(12);
	auto put = [d](const char *key, uint64_t v) {
		if (OSNumber *n = d ? OSNumber::withNumber(v, 64) : nullptr) {
			d->setObject(key, n);
			n->release();
		}
	};
	auto publish = [this, d]() {
		if (d) {
			env.owner->setProperty("Compute,SDMA", d);
			d->release();
		}
	};
	auto status = [this]() {
		CLOG("sdma: SDMA0 status 0x%08x rb_cntl 0x%08x rptr 0x%x wptr 0x%x mcu 0x%08x, "
		     "rptr writeback 0x%x", rdGc(sdma(0, SdmaStatusReg)), rdGc(sdma(0, SdmaQ0RbCntl)),
		     rdGc(sdma(0, SdmaQ0RbRptr)), rdGc(sdma(0, SdmaQ0RbWptr)),
		     rdGc(sdma(0, SdmaMcuCntl)), *poolDw(kSdmaRptrOffset));
		logGcFault("sdma");
	};

	// 1. GC hub, after the microengines' start addresses (amdgpu's order).
	trail("s4: CP RS64 config");
	cpConfigRs64();
	trail("s4: GC hub init");
	if (!gcHubInit()) {
		publish();
		return false;
	}
	CLOG("sdma: GC hub up: fb 0x%08x..0x%08x sys 0x%08x..0x%08x l1 0x%08x l2 0x%08x ctx0 0x%08x",
	     rdGc(GcFbLocationBase), rdGc(GcFbLocationTop), rdGc(GcSysApertureLow),
	     rdGc(GcSysApertureHigh), rdGc(GcMxL1TlbCntl), rdGc(GcL2Cntl), rdGc(GcCtx0Cntl));

	// 2. SDMA0 queue 0.
	trail("s4: SDMA0 queue init");
	Sdma::Ring &ring = sdmaRing;              // kept: the runtime's DMA uses it
	if (!ring.init(poolDw(kSdmaRingOffset), poolMc(kSdmaRingOffset), kSdmaRingSize)) {
		publish();
		return false;
	}
	*poolDw(kSdmaRptrOffset) = 0;
	*poolDw(kSdmaTestOffset) = 0xCAFEDEAD;
	*poolDw(kSdmaFenceOffset) = 0;
	*poolDw(kSdmaWptrOffset) = 0;           // the MCU polls this from enable on
	*poolDw(kSdmaWptrOffset + 4) = 0;
	flushHdp();
	trail("s4: SDMA MCU unhalt");
	sdmaStartMcus();
	sdmaDoorbell = doorbellMapBar();
	sdmaQueueInit();
	status();

	// 3. First packet: one dword written by the engine (amdgpu's ring test),
	//    kicked through the doorbell as amdgpu does; if it does not land,
	//    the doorbell goes off and the RB_WPTR registers are tried.
	trail("s4: SDMA WRITE_LINEAR test");
	uint32_t pkt[8];
	ring.emit(pkt, Sdma::writeDword(pkt, poolMc(kSdmaTestOffset), 0xDEADBEEF));
	auto landed = [this]() {
		for (uint32_t ms = 0; ms < 200; ms++) {
			if (*poolDw(kSdmaTestOffset) == 0xDEADBEEF)
				return true;
			IOSleep(1);
		}
		return false;
	};
	sdmaKick(ring.wptr());
	bool wrote = landed();
	CLOG("sdma: WRITE_LINEAR through the %s: %s", sdmaDoorbell ? "doorbell" : "RB_WPTR registers",
	     wrote ? "landed" : "did NOT land");
	if (!wrote && sdmaDoorbell) {
		status();
		sdmaDoorbell = false;
		wr(IpDiscovery::HwGc, sdma(0, SdmaQ0Doorbell),
		   rdGc(sdma(0, SdmaQ0Doorbell)) & ~kSdmaDoorbellEnable);
		sdmaKick(ring.wptr());
		wrote = landed();
		CLOG("sdma: WRITE_LINEAR through the RB_WPTR registers: %s", wrote ? "landed" : "did NOT land");
	}
	put("WriteTest", wrote);
	put("Doorbell", sdmaDoorbell);
	if (!wrote) {
		CLOG("sdma: WRITE_LINEAR did not land (test dword 0x%08x)", *poolDw(kSdmaTestOffset));
		status();
		publish();
		return false;
	}
	CLOG("sdma: WRITE_LINEAR landed: the GPU wrote 0xDEADBEEF to VRAM");

	// 4. The first real work: fill 1 MiB, fence, verify from the CPU.
	trail("s4: SDMA CONST_FILL 1 MiB");
	const uint32_t pattern = 0x5A5AC0DE;
	*poolDw(kFillOffset - 4) = 0x11111111;             // guards either side
	*poolDw(kFillOffset + kFillBytes) = 0x22222222;
	*poolDw(kFillOffset) = 0;
	*poolDw(kFillOffset + kFillBytes - 4) = 0;
	flushHdp();
	uint64_t t0 = mach_absolute_time();
	ring.emit(pkt, Sdma::constFill(pkt, poolMc(kFillOffset), pattern, kFillBytes));
	ring.emit(pkt, Sdma::fence(pkt, poolMc(kSdmaFenceOffset), sdmaFence = 1));
	sdmaKick(ring.wptr());
	bool fenced = false;
	for (uint32_t us = 0; us < 500000 && !fenced; us += 10) {
		fenced = *poolDw(kSdmaFenceOffset) == 1;
		if (!fenced)
			IODelay(10);
	}
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	put("FillFenced", fenced);
	if (!fenced) {
		CLOG("sdma: fill fence never came (0x%08x)", *poolDw(kSdmaFenceOffset));
		status();
		publish();
		return false;
	}
	uint32_t bad = 0;
	for (uint32_t off = 0; off < kFillBytes; off += 4096 + 4)   // a spread of dwords
		bad += *poolDw(kFillOffset + (off & ~3u)) != pattern;
	bad += *poolDw(kFillOffset + kFillBytes - 4) != pattern;
	const bool guards = *poolDw(kFillOffset - 4) == 0x11111111 &&
	                    *poolDw(kFillOffset + kFillBytes) == 0x22222222;
	CLOG("sdma: CONST_FILL 1 MiB fenced in %llu us: %s, guards %s", ns / 1000,
	     bad ? "MISMATCHED dwords" : "every sampled dword matches", guards ? "intact" : "OVERWRITTEN");
	put("FillMicros", ns / 1000);
	put("FillBadDwords", bad);
	put("FillGuardsIntact", guards);
	publish();
	return !bad && guards;
}

// ---------------------------------------------------------------------------
// Stage 5: a MEC compute queue
// ---------------------------------------------------------------------------

// soc24_grbm_select: bank the CP_HQD_* / CP_MQD_* / per-pipe registers.
void RDNA4Compute::grbmSelect(uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid) {
	wr(IpDiscovery::HwGc, GrbmGfxCntl,
	   (pipe & 3) | ((me & 3) << 2) | ((vmid & 0xf) << 4) | ((queue & 7) << 8));
}

// gfx_v12_0_config_gfx_rs64 (the MEC half) + gfx_v12_0_cp_compute_enable:
// point every MEC pipe at the firmware's entry, pulse the pipe resets, then
// unhalt the MEC with all four pipes active.
bool RDNA4Compute::mecStart() {
#ifdef RDNA4FB_NO_FIRMWARE
	return false;
#else
	const AmdFw::Blob mec = FW_BLOB(mec);
	if (mec.size < 60)
		return false;
	auto le32 = [&](uint32_t off) {
		return static_cast<uint32_t>(mec.data[off]) | (static_cast<uint32_t>(mec.data[off + 1]) << 8) |
		       (static_cast<uint32_t>(mec.data[off + 2]) << 16) |
		       (static_cast<uint32_t>(mec.data[off + 3]) << 24);
	};
	const uint32_t startLo = le32(52), startHi = le32(56);   // gfx_firmware_header_v2_0
	for (uint32_t pipe = 0; pipe < 4; pipe++) {
		grbmSelect(1, pipe, 0, 0);
		wr(IpDiscovery::HwGc, CpMecPrgrmStart, (startLo >> 2) | (startHi << 30));
		wr(IpDiscovery::HwGc, CpMecPrgrmStartHi, startHi >> 2);
	}
	grbmSelect(0, 0, 0, 0);
	uint32_t v = rdGc(CpMecRs64Cntl);
	wr(IpDiscovery::HwGc, CpMecRs64Cntl, v | kMecPipeResetMask);
	wr(IpDiscovery::HwGc, CpMecRs64Cntl, v & ~kMecPipeResetMask);

	v = rdGc(CpMecRs64Cntl);
	v &= ~(kMecInvalidateIcache | kMecPipeResetMask | kRs64Halt);
	v |= kMecPipeActiveMask;
	wr(IpDiscovery::HwGc, CpMecRs64Cntl, v);
	IODelay(50);
	CLOG("mec: MEC entry 0x%08x%08x, CP_MEC_RS64_CNTL 0x%08x", startHi, startLo,
	     rdGc(CpMecRs64Cntl));
	return true;
#endif
}

// soc24_common_hw_init's doorbell aperture + nbif_v6_3_1_gc_doorbell_init +
// gfx_v12_0_cp_set_doorbell_range (MEC part), then map the doorbell BAR.
bool RDNA4Compute::doorbellInit() {
	CLOG("mec: doorbells before: aperture 0x%08x, S2A entry0 0x%08x entry3 0x%08x",
	     rd(IpDiscovery::HwNbif, NbifDoorbellAperEn), rd(IpDiscovery::HwNbif, NbifS2aDoorbell0),
	     rd(IpDiscovery::HwNbif, NbifS2aDoorbell3));
	wr(IpDiscovery::HwNbif, NbifS2aDoorbell0, kS2aDoorbell0Gc);
	wr(IpDiscovery::HwNbif, NbifS2aDoorbell3, kS2aDoorbell3Gc);
	wr(IpDiscovery::HwGc, CpMecDoorbellLower, kMecDoorbellLowerBytes);
	wr(IpDiscovery::HwGc, CpMecDoorbellUpper, kMecDoorbellUpperBytes);
	return doorbellMapBar();
}

// The doorbell aperture on (soc24_common_hw_init) and BAR2 mapped; stage 4
// needs it first, for SDMA's doorbell.
bool RDNA4Compute::doorbellMapBar() {
	wr(IpDiscovery::HwNbif, NbifDoorbellAperEn, rd(IpDiscovery::HwNbif, NbifDoorbellAperEn) | 1);
	if (!doorbells) {
		IODeviceMemory *bar2 = env.pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
		if (!bar2 || bar2->getLength() < 0x1000)
			return false;
		doorbellMap = bar2->map(kIOMapInhibitCache);
		if (!doorbellMap)
			return false;
		doorbells = reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress());
	}
	return doorbells != nullptr;
}

// gfx_v12_0_compute_mqd_init + gfx_v12_0_kiq_init_register for ME1 pipe 0
// queue 0, VMID0, doorbell dword kComputeDoorbellDword.
bool RDNA4Compute::hqdInit(bool asKiq) {
	return hqdInitFor(asKiq, 0, 0, 0, poolMc(kMqdOffset), poolMc(kEopOffset) >> 8,
	                  poolMc(kPqOffset) >> 8, poolMc(kPqRptrOffset), poolMc(kPqWptrOffset),
	                  kComputeDoorbellDword);
}

bool RDNA4Compute::hqdInitFor(bool asKiq, uint32_t pipe, uint32_t queue, uint32_t vmid,
	                            uint64_t mqd, uint64_t eop, uint64_t pq, uint64_t rptr,
	                            uint64_t wpoll, uint32_t doorbell) {
	const uint32_t mqdOff = static_cast<uint32_t>(mqd - pool.mcAddress);
	const bool rptrCpu = rptr >= pool.mcAddress && rptr < pool.mcAddress + pool.size;
	const uint32_t rptrOff = rptrCpu ? static_cast<uint32_t>(rptr - pool.mcAddress) : 0;
	for (uint32_t off = mqdOff; off < mqdOff + 0x2000; off += 4)   // MQD + EOP
		*poolDw(off) = 0;
	if (rptrCpu) {
		*poolDw(rptrOff) = 0;
		*poolDw(rptrOff + 0x40) = 0;
		*poolDw(rptrOff + 0x44) = 0;
	}
	flushHdp();

	const uint32_t eopSize = 8;                 // 2^(8+1) dwords = GFX12_MEC_HPD_SIZE
	const uint32_t queueSize = 9;               // 2^(9+1) dwords = 4 KiB
	uint32_t pqControl = (kHqdPqControlDefault & ~(kPqQueueSizeMask | kPqRptrBlockMask |
	                                               kPqTunnelDispatch)) |
	                     queueSize | (9u << 8) | kPqUnordDispatch | kPqPrivState | kPqKmdQueue;

	// The MQD in memory, as gfx_v12_0_compute_mqd_init fills it (v12_compute_mqd
	// dword offsets): the CP reads it back (CU masks, save/restore), so it
	// must describe the same queue the registers below do.
	auto mqdDw = [this, mqdOff](uint32_t dw, uint32_t v) { *poolDw(mqdOff + 4 * dw) = v; };
	mqdDw(0, 0xC0310800);                             // header
	mqdDw(11, 1);                                     // compute_pipelinestat_enable
	mqdDw(23, 0xffffffff);                            // compute_static_thread_mgmt_se0
	mqdDw(24, 0xffffffff);                            // ..._se1
	mqdDw(26, 0xffffffff);                            // ..._se2
	mqdDw(27, 0xffffffff);                            // ..._se3
	mqdDw(32, 7);                                     // compute_misc_reserved
	mqdDw(128, static_cast<uint32_t>(mqd) & ~3u);     // cp_mqd_base_addr_lo
	mqdDw(129, static_cast<uint32_t>(mqd >> 32));
	mqdDw(130, 1);                                    // cp_hqd_active
	mqdDw(131, vmid);                                 // cp_hqd_vmid
	mqdDw(132, kHqdPersistentDefault);                // cp_hqd_persistent_state
	mqdDw(135, (1u << 0) | (1u << 4) | (1u << 8));    // cp_hqd_quantum: EN, SCALE 1, DURATION 1
	mqdDw(136, static_cast<uint32_t>(pq));            // cp_hqd_pq_base_lo
	mqdDw(137, static_cast<uint32_t>(pq >> 32));
	mqdDw(139, static_cast<uint32_t>(rptr) & ~3u);    // cp_hqd_pq_rptr_report_addr_lo
	mqdDw(140, static_cast<uint32_t>(rptr >> 32) & 0xffff);
	mqdDw(141, static_cast<uint32_t>(wpoll) & ~3u);   // cp_hqd_pq_wptr_poll_addr_lo
	mqdDw(142, static_cast<uint32_t>(wpoll >> 32) & 0xffff);
	mqdDw(143, (doorbell << kDoorbellOffsetShift) | kDoorbellEn); // cp_hqd_pq_doorbell_control
	mqdDw(145, pqControl);                            // cp_hqd_pq_control
	mqdDw(149, 0x00300000);                           // cp_hqd_ib_control: MIN_IB_AVAIL_SIZE 3
	mqdDw(162, kMqdControlDefault & ~0xfu);           // cp_mqd_control: VMID 0, always
	mqdDw(165, static_cast<uint32_t>(eop));           // cp_hqd_eop_base_addr_lo
	mqdDw(166, static_cast<uint32_t>(eop >> 32));
	mqdDw(167, (kHqdEopControlDefault & ~0x3fu) | eopSize);
	flushHdp();

	// As the KIQ (gfx_v12_0_kiq_setting): the RLC names this queue (ME1,
	// pipe 0, queue 0) in the low byte, bit 7 marks it valid. Otherwise
	// it stays an ordinary MMIO-activated queue (kfd's hqd_load, gfx9-11).
	const uint32_t sched = (rdGc(RlcCpSchedulers) & 0xffffff00u) | (1u << 5) | (pipe << 3) | queue;
	if (vmid == 0 && pipe == 0 && queue == 0 && asKiq) {
		wr(IpDiscovery::HwGc, RlcCpSchedulers, sched);
		wr(IpDiscovery::HwGc, RlcCpSchedulers, sched | 0x80);
	} else if (vmid == 0 && pipe == 0 && queue == 0) {
		wr(IpDiscovery::HwGc, RlcCpSchedulers, sched & 0xffffff00u);
	}

	grbmSelect(1, pipe, queue, vmid);
	wr(IpDiscovery::HwGc, CpPqWptrPollCntl, rdGc(CpPqWptrPollCntl) & ~kPqWptrPollEn);
	wr(IpDiscovery::HwGc, CpHqdEopBase, static_cast<uint32_t>(eop));
	wr(IpDiscovery::HwGc, CpHqdEopBaseHi, static_cast<uint32_t>(eop >> 32));
	wr(IpDiscovery::HwGc, CpHqdEopControl, (kHqdEopControlDefault & ~0x3fu) | eopSize);
	wr(IpDiscovery::HwGc, CpHqdPqDoorbell, (doorbell << kDoorbellOffsetShift) | kDoorbellEn);
	if (rdGc(CpHqdActive) & 1) {                // a queue left behind: drain it
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
		for (uint32_t us = 0; us < 100000 && (rdGc(CpHqdActive) & 1); us += 10)
			IODelay(10);
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
		wr(IpDiscovery::HwGc, CpHqdPqRptr, 0);
		wr(IpDiscovery::HwGc, CpHqdPqWptrLo, 0);
		wr(IpDiscovery::HwGc, CpHqdPqWptrHi, 0);
	}
	/* A dequeued VM queue retains its read pointer; every fresh HQD starts
	 * consuming the newly initialized ring at dword zero. */
	wr(IpDiscovery::HwGc, CpHqdPqRptr, 0);
	wr(IpDiscovery::HwGc, CpMqdBaseAddr, static_cast<uint32_t>(mqd) & ~3u);
	wr(IpDiscovery::HwGc, CpMqdBaseAddrHi, static_cast<uint32_t>(mqd >> 32));
	/* The MQD is a kernel (VMID0 MC) address for every queue, whatever the
	 * queue's own VMID: gfx_v12_0_compute_mqd_init ("set MQD vmid to 0") and
	 * kfd_mqd_manager_v12 init_mqd (cp_mqd_control = PRIV_STATE only) keep
	 * CP_MQD_CONTROL.VMID at 0. With the queue's VMID here the CP's MQD
	 * accesses walked the client tables at an MC address and faulted (W16). */
	wr(IpDiscovery::HwGc, CpMqdControl, kMqdControlDefault & ~0xfu);
	wr(IpDiscovery::HwGc, CpHqdPqBase, static_cast<uint32_t>(pq));
	wr(IpDiscovery::HwGc, CpHqdPqBaseHi, static_cast<uint32_t>(pq >> 32));
	wr(IpDiscovery::HwGc, CpHqdPqControl, pqControl);
	wr(IpDiscovery::HwGc, CpHqdPqRptrReport, static_cast<uint32_t>(rptr) & ~3u);
	wr(IpDiscovery::HwGc, CpHqdPqRptrReportHi, static_cast<uint32_t>(rptr >> 32) & 0xffff);
	wr(IpDiscovery::HwGc, CpHqdPqWptrPoll, static_cast<uint32_t>(wpoll) & ~3u);
	wr(IpDiscovery::HwGc, CpHqdPqWptrPollHi, static_cast<uint32_t>(wpoll >> 32) & 0xffff);
	wr(IpDiscovery::HwGc, CpMecDoorbellLower, kMecDoorbellLowerBytes);
	wr(IpDiscovery::HwGc, CpMecDoorbellUpper, kMecDoorbellUpperBytes);
	wr(IpDiscovery::HwGc, CpHqdPqDoorbell, (doorbell << kDoorbellOffsetShift) | kDoorbellEn);
	wr(IpDiscovery::HwGc, CpHqdPqWptrLo, 0);
	wr(IpDiscovery::HwGc, CpHqdPqWptrHi, 0);
	wr(IpDiscovery::HwGc, CpHqdVmid, vmid);
	wr(IpDiscovery::HwGc, CpHqdPersistent, kHqdPersistentDefault);
	wr(IpDiscovery::HwGc, CpHqdEopRptr, kEopInitFetcher);   // start the EOP fetcher (kfd hqd_load)
	wr(IpDiscovery::HwGc, CpHqdActive, 1);
	wr(IpDiscovery::HwGc, CpPqStatus, rdGc(CpPqStatus) | kPqStatusDoorbellEnable);
	const uint32_t active = rdGc(CpHqdActive), pqc = rdGc(CpHqdPqControl);
	grbmSelect(0, 0, 0, 0);
	CLOG("mec: HQD ME1/pipe%u/queue%u VMID%u: active %u, PQ_CONTROL 0x%08x, doorbell dword %u",
	     pipe, queue, vmid, active & 1, pqc, doorbell);
	return active & 1;
}

// gfx_v12_0_ring_set_wptr_compute: the wptr copy, then the 64-bit doorbell.
void RDNA4Compute::pm4Kick(uint64_t wptrDwords) {
	*poolDw(kPqWptrOffset) = static_cast<uint32_t>(wptrDwords);
	*poolDw(kPqWptrOffset + 4) = static_cast<uint32_t>(wptrDwords >> 32);
	flushHdp();
	{ GcAccess g(*this, 0xdb000000u | kComputeDoorbellDword); if (g.ok) doorbells[kComputeDoorbellDword / 2] = wptrDwords; }   // W27
}

void RDNA4Compute::logComputeQueueState(const char *tag, uint32_t pipe, uint32_t queue,
                                        uint32_t vmid) {
	grbmSelect(1, pipe, queue, vmid);
	CLOG("%s: GRBM 0x%08x GRBM2 0x%08x CP_STAT 0x%08x CPC 0x%08x CPC_BUSY 0x%08x",
	     tag, rdGc(GrbmStatus), rdGc(GrbmStatus2), rdGc(CpStat), rdGc(CpCpcStatus),
	     rdGc(CpCpcBusyStat));
	CLOG("%s: HQD active 0x%08x vmid 0x%08x persistent 0x%08x PQ base 0x%08x:%08x rptr 0x%08x "
	     "wptr 0x%08x:%08x doorbell 0x%08x PQ_CONTROL 0x%08x dequeue 0x%08x",
	     tag, rdGc(CpHqdActive), rdGc(CpHqdVmid), rdGc(CpHqdPersistent), rdGc(CpHqdPqBaseHi),
	     rdGc(CpHqdPqBase), rdGc(CpHqdPqRptr), rdGc(CpHqdPqWptrHi), rdGc(CpHqdPqWptrLo),
	     rdGc(CpHqdPqDoorbell), rdGc(CpHqdPqControl), rdGc(CpHqdDequeueReq));
	CLOG("%s: HQ_STATUS0 0x%08x EOP 0x%08x:%08x EOP_RPTR 0x%08x SQ_CMD 0x%08x MEC pc 0x%x",
	     tag, rdGc(CpHqdHqStatus0), rdGc(CpHqdEopBaseHi), rdGc(CpHqdEopBase), rdGc(CpHqdEopRptr),
	     rdGc(SqCmd), rdGc(CpMecRs64InstrPntr));
	grbmSelect(0, 0, 0, 0);
	logGcFault(tag);
}

bool RDNA4Compute::queueWriteTest(const char *tag, const Launch *l) {
	const bool client = l && l->queue;
	Pm4::Queue *queue = client ? l->queue : &pm4Queue;
	const uint32_t doorbell = client ? l->doorbell : kComputeDoorbellDword;
	const uint64_t address = client ? l->recoveryProofAddress : poolMc(kPm4TestOffset);
	volatile uint32_t *cpu = client ? l->recoveryProofCpu : poolDw(kPm4TestOffset);
	if (!queue || !cpu || !address)
		return false;
	*cpu = 0;
	flushHdp();
	uint32_t pkt[8];
	if (!queue->emit(pkt, Pm4::writeData(pkt, address, 0x600DF00D)))
		return false;
	if (client)
		pm4Kick(*queue, doorbell, queue->wptr());
	else
		pm4Kick(queue->wptr());
	for (uint32_t ms = 0; ms < 200; ms++) {
		if (*cpu == 0x600DF00D) {
			CLOG("%s: recovered queue WRITE_DATA landed", tag);
			return true;
		}
		IOSleep(1);
	}
	CLOG("%s: recovered queue WRITE_DATA did NOT land", tag);
	return false;
}

bool RDNA4Compute::recoverComputeQueue(const char *tag, const Launch *l) {
	if (!hangRecoveryEnabled) {
		CLOG("%s: queue recovery disabled; rdna4-hang=1 is required", tag);
		return false;
	}
	const bool client = l && l->queue;
	const uint32_t pipe = client ? l->pipe : 0;
	const uint32_t queueId = client ? l->queueId : 0;
	const uint32_t vmid = client ? l->vmid : 0;
	CLOG("%s: recovering compute queue without a GPU reset", tag);
	// gfx_v12_0_reset_kcq is MES-backed upstream. This is our no-MES sequence:
	// the documented HQD RESET_WAVES dequeue, gfx12 SQ_CMD wave kill, then the
	// same queue's HQD register set and a fenced WRITE_DATA proof.
	logComputeQueueState(tag, pipe, queueId, vmid);
	grbmSelect(1, pipe, queueId, vmid);
	wr(IpDiscovery::HwGc, CpHqdDequeueReq, 2); // RESET_WAVES (amdgpu enum value)
	// amdgpu_amdkfd_gfx_v12 exposes SQ_CMD wave_control_execute. CMD=3 is
	// kill, MODE=1 selects all waves, CHECK_VMID=1 limits it to this VMID.
	const uint32_t sqCmd = 3u | (1u << 4) | (1u << 7) | ((vmid & 0xf) << 28);
	wr(IpDiscovery::HwGc, SqCmd, sqCmd);
	bool inactive = false;
	for (uint32_t us = 0; us < 100000; us += 10) {
		if (!(rdGc(CpHqdActive) & 1)) {
			inactive = true;
			break;
		}
		IODelay(10);
	}
	wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
	if (!inactive) {
		grbmSelect(0, 0, 0, 0);
		CLOG("%s: HQD dequeue/wave kill did not clear CP_HQD_ACTIVE", tag);
		logGcFault(tag);
		return false;
	}
	if (client) {
		if (!l->queueCpu || !l->queueAddress ||
		    !l->queue->init(l->queueCpu, l->queueAddress, kPqSize) ||
		    !hqdInitFor(false, pipe, queueId, vmid, l->recoveryMqd, l->recoveryEop,
	                   l->queueAddress >> 8, l->recoveryRptr, l->recoveryWpoll, l->doorbell) ||
		    !queueWriteTest(tag, l)) {
			CLOG("%s: client queue reinitialisation or fenced WRITE_DATA failed; staying wedged", tag);
			logComputeQueueState(tag, pipe, queueId, vmid);
			return false;
		}
	} else if (!pm4Queue.init(poolDw(kPqOffset), poolMc(kPqOffset), kPqSize) ||
	           !hqdInit(hqdMode == 2) || !queueWriteTest(tag)) {
		CLOG("%s: queue reinitialisation or fenced WRITE_DATA failed; staying wedged", tag);
		logComputeQueueState(tag);
		return false;
	}
	CLOG("%s: queue recovered; subsequent clients may dispatch", tag);
	return true;
}

void RDNA4Compute::pm4Kick(Pm4::Queue &queue, uint32_t doorbell, uint64_t wptrDwords) {
	/* The VM queue's wptr report is mapped in its client VA; the CP owns it. */
	flushHdp();
	{ GcAccess g(*this, 0xdb000000u | doorbell); if (g.ok) doorbells[doorbell / 2] = wptrDwords; }   // W27
}

bool RDNA4Compute::stageCompute() {
	OSDictionary *d = OSDictionary::withCapacity(8);
	auto put = [d](const char *key, uint64_t v) {
		if (OSNumber *n = d ? OSNumber::withNumber(v, 64) : nullptr) {
			d->setObject(key, n);
			n->release();
		}
	};
	auto publish = [this, d]() {
		if (d) {
			env.owner->setProperty("Compute,MEC", d);
			d->release();
		}
	};
	auto status = [this]() {
		grbmSelect(1, 0, 0, 0);
		const uint32_t rptr = rdGc(CpHqdPqRptr), active = rdGc(CpHqdActive);
		const uint32_t hq = rdGc(CpHqdHqStatus0), pq = rdGc(CpPqStatus);
		grbmSelect(0, 0, 0, 0);
		CLOG("mec: GRBM 0x%08x CP_STAT 0x%08x CPC 0x%08x CPC_BUSY 0x%08x MEC 0x%08x pc 0x%x",
		     rdGc(GrbmStatus), rdGc(CpStat), rdGc(CpCpcStatus), rdGc(CpCpcBusyStat),
		     rdGc(CpMecRs64Cntl), rdGc(CpMecRs64InstrPntr));
		CLOG("mec: HQD active %u rptr 0x%x rptr report 0x%x HQ_STATUS0 0x%08x PQ_STATUS 0x%08x",
		     active & 1, rptr, *poolDw(kPqRptrOffset), hq, pq);
		logGcFault("mec");
	};

	trail("s5: MEC start");
	wr(IpDiscovery::HwGc, GrbmCntl, (rdGc(GrbmCntl) & ~0xfffu) | 0xff);   // READ_TIMEOUT [11:0]
	if (!mecStart()) {
		publish();
		return false;
	}
	trail("s5: doorbells");
	if (!doorbellInit()) {
		CLOG("mec: doorbell BAR unavailable");
		publish();
		return false;
	}

	// Upstream has no MMIO-queue path on gfx12 (MES maps every queue), so
	// the card decides: first a plain HQD, as kfd's hqd_load programs one on
	// gfx9-11, then the same queue as the KIQ. Each is proven with a lone
	// WRITE_DATA — the one packet amdgpu also uses on a KIQ.
	Pm4::Queue &q = pm4Queue;
	auto writeTest = [&](const char *mode) -> bool {
		*poolDw(kPm4TestOffset) = 0;
		flushHdp();
		uint32_t pkt[8];
		q.emit(pkt, Pm4::writeData(pkt, poolMc(kPm4TestOffset), 0x600DF00D));
		pm4Kick(q.wptr());
		bool ok = false;
		for (uint32_t ms = 0; ms < 200 && !ok; ms++) {
			ok = *poolDw(kPm4TestOffset) == 0x600DF00D;
			if (!ok)
				IOSleep(1);
		}
		CLOG("mec: %s queue: WRITE_DATA %s", mode, ok ? "landed" : "did NOT land");
		if (!ok)
			status();
		return ok;
	};
	trail("s5: HQD init (plain queue)");
	uint32_t mode = 1;
	bool running = q.init(poolDw(kPqOffset), poolMc(kPqOffset), kPqSize) && hqdInit(false) &&
	               writeTest("plain");
	if (!running) {
		trail("s5: HQD init (as KIQ)");
		mode = 2;
		running = q.init(poolDw(kPqOffset), poolMc(kPqOffset), kPqSize) && hqdInit(true) &&
		          writeTest("KIQ");
	}
	put("QueueMode", running ? mode : 0);
	if (running) {
		hqdMode = mode;
		bootQueueLive = true;
	}
	if (!running) {
		publish();
		return false;
	}

	// amdgpu's compute ring test (SCRATCH_REG0 through SET_UCONFIG_REG),
	// a WRITE_DATA to VRAM, and the end-of-pipe fence it uses for jobs.
	trail("s5: PM4 ring test");
	uint32_t scratchByte = 0;
	if (!env.disc->regByteOffset(IpDiscovery::HwGc, 0, ScratchReg0.seg, ScratchReg0.dword,
	                             scratchByte)) {
		publish();
		return false;
	}
	const uint32_t scratchAbs = scratchByte / 4;   // SET_UCONFIG_REG takes the dword address
	wr(IpDiscovery::HwGc, ScratchReg0, 0xCAFEDEAD);
	*poolDw(kPm4TestOffset) = 0;
	*poolDw(kPm4FenceOffset) = 0;
	flushHdp();
	uint32_t pkt[8];
	q.emit(pkt, Pm4::setUconfigReg(pkt, scratchAbs, 0xDEADBEEF));
	q.emit(pkt, Pm4::writeData(pkt, poolMc(kPm4TestOffset), 0x600DF00D));
	q.emit(pkt, Pm4::releaseMem(pkt, poolMc(kPm4FenceOffset), ++pm4Fence));
	pm4Kick(q.wptr());

	bool done = false;
	for (uint32_t us = 0; us < 500000 && !done; us += 10) {
		done = *poolDw(kPm4FenceOffset) == pm4Fence;
		if (!done)
			IODelay(10);
	}
	const uint32_t scratch = rdGc(ScratchReg0), data = *poolDw(kPm4TestOffset);
	put("ScratchReg", scratch);
	put("WriteData", data);
	put("Fence", *poolDw(kPm4FenceOffset));
	CLOG("mec: PM4 on the MEC: SCRATCH_REG0 0x%08x (%s), WRITE_DATA 0x%08x (%s), "
	     "RELEASE_MEM fence %s", scratch, scratch == 0xDEADBEEF ? "ok" : "not written", data,
	     data == 0x600DF00D ? "ok" : "not written", done ? "signalled" : "NOT signalled");
	if (!done || scratch != 0xDEADBEEF || data != 0x600DF00D) {
		status();
		publish();
		return false;
	}
	publish();
	return true;
}

// ---------------------------------------------------------------------------
// Stage 6: a kernel on the compute units
// ---------------------------------------------------------------------------

uint32_t RDNA4Compute::shAbs(const Reg &r) const {
	uint32_t byte = 0;
	return env.disc->regByteOffset(IpDiscovery::HwGc, 0, r.seg, r.dword, byte) ? byte / 4 : 0;
}

// The shape of amdgpu's run_shader (gfx_v9_4_2): SET_SH_REG the program,
// its resources and user data, DISPATCH_DIRECT; with gfx_v12_0's
// ACQUIRE_MEM first (code and inputs were just written by the CPU) and its
// RELEASE_MEM fence after (GL2 written back, so the CPU reads the results).
bool RDNA4Compute::launch(const Launch &l, const char *tag, uint64_t &ns) {
	const bool vm = l.queue != nullptr;
	volatile uint32_t *fenceCpu = vm ? l.fenceCpu : poolDw(kPm4FenceOffset);
	const uint32_t fenceValue = vm ? l.fenceValue : (pm4Fence + 1);
	if (!vm || !l.preserveFence) {
		*fenceCpu = 0;
		flushHdp();
	}

	// Shader memory model for the selected VMID (gfx_v12_0_constants_init).
	grbmSelect(0, vm ? l.pipe : 0, vm ? l.queueId : 0, vm ? l.vmid : 0);
	wr(IpDiscovery::HwGc, ShMemConfig, kShMemConfigDefault);
	if (vm)
		wr(IpDiscovery::HwGc, ShMemBases, kShMemBasesDefault);

	const uint32_t pgm[2] = { static_cast<uint32_t>(l.code >> 8), static_cast<uint32_t>(l.code >> 40) };
	const uint32_t rsrc[2] = { l.rsrc1, (l.rsrc2 & ~kRsrc2LdsMask) | ldsSizeField(l.ldsBytes) };
	// Navi 48 has 4 shader engines: every CU of SE0-3, none of the absent
	// SE4-7 (Mesa's gfx12_init_compute_preamble_state).
	const uint32_t zero = 0, all[2] = { 0xffffffff, 0xffffffff };
	const uint32_t none4[4] = { 0, 0, 0, 0 };
	const uint32_t start[3] = { 0, 0, 0 };

	Pm4::Queue &q = vm ? *l.queue : pm4Queue;
	uint32_t pkt[24];
	q.emit(pkt, Pm4::acquireMem(pkt, Pm4::kGcrMemSync));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputePgmLo), pgm, 2));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputePgmRsrc1), rsrc, 2));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputePgmRsrc3), &l.rsrc3, 1));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeResourceLim), &zero, 1));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeTmpringSize), &zero, 1));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeThreadMgmtSe0), all, 2));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeThreadMgmtSe2), all, 2));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeThreadMgmtSe4), none4, 4));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeStartX), start, 3));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeNumThreadX), l.groupSize, 3));
	if (l.userCount)
		q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeUserData0), l.user, l.userCount));
	q.emit(pkt, Pm4::dispatchDirect(pkt, l.groups[0], l.groups[1], l.groups[2],
	                                Pm4::kDispatchShaderEn | Pm4::kDispatchForceStart0 |
	                                (l.wave32 ? Pm4::kDispatchWave32 : 0)));
	const uint64_t fenceAddress = vm ? l.fenceAddress : poolMc(kPm4FenceOffset);
	const bool irq = l.useInterrupt && ihActive;
	q.emit(pkt, Pm4::releaseMem(pkt, fenceAddress, fenceValue, irq));

	// The client's own queue (W2) or the kernel's; the end-of-pipe interrupt
	// (W1) only wakes the wait, the fence decides. Without it: spin for the
	// first 2 ms (short kernels), then sleep between polls, since user
	// dispatches may run for seconds.
	const uint32_t timeoutUs = l.timeoutUs ? l.timeoutUs : 1000000;
	uint64_t t0 = mach_absolute_time(), span = 0;
	nanoseconds_to_absolutetime(static_cast<uint64_t>(timeoutUs) * 1000, &span);
	if (vm)
		pm4Kick(q, l.doorbell, q.wptr());
	else {
		pm4Fence = fenceValue;
		pm4Kick(q.wptr());
	}
	bool done = false;
	if (irq) {
		done = ihWaitFence(fenceCpu, fenceValue, (timeoutUs + 999) / 1000, true, nullptr, ns);
		// Time the whole dispatch, from before the kick (the emulator even
		// runs it inside the kick), like the polling path below.
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	} else {
		for (uint32_t polls = 0;; polls++) {
			done = *fenceCpu == fenceValue;
			if (done || mach_absolute_time() - t0 > span)
				break;
			if (polls < 200)
				IODelay(10);
			else
				IOSleep(1);
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	}
	if (!done) {
		CLOG("%s: the kernel's fence never came (0x%08x, want 0x%08x)", tag,
		     *fenceCpu, fenceValue);
		grbmSelect(1, vm ? l.pipe : 0, vm ? l.queueId : 0, vm ? l.vmid : 0);
		CLOG("%s: GRBM 0x%08x CP_STAT 0x%08x CPC 0x%08x CPC_BUSY 0x%08x HQD rptr 0x%x wptr %llu "
		     "HQ_STATUS0 0x%08x MEC pc 0x%x", tag, rdGc(GrbmStatus), rdGc(CpStat), rdGc(CpCpcStatus),
		     rdGc(CpCpcBusyStat), rdGc(CpHqdPqRptr), static_cast<unsigned long long>(q.wptr()),
		     rdGc(CpHqdHqStatus0), rdGc(CpMecRs64InstrPntr));
		grbmSelect(0, 0, 0, 0);
		logGcFault(tag);
		if (!vm)
			logComputeQueueState(tag);
	}
	if (vm) {
		const uint32_t status = rdGc(GcL2FaultStatusLo);
		if (status) {
			CLOG("vmid %u: %s: GC hub fault status 0x%08x (fault VMID %u) VA 0x%llx", l.vmid, tag,
			     status, (status >> 20) & 0xf, gcFaultVa());
			gcFaultClear();
			vmInvalidate(l.vmid, "dispatch fault clear");
			scrubFaultPage();
		}
	}
	return done;
}

bool RDNA4Compute::kernelFits(const CodeObj::Kernel &k, const char **why) {
	constexpr uint16_t kSgprRequests = 0x7f;   // kernel_code_properties [6:0]
	const char *w = nullptr;
	if (k.properties & kSgprRequests & ~(1u << 3))
		w = "it asks for dispatch/queue pointers, a dispatch id or scratch setup";
	else if (k.userSgprCount() != (k.wantsKernargPtr() ? 2u : 0u))
		w = "its user SGPR count does not match its requests";
	else if (k.privateSegmentSize)
		w = "it needs scratch memory";
	else if (k.groupSegmentSize > kMaxLdsPerGroup)
		w = "it needs more than 64 KiB of LDS";
	else if (k.kernargSize > kKernargMax)
		w = "its kernel arguments exceed 4 KiB";
	if (why)
		*why = w;
	return !w;
}

// shaders/probe.s: VMID0, wave32, 4 groups of 64 work-items, each writing
// 0x5EED0000 + 3 * its global id.
bool RDNA4Compute::stageDispatch() {
	OSDictionary *d = OSDictionary::withCapacity(8);
	auto put = [d](const char *key, uint64_t v) {
		if (OSNumber *n = d ? OSNumber::withNumber(v, 64) : nullptr) {
			d->setObject(key, n);
			n->release();
		}
	};
	auto publish = [this, d]() {
		if (d) {
			env.owner->setProperty("Compute,Dispatch", d);
			d->release();
		}
	};
	const uint32_t items = kDispatchGroups * kGroupSize;

	// Code and a result buffer with a sentinel past its end.
	trail("s6: kernel upload");
	const uint32_t codeDw = sizeof(kProbeKernel) / 4;
	for (uint32_t i = 0; i < codeDw; i++)
		*poolDw(kShaderOffset + 4 * i) = kProbeKernel[i];
	for (uint32_t i = 0; i <= items; i++)
		*poolDw(kDispatchBuffer + 4 * i) = 0xFFFFFFFF;

	const uint64_t buf = poolMc(kDispatchBuffer);
	const uint32_t user[2] = { static_cast<uint32_t>(buf), static_cast<uint32_t>(buf >> 32) };
	Launch l {};
	l.code = poolMc(kShaderOffset);
	// 4 VGPRs (v0-v3): one block of 8 in wave32.
	l.rsrc1 = 0 | kRsrc1FloatDenorm | kRsrc1MemOrdered;
	l.rsrc2 = (2u << kRsrc2UserSgprShift) | kRsrc2TgidXEn;
	l.user = user;
	l.userCount = 2;
	l.groups[0] = kDispatchGroups;
	l.groups[1] = l.groups[2] = 1;
	l.groupSize[0] = kGroupSize;
	l.groupSize[1] = l.groupSize[2] = 1;
	l.wave32 = true;

	trail("s6: DISPATCH_DIRECT");
	uint64_t ns = 0;
	const bool done = launch(l, "dispatch", ns);
	put("Fenced", done);
	if (!done) {
		CLOG("dispatch: buffer[0] 0x%08x", *poolDw(kDispatchBuffer));
		publish();
		return false;
	}

	uint32_t bad = 0, firstBad = items;
	for (uint32_t i = 0; i < items; i++) {
		if (*poolDw(kDispatchBuffer + 4 * i) != 0x5EED0000u + 3 * i) {
			if (!bad)
				firstBad = i;
			bad++;
		}
	}
	const bool sentinel = *poolDw(kDispatchBuffer + 4 * items) == 0xFFFFFFFF;
	put("Items", items);
	put("WrongItems", bad);
	put("SentinelIntact", sentinel);
	put("Micros", ns / 1000);
	if (bad)
		CLOG("dispatch: %u of %u results wrong, first at %u: 0x%08x (want 0x%08x)", bad, items,
		     firstBad, *poolDw(kDispatchBuffer + 4 * firstBad), 0x5EED0000u + 3 * firstBad);
	else
		CLOG("dispatch: kernel ran on the compute units: all %u work-items wrote their result "
		     "(buffer[255] = 0x%08x) in %llu us, sentinel %s", items,
		     *poolDw(kDispatchBuffer + 4 * (items - 1)), ns / 1000, sentinel ? "intact" : "OVERWRITTEN");
	publish();
	return !bad && sentinel;
}

// ---------------------------------------------------------------------------
// Stage 7: a clang-built kernel from its code object
// ---------------------------------------------------------------------------

// shaders/vadd.cl as clang and ld.lld built it: the loader finds the kernel
// and its descriptor in the ELF, and the launch is what the descriptor asks
// for — RSRC1/2/3 as compiled, the kernarg pointer in the first user SGPRs.
// c[i] = a[i] + 3 * b[i] over kVaddItems work-items.
bool RDNA4Compute::stageKernel() {
	OSDictionary *d = OSDictionary::withCapacity(10);
	auto put = [d](const char *key, uint64_t v) {
		if (OSNumber *n = d ? OSNumber::withNumber(v, 64) : nullptr) {
			d->setObject(key, n);
			n->release();
		}
	};
	auto publish = [this, d]() {
		if (d) {
			env.owner->setProperty("Compute,Kernel", d);
			d->release();
		}
	};

	trail("s7: code object");
	CodeObj::Kernel k {};
	const char *why = nullptr;
	if (!CodeObj::findKernel(kVaddCodeObject, sizeof(kVaddCodeObject), "vadd", k, &why)) {
		CLOG("kernel: vadd not found in its code object (%s)", why ? why : "?");
		publish();
		return false;
	}
	CLOG("kernel: vadd: %u bytes of code, %u of kernargs, RSRC1 0x%08x RSRC2 0x%08x RSRC3 0x%08x, "
	     "%u user SGPRs, wave%u", k.codeSize, k.kernargSize, k.rsrc1, k.rsrc2, k.rsrc3,
	     k.userSgprCount(), k.wave32() ? 32 : 64);
	put("CodeBytes", k.codeSize);
	put("KernargBytes", k.kernargSize);
	put("Rsrc1", k.rsrc1);
	put("Rsrc2", k.rsrc2);

	why = nullptr;
	if (!kernelFits(k, &why) || k.codeSize > kCodeObjCodeMax) {
		CLOG("kernel: vadd cannot be launched here: %s", why ? why : "code too large");
		publish();
		return false;
	}

	// Code, with a zeroed tail for the instruction prefetch.
	trail("s7: upload");
	const uint8_t *src = kVaddCodeObject + k.codeOffset;
	for (uint32_t off = 0; off < k.codeSize + 0x100; off += 4) {
		uint32_t w = 0;
		for (uint32_t b = 0; b < 4; b++)
			if (off + b < k.codeSize)
				w |= static_cast<uint32_t>(src[off + b]) << (8 * b);
		*poolDw(kCodeObjCode + off) = w;
	}

	// Inputs, a result buffer with a sentinel past its end, and the kernel
	// arguments: three global pointers at 0/8/16, as vadd's signature lays
	// them out; anything past them (hidden arguments) zero.
	auto aOf = [](uint32_t i) { return i * 2654435761u; };
	auto bOf = [](uint32_t i) { return i ^ 0x5A5A5A5Au; };
	for (uint32_t i = 0; i < kVaddItems; i++) {
		*poolDw(kVaddA + 4 * i) = aOf(i);
		*poolDw(kVaddB + 4 * i) = bOf(i);
		*poolDw(kVaddC + 4 * i) = 0xFFFFFFFF;
	}
	*poolDw(kVaddC + 4 * kVaddItems) = 0xFFFFFFFF;
	for (uint32_t off = 0; off < ((k.kernargSize + 3) & ~3u); off += 4)
		*poolDw(kKernargOffset + off) = 0;
	const uint64_t args[3] = { poolMc(kVaddA), poolMc(kVaddB), poolMc(kVaddC) };
	for (uint32_t i = 0; i < 3 && 8 * i + 8 <= k.kernargSize; i++) {
		*poolDw(kKernargOffset + 8 * i) = static_cast<uint32_t>(args[i]);
		*poolDw(kKernargOffset + 8 * i + 4) = static_cast<uint32_t>(args[i] >> 32);
	}

	const uint64_t kernarg = poolMc(kKernargOffset);
	const uint32_t user[2] = { static_cast<uint32_t>(kernarg), static_cast<uint32_t>(kernarg >> 32) };
	Launch l {};
	l.code = poolMc(kCodeObjCode);
	l.rsrc1 = k.rsrc1;
	l.rsrc2 = k.rsrc2;
	l.rsrc3 = k.rsrc3;
	l.user = user;
	l.userCount = k.userSgprCount();
	l.groups[0] = kVaddItems / kGroupSize;
	l.groups[1] = l.groups[2] = 1;
	l.groupSize[0] = kGroupSize;           // vadd's reqd_work_group_size
	l.groupSize[1] = l.groupSize[2] = 1;
	l.wave32 = k.wave32();

	trail("s7: DISPATCH_DIRECT vadd");
	uint64_t ns = 0;
	const bool done = launch(l, "kernel", ns);
	put("Fenced", done);
	if (!done) {
		CLOG("kernel: c[0] 0x%08x (want 0x%08x)", *poolDw(kVaddC), aOf(0) + 3 * bOf(0));
		publish();
		return false;
	}

	uint32_t bad = 0, firstBad = kVaddItems;
	for (uint32_t i = 0; i < kVaddItems; i++) {
		if (*poolDw(kVaddC + 4 * i) != aOf(i) + 3 * bOf(i)) {
			if (!bad)
				firstBad = i;
			bad++;
		}
	}
	const bool sentinel = *poolDw(kVaddC + 4 * kVaddItems) == 0xFFFFFFFF;
	put("Items", kVaddItems);
	put("WrongItems", bad);
	put("SentinelIntact", sentinel);
	put("Micros", ns / 1000);
	if (bad)
		CLOG("kernel: %u of %u results wrong, first at %u: 0x%08x (want 0x%08x)", bad, kVaddItems,
		     firstBad, *poolDw(kVaddC + 4 * firstBad), aOf(firstBad) + 3 * bOf(firstBad));
	else
		CLOG("kernel: clang's vadd ran from its code object: all %u results right "
		     "(c[%u] = 0x%08x) in %llu us, sentinel %s", kVaddItems, kVaddItems - 1,
		     *poolDw(kVaddC + 4 * (kVaddItems - 1)), ns / 1000, sentinel ? "intact" : "OVERWRITTEN");
	publish();
	return !bad && sentinel;
}
