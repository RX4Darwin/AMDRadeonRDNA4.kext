/*
 *  pvdraw.hpp — the fake paravirtual GPU's pure helpers for draws (src/pvgpu.cpp): integer-only number conversions (no floating point in the
 *  kernel), the blend state from a render pipeline's descriptor and its registers, and a linear image's T#. Host-tested by
 *  test/pvdraw-test.cpp (make test).
 */
#pragma once

#include <stdint.h>
#include <string.h>

// A double in [0, 1] (its bits) as an 8-bit unorm, round(v * 255), with integers only; below 0 (or NaN) 0, from 1 up 255.
static inline uint32_t unorm8(uint64_t bits) {
	const uint32_t e = static_cast<uint32_t>(bits >> 52) & 0x7ff;
	if (bits >> 63 || e < 1023 - 9)
		return 0;   // negative, zero, below 2^-9 (rounds to 0; also keeps the shift below 64) or NaN with the sign set
	if (e >= 1023)
		return e == 0x7ff && (bits & ((1ull << 52) - 1)) ? 0 : 255;   // 1 and up (NaN: 0)
	const uint64_t m = (bits & ((1ull << 52) - 1)) | (1ull << 52);    // 53-bit significand, v = m * 2^(e - 1075)
	const uint32_t shift = 52 + (1023 - e);
	return static_cast<uint32_t>((m * 255 + (1ull << (shift - 1))) >> shift);
}

// A double (its bits) as a whole number of magnitude below 2^30; false if it has a fraction or is larger (no floating point in the kernel).
static inline bool wholeDouble(uint64_t bits, int64_t &out) {
	out = 0;
	if (!(bits << 1))
		return true;   // +-0
	const uint32_t e = static_cast<uint32_t>(bits >> 52) & 0x7ff;
	if (e < 1023 || e > 1023 + 29)
		return false;
	const uint32_t shift = 52 - (e - 1023);
	const uint64_t m = (bits & ((1ull << 52) - 1)) | (1ull << 52);
	if (m & ((1ull << shift) - 1))
		return false;
	out = static_cast<int64_t>(m >> shift);
	if (bits >> 63)
		out = -out;
	return true;
}

// n / 2 as an IEEE-754 single's bits, negated if asked, for n < 2^24 (no floating point in the kernel).
static inline uint32_t halfAsFloat(uint32_t n, bool negative) {
	if (!n)
		return 0;
	const uint32_t e = 31 - static_cast<uint32_t>(__builtin_clz(n));
	return (negative ? 0x80000000u : 0) | ((126 + e) << 23) | ((n << (23 - e)) & 0x7fffff);
}

// The same for a signed n, |n| < 2^24.
static inline uint32_t halfAsFloat(int64_t n) { return halfAsFloat(static_cast<uint32_t>(n < 0 ? -n : n), n < 0); }

// A linear 2D image's T# (SQ_IMG_RSRC_WORD0-7, as ac_build_gfx12_texture_descriptor, one mip level, a custom pitch) at t: gfx12 image format,
// DST_SEL x, y, z, w (4 X .. 7 W, 0 zero, 1 one), width, height, pitch in pixels.
static inline void linearImage(uint32_t *t, uint64_t gpu, uint32_t format, uint32_t sel, uint32_t w, uint32_t h, uint32_t pitchPx) {
	t[0] = static_cast<uint32_t>(gpu >> 8);
	t[1] = static_cast<uint32_t>((gpu >> 40) & 0xff) | format << 17 | ((w - 1) & 3) << 30;   // MAX_MIP 0, FORMAT, BASE_LEVEL 0, WIDTH_LO
	t[2] = (w - 1) >> 2 | (h - 1) << 14;                                                    // WIDTH_HI, HEIGHT
	t[3] = sel | 9u << 28;                                                                  // DST_SEL, LAST_LEVEL 0, SW_MODE linear, TYPE 2D
	t[4] = ((pitchPx - 1) & 0x3fff) | ((pitchPx - 1) >> 14 & 3) << 14;                       // DEPTH = pitch - 1 low, PITCH_MSB
	t[5] = 4u << 20;                                                                        // PERF_MOD 4
	t[6] = 1u << 15;                                                                        // MAX_UNCOMPRESSED_BLOCK_SIZE 256 B
	t[7] = 0;
}

