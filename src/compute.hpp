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
//    stage 3  gfx       the GC firmware set (SDMA, RS64 PFP/ME/MEC + stacks,
//                       MES, IMU, RLC) into the TMR via LOAD_IP_FW, then
//                       AUTOLOAD_RLC: the RLC boots GFX; verified by
//                       RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE.
//    stage 4  sdma      GC hub apertures, one SDMA queue, a VRAM fill read
//                       back by the CPU: the first work the GPU does for us.
//    stage 5  compute   one MEC compute queue (HQD) programmed directly, a
//                       PM4 fence.
//    stage 6  dispatch  a hand-written gfx1201 kernel on the compute units.
//    stage 7  kernel    a clang-built kernel launched from its code object.
//
//  Selected with the boot-arg rdna4-compute=<stage>; absent/0 = off, and the
//  plugin behaves exactly as without this file. Each stage runs the ones
//  before it. Stage 1 runs inline; stages 2+ run on their own kernel thread
//  a few seconds later, so the desktop never waits on (or for) them.
//
//  Once stage 6 or 7 finished, the runtime (runtime.cpp) is published for
//  user space as RDNA4ComputeService (userclient.cpp, include/rdna4compute.h).
//

#ifndef RDNA4Compute_hpp
#define RDNA4Compute_hpp

#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

#include <kern/thread.h>

#include "amdfw.hpp"
#include "codeobj.hpp"
#include "gfxregs.hpp"
#include "gpuheap.hpp"
#include "ipdiscovery.hpp"
#include "pm4.hpp"
#include "psp.hpp"
#include "rdna4compute.h"

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
		StageSdma     = 4,
		StageCompute  = 5,
		StageDispatch = 6,
		StageKernel   = 7,
	};

	// rdna4-compute=<stage>, clamped to StageKernel. 0 when absent.
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

	// Breadcrumb in NVRAM (Lilu vendor GUID, key rdna4-trail), written and
	// flushed before each step that could hang the GPU, so the step a hard
	// hang stopped at survives the reset. Logged by the next boot's survey.
	void trail(const char *step);
	// Log the previous boot's trail (into `text`); true if that boot died
	// in the middle of a step rather than ending the bring-up itself.
	bool logPreviousTrail(char *text, size_t size);

	// The compute pool, mapped uncached through BAR0.
	IOMemoryMap *poolMap { nullptr };
	uint8_t     *poolCpu { nullptr };
	bool mapPool();
	void flushHdp();

	// Stage 2.
	Psp::Driver psp;
	bool stagePsp();

	// Stage 3.
	bool stageGfx();
	// What amdgpu's smu_hw_init sends between AUTOLOAD_RLC and the autoload
	// wait: on a dGPU the PMFW's GFX power-up (feature GFX_IMU) is what
	// releases the IMU that performs the autoload. `allowed` is the feature
	// mask offered; returns the running mask (0 if unreadable).
	uint64_t smuEnableFeatures(uint64_t allowed, uint32_t domain);
	bool waitRlcAutoload(uint32_t ms, uint32_t &cpStat, uint32_t &boot);
	static constexpr uint32_t kSmuTableOffset = 6u << 20;            // driver table, 64 KiB

	// Stage 4: the GC hub, then SDMA0 queue 0 and a VRAM fill. Pool layout
	// past the PSP's first 4 MiB (offsets within the pool).
	static constexpr uint32_t kPtOffset       = 4u << 20;             // VMID0 page table (unused range)
	static constexpr uint32_t kScratchOffset  = kPtOffset + 0x1000;   // system-aperture default page
	static constexpr uint32_t kSdmaRingOffset = 8u << 20;             // 4 KiB ring
	static constexpr uint32_t kSdmaRingSize   = 0x1000;
	static constexpr uint32_t kSdmaRptrOffset = kSdmaRingOffset + 0x1000;   // rptr writeback
	static constexpr uint32_t kSdmaWptrOffset = kSdmaRptrOffset + 0x40;     // wptr poll copy
	static constexpr uint32_t kSdmaTestOffset = kSdmaRingOffset + 0x2000;   // WRITE target
	static constexpr uint32_t kSdmaFenceOffset = kSdmaTestOffset + 0x40;    // FENCE target
	static constexpr uint32_t kFillOffset     = 16u << 20;            // CONST_FILL target
	static constexpr uint32_t kFillBytes      = 1u << 20;
	uint64_t poolMc(uint32_t off) const { return pool.mcAddress + off; }
	volatile uint32_t *poolDw(uint32_t off) const {
		return reinterpret_cast<volatile uint32_t *>(poolCpu + off);
	}
	bool gcHubInit();
	bool gcHubFlush();
	bool sdmaQueueInit();
	void sdmaKick(uint32_t wptrBytes);
	bool stageSdma();

	// Stage 5: one MEC compute queue (ME1 pipe 0 queue 0) programmed
	// directly, fed through its doorbell, running PM4.
	static constexpr uint32_t kMqdOffset      = 12u << 20;            // 4 KiB MQD (kept zero)
	static constexpr uint32_t kEopOffset      = kMqdOffset + 0x1000;  // 2 KiB EOP buffer
	static constexpr uint32_t kPqOffset       = kMqdOffset + 0x2000;  // 4 KiB PM4 queue
	static constexpr uint32_t kPqSize         = 0x1000;
	static constexpr uint32_t kPqRptrOffset   = kMqdOffset + 0x3000;  // rptr report
	static constexpr uint32_t kPqWptrOffset   = kPqRptrOffset + 0x40; // wptr poll copy
	static constexpr uint32_t kPm4TestOffset  = kPqRptrOffset + 0x80; // WRITE_DATA target
	static constexpr uint32_t kPm4FenceOffset = kPqRptrOffset + 0xc0; // RELEASE_MEM target
	IOMemoryMap        *doorbellMap { nullptr };
	volatile uint64_t  *doorbells { nullptr };
	void grbmSelect(uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid);
	bool mecStart();
	bool doorbellInit();
	bool hqdInit();
	void pm4Kick(uint64_t wptrDwords);
	bool stageCompute();

	// Stage 6: run a real kernel (shaders/probe.s) on the compute units.
	static constexpr uint32_t kShaderOffset   = 14u << 20;            // code, 256-byte aligned
	static constexpr uint32_t kDispatchBuffer = 20u << 20;            // results
	static constexpr uint32_t kDispatchGroups = 4, kGroupSize = 64;
	Pm4::Queue  pm4Queue;                   // set up by stage 5, fed again by stage 6
	uint32_t    pm4Fence { 0 };             // last RELEASE_MEM sequence number
	uint32_t shAbs(const GfxReg::Reg &r) const;   // absolute dword address of a GC register
	// One DISPATCH_DIRECT on the stage-5 queue, fenced; false if the fence
	// never came within timeoutUs (the engine state is logged under `tag`).
	struct Launch {
		uint64_t        code;               // MC address, 256-byte aligned
		uint32_t        rsrc1, rsrc2, rsrc3;
		const uint32_t *user;               // COMPUTE_USER_DATA_0..
		uint32_t        userCount;
		uint32_t        groups[3];          // work-groups per dimension
		uint32_t        groupSize[3];       // work-items per group
		bool            wave32;
		uint32_t        timeoutUs;
	};
	bool launch(const Launch &l, const char *tag, uint64_t &ns);
	// What launch() can give a code-object kernel: the kernarg pointer and
	// nothing else yet — no dispatch/queue pointers, scratch or LDS.
	static bool kernelFits(const CodeObj::Kernel &k, const char **why);
	bool stageDispatch();

	// Stage 7: a clang-built kernel (shaders/vadd.cl) from its code object.
	static constexpr uint32_t kCodeObjCode    = kShaderOffset + 0x10000;   // up to 64 KiB
	static constexpr uint32_t kCodeObjCodeMax = 0x10000 - 0x100;          // + prefetch pad
	static constexpr uint32_t kKernargOffset  = kShaderOffset + 0x20000;   // up to 4 KiB
	static constexpr uint32_t kKernargMax     = 0x1000;
	static constexpr uint32_t kVaddA          = 24u << 20;            // a, b, c: 64 KiB apart
	static constexpr uint32_t kVaddB          = kVaddA + 0x10000;
	static constexpr uint32_t kVaddC          = kVaddA + 0x20000;
	static constexpr uint32_t kVaddItems      = 4096;                 // 64 groups of 64
	bool stageKernel();

