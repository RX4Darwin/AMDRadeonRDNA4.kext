//
//  compute.hpp
//  RDNA4FB
//
//  RDNA4Compute: our own bring-up of the RX 9070 XT's compute engines, next
//  to (never instead of) the display path. Written from scratch against the
//  hardware; amdgpu's register headers and bring-up order are the reference.
//
//  The card arrives POSTed by the VBIOS/GOP (as surveyed on the RX 9070 XT,
//  2026-09-27): memory trained, the MM hub and DCN scanning out the desktop,
//  the PSP *bootloader* waiting with no secure OS loaded, no SMU firmware,
//  and GFX (IMU/RLC/CP), SDMA and the GC hub cold. Because the sOS is not
//  up, the normal driver path needs no GPU reset, so the display that
//  RDNA4Device drives stays lit:
//
//    stage 1  survey    read-only: state of every engine, both hubs, the PSP,
//                       and the VRAM layout the compute side can use.
//    stage 2  psp       sOS components through the bootloader, the GPCOM
//                       ring, LOAD_TOC, then the SMU firmware (LOAD_IP_FW);
//                       verified by the SMU answering its mailbox.
//    stage 3  gfx       GC firmware (SDMA, RS64 PFP/ME/MEC, IMU, RLC) into the
//                       TMR, RLC autoload + IMU start, GC hub, an SDMA VRAM
//                       fill and a compute-queue PM4 fence.
//    stage 4  dispatch  an amdhsa code object (upstream LLVM, gfx1201).
//
//  Selected with the boot-arg rdna4-compute=<stage>; absent/0 = off, and the
//  plugin behaves exactly as without this file. Each stage runs the ones
//  before it. Stage 1 runs inline; stages 2+ run on their own kernel thread
//  a few seconds later, so the desktop never waits on (or for) them.
//  Stages not implemented yet are logged and skipped.
//

#ifndef RDNA4Compute_hpp
#define RDNA4Compute_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

#include <kern/thread.h>

#include "amdfw.hpp"
#include "gfxregs.hpp"
#include "ipdiscovery.hpp"
#include "psp.hpp"

class RDNA4Compute {
public:
	// What the compute side borrows from the display device: the GPU, the
	// service registry properties go on, the BAR5 mapping and the IP
	// discovery table, and the scanout the GOP set up (to keep clear of it).
	struct Env {
		IOPCIDevice        *pci;
		IOService          *owner;
		volatile uint32_t  *mmio;
		size_t              mmioSize;
		const IpDiscovery  *disc;
		uint64_t            scanoutPhys;     // CPU physical address (BAR0)
		uint64_t            scanoutLength;
	};

	enum Stage : uint32_t {
		StageOff      = 0,
		StageSurvey   = 1,
		StagePsp      = 2,
		StageGfx      = 3,
		StageDispatch = 4,
	};

	// rdna4-compute=<stage>, clamped to StageDispatch. 0 when absent.
	static uint32_t requestedStage();

	// Run the survey now and, for stage >= 2, start the bring-up thread.
	// Returns the last stage completed inline.
	uint32_t start(const Env &env, uint32_t stage);

	// Snapshot of the engines, as read by the survey.
	struct Survey {
		// GC
		uint8_t  gcMajor, gcMinor, gcRev;
		bool     gcReadable;
		uint32_t grbmStatus, grbmStatus2, cpStat, cpcStatus, cpfStatus;
		uint32_t cpMeCntl, mecCntl, mesCntl, pfpPc, mePc, mecPc;
		uint32_t rlcCntl, rlcStat, rlcGpmStat, rlcBootload;
		uint32_t imuCoreCtrl, imuGfxReset;
		// SDMA
		uint32_t sdmaMcuCntl[GfxReg::kSdmaInstances];
		uint32_t sdmaStatus[GfxReg::kSdmaInstances];
		uint32_t sdmaUcodeRev[GfxReg::kSdmaInstances];
		uint32_t sdmaRbCntl[GfxReg::kSdmaInstances];
		// Hubs
		uint32_t gcFbBase, gcFbTop, gcFbOffset, gcAgpBase, gcAgpBot, gcAgpTop, gcSysLow;
		uint32_t gcL2Cntl, gcCtx0Cntl, gcPtLo, gcPtHi;
		uint32_t mmFbBase, mmFbTop, mmFbOffset, mmAgpBase, mmAgpBot, mmAgpTop;
		uint32_t mmSysLow, mmSysHigh, mmL2Cntl, mmCtx0Cntl, mmPtLo, mmPtHi;
		// PSP
		uint32_t pspBoot, pspRing, pspSos;
		// HDP flush remap (NBIF)
		uint32_t hdpMemFlushRemap, hdpRegFlushRemap;
		// Memory
		uint32_t vramMiB;
		uint64_t bar0Phys, bar0Size, bar2Size;
		uint64_t scanoutOffset;              // in VRAM; ~0 = not in BAR0
		uint64_t fbMcBase, fbMcTop;          // from the MM hub
	};

	// The VRAM window the compute side may use: inside the CPU-visible
	// aperture (BAR0), clear of the scanout. Offsets are from VRAM start.
	struct Pool {
		uint64_t offset;
		uint64_t size;
		uint64_t mcAddress;                  // as the GPU addresses it
		bool     valid;
	};

private:
	Env    env {};
	Survey sv {};
	Pool   pool {};
	uint32_t target { StageOff };

	uint32_t rd(uint16_t hwId, const GfxReg::Reg &r) const;
	void     wr(uint16_t hwId, const GfxReg::Reg &r, uint32_t value);
	uint32_t rdGc(const GfxReg::Reg &r) const { return rd(IpDiscovery::HwGc, r); }

	bool survey();
	void logSurvey() const;
	void publishSurvey() const;
	void choosePool();

	// Stages 2+ (bring-up thread).
	static void threadMain(void *arg, wait_result_t);
	void runStages();

	// The compute pool, mapped uncached through BAR0.
	IOMemoryMap *poolMap { nullptr };
	uint8_t     *poolCpu { nullptr };
	bool mapPool();
	void flushHdp();

	// Stage 2.
	Psp::Driver psp;
	bool stagePsp();
	// SMU mailbox (MP1): send one message, return the response code
	// (1 = OK, 0 = no answer) and the argument register after it.
	uint32_t smuSend(uint32_t msg, uint32_t param, uint32_t &ret, uint32_t timeoutMs);

	static uint32_t pspRead(void *ctx, uint32_t dword);
	static void     pspWrite(void *ctx, uint32_t dword, uint32_t value);
	static void     pspDelay(void *ctx, uint32_t us);
	static void     pspFlush(void *ctx);
};

#endif /* RDNA4Compute_hpp */
