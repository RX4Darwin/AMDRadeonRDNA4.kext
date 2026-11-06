// Host test of src/pvdraw.hpp: the fake paravirtual GPU's integer-only conversions, the blend state read from a render pipeline's descriptor
// and its registers, and a linear image's T# (docs/m1-stream.md section 5b).
#include "../src/pvdraw.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint64_t bitsOf(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
static uint32_t bitsOf(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

int main() {
	// wholeDouble: whole numbers below 2^30 in magnitude; fractions, larger values, infinities and NaNs refused.
	{
		int64_t v = 7;
		CHECK(wholeDouble(bitsOf(0.0), v) && v == 0);
		CHECK(wholeDouble(bitsOf(-0.0), v) && v == 0);
		CHECK(wholeDouble(bitsOf(-400.0), v) && v == -400);   // iTerm2's viewport origin (rig, 2026-10-22)
		CHECK(wholeDouble(bitsOf(1174.0), v) && v == 1174);
		CHECK(wholeDouble(bitsOf(536870912.0), v) && v == 536870912);   // 2^29
		CHECK(!wholeDouble(bitsOf(1073741824.0), v));                  // 2^30
		CHECK(!wholeDouble(bitsOf(0.5), v));
		CHECK(!wholeDouble(bitsOf(1.25), v));
		CHECK(!wholeDouble(bitsOf(INFINITY), v));
		CHECK(!wholeDouble(bitsOf(NAN), v));
	}
	// halfAsFloat: n / 2 as a float's bits, either sign.
	for (uint32_t n = 0; n < (1u << 16); n++) {
		CHECK(halfAsFloat(n, false) == (n ? bitsOf(n / 2.0f) : 0u));
		CHECK(halfAsFloat(n, true) == (n ? bitsOf(-(n / 2.0f)) : 0u));
		CHECK(halfAsFloat(-static_cast<int64_t>(n)) == (n ? bitsOf(-(n / 2.0f)) : 0u));
	}
	// doubleAsFloat: the single nearest toward zero, as a float cast truncating; the ends: 0, infinity, NaN.
	for (int i = -100000; i <= 100000; i++) {
		const double v = i / 25000.0;
		float want = static_cast<float>(v);

		if (std::fabs(static_cast<double>(want)) > std::fabs(v))
			want = std::nextafter(want, 0.0f);
		CHECK(doubleAsFloat(bitsOf(v)) == bitsOf(want));
	}
	CHECK(doubleAsFloat(bitsOf(0.0)) == 0 && doubleAsFloat(bitsOf(-0.0)) == 0x80000000u && doubleAsFloat(bitsOf(1e-50)) == 0);
	CHECK(doubleAsFloat(bitsOf(1e50)) == 0x7f800000u && doubleAsFloat(bitsOf(-static_cast<double>(INFINITY))) == 0xff800000u &&
	      doubleAsFloat(bitsOf(static_cast<double>(NAN))) == 0x7fc00000u);
	// unorm8: round(v * 255) of the exact product, clamped; NaN 0. A double product rounds 0.3 * 255 up to 76.5; fma gives the rounding error.
	for (int i = 0; i <= 100000; i++) {
		const double v = i / 100000.0, p = v * 255;
		const long want = std::lround(p) - (p - std::floor(p) == 0.5 && std::fma(v, 255, -p) < 0);
		CHECK(unorm8(bitsOf(v)) == static_cast<uint32_t>(want));
	}
	CHECK(unorm8(bitsOf(-0.5)) == 0 && unorm8(bitsOf(2.0)) == 255 && unorm8(bitsOf(NAN)) == 0 && unorm8(bitsOf(1e-30)) == 0);
	// BlendPass: CB_BLEND0_CONTROL and SX_MRT0_BLEND_OPT for one equation.
	{
		const BlendPass over(4, 5, 0);   // SourceAlpha, OneMinusSourceAlpha, Add
		CHECK(over.control == 0x45040504 && over.opt == 0x01770177);
		const BlendPass colour(11, 0, 0);   // BlendColor, Zero, Add -> BLEND_CONSTANT_COLOR 11
		CHECK(colour.control == 0x400b000b);
		const BlendPass min(4, 5, 3);   // Min: both factors One, COMB_MIN_DST_SRC 2
		CHECK(min.control == 0x41410141 && min.opt == 0x03770377);
	}
	// readBlendState: iTerm2's text pipeline (pipeline 228 on the rig, 2026-10-22: labelled "Pipeline for iTermMetalCellRenderer"; BGRA8,
	// blending on, both destination factors OneMinusSourceAlpha).
	{
		static const uint32_t iterm[30] = {0x0000000e, 0x00000078, 0x000000e4, 0x00000067, 0x19040004, 0x08000000, 0x00004004, 0xe3040100,
		                                   0x02000000, 0x0000e204, 0x70695000, 0x6e696c65, 0x6f662065, 0x54692072, 0x4d6d7265, 0x6c617465,
		                                   0x6c6c6543, 0x646e6552, 0x72657265, 0x00000000, 0x00000001, 0x00000008, 0x00040005, 0x01000000,
		                                   0x00005004, 0x01040200, 0x04000000, 0x00000504, 0x05040700, 0x00000000};
		uint32_t blend[10];
		readBlendState(reinterpret_cast<const uint8_t *>(iterm), sizeof(iterm), blend);
		const uint32_t want[10] = {0, 80, 1, 1, 5, 0, 1, 5, 0, 0xf};
		CHECK(!std::memcmp(blend, want, sizeof(want)));
	}
	// readBlendState: an unlabelled pipeline as mtlprobe's (the label one zero byte), with a write mask.
	{
		uint8_t pb[80] = {};
		pb[16] = 3;   // the function list: vertex 0xe3, fragment 0xe2, key 8 = 0x14
		const uint8_t fns[18] = {1, 4, 0xe3, 0, 0, 0, 2, 4, 0xe2, 0, 0, 0, 8, 4, 0x14, 0, 0, 0};
		std::memcpy(pb + 17, fns, sizeof(fns));
		pb[35] = 0;   // the empty label; the attachments at 36
		const uint32_t attachments[2] = {1, 8};
		std::memcpy(pb + 36, attachments, 8);
		const uint8_t tlv[25] = {4, 2, 4, 1, 0, 0, 0, 3, 4, 4, 0, 0, 0, 4, 4, 5, 0, 0, 0, 9, 4, 3, 0, 0, 0};
		std::memcpy(pb + 44, tlv, sizeof(tlv));
		uint32_t blend[10];
		readBlendState(pb, 44 + sizeof(tlv), blend);
		const uint32_t want[10] = {0, 0, 1, 4, 5, 0, 1, 0, 0, 3};
		CHECK(!std::memcmp(blend, want, sizeof(want)));
		readBlendState(pb, 10, blend);   // too short: Metal's defaults
		const uint32_t defaults[10] = {0, 0, 0, 1, 0, 0, 1, 0, 0, 0xf};
		CHECK(!std::memcmp(blend, defaults, sizeof(defaults)));
	}
	// linearImage: a 64x32 RGBA8 image at a 64-pixel pitch.
	{
		uint32_t t[8];
		linearImage(t, 0x123456700ull, 42, 4 | 5 << 3 | 6 << 6 | 7 << 9, 64, 32, 64);
		const uint32_t want[8] = {0x01234567, 0xc0540000, 0x0007c00f, 0x90000fac, 63, 0x00400000, 0x00008000, 0};
		CHECK(!std::memcmp(t, want, sizeof(want)));
	}
	// linearVolume: a 5x3x4 RGBA8 3D image, the hardware's pitch (DEPTH the depth, no pitch).
	{
		uint32_t t[8];
		linearVolume(t, 0x123456700ull, 42, 4 | 5 << 3 | 6 << 6 | 7 << 9, 5, 3, 4);
		const uint32_t want[8] = {0x01234567, 0x00540000, 0x00008001, 0xa0000fac, 3, 0x00400000, 0x00008000, 0};
		CHECK(!std::memcmp(t, want, sizeof(want)));
	}
	// A render pipeline with a vertex descriptor (Sequoia 24H232, rdna4-mtlvdesc, 2026-11-04): attribute 0 float2 at 4 in buffer 1, attribute 3
	// uchar4Normalized at 12 in buffer 1, attribute 5 half2 at 0 in buffer 2; layout 1 stride 20 per vertex, layout 2 stride 8 per instance, step
	// rate 3; colour attachment 0 BGRA8Unorm (80) without blending. Key 8 says where the attachments are: they no longer follow the label.
	{
		static const char *hex =
		    "0e000000f000000003000000dd0000000403041c0000000804c80000000104020000000204010000000000000200041000000001046800000000000003000000"
		    "1000000029000000420000000400040000000001041d0000000204040000000304010000000400040300000001040900000002040c0000000304010000000300"
		    "040500000001041900000003040200000000000003000000100000001d0000002a00000002000401000000030414000000020004010000000304140000000400"
		    "040200000001040200000002040300000003040800000000010000000800000002000400000000010450000000000000";
		uint8_t pb[240] = {};
		for (unsigned k = 0; k < sizeof(pb); k++)
			std::sscanf(hex + 2 * k, "%2hhx", &pb[k]);
		uint32_t top[16], rec[16][4], blend[10];
		pipelineKeys(pb, sizeof(pb), top);
		CHECK(top[1] == 2 && top[2] == 1 && top[3] == 0x1c && top[8] == 0xc8);
		CHECK(readVertexDescriptor(pb, sizeof(pb), rec));
		CHECK(rec[0][0] == 4 && rec[0][1] == 20 && rec[0][2] == (1 | 0u << 8 | 2u << 12 | 1u << 16) && rec[0][3] == 1);   // float2
		CHECK(rec[3][0] == 12 && rec[3][1] == 20 && rec[3][2] == (1 | 2u << 8 | 4u << 12 | 1u << 16));                  // uchar4Normalized
		CHECK(rec[5][0] == 0 && rec[5][1] == 8 && rec[5][2] == (2 | 1u << 8 | 2u << 12 | 2u << 16) && rec[5][3] == 3);   // half2, per instance
		CHECK(rec[1][2] == 0 && rec[2][2] == 0 && rec[4][2] == 0);
		readBlendState(pb, sizeof(pb), blend);
		CHECK(blend[1] == 80 && blend[2] == 0 && blend[3] == 1 && blend[9] == 0xf);
		CHECK(vertexFormat(42) == (2 | 4u << 4 | 1u << 7) && vertexFormat(31) == (0 | 4u << 4) && vertexFormat(40) == 0);
	}
	if (failures) {
		std::printf("pvdraw: %d check(s) FAILED\n", failures);
		return 1;
	}
	std::printf("pvdraw: all checks passed\n");
	return 0;
}
