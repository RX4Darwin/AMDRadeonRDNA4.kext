/*
 *  rdna4-run.c
 *  RDNA4FB
 *
 *  Command-line client of the RDNA4FB compute runtime (run as root):
 *
 *    rdna4-run info                        what the runtime reports
 *    rdna4-run selftest [items]            shaders/vadd.cl and bench.cl's
 *                                          lds_reverse (embedded) on the GPU,
 *                                          every result checked, plus the
 *                                          runtime's refusals
 *    rdna4-run bench [small]               host<->GPU copies, VRAM bandwidth
 *                                          and SGEMM GFLOPS (bench.cl), next
 *                                          to the CPU (memcpy, Accelerate's
 *                                          cblas_sgemm), every SGEMM checked
 *                                          exactly; `small` for the emulator
 *    rdna4-run load <file.hsaco> <kernel>  load a code object, describe the
 *                                          kernel, unload it
 *
 *  Exit status 0 only when everything asked for succeeded.
 */

#include "librdna4.h"
#include "bench_codeobj.h"
#include "vadd_codeobj.h"

#include <Accelerate/Accelerate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

// Accelerate is weak-linked (-weak_framework): a recovery system may not
// carry it, and rdna4-run must still run there. Null when it is absent.
extern void cblas_sgemm(const enum CBLAS_ORDER, const enum CBLAS_TRANSPOSE,
                        const enum CBLAS_TRANSPOSE, const int, const int, const int, const float,
                        const float *, const int, const float *, const int, const float, float *,
                        const int) __attribute__((weak_import));

static int openRuntime(rdna4_t *gpu) {
	kern_return_t kr = rdna4_open(gpu);
	if (kr == KERN_SUCCESS)
		return 1;
	if (kr == kIOReturnNotFound)
		fprintf(stderr, "no %s: the kext is not loaded, or the bring-up did not reach "
		        "rdna4-compute=6/7 (see the kernel log)\n", RDNA4_COMPUTE_SERVICE);
	else
		fprintf(stderr, "opening %s failed: %s%s\n", RDNA4_COMPUTE_SERVICE, rdna4_error(kr),
		        geteuid() ? " (run as root)" : "");
	return 0;
}

static int cmdInfo(rdna4_t *gpu) {
	rdna4_info_t in;
	kern_return_t kr = rdna4_info(gpu, &in);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "info: %s\n", rdna4_error(kr));
		return 1;
	}
	printf("ABI %llu, bring-up stage %llu, %s%s\n", in.abi, in.stage,
	       (in.flags & RDNA4_FLAG_READY) ? "ready" : "not ready",
	       (in.flags & RDNA4_FLAG_WEDGED) ? ", WEDGED (a dispatch timed out; reboot)" : "");
	printf("heap: %llu MiB, %llu MiB free, GPU base 0x%llx\n", in.heapBytes >> 20,
	       in.heapFree >> 20, in.heapBase);
	printf("transfers: %s\n", (in.flags & RDNA4_FLAG_DMA)
	       ? "DMA by the GPU's copy engine; buffers from VRAM past the BAR"
	       : "the CPU through the BAR (no DMA)");
	return 0;
}

static int hasDma(rdna4_t *gpu, uint64_t *heapFree) {
	rdna4_info_t in;
	if (rdna4_info(gpu, &in) != KERN_SUCCESS)
		return 0;
	if (heapFree)
		*heapFree = in.heapFree;
	return (in.flags & RDNA4_FLAG_DMA) != 0;
}

