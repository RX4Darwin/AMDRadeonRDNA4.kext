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
#include "flip.hpp"
#include "gfxregs.hpp"
#include "gpuheap.hpp"
#include "ih.hpp"
#include "gpuvm.hpp"
#include "ipdiscovery.hpp"
#include "pm4.hpp"
#include "gpuvmtable.hpp"
#include "ptpages.hpp"
#include "vmid.hpp"
#include "pipe.hpp"
#include "psp.hpp"
#include "rdna4compute.h"
#include "sdma.hpp"

struct IOExternalMethodArguments;

class IOBufferMemoryDescriptor;
class IODMACommand;
class RDNA4Compute;

namespace Flip {
bool run(RDNA4Compute &compute);
bool waitNextVblank(RDNA4Compute &compute, uint8_t otg, uint32_t timeoutMs,
                    uint64_t &frame);
}
class IOFilterInterruptEventSource;
class IOTimerEventSource;
class IOWorkLoop;
class OSObject;
class IOMemoryMap;
class OSDictionary;
class IONotifier;

// The plugin owns the boot-arg value; runtime/display paths use the level to
// keep high-rate diagnostics off the real card's normal trace setting.
extern uint32_t rdna4TraceLevel;

class RDNA4Compute {
public:
	~RDNA4Compute();
	// Called by the IOKit filter/action callbacks; they only inspect the
	// writeback pointer and drain the already-programmed ring.
	bool ihHasWork() const;
	void ihAction();
	bool ihWaitVblank(uint32_t otg, uint32_t timeoutMs, uint64_t &count, uint64_t &timeNs);
	bool ihWaitFlip(uint32_t hubp, uint64_t sinceCount, uint32_t timeoutMs);
	// Bridge for Pipe::discover: DCN reads use the same discovered DMU path as
	// the display device, while the IH code owns the callback context.
	uint32_t ihDcnRead(uint8_t baseIdx, uint32_t dword) const;

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
		uint64_t            scanoutFrameNs;  // 0 when the display timing is unknown
		void (*vblankServiceReady)(IOService *framebuffer) { nullptr };
		// W32: read-only cursor register dump the display device offers (flip.cpp calls it after each flip).
		void (*cursorProbe)(void *ctx, const char *why) { nullptr };
		void *cursorProbeCtx { nullptr };
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
	static bool requestedVm();
	static bool requestedPowerManagement();

	// Run the survey now and, for stage >= 2, start the bring-up thread.
	// Returns the last stage completed inline.
	uint32_t start(const Env &env, uint32_t stage);
	void powerWillSleep();
	void powerSleepRequest();   // P7: setPowerState(0) / the debug sleep selector call this first, without rtLock (src/pmidle.cpp)
	void powerDidWake();

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
	// It never starts below kPoolFloor: the firmware's console lives at the
	// bottom of VRAM, and the plugin keeps a second display's surface there too.
	static constexpr uint64_t kPoolFloor = 128ull << 20;
	struct Pool {
		uint64_t offset;
		uint64_t size;
		uint64_t mcAddress;                  // as the GPU addresses it
		bool     valid;
	};

private:
	friend bool Flip::run(RDNA4Compute &compute);
	friend bool Flip::findPipe(RDNA4Compute &compute, Flip::Surface &out, bool log);
	friend bool Flip::waitNextVblank(RDNA4Compute &compute, uint8_t otg, uint32_t timeoutMs,
	                                uint64_t &frame);
	friend bool Flip::flipTo(RDNA4Compute &compute, const Flip::Surface &surface,
	                         uint64_t target, const char *name, uint64_t *latencyUs,
	                         bool async);

	Env    env {};
	Survey sv {};
	Pool   pool {};
	uint32_t target { StageOff };
	IOLock *resultLock { nullptr };
	OSDictionary *resultDictionary { nullptr };

	uint32_t rd(uint16_t hwId, const GfxReg::Reg &r) const;
	void     wr(uint16_t hwId, const GfxReg::Reg &r, uint32_t value);
	void     publishResult(const char *feature, const char *value);
	uint32_t rdGc(const GfxReg::Reg &r) const { return rd(IpDiscovery::HwGc, r); }

	bool survey();
	void logSurvey() const;
	void publishSurvey() const;
	void choosePool();

	// Stages 2+ (bring-up thread).
        static void threadMain(void *arg, wait_result_t);
        static void resumeMain(void *arg, wait_result_t);
        static IOReturn systemPowerMessage(void *target, void *refCon, UInt32 messageType,
                                            IOService *provider, void *messageArgument,
                                            vm_size_t argSize);
        void registerShutdownInterest();
        void defensiveStart();
        void quiesceForShutdown(const char *why);
        void runStages();
	bool beginBringup();
	void endBringup();
	bool bringupStepAllowed(const char *step);
	void resetRuntimeForResume();

	// Breadcrumb in NVRAM (Lilu vendor GUID, key rdna4-trail), written and
	// flushed before each step that could hang the GPU, so the step a hard
	// hang stopped at survives the reset. Logged by the next boot's survey.
	void trail(const char *step);
	// The pre-Metal features past the compute stages (W1-W5) name their
	// trail steps "<feature>: ..." (gfx, ih, vm, flip). If the previous
	// boot died in one of those, only that feature sits this boot out; the
	// stages and the other features run. featureAllowed("gfx") etc.
	char hungFeature[8] {};
	bool pmCapPending { false };       // the last boot died inside a "pm: cap ..." step
	bool featureAllowed(const char *name) const;
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
	bool readSensors(RDNA4Sensors &out);
	bool readSensorsEx(RDNA4SensorsEx &out);

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
	void cpConfigRs64();                // gfx_v12_0_config_gfx_rs64 (PSP loading)
	void logGcFault(const char *tag);   // GCVM_L2 protection fault status/address
	// GCVM_L2_PROTECTION_FAULT_ADDR_LO32/HI32 hold the logical page
	// (LOGICAL_PAGE_ADDR_LO32, HI4 in gc_12_0_0_sh_mask.h): the VA is page << 12.
	uint64_t gcFaultVa() const {
		return (rdGc(GfxReg::GcL2FaultAddrLo) |
		        (static_cast<uint64_t>(rdGc(GfxReg::GcL2FaultAddrHi) & 0xf) << 32)) << 12;
	}
	// gmc_v12_0_process_interrupt: WREG32_P(vm_l2_pro_fault_cntl, 1, ~1), i.e.
	// GCVM_L2_PROTECTION_FAULT_CNTL.CLEAR_PROTECTION_FAULT_STATUS_ADDR (bit 0)
	// clears the latched status and address.
	void gcFaultClear() {
		wr(IpDiscovery::HwGc, GfxReg::GcL2FaultCntl, rdGc(GfxReg::GcL2FaultCntl) | 1);
	}
	bool sdmaStartMcus();               // sdma_v7_0_enable: unhalt before queue setup
	bool sdmaQueueInit();
	uint64_t sdmaStartPtr { 0 };             // P7: the SDMA ring's 64-bit pointers after a wake without power loss (0 at boot and after a real power loss)
	void sdmaKick(uint64_t wptrBytes);
	bool sdmaDoorbell { false };        // kick SDMA0 through its doorbell (amdgpu's way)
	Sdma::Ring sdmaRing;                // SDMA0 queue 0: stage 4, then the runtime's DMA
	uint32_t   sdmaFence { 0 };         // last FENCE value written by SDMA
	bool doorbellMapBar();
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
	bool hqdInit(bool asKiq);
	bool hqdInitFor(bool asKiq, uint32_t pipe, uint32_t queue, uint32_t vmid,
	                uint64_t mqd, uint64_t eop, uint64_t pq, uint64_t rptr,
	                uint64_t wpoll, uint32_t doorbell);
	void pm4Kick(uint64_t wptrDwords);
	void pm4Kick(Pm4::Queue &queue, uint32_t doorbell, uint64_t wptrDwords);
	bool stageCompute();

