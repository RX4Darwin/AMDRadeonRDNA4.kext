/*
 *  rdna4-run.c
 *  RDNA4FB
 *
 *  Command-line client of the RDNA4FB compute runtime (run as root):
 *
 *    rdna4-run info                        what the runtime reports
 *    rdna4-run selftest [items]            shaders/vadd.cl (embedded) on the
 *                                          GPU, every result checked, plus
 *                                          the runtime's refusals
 *    rdna4-run load <file.hsaco> <kernel>  load a code object, describe the
 *                                          kernel, unload it
 *
 *  Exit status 0 only when everything asked for succeeded.
 */

#include "librdna4.h"
#include "vadd_codeobj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	return 0;
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

	// 3. What the runtime must refuse.
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

	// 4. Release everything; a second free of the same buffer must fail.
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