public:
	// User-space runtime (runtime.cpp), reached through RDNA4ComputeClient.
	// `owner` is the client: its buffers and programs are only its own, and
	// rtRelease frees them all. Every call takes rtLock.
	IOReturn rtInfo(uint64_t out[6]);
	IOReturn rtAlloc(const void *owner, uint64_t bytes, uint64_t &handle, uint64_t &gpu);
	IOReturn rtFree(const void *owner, uint64_t handle);
	IOReturn rtCopy(const void *owner, uint64_t handle, uint64_t offset, task_t task,
	                mach_vm_address_t user, uint64_t length, bool toGpu);
	IOReturn rtLoad(const void *owner, task_t task, mach_vm_address_t elf, uint64_t length,
	                const char *name, uint64_t out[7]);
	IOReturn rtUnload(const void *owner, uint64_t program);
	IOReturn rtDispatch(const void *owner, const RDNA4Dispatch &d, uint64_t &micros);
	void     rtRelease(const void *owner);

private:
	// The pool past kHeapOffset is the runtime's heap (buffers and code).
	static constexpr uint32_t kHeapOffset  = 32u << 20;
	static constexpr uint32_t kMaxBuffers  = 256;
	static constexpr uint32_t kMaxPrograms = 32;
	struct RtBuffer  { const void *owner; uint64_t offset, bytes; uint16_t gen; };
	struct RtProgram { const void *owner; uint64_t offset; CodeObj::Kernel k; uint16_t gen; };
	IOLock        *rtLock { nullptr };
	bool           rtReady { false };       // a dispatching stage finished
	bool           rtWedged { false };      // a dispatch timed out
	uint32_t       rtStage { 0 };
	GpuHeap::Heap  heap;
	RtBuffer       buffers[kMaxBuffers] {};
	RtProgram      programs[kMaxPrograms] {};
	IOService     *rtService { nullptr };
	void publishRuntime(uint32_t stage);
	RtBuffer  *bufferFor(const void *owner, uint64_t handle);
	RtProgram *programFor(const void *owner, uint64_t handle);
	// SMU mailbox (MP1): send one message, return the response code
	// (1 = OK, 0 = no answer) and the argument register after it.
	uint32_t smuSend(uint32_t msg, uint32_t param, uint32_t &ret, uint32_t timeoutMs);

	static uint32_t pspRead(void *ctx, uint32_t dword);
	static void     pspWrite(void *ctx, uint32_t dword, uint32_t value);
	static void     pspDelay(void *ctx, uint32_t us);
	static void     pspFlush(void *ctx);
};

#endif /* RDNA4Compute_hpp */
