/*
 *  librdna4.h
 *  RDNA4FB
 *
 *  A thin C API over RDNA4ComputeClient (include/rdna4compute.h) for macOS
 *  programs: open the service, allocate VRAM buffers, copy data, load an
 *  AMDGPU code object and dispatch its kernels. Needs root.
 *
 *    rdna4_t gpu;
 *    rdna4_open(&gpu);
 *    rdna4_buffer_t buf;  rdna4_alloc(&gpu, bytes, &buf);   // buf.gpu -> kernargs
 *    rdna4_write(&gpu, &buf, 0, host, bytes);
 *    rdna4_program_t k;   rdna4_load(&gpu, elf, elfBytes, "kernel", &k);
 *    rdna4_dispatch(&gpu, &k, groups, groupSize, &args, sizeof args, 1000, &us);
 *    rdna4_read(&gpu, &buf, 0, host, bytes);
 *    rdna4_close(&gpu);                                     // frees everything
 *
 *  Every call returns an IOKit status (kIOReturnSuccess = 0);
 *  rdna4_error() names it.
 */

#ifndef LibRDNA4_h
#define LibRDNA4_h

#include <stddef.h>
#include <stdint.h>

#include <IOKit/IOKitLib.h>

#include "rdna4compute.h"

typedef struct {
	io_connect_t conn;
} rdna4_t;

typedef struct {
	uint64_t abi, stage, flags;             /* flags: RDNA4_FLAG_* */
	uint64_t heapBytes, heapFree, heapBase;
	uint64_t vmid, pipe, queue;
} rdna4_info_t;

typedef struct {
	uint64_t handle;
	uint64_t gpu;                           /* GPU (MC) address, 4 KiB aligned */
	uint64_t bytes;
} rdna4_buffer_t;

typedef struct {
	uint64_t handle;
	uint64_t kernargBytes;                  /* the kernel's kernarg segment */
	uint64_t imageBytes;
	uint64_t rsrc1, rsrc2, rsrc3, properties;
	uint64_t ldsBytes;                      /* static LDS per work-group */
} rdna4_program_t;

kern_return_t rdna4_open(rdna4_t *dev);
void          rdna4_close(rdna4_t *dev);
kern_return_t rdna4_info(rdna4_t *dev, rdna4_info_t *out);

kern_return_t rdna4_alloc(rdna4_t *dev, uint64_t bytes, rdna4_buffer_t *out);
kern_return_t rdna4_free(rdna4_t *dev, const rdna4_buffer_t *buf);
kern_return_t rdna4_write(rdna4_t *dev, const rdna4_buffer_t *buf, uint64_t offset,
                          const void *src, uint64_t bytes);
kern_return_t rdna4_read(rdna4_t *dev, const rdna4_buffer_t *buf, uint64_t offset,
                         void *dst, uint64_t bytes);

kern_return_t rdna4_load(rdna4_t *dev, const void *elf, size_t bytes, const char *kernel,
                         rdna4_program_t *out);
kern_return_t rdna4_unload(rdna4_t *dev, const rdna4_program_t *prog);

/* groups/groupSize: 3 dimensions each; kernargs may be NULL when bytes is 0.
 * timeoutMs 0 = 1000. micros may be NULL. */
kern_return_t rdna4_dispatch(rdna4_t *dev, const rdna4_program_t *prog, const uint32_t groups[3],
                             const uint32_t groupSize[3], const void *kernargs,
                             uint32_t kernargBytes, uint32_t timeoutMs, uint64_t *micros);
/* The same, with LDS per work-group beyond the kernel's static size. */
kern_return_t rdna4_dispatch_lds(rdna4_t *dev, const rdna4_program_t *prog,
                                 const uint32_t groups[3], const uint32_t groupSize[3],
                                 const void *kernargs, uint32_t kernargBytes,
                                 uint32_t dynamicLdsBytes, uint32_t timeoutMs, uint64_t *micros);

/* Present a 256-byte-aligned ARGB8888 slice of a device buffer. The returned
 * geometry is the active scanout width, height and pitch in pixels. */
kern_return_t rdna4_present(rdna4_t *dev, const rdna4_buffer_t *buf, uint64_t offset,
                            uint32_t *width, uint32_t *height, uint32_t *pitch);
kern_return_t rdna4_display_query(rdna4_t *dev, uint32_t *width, uint32_t *height,
                                  uint32_t *pitch);
kern_return_t rdna4_restore(rdna4_t *dev);

const char *rdna4_error(kern_return_t kr);

#endif /* LibRDNA4_h */
