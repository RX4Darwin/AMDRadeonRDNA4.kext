/*
 * W12k: where `rdna4-run tri` / `tricol` puts the G3 / G4 triangle in an application's own buffers (docs/w12k-gfx-submit.md).
 * Pure arithmetic, so it is checked on any host (userspace/test-gfx12tri.c).
 *
 * The app allocates five device buffers with rdna4_alloc (GPU VAs are 64 KiB aligned, which is all the stream needs except for the rings):
 *   code    RDNA4_TRI_CODE_BYTES   (VS at +0, PS at +0x1000; 256-byte aligned)
 *   target  RDNA4_TRI_TARGET_BYTES (256x256 RGBA8, zeroed by the app)
 *   rings   kRingBytes + RDNA4_TRIRUN_RING_SLACK, and the stream's ring block starts at the next 2 MiB boundary inside it
 *           (the GE ring bases are in 64 KiB units and Mesa aligns the block to 2 MiB: gfx12tri.h refuses anything else)
 *   misc    one page: the stream's own end-of-pipe RELEASE_MEM writes 1 at +0 (the fence word)
 *   ib      one page: the recorded stream (489 / 492 dwords)
 */
#ifndef RDNA4_GFX12TRIRUN_H
#define RDNA4_GFX12TRIRUN_H

#include <stdint.h>

#define RDNA4_TRIRUN_RING_SLACK 0x200000ull   /* room to align the ring block to 2 MiB */
#define RDNA4_TRIRUN_PAGE       0x1000u

static inline uint64_t rdna4_trirun_rings_va(uint64_t ringBufGpu) {
	return (ringBufGpu + 0x1fffffull) & ~0x1fffffull;
}

/* The aligned ring block of `ringBytes` lies wholly inside the allocation [ringBufGpu, ringBufGpu + ringBufBytes). */
static inline int rdna4_trirun_rings_fit(uint64_t ringBufGpu, uint64_t ringBufBytes, uint64_t ringBytes) {
	const uint64_t va = rdna4_trirun_rings_va(ringBufGpu);
	return va >= ringBufGpu && ringBufBytes >= ringBytes && va - ringBufGpu <= ringBufBytes - ringBytes;
}

#endif