// With DMA: a buffer bigger than the BAR's heap, written and read back at
// both ends and in the middle, plus an odd-sized, odd-offset transfer.
// Returns the failures.
static int testLargeBuffer(rdna4_t *gpu) {
	const uint64_t size = 512ull << 20, part = 1u << 20;
	rdna4_buffer_t big;
	kern_return_t kr = rdna4_alloc(gpu, size, &big);
	if (kr != KERN_SUCCESS) {
		printf("  FAIL  a 512 MiB buffer: %s\n", rdna4_error(kr));
		return 1;
	}
	uint8_t *w = malloc(part), *r = malloc(part);
	int fails = 0;
	for (uint64_t i = 0; i < part; i++)
		w[i] = (uint8_t)(i * 7 + 13);
	const uint64_t at[3] = { 0, size / 2 + 12345, size - part };
	for (int k = 0; k < 3 && !fails; k++) {
		memset(r, 0, part);
		if ((kr = rdna4_write(gpu, &big, at[k], w, part)) || (kr = rdna4_read(gpu, &big, at[k], r, part)) ||
		    memcmp(w, r, part)) {
			printf("  FAIL  512 MiB buffer at +0x%llx: %s\n", at[k], kr ? rdna4_error(kr) : "data differs");
			fails++;
		}
	}
	// 777 bytes at an odd offset, read back with a byte of margin each side.
	if (!fails) {
		memset(r, 0xEE, 779);
		if ((kr = rdna4_write(gpu, &big, 3, w + 100, 777)) ||
		    (kr = rdna4_read(gpu, &big, 3, r + 1, 777)) || memcmp(w + 100, r + 1, 777) ||
		    r[0] != 0xEE || r[778] != 0xEE) {
			printf("  FAIL  odd-sized transfer: %s\n", kr ? rdna4_error(kr) : "data differs");
			fails++;
		}
	}
	if (!fails)
		printf("  ok    a 512 MiB buffer (GPU 0x%llx, past the BAR) written and read back at "
		       "both ends and the middle, and an odd-sized transfer\n", big.gpu);
	rdna4_free(gpu, &big);
	free(w);
	free(r);
	return fails;
}

static int checkFailed(const char *what, kern_return_t kr) {
	if (kr == KERN_SUCCESS) {
		printf("  FAIL  %s was accepted\n", what);
		return 1;
	}
	printf("  ok    %s refused (%s)\n", what, rdna4_error(kr));
	return 0;
}

static uint32_t aOf(uint32_t i) { return i * 2654435761u; }
static uint32_t bOf(uint32_t i, uint32_t round) { return (i ^ 0x5A5A5A5Au) + round * 0x01000193u; }

static double nowUs(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

// LDS and a work-group barrier (bench.cl's lds_reverse): each group of 64
// reverses its slice of a[] through LDS. Also checks what the runtime must
// refuse about LDS. Returns the number of failures.
static int testLds(rdna4_t *gpu) {
	int fails = 0;
	const uint32_t items = 4096, bytes = items * 4;
	rdna4_program_t prog;
	kern_return_t kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "lds_reverse", &prog);
	if (kr != KERN_SUCCESS) {
		printf("  FAIL  load lds_reverse: %s\n", rdna4_error(kr));
		return 1;
	}
	rdna4_buffer_t a, b, c;
	uint32_t *ha = malloc(bytes), *hb = malloc(bytes), *hc = malloc(bytes);
	if (!ha || !hb || !hc || (kr = rdna4_alloc(gpu, bytes, &a)) || (kr = rdna4_alloc(gpu, bytes, &b)) ||
	    (kr = rdna4_alloc(gpu, bytes, &c))) {
		printf("  FAIL  LDS test setup: %s\n", rdna4_error(kr));
		return 1;
	}
	for (uint32_t i = 0; i < items; i++) {
		ha[i] = aOf(i);
		hb[i] = bOf(i, 7);
		hc[i] = 0xFFFFFFFFu;
	}
	const uint64_t args[3] = { a.gpu, b.gpu, c.gpu };
	const uint32_t groups[3] = { items / 64, 1, 1 }, size[3] = { 64, 1, 1 };
	if ((kr = rdna4_write(gpu, &a, 0, ha, bytes)) || (kr = rdna4_write(gpu, &b, 0, hb, bytes)) ||
	    (kr = rdna4_write(gpu, &c, 0, hc, bytes)) ||
	    (kr = rdna4_dispatch(gpu, &prog, groups, size, args, sizeof(args), 2000, NULL)) ||
	    (kr = rdna4_read(gpu, &c, 0, hc, bytes))) {
		printf("  FAIL  lds_reverse: %s\n", rdna4_error(kr));
		fails++;
	} else {
		uint32_t bad = 0, first = items;
		for (uint32_t i = 0; i < items; i++) {
			uint32_t g = i & ~63u, want = aOf(g + 63 - (i & 63)) + bOf(i, 7);
			if (hc[i] != want && !bad++)
				first = i;
		}
		if (bad) {
			printf("  FAIL  LDS + barrier: %u of %u wrong, first c[%u] = 0x%08x\n", bad, items, first,
			       hc[first]);
			fails++;
		} else {
			printf("  ok    LDS + barrier: %u items reversed per work-group through %llu bytes "
			       "of LDS\n", items, prog.ldsBytes);
		}
	}
	fails += checkFailed("a dispatch asking for more than 64 KiB of LDS",
	                     rdna4_dispatch_lds(gpu, &prog, groups, size, args, sizeof(args), 65536, 0,
	                                        NULL));
	rdna4_free(gpu, &a);
	rdna4_free(gpu, &b);
	rdna4_free(gpu, &c);
	rdna4_unload(gpu, &prog);
	free(ha);
	free(hb);
	free(hc);
	return fails;
}