// One pass of colour attachment 0's Metal blend state: {source factor, destination factor, operation} -> CB_BLEND0_CONTROL and
// SX_MRT0_BLEND_OPT. The card ignores SEPARATE_ALPHA_BLEND here (rig 2026-10-22, mtlprobe blendx: alpha always takes the colour
// equation), so a pass has one equation for all four channels and differing alpha is a second pass (runDraw).
// Metal's factors are AMD's BlendOp in another order; for min and max AMD wants both factors One (Metal ignores them there).
struct BlendPass {
	uint32_t control, opt;
	BlendPass(uint32_t src, uint32_t dst, uint32_t op) {
		static const uint8_t factor[19] = {0, 1, 2, 3, 4, 5, 8, 9, 6, 7, 10, 11, 12, 17, 18, 13, 14, 15, 16};   // by MTLBlendFactor
		static const uint8_t comb[5] = {0, 1, 4, 2, 3};      // Add, Subtract (src - dst), ReverseSubtract, Min, Max -> COMB_*
		static const uint8_t optComb[5] = {1, 2, 5, 3, 4};   // the same -> OPT_COMB_*
		op = op < 5 ? op : 0;
		src = src < 19 ? factor[src] : 1;
		dst = dst < 19 ? factor[dst] : 1;
		if (op >= 3)
			src = dst = 1;
		const uint32_t eq = src | uint32_t{comb[op]} << 5 | dst << 8;
		control = eq | eq << 16 | 1u << 30;   // the same equation in the alpha fields, ENABLE
		opt = 0x00770077 | uint32_t{optComb[op]} << 8 | uint32_t{optComb[op]} << 24;   // PRESERVE_NONE_IGNORE_NONE, as RADV for factors it has no rule for
	}
};

// Colour attachment 0's blend state from a render pipeline's descriptor (pb, len bytes; measured with mtlprobe's blendinfo, docs/m1-stream.md
// s.5b): a 16-byte header, the function list {u8 n, n x {u8 key, u8 size, value}}, the pipeline's label (a C string, then 4-aligned; empty: one
// zero byte; iTerm2's: "Pipeline for iTermMetalCellRenderer"), u32 colour attachments, u32 ?, then per attachment {u8 n, n x {u8 key, u8 size,
// value}}: 0 index, 1 pixel format, 2 blending, 3 / 4 / 5 colour source factor, destination factor, operation, 6 / 7 / 8 the same for alpha,
// 9 write mask. Keys not given keep Metal's defaults (off, One, Zero, Add, One, Zero, Add, 0xf); so does everything for a short descriptor.
static inline void readBlendState(const uint8_t *pb, uint32_t len, uint32_t blend[10]) {
	static const uint32_t defaults[10] = {0, 0, 0, 1, 0, 0, 1, 0, 0, 0xf};
	memcpy(blend, defaults, sizeof(defaults));
	if (len < 17)
		return;
	uint32_t at = 17;
	for (uint32_t k = 0; k < pb[16] && at + 2 <= len; k++)   // the function list
		at += 2 + pb[at + 1];
	while (at < len && pb[at])   // the label
		at++;
	at = (at + 1 + 3) & ~3u;
	uint32_t attachments = 0;
	if (at + 9 <= len)
		memcpy(&attachments, pb + at, 4);
	at += 8;
	for (uint32_t k = 0, n = attachments && at < len ? pb[at++] : 0; k < n && at + 2 <= len; k++, at += 2 + pb[at + 1])
		if (pb[at] < 10 && pb[at + 1] == 4 && at + 6 <= len)
			memcpy(&blend[pb[at]], pb + at + 2, 4);
}
