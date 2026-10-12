/*
 * The G4 triangle as an application's own gfx IB: the G3 triangle (gfx12tri.h) with a per-vertex colour, vertex 0 red, 1 green,
 * 2 blue, interpolated by the PS. The NGG stage (shaders/nggcol.s) stores the colour to the attribute ring, the PS
 * (shaders/pscol.s) reads it back with ds_param_load / v_interp and exports it. Stream: src/gfx12_draw_col.h (G3 plus the register
 * deltas of docs/g4-colour.md). Shared by tools/linux-replay (REPLAY_DRAW=col) and, later, rdna4-run.
 *
 * What the app provides is what gfx12tri.h asks for (code, target, rings 2 MiB aligned, fence, ib), plus: the NGG shader needs the
 * attribute ring's address in its buffer descriptor, so the shaders are placed with the ring VA (rdna4_tricol_place_shaders).
 * All VAs below 2^48.
 *
 * Freestanding (only <stdint.h>, integer arithmetic): included by tools/linux-replay, userspace/test-gfx12tricol.c and the kext
 * (src/gfxring.cpp, the boot-arg rdna4-gfxcol=1 draw), so all three run the same shader words, descriptor and colour check.
 */
#ifndef RDNA4_GFX12TRICOL_H
#define RDNA4_GFX12TRICOL_H

#include <stdint.h>

#include "../src/gfx12_draw_col.h"   /* relative: the kext has no -Isrc / -Iuserspace in its flags */
#include "../src/nggcol_kernel.h"
#include "../src/pscol_kernel.h"

#ifdef __cplusplus
#define RDNA4_TRICOL(x) Gfx12DrawCol::x
#else
#define RDNA4_TRICOL(x) x
#endif

#define RDNA4_TRICOL_CODE_BYTES   0x2000u
#define RDNA4_TRICOL_PS_OFFSET    0x1000u
#define RDNA4_TRICOL_SHADER_PAD   0x100u     /* dwords per shader slot, s_code_end after the code */
#define RDNA4_TRICOL_TARGET_BYTES (256u * 256u * 4u)
#define RDNA4_TRICOL_RING_ALIGN   0x200000u

typedef struct rdna4_tricol_va {
	uint64_t code;    /* VS at +0, PS at +RDNA4_TRICOL_PS_OFFSET */
	uint64_t target;
	uint64_t rings;   /* attribute, position, primitive rings, back to back */
	uint64_t fence;
} rdna4_tricol_va;

static inline uint64_t rdna4_tricol_ring_bytes(void) { return RDNA4_TRICOL(kColRingBytes); }
static inline uint32_t rdna4_tricol_ib_dwords(void) { return sizeof(RDNA4_TRICOL(kColStream)) / 4; }

/* The dwords of the two shaders, each followed by s_code_end padding (RDNA4_TRICOL_SHADER_PAD dwords per slot), for callers whose code slots are
 * not one buffer (the kext writes them through volatile pool pointers). nggcol.s builds its attribute ring buffer descriptor from three
 * literal dwords (shaders/nggcol.s): 0xc01a0001 = ring VA low, 0xc01a0002 = dword1 = VA[47:32] | swizzle_enable 3 (0xc0000000) | stride
 * 16 << 16, 0xc01a0003 = ring size in bytes (ac_build_attr_ring_descriptor + radv_nir_lower_abi.c:109-118). */
static inline uint32_t rdna4_tricol_vs_dword(uint32_t i, uint64_t attrRingVa) {
	if (i >= sizeof(kNggcolKernel) / 4)
		return 0xbf9f0000u;
	const uint32_t d = kNggcolKernel[i];
	if (d == 0xc01a0001u)
		return (uint32_t)attrRingVa;
	if (d == 0xc01a0002u)
		return (uint32_t)((attrRingVa >> 32) & 0xffffu) | 0xc0000000u | (16u << 16);
	if (d == 0xc01a0003u)
		return (uint32_t)RDNA4_TRICOL(kColRingBytes);
	return d;
}
static inline uint32_t rdna4_tricol_ps_dword(uint32_t i) {
	return i < sizeof(kPscolKernel) / 4 ? kPscolKernel[i] : 0xbf9f0000u;
}

