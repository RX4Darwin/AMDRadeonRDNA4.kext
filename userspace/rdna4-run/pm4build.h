/*
 * Small C PM4 builder for the gfx12 compute IB accepted by SubmitIb.
 *
 * The register numbers are the GC segment-0 discovery base (0x1260) plus the
 * compute register offsets in src/gfxregs.hpp.  The packet encodings mirror
 * src/pm4.cpp and gfx_v12_0_ring_emit_ib_compute.
 */
#ifndef RDNA4_PM4BUILD_H
#define RDNA4_PM4BUILD_H

#include <stdint.h>

#define RDNA4_PM4_SH_START             0x2c00u
#define RDNA4_PM4_COMPUTE_START_X     (0x1260u + 0x1ba4u)
#define RDNA4_PM4_COMPUTE_NUM_THREAD_X (0x1260u + 0x1ba7u)
#define RDNA4_PM4_COMPUTE_PGM_LO      (0x1260u + 0x1bacu)
#define RDNA4_PM4_COMPUTE_PGM_RSRC1  (0x1260u + 0x1bb2u)
#define RDNA4_PM4_COMPUTE_RESOURCE_LIM (0x1260u + 0x1bb5u)
#define RDNA4_PM4_COMPUTE_THREAD_SE0  (0x1260u + 0x1bb6u)
#define RDNA4_PM4_COMPUTE_TMPRING     (0x1260u + 0x1bb8u)
#define RDNA4_PM4_COMPUTE_THREAD_SE2  (0x1260u + 0x1bb9u)
#define RDNA4_PM4_COMPUTE_PGM_RSRC3   (0x1260u + 0x1bc8u)
#define RDNA4_PM4_COMPUTE_THREAD_SE4  (0x1260u + 0x1bcbu)
#define RDNA4_PM4_COMPUTE_USER_DATA0  (0x1260u + 0x1be0u)

#define RDNA4_PM4_GCR_MEM_SYNC        0x0000c3b1u
#define RDNA4_PM4_DISPATCH_SHADER_EN  (1u << 0)
#define RDNA4_PM4_DISPATCH_FORCE_START0 (1u << 2)
#define RDNA4_PM4_DISPATCH_WAVE32     (1u << 15)

static inline uint32_t rdna4_pm4_header(uint32_t op, uint32_t count) {
	return (3u << 30) | ((op & 0xffu) << 8) | ((count & 0x3fffu) << 16);
}

static inline uint32_t rdna4_pm4_set_sh_reg(uint32_t *out, uint32_t reg,
                                             const uint32_t *values, uint32_t count) {
	out[0] = rdna4_pm4_header(0x76, count);
	out[1] = reg - RDNA4_PM4_SH_START;
	for (uint32_t i = 0; i < count; i++)
		out[2 + i] = values[i];
	return 2 + count;
}

static inline uint32_t rdna4_pm4_acquire_mem(uint32_t *out, uint32_t gcr) {
	out[0] = rdna4_pm4_header(0x58, 6);
	out[1] = 0;
	out[2] = 0xffffffffu;
	out[3] = 0x00ffffffu;
	out[4] = 0;
	out[5] = 0;
	out[6] = 0x0000000au;
	out[7] = gcr;
	return 8;
}

static inline uint32_t rdna4_pm4_dispatch_direct(uint32_t *out, uint32_t x, uint32_t y,
                                                 uint32_t z, uint32_t initiator) {
	out[0] = rdna4_pm4_header(0x15, 3);
	out[1] = x;
	out[2] = y;
	out[3] = z;
	out[4] = initiator;
	return 5;
}

#endif /* RDNA4_PM4BUILD_H */
