/* Host-only test (no GPU) of what `rdna4-run tri` / `tricol` record: C11, gcc or clang,
 *   cc -std=c11 -Wall -Wextra -pedantic -I userspace -I src userspace/test-gfx12tri.c -o /tmp/t && /tmp/t
 * - the G3 builder (gfx12tri.h): shaders placed with s_code_end padding, the stream records, every packet walks, every relocation lands
 *   where the address is, misaligned / out-of-range addresses are refused, the image check accepts 8192 red px and rejects the rest;
 * - the W12k layout (gfx12trirun.h) with a simulated allocator (64 KiB aligned VAs from the client VA base): the same sizes
 *   `rdna4-run` allocates always give addresses both builders accept, the ring block stays inside its allocation;
 * - the stream's own end-of-pipe fence (RELEASE_MEM) writes to the fence buffer's address. */
#include <stdio.h>
#include <string.h>

#include "gfx12tri.h"
#include "gfx12tricol.h"
#include "gfx12trirun.h"

static int failures;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int walk(const uint32_t *s, uint32_t n) {
	uint32_t i = 0;
	while (i < n) {
		if ((s[i] >> 30) != 3)
			return 0;
		i += 2 + ((s[i] >> 16) & 0x3fff);
	}
	return i == n;
}

/* the dword index of the RELEASE_MEM packet (opcode 0x49) whose address dwords hold `fenceVa`, or -1 */
static int find_fence(const uint32_t *s, uint32_t n, uint64_t fenceVa) {
	for (uint32_t i = 0; i < n; i += 2 + ((s[i] >> 16) & 0x3fff))
		if (((s[i] >> 8) & 0xff) == 0x49 && s[i + 3] == (uint32_t)fenceVa && s[i + 4] == (uint32_t)(fenceVa >> 32))
			return (int)i;
	return -1;
}

int main(void) {
	static uint32_t code[RDNA4_TRI_CODE_BYTES / 4], ib[1024], img[256 * 256];

	/* the simulated allocator: sizes of rdna4-run's cmdTri, VAs 64 KiB aligned, sequential from the client VA base */
	const uint64_t base = 0x0000000100000000ull;
	const uint64_t sizes[5] = { RDNA4_TRI_CODE_BYTES, RDNA4_TRI_TARGET_BYTES, 0xA80000ull + RDNA4_TRIRUN_RING_SLACK, RDNA4_TRIRUN_PAGE, RDNA4_TRIRUN_PAGE };
	for (uint64_t skew = 0; skew < 8; skew++) {   /* different starting points: the ring buffer lands on every 64 KiB phase of 2 MiB */
		uint64_t va[5], next = base + skew * 0x10000;
		for (int i = 0; i < 5; i++) {
			va[i] = next;
			next = (next + sizes[i] + 0xffff) & ~0xffffull;
		}
		EXPECT(rdna4_trirun_rings_fit(va[2], sizes[2], rdna4_tri_ring_bytes()));
		const uint64_t ringVa = rdna4_trirun_rings_va(va[2]);
		EXPECT((ringVa & 0x1fffff) == 0 && ringVa >= va[2] && ringVa + rdna4_tri_ring_bytes() <= va[2] + sizes[2]);
		const rdna4_tri_va v3 = { va[0], va[1], ringVa, va[3] };
		EXPECT(rdna4_tri_record(ib, &v3) == rdna4_tri_ib_dwords() && rdna4_tri_ib_dwords() == 489);
		EXPECT(walk(ib, 489));
		EXPECT(find_fence(ib, 489, va[3]) >= 0);            /* the stream's own fence writes to the misc buffer */
		const rdna4_tricol_va vc = { va[0], va[1], ringVa, va[3] };
		EXPECT(rdna4_tricol_record(ib, &vc) == rdna4_tricol_ib_dwords() && rdna4_tricol_ib_dwords() == 492);
		EXPECT(walk(ib, 492));
		EXPECT(find_fence(ib, 492, va[3]) >= 0);
	}
	EXPECT(!rdna4_trirun_rings_fit(0x100000000ull, 0xA80000ull, 0xA80000ull + 1));   /* too small for the block */
	EXPECT(!rdna4_trirun_rings_fit(0x100010000ull, 0xA80000ull, 0xA80000ull));      /* no slack: alignment pushes it out */
	EXPECT(rdna4_trirun_rings_fit(0x100000000ull, 0xA80000ull, 0xA80000ull));        /* already aligned: fits exactly */

	/* G3 builder refusals */
	{
		rdna4_tri_va v = { 0x100000000ull, 0x100002000ull, 0x100200000ull, 0x100042000ull };
		EXPECT(rdna4_tri_record(ib, &v) == 489);
		v.code += 0x10;
		EXPECT(rdna4_tri_record(ib, &v) == 0);                                       /* code not 256-byte aligned */
		v.code = 0x100000000ull; v.rings += 0x10000;
		EXPECT(rdna4_tri_record(ib, &v) == 0);                                       /* rings not 2 MiB aligned */
		v.rings = 0x100200000ull; v.fence = 0x0000fffffffffffeull;
		EXPECT(rdna4_tri_record(ib, &v) == 0);                                       /* fence end crosses 2^48 */
	}

	/* shaders: VS at +0, PS at +0x1000, each followed by s_code_end up to the pad */
	rdna4_tri_place_shaders(code);
	EXPECT(code[0] == kNggKernel[0] && code[RDNA4_TRI_PS_OFFSET / 4] == kPsredKernel[0]);
	EXPECT(code[RDNA4_TRI_SHADER_PAD - 1] == 0xbf9f0000u && code[RDNA4_TRI_PS_OFFSET / 4 + RDNA4_TRI_SHADER_PAD - 1] == 0xbf9f0000u);

	/* the G3 image check */
	{
		uint32_t others = 0, red;
		memset(img, 0, sizeof(img));
		for (uint32_t i = 0; i < RDNA4_TRI(kCoveredPixels); i++)
			img[i] = RDNA4_TRI(kCoveredRgba);
		red = rdna4_tri_count(img, &others);   /* (not as one expression: the argument order is unspecified) */
		EXPECT(rdna4_tri_ok(red, others));
		img[9000] = 0xff00ff00u;                                                     /* a stray pixel */
		red = rdna4_tri_count(img, &others);
		EXPECT(!rdna4_tri_ok(red, others));
		img[9000] = 0;
		img[5] = 0;                                                                  /* a missing one */
		red = rdna4_tri_count(img, &others);
		EXPECT(!rdna4_tri_ok(red, others));
	}

	printf(failures ? "%d FAILURES\n" : "all gfx12tri / W12k layout host tests passed\n", failures);
	return failures != 0;
}