/* nggcol.s and pscol.s into one code buffer (PS at +RDNA4_TRICOL_PS_OFFSET). */
static inline void rdna4_tricol_place_shaders(void *code, uint64_t attrRingVa) {
	uint32_t *vs = (uint32_t *)code, *ps = (uint32_t *)((char *)code + RDNA4_TRICOL_PS_OFFSET);
	for (uint32_t i = 0; i < RDNA4_TRICOL_SHADER_PAD; i++) {
		vs[i] = rdna4_tricol_vs_dword(i, attrRingVa);
		ps[i] = rdna4_tricol_ps_dword(i);
	}
}

/* The stream with the app's addresses: (va >> shift) & mask at every relocation. Returns the dword count, or 0 if an address is
 * misaligned or out of range (base or end of any buffer at or above 2^48). */
static inline uint32_t rdna4_tricol_record(uint32_t *ib, const rdna4_tricol_va *v) {
	const uint32_t n = rdna4_tricol_ib_dwords();
	uint64_t va[7];
	if ((v->code & 0xff) || (v->target & 0xff) || (v->rings & (RDNA4_TRICOL_RING_ALIGN - 1)) || (v->fence & 3) ||
	    ((v->code | v->target | v->rings | v->fence) >> 48))
		return 0;
	if (((v->code + RDNA4_TRICOL_CODE_BYTES) >> 48) || ((v->target + RDNA4_TRICOL_TARGET_BYTES) >> 48) ||
	    ((v->rings + RDNA4_TRICOL(kColRingBytes)) >> 48) || ((v->fence + 4) >> 48))
		return 0;
	va[RDNA4_TRICOL(kColVs)] = v->code;
	va[RDNA4_TRICOL(kColPs)] = v->code + RDNA4_TRICOL_PS_OFFSET;
	va[RDNA4_TRICOL(kColCb)] = v->target;
	va[RDNA4_TRICOL(kColAttrRing)] = v->rings;
	va[RDNA4_TRICOL(kColPosRing)] = v->rings + RDNA4_TRICOL(kColAttrRingBytes);
	va[RDNA4_TRICOL(kColPrimRing)] = v->rings + RDNA4_TRICOL(kColAttrRingBytes) + RDNA4_TRICOL(kColPosRingBytes);
	va[RDNA4_TRICOL(kColFence)] = v->fence;
	for (uint32_t i = 0; i < n; i++)
		ib[i] = RDNA4_TRICOL(kColStream)[i];
	for (uint32_t i = 0; i < sizeof(RDNA4_TRICOL(kColRelocs)) / sizeof(RDNA4_TRICOL(kColRelocs)[0]); i++) {
		const RDNA4_TRICOL(ColReloc) *r = &RDNA4_TRICOL(kColRelocs)[i];
		ib[r->dword] = (uint32_t)((va[r->sym] >> r->shift) & r->mask);
	}
	return n;
}

/* The check of the colour triangle (docs/g4-colour.md, the same as tools/radv-triangle TRI_COLOR=1), in integer arithmetic. The 256x256 target's
 * covered (non-zero) pixels must be the G3 triangle's 8192 (bounds x 64..191, y 64..190), each with alpha 0xFF and R, G, B within
 * kColChannelTolerance of 255 * the barycentric weights of (64,64) red, (192,64) green, (128,192) blue at the pixel centre (RADV and the kext's own
 * first run on the card: largest channel error 0.50, |R+G+B - 255| <= 1), each pixel's strongest channel the nearest vertex's (within the same
 * tolerance), the pixels near the vertices dominated by that vertex's colour and the centroid pixel ~(85,85,85).
 * Weights: with doubled coordinates v0 (128,128), v1 (384,128), v2 (256,384) and the pixel centre (2x+1, 2y+1) the barycentric numerators are
 * n0 = -256 (cx - 256) - 128 (cy - 384), n1 = 256 (cx - 256) - 128 (cy - 384), n2 = 65536 - n0 - n1 over a denominator of 65536. */