static int cmdSelftest(rdna4_t *gpu, uint32_t items) {
	int fails = 0;
	if (!items || items % 64) {
		fprintf(stderr, "selftest: items must be a positive multiple of 64\n");
		return 1;
	}
	if (cmdInfo(gpu))
		return 1;

	rdna4_program_t prog;
	kern_return_t kr = rdna4_load(gpu, kVaddCodeObject, sizeof(kVaddCodeObject), "vadd", &prog);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "selftest: load vadd: %s\n", rdna4_error(kr));
		return 1;
	}
	printf("vadd: %llu-byte image, %llu bytes of kernargs, RSRC1 0x%08llx RSRC2 0x%08llx\n",
	       prog.imageBytes, prog.kernargBytes, prog.rsrc1, prog.rsrc2);

	const uint64_t bytes = (uint64_t)items * 4;
	rdna4_buffer_t a, b, c;
	if ((kr = rdna4_alloc(gpu, bytes, &a)) || (kr = rdna4_alloc(gpu, bytes, &b)) ||
	    (kr = rdna4_alloc(gpu, bytes, &c))) {
		fprintf(stderr, "selftest: alloc %llu bytes: %s\n", bytes, rdna4_error(kr));
		return 1;
	}
	printf("buffers: a 0x%llx, b 0x%llx, c 0x%llx (%llu KiB each)\n", a.gpu, b.gpu, c.gpu,
	       bytes >> 10);

	uint32_t *ha = malloc(bytes), *hb = malloc(bytes), *hc = malloc(bytes);
	if (!ha || !hb || !hc)
		return 1;
	for (uint32_t i = 0; i < items; i++) {
		ha[i] = aOf(i);
		hb[i] = bOf(i, 0);
		hc[i] = 0xFFFFFFFFu;
	}
	if ((kr = rdna4_write(gpu, &a, 0, ha, bytes)) || (kr = rdna4_write(gpu, &b, 0, hb, bytes)) ||
	    (kr = rdna4_write(gpu, &c, 0, hc, bytes))) {
		fprintf(stderr, "selftest: write: %s\n", rdna4_error(kr));
		return 1;
	}

	// 1. One big dispatch.
	const uint64_t args[3] = { a.gpu, b.gpu, c.gpu };
	const uint32_t size[3] = { 64, 1, 1 };
	uint32_t groups[3] = { items / 64, 1, 1 };
	uint64_t us = 0;
	kr = rdna4_dispatch(gpu, &prog, groups, size, args, sizeof(args), 5000, &us);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "selftest: dispatch: %s\n", rdna4_error(kr));
		return 1;
	}
	memset(hc, 0, bytes);
	if ((kr = rdna4_read(gpu, &c, 0, hc, bytes))) {
		fprintf(stderr, "selftest: read: %s\n", rdna4_error(kr));
		return 1;
	}
	uint32_t bad = 0, first = items;
	for (uint32_t i = 0; i < items; i++) {
		if (hc[i] != aOf(i) + 3 * bOf(i, 0)) {
			if (!bad)
				first = i;
			bad++;
		}
	}
	if (bad) {
		printf("  FAIL  %u items: %u wrong, first c[%u] = 0x%08x (want 0x%08x)\n", items, bad,
		       first, hc[first], aOf(first) + 3 * bOf(first, 0));
		fails++;
	} else {
		printf("  ok    %u items of c = a + 3b right, %llu us on the GPU\n", items, us);
	}

	// 2. Many small dispatches: the PM4 ring wraps several times, and each
	//    round's inputs are new, so a stale result shows.
	const uint32_t rounds = 40, small = 256;
	uint32_t roundsBad = 0;
	groups[0] = small / 64;
	for (uint32_t r = 1; r <= rounds && !roundsBad; r++) {
		for (uint32_t i = 0; i < small; i++)
			hb[i] = bOf(i, r);
		if ((kr = rdna4_write(gpu, &b, 0, hb, small * 4)) ||
		    (kr = rdna4_dispatch(gpu, &prog, groups, size, args, sizeof(args), 1000, NULL)) ||
		    (kr = rdna4_read(gpu, &c, 0, hc, small * 4))) {
			printf("  FAIL  round %u: %s\n", r, rdna4_error(kr));
			roundsBad++;
			break;
		}
		for (uint32_t i = 0; i < small; i++) {
			if (hc[i] != aOf(i) + 3 * bOf(i, r)) {
				printf("  FAIL  round %u: c[%u] = 0x%08x (want 0x%08x)\n", r, i, hc[i],
				       aOf(i) + 3 * bOf(i, r));
				roundsBad++;
				break;
			}
		}
	}
	if (!roundsBad)
		printf("  ok    %u back-to-back dispatches of %u items, each checked\n", rounds, small);
	fails += roundsBad != 0;

	// 3. LDS and barriers, on a second kernel from a multi-kernel file.
	fails += testLds(gpu);
	if (hasDma(gpu, NULL))
		fails += testLargeBuffer(gpu);

	// 4. What the runtime must refuse.
	rdna4_program_t bogus = prog;
	bogus.handle ^= 0x10000;                                   // stale generation
	fails += checkFailed("a dispatch of a stale program handle",
	                     rdna4_dispatch(gpu, &bogus, groups, size, args, sizeof(args), 0, NULL));
	const uint32_t tooBig[3] = { 1024, 2, 1 };
	fails += checkFailed("a 2048-item work-group",
	                     rdna4_dispatch(gpu, &prog, groups, tooBig, args, sizeof(args), 0, NULL));
	fails += checkFailed("a read past the end of a buffer", rdna4_read(gpu, &c, bytes - 4, hc, 8));
	fails += checkFailed("a kernel that is not in the code object",
	                     rdna4_load(gpu, kVaddCodeObject, sizeof(kVaddCodeObject), "vsub", &bogus));
	fails += checkFailed("a truncated code object",
	                     rdna4_load(gpu, kVaddCodeObject, 200, "vadd", &bogus));

	// 5. Release everything; a second free of the same buffer must fail.
	kr = rdna4_free(gpu, &a);
	if (kr != KERN_SUCCESS) {
		printf("  FAIL  free: %s\n", rdna4_error(kr));
		fails++;
	}
	fails += checkFailed("a second free of the same buffer", rdna4_free(gpu, &a));
	rdna4_free(gpu, &b);
	rdna4_free(gpu, &c);
	rdna4_unload(gpu, &prog);
	free(ha);
	free(hb);
	free(hc);

	printf("selftest: %s\n", fails ? "FAILED" : "PASS");
	return fails ? 1 : 0;
}

