// fsr1-test.c: src/shaders/fsr1.cl's EASU and RCAS pixel functions run as C on the Mac (`make test`): a flat image stays flat,
// a 1:1 pass on gradients is close to the input, an upscaled edge stays sharp and is not rung, RCAS sharpens an edge within range.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/shaders/fsr1.cl"

static uint px(uint g) { return g | g << 8 | g << 16 | 0xffu << 24; }   // a grey pixel
static uint grey(uint p) { return p & 0xff; }
// within `tol` of each other in every channel (RCAS's resolve uses AMD's medium-precision reciprocal: a step off at times)
static int near(uint a, uint b, int tol)
{
	for (int s = 0; s < 32; s += 8)
		if (abs((int)(a >> s & 0xff) - (int)(b >> s & 0xff)) > tol)
			return 0;
	return 1;
}

int main(void)
{
	enum { SW = 64, SH = 36, DW = 128, DH = 72 };
	static uint src[SW * SH], mid[DW * DH], out[DW * DH];

	// flat: every output pixel the input's (EASU 2x exactly, RCAS at its sharpest within a step)
	for (int i = 0; i < SW * SH; i++)
		src[i] = 0x80c04020u;
	for (uint y = 0; y < DH; y++)
		for (uint x = 0; x < DW; x++)
			mid[y * DW + x] = fsrEasuPixel(src, SW, SW, SH, DW, DH, x, y, 0);
	for (uint y = 0; y < DH; y++)
		for (uint x = 0; x < DW; x++) {
			assert(mid[y * DW + x] == 0x80c04020u);
			out[y * DW + x] = fsrRcasPixel(mid, DW, DW, DH, x, y, 0, 0);
			assert(near(out[y * DW + x], 0x80c04020u, 1));
		}

	// 1:1 on smooth gradients: within 2/255 of the input in each channel (not on noise: EASU still stretches its kernel along the
	// edges it finds, as AMD's does)
	for (uint y = 0; y < SH; y++)
		for (uint x = 0; x < SW; x++)
			src[y * SW + x] = x * 3 | y * 6 << 8 | (x + y) * 2 << 16 | 0xffu << 24;
	int worst = 0;
	for (uint y = 0; y < SH; y++)
		for (uint x = 0; x < SW; x++) {
			const uint a = src[y * SW + x], b = fsrEasuPixel(src, SW, SW, SH, SW, SH, x, y, 0);
			for (int s = 0; s < 32; s += 8) {
				const int d = abs((int)(a >> s & 0xff) - (int)(b >> s & 0xff));
				worst = d > worst ? d : worst;
			}
		}
	assert(worst <= 2);

	// an edge (black left of x 32, white right), upscaled 2x: no value outside 0-255 (dering), and two output pixels or more from
	// the edge (at x 64) exactly black or white; at most two pixels in a row in between
	for (uint y = 0; y < SH; y++)
		for (uint x = 0; x < SW; x++)
			src[y * SW + x] = px(x < 32 ? 0 : 255);
	for (uint y = 0; y < DH; y++) {
		int between = 0;
		for (uint x = 0; x < DW; x++) {
			const uint g = grey(mid[y * DW + x] = fsrEasuPixel(src, SW, SW, SH, DW, DH, x, y, 0));
			if (x < 62)
				assert(g == 0);
			else if (x > 65)
				assert(g == 255);
			between += g != 0 && g != 255;
		}
		assert(between <= 2);
	}

	// RCAS on a soft edge (a ramp of 64 to 192 over three pixels): steeper, still within 0-255, flat parts untouched
	for (uint y = 0; y < DH; y++)
		for (uint x = 0; x < DW; x++)
			mid[y * DW + x] = px(x < 63 ? 64 : x == 63 ? 96 : x == 64 ? 160 : 192);
	for (uint y = 0; y < DH; y++)
		for (uint x = 0; x < DW; x++)
			out[y * DW + x] = fsrRcasPixel(mid, DW, DW, DH, x, y, 0, 0);
	for (uint y = 0; y < DH; y++) {
		assert(near(out[y * DW + 10], px(64), 1) && near(out[y * DW + 100], px(192), 1));
		assert(grey(out[y * DW + 63]) < 96 && grey(out[y * DW + 64]) > 160);
	}

	// 10-bit pixels: a flat one stays flat
	for (int i = 0; i < SW * SH; i++)
		src[i] = 0x3u << 30 | 700u << 20 | 300u << 10 | 1000u;
	for (uint y = 0; y < DH; y++)
		for (uint x = 0; x < DW; x++)
			assert(fsrEasuPixel(src, SW, SW, SH, DW, DH, x, y, 1) == src[0]);

	printf("fsr1: all checks passed\n");
	return 0;
}
