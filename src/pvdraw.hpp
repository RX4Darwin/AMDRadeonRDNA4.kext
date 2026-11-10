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

// A double (its bits) as an IEEE-754 single's bits, its significand cut short (rounded toward zero), with integers only: below the
// single's normal range 0 of its sign, above it infinity of its sign, NaN a quiet NaN.
static inline uint32_t doubleAsFloat(uint64_t bits) {
	const uint32_t sign = static_cast<uint32_t>(bits >> 32) & 0x80000000u;
	const int32_t e = static_cast<int32_t>(bits >> 52 & 0x7ff);
	if (e == 0x7ff)
		return sign | (bits & ((1ull << 52) - 1) ? 0x7fc00000u : 0x7f800000u);
	const int32_t f = e - 1023 + 127;
	if (f <= 0)
		return sign;
	if (f >= 0xff)
		return sign | 0x7f800000u;
	return sign | static_cast<uint32_t>(f) << 23 | static_cast<uint32_t>(bits >> 29 & 0x7fffff);
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

// A linear 3D image's T#, as linearImage: its slices `depth` deep, the hardware's own pitches: rows the row's bytes aligned to 128, slices
// `height` rows of them aligned to 256 (rdna4-mtltex3d, 2026-11-04).
static inline void linearVolume(uint32_t *t, uint64_t gpu, uint32_t format, uint32_t sel, uint32_t w, uint32_t h, uint32_t depth) {
	linearImage(t, gpu, format, sel, w, h, 1);
	t[3] = sel | 10u << 28;   // TYPE 3D
	t[4] = (depth - 1) & 0x3fff;
}

// A linear 1D array's T#: `layers` rows of `w` texels, the hardware's own slice pitch (the row's bytes aligned to 256, as a 3D image's).
static inline void linearArray1d(uint32_t *t, uint64_t gpu, uint32_t format, uint32_t sel, uint32_t w, uint32_t layers) {
	linearImage(t, gpu, format, sel, w, 1, 1);
	t[3] = sel | 12u << 28;   // TYPE 1D_ARRAY
	t[4] = (layers - 1) & 0x3fff;
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

// The entry list {u8 n, n x {u8 key, u8 size, value}} at `at`: the 4-byte values of keys below `keys` into values (those absent keep theirs);
// returns where the list ends, 0 if it runs past len.
static inline uint32_t entryList(const uint8_t *pb, uint32_t len, uint32_t at, uint32_t *values, uint32_t keys) {
	if (at >= len)
		return 0;
	const uint32_t n = pb[at++];
	for (uint32_t k = 0; k < n; k++) {
		if (at + 2 > len || at + 2 + pb[at + 1] > len)
			return 0;
		if (pb[at] < keys && pb[at + 1] == 4)
			memcpy(&values[pb[at]], pb + at + 2, 4);
		at += 2 + pb[at + 1];
	}
	return at;
}

// A render pipeline's descriptor (pb, len bytes: the object with its 16-byte header) starts with an entry list: key 1 the vertex function, 2 the
// fragment function, 3 the vertex descriptor's offset and 8 the colour attachments', both counted from the descriptor (+16) (measured on Sequoia
// 24H232 with a vertex descriptor of known values, rdna4-mtlvdesc, 2026-11-04; with one the attachments no longer follow the label).
static inline void pipelineKeys(const uint8_t *pb, uint32_t len, uint32_t top[16]) {
	memset(top, 0, 16 * sizeof(uint32_t));
	if (len >= 17)
		(void)entryList(pb, len, 16, top, 16);
}

// The colour attachments' blend states from a render pipeline's descriptor (pb, len bytes; measured with mtlprobe's blendinfo, docs/m1-stream.md
// s.5b): a 16-byte header, the function list {u8 n, n x {u8 key, u8 size, value}}, the pipeline's label (a C string, then 4-aligned; empty: one
// zero byte; iTerm2's: "Pipeline for iTermMetalCellRenderer"), u32 colour attachments, u32 ?, then per attachment {u8 n, n x {u8 key, u8 size,
// value}}: 0 index, 1 pixel format, 2 blending, 3 / 4 / 5 colour source factor, destination factor, operation, 6 / 7 / 8 the same for alpha,
// 9 write mask. Keys not given keep Metal's defaults (off, One, Zero, Add, One, Zero, Add, 0xf); so does everything for a short descriptor.
// Where the list's key 8 says, the attachments are there (a vertex descriptor lies between the label and them).
static inline void readBlendStates(const uint8_t *pb, uint32_t len, uint32_t blend[8][10]) {
	static const uint32_t defaults[10] = {0, 0, 0, 1, 0, 0, 1, 0, 0, 0xf};
	for (uint32_t k = 0; k < 8; k++) {
		memcpy(blend[k], defaults, sizeof(defaults));
		blend[k][0] = k;
	}
	if (len < 17)
		return;
	uint32_t top[16], at = 17;
	pipelineKeys(pb, len, top);
	if (top[8]) {
		at = 16 + top[8];
	} else {
		for (uint32_t k = 0; k < pb[16] && at + 2 <= len; k++)   // the function list
			at += 2 + pb[at + 1];
		while (at < len && pb[at])   // the label
			at++;
		at = (at + 1 + 3) & ~3u;
	}
	// a table as the vertex descriptor's {u32 n, u32 offset of each attachment's list from the count}: {1, 8} for one; key 0 its index
	uint32_t n = 0;
	if (at + 4 <= len)
		memcpy(&n, pb + at, 4);
	if (n > 8 || at + 4 + 4 * n > len)
		return;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t off, v[10];
		memcpy(&off, pb + at + 4 + 4 * i, 4);
		memcpy(v, defaults, sizeof(v));
		if (entryList(pb, len, at + off, v, 10) && v[0] < 8)
			memcpy(blend[v[0]], v, sizeof(v));
	}
}

// Colour attachment 0's.
static inline void readBlendState(const uint8_t *pb, uint32_t len, uint32_t blend[10]) {
	uint32_t all[8][10];
	readBlendStates(pb, len, all);
	memcpy(blend, all[0], sizeof(all[0]));
}

// A MTLVertexFormat as the translator's attribute kind (include/rdna4mtlc.h) | components << 4 | BGRA << 7; 0 for formats not handled.
static inline uint32_t vertexFormat(uint32_t f) {
	static const struct { uint8_t first, last, kind, count; } groups[] = {
		{1, 3, 4, 2}, {4, 6, 5, 2}, {7, 9, 2, 2}, {10, 12, 3, 2}, {13, 15, 8, 2}, {16, 18, 9, 2}, {19, 21, 6, 2}, {22, 24, 7, 2},
		{25, 27, 1, 2}, {28, 31, 0, 1}, {32, 35, 10, 1}, {36, 39, 11, 1}, {45, 45, 4, 1}, {46, 46, 5, 1}, {47, 47, 2, 1}, {48, 48, 3, 1},
		{49, 49, 8, 1}, {50, 50, 9, 1}, {51, 51, 6, 1}, {52, 52, 7, 1}, {53, 53, 1, 1},
	};
	if (f == 42)   // uchar4Normalized_BGRA
		return 2 | 4u << 4 | 1u << 7;
	for (const auto &g : groups)
		if (f >= g.first && f <= g.last)
			return g.kind | (g.count + f - g.first) << 4;
	return 0;
}

// One table of the vertex descriptor at `at`: {u32 n, u32 offset of each entry list from the count}; each list's keys 0-3 by its key 0, the
// index (below `max`), into out[index], with present[index] set.
static inline bool vertexTable(const uint8_t *pb, uint32_t len, uint32_t at, uint32_t (*out)[4], bool *present, uint32_t max) {
	uint32_t n;
	if (at + 4 > len)
		return false;
	memcpy(&n, pb + at, 4);
	if (n > 31 || at + 4 + 4 * n > len)
		return false;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t off, v[4] = {};
		memcpy(&off, pb + at + 4 + 4 * i, 4);
		if (!entryList(pb, len, at + off, v, 4) || v[0] >= max)
			return false;
		present[v[0]] = true;
		memcpy(out[v[0]] + 1, v + 1, 3 * sizeof(uint32_t));
	}
	return true;
}