	// Stage 6: run a real kernel (shaders/probe.s) on the compute units.
	static constexpr uint32_t kShaderOffset   = 14u << 20;            // code, 256-byte aligned
	static constexpr uint32_t kDispatchBuffer = 20u << 20;            // results
	static constexpr uint32_t kDispatchGroups = 4, kGroupSize = 64;
	Pm4::Queue  pm4Queue;                   // set up by stage 5, fed again by stage 6
	uint32_t    pm4Fence { 0 };             // last RELEASE_MEM sequence number
	uint32_t    hqdMode { 1 };              // 1 = plain MMIO HQD, 2 = KIQ fallback
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
		uint32_t        ldsBytes;           // per work-group (RSRC2.LDS_SIZE)
		bool            useInterrupt;       // runtime only; the fence remains authoritative
		bool            preserveFence;      // another client IB may still be outstanding
		Pm4::Queue     *queue;
		uint32_t        vmid, pipe, queueId, fenceValue, doorbell;
		uint64_t        fenceAddress;
		volatile uint32_t *fenceCpu;
		volatile uint32_t *queueCpu;
		uint64_t        queueAddress;
		uint64_t        recoveryMqd, recoveryEop, recoveryRptr, recoveryWpoll;
		uint64_t        recoveryProofAddress;
		volatile uint32_t *recoveryProofCpu;
		// W13 S7-lite (rdna4-vmshared=1): the packets go into the client's IB page (ibCpu, at VA ibVa in the client's VM) and the
		// shared VMID-0 queue `queue` runs INDIRECT_BUFFER(ibVmid) + a ring-level RELEASE_MEM to fenceAddress (an MC address).
		volatile uint32_t *ibCpu;
		uint64_t        ibVa;
		uint32_t        ibVmid;
		uint64_t        queueFenceAddr;       // rdna4-vmshared=2: a second ring-level fence on the shared queue's own word (the pool's idleness test)
		uint32_t        queueFenceSeq;
	};
	bool launch(const Launch &l, const char *tag, uint64_t &ns);
	void logComputeQueueState(const char *tag, uint32_t pipe = 0, uint32_t queue = 0,
	                         uint32_t vmid = 0);
	bool queueWriteTest(const char *tag, const Launch *l = nullptr);
	bool recoverComputeQueue(const char *tag, const Launch *l = nullptr);
	// What launch() can give a code-object kernel: the kernarg pointer and
	// up to 64 KiB of LDS — no dispatch/queue pointers or scratch yet.
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

	// W3, the gfx ring (gfxring.cpp): after the compute stages when the
	// boot-arg rdna4-gfx is 2 (1 is an alias); the write pointer goes through the
	// ring's doorbell. Pool layout past the DMA scratch, below the heap.
	static constexpr uint32_t kGfxOffset      = 30u << 20;
	static constexpr uint32_t kGfxRingOffset  = kGfxOffset;              // 16 KiB ring
	static constexpr uint32_t kGfxRingSize    = 0x4000;
	static constexpr uint32_t kGfxRptrOffset  = kGfxOffset + 0x4000;     // rptr writeback
	static constexpr uint32_t kGfxWptrOffset  = kGfxRptrOffset + 0x40;   // wptr poll copy
	static constexpr uint32_t kGfxFenceOffset = kGfxRptrOffset + 0x80;   // RELEASE_MEM target
	static constexpr uint32_t kGfxTestOffset  = kGfxRptrOffset + 0xc0;   // WRITE_DATA targets
	static constexpr uint32_t kGfxCsbOffset   = kGfxOffset + 0x5000;     // clear-state buffer
	static constexpr uint32_t kGfxCsbMax      = 0x1000;
	static constexpr uint32_t kGfxIbOffset    = kGfxOffset + 0x10000;    // indirect buffers
	// G3, the first draw (stageGfxDraw, after the runtime: its GE rings come
	// from the device heap): the NGG VS and the PS, 1 KiB apart (256-byte
	// aligned, prefetch-padded), the 256x256 RGBA8 target and the draw's
	// own fence.
	static constexpr uint32_t kGfxVsOffset    = kGfxOffset + 0x20000;
	static constexpr uint32_t kGfxPsOffset    = kGfxOffset + 0x20400;
	static constexpr uint32_t kGfxTargetOffset = kGfxOffset + 0x40000;   // 256 KiB
	// G4 (rdna4-gfxcol=1, docs/g4-colour.md): its own code slots (256-byte aligned, 0x400 each like the G3 ones) so that nothing G3 or
	// the ladder still needs is overwritten; it shares the target, the draw fence, the ring and the GE rings with G3.
	static constexpr uint32_t kGfxColVsOffset = kGfxOffset + 0x28000;
	static constexpr uint32_t kGfxColPsOffset = kGfxOffset + 0x28400;
	static constexpr uint32_t kGfxDrawFenceOffset = kGfxTestOffset + 0x20;
	static constexpr uint32_t kGfxMarkerOffset = kGfxTestOffset + 0x40;   // the PS-store diagnostic marker
	static constexpr uint32_t kGfxProbeOffset  = kGfxRptrOffset + 0x200;  // CP-view readback, state before the draw (32 dwords); +0x100 is the PS marker (W33: they overlapped)
	static constexpr uint32_t kGfxProbePost    = kGfxRptrOffset + 0x280;  // ... and after it
	// W37: pipeline statistics before/after the draw (SAMPLE_PIPELINESTAT: 14 x u64 = 0x70 bytes, 8-byte aligned), the
	// NGG/VS marker variant's three dwords, and a private zeroed page to map at VMID0 VA 0 (rootcause-draw.md #2).
	static constexpr uint32_t kGfxPstatPre     = kGfxRptrOffset + 0x300;
	static constexpr uint32_t kGfxPstatPost    = kGfxRptrOffset + 0x400;
	static constexpr uint32_t kGfxNggMarkOffset = kGfxRptrOffset + 0x500;
	static constexpr uint32_t kGfxVa0Offset    = kGfxOffset + 0x80000;    // 4 KiB, page aligned
	static constexpr uint32_t kGfxClipOffset   = kGfxRptrOffset + 0x600;  // W45: clip-state readback, 64 dwords
	static constexpr uint32_t kGfxVgprOffset   = kGfxRptrOffset + 0x800;  // W45: per-lane VGPR arrays of variant 512, 4 x 0x80 bytes
	uint64_t   gfxRings { 0 };               // device-heap offset of the GE rings (0 = none)
	bool stageGfxDraw();
	// W37 (rootcause-draw.md #1-#4).
	void gfxSrmEnable();                     // RLC_SRM_CNTL |= AUTO_INCR_ADDR | SRM_ENABLE after the CSB init, as amdgpu
	static bool requestedGfxSane();          // rdna4-gfxsane=0 leaves the explicit clip state block out
	void gfxEmitSaneClip();                  // W45: the clip/cull/viewport/scissor registers the stream leaves undefined, neutral, before the draw
	void gfxEmitClipProbe(uint32_t poolOff); // W45: COPY_DATA of the clip state after the draw
	void gfxClipReport(const char *label, uint32_t poolOff, volatile uint32_t *ib);
	uint64_t gfxPstatLast[8] {};             // the last pipeline-statistics deltas (PS, C_PRIM, C_INV, VS, GS_INV, GS_PRIM, IA_PRIM, IA_VERT)
	struct GfxDrawResult;
	void gfxVerdictAdd(const char *fmt, ...) __printflike(2, 3);   // W46: append to the registry property Compute,GFXVerdict
	void gfxVerdictDraw(const char *label, uint32_t variant, const GfxDrawResult &r);
	static bool requestedGfxPark();          // probe boots: park PFP/ME after the draws unless rdna4-gfxpark=0
	void gfxPark();
	char gfxVerdict[3072] {};
	uint32_t gfxVerdictLen { 0 };
	bool gfxVerdictFull { false };
	uint32_t gfxClipEq { 0 }, gfxClipCmp { 0 }, gfxClipSw { 0 };   // the last clip-state readback summary
	uint32_t gfxNggVid { 0 }, gfxNggPos { 0 }, gfxNggV0 { 0 };    // the last NGG VGPR variant verdict
	void gfxLinuxDiff(const char *tag);      // W42: the VM/queue/RLC/CP registers next to what Linux 7.2.2 reads on this card
	void gfxClearStatePre();                 // W39: clear-state registers before the first CSB replay (CP view + MMIO)
	void gfxEmitCsbReplay();                 // the clear-state extents as SET_CONTEXT_REG on the ring before the draw
	void gfxContextDump(const char *tag);    // MMIO read of the clear-state registers and their neighbours
	void gfxQueueEvidence(const char *tag);  // MQD / HQD / RS64 local-base registers the kext never programs
	void gfxMapVa0();                        // VMID0 VA 0 -> a private zeroed page (own opt-in rdna4-gfxva0=1), flushes the GC TLB
	void gfxDumpVa0(const char *tag);        // what the CP wrote at VA 0
	void gfxPstatReport(const char *label);  // deltas of the two SAMPLE_PIPELINESTAT buffers
	static bool requestedGfxVa0();           // rdna4-gfxva0=1: map VMID0 VA 0 (own opt-in since W41, not part of rdna4-gfxprobe)
	static bool requestedGfxSrm();           // rdna4-gfxsrm=0 turns SRM off (A/B control)
	static bool requestedGfxCsbReplay();     // rdna4-gfxcsb=0 turns the replay off (A/B control)
	struct GfxDrawResult {
		bool ok, ringDone;
		uint32_t drawFence, covered, other, firstNonZero, marker;
		uint32_t cInv, cPrim, ps;              // W45: pipeline statistics deltas of this draw (probe boots)
		uint32_t nggMarker, nggS2, nggS3;      // W37: the NGG marker variant (bit 32)
		uint32_t minX, maxX, minY, maxY, row64[2], row190[2];
		uint64_t ns;
		bool probeSeen;                         // rdna4-gfxprobe: the CP-side readback ran for this draw
		uint32_t probeEqual, probeCounted;      // 'probe mid': registers equal to the stream / registers compared
	};
	bool gfxDrawRun(const char *label, uint32_t variant, const uint64_t *va, GfxDrawResult &r);
	// G4: the colour triangle (vertex 0 red, 1 green, 2 blue through the attribute ring), after a passing G3 baseline, rdna4-gfxcol=1.
	struct GfxColResult {
		bool ok, ringDone;
		uint32_t drawFence, covered, badAlpha, notDominant, maxChannelErr256, maxSumErr;   // the check of userspace/gfx12tricol.h
		uint32_t minX, maxX, minY, maxY, near0, near1, near2, centroid;
		uint32_t cInv, cPrim, ps;              // pipeline statistics deltas (probe boots)
		uint64_t ns;
	};
	static uint32_t requestedGfxCol();       // rdna4-gfxcol=1 (default off): draw G4 after a passing G3 baseline
	bool gfxColDrawRun(const char *label, const uint64_t *va, GfxColResult &r);
	void gfxVerdictCol(const GfxColResult &r);
	void gfxCountTarget(GfxDrawResult &r);
	void gfxEvidence(const char *tag, bool state);
	void gfxGoldenInit();
	static uint32_t requestedGfxDiag();
	static uint32_t requestedGfxProbe();     // rdna4-gfxprobe=1: read the CP's own view of the state (COPY_DATA)
	void gfxEmitProbe(uint32_t poolOff);
	void gfxProbeReport(const char *label, uint32_t poolOff, volatile uint32_t *ib, uint32_t *equal = nullptr,
	                    uint32_t *counted = nullptr);
	void gfxPacketProbe();                   // W33 S1: NOP / WRITE_DATA / RELEASE_MEM / ACQUIRE_MEM alone, a fault mark after each
	bool gfxSentinelCheck();   // false only when its submission did not finish                 // W33 S2: does a CP-side read observe a context write made just before it?
	void gfxFaultMark(const char *tag);      // GC hub fault status: log and clear, to pin the step that faults
	void gfxRs64Evidence(const char *tag);   // PFP/ME/MEC data-cache and instruction-cache base registers
	Pm4::Queue gfxRing;
	uint32_t   gfxFence { 0 };               // last RELEASE_MEM sequence number
	uint32_t   gfxMode { 0 };                // 0 off, 2 doorbell (the only mode)
	static uint32_t requestedGfx();
	// W19: rdna4-gfxpm, the GFX power-management experiment.
	static constexpr uint32_t kPmWorkload = 1, kPmSoftAuto = 2, kPmCapProbe = 4, kPmSampleOnly = 8,
	                               kPmSurvey = 16;      // W24: engine busy/CG survey at each bring-up stage
	static constexpr uint32_t kGfxCapMinMHz = 200, kGfxCapMaxMHz = 5000;
	static constexpr uint32_t kPmSoftMaxAuto = 0xffff;         // (PPCLK_GFXCLK << 16) | 0xffff
	static constexpr uint32_t kPmNoMin = 0xffffffffu;          // leave SoftMin alone
	static constexpr uint32_t kPmProbeMHz = 1000;
	static uint32_t requestedGfxPm();
	static bool requestedGfxCap(uint32_t &mhz);      // rdna4-gfxcap: false when absent
	void gfxPmSurvey(const char *tag);
	void gfxCapApply(uint32_t mhz);
	// W27: rdna4-gfxoff (GFXOFF allowed at the very end of bring-up, guarded on every
	// GC access) and rdna4-gfxcg (clock gating), see the comments in compute.cpp.
	// kGcHold: GFXOFF could not be lifted (the SMU did not answer). Every GC access is
	// dropped for the rest of the boot: rd() reads kBad, wr() and doorbells do nothing.
	static constexpr SInt32 kGcOn = 0, kGcAllowing = 1, kGcOff = 2, kGcHold = 3;
	struct GcAccess {
		const RDNA4Compute &c;
		bool ok { true };            // false: the access must be dropped
		bool counted { false };
		explicit GcAccess(const RDNA4Compute &comp, uint32_t what);   // what: (seg << 16) | dword, or 0xdb000000 | doorbell dword
		~GcAccess();
	};
	mutable volatile SInt32 gcState { 0 };      // kGcOn until the end-of-bring-up Allow
	mutable volatile SInt32 gcBusy { 0 };       // GC accessors inside an access
	mutable thread_t gcOwner { nullptr };       // the thread that is restoring GC state after a GFXOFF wake
	bool bootQueueLive { false };               // stage 5's plain HQD is up ...
	uint32_t bringupGen { 0 };                  // ... in THIS bring-up: bumped at every runStages start
	uint32_t bootQueueGen { 0 };
	static constexpr uint32_t kGcSnapMax = 16;
	uint32_t gcSnapVal[kGcSnapMax] {};          // GC registers we program by MMIO, read just before AllowGfxOff
	bool gcSnapValid { false };
	IOLock *gcLock { nullptr };
	IOLock *smuLock { nullptr };                // serializes the SMU mailbox
	void gcWake(uint32_t what) const;
	void gfxOffAfterWake(uint32_t what);
	bool gcEnsureAwake(uint32_t what);          // wake GFX before a submitter emits into pm4Queue; false = still held
	void gfxOffSnapshot();
	uint32_t gfxOffRestoreRegs();
	bool gfxOffAllowedNow(const char **why);
	// Persistent "GFXOFF may be allowed" flag in NVRAM (set before AllowGfxOff, cleared
	// after a successful DisallowGfxOff), read at the next start.
	static bool gfxOffFlagGet();
	static bool gfxOffFlagSet(bool set);         // true when the read-back agrees
	static bool runsUnderHypervisor();            // CPUID.1:ECX[31]: the emulated device runs in a VM
	static uint32_t gfxOffHook();                 // rdna4-gfxoff=2/3, honoured only under a hypervisor
	void gfxOffPreflight(bool attach);
	bool gfxOffAllow();
	void gfxOffProbe();
	static bool requestedGfxOff();
	static uint32_t gfxOffMode();               // rdna4-gfxoff value; 2 = test hook: only leave the NVRAM flag set
	static constexpr uint32_t kCgStepCoarse = 1, kCgStepMedium = 2, kCgStepFine = 4, kCgStepGuiIdle = 8;
	static bool requestedGfxCg(uint32_t &mask);       // default 15 (amdgpu's late init); rdna4-gfxcg=0 disables
	static bool gfxCgIsDefault();                      // no rdna4-gfxcg boot-arg
	bool rlcSafeMode(bool enter);
	void gfxCgRmw(const GfxReg::Reg &r, const char *name, uint32_t clear, uint32_t set);
	void gfxCgCoarse(bool enable);
	void gfxCgMedium(bool enable);
	void gfxCgFine(bool enable);
	void gfxCgGuiIdle(bool enable);
	void gfxCgApply(uint32_t mask);
	void gfxPmSample(const char *tag);
	bool gfxPmSoftLimits(uint32_t maxParam, uint32_t minParam, const char *what);
	void gfxPmExperiment(uint32_t mask);
	bool gfxPmRestoreAuto(const char *why);
	void gfxPmRecoverCap();
	bool gfxCsbInit();
	bool gfxRingResume();
	void gfxKick(uint64_t wptrDwords);
	bool gfxFenceWait(uint32_t seq, uint32_t timeoutUs);
	void gfxStatus(const char *tag);
	bool stageGfxRing();
	// IH v7 ring and MSI delivery. The ring is brought up only after the
	// runtime's DMA path has established bus mastering; stage bring-up keeps
	// its existing bounded polling waits.
	static constexpr uint32_t kIhRingBytes = 256u << 10;
	static constexpr uint32_t kIhWptrBytes = 4096;
	IOBufferMemoryDescriptor *ihRingMemory { nullptr };
	IOBufferMemoryDescriptor *ihWptrMemory { nullptr };
	IODMACommand *ihRingDma { nullptr };
	IODMACommand *ihWptrDma { nullptr };
	volatile uint32_t *ihRingCpu { nullptr };
	volatile uint32_t *ihWptrCpu { nullptr };
	uint64_t ihRingBus { 0 };
	uint64_t ihWptrBus { 0 };
	uint32_t ihRptr { 0 };
	uint32_t ihRingMask { kIhRingBytes - 1 };
	IOFilterInterruptEventSource *ihSource { nullptr };
	IOWorkLoop *ihWorkLoop { nullptr };
	OSObject *ihContext { nullptr };
	IOLock *ihLock { nullptr };             // independent of rtLock; kept until object destruction
	void *ihWaitEvent { nullptr };
	bool ihActive { false };
	bool ihDispatchPolling { false };
	bool ihSdmaPolling { false };
	uint32_t ihDispatchMisses { 0 };
	uint32_t ihSdmaMisses { 0 };
	uint32_t ihDispatchObserved { 0 };
	uint32_t ihSdmaObserved { 0 };
	uint32_t ihDispatchWakeups { 0 };
	uint32_t ihSdmaWakeups { 0 };
	uint32_t ihEopCount { 0 };
	uint32_t ihSdmaTrapCount { 0 };
	uint64_t ihLastInterruptLog { 0 };
	uint32_t ihInterruptLogCount { 0 };   // lines allowed so far (see ihInterruptLogAllowed)
	uint32_t ihFaultCount { 0 };
	uint32_t ihUnknownCount { 0 };
	uint8_t ihUnknownSeen[256][32] {};
	bool ihDcnRequested { false };
	bool ihVblRequested { false };
	bool ihDcnActive { false };
	uint8_t ihDcnOtg { Pipe::kNone };
	uint8_t ihDcnHubp { Pipe::kNone };
	uint64_t ihDcnExpectedFrameNs { 0 };
	uint32_t ihDcnVblankFrames { 0 };
	uint32_t ihDcnStorms { 0 };
	// OTG_FRAME_COUNT (DCN 4.1.0, base 2, 0x1b4d) distinguishes many
	// interrupts in one raster frame from entries drained late by ihAction.
	uint32_t ihDcnFrameCounter { 0 };
	uint32_t ihDcnFrameEvents { 0 };
	uint32_t ihDcnStormFrames { 0 };
	uint32_t ihDcnShortIntervals { 0 };
	bool ihDcnFrameCounterValid { false };
	uint64_t ihDcnLastIvTimestamp { 0 };
	uint64_t ihDcnExpectedIvTicks { 0 };
	uint32_t ihDcnIvShortIntervals { 0 };
	bool ihDcnIvTimestampValid { false };
	uint64_t ihVblankCount[Pipe::kMaxOtg] {};
	uint64_t ihVblankTime[Pipe::kMaxOtg] {};
	uint64_t ihPflipCount[Pipe::kMaxOtg] {};

	bool ihInit();
	void ihStop();
	void ihDcnStop(const char *why);
	void ihDcnStopLocked(const char *why);
	void ihDcnAckVblank();
	void ihDcnAckFlip();
	void ihDcnObserveVblank(uint64_t now, uint64_t ivTimestamp);
	void ihPublishResult();
	bool ihInterruptLogAllowed();
	void ihDecodeEntry(const uint32_t *dw);
	void ihUnknown(uint8_t client, uint8_t source, uint8_t ring);
	void ihRecordWait(bool dispatch, bool slept, bool completed,
	                  bool recheckElapsed, uint32_t eventsBefore);
	bool ihWaitFence(volatile uint32_t *fence, uint32_t value, uint32_t timeoutMs,
	                bool dispatch, const char *tag, uint64_t &ns);

