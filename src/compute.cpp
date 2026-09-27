//
//  compute.cpp
//  RDNA4FB
//
//  See compute.hpp. Stage 1 (survey) only reads registers. Stage 2 is the
//  first to write: PSP mailbox/ring registers, the HDP flush, and the compute
//  pool in VRAM — never DCN, the MM hub or the scanout.
//

#include "compute.hpp"

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
	return stage > StageDispatch ? StageDispatch : stage;
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
	constexpr uint64_t kAlign = 16 * kMiB, kWant = 64 * kMiB, kFloor = 128 * kMiB;
	uint64_t start = (sv.scanoutOffset + env.scanoutLength + kAlign - 1) & ~(kAlign - 1);
	if (start < kFloor)
		start = kFloor;
	if (start + kWant > sv.bar0Size)
		return;
	pool.offset = start;
	pool.size = kWant;
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
	if (target >= StageSdma)
		CLOG("stage 4 (sdma) is not implemented yet; stopping after stage 3");
	env.owner->setProperty("Compute,Stage", static_cast<uint64_t>(done), 32);
	snprintf(note, sizeof(note), "finished at stage %u", done);
	trail(note);
	CLOG("bring-up finished at stage %u", done);
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
	uint32_t ret = 0, answer = 0;
	for (int tries = 0; tries < 30 && answer != 1; tries++) {
		answer = smuSend(kSmuMsgTest, 0xC0FFEE, ret, 100);
		if (answer != 1)
			IOSleep(100);
	}
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
