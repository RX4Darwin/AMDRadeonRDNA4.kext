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
#include "pm4.hpp"
#include "probe_kernel.h"
#include "sdma.hpp"
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

uint32_t RDNA4Compute::requestedStage() {
	uint32_t stage = 0;
	if (!PE_parse_boot_argn("rdna4-compute", &stage, sizeof(stage)))
		return StageOff;
	return stage > StageKernel ? StageKernel : stage;
}

uint32_t RDNA4Compute::rd(uint16_t hwId, const Reg &r) const {
	uint32_t off;
	if (!env.mmio || !env.disc || !env.disc->regByteOffset(hwId, 0, r.seg, r.dword, off) ||
	    off + 4 > env.mmioSize)
		return kBad;
	return env.mmio[off / 4];
}

void RDNA4Compute::wr(uint16_t hwId, const Reg &r, uint32_t value) {
	uint32_t off;
	if (!env.mmio || !env.disc || !env.disc->regByteOffset(hwId, 0, r.seg, r.dword, off) ||
	    off + 4 > env.mmioSize)
		return;
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
}

// ---------------------------------------------------------------------------

uint32_t RDNA4Compute::start(const Env &e, uint32_t stage) {
	env = e;
	if (stage == StageOff)
		return StageOff;
	if (!env.pci || !env.owner || !env.mmio || !env.disc || !env.disc->isValid()) {
		CLOG("no MMIO or IP discovery table, compute bring-up skipped");
		return StageOff;
	}
	CLOG("bring-up to stage %u requested", stage);
	if (!rtLock)
		rtLock = IOLockAlloc();

	if (!survey())
		return StageOff;
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

void RDNA4Compute::logPreviousTrail() {
	IORegistryEntry *nvram = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!nvram)
		return;
	char text[96] {};
	if (OSObject *prev = nvram->copyProperty(kTrailKey)) {
		if (auto *s = OSDynamicCast(OSString, prev))
			strlcpy(text, s->getCStringNoCopy(), sizeof(text));
		else if (auto *d = OSDynamicCast(OSData, prev))
			memcpy(text, d->getBytesNoCopy(),
			       d->getLength() < sizeof(text) - 1 ? d->getLength() : sizeof(text) - 1);
		prev->release();
	}
	if (text[0])
		CLOG("previous boot's bring-up ended at: %s", text);
	else
		CLOG("no bring-up trail from a previous boot");
	nvram->release();
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

void RDNA4Compute::runStages() {
	// Read before this boot writes its own. Not at attach: that is before
	// the EFI NVRAM driver has published the stored variables.
	logPreviousTrail();
	uint32_t done = StageSurvey;
	char note[64];
	auto stop = [&](const char *what) {
		CLOG("%s failed; stopping, the display is not affected", what);
		snprintf(note, sizeof(note), "stopped: %s failed (no hang)", what);
		trail(note);
		env.owner->setProperty("Compute,Stage", static_cast<uint64_t>(done), 32);
	};
	if (target >= StagePsp) {
		if (!stagePsp())
			return stop("stage 2 (psp)");
		done = StagePsp;
	}
	if (target >= StageGfx) {
		if (!stageGfx())
			return stop("stage 3 (gfx)");
		done = StageGfx;
	}
	if (target >= StageSdma) {
		if (!stageSdma())
			return stop("stage 4 (sdma)");
		done = StageSdma;
	}
	if (target >= StageCompute) {
		if (!stageCompute())
			return stop("stage 5 (compute)");
		done = StageCompute;
	}
	if (target >= StageDispatch) {
		if (!stageDispatch())
			return stop("stage 6 (dispatch)");
		done = StageDispatch;
	}
	if (target >= StageKernel) {
		if (!stageKernel())
			return stop("stage 7 (kernel)");
		done = StageKernel;
	}
	env.owner->setProperty("Compute,Stage", static_cast<uint64_t>(done), 32);
	snprintf(note, sizeof(note), "finished at stage %u", done);
	trail(note);
	CLOG("bring-up finished at stage %u", done);
	if (done >= StageDispatch)
		publishRuntime(done);
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
	(void)env.mmio[off / 4];
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
	return resp == kBad ? 0 : resp;
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

	// 3. gfx_v12_0_wait_for_rlc_autoload_complete: CP idle and the RLC
	//    reports its bootload complete.
	trail("s3: wait for RLC bootload");
	uint32_t cpStat = kBad, boot = kBad;
	bool complete = false;
	for (uint32_t ms = 0; ms < 2000 && !complete; ms++) {
		cpStat = rdGc(CpStat);
		boot = rdGc(RlcBootloadStatus);
		complete = cpStat == 0 && boot != kBad && (boot & kRlcBootComplete);
		if (!complete)
			IOSleep(1);
	}
	put("CP_STAT", cpStat);
	put("RLC_BOOTLOAD_STATUS", boot);
	if (!complete) {
		CLOG("gfx: RLC autoload did not complete (CP_STAT 0x%08x, bootload 0x%08x)", cpStat, boot);
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
	put("GCMC_FB_BASE", fbBase);
	put("GCMC_FB_TOP", fbTop);
	publish();
	return true;
#endif
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

	// FB aperture: the VBIOS programs it on bare metal; if it reads back
	// empty, mirror the MM hub's, as amdgpu does for a VF.
	uint32_t gcBase = rdGc(GcFbLocationBase) & 0xffffff, gcTop = rdGc(GcFbLocationTop) & 0xffffff;
	if (!gcTop || gcTop < gcBase) {
		wr(IpDiscovery::HwGc, GcFbLocationBase, mmBase);
		wr(IpDiscovery::HwGc, GcFbLocationTop, mmTop);
		CLOG("sdma: GC hub FB aperture was empty; set to the MM hub's 0x%x..0x%x", mmBase, mmTop);
	} else if (gcBase != mmBase || gcTop != mmTop) {
		CLOG("sdma: GC hub FB aperture 0x%x..0x%x differs from the MM hub's 0x%x..0x%x",
		     gcBase, gcTop, mmBase, mmTop);
	}

	// Context 0: a flat table over one page nobody uses (MC page 0).
	for (uint32_t i = 0; i < 0x1000 / 4; i++)
		*poolDw(kPtOffset + i * 4) = 0;
	flushHdp();
	const uint64_t pt = poolMc(kPtOffset);
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
	const uint64_t scratchVram = pool.offset + kScratchOffset;
	wr(IpDiscovery::HwGc, GcSysDefaultLsb, static_cast<uint32_t>(scratchVram >> 12));
	wr(IpDiscovery::HwGc, GcSysDefaultMsb, static_cast<uint32_t>(scratchVram >> 44));
	const uint64_t scratchMc = poolMc(kScratchOffset);
	wr(IpDiscovery::HwGc, GcL2FaultDefaultLo, static_cast<uint32_t>(scratchMc >> 12));
	wr(IpDiscovery::HwGc, GcL2FaultDefaultHi, static_cast<uint32_t>(scratchMc >> 44));
	wr(IpDiscovery::HwGc, GcL2FaultCntl2, rdGc(GcL2FaultCntl2) | kL2FaultRetryRead);

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
	return gcHubFlush();
}

// gmc_v12_0_flush_vm_hub for VMID0 on the GC hub (engine 17, no semaphore).
bool RDNA4Compute::gcHubFlush() {
	wr(IpDiscovery::HwGc, Reg { 0, GcInvEng0Req.dword + kGcInvEngGart }, kInvReqVmid0);
	for (uint32_t us = 0; us < 100000; us += 10) {
		if (rdGc(Reg { 0, GcInvEng0Ack.dword + kGcInvEngGart }) & 1)
			return true;
		IODelay(10);
	}
	CLOG("sdma: GC hub TLB flush not acknowledged");
	return false;
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
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0Doorbell),
	   rdGc(sdma(0, SdmaQ0Doorbell)) & ~kSdmaDoorbellEnable);
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

void RDNA4Compute::sdmaKick(uint32_t wptrBytes) {
	*poolDw(kSdmaWptrOffset) = wptrBytes;
	*poolDw(kSdmaWptrOffset + 4) = 0;
	flushHdp();
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbWptr), wptrBytes);
	wr(IpDiscovery::HwGc, sdma(0, SdmaQ0RbWptrHi), 0);
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
	};

	// 1. GC hub.
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
	Sdma::Ring ring;
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
	sdmaQueueInit();
	status();

	// 3. First packet: one dword written by the engine (amdgpu's ring test).
	trail("s4: SDMA WRITE_LINEAR test");
	uint32_t pkt[8];
	ring.emit(pkt, Sdma::writeDword(pkt, poolMc(kSdmaTestOffset), 0xDEADBEEF));
	sdmaKick(ring.wptr());
	bool wrote = false;
	for (uint32_t us = 0; us < 200000 && !wrote; us += 10) {
		wrote = *poolDw(kSdmaTestOffset) == 0xDEADBEEF;
		if (!wrote)
			IODelay(10);
	}
	put("WriteTest", wrote);
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
	ring.emit(pkt, Sdma::fence(pkt, poolMc(kSdmaFenceOffset), 1));
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
	wr(IpDiscovery::HwNbif, NbifDoorbellAperEn, rd(IpDiscovery::HwNbif, NbifDoorbellAperEn) | 1);
	wr(IpDiscovery::HwNbif, NbifS2aDoorbell0, kS2aDoorbell0Gc);
	wr(IpDiscovery::HwNbif, NbifS2aDoorbell3, kS2aDoorbell3Gc);
	wr(IpDiscovery::HwGc, CpMecDoorbellLower, kMecDoorbellLowerBytes);
	wr(IpDiscovery::HwGc, CpMecDoorbellUpper, kMecDoorbellUpperBytes);

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
bool RDNA4Compute::hqdInit() {
	for (uint32_t off = kMqdOffset; off < kPqOffset; off += 4)   // MQD + EOP
		*poolDw(off) = 0;
	*poolDw(kPqRptrOffset) = 0;
	*poolDw(kPqWptrOffset) = 0;
	*poolDw(kPqWptrOffset + 4) = 0;
	flushHdp();

	const uint64_t eop = poolMc(kEopOffset) >> 8, pq = poolMc(kPqOffset) >> 8;
	const uint64_t mqd = poolMc(kMqdOffset), rptr = poolMc(kPqRptrOffset), wpoll = poolMc(kPqWptrOffset);
	const uint32_t eopSize = 8;                 // 2^(8+1) dwords = GFX12_MEC_HPD_SIZE
	const uint32_t queueSize = 9;               // 2^(9+1) dwords = 4 KiB
	uint32_t pqControl = (kHqdPqControlDefault & ~(kPqQueueSizeMask | kPqRptrBlockMask |
	                                               kPqTunnelDispatch)) |
	                     queueSize | (9u << 8) | kPqUnordDispatch | kPqPrivState | kPqKmdQueue;
	uint32_t doorbell = (kComputeDoorbellDword << kDoorbellOffsetShift) | kDoorbellEn;

	// The MQD in memory, as gfx_v12_0_compute_mqd_init fills it (v12_compute_mqd
	// dword offsets): the CP reads it back (CU masks, save/restore), so it
	// must describe the same queue the registers below do.
	auto mqdDw = [this](uint32_t dw, uint32_t v) { *poolDw(kMqdOffset + 4 * dw) = v; };
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
	mqdDw(131, 0);                                    // cp_hqd_vmid
	mqdDw(132, kHqdPersistentDefault);                // cp_hqd_persistent_state
	mqdDw(135, (1u << 0) | (1u << 4) | (1u << 8));    // cp_hqd_quantum: EN, SCALE 1, DURATION 1
	mqdDw(136, static_cast<uint32_t>(pq));            // cp_hqd_pq_base_lo
	mqdDw(137, static_cast<uint32_t>(pq >> 32));
	mqdDw(139, static_cast<uint32_t>(rptr) & ~3u);    // cp_hqd_pq_rptr_report_addr_lo
	mqdDw(140, static_cast<uint32_t>(rptr >> 32) & 0xffff);
	mqdDw(141, static_cast<uint32_t>(wpoll) & ~3u);   // cp_hqd_pq_wptr_poll_addr_lo
	mqdDw(142, static_cast<uint32_t>(wpoll >> 32) & 0xffff);
	mqdDw(143, doorbell);                             // cp_hqd_pq_doorbell_control
	mqdDw(145, pqControl);                            // cp_hqd_pq_control
	mqdDw(149, 0x00300000);                           // cp_hqd_ib_control: MIN_IB_AVAIL_SIZE 3
	mqdDw(162, kMqdControlDefault & ~0xfu);           // cp_mqd_control: VMID 0
	mqdDw(165, static_cast<uint32_t>(eop));           // cp_hqd_eop_base_addr_lo
	mqdDw(166, static_cast<uint32_t>(eop >> 32));
	mqdDw(167, (kHqdEopControlDefault & ~0x3fu) | eopSize);
	flushHdp();

	// gfx_v12_0_kiq_setting: the RLC schedules this VMID0 queue (ME1, pipe 0,
	// queue 0) — the low byte names it, bit 7 marks it valid.
	const uint32_t sched = (rdGc(RlcCpSchedulers) & 0xffffff00u) | (1u << 5) | (0u << 3) | 0u;
	wr(IpDiscovery::HwGc, RlcCpSchedulers, sched);
	wr(IpDiscovery::HwGc, RlcCpSchedulers, sched | 0x80);

	grbmSelect(1, 0, 0, 0);
	wr(IpDiscovery::HwGc, CpPqWptrPollCntl, rdGc(CpPqWptrPollCntl) & ~kPqWptrPollEn);
	wr(IpDiscovery::HwGc, CpHqdEopBase, static_cast<uint32_t>(eop));
	wr(IpDiscovery::HwGc, CpHqdEopBaseHi, static_cast<uint32_t>(eop >> 32));
	wr(IpDiscovery::HwGc, CpHqdEopControl, (kHqdEopControlDefault & ~0x3fu) | eopSize);
	wr(IpDiscovery::HwGc, CpHqdPqDoorbell, doorbell);
	if (rdGc(CpHqdActive) & 1) {                // a queue left behind: drain it
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 1);
		for (uint32_t us = 0; us < 100000 && (rdGc(CpHqdActive) & 1); us += 10)
			IODelay(10);
		wr(IpDiscovery::HwGc, CpHqdDequeueReq, 0);
		wr(IpDiscovery::HwGc, CpHqdPqRptr, 0);
		wr(IpDiscovery::HwGc, CpHqdPqWptrLo, 0);
		wr(IpDiscovery::HwGc, CpHqdPqWptrHi, 0);
	}
	wr(IpDiscovery::HwGc, CpMqdBaseAddr, static_cast<uint32_t>(mqd) & ~3u);
	wr(IpDiscovery::HwGc, CpMqdBaseAddrHi, static_cast<uint32_t>(mqd >> 32));
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
	wr(IpDiscovery::HwGc, CpHqdPqDoorbell, doorbell);
	wr(IpDiscovery::HwGc, CpHqdPqWptrLo, 0);
	wr(IpDiscovery::HwGc, CpHqdPqWptrHi, 0);
	wr(IpDiscovery::HwGc, CpHqdVmid, 0);
	wr(IpDiscovery::HwGc, CpHqdPersistent, kHqdPersistentDefault);
	wr(IpDiscovery::HwGc, CpHqdEopRptr, kEopInitFetcher);   // start the EOP fetcher (kfd hqd_load)
	wr(IpDiscovery::HwGc, CpHqdActive, 1);
	wr(IpDiscovery::HwGc, CpPqStatus, rdGc(CpPqStatus) | kPqStatusDoorbellEnable);
	const uint32_t active = rdGc(CpHqdActive), pqc = rdGc(CpHqdPqControl);
	grbmSelect(0, 0, 0, 0);
	CLOG("mec: HQD ME1/pipe0/queue0: active %u, PQ_CONTROL 0x%08x, doorbell dword %u",
	     active & 1, pqc, kComputeDoorbellDword);
	return active & 1;
}

