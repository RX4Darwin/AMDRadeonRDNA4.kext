/*
 *  rdna4compute.h
 *  RDNA4FB
 *
 *  The user-space compute interface: what a program passes to the kext's
 *  RDNA4ComputeClient through IOConnectCallMethod. Shared, unchanged, by the
 *  kext (src/userclient.cpp) and user space (userspace/librdna4.c), so it
 *  is plain C with fixed-width types.
 *
 *  The service appears once the bring-up reached stage 6 (rdna4-compute=6
 *  or 7): IOServiceMatching(RDNA4_COMPUTE_SERVICE). Opening it needs root —
 *  a kernel runs in VMID0 and can address all of VRAM.
 *
 *  Model (HSA-like, synchronous):
 *    Alloc      a VRAM buffer; its GPU address goes into kernel arguments.
 *    Write/Read copy between user memory and a buffer.
 *    Load       an AMDGPU code object (clang -mcpu=gfx1201, ld.lld -shared)
 *               and pick one kernel by name.
 *    Dispatch   run it over a 3-D grid with the caller's kernarg bytes, and
 *               wait for it (the timeout is the caller's). Each work-group
 *               gets the kernel's own LDS plus dynamicLdsBytes.
 *  Buffers and programs belong to the connection and are freed with it.
 *  With RDNA4_FLAG_DMA, Write/Read run on the GPU's copy engine (GB/s) and
 *  buffers come from VRAM past the BAR (gigabytes); without it, the CPU
 *  copies through the BAR and buffers share the 96 MiB CPU-visible heap.
 */

#ifndef RDNA4Compute_h
#define RDNA4Compute_h

#include <stdint.h>

#define RDNA4_COMPUTE_SERVICE   "RDNA4ComputeService"
#define RDNA4_COMPUTE_ABI       4u   /* 4: per-client GPUVM and one MEC queue per client */

/* Largest kernarg block a dispatch carries; bytes past what the caller
 * passes, up to the kernel's own kernarg size, are zero. */
#define RDNA4_MAX_KERNARG       2048u
#define RDNA4_MAX_NAME          64u        /* kernel name, NUL included */
#define RDNA4_MAX_CODE_OBJECT   (4u << 20) /* ELF bytes accepted by Load */
#define RDNA4_MAX_TIMEOUT_MS    10000u
#define RDNA4_MAX_LDS           65536u     /* per work-group: static + dynamic */

/* Selectors: scalar inputs -> scalar outputs, unless a struct is named. */
enum {
	/* -> abi, stage, flags, heap bytes, heap free, heap GPU/VA base, VMID, pipe, queue */
	kRDNA4MethodInfo = 0,
	/* bytes -> handle, GPU address (4 KiB aligned) */
	kRDNA4MethodAlloc,
	/* handle */
	kRDNA4MethodFree,
	/* handle, offset, user address, length: user memory -> buffer */
	kRDNA4MethodWrite,
	/* handle, offset, user address, length: buffer -> user memory */
	kRDNA4MethodRead,
	/* user address, length of the ELF; struct in: kernel name (NUL-terminated)
	 * -> program, kernarg bytes, image bytes, RSRC1, RSRC2, RSRC3, properties,
	 *    static LDS bytes */
	kRDNA4MethodLoad,
	/* program */
	kRDNA4MethodUnload,
	/* struct in: RDNA4Dispatch -> microseconds from doorbell to fence */
	kRDNA4MethodDispatch,
	/* timeout milliseconds -> vblank count, timestamp in nanoseconds */
	kRDNA4MethodWaitVBlank,
	/* handle, byte offset (0,0 queries geometry) -> width | height<<16, pitch */
	kRDNA4MethodPresent,
	/* restore the desktop surface for this connection */
	kRDNA4MethodRestore,
	/* bytes, flags -> handle, GPU VA, user CPU address */
	kRDNA4MethodAllocHost,
	/* -> RDNA4Sensors, the current SMU metrics snapshot */
	kRDNA4MethodSensors,
	/* debug-only root sleep cycle: 1 quiesce, 2 re-bring-up */
	kRDNA4MethodSleepTest,
	/* IB GPU VA, dwords, flags -> fence value */
	kRDNA4MethodSubmitIb,
	/* fence value, timeout milliseconds -> elapsed nanoseconds */
	kRDNA4MethodWaitFence,
	kRDNA4MethodCount
};

/* AllocHost flags.  Host memory is non-executable unless this bit is set. */
#define RDNA4_HOST_EXECUTABLE (1u << 0)

#define RDNA4_FLAG_READY   (1u << 0)   /* bring-up reached a dispatching stage */
#define RDNA4_FLAG_WEDGED  (1u << 1)   /* a dispatch timed out: no more work */
#define RDNA4_FLAG_DMA     (1u << 2)   /* Write/Read by SDMA; buffers from all of VRAM */
#define RDNA4_FLAG_VM      (1u << 3)   /* this client has a private GPU VM and queue */
#define RDNA4_FLAG_RESUMED (1u << 4)   /* runtime was re-published after system sleep */

/* A compact view of the SMU 14.0.2/14.0.3 metrics table. */
typedef struct {
	uint32_t edgeTempC, hotspotTempC;
	uint32_t gfxClockMHz, memoryClockMHz;
	uint32_t socketPowerW, fanRpm;
} RDNA4Sensors;

#ifdef __cplusplus
static_assert(sizeof(RDNA4Sensors) == 6 * sizeof(uint32_t), "RDNA4Sensors layout");
#else
_Static_assert(sizeof(RDNA4Sensors) == 6 * sizeof(uint32_t), "RDNA4Sensors layout");
#endif

typedef struct {
	uint32_t program;
	uint32_t groups[3];         /* work-groups per dimension, each >= 1 */
	uint32_t groupSize[3];      /* work-items per group; product <= 1024 */
	uint32_t timeoutMs;         /* 0 = 1000, at most RDNA4_MAX_TIMEOUT_MS */
	uint32_t kernargBytes;      /* valid bytes in kernargs */
	uint32_t dynamicLdsBytes;   /* LDS beyond the kernel's static size (ABI 1: reserved, 0) */
	uint8_t  kernargs[RDNA4_MAX_KERNARG];
} RDNA4Dispatch;

#ifdef __cplusplus
static_assert(sizeof(RDNA4Dispatch) == 40 + RDNA4_MAX_KERNARG, "RDNA4Dispatch layout");
#else
_Static_assert(sizeof(RDNA4Dispatch) == 40 + RDNA4_MAX_KERNARG, "RDNA4Dispatch layout");
#endif

#endif /* RDNA4Compute_h */