public:
	// User-space runtime (runtime.cpp), reached through RDNA4ComputeClient.
	// `owner` is the client: its buffers and programs are only its own, and
	// rtRelease frees them all. Every call takes rtLock.
	IOReturn rtOpen(const void *owner);
	IOReturn rtInfo(const void *owner, uint64_t out[9]);
	IOReturn rtSensors(const void *owner, RDNA4Sensors &out);
	IOReturn rtSensorsEx(const void *owner, RDNA4SensorsEx &out);
	IOReturn rtSleepTest(const void *owner, uint32_t phase);
	IOReturn rtQuiesce(const void *owner);
	IOReturn rtAlloc(const void *owner, uint64_t bytes, uint64_t &handle, uint64_t &gpu);
	IOReturn rtAllocHost(const void *owner, task_t task, uint64_t bytes, uint64_t flags,
	                     uint64_t &handle, uint64_t &gpu, uint64_t &user);
	IOReturn rtFree(const void *owner, uint64_t handle);
	IOReturn rtCopy(const void *owner, uint64_t handle, uint64_t offset, task_t task,
	                mach_vm_address_t user, uint64_t length, bool toGpu);
	IOReturn rtLoad(const void *owner, task_t task, mach_vm_address_t elf, uint64_t length,
	                const char *name, uint64_t out[8]);
	IOReturn rtUnload(const void *owner, uint64_t program);
	IOReturn rtDispatch(const void *owner, const RDNA4Dispatch &d, uint64_t &micros);
	IOReturn rtSubmitIb(const void *owner, uint64_t ibVa, uint64_t dwords, uint64_t flags,
	                    uint64_t &fence);
	IOReturn rtWaitFence(const void *owner, uint32_t fence, uint32_t timeoutMs, uint64_t &ns);
	// W12k: a client's own gfx IB on the kernel's gfx ring (runtime.cpp, docs/w12k-gfx-submit.md)
	IOReturn rtSubmitGfxIb(const void *owner, uint64_t ibVa, uint64_t dwords, uint64_t flags, uint64_t &fence);
	IOReturn rtWaitGfxFence(const void *owner, uint32_t fence, uint32_t timeoutMs, uint64_t &ns);
	IOReturn rtPresent(const void *owner, uint64_t handle, uint64_t offset,
	                   uint64_t &geometry, uint64_t &pitch);
	IOReturn rtPresentAsync(const void *owner, uint64_t handle, uint64_t offset,
	                        uint64_t &presentId);
	IOReturn rtWaitPresent(const void *owner, uint64_t presentId, uint32_t timeoutMs,
	                       uint64_t &frame);
	IOReturn rtRestore(const void *owner);
	void     rtRelease(const void *owner);

	// The Vulkan interface (src/n48nkext.cpp, docs/vulkan-port.md): one client at a time, in an address space of
	// its own. `owner` is its user client.
	IOReturn n48nOpen(const void *owner);
	void     n48nClose(const void *owner);
	IOReturn n48nCall(const void *owner, uint32_t selector, IOExternalMethodArguments *args);
	IOReturn n48nMemory(const void *owner, uint32_t handle, IOMemoryDescriptor **memory);