typedef struct rdna4_tricol_result {
	uint32_t covered;          /* non-zero pixels */
	uint32_t badAlpha;         /* covered pixels whose alpha is not 0xFF */
	uint32_t notDominant;      /* covered pixels whose strongest channel is not (within tolerance) the nearest vertex's */
	uint32_t maxChannelErr256; /* max |channel - exact barycentric colour|, in 1/256 channel units (0.50 = 128) */
	uint32_t maxSumErr;        /* max |R + G + B - 255| */
	uint32_t minX, maxX, minY, maxY;   /* bounds of the covered pixels */
	uint32_t near0, near1, near2, centroid;   /* pixels (66,65), (189,65), (128,188), (128,106) */
} rdna4_tricol_result;

static inline uint32_t rdna4_tricol_absdiff(int32_t a, int32_t b) { return (uint32_t)(a > b ? a - b : b - a); }

static inline int rdna4_tricol_check(const uint32_t *target, rdna4_tricol_result *res) {
	rdna4_tricol_result r = { 0, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0, 0, 0 };
	const int32_t tol = (int32_t)RDNA4_TRICOL(kColChannelTolerance);
	for (int32_t y = 0; y < 256; y++)
		for (int32_t x = 0; x < 256; x++) {
			const uint32_t v = target[y * 256 + x];
			if (!v)
				continue;
			r.covered++;
			if ((uint32_t)x < r.minX) r.minX = (uint32_t)x;
			if ((uint32_t)x > r.maxX) r.maxX = (uint32_t)x;
			if ((uint32_t)y < r.minY) r.minY = (uint32_t)y;
			if ((uint32_t)y > r.maxY) r.maxY = (uint32_t)y;
			const int32_t cx = 2 * x + 1, cy = 2 * y + 1;
			const int32_t n0 = -256 * (cx - 256) - 128 * (cy - 384);
			const int32_t n1 = 256 * (cx - 256) - 128 * (cy - 384);
			const int32_t n[3] = { n0, n1, 65536 - n0 - n1 };
			const int32_t c[3] = { (int32_t)(v & 0xff), (int32_t)((v >> 8) & 0xff), (int32_t)((v >> 16) & 0xff) };
			if ((v >> 24) != 0xff)
				r.badAlpha++;
			const uint32_t sum = rdna4_tricol_absdiff(c[0] + c[1] + c[2], 255);
			if (sum > r.maxSumErr)
				r.maxSumErr = sum;
			for (int k = 0; k < 3; k++) {
				const uint32_t e = rdna4_tricol_absdiff(256 * c[k], (255 * n[k]) / 256);
				if (e > r.maxChannelErr256)
					r.maxChannelErr256 = e;
			}
			const int dom = c[0] >= c[1] && c[0] >= c[2] ? 0 : c[1] >= c[2] ? 1 : 2;
			const int32_t nmax = n[0] >= n[1] && n[0] >= n[2] ? n[0] : n[1] >= n[2] ? n[1] : n[2];
			if (n[dom] < nmax - tol * 65536 / 255)   /* the chosen channel's weight must be the largest, within the tolerance */
				r.notDominant++;
		}
	r.near0 = target[65 * 256 + 66];
	r.near1 = target[65 * 256 + 189];
	r.near2 = target[188 * 256 + 128];
	r.centroid = target[106 * 256 + 128];
	if (res)
		*res = r;
	int cenOk = 1;
	for (int k = 0; k < 3; k++) {
		const int32_t c = (int32_t)((r.centroid >> (8 * k)) & 0xff);
		cenOk = cenOk && c >= 79 && c <= 91;   /* ~85 +- 6 */
	}
	return r.covered == RDNA4_TRICOL(kColCoveredPixels) && r.minX == 64 && r.maxX == 191 && r.minY == 64 && r.maxY == 190 &&
	       !r.badAlpha && !r.notDominant && r.maxChannelErr256 <= (uint32_t)tol * 256 && r.maxSumErr <= (uint32_t)tol &&
	       (r.near0 & 0xff) > 200 && ((r.near1 >> 8) & 0xff) > 200 && ((r.near2 >> 16) & 0xff) > 200 && cenOk;
}

#endif
