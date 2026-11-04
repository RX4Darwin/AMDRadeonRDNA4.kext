/*
 *  rdna4pvjob.h
 *  AMDRadeonRDNA4
 *
 *  A compute dispatch of a Metal client on the paravirtual GPU (PVGPU
 *  builds), as the kext's host hands it to the compiler service in user
 *  space (userspace/rdna4-pvexec) through RDNA4ComputeClient: PvNext gives
 *  the next one, PvAccess reads and writes the client's memory by the
 *  task's GPU addresses, PvDone ends it. The host waits for the job before
 *  it completes the client's command buffer.
 *
 *  The function is the metallib the client compiled (one AIR module); the
 *  service compiles it for the threadgroup size, runs it over the grid with
 *  the buffers bound by slot, and writes the buffers back.
 */

#ifndef RDNA4PvJob_h
#define RDNA4PvJob_h

#include <stdint.h>

#define RDNA4_PVJOB_MAX_BUFFERS 31u          /* Metal's buffer slots */
#define RDNA4_PVJOB_MAX_CODE    (1u << 20)   /* metallib bytes */

typedef struct {
	uint32_t slot;          /* [[buffer(slot)]] */
	uint32_t reserved;
	uint64_t va;            /* the bound offset, in the task's GPU space */
	uint64_t length;        /* from va to the end of the buffer */
} RDNA4PvBuffer;

typedef struct {
	uint32_t id;
	uint32_t task;
	uint64_t codeVa;        /* the metallib, in the task's GPU space */
	uint32_t codeBytes;
	uint32_t reserved;
	uint32_t threads[3];    /* the grid, in threads */
	uint32_t groupSize[3];  /* threads per threadgroup */
	uint32_t bufferCount;
	uint32_t reserved2;
	RDNA4PvBuffer buffers[RDNA4_PVJOB_MAX_BUFFERS];
} RDNA4PvJob;

#ifdef __cplusplus
static_assert(sizeof(RDNA4PvJob) == 56 + RDNA4_PVJOB_MAX_BUFFERS * sizeof(RDNA4PvBuffer), "RDNA4PvJob layout");
#else
_Static_assert(sizeof(RDNA4PvJob) == 56 + RDNA4_PVJOB_MAX_BUFFERS * sizeof(RDNA4PvBuffer), "RDNA4PvJob layout");
#endif

#endif /* RDNA4PvJob_h */