// Multiples of 1/8 in [-1, 1]: every product is a multiple of 1/64 and every
// partial sum of up to 2048 of them fits a float's 24-bit mantissa, so the
// GPU must match the CPU bit for bit, whatever its summation order.
static float mval(uint32_t i, uint32_t seed) {
	uint32_t h = (i + seed) * 2654435761u;
	return (float)((int)((h >> 16) % 17) - 8) / 8.0f;
}

static int cmdBench(rdna4_t *gpu, int small) {
	int fails = 0;
	kern_return_t kr;
	if (cmdInfo(gpu))
		return 1;
	rdna4_program_t copy, sgemm;
	if ((kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "copy", &copy)) ||
	    (kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "sgemm", &sgemm))) {
		fprintf(stderr, "bench: load: %s\n", rdna4_error(kr));
		return 1;
	}
	printf("sgemm: 64x64 tiles, 16x16 work-items, %llu bytes of LDS per work-group\n",
	       sgemm.ldsBytes);
	int vm = 0;
	size_t vmLen = sizeof(vm);
	if (!sysctlbyname("kern.hv_vmm_present", &vm, &vmLen, NULL, 0) && vm)
		printf("note: running in a VM — the GPU is emulated there, its speeds are not real\n");
	const int blas = cblas_sgemm != NULL;
	if (!blas)
		printf("note: Accelerate is not available here: no CPU comparison\n");

	// 1. Host <-> GPU: the CPU copies through the BAR window.
	uint64_t heapFree = 0;
	const int dma = hasDma(gpu, &heapFree);
	const uint64_t xfer = dma ? (small ? (16u << 20) : (256u << 20)) : (small ? (1u << 20) : (16u << 20));
	uint8_t *h1 = malloc(xfer), *h2 = malloc(xfer);
	rdna4_buffer_t x;
	if (!h1 || !h2 || (kr = rdna4_alloc(gpu, xfer, &x))) {
		fprintf(stderr, "bench: transfer buffer: %s\n", rdna4_error(kr));
		return 1;
	}
	for (uint64_t i = 0; i < xfer; i++)
		h1[i] = (uint8_t)(i * 131 + 7);
	double t0 = nowUs();
	kr = rdna4_write(gpu, &x, 0, h1, xfer);
	double t1 = nowUs();
	if (!kr)
		kr = rdna4_read(gpu, &x, 0, h2, xfer);
	double t2 = nowUs();
	if (kr || memcmp(h1, h2, xfer)) {
		printf("  FAIL  host<->GPU copy of %llu MiB: %s\n", xfer >> 20,
		       kr ? rdna4_error(kr) : "data differs");
		fails++;
	} else {
		printf("  ok    host->GPU %.0f MB/s, GPU->host %.0f MB/s (%llu MiB each way)\n",
		       xfer / (t1 - t0), xfer / (t2 - t1), xfer >> 20);
	}
	rdna4_free(gpu, &x);

	// 2. VRAM bandwidth: `copy` reads and writes 16 bytes per work-item.
	const uint64_t cb = dma ? (small ? (4u << 20) : (256u << 20)) : (small ? (1u << 20) : (32u << 20));
	const uint64_t edge = 64u << 10;                  // checked at both ends
	rdna4_buffer_t src, dst;
	if ((kr = rdna4_alloc(gpu, cb, &src)) || (kr = rdna4_alloc(gpu, cb, &dst))) {
		fprintf(stderr, "bench: copy buffers: %s\n", rdna4_error(kr));
		return 1;
	}
	for (uint64_t i = 0; i < edge; i++)
		h1[i] = (uint8_t)(i * 29 + 3);
	rdna4_write(gpu, &src, 0, h1, edge);
	rdna4_write(gpu, &src, cb - edge, h1, edge);
	{
		const uint64_t args[2] = { src.gpu, dst.gpu };
		const uint32_t groups[3] = { (uint32_t)(cb / 16 / 256), 1, 1 }, size[3] = { 256, 1, 1 };
		uint64_t best = ~0ull, us = 0;
		for (int r = 0; r < (small ? 1 : 5) && !kr; r++) {
			kr = rdna4_dispatch(gpu, &copy, groups, size, args, sizeof(args), 10000, &us);
			if (us < best)
				best = us;
		}
		int same = !kr && !rdna4_read(gpu, &dst, 0, h2, edge) && !memcmp(h1, h2, edge) &&
		           !rdna4_read(gpu, &dst, cb - edge, h2, edge) && !memcmp(h1, h2, edge);
		// The CPU's own copy between two buffers of the same size, for scale;
		// called through a volatile pointer, or the unused copy is optimised out.
		double cpuBest = 1e30;
		uint8_t *m1 = malloc(cb), *m2 = malloc(cb);
		void *(*volatile cpy)(void *, const void *, size_t) = memcpy;
		if (m1 && m2) {
			memset(m1, 1, cb);
			memset(m2, 2, cb);
			for (int r = 0; r < 3; r++) {
				double t = nowUs();
				cpy(m2, m1, cb);
				t = nowUs() - t;
				if (t < cpuBest)
					cpuBest = t;
			}
		}
		if (kr || !same) {
			printf("  FAIL  VRAM copy: %s\n", kr ? rdna4_error(kr) : "data differs");
			fails++;
		} else {
			printf("  ok    VRAM copy of %llu MiB: GPU %.1f GB/s (%llu us) | CPU memcpy %.1f GB/s\n",
			       cb >> 20, 2.0 * cb / best / 1000.0, best, 2.0 * cb / cpuBest / 1000.0);
		}
		free(m1);
		free(m2);
	}
	rdna4_free(gpu, &src);
	rdna4_free(gpu, &dst);
	free(h1);
	free(h2);

	// 3. SGEMM, C = A x B: GFLOPS from the best of a few runs, and exact
	//    agreement with the CPU (every element up to 512, sampled rows above).
	static const uint32_t sizes[] = { 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
	const uint32_t maxN = small ? 128 : dma ? 8192 : 2048;
	double bestSpeedup = 0;
	uint32_t bestSpeedupN = 0;
	for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]) && sizes[si] <= maxN; si++) {
		const uint32_t n = sizes[si];
		const uint64_t bytes = (uint64_t)n * n * 4;
		if (3 * bytes > heapFree)
			break;                                  // the heap bounds the sizes
		float *A = malloc(bytes), *B = malloc(bytes), *C = malloc(bytes);
		rdna4_buffer_t a, b, c;
		if (!A || !B || !C || (kr = rdna4_alloc(gpu, bytes, &a)) || (kr = rdna4_alloc(gpu, bytes, &b)) ||
		    (kr = rdna4_alloc(gpu, bytes, &c))) {
			printf("  FAIL  sgemm n=%u: buffers: %s\n", n, rdna4_error(kr));
			fails++;
			break;
		}
		for (uint32_t i = 0; i < n * n; i++) {
			A[i] = mval(i, 1);
			B[i] = mval(i, 2);
		}
		uint8_t args[32] = { 0 };
		memcpy(args, &a.gpu, 8);
		memcpy(args + 8, &b.gpu, 8);
		memcpy(args + 16, &c.gpu, 8);
		memcpy(args + 24, &n, 4);
		const uint32_t groups[3] = { n / 64, n / 64, 1 }, size[3] = { 16, 16, 1 };
		uint64_t best = ~0ull, us = 0;
		kr = rdna4_write(gpu, &a, 0, A, bytes);
		if (!kr)
			kr = rdna4_write(gpu, &b, 0, B, bytes);
		for (int r = 0; r < (small ? 1 : 3) && !kr; r++) {
			kr = rdna4_dispatch(gpu, &sgemm, groups, size, args, 28, 10000, &us);
			if (us < best)
				best = us;
		}
		// The same product on the CPU with Accelerate (all cores), best of 3.
		float *Cc = blas ? malloc(bytes) : NULL;
		double cpuUs = 0;
		if (Cc) {
			cpuUs = 1e30;
			for (int r = 0; r < (n >= 4096 ? 1 : 3); r++) {
				double t = nowUs();
				cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, (int)n, (int)n, (int)n, 1.0f, A,
				            (int)n, B, (int)n, 0.0f, Cc, (int)n);
				t = nowUs() - t;
				if (t < cpuUs)
					cpuUs = t;
			}
		}
		// Rows to check: all of them up to 512, else 64 spread ones. Both the
		// GPU's and Accelerate's results must equal the exact reference.
		const uint32_t rows = n <= 512 ? n : n >= 4096 ? 16 : 64;
		uint32_t bad = 0, br = 0, bc = 0, cpuBad = 0;
		float got = 0, want = 0;
		float *ref = malloc((uint64_t)n * 4);
		for (uint32_t k = 0; k < rows && !kr && ref; k++) {
			const uint32_t r = rows == n ? k : (uint32_t)(((uint64_t)k * 2654435761u) % n);
			kr = rdna4_read(gpu, &c, (uint64_t)r * n * 4, C, (uint64_t)n * 4);
			// The reference row, B walked row by row (cache-friendly); the
			// order does not matter, every sum is exact.
			memset(ref, 0, (uint64_t)n * 4);
			for (uint32_t q = 0; q < n && !kr; q++) {
				const float a = A[r * n + q];
				const float *brow = B + (uint64_t)q * n;
				for (uint32_t j = 0; j < n; j++)
					ref[j] += a * brow[j];
			}
			for (uint32_t j = 0; j < n && !kr; j++) {
				if (Cc && Cc[(uint64_t)r * n + j] != ref[j])
					cpuBad++;
				if (C[j] != ref[j] && !bad++) {
					br = r;
					bc = j;
					got = C[j];
					want = ref[j];
				}
			}
		}
		free(ref);
		const double gpuGflops = 2.0 * n * n * (double)n / best / 1000.0;
		if (kr || bad) {
			if (kr)
				printf("  FAIL  sgemm n=%u: %s\n", n, rdna4_error(kr));
			else
				printf("  FAIL  sgemm n=%u: %u wrong, first C[%u][%u] = %g (want %g)\n", n, bad, br,
				       bc, got, want);
			fails++;
		} else if (Cc) {
			const double cpuGflops = 2.0 * n * n * (double)n / cpuUs / 1000.0;
			printf("  ok    sgemm n=%-4u GPU %8.1f GFLOPS (%8.3f ms) | CPU %7.1f GFLOPS (%8.3f ms, "
			       "Accelerate) | GPU %.1fx; %s exact%s\n", n, gpuGflops, best / 1000.0, cpuGflops,
			       cpuUs / 1000.0, gpuGflops / cpuGflops,
			       rows == n ? "every element" : rows == 64 ? "64 sampled rows" : "16 sampled rows",
			       cpuBad ? " (Accelerate's result differs!)" : "");
			if (gpuGflops / cpuGflops > bestSpeedup) {
				bestSpeedup = gpuGflops / cpuGflops;
				bestSpeedupN = n;
			}
		} else {
			printf("  ok    sgemm n=%-4u GPU %8.1f GFLOPS (%.3f ms), %s exact\n", n, gpuGflops,
			       best / 1000.0,
			       rows == n ? "every element" : rows == 64 ? "64 sampled rows" : "16 sampled rows");
		}
		free(Cc);
		rdna4_free(gpu, &a);
		rdna4_free(gpu, &b);
		rdna4_free(gpu, &c);
		free(A);
		free(B);
		free(C);
		if (kr)
			break;
	}
	rdna4_unload(gpu, &copy);
	rdna4_unload(gpu, &sgemm);
	if (bestSpeedupN)
		printf("bench: SGEMM on the GPU is up to %.1fx the CPU with Accelerate (n=%u)\n",
		       bestSpeedup, bestSpeedupN);
	printf("bench: %s\n", fails ? "FAILED" : "PASS");
	return fails ? 1 : 0;
}

