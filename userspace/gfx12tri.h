/*
 * The G3 triangle as an application's own gfx IB (W12k, rdna4-run tri): the
 * shaders placed in the app's code buffer, the stream of src/gfx12_draw.h with
 * the app's addresses filled in, and the check of the target.
 *
 * Shared by rdna4-run (macOS) and tools/linux-replay, which submits exactly
 * these bytes to the RX 9070 XT through amdgpu as an unprivileged IB in a
 * per-process VMID: 8192 px (docs/linux-replay.md).
 *
 * What the app provides, all mapped in its own VMID:
 *   code    RDNA4_TRI_CODE_BYTES, VS at 0 and PS at RDNA4_TRI_PS_OFFSET (256-byte aligned)
 *   target  RDNA4_TRI_TARGET_BYTES, zeroed, 256-byte aligned
 *   rings   rdna4_tri_ring_bytes(), 2 MiB aligned (the GE ring bases are 64 KiB units)
 *   fence   4 bytes: the stream's own end-of-pipe RELEASE_MEM writes 1 there
 *   ib      rdna4_tri_ib_dwords() dwords
 * All VAs below 2^48.
 */
#ifndef RDNA4_GFX12TRI_H
#define RDNA4_GFX12TRI_H

#include <stdint.h>

#include "gfx12_draw.h"
#include "ngg_kernel.h"
#include "psred_kernel.h"

#ifdef __cplusplus
#define RDNA4_TRI(x) Gfx12Draw::x
#else
#define RDNA4_TRI(x) x
#endif

#define RDNA4_TRI_CODE_BYTES   0x2000u
#define RDNA4_TRI_PS_OFFSET    0x1000u
#define RDNA4_TRI_SHADER_PAD   0x100u     /* dwords per shader slot, s_code_end after the code */
#define RDNA4_TRI_TARGET_BYTES (256u * 256u * 4u)
#define RDNA4_TRI_RING_ALIGN   0x200000u

typedef struct rdna4_tri_va {
	uint64_t code;    /* VS at +0, PS at +RDNA4_TRI_PS_OFFSET */
	uint64_t target;
	uint64_t rings;   /* attribute, position, primitive rings, back to back */
	uint64_t fence;
} rdna4_tri_va;

static inline uint64_t rdna4_tri_ring_bytes(void) { return RDNA4_TRI(kRingBytes); }
static inline uint32_t rdna4_tri_ib_dwords(void) { return sizeof(RDNA4_TRI(kStream)) / 4; }

/* ngg.s and psred.s into the code buffer, each padded with s_code_end for the SQ's prefetch
 * (gfxring.cpp gfxDrawRun place()). */
static inline void rdna4_tri_place_shaders(void *code) {
	uint32_t *vs = (uint32_t *)code, *ps = (uint32_t *)((char *)code + RDNA4_TRI_PS_OFFSET);
	for (uint32_t i = 0; i < RDNA4_TRI_SHADER_PAD; i++) {
		vs[i] = i < sizeof(kNggKernel) / 4 ? kNggKernel[i] : 0xbf9f0000u;
		ps[i] = i < sizeof(kPsredKernel) / 4 ? kPsredKernel[i] : 0xbf9f0000u;
	}
}

/* The stream with the app's addresses: (va >> shift) & mask at every relocation. Returns the
 * dword count, or 0 if an address is misaligned or out of range. */
static inline uint32_t rdna4_tri_record(uint32_t *ib, const rdna4_tri_va *v) {
	const uint32_t n = rdna4_tri_ib_dwords();
	uint64_t va[7];
	if ((v->code & 0xff) || (v->target & 0xff) || (v->rings & (RDNA4_TRI_RING_ALIGN - 1)) || (v->fence & 3) ||
	    ((v->code | v->target | v->rings | v->fence) >> 48))
		return 0;
	/* the ends too: every relocated address (the rings are addressed up to 2^48 in 64 KiB units) must stay below 2^48 */
	if (((v->code + RDNA4_TRI_CODE_BYTES) >> 48) || ((v->target + RDNA4_TRI_TARGET_BYTES) >> 48) ||
	    ((v->rings + RDNA4_TRI(kRingBytes)) >> 48) || ((v->fence + 4) >> 48))
		return 0;
	va[RDNA4_TRI(kVs)] = v->code;
	va[RDNA4_TRI(kPs)] = v->code + RDNA4_TRI_PS_OFFSET;
	va[RDNA4_TRI(kCb)] = v->target;
	va[RDNA4_TRI(kAttrRing)] = v->rings;
	va[RDNA4_TRI(kPosRing)] = v->rings + RDNA4_TRI(kAttrRingBytes);
	va[RDNA4_TRI(kPrimRing)] = v->rings + RDNA4_TRI(kAttrRingBytes) + RDNA4_TRI(kPosRingBytes);
	va[RDNA4_TRI(kFence)] = v->fence;
	for (uint32_t i = 0; i < n; i++)
		ib[i] = RDNA4_TRI(kStream)[i];
	for (uint32_t i = 0; i < sizeof(RDNA4_TRI(kRelocs)) / sizeof(RDNA4_TRI(kRelocs)[0]); i++) {
		const RDNA4_TRI(Reloc) *r = &RDNA4_TRI(kRelocs)[i];
		ib[r->dword] = (uint32_t)((va[r->sym] >> r->shift) & r->mask);
	}
	return n;
}

/* Pixels of the expected colour in the 256x256 target; *others gets every other non-zero pixel. */
static inline uint32_t rdna4_tri_count(const uint32_t *target, uint32_t *others) {
	uint32_t red = 0, other = 0;
	for (uint32_t i = 0; i < 256u * 256u; i++) {
		if (target[i] == RDNA4_TRI(kCoveredRgba))
			red++;
		else if (target[i])
			other++;
	}
	if (others)
		*others = other;
	return red;
}

static inline int rdna4_tri_ok(uint32_t red, uint32_t others) {
	return red == RDNA4_TRI(kCoveredPixels) && !others;
}

#endif