private:
	friend struct N48nBackend;
	struct N48nState;
	N48nState *n48n { nullptr };
	void     n48nDiscard();        // the card lost its memory under the client (sleep): forget it, write nothing
	IOReturn n48nCopyTest(uint64_t source, uint64_t destination, uint64_t bytes);
	// The pool past kHeapOffset is the CPU-visible heap (code, and buffers
	// when there is no DMA). With DMA, buffers come from the device heap:
	// VRAM past the BAR, which the CPU never touches.
	static constexpr uint32_t kHeapOffset  = 32u << 20;
	static constexpr uint32_t kMaxBuffers  = 256;          // the legacy global caps (modes 0 and 1)
	static constexpr uint32_t kMaxPrograms = 32;
	static constexpr uint32_t kBufferSlots  = 1024;        // rdna4-vmshared=2: array sizes; the cap is per client (accounting), not global
	static constexpr uint32_t kProgramSlots = 256;
	static constexpr uint32_t kClientBuffers  = 128;
	static constexpr uint32_t kClientPrograms = 16;
	uint32_t bufferCap() const { return vmShared == 2 ? kBufferSlots : kMaxBuffers; }
	uint32_t programCap() const { return vmShared == 2 ? kProgramSlots : kMaxPrograms; }
	bool clientOverQuota(const void *owner, bool program) const;   // rdna4-vmshared=2: per-client buffer/program counts
	static constexpr uint32_t kMaxClients = 8;              // the per-client-HQD path and rdna4-vmshared=1 (static VMIDs 8-15)
	static constexpr uint32_t kClientSlots = 64;            // rdna4-vmshared=2 (VMIDs from the pool): the array size, bounded by memory not hardware
	uint32_t clientCap() const { return vmShared == 2 ? kClientSlots : kMaxClients; }
	static constexpr uint32_t kVmTableBytes = 4u << 20;
	static constexpr uint32_t kVmTableStage = 20u << 20;
	static constexpr uint32_t kVmTableCpu = 28u << 20;    // W22 variant T: CPU-written tables (0x4000)
	uint64_t vmPteSet { 0 }, vmPteClear { 0 };            // W22 diagnostics: extra/removed leaf PTE bits
	bool     vmExecOff { false };                         // rdna4-vm-exec=0: leaves not EXECUTABLE (negative control)
	bool     vmIsPteOff { false };                        // rdna4-vm-ispte=0: leave bit 63 off leaf PTEs (negative control)
	uint32_t vmTableCpu { 0 };                            // W22 variant T: pool offset of CPU-written tables
	static constexpr uint32_t kVmQueueBase = 26u << 20;
	static constexpr uint32_t kVmQueueStride = 0x10000;
	// rdna4-vmshared=2 keeps a client's kernarg, fence word and IB page in a compact 12 KiB pool area, not a 64 KiB slot; poolOff is set so
	// that poolOff + kVmFence / kVmKernarg / kVmIb land on them. The area must end below the gfx region (PtPages::areaFor).
	static constexpr uint32_t kVmDynBase = kVmQueueBase + (16u << 16);
	static constexpr uint32_t kVmDynStride = 0x3000;
	static constexpr uint32_t kVmMqd = 0x0000;
	static constexpr uint32_t kVmEop = 0x1000;
	static constexpr uint32_t kVmPq = 0x2000;
	static constexpr uint32_t kVmRptr = 0x3000;
	static constexpr uint32_t kVmWptr = 0x4000;
	static constexpr uint32_t kVmFence = 0x5000;
	static constexpr uint32_t kVmKernarg = 0x6000;
	static constexpr uint32_t kVmIb = 0x7000;                 // shared mode: the client's IB page
	static constexpr uint64_t kHostMaxBuffer = 256ull << 20;
	static constexpr uint64_t kHostMaxClient = 1ull << 30;
	static constexpr uint64_t kHostMaxTotal = 4ull << 30;
	static constexpr uint32_t kMaxIbOutstanding = 16;
	struct RtBuffer  {
		const void *owner { nullptr };
		uint64_t    offset { 0 }, bytes { 0 }; // heap offset (pool or VRAM), size
		uint64_t    mc { 0 }, va { 0 };        // physical MC and client GPU VA
		uint16_t    gen { 0 };
		bool        device { false };          // from the device heap
		bool        host { false };
		IOBufferMemoryDescriptor *hostMemory { nullptr };
		IODMACommand *hostDma { nullptr };
		IOMemoryMap *hostMap { nullptr };
		uint64_t    hostUser { 0 };
	};
	struct RtProgram { const void *owner; uint64_t offset, va; CodeObj::Kernel k; uint16_t gen; };
	// Sparse page tables (rdna4-vmshared=2; ptpages.hpp, gpuvmtable.hpp): root, PDB1, PDB0 and PT pages exist only where VA is mapped, so a client's VA
	// reaches as far as its page quota allows (the contiguous image of modes 0/1 stops at 1 GiB above kVaStart).
	struct SparseTables {
		PtPages::Sparse s;
	};
	static constexpr uint32_t kFaultRedirects = 96;        // pages one job may fault on and still finish
	struct RtClient {
		const void *owner { nullptr };
		uint32_t vmid { 0 }, pipe { 0 }, queue { 0 };
		uint64_t tableOffset { 0 }, rootMc { 0 }, rootPhys { 0 }, nextVa { GpuVm::kVaStart };
		uint64_t kernargVa { 0 }, fenceVa { 0 };
		uint64_t queueVa { 0 }, mqdMc { 0 }, eopVa { 0 }, rptrVa { 0 }, wpollVa { 0 };
		volatile uint32_t *queueCpu { nullptr };
		uint64_t hostBytes { 0 };
		volatile uint32_t *kernargCpu { nullptr };
		volatile uint32_t *fenceCpu { nullptr };
		uint32_t fence { 0 }, doorbell { 0 };
		SparseTables *sp { nullptr };   // rdna4-vmshared=2: demand-allocated page tables (ptpages.hpp) instead of tableShadow
		uint64_t tlbSeq { 0 };            // bumped whenever a PTE is removed: the next job on a VMID owned by this client flushes (amdgpu's tlb_seq)
		uint32_t pasid { 0 };             // what the IH_VMID_LUT of this client's VMID holds, so a fault names the client (slot + 1)
		bool     shared { false };        // rdna4-vmshared: no HQD of its own; its jobs run on a shared VMID-0 queue
		uint8_t  sq { 0 };                // which shared queue (fixed at open: a client's jobs stay in order)
		uint32_t poolOff { 0 };           // this client's pool slot area (kVmQueueBase + slot * stride)
		uint64_t ibVa { 0 };              // shared mode: the kernel-written IB page, mapped executable in the client's VM
		uint32_t ibFences[kMaxIbOutstanding] {};
		uint32_t ibOutstanding { 0 };
		// W12k: this client's gfx fence: a dword of its own in the fence page (+0x40; the gfx ring writes it at the VMID0 MC
		// address gfxFenceMc, the client can read it at fenceVa + 0x40), the last value handed out, and the ones not yet retired.
		volatile uint32_t *gfxFenceCpu { nullptr };
		uint64_t gfxFenceMc { 0 };
		uint32_t gfxFence { 0 };
		uint32_t gfxFences[kMaxIbOutstanding] {};
		uint32_t gfxOutstanding { 0 };
		uint64_t *tableShadow { nullptr };
		Pm4::Queue pm4;
		// Pages of this address space that hold the dummy page while a job runs, because a shader touched them
		// unmapped (vmRedirectFault); taken out again when the job is over.
		uint64_t faultVa[kFaultRedirects] {};
		uint32_t faultCount { 0 };
		bool active { false };
		bool aborted { false };
	};
        IOLock        *rtLock { nullptr };
        IONotifier   *shutdownInterest { nullptr };
        bool           shutdownQuiesced { false };
	bool           bringupRunning { false };
	bool           rtReady { false };       // a dispatching stage finished
	bool           rtWedged { false };      // a dispatch timed out
	bool           powerSleeping { false };
	bool           resumed { false };
	bool           resumePending { false };
	bool           hangRecoveryEnabled { false }; // opt in with rdna4-hang=1
	uint32_t       rtStage { 0 };
	bool           vmEnabled { false };
	bool           vmidUsed[16] {};
	bool           queueUsed[4][8] {};
	RtClient       clients[kClientSlots] {};
	GpuHeap::Heap  heap;
	uint8_t        heapMap[(128u << 20) / 4096] {};   // pool heap: 4 KiB granules
	RtBuffer       buffers[kBufferSlots] {};
	uint64_t       hostBytesTotal { 0 };
	RtProgram      programs[kProgramSlots] {};
	bool           presentActive { false };
	const void    *presentOwner { nullptr };
	uint64_t       presentHandle { 0 };
	uint64_t       presentOffset { 0 };
	uint64_t       presentStarted { 0 };
	Flip::Surface  presentSurface {};
	static constexpr uint32_t kPresentSlots = 4;
	struct PresentSlot {
		const void *owner { nullptr };
		uint64_t handle { 0 }, offset { 0 }, id { 0 }, frame { 0 };
		IOReturn result { kIOReturnSuccess };
		uint8_t state { 0 };                 // 0 free, 1 pending, 2 completed
	};
	PresentSlot  presentSlots[kPresentSlots] {};
	uint32_t     presentPending { 0 };
	uint64_t     nextPresentId { 1 };
	uint32_t     presentNoVblankTicks { 0 };
	IOTimerEventSource *presentTimer { nullptr };
	IOWorkLoop        *presentWorkLoop { nullptr };
	OSObject          *presentContext { nullptr };
	static void presentTimerAction(OSObject *owner, IOTimerEventSource *timer);
	bool     initPresentationTimer();
	void     stopPresentationTimer();
	void     schedulePresentationTimer();
	void     schedulePresentationRetry();
	void     presentTimerTick();
	// P2 idle accounting (rdna4-gfxidle=1) and P7 sleep/wake hardening (docs/power-gfx.md): src/pmidle.cpp.
	// Idle accounting is software only: it counts synchronous client operations (IdleUse, under rtLock) and looks at fences that are still
	// outstanding; it sends no SMU message and touches no GC register. Its state is a log line per transition and the property Compute,GFXIdle.
	struct IdleUse {
		RDNA4Compute *c;
		IdleUse(RDNA4Compute *self, const char *what) : c(self) { c->idleBegin(what); }
		~IdleUse() { c->idleEnd(); }
	};
	static bool requestedGfxIdle();            // rdna4-gfxidle=1
	static bool requestedSleepAbort();         // default ON; rdna4-sleepabort=0 restores waits that hold rtLock until their timeout
	static bool requestedResumeTests();        // rdna4-resume-tests=1: the wake re-runs the G3/G4 draws, the gfx client self-test and the flip test
	void     idleStart();                      // bring-up finished: create the poll timer, start counting (idempotent)
	void     idleStop();                       // shutdown / sleep: cancel the timer
	void     idleBegin(const char *what);      // rtLock held
	void     idleEnd();                        // rtLock held
	void     idleTouchLocked(const char *what);
	void     idleEvaluateLocked();             // retire fences, then busy -> idle if nothing has used the GPU for 100 ms
	void     idleTick();                       // the timer's body: TryLock rtLock, evaluate, re-arm while busy
	void     idleReport(const char *why);      // registry property Compute,GFXIdle
	static void idleTimerAction(OSObject *owner, IOTimerEventSource *timer);
	bool     idleOn { false };
	bool     idleBusy { true };                // the accounting state: bring-up counts as busy
	uint32_t idleSync { 0 };                   // synchronous client operations in progress
	uint64_t idleLastUseAbs { 0 }, idleStateAbs { 0 }, idleBusyNs { 0 }, idleIdleNs { 0 };
	uint32_t idleTransitions { 0 };
	char     idleLastWhat[24] {};
	IOTimerEventSource *idleTimer { nullptr };
	IOWorkLoop         *idleWorkLoop { nullptr };
	OSObject           *idleContext { nullptr };
	// P7: set by the power callback BEFORE it takes rtLock; waits that hold rtLock poll it and return kIOReturnAborted.
	volatile uint32_t sleepRequested { 0 };
	bool     sleepAbortOn { true };
	bool     waitAborted { false };            // the last wait ended because of sleepRequested (rtLock held by the waiter)
	bool     sleepAbortWanted() const { return sleepAbortOn && __atomic_load_n(&sleepRequested, __ATOMIC_ACQUIRE) != 0; }
	void     powerSleepClear();                // the wake finished (or was cancelled)
	void     gfxSleepDrain();                  // powerWillSleep, rtLock held: wait <= 100 ms for client gfx IBs, then drop them (no wedge)
	PresentSlot *presentSlot(uint64_t id, const void *owner);
	void     completePresentLocked(PresentSlot &slot, IOReturn result, uint64_t frame);
	void     dropPendingPresentsLocked(IOReturn result);
	void     checkPresentationTimeoutLocked();
	IOReturn restorePresentationLocked(const char *why);
	void     clearPresentationLocked();
	IOService     *rtService { nullptr };
	uint64_t       dmubVram { 0 };            // DMUB memory (VRAM offset), from choosePool
	void publishRuntime(uint32_t stage);
	bool initRuntimeHeap();
	bool vmBootSelfTest();
	bool vmSharedBootTest();           // the same proof without a queue inside a client address space (vmshared.cpp)
	// Sparse page tables (rdna4-vmshared=2): a multi-level tree, pages backed on demand.
	static constexpr uint32_t kSparseShadowMax = 8192;        // wired 4 KiB shadow pages over all clients (32 MiB)
	uint32_t sparseShadowPages { 0 };
	static bool sparseAllocChunk(void *ctx, uint64_t &off);
	static void sparseFreeChunk(void *ctx, uint64_t off);
	PtPages::Backend sparseBackend() { return PtPages::Backend { sparseAllocChunk, sparseFreeChunk, this }; }
	static uint64_t *sparseAllocShadow(void *ctx);
	static void sparseFreeShadow(void *ctx, uint64_t *page);
	static bool sparsePhysOf(void *ctx, uint64_t heapOffset, uint64_t &physical);
	PtPages::Host sparseHost() { return PtPages::Host { sparseBackend(), sparseAllocShadow, sparseFreeShadow, sparsePhysOf, this }; }
	struct TreeCtx { RDNA4Compute *self; RtClient *c; };
	static bool treePageThunk(void *ctx, uint32_t level, uint64_t key, bool create, uint64_t *&entries, uint64_t &phys, uint32_t &id);
	static void treeDirtyThunk(void *ctx, uint32_t id);
	bool sparseSync(RtClient &c);      // every page whose shadow changed goes to its VRAM page
	uint64_t *ptEntry(RtClient &c, uint64_t off);
	struct PtCtx { RDNA4Compute *self; RtClient *c; };
	static uint64_t *ptEntryThunk(void *ctx, uint64_t off);
	static bool ptPhysThunk(void *ctx, uint64_t off, uint64_t &phys);
	GpuVmTable::Policy vmPolicy() const { return GpuVmTable::Policy { vmIsPteOff, vmExecOff, vmPteSet, vmPteClear }; }
	bool ptPhys(RtClient &c, uint64_t off, uint64_t &phys);
	bool hasTables(const RtClient &c) const { return c.tableShadow || c.sp; }
	bool sparseOpen(RtClient &c, uint32_t quotaPages);
	void sparseTeardown(RtClient &c, bool clearVram);
	// W13 S7-lite (docs/w13-vmid.md): with rdna4-vmshared=1 clients own no HQD. Two kernel-owned MEC queues (VMID 0, PRIV_STATE|KMD_QUEUE,
	// one per MEC pipe) run every client job as INDIRECT_BUFFER(vmid = the client's) followed by a ring-level fence, as amdgpu's kernel
	// compute rings do. Clients keep a static VMID 8-15. Default off; the per-client-HQD path is unchanged when off.
	struct SharedQueue {
		bool up { false }, wedged { false };
		uint32_t pipe { 0 }, queue { 0 }, doorbell { 0 }, area { 0 };   // area: pool offset of MQD/EOP/PQ/rptr/wptr pages
		Pm4::Queue pm;
		// rdna4-vmshared=2: a per-queue fence (amdgpu's per-ring fence) every job also writes, and the ring space the jobs in flight occupy
		volatile uint32_t *fenceCpu { nullptr };
		uint64_t fenceMc { 0 };
		uint32_t seq { 0 };
		struct Job { uint32_t seq, dwords; };
		Job      jobs[64] {};
		uint32_t jobHead { 0 }, jobCount { 0 }, ringUsed { 0 };
	};
	static constexpr uint32_t kSharedQueues = 2;
	SharedQueue sharedQ[kSharedQueues] {};
	uint32_t vmShared { 0 };          // rdna4-vmshared: 0 off, 1 static VMIDs 8-15, 2 VMIDs from the pool per job
	bool sharedInit { false };
	Vmid::Pool vmPool;
	static uint32_t requestedVmShared();
	bool sharedFenceReached(uint32_t domain, uint32_t seq);
	int vmClientSlotByPasid(uint32_t pasid) const;     // rdna4-vmshared=2: which client a fault vector's PASID names (-1: none); a racy read, for logs only
	static bool poolFenceReached(void *context, uint32_t domain, uint32_t seq);
	bool sharedReserve(uint32_t k, uint32_t dwords);
	void sharedCommit(uint32_t k, uint32_t seq, uint32_t dwords);
	uint32_t vmAcquire(RtClient &c);                 // a VMID bound to the client's tables, rebound/flushed as needed; 0 = none available
	void vmReleaseVmids(RtClient &c);                // client closes: every VMID it owns goes back, contexts off
	IOReturn rtOpenPooled(const void *owner, uint32_t slot, RtClient *c);
	bool vmSharedEnsure();
	IOReturn rtOpenShared(const void *owner, uint32_t slot, RtClient *c);
	IOReturn rtOpenInner(const void *owner);
	bool sharedStart(uint32_t k);
	void sharedStopAll(const char *why);
	bool recoverSharedQueue(uint32_t k, uint32_t guiltyVmid, const char *tag);
	// The VMID a client's next submission runs in. One place: the client's fixed VMID (modes 0 and 1), or, with rdna4-vmshared=2, a VMID the
	// pool binds to the client's tables for this job (vmAcquire: reused, or an idle one rebound over MMIO). 0 = none available (the caller
	// answers Busy). W12k's gfx submit takes the IB's VMID from here; the compute paths acquire explicitly (they reserve ring space first) and
	// read c.vmid afterwards.
	uint32_t vmidForSubmit(RtClient &c) { return c.shared && vmShared == 2 ? vmAcquire(c) : c.vmid; }
	static constexpr uint32_t kGfxDomain = 2;   // the VMID pool's fence domain for the gfx ring (0 and 1 are the shared compute queues)
	static bool requestedVmIdTest();
	static uint32_t vmIdTestMask();          // rdna4-vmid-test: 1 probes, 2 flow-point surveys, 4 client-op trace
	void vmIdSurvey(const char *tag, uint32_t settleMs = 0);
	void vmOpTrace(const char *op, uint32_t vmid, uint32_t pipe, uint32_t queue);
	void vmSurvey(const char *tag, uint32_t settleMs = 0) { if (vmSurveyOn) vmIdSurvey(tag, settleMs); }
	void vmIdTest(bool late);                 // late = the probes that repeat after clock gating (mask bit 8)
	// Compact one-line entries for the registry (properties Compute,VMSurvey and Compute,VMOps), " ## " separated, bounded, like the gfx verdict:
	// the dmesg window loses bring-up lines, the registry does not.
	void vmRegistryAdd(const char *prop, char *buf, size_t cap, uint32_t &len, bool &full, const char *entry);
	bool     vmSurveyOn { false };
	bool     vmOpTraceOn { false };
	uint32_t vmOpTraceLines { 0 };        // runtime clients (boot self-tests, diagnostic-log clients, the user client)
	uint32_t vmOpProbeLines { 0 };        // the S1 probes of vmIdTest: their own budget, so they cannot use up the clients' (hub-task-347)
	bool     vmOpInProbe { false };
	bool     vmSurveyClientDone { false };
	bool     vmSurveyDispatchDone { false };
	bool     vmIdShaderHung { false };        // an early S1 shader probe hung: the late ones are skipped
	char     vmSurveyBuf[3072] {};
	uint32_t vmSurveyLen { 0 };
	bool     vmSurveyFull { false };
	char     vmOpsBuf[3072] {};               // Compute,VMOps: runtime clients
	uint32_t vmOpsLen { 0 };
	bool     vmOpsFull { false };
	char     vmProbeOpsBuf[1536] {};          // Compute,VMProbeOps: the S1 probes' own operations
	uint32_t vmProbeOpsLen { 0 };
	bool     vmProbeOpsFull { false };
	void vmDumpHubWindows(const char *tag);   // W17 E1, read-only
	RtBuffer  *bufferFor(const void *owner, uint64_t handle);
	RtProgram *programFor(const void *owner, uint64_t handle);
	RtClient  *clientFor(const void *owner);
	RtClient  *vmClientFor(const void *owner);
	IOReturn ownerStateLocked(const void *owner) const;
	bool vmTableSync(RtClient &client, uint32_t offset, uint32_t bytes);
	bool vmMap(RtClient &client, uint64_t va, uint64_t mc, uint64_t bytes, bool executable);
	bool vmMapHost(RtClient &client, uint64_t va, const uint64_t *pageBuses,
	               uint64_t bytes, bool executable);
	void vmUnmap(RtClient &client, uint64_t va, uint64_t bytes);
	bool vmContextInit(RtClient &client);
	bool vmInvalidate(uint32_t vmid, const char *tag);
	bool vmInvalidateOwned(RtClient &c, const char *tag);
	void logClientFault(RtClient &client, const char *tag);
	void scrubFaultPage();
	void releaseHost(RtBuffer &buffer);
	void retireIbFences(RtClient &client);
	IOReturn submitIbLocked(RtClient &client, uint64_t ibVa, uint64_t dwords, uint64_t &fence);
	// W12k (runtime.cpp): the client side of the gfx ring. The kernel's own gfx users (stageGfxRing/stageGfxDraw, gfxPark) run on the
	// bring-up thread with bringupRunning set and do NOT take rtLock; client submissions take rtLock and refuse while bringupRunning,
	// so the two never use the ring at the same time. Everything below runs under rtLock.
	static constexpr uint32_t kMaxGfxOutstanding = 16;     // submissions in the ring across all clients (15 dwords each, 22 with the pool's ring fence: 352 of 4096)
	static constexpr uint32_t kGfxFenceSlot = 0x40;        // bytes into the client's fence page
	bool     gfxWedged { false };                          // a client gfx IB timed out: the gfx ring is given up until the next bring-up
	bool     gfxParked { false };                          // gfxPark halted PFP/ME (probe boots)
	uint32_t gfxClientPending { 0 };                       // client submissions not yet retired, all clients
	IOReturn gfxClientReady() const;                       // kIOReturnSuccess when a client may use the ring now
	void     gfxClientRetire(RtClient &client);
	IOReturn gfxClientEmit(RtClient &client, uint64_t ibVa, uint32_t dwords, uint32_t &fence);
	bool     gfxClientWait(RtClient &client, uint32_t fence, uint32_t timeoutMs, uint64_t &ns, const char *why);
	void     gfxClientWedge(const char *why);
	void     gfxClientDrain(RtClient &client, const char *why);
	void     gfxClientReset();                             // a fresh gfx ring: nothing is pending
	bool     gfxClientSelfTest();                          // rdna4-gfxclient=1 (bring-up thread)
	static bool requestedGfxClient();

	// DMA between host memory and VRAM (runtime.cpp). One pinned, physically
	// contiguous bounce buffer; the GC hub's AGP aperture maps system memory
	// up to its end (amdgpu's layout: MC = agpStart + bus address), and SDMA
	// copies through it. Proven by a read-then-write self-test before use.
	static constexpr uint32_t kBounceBytes    = 16u << 20;
	static constexpr uint32_t kDmaTestOffset  = 28u << 20;   // pool scratch, 128 KiB
	static constexpr uint32_t kDevHeapGranule = 64u << 10;
	IOBufferMemoryDescriptor *bounce { nullptr };
	IODMACommand  *bounceDma { nullptr };
	uint8_t       *bounceVa { nullptr };
	uint64_t       bounceBus { 0 };           // the device's address for it
	uint64_t       agpStart { 0 };            // MC address of bus address 0
	bool           dmaReady { false };
	bool           busMasterSet { false }, busMasterWas { false };
	GpuHeap::Heap  devHeap;                   // VRAM offsets
	uint8_t       *devHeapMap { nullptr };
	uint32_t       devHeapMapBytes { 0 };
	bool dmaInit();
	// The page of system memory faults are answered from, as amdgpu's dummy page (runtime.cpp, faultPageToSystem).
	IOBufferMemoryDescriptor *faultPage { nullptr };
	IODMACommand  *faultPageDma { nullptr };
	uint64_t       faultPageBus { 0 };
	void faultPageToSystem();
	bool vmRedirectFault();            // a client's shader waits on an unmapped page: the dummy page goes there
	void vmEndRedirects();             // the job is over: every such page is unmapped again
	void dmaTeardown(const char *why);
	void devHeapInit();
	bool sdmaRun(const uint32_t *pkt, uint32_t dwords, uint32_t timeoutMs);
	bool bounceCopy(uint64_t vramMc, uint32_t hostOffset, uint32_t bytes, bool toGpu);
	IOReturn dmaCopy(task_t task, mach_vm_address_t user, uint64_t mc, uint64_t length, bool toGpu);
	uint64_t vramMc(uint64_t vramOffset) const { return sv.fbMcBase + vramOffset; }
	bool gpuPhysical(uint64_t mc, uint64_t &physical) const {
		return GpuVm::mcToPhysical(mc, sv.fbMcBase, sv.gcFbOffset, physical);
	}
	// SMU mailbox (MP1): send one message, return the response code
	// (1 = OK, 0 = no answer) and the argument register after it.
	uint32_t smuSend(uint32_t msg, uint32_t param, uint32_t &ret, uint32_t timeoutMs);

	static uint32_t pspRead(void *ctx, uint32_t dword);
	static void     pspWrite(void *ctx, uint32_t dword, uint32_t value);
	static void     pspDelay(void *ctx, uint32_t us);
	static void     pspFlush(void *ctx);
};

#endif /* RDNA4Compute_hpp */