static int cmdLoad(rdna4_t *gpu, const char *path, const char *kernel) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *elf = n > 0 ? malloc((size_t)n) : NULL;
	if (!elf || fread(elf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "%s: could not read\n", path);
		fclose(f);
		return 1;
	}
	fclose(f);
	rdna4_program_t prog;
	kern_return_t kr = rdna4_load(gpu, elf, (size_t)n, kernel, &prog);
	free(elf);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "load %s: %s (the kernel log has the reason)\n", kernel, rdna4_error(kr));
		return 1;
	}
	printf("%s: %llu-byte image, %llu bytes of kernargs, RSRC1 0x%08llx RSRC2 0x%08llx "
	       "RSRC3 0x%08llx, properties 0x%llx (%s)\n", kernel, prog.imageBytes, prog.kernargBytes,
	       prog.rsrc1, prog.rsrc2, prog.rsrc3, prog.properties,
	       (prog.properties & (1u << 10)) ? "wave32" : "wave64");
	rdna4_unload(gpu, &prog);
	return 0;
}

static void usage(void) {
	fprintf(stderr, "usage: rdna4-run info\n"
	                "       rdna4-run selftest [items]\n"
	                "       rdna4-run bench [small]\n"
	                "       rdna4-run load <file.hsaco> <kernel>\n");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		usage();
		return 2;
	}
	rdna4_t gpu;
	int rc;
	if (!strcmp(argv[1], "info") && argc == 2) {
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdInfo(&gpu);
	} else if (!strcmp(argv[1], "selftest") && argc <= 3) {
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdSelftest(&gpu, argc == 3 ? (uint32_t)strtoul(argv[2], NULL, 0) : 65536);
	} else if (!strcmp(argv[1], "bench") && argc <= 3) {
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdBench(&gpu, argc == 3 && !strcmp(argv[2], "small"));
	} else if (!strcmp(argv[1], "load") && argc == 4) {
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdLoad(&gpu, argv[2], argv[3]);
	} else {
		usage();
		return 2;
	}
	rdna4_close(&gpu);
	return rc;
}
