/* Host-only test of userspace/gfx12tricol.h (no GPU): C11, gcc -std=c11 -Wall -Wextra -pedantic -I userspace -I src userspace/test-gfx12tricol.c -lm
 * - the shaders are placed with the attribute ring's address in the three descriptor literals and no marker left over;
 * - the stream records with every relocation landing in a SET_*_REG / packet field, the dword count matches, bad addresses are refused;
 * - the PM4 stream walks cleanly (every header's length adds up to the dword count) and contains the G4 register deltas;
 * - the checker accepts the exact barycentric image and rejects a flat red one, a missing pixel, a wrong colour and a wrong alpha. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx12tricol.h"

static int failures;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint32_t reg_value(const uint32_t *s, uint32_t n, uint32_t opcode, uint32_t reg, int *found) {
	for (uint32_t i = 0; i < n; i += 2 + ((s[i] >> 16) & 0x3fff)) {
		const uint32_t count = ((s[i] >> 16) & 0x3fff) + 1, op = (s[i] >> 8) & 0xff;
		if (op == opcode && count >= 2) {
			const uint32_t first = s[i + 1] & 0xffff;
			if (reg >= first && reg < first + count - 1) {
				*found = 1;
				return s[i + 2 + (reg - first)];
			}
		}
	}
	*found = 0;
	return 0;
}

static void exact_image(uint32_t *img) {
	memset(img, 0, 256 * 256 * 4);
	for (uint32_t y = 0; y < 256; y++)
		for (uint32_t x = 0; x < 256; x++) {
			const double cx = x + 0.5, cy = y + 0.5;
			const double x0 = 64, y0 = 64, x1 = 192, y1 = 64, x2 = 128, y2 = 192;
			const double den2 = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
			const double l0 = ((y1 - y2) * (cx - x2) + (x2 - x1) * (cy - y2)) / den2;
			const double l1 = ((y2 - y0) * (cx - x2) + (x0 - x2) * (cy - y2)) / den2;
			const double l2 = 1.0 - l0 - l1;
			if (l0 < 0 || l1 < 0 || l2 < 0)
				continue;
			if (cy >= 191)
				continue;   /* the G3 triangle covers rows 64..190 */
			img[y * 256 + x] = 0xff000000u | ((uint32_t)(255 * l2 + 0.5) << 16) | ((uint32_t)(255 * l1 + 0.5) << 8) | (uint32_t)(255 * l0 + 0.5);
		}
}