// gfx_v12_0_ring_set_wptr_compute: the wptr copy, then the 64-bit doorbell.
void RDNA4Compute::pm4Kick(uint64_t wptrDwords) {
	*poolDw(kPqWptrOffset) = static_cast<uint32_t>(wptrDwords);
	*poolDw(kPqWptrOffset + 4) = static_cast<uint32_t>(wptrDwords >> 32);
	flushHdp();
	doorbells[kComputeDoorbellDword / 2] = wptrDwords;
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
		grbmSelect(0, 0, 0, 0);
		CLOG("mec: GRBM 0x%08x CP_STAT 0x%08x CPC 0x%08x MEC 0x%08x HQD active %u rptr 0x%x "
		     "rptr report 0x%x", rdGc(GrbmStatus), rdGc(CpStat), rdGc(CpCpcStatus),
		     rdGc(CpMecRs64Cntl), active & 1, rptr, *poolDw(kPqRptrOffset));
	};

	trail("s5: MEC start");
	wr(IpDiscovery::HwGc, GrbmCntl, (rdGc(GrbmCntl) & ~0xffu) | 0xff);   // READ_TIMEOUT
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
	trail("s5: HQD init");
	Pm4::Queue &q = pm4Queue;
	if (!q.init(poolDw(kPqOffset), poolMc(kPqOffset), kPqSize) || !hqdInit()) {
		status();
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
	*poolDw(kPm4FenceOffset) = 0;
	flushHdp();

	// Shader memory model for VMID0 (gfx_v12_0_constants_init).
	grbmSelect(0, 0, 0, 0);
	wr(IpDiscovery::HwGc, ShMemConfig, kShMemConfigDefault);

	const uint32_t pgm[2] = { static_cast<uint32_t>(l.code >> 8), static_cast<uint32_t>(l.code >> 40) };
	const uint32_t rsrc[2] = { l.rsrc1, l.rsrc2 };
	const uint32_t zero = 0, all[2] = { 0xffffffff, 0xffffffff };
	const uint32_t all4[4] = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff };
	const uint32_t start[3] = { 0, 0, 0 };

	Pm4::Queue &q = pm4Queue;
	uint32_t pkt[24];
	q.emit(pkt, Pm4::acquireMem(pkt, Pm4::kGcrMemSync));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputePgmLo), pgm, 2));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputePgmRsrc1), rsrc, 2));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputePgmRsrc3), &l.rsrc3, 1));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeResourceLim), &zero, 1));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeTmpringSize), &zero, 1));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeThreadMgmtSe0), all, 2));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeThreadMgmtSe2), all, 2));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeThreadMgmtSe4), all4, 4));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeStartX), start, 3));
	q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeNumThreadX), l.groupSize, 3));
	if (l.userCount)
		q.emit(pkt, Pm4::setShReg(pkt, shAbs(ComputeUserData0), l.user, l.userCount));
	q.emit(pkt, Pm4::dispatchDirect(pkt, l.groups[0], l.groups[1], l.groups[2],
	                                Pm4::kDispatchShaderEn | Pm4::kDispatchForceStart0 |
	                                (l.wave32 ? Pm4::kDispatchWave32 : 0)));
	q.emit(pkt, Pm4::releaseMem(pkt, poolMc(kPm4FenceOffset), ++pm4Fence));

	// Spin for the first 2 ms (short kernels), then sleep between polls:
	// user dispatches may run for seconds.
	uint64_t t0 = mach_absolute_time(), span = 0;
	nanoseconds_to_absolutetime(static_cast<uint64_t>(l.timeoutUs ? l.timeoutUs : 1000000) * 1000,
	                            &span);
	pm4Kick(q.wptr());
	bool done = false;
	for (uint32_t polls = 0;; polls++) {
		done = *poolDw(kPm4FenceOffset) == pm4Fence;
		if (done || mach_absolute_time() - t0 > span)
			break;
		if (polls < 200)
			IODelay(10);
		else
			IOSleep(1);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	if (!done) {
		CLOG("%s: the kernel's fence never came (0x%08x, want 0x%08x)", tag,
		     *poolDw(kPm4FenceOffset), pm4Fence);
		grbmSelect(1, 0, 0, 0);
		CLOG("%s: GRBM 0x%08x CP_STAT 0x%08x CPC 0x%08x HQD rptr 0x%x wptr %llu", tag,
		     rdGc(GrbmStatus), rdGc(CpStat), rdGc(CpCpcStatus), rdGc(CpHqdPqRptr),
		     static_cast<unsigned long long>(q.wptr()));
		grbmSelect(0, 0, 0, 0);
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
	else if (k.groupSegmentSize)
		w = "it needs LDS (group segment)";
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
