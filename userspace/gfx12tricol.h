/*
 * The G4 triangle as an application's own gfx IB: the G3 triangle (gfx12tri.h) with a per-vertex colour, vertex 0 red, 1 green,
 * 2 blue, interpolated by the PS. The NGG stage (shaders/nggcol.s) stores the colour to the attribute ring, the PS
 * (shaders/pscol.s) reads it back with ds_param_load / v_interp and exports it. Stream: src/gfx12_draw_col.h (G3 plus the register
 * deltas of docs/g4-colour.md). Shared by tools/linux-replay (REPLAY_DRAW=col) and, later, rdna4-run.
 *
 * What the app provides is what gfx12tri.h asks for (code, target, rings 2 MiB aligned, fence, ib), plus: the NGG shader needs the
 * attribute ring's address in its buffer descriptor, so the shaders are placed with the ring VA (rdna4_tricol_place_shaders).
 * All VAs below 2^48.
 */
#ifndef RDNA4_GFX12TRICOL_H
#define RDNA4_GFX12TRICOL_H

#include <stdint.h>

#include "gfx12_draw_col.h"
#include "nggcol_kernel.h"
#include "pscol_kernel.h"

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

/* nggcol.s and pscol.s into the code buffer, each padded with s_code_end. nggcol.s builds its attribute ring buffer descriptor
 * from three literal dwords (shaders/nggcol.s): 0xc01a0001 = ring VA low, 0xc01a0002 = dword1 = VA[47:32] | swizzle_enable 3
 * (0xc0000000) | stride 16 << 16, 0xc01a0003 = ring size in bytes (ac_build_attr_ring_descriptor + radv_nir_lower_abi.c:109-118). */
static inline void rdna4_tricol_place_shaders(void *code, uint64_t attrRingVa) {
	uint32_t *vs = (uint32_t *)code, *ps = (uint32_t *)((char *)code + RDNA4_TRICOL_PS_OFFSET);
	const uint32_t dword1 = (uint32_t)((attrRingVa >> 32) & 0xffffu) | 0xc0000000u | (16u << 16);
	for (uint32_t i = 0; i < RDNA4_TRICOL_SHADER_PAD; i++) {
		uint32_t d = i < sizeof(kNggcolKernel) / 4 ? kNggcolKernel[i] : 0xbf9f0000u;
		if (d == 0xc01a0001u) d = (uint32_t)attrRingVa;
		else if (d == 0xc01a0002u) d = dword1;
		else if (d == 0xc01a0003u) d = (uint32_t)RDNA4_TRICOL(kColRingBytes);
		vs[i] = d;
		ps[i] = i < sizeof(kPscolKernel) / 4 ? kPscolKernel[i] : 0xbf9f0000u;
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

/* The check of the colour triangle (docs/g4-colour.md, the same as tools/radv-triangle TRI_COLOR=1): the 256x256 target's covered
 * (non-zero) pixels must be the G3 triangle's 8192, each with alpha 0xFF and R, G, B within kColChannelTolerance of 255 * the
 * barycentric weights of (64,64) red, (192,64) green, (128,192) blue at the pixel centre (RADV on the card: max error 0.5, and
 * |R+G+B - 255| <= 1), each the nearest vertex's colour dominant. */
typedef struct rdna4_tricol_result {
	uint32_t covered;        /* non-zero pixels */
	uint32_t badAlpha;       /* covered pixels whose alpha is not 0xFF */
	uint32_t notDominant;    /* covered pixels whose strongest channel is not the nearest vertex's */
	double maxChannelError;  /* max |channel - exact barycentric colour| */
	double maxSumError;      /* max |R + G + B - 255| */
	uint32_t near0, near1, near2, centroid;   /* pixels (66,65), (189,65), (128,188), (128,106) */
} rdna4_tricol_result;

static inline double rdna4_tricol_abs(double x) { return x < 0 ? -x : x; }

static inline int rdna4_tricol_check(const uint32_t *target, rdna4_tricol_result *res) {
	const double x0 = 64, y0 = 64, x1 = 192, y1 = 64, x2 = 128, y2 = 192;
	const double den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
	rdna4_tricol_result r = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	for (uint32_t y = 0; y < 256; y++)
		for (uint32_t x = 0; x < 256; x++) {
			const uint32_t v = target[y * 256 + x];
			if (!v)
				continue;
			r.covered++;
			const double cx = x + 0.5, cy = y + 0.5;
			const double l0 = ((y1 - y2) * (cx - x2) + (x2 - x1) * (cy - y2)) / den;
			const double l1 = ((y2 - y0) * (cx - x2) + (x0 - x2) * (cy - y2)) / den;
			const double l2 = 1.0 - l0 - l1;
			const double e[3] = { 255 * l0, 255 * l1, 255 * l2 };
			const int c[3] = { (int)(v & 0xff), (int)((v >> 8) & 0xff), (int)((v >> 16) & 0xff) };
			if ((v >> 24) != 0xff)
				r.badAlpha++;
			const double sum = rdna4_tricol_abs(c[0] + c[1] + c[2] - 255.0);
			if (sum > r.maxSumError)
				r.maxSumError = sum;
			for (int k = 0; k < 3; k++) {
				const double d = rdna4_tricol_abs(c[k] - e[k]);
				if (d > r.maxChannelError)
					r.maxChannelError = d;
			}
			const int dom = c[0] >= c[1] && c[0] >= c[2] ? 0 : c[1] >= c[2] ? 1 : 2;
			const int want = l0 >= l1 && l0 >= l2 ? 0 : l1 >= l2 ? 1 : 2;
			if (dom != want)
				r.notDominant++;
		}
	r.near0 = target[65 * 256 + 66];
	r.near1 = target[65 * 256 + 189];
	r.near2 = target[188 * 256 + 128];
	r.centroid = target[106 * 256 + 128];
	if (res)
		*res = r;
	const double tol = (double)RDNA4_TRICOL(kColChannelTolerance);
	int cenOk = 1;
	for (int k = 0; k < 3; k++) {
		const int c = (int)((r.centroid >> (8 * k)) & 0xff);
		cenOk = cenOk && c >= 79 && c <= 91;   /* ~85 +- 6 */
	}
	return r.covered == RDNA4_TRICOL(kColCoveredPixels) && !r.badAlpha && !r.notDominant && r.maxChannelError <= tol &&
	       r.maxSumError <= tol && (r.near0 & 0xff) > 200 && ((r.near1 >> 8) & 0xff) > 200 && ((r.near2 >> 16) & 0xff) > 200 && cenOk;
}

#endif