int main(void) {
	static uint32_t code[0x800], ib[600], img[256 * 256];
	const uint64_t ring = 0x0000800100200000ull;   /* a canonical-looking 48-bit VA with bits above 2^32 */
	rdna4_tricol_va va = { 0x0000800100000000ull, 0x0000800100002000ull, ring, 0x0000800100042000ull };

	rdna4_tricol_place_shaders(code, ring);
	int lo = 0, hi = 0, sz = 0, mark = 0;
	for (uint32_t i = 0; i < RDNA4_TRICOL_SHADER_PAD; i++) {
		lo += code[i] == (uint32_t)ring;
		hi += code[i] == (((uint32_t)(ring >> 32) & 0xffffu) | 0xc0000000u | (16u << 16));
		sz += code[i] == (uint32_t)RDNA4_TRICOL(kColRingBytes);
		mark += (code[i] & 0xffff0000u) == 0xc01a0000u;
	}
	EXPECT(lo == 1 && hi == 1 && sz == 1 && mark == 0);
	int d3 = 0;                                                                   /* descriptor dword3 of Mesa's ac_build_attr_ring_descriptor for gfx12 */
	for (uint32_t k = 0; k < RDNA4_TRICOL_SHADER_PAD; k++)
		d3 += code[k] == 0x0043ffacu;
	EXPECT(d3 == 1);
	EXPECT(code[sizeof(kNggcolKernel) / 4] == 0xbf9f0000u);                       /* s_code_end padding */
	EXPECT(((uint32_t *)((char *)code + RDNA4_TRICOL_PS_OFFSET))[0] == kPscolKernel[0]);

	const uint32_t n = rdna4_tricol_record(ib, &va);
	EXPECT(n == rdna4_tricol_ib_dwords() && n == 492);
	uint32_t i = 0;
	while (i < n)
		i += 2 + ((ib[i] >> 16) & 0x3fff);
	EXPECT(i == n);                                                               /* the packets add up */
	for (uint32_t k = 0; k < sizeof(RDNA4_TRICOL(kColRelocs)) / sizeof(RDNA4_TRICOL(kColRelocs)[0]); k++)
		EXPECT(RDNA4_TRICOL(kColRelocs)[k].dword < n);
	int f;
	EXPECT(reg_value(ib, n, 0x69, 0x199, &f) == 0 && f);                          /* SPI_PS_INPUT_CNTL_0 */
	EXPECT(reg_value(ib, n, 0x76, 0x031, &f) == 0x800 && f);                      /* SPI_SHADER_GS_OUT_CONFIG_PS: NUM_INTERP 1, NO_PC_EXPORT 0 */
	EXPECT(reg_value(ib, n, 0x76, 0x08a, &f) == 0x000c0001 && f);                 /* RSRC1_GS VGPRS 1 */
	EXPECT(reg_value(ib, n, 0x76, 0x00a, &f) == 0x000c0001 && f);                 /* RSRC1_PS VGPRS 1 */
	EXPECT(reg_value(ib, n, 0x76, 0x00b, &f) == 4 && f);                          /* RSRC2_PS USER_SGPR 2 */
	EXPECT(reg_value(ib, n, 0x69, 0x190, &f) == 0x8000 && f);                     /* SPI_PS_IN_CONTROL: PS wave32 */
	EXPECT(reg_value(ib, n, 0x69, 0x2a6, &f) == 0x04400000 && f);                 /* VGT_SHADER_STAGES_EN: wave32 NGG passthrough */
	EXPECT(reg_value(ib, n, 0x79, 0x446, &f) == (uint32_t)((ring >> 16) & 0xffffffff) && f);   /* SPI_ATTRIBUTE_RING_BASE = attr ring VA >> 16 */

	va.rings += 0x1000;                                                           /* not 2 MiB aligned */
	EXPECT(rdna4_tricol_record(ib, &va) == 0);
	va.rings = ring; va.fence = 0x0001000000000000ull - 2;                        /* end crosses 2^48 */
	EXPECT(rdna4_tricol_record(ib, &va) == 0);

	exact_image(img);
	rdna4_tricol_result r;
	EXPECT(rdna4_tricol_check(img, &r));
	printf("exact image: covered %u, max channel error %.2f, max sum error %.0f, centroid 0x%08x\n", r.covered, r.maxChannelError, r.maxSumError, r.centroid);
	uint32_t bad[256 * 256];
	memcpy(bad, img, sizeof(bad));
	for (uint32_t k = 0; k < 256 * 256; k++)
		if (bad[k])
			bad[k] = 0xff0000ffu;                                               /* flat red: the G3 image */
	EXPECT(!rdna4_tricol_check(bad, NULL));
	memcpy(bad, img, sizeof(bad)); bad[100 * 256 + 128] = 0;                       /* one pixel missing */
	EXPECT(!rdna4_tricol_check(bad, NULL));
	memcpy(bad, img, sizeof(bad)); bad[100 * 256 + 128] ^= 0x00001000u;           /* a wrong green */
	EXPECT(!rdna4_tricol_check(bad, NULL));
	memcpy(bad, img, sizeof(bad)); bad[100 * 256 + 128] &= 0x00ffffffu;           /* a wrong alpha */
	EXPECT(!rdna4_tricol_check(bad, NULL));

	printf(failures ? "%d FAILURES\n" : "all gfx12tricol host tests passed\n", failures);
	return failures != 0;
}