// The pipeline's vertex descriptor as the vertex stage's attribute records (RDNA4_MTLC_TABLE_ATTRIBUTES, include/rdna4mtlc.h): at the list's key 3,
// an entry list with key 0 the attributes' table and key 1 the layouts', both offsets from it. An attribute: key 1 format (MTLVertexFormat), 2
// offset, 3 buffer index; a layout (by buffer index): key 1 step function (default per vertex), 2 step rate (default 1), 3 stride. A record:
// {offset, stride, buffer | kind << 8 | components << 12 | BGRA << 15 | step function << 16, step rate}; all zero without a descriptor or for an
// attribute of a format not handled (the shader then reads (0, 0, 0, 1)). False if the descriptor is there but not understood.
static inline bool readVertexDescriptor(const uint8_t *pb, uint32_t len, uint32_t rec[16][4], uint32_t *skipped = nullptr) {
	uint32_t top[16], vd[2] = {}, attr[16][4] = {}, layout[31][4] = {};
	bool attrSeen[16] = {}, layoutSeen[31] = {};
	memset(rec, 0, 16 * 4 * sizeof(uint32_t));
	pipelineKeys(pb, len, top);
	if (!top[3])
		return true;
	const uint32_t at = 16 + top[3];
	if (!entryList(pb, len, at, vd, 2) || !vertexTable(pb, len, at + vd[0], attr, attrSeen, 16) ||
	    !vertexTable(pb, len, at + vd[1], layout, layoutSeen, 31))
		return false;
	for (uint32_t i = 0; i < 16; i++) {
		const uint32_t f = attrSeen[i] ? vertexFormat(attr[i][1]) : 0, buffer = attr[i][3];
		if (attrSeen[i] && (!f || buffer >= 31) && skipped)
			skipped[i] = attr[i][1] | buffer << 16;   // the Metal vertex format the host does not know, and its buffer
		if (!f || buffer >= 31)
			continue;
		const uint32_t step = layoutSeen[buffer] && layout[buffer][1] ? layout[buffer][1] : 1;
		const uint32_t rate = layoutSeen[buffer] && layout[buffer][2] ? layout[buffer][2] : 1;
		rec[i][0] = attr[i][2];
		rec[i][1] = layoutSeen[buffer] ? layout[buffer][3] : 0;
		rec[i][2] = buffer | (f & 15) << 8 | (f >> 4 & 7) << 12 | (f >> 7 & 1) << 15 | step << 16;
		rec[i][3] = rate;
	}
	return true;
}
