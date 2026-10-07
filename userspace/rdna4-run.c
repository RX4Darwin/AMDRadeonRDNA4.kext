/*
 *  rdna4-run.c
 *  RDNA4FB
 *
 *  Command-line client of the RDNA4FB compute runtime (run as root):
 *
 *    rdna4-run info                        what the runtime reports
 *    rdna4-run sleeptest                  debug-only simulated sleep cycle
 *    rdna4-run selftest [items]            shaders/vadd.cl and bench.cl's
 *                                          lds_reverse and wmma16 (embedded)
 *                                          on the GPU, every result checked,
 *                                          plus the runtime's refusals
 *    rdna4-run selftest hang               selftest, then queue recovery
 *    rdna4-run hangtest                    hang recovery followed by vadd
 *    rdna4-run bench [small]               host<->GPU copies, VRAM bandwidth,
 *                                          SGEMM GFLOPS and the matrix units
 *                                          (FP16/BF16 GEMM) from bench.cl,
 *                                          next to the CPU (memcpy,
 *                                          Accelerate's cblas_sgemm), every
 *                                          GEMM checked exactly; `small` for
 *                                          the emulator
 *    rdna4-run anim [seconds]              double-buffered async Mandelbrot
 *    rdna4-run load <file.hsaco> <kernel>  load a code object, describe the
 *                                          kernel, unload it
 *
 *  Exit status 0 only when everything asked for succeeded.
 */

#include "librdna4.h"
#include "bench_codeobj.h"
#include "pm4build.h"
#include "vadd_codeobj.h"

#include <Accelerate/Accelerate.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <pthread.h>
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
	if (in.flags & RDNA4_FLAG_VM)
		printf("GPUVM: VMID %llu, MEC1 pipe %llu queue %llu\n", in.vmid, in.pipe, in.queue);
	return 0;
}

/* Two power-management samples one second apart (SmuMetrics_t fields the
 * SMU averages itself).  GFX activity near 0 with a max clock means the
 * firmware is holding the clock (feature/PM problem); activity near 100 means
 * something really keeps GFX busy; a MetricsCounter that does not advance means
 * the table is stale. */
static void printSensorsPm(int n, const RDNA4SensorsEx *s) {
	printf("sensors-pm[%d]: GFXCLK avg pre-DS %u post-DS %u MHz (CurrClock %u, AvgCurrent %u A: "
	       "not trusted on driver IF 0x33), "
	       "GFX activity %u %%, UCLK activity %u %%, VDD_GFX %u mV, "
	       "socket %u W, hotspot %u C, MetricsCounter %u%s\n", n,
	       s->avgGfxclkPreDsMHz, s->avgGfxclkPostDsMHz, s->currGfxclkMHz, s->vddGfxCurrentA,
	       s->gfxActivity, s->uclkActivity, s->vddGfxMv,
	       s->socketPowerW, s->hotspotTempC, s->metricsCounter,
	       (s->flags & RDNA4_SENSORS_EX_LIVE) ? "" : " (STALE: table not rewritten)");
	printf("sensors-pm[%d]: throttling %%", n);
	int any = 0;
	for (int i = 0; i < RDNA4_SENSORS_THROTTLERS; i++) {
		if (s->throttlingPercent[i]) {
			printf(" #%d=%u", i, s->throttlingPercent[i]);
			any = 1;
		}
	}
	printf("%s (mask 0x%x; #12-17 = TDC/PPT, THROTTLER_*_BIT in smu14_driver_if_v14_0.h)\n",
	       any ? "" : " none", s->throttlingMask);
}

static int cmdSensorsPm(rdna4_t *gpu) {
	RDNA4SensorsEx a, b;
	kern_return_t kr = rdna4_sensors_ex(gpu, &a);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "sensors-pm: %s\n", rdna4_error(kr));
		return 1;
	}
	sleep(1);
	kr = rdna4_sensors_ex(gpu, &b);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "sensors-pm: %s\n", rdna4_error(kr));
		return 1;
	}
	printSensorsPm(1, &a);
	printSensorsPm(2, &b);
	const int live = (a.flags & b.flags & RDNA4_SENSORS_EX_LIVE) &&
	                 b.metricsCounter != a.metricsCounter;
	printf("sensors-pm: MetricsCounter %u -> %u: table %s\n", a.metricsCounter,
	       b.metricsCounter, live ? "LIVE" : "NOT advancing (stale or SMU idle-gated)");
	/* The card's driver interface is 0x33, amdgpu's tables are 0x2E: trust the
	 * new fields only if they look like what they should be. */
	int plausible = 1;
	const char *why = "";
	const RDNA4SensorsEx *both[2] = { &a, &b };
	/* CurrClock and AvgCurrent are not used: round 3 on the card showed them
	 * constant (1000 MHz) or absurd (tens of kA), so their offsets are wrong
	 * for driver IF 0x33. The averages, activity, power and counter were right. */
	for (int i = 0; i < 2; i++) {
		const RDNA4SensorsEx *s = both[i];
		if (s->gfxActivity > 100 || s->uclkActivity > 100) {
			plausible = 0; why = "activity above 100 %";
		} else if (s->vddGfxMv < 300 || s->vddGfxMv > 1400) {
			plausible = 0; why = "VDD_GFX outside 300-1400 mV";
		}
	}
	if (live) {
		const uint32_t delta = b.metricsCounter - a.metricsCounter;   /* about 1000 per second */
		if (delta < 10 || delta > 1000000) {
			plausible = 0; why = "MetricsCounter delta not about 1 ms per tick";
		}
	}
	/* The clock is the averaged one (pre-DS: what the SMU reports when busy). */
	const uint32_t avgClock = b.avgGfxclkPreDsMHz;
	if (live && !plausible)
		printf("sensors-pm: verdict INCONCLUSIVE (activity field implausible: %s; read the raw fields)\n", why);
	else if (live && b.gfxActivity >= 50)
		printf("sensors-pm: verdict SMU-REPORTS-GFX-BUSY (%u %% activity at an average of %u MHz; compare with the "
		       "pm: survey lines' GRBM/CP/queue state to see which engine, if any, is busy)\n",
		       b.gfxActivity, avgClock);
	else if (live && b.gfxActivity < 10)
		printf("sensors-pm: verdict LOW-ACTIVITY (%u %% at an average of %u MHz; whether that clock is the DPM "
		       "maximum needs the rdna4-gfxpm cap probe)\n", b.gfxActivity, avgClock);
	else
		printf("sensors-pm: verdict %s\n", live ? "no anomaly" : "INCONCLUSIVE");
	return 0;
}

static int cmdSensors(rdna4_t *gpu) {
	RDNA4Sensors s;
	kern_return_t kr = rdna4_sensors(gpu, &s);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "sensors: %s\n", rdna4_error(kr));
		return 1;
	}
	printf("sensors: edge %u C, hotspot %u C, GFX %u MHz, memory %u MHz, "
	       "socket %u W, fan %u RPM\n", s.edgeTempC, s.hotspotTempC,
	       s.gfxClockMHz, s.memoryClockMHz, s.socketPowerW, s.fanRpm);
	/* the power-management view is additive: its failure does not fail sensors */
	(void)cmdSensorsPm(gpu);
	return 0;
}

static int cmdSleepTest(rdna4_t *gpu) {
	kern_return_t kr = rdna4_sleep_test(gpu, 1);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "sleeptest: phase 1: %s\n", rdna4_error(kr));
		return 1;
	}
	printf("sleeptest: sleep selector acknowledged; waiting for emulator reset\n");
	fflush(stdout);
	/* RDNA4_POST sets the QEMU sleep-reset property during this window. */
	sleep(15);
	kr = rdna4_sleep_test(gpu, 2);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "sleeptest: phase 2: %s\n", rdna4_error(kr));
		return 1;
	}
	printf("sleeptest: wake selector acknowledged; waiting for re-bring-up\n");
	fflush(stdout);
	sleep(15);
	rdna4_info_t info;
	kr = rdna4_info(gpu, &info);
	if (kr != kIOReturnAborted) {
		fprintf(stderr, "sleeptest: pre-sleep client returned %s, want aborted\n",
		        rdna4_error(kr));
		return 1;
	}
	printf("sleeptest: pre-sleep client aborted as expected\n");
	return 0;
}

static int cmdQuiesce(rdna4_t *gpu) {
	kern_return_t kr = rdna4_quiesce(gpu);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "quiesce: %s\n", rdna4_error(kr));
		return 1;
	}
	printf("quiesce: shutdown/restart path complete; card is safe to reset\n");
	return 0;
}

static int cmdVsync(rdna4_t *gpu, uint32_t frames) {
	uint64_t previous = 0, minNs = UINT64_MAX, maxNs = 0, sumNs = 0;
	uint32_t intervals = 0;
	for (uint32_t i = 0; i < frames; i++) {
		uint64_t count = 0, timeNs = 0;
		kern_return_t kr = rdna4_wait_vblank(gpu, 1000, &count, &timeNs);
		if (kr != KERN_SUCCESS) {
			fprintf(stderr, "vsync: vblank %u/%u: %s\n", i + 1, frames, rdna4_error(kr));
			return 1;
		}
		if (previous) {
			uint64_t interval = timeNs - previous;
			if (interval < minNs)
				minNs = interval;
			if (interval > maxNs)
				maxNs = interval;
			sumNs += interval;
			intervals++;
		}
		previous = timeNs;
	}
	if (!intervals) {
		printf("vsync: %u vblank, no interval\n", frames);
		return 0;
	}
	const double average = (double)sumNs / intervals;
	printf("vsync: %u vblanks, refresh %.3f Hz, interval %.0f ns (jitter %llu..%llu ns)\n",
	       frames, 1.0e9 / average, average, minNs, maxNs);
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

static uint32_t aOf(uint32_t i);
static uint32_t bOf(uint32_t i, uint32_t round);

static int testVadd(rdna4_t *gpu, uint32_t items) {
	int fails = 0;
	const uint64_t bytes = (uint64_t)items * 4;
	rdna4_program_t prog;
	kern_return_t kr = rdna4_load(gpu, kVaddCodeObject, sizeof(kVaddCodeObject), "vadd", &prog);
	if (kr != KERN_SUCCESS) {
		printf("  FAIL  recovery vadd load: %s\n", rdna4_error(kr));
		return 1;
	}
	rdna4_buffer_t a, b, c;
	uint32_t *ha = malloc(bytes), *hb = malloc(bytes), *hc = malloc(bytes);
	if (!ha || !hb || !hc || (kr = rdna4_alloc(gpu, bytes, &a)) ||
	    (kr = rdna4_alloc(gpu, bytes, &b)) || (kr = rdna4_alloc(gpu, bytes, &c))) {
		printf("  FAIL  recovery vadd setup: %s\n", rdna4_error(kr));
		free(ha); free(hb); free(hc); rdna4_unload(gpu, &prog);
		return 1;
	}
	for (uint32_t i = 0; i < items; i++) {
		ha[i] = aOf(i);
		hb[i] = bOf(i, 0);
		hc[i] = 0xffffffffu;
	}
	const uint64_t args[3] = { a.gpu, b.gpu, c.gpu };
	const uint32_t groups[3] = { items / 64, 1, 1 }, size[3] = { 64, 1, 1 };
	if ((kr = rdna4_write(gpu, &a, 0, ha, bytes)) || (kr = rdna4_write(gpu, &b, 0, hb, bytes)) ||
	    (kr = rdna4_write(gpu, &c, 0, hc, bytes)) ||
	    (kr = rdna4_dispatch(gpu, &prog, groups, size, args, sizeof(args), 2000, NULL)) ||
	    (kr = rdna4_read(gpu, &c, 0, hc, bytes))) {
		printf("  FAIL  recovery vadd: %s\n", rdna4_error(kr));
		fails++;
	} else {
		for (uint32_t i = 0; i < items; i++) {
			if (hc[i] != aOf(i) + 3 * bOf(i, 0)) {
				printf("  FAIL  recovery vadd c[%u] = 0x%08x (want 0x%08x)\n", i, hc[i],
				       aOf(i) + 3 * bOf(i, 0));
				fails++;
				break;
			}
		}
		if (!fails)
			printf("  ok    recovery vadd: %u results correct after queue reset\n", items);
	}
	rdna4_free(gpu, &a); rdna4_free(gpu, &b); rdna4_free(gpu, &c);
	rdna4_unload(gpu, &prog);
	free(ha); free(hb); free(hc);
	return fails;
}

static int cmdHangtest(rdna4_t *gpu) {
	int fails = 0;
	rdna4_program_t spin;
	kern_return_t kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "spin", &spin);
	if (kr != KERN_SUCCESS) {
		printf("hangtest: load spin: %s\n", rdna4_error(kr));
		return 1;
	}
	rdna4_buffer_t flag;
	uint32_t zero = 0;
	const uint32_t groups[3] = { 1, 1, 1 }, size[3] = { 1, 1, 1 };
	if ((kr = rdna4_alloc(gpu, sizeof(zero), &flag)) || (kr = rdna4_write(gpu, &flag, 0, &zero, sizeof(zero)))) {
		printf("hangtest: flag setup: %s\n", rdna4_error(kr));
		rdna4_unload(gpu, &spin);
		return 1;
	}
	const uint64_t args[1] = { flag.gpu };
	kr = rdna4_dispatch(gpu, &spin, groups, size, args, sizeof(args), 200, NULL);
	if (kr != kIOReturnTimeout) {
		printf("  FAIL  spin dispatch returned %s (want timeout)\n", rdna4_error(kr));
		fails++;
	} else {
		printf("  ok    spin dispatch timed out; queue recovery was attempted\n");
	}
	rdna4_free(gpu, &flag);
	rdna4_unload(gpu, &spin);
	fails += testVadd(gpu, 256);
	printf("hangtest: %s\n", fails ? "FAILED" : "PASS");
	return fails ? 1 : 0;
}

typedef struct VmPeerJob {
	rdna4_t *gpu;
	rdna4_program_t prog;
	rdna4_buffer_t a, b, c;
	uint32_t ha[256], hb[256], hc[256];
	kern_return_t kr;
	int bad;
} VmPeerJob;

static void *runVmPeer(void *opaque) {
	VmPeerJob *j = (VmPeerJob *)opaque;
	const uint64_t args[3] = { j->a.gpu, j->b.gpu, j->c.gpu };
	const uint32_t groups[3] = { 4, 1, 1 }, size[3] = { 64, 1, 1 };
	j->kr = rdna4_dispatch(j->gpu, &j->prog, groups, size, args, sizeof(args), 2000, NULL);
	if (!j->kr)
		j->kr = rdna4_read(j->gpu, &j->c, 0, j->hc, sizeof(j->hc));
	if (!j->kr)
		for (uint32_t i = 0; i < 256; i++)
			if (j->hc[i] != j->ha[i] + 3 * j->hb[i])
				j->bad++;
	return NULL;
}

/* Prove that two VM clients have private address spaces and queues. */
static int testVmIsolationAndPeers(rdna4_t *a) {
	rdna4_t b;
	memset(&b, 0, sizeof(b));
	kern_return_t kr = rdna4_open(&b);
	if (kr != KERN_SUCCESS) {
		printf("  FAIL  VM isolation: second client: %s\n", rdna4_error(kr));
		return 1;
	}
	int fails = 0;
	rdna4_program_t copy = {};
	rdna4_buffer_t aPad1 = {}, aPad2 = {}, aSrc = {}, bDst = {};
	uint32_t src[1024], dst[1024];
	memset(dst, 0, sizeof(dst));
	if ((kr = rdna4_load(&b, kBenchCodeObject, sizeof(kBenchCodeObject), "copy", &copy)) ||
	    (kr = rdna4_alloc(a, 65536, &aPad1)) || (kr = rdna4_alloc(a, 65536, &aPad2)) ||
	    (kr = rdna4_alloc(a, 65536, &aSrc)) || (kr = rdna4_alloc(&b, 65536, &bDst))) {
		printf("  FAIL  VM isolation setup: %s\n", rdna4_error(kr));
		fails++;
		goto isolation_cleanup;
	}
	for (uint32_t i = 0; i < 1024; i++)
		src[i] = 0xA5000000u ^ i;
	if ((kr = rdna4_write(a, &aSrc, 0, src, sizeof(src))) ||
	    (kr = rdna4_write(&b, &bDst, 0, dst, sizeof(dst)))) {
		printf("  FAIL  VM isolation write: %s\n", rdna4_error(kr));
		fails++;
		goto isolation_cleanup;
	}
	{
		const uint64_t args[2] = { aSrc.gpu, bDst.gpu };
		const uint32_t groups[3] = { 1, 1, 1 }, size[3] = { 256, 1, 1 };
		kr = rdna4_dispatch(&b, &copy, groups, size, args, sizeof(args), 2000, NULL);
	}
	if (kr || (kr = rdna4_read(&b, &bDst, 0, dst, sizeof(dst)))) {
		printf("  FAIL  VM isolation dispatch: %s\n", rdna4_error(kr));
		fails++;
	} else {
		/* B's read of A's VA must fault. A fault reads the GPU's fault-default
		 * page, so the property is "none of A's data", not "all zero". */
		uint32_t leaked = 0, other = 0;
		for (uint32_t i = 0; i < 1024; i++) {
			if (dst[i] == src[i])
				leaked++;
			else if (dst[i])
				other++;
		}
		fails += leaked;
		printf("  %s  VM isolation: client B could not read client A's VA", leaked ? "FAIL" : "ok");
		if (leaked || other)
			printf(" (%u of A's words seen, %u other non-zero words)", leaked, other);
		printf("\n");
	}

isolation_cleanup:
	if (bDst.handle)
		rdna4_free(&b, &bDst);
	if (aSrc.handle)
		rdna4_free(a, &aSrc);
	if (aPad2.handle)
		rdna4_free(a, &aPad2);
	if (aPad1.handle)
		rdna4_free(a, &aPad1);
	if (copy.handle)
		rdna4_unload(&b, &copy);

	VmPeerJob jobs[2];
	memset(jobs, 0, sizeof(jobs));
	jobs[0].gpu = a;
	jobs[1].gpu = &b;
	for (int j = 0; j < 2; j++) {
		if ((kr = rdna4_load(jobs[j].gpu, kVaddCodeObject, sizeof(kVaddCodeObject), "vadd",
		                     &jobs[j].prog)) ||
		    (kr = rdna4_alloc(jobs[j].gpu, sizeof(jobs[j].ha), &jobs[j].a)) ||
		    (kr = rdna4_alloc(jobs[j].gpu, sizeof(jobs[j].hb), &jobs[j].b)) ||
		    (kr = rdna4_alloc(jobs[j].gpu, sizeof(jobs[j].hc), &jobs[j].c))) {
			printf("  FAIL  VM peer setup: %s\n", rdna4_error(kr));
			fails++;
			goto peers_cleanup;
		}
		for (uint32_t i = 0; i < 256; i++) {
			jobs[j].ha[i] = aOf(i + j * 17);
			jobs[j].hb[i] = bOf(i, j + 3);
			jobs[j].hc[i] = 0xffffffffu;
		}
		if ((kr = rdna4_write(jobs[j].gpu, &jobs[j].a, 0, jobs[j].ha, sizeof(jobs[j].ha))) ||
		    (kr = rdna4_write(jobs[j].gpu, &jobs[j].b, 0, jobs[j].hb, sizeof(jobs[j].hb))) ||
		    (kr = rdna4_write(jobs[j].gpu, &jobs[j].c, 0, jobs[j].hc, sizeof(jobs[j].hc)))) {
			printf("  FAIL  VM peer write: %s\n", rdna4_error(kr));
			fails++;
			goto peers_cleanup;
		}
	}
	{
		pthread_t threads[2];
		int made = 0;
		for (; made < 2; made++)
			if (pthread_create(&threads[made], NULL, runVmPeer, &jobs[made]))
				break;
		if (made != 2) {
			printf("  FAIL  VM peer dispatch: could not create both threads\n");
			fails++;
			for (int j = 0; j < made; j++)
				pthread_join(threads[j], NULL);
		} else {
			for (int j = 0; j < 2; j++)
				pthread_join(threads[j], NULL);
			for (int j = 0; j < 2; j++)
				if (jobs[j].kr || jobs[j].bad) {
					printf("  FAIL  VM peer %d: %s%s\n", j,
					       jobs[j].kr ? rdna4_error(jobs[j].kr) : "wrong results",
					       jobs[j].bad ? " (data mismatch)" : "");
					fails++;
				}
			if (!fails)
				printf("  ok    two VM clients dispatched concurrently; both results exact\n");
		}
	}

peers_cleanup:
	for (int j = 0; j < 2; j++) {
		if (jobs[j].c.handle)
			rdna4_free(jobs[j].gpu, &jobs[j].c);
		if (jobs[j].b.handle)
			rdna4_free(jobs[j].gpu, &jobs[j].b);
		if (jobs[j].a.handle)
			rdna4_free(jobs[j].gpu, &jobs[j].a);
		if (jobs[j].prog.handle)
			rdna4_unload(jobs[j].gpu, &jobs[j].prog);
	}
	rdna4_close(&b);
	return fails;
}

static uint32_t aOf(uint32_t i) { return i * 2654435761u; }
static uint32_t bOf(uint32_t i, uint32_t round) { return (i ^ 0x5A5A5A5Au) + round * 0x01000193u; }

/* The host buffer test deliberately uses only the returned CPU pointers.  A
 * stale GPU VA is dispatched once after Free so the VM fault path is covered
 * without attempting to read the unmapped handle. */
static int testHostZeroCopy(rdna4_t *gpu, const rdna4_program_t *prog, uint32_t items) {
	const uint64_t bytes = (uint64_t)items * 4;
	rdna4_buffer_t a = {}, b = {}, c = {};
	uint32_t *ha = NULL, *hb = NULL, *hc = NULL;
	void *pa = NULL, *pb = NULL, *pc = NULL;
	kern_return_t kr;
	int fails = 0;
	if ((kr = rdna4_alloc_host(gpu, bytes, &a, &pa)) ||
	    (kr = rdna4_alloc_host(gpu, bytes, &b, &pb)) ||
	    (kr = rdna4_alloc_host(gpu, bytes, &c, &pc))) {
		printf("  FAIL  host buffer allocation: %s\n", rdna4_error(kr));
		if (c.handle) rdna4_free(gpu, &c);
		if (b.handle) rdna4_free(gpu, &b);
		if (a.handle) rdna4_free(gpu, &a);
		return 1;
	}
	ha = (uint32_t *)pa;
	hb = (uint32_t *)pb;
	hc = (uint32_t *)pc;
	for (uint32_t i = 0; i < items; i++) {
		ha[i] = aOf(i);
		hb[i] = bOf(i, 23);
		hc[i] = 0xdeadbeefu;
	}
	const uint64_t args[3] = { a.gpu, b.gpu, c.gpu };
	const uint32_t groups[3] = { items / 64, 1, 1 }, size[3] = { 64, 1, 1 };
	if ((kr = rdna4_dispatch(gpu, prog, groups, size, args, sizeof(args), 5000, NULL))) {
		printf("  FAIL  host-memory vadd dispatch: %s\n", rdna4_error(kr));
		fails++;
	} else {
		uint32_t bad = 0;
		for (uint32_t i = 0; i < items; i++)
			bad += hc[i] != ha[i] + 3 * hb[i];
		if (bad) {
			printf("  FAIL  zero-copy vadd: %u of %u CPU-visible results wrong\n", bad, items);
			fails++;
		} else {
			printf("  ok    zero-copy vadd: %u items read/written through CPU pointers\n", items);
		}
	}
	const uint64_t stale = c.gpu;
	if (rdna4_free(gpu, &c)) {
		printf("  FAIL  free host result buffer\n");
		fails++;
	} else {
		const uint64_t badArgs[3] = { a.gpu, b.gpu, stale };
		kr = rdna4_dispatch(gpu, prog, groups, size, badArgs, sizeof(badArgs), 1000, NULL);
		if (kr == kIOReturnTimeout) {
			printf("  FAIL  dispatch through freed host VA timed out\n");
			fails++;
		} else {
			printf("  ok    dispatch through freed host VA faulted cleanly (%s)\n",
			       kr ? rdna4_error(kr) : "GPU fence landed after fault");
		}
	}
	rdna4_free(gpu, &b);
	rdna4_free(gpu, &a);
	return fails;
}

static uint32_t recordVaddIb(uint32_t *ib, const rdna4_program_t *prog,
                             uint64_t kernarg, uint32_t groups) {
	uint32_t n = 0;
	const uint32_t zero = 0, all[2] = { 0xffffffffu, 0xffffffffu };
	const uint32_t none4[4] = { 0, 0, 0, 0 }, start[3] = { 0, 0, 0 };
	const uint32_t threads[3] = { 64, 1, 1 };
	const uint32_t pgm[2] = { (uint32_t)(prog->gpu >> 8), (uint32_t)(prog->gpu >> 40) };
	const uint32_t rsrc[2] = { (uint32_t)prog->rsrc1, (uint32_t)prog->rsrc2 };
	const uint32_t rsrc3 = (uint32_t)prog->rsrc3;
	const uint32_t user[2] = { (uint32_t)kernarg, (uint32_t)(kernarg >> 32) };
	n += rdna4_pm4_acquire_mem(ib + n, RDNA4_PM4_GCR_MEM_SYNC);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_LO, pgm, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC1, rsrc, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC3, &rsrc3, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_RESOURCE_LIM, &zero, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_TMPRING, &zero, 1);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE0, all, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE2, all, 2);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE4, none4, 4);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_START_X, start, 3);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_NUM_THREAD_X, threads, 3);
	n += rdna4_pm4_set_sh_reg(ib + n, RDNA4_PM4_COMPUTE_USER_DATA0, user, 2);
	n += rdna4_pm4_dispatch_direct(ib + n, groups, 1, 1,
	                               RDNA4_PM4_DISPATCH_SHADER_EN |
	                               RDNA4_PM4_DISPATCH_FORCE_START0 |
	                               (prog->properties & (1u << 10) ? RDNA4_PM4_DISPATCH_WAVE32 : 0));
	return n;
}

/* Record the same vadd setup as launch(), then let the kernel chain it through
 * the client's VMID. The data and kernargs are read and checked through the
 * returned host pointers only. */
static int testIndirectBuffers(rdna4_t *gpu, const rdna4_program_t *prog, int badIb) {
	const uint32_t items = 256, groups = items / 64, stride = 256;
	const uint64_t bytes = items * 4ull;
	rdna4_buffer_t ib = {}, kernarg = {}, a = {}, b = {}, c = {};
	uint32_t *ibCpu = NULL, *kernargCpu = NULL, *ha = NULL, *hb = NULL, *hc = NULL;
	void *p = NULL;
	uint64_t fence = 0, ns = 0;
	kern_return_t kr;
	int fails = 0;
	if ((kr = rdna4_alloc_host(gpu, 4096, &ib, &p)) ||
	    (ibCpu = (uint32_t *)p) == NULL ||
	    (kr = rdna4_alloc_host(gpu, 4096, &kernarg, &p)) ||
	    (kernargCpu = (uint32_t *)p) == NULL ||
	    (kr = rdna4_alloc_host(gpu, bytes, &a, &p)) || !(ha = (uint32_t *)p) ||
	    (kr = rdna4_alloc_host(gpu, bytes, &b, &p)) || !(hb = (uint32_t *)p) ||
	    (kr = rdna4_alloc_host(gpu, bytes, &c, &p)) || !(hc = (uint32_t *)p)) {
		printf("  FAIL  SubmitIb host setup: %s\n", rdna4_error(kr));
		fails++;
		goto done;
	}
	for (uint32_t i = 0; i < items; i++) {
		ha[i] = aOf(i + 41);
		hb[i] = bOf(i, 41);
		hc[i] = 0xdeadbeefu;
	}
	const uint64_t args[3] = { a.gpu, b.gpu, c.gpu };
	memcpy(kernargCpu, args, sizeof(args));
	uint32_t ibDwords = recordVaddIb(ibCpu, prog, kernarg.gpu, groups);
	if (rdna4_submit_ib(gpu, &ib, 0, ibDwords, &fence) != KERN_SUCCESS ||
	    (kr = rdna4_wait_fence(gpu, fence, 5000, &ns)) != KERN_SUCCESS) {
		printf("  FAIL  SubmitIb vadd/wait: %s\n", rdna4_error(kr));
		fails++;
	} else {
		uint32_t bad = 0;
		for (uint32_t i = 0; i < items; i++)
			bad += hc[i] != ha[i] + 3 * hb[i];
		printf("  %s  SubmitIb vadd: %u CPU-visible results, fence %llu ns\n",
		       bad ? "FAIL" : "ok", items, (unsigned long long)ns);
		fails += bad != 0;
	}

	/* Keep several fences outstanding, then wait the oldest one.  The wait
	 * must use >= semantics and leave the newer fence ordered behind it. */
	uint64_t first = 0, third = 0;
	kr = KERN_SUCCESS;
	for (uint32_t i = 0; i < 3; i++) {
		kr = rdna4_submit_ib(gpu, &ib, 0, ibDwords, &fence);
		if (kr != KERN_SUCCESS)
			break;
		if (i == 0)
			first = fence;
		if (i == 2)
			third = fence;
	}
	if (kr == KERN_SUCCESS)
		kr = rdna4_wait_fence(gpu, first, 5000, &ns);
	if (kr != KERN_SUCCESS) {
		printf("  FAIL  three IBs then oldest-fence wait: %s\n", rdna4_error(kr));
		fails++;
	} else {
		printf("  ok    three IBs back-to-back; oldest fence %llu returned before fence %llu\n",
		       (unsigned long long)first, (unsigned long long)third);
	}
	if (kr == KERN_SUCCESS && third != first) {
		kr = rdna4_wait_fence(gpu, third, 5000, &ns);
		if (kr != KERN_SUCCESS) {
			printf("  FAIL  newest fence after oldest-fence wait: %s\n", rdna4_error(kr));
			fails++;
		}
	}

	/* Ten distinct IB locations are chained before waiting for the last one. */
	for (uint32_t i = 0; i < 10; i++)
		recordVaddIb(ibCpu + (i * stride) / 4, prog, kernarg.gpu, groups);
	uint64_t last = 0;
	for (uint32_t i = 0; i < 10; i++) {
		kr = rdna4_submit_ib(gpu, &ib, i * stride, ibDwords, &last);
		if (kr != KERN_SUCCESS)
			break;
	}
	if (kr == KERN_SUCCESS)
		kr = rdna4_wait_fence(gpu, last, 5000, &ns);
	uint32_t bad = 0;
	for (uint32_t i = 0; i < items; i++)
		bad += hc[i] != ha[i] + 3 * hb[i];
	if (kr != KERN_SUCCESS || bad) {
		printf("  FAIL  ten back-to-back IBs: %s%s\n", rdna4_error(kr), bad ? " (data differs)" : "");
		fails++;
	} else {
		printf("  ok    ten back-to-back IBs completed in order; last fence %llu ns\n",
		       (unsigned long long)ns);
	}
	rdna4_buffer_t outside = ib;
	outside.gpu += 0x100000;
	fence = 0;
	fails += checkFailed("an IB VA outside the client's buffers",
	                     rdna4_submit_ib(gpu, &outside, 0, ibDwords, &fence));

	if (badIb) {
		ibCpu[0] = 0xffffffffu;
		kr = rdna4_submit_ib(gpu, &ib, 0, 1, &fence);
		if (kr == KERN_SUCCESS)
			kr = rdna4_wait_fence(gpu, fence, 200, &ns);
		if (kr != kIOReturnTimeout) {
			printf("  FAIL  invalid IB returned %s, want timeout after recovery\n", rdna4_error(kr));
			fails++;
		} else {
			printf("  ok    invalid IB timed out; W6 queue recovery completed\n");
		}
		ibDwords = recordVaddIb(ibCpu, prog, kernarg.gpu, groups);
		kr = rdna4_submit_ib(gpu, &ib, 0, ibDwords, &fence);
		if (kr == KERN_SUCCESS)
			kr = rdna4_wait_fence(gpu, fence, 5000, &ns);
		bad = 0;
		for (uint32_t i = 0; i < items; i++)
			bad += hc[i] != ha[i] + 3 * hb[i];
		if (kr != KERN_SUCCESS || bad) {
			printf("  FAIL  vadd after invalid IB recovery: %s%s\n", rdna4_error(kr),
			       bad ? " (data differs)" : "");
			fails++;
		} else {
			printf("  ok    vadd after invalid IB recovery\n");
		}
	}

done:
	if (c.handle) rdna4_free(gpu, &c);
	if (b.handle) rdna4_free(gpu, &b);
	if (a.handle) rdna4_free(gpu, &a);
	if (kernarg.handle) rdna4_free(gpu, &kernarg);
	if (ib.handle) rdna4_free(gpu, &ib);
	return fails;
}

static double nowUs(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static volatile sig_atomic_t showSignal;

static void showSignalHandler(int signalNumber) {
	(void)signalNumber;
	showSignal = 1;
}

static uint32_t mandelbrotIteration(float cx, float cy) {
	float zx = 0.0f, zy = 0.0f;
	uint32_t iteration = 0;
	for (; iteration < 256u; iteration++) {
		const float zx2 = zx * zx;
		const float zy2 = zy * zy;
		if (zx2 + zy2 > 4.0f)
			break;
		const float nextZx = zx2 - zy2 + cx;
		zy = fmaf(2.0f * zx, zy, cy);
		zx = nextZx;
	}
	return iteration;
}

static uint32_t mandelbrotColorForIteration(uint32_t iteration) {
	if (iteration >= 256u)
		return 0xff000000u;
	const float t = (float)iteration * (1.0f / 255.0f);
	const uint32_t r = (uint32_t)fmaf(246.0f, t, 9.0f);
	const uint32_t g = (uint32_t)fmaf(200.0f, 1.0f - t, 20.0f);
	const uint32_t b = (uint32_t)fmaf(175.0f, t, 80.0f);
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

static uint32_t mandelbrotColorArgs(uint32_t x, uint32_t y, float x0, float dx,
	                                  float y0, float dy, uint32_t *iterationOut) {
	const float cx = x0 + (float)x * dx;
	const float cy = y0 + (float)y * dy;
	const uint32_t iteration = mandelbrotIteration(cx, cy);
	if (iterationOut)
		*iterationOut = iteration;
	return mandelbrotColorForIteration(iteration);
}

static int rdna4VmmPresent(void) {
	int vm = 0;
	size_t vmLen = sizeof(vm);
	return !sysctlbyname("kern.hv_vmm_present", &vm, &vmLen, NULL, 0) && vm;
}

static int cmdShow(rdna4_t *gpu, uint32_t seconds) {
	uint32_t width = 0, height = 0, pitch = 0;
	kern_return_t kr = rdna4_display_query(gpu, &width, &height, &pitch);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "show: display query: %s\n", rdna4_error(kr));
		return 1;
	}
	if (!width || !height || !pitch || (uint64_t)pitch * height > UINT64_MAX / 4) {
		fprintf(stderr, "show: invalid display geometry %ux%u pitch %u\n", width, height, pitch);
		return 1;
	}
	const uint64_t bytes = (uint64_t)pitch * height * 4;
	/* Keep the VM proof visibly interpreter-backed while making the full-screen
	 * Mandelbrot complete before the runtime's bounded dispatch wait. */
	const uint32_t scale = rdna4VmmPresent() ? 20u : 1u;
	const float x0 = -2.3f, dx = 3.2f / (float)width;
	const float y0 = -1.1f, dy = 2.2f / (float)height;
	printf("show: geometry %ux%u pitch %u (%llu bytes)\n", width, height, pitch, bytes);
	printf("show: Mandelbrot scale %u (%s)\n", scale,
	       scale == 1 ? "real hardware" : "VM interpreter");

	rdna4_program_t prog = { 0 };
	rdna4_buffer_t buf = { 0 };
	int loaded = 0, allocated = 0, presented = 0, rc = 1;
	kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "mandelbrot", &prog);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "show: load mandelbrot: %s\n", rdna4_error(kr));
		goto done;
	}
	loaded = 1;
	kr = rdna4_alloc(gpu, bytes, &buf);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "show: allocate %llu bytes: %s\n", bytes, rdna4_error(kr));
		goto done;
	}
	allocated = 1;
	{
		uint8_t args[40] = { 0 };
		memcpy(args, &buf.gpu, sizeof(buf.gpu));
		memcpy(args + 8, &width, sizeof(width));
		memcpy(args + 12, &height, sizeof(height));
		memcpy(args + 16, &pitch, sizeof(pitch));
		memcpy(args + 20, &x0, sizeof(x0));
		memcpy(args + 24, &dx, sizeof(dx));
		memcpy(args + 28, &y0, sizeof(y0));
		memcpy(args + 32, &dy, sizeof(dy));
		memcpy(args + 36, &scale, sizeof(scale));
		const uint32_t groups[3] = { (width + 16u * scale - 1) / (16u * scale),
		                              (height + 16u * scale - 1) / (16u * scale), 1 };
		const uint32_t groupSize[3] = { 16, 16, 1 };
		uint64_t kernelUs = 0;
		kr = rdna4_dispatch(gpu, &prog, groups, groupSize, args, sizeof(args), 10000, &kernelUs);
		if (kr != KERN_SUCCESS) {
			fprintf(stderr, "show: Mandelbrot dispatch: %s\n", rdna4_error(kr));
			goto done;
		}
		printf("show: Mandelbrot kernel %llu us\n", kernelUs);
	}

	showSignal = 0;
	signal(SIGINT, showSignalHandler);
	signal(SIGTERM, showSignalHandler);
	{
		const double presentStart = nowUs();
		kr = rdna4_present(gpu, &buf, 0, &width, &height, &pitch);
		printf("show: present latency %.0f us\n", nowUs() - presentStart);
	}
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "show: present: %s\n", rdna4_error(kr));
		goto done;
	}
	presented = 1;
	{
		uint32_t mismatches = 0, tolerated = 0, hardMismatches = 0;
		for (uint32_t sy = 1; sy <= 8; sy++) {
			const uint32_t y = (uint64_t)sy * height / 9;
			for (uint32_t sx = 1; sx <= 8; sx++) {
				const uint32_t x = (uint64_t)sx * width / 9;
				const uint32_t sampleX = x - x % scale, sampleY = y - y % scale;
				uint32_t expectedIteration = 0;
				uint32_t got = 0;
				kr = rdna4_read(gpu, &buf, ((uint64_t)y * pitch + x) * 4, &got, sizeof(got));
				const uint32_t want = mandelbrotColorArgs(sampleX, sampleY, x0, dx, y0, dy,
				                                                &expectedIteration);
				const uint32_t nearLo = expectedIteration ?
					mandelbrotColorForIteration(expectedIteration - 1) : want;
				const uint32_t nearHi = expectedIteration < 256u ?
					mandelbrotColorForIteration(expectedIteration + 1) : want;
				if (kr != KERN_SUCCESS || got != want) {
					mismatches++;
					if (kr == KERN_SUCCESS && (got == nearLo || got == nearHi))
						tolerated++;
					else
						hardMismatches++;
				}
			}
		}
		printf("show: CPU spot check 64 pixels, mismatches %u (iteration +/-1 %u, hard %u)\n",
		       mismatches, tolerated, hardMismatches);
		if (kr != KERN_SUCCESS || hardMismatches > 0 || tolerated > 4)
			goto done;
	}
	for (uint32_t left = seconds * 10; left && !showSignal; left--)
		usleep(100000);
	if (showSignal)
		fprintf(stderr, "show: interrupted; restoring desktop\n");
	kr = rdna4_restore(gpu);
	presented = 0;
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "show: restore: %s\n", rdna4_error(kr));
		goto done;
	}
	printf("show: desktop restored\n");
	rc = showSignal ? 1 : 0;

done:
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	if (presented) {
		kern_return_t restore = rdna4_restore(gpu);
		if (restore != KERN_SUCCESS) {
			fprintf(stderr, "show: cleanup restore: %s\n", rdna4_error(restore));
			rc = 1;
		}
	}
	if (allocated)
		rdna4_free(gpu, &buf);
	if (loaded)
		rdna4_unload(gpu, &prog);
	return rc;
}

typedef struct AnimPending {
	uint64_t id;
	double submittedUs;
} AnimPending;

typedef struct AnimStats {
	uint32_t rendered;
	uint32_t presented;
	uint64_t noFrameVblanks;
	uint64_t firstFrame;
	uint64_t lastFrame;
	double firstUs;
	double lastUs;
	double latencySumUs;
	double latencyWorstUs;
} AnimStats;

static int animWaitOne(rdna4_t *gpu, AnimPending *pending, AnimStats *stats) {
	uint64_t frame = 0;
	kern_return_t kr = rdna4_wait_present(gpu, pending->id, 2000, &frame);
	const double now = nowUs();
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "anim: wait present %llu: %s\n", pending->id, rdna4_error(kr));
		return 1;
	}
	if (!stats->presented) {
		stats->firstFrame = frame;
		stats->firstUs = now;
	} else if (frame > stats->lastFrame + 1) {
		stats->noFrameVblanks += frame - stats->lastFrame - 1;
	}
	stats->lastFrame = frame;
	stats->lastUs = now;
	stats->presented++;
	const double latency = now - pending->submittedUs;
	stats->latencySumUs += latency;
	if (latency > stats->latencyWorstUs)
		stats->latencyWorstUs = latency;
	return 0;
}

static int cmdAnim(rdna4_t *gpu, uint32_t seconds) {
	uint32_t width = 0, height = 0, pitch = 0;
	kern_return_t kr = rdna4_display_query(gpu, &width, &height, &pitch);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "anim: display query: %s\n", rdna4_error(kr));
		return 1;
	}
	if (!width || !height || !pitch || (uint64_t)pitch * height > UINT64_MAX / 4) {
		fprintf(stderr, "anim: invalid display geometry %ux%u pitch %u\n", width, height, pitch);
		return 1;
	}
	const uint64_t bytes = (uint64_t)pitch * height * 4;
	const uint32_t scale = rdna4VmmPresent() ? 20u : 1u;
	printf("anim: geometry %ux%u pitch %u (%llu bytes), scale %u (%s)\n", width, height,
	       pitch, bytes, scale, scale == 1 ? "real hardware" : "VM interpreter");

	rdna4_program_t prog = { 0 };
	rdna4_buffer_t buffers[2] = { { 0 }, { 0 } };
	AnimPending pending[2] = { { 0 }, { 0 } };
	AnimStats stats = { 0 };
	uint32_t pendingCount = 0, rendered = 0;
	int loaded = 0, allocated = 0, ownsPresentation = 0, rc = 1;
	kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "mandelbrot_zoom", &prog);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "anim: load mandelbrot_zoom: %s\n", rdna4_error(kr));
		goto done;
	}
	loaded = 1;
	for (int i = 0; i < 2; i++) {
		kr = rdna4_alloc(gpu, bytes, &buffers[i]);
		if (kr != KERN_SUCCESS) {
			fprintf(stderr, "anim: allocate buffer %d: %s\n", i, rdna4_error(kr));
			goto done;
		}
		allocated++;
	}

	showSignal = 0;
	signal(SIGINT, showSignalHandler);
	signal(SIGTERM, showSignalHandler);
	const double startUs = nowUs();
	const double endUs = startUs + (seconds ? seconds : 1u) * 1e6;
	for (uint32_t frameNo = 0; !showSignal && nowUs() < endUs; frameNo++) {
		const uint32_t index = frameNo & 1u;
		const float zoom = 1.0f + (float)frameNo * 0.035f;
		const float x0 = -0.7f - 1.6f / zoom;
		const float dx = 3.2f / ((float)width * zoom);
		const float y0 = -1.1f / zoom;
		const float dy = 2.2f / ((float)height * zoom);
		uint8_t args[40] = { 0 };
		memcpy(args, &buffers[index].gpu, sizeof(buffers[index].gpu));
		memcpy(args + 8, &width, sizeof(width));
		memcpy(args + 12, &height, sizeof(height));
		memcpy(args + 16, &pitch, sizeof(pitch));
		memcpy(args + 20, &x0, sizeof(x0));
		memcpy(args + 24, &dx, sizeof(dx));
		memcpy(args + 28, &y0, sizeof(y0));
		memcpy(args + 32, &dy, sizeof(dy));
		memcpy(args + 36, &scale, sizeof(scale));
		const uint32_t groups[3] = { (width + 16u * scale - 1) / (16u * scale),
		                              (height + 16u * scale - 1) / (16u * scale), 1 };
		const uint32_t groupSize[3] = { 16, 16, 1 };
		uint64_t kernelUs = 0;
		kr = rdna4_dispatch(gpu, &prog, groups, groupSize, args, sizeof(args), 10000, &kernelUs);
		if (kr != KERN_SUCCESS) {
			fprintf(stderr, "anim: frame %u dispatch: %s\n", frameNo, rdna4_error(kr));
			goto done;
		}
		if (!frameNo) {
			uint32_t frameMismatches = 0, tolerated = 0, hardMismatches = 0;
			for (uint32_t sy = 1; sy <= 8; sy++) {
				const uint32_t y = (uint64_t)sy * height / 9;
				for (uint32_t sx = 1; sx <= 8; sx++) {
					const uint32_t x = (uint64_t)sx * width / 9;
					const uint32_t sampleX = x - x % scale, sampleY = y - y % scale;
					uint32_t expectedIteration = 0;
					uint32_t got = 0;
					const uint32_t want = mandelbrotColorArgs(sampleX, sampleY, x0, dx, y0, dy,
					                                                &expectedIteration);
					const kern_return_t readKr = rdna4_read(gpu, &buffers[index],
												 ((uint64_t)y * pitch + x) * 4,
												 &got, sizeof(got));
					const uint32_t lo = expectedIteration ?
						mandelbrotColorForIteration(expectedIteration - 1) : want;
					const uint32_t hi = expectedIteration < 256u ?
						mandelbrotColorForIteration(expectedIteration + 1) : want;
					if (readKr != KERN_SUCCESS || got != want) {
						frameMismatches++;
						if (readKr == KERN_SUCCESS && (got == lo || got == hi))
							tolerated++;
						else
							hardMismatches++;
					}
				}
			}
			printf("anim: CPU spot check 64 pixels, mismatches %u (iteration +/-1 %u, hard %u)\n",
			       frameMismatches, tolerated, hardMismatches);
			if (hardMismatches > 0 || tolerated > 4)
			goto done;
		}
		rendered++;
		stats.rendered = rendered;
		uint64_t id = 0;
		const double submittedUs = nowUs();
		kr = rdna4_present_async(gpu, &buffers[index], 0, &id);
		if (kr != KERN_SUCCESS) {
			fprintf(stderr, "anim: frame %u present async: %s\n", frameNo, rdna4_error(kr));
			goto done;
		}
		ownsPresentation = 1;
		pending[pendingCount++] = (AnimPending) { id, submittedUs };
		if (pendingCount == 2) {
			if (animWaitOne(gpu, &pending[0], &stats))
				goto done;
			pending[0] = pending[1];
			pendingCount = 1;
		}
	}
	while (!showSignal && pendingCount) {
		if (animWaitOne(gpu, &pending[0], &stats))
			goto done;
		if (pendingCount == 2)
			pending[0] = pending[1];
		pendingCount--;
	}
	if (showSignal)
		fprintf(stderr, "anim: interrupted; restoring desktop\n");
	kr = rdna4_restore(gpu);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "anim: restore: %s\n", rdna4_error(kr));
		goto done;
	}
	ownsPresentation = 0;
	{
		const double elapsedUs = (stats.lastUs > startUs ? stats.lastUs : nowUs()) - startUs;
		const double fps = elapsedUs > 0 ? stats.presented * 1e6 / elapsedUs : 0;
		const double avgLatency = stats.presented ? stats.latencySumUs / stats.presented : 0;
		const double refresh = stats.presented > 1 && stats.lastUs > stats.firstUs &&
		                      stats.lastFrame >= stats.firstFrame
			? (stats.lastFrame - stats.firstFrame) * 1e6 / (stats.lastUs - stats.firstUs) : 0;
		printf("anim: frames rendered %u, presented %u\n", stats.rendered, stats.presented);
		printf("anim: average present wait %.0f us, worst %.0f us\n", avgLatency,
		       stats.latencyWorstUs);
		printf("anim: %.2f FPS vs OTG %.2f Hz, vblanks with no frame %llu\n", fps, refresh,
		       stats.noFrameVblanks);
		printf("anim: desktop restored\n");
	}
	rc = showSignal ? 1 : 0;

done:
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	if (ownsPresentation) {
		kern_return_t restore = rdna4_restore(gpu);
		if (restore != KERN_SUCCESS) {
			fprintf(stderr, "anim: cleanup restore: %s\n", rdna4_error(restore));
			rc = 1;
		}
	}
	for (int i = 0; i < allocated; i++)
		rdna4_free(gpu, &buffers[i]);
	if (loaded)
		rdna4_unload(gpu, &prog);
	return rc;
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

// A float that is exactly an IEEE half (or zero) as its 16 bits; and as
// BF16, the top half of the float. The matrix tests only use such values.
static uint16_t toHalf(float f) {
	uint32_t u;
	memcpy(&u, &f, 4);
	uint32_t sign = (u >> 16) & 0x8000u, exp = (u >> 23) & 0xff;
	if (!exp)
		return (uint16_t)sign;
	return (uint16_t)(sign | ((exp - 112) << 10) | ((u >> 13) & 0x3ff));
}

static uint16_t toBf16(float f) {
	uint32_t u;
	memcpy(&u, &f, 4);
	return (uint16_t)(u >> 16);
}

// The matrix units' register layout (bench.cl's wmma16: one wave, one
// 16x16x16 FP16 product, D = A x B + C). Lane l holds, 8 values each, row
// l % 16 of A and column l % 16 of B for k = (l / 16) * 8 + i, and D/C rows
// (l / 16) * 8 + i of column l % 16. C[r][c] = 64 * (16r + c), far apart,
// so a wrong result shows which element of D landed there. Returns the
// failures.
static int testWmma(rdna4_t *gpu) {
	rdna4_program_t prog;
	kern_return_t kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "wmma16", &prog);
	if (kr != KERN_SUCCESS) {
		printf("  FAIL  load wmma16: %s\n", rdna4_error(kr));
		return 1;
	}
	float A[16][16], B[16][16], D[16][16];
	uint16_t fa[32][8], fb[32][8];
	float fc[32][8];
	for (int r = 0; r < 16; r++) {
		for (int c = 0; c < 16; c++) {
			A[r][c] = (float)((r * 16 + c) * 7 % 17 - 8) / 8.0f;
			B[r][c] = (float)((r * 16 + c) * 11 % 17 - 8) / 8.0f;
		}
	}
	for (int r = 0; r < 16; r++) {
		for (int c = 0; c < 16; c++) {
			D[r][c] = 64.0f * (float)(r * 16 + c);
			for (int k = 0; k < 16; k++)
				D[r][c] += A[r][k] * B[k][c];
		}
	}
	for (int l = 0; l < 32; l++) {
		for (int i = 0; i < 8; i++) {
			const int rc = l % 16, k = (l / 16) * 8 + i;
			fa[l][i] = toHalf(A[rc][k]);
			fb[l][i] = toHalf(B[k][rc]);
			fc[l][i] = 64.0f * (float)(k * 16 + rc);          // C[k][rc]: row k, column rc
		}
	}
	rdna4_buffer_t a, b, c;
	int fails = 0;
	if ((kr = rdna4_alloc(gpu, sizeof(fa), &a)) || (kr = rdna4_alloc(gpu, sizeof(fb), &b)) ||
	    (kr = rdna4_alloc(gpu, sizeof(fc), &c))) {
		printf("  FAIL  WMMA test setup: %s\n", rdna4_error(kr));
		return 1;
	}
	const uint64_t args[3] = { a.gpu, b.gpu, c.gpu };
	const uint32_t groups[3] = { 1, 1, 1 }, size[3] = { 32, 1, 1 };
	if ((kr = rdna4_write(gpu, &a, 0, fa, sizeof(fa))) || (kr = rdna4_write(gpu, &b, 0, fb, sizeof(fb))) ||
	    (kr = rdna4_write(gpu, &c, 0, fc, sizeof(fc))) ||
	    (kr = rdna4_dispatch(gpu, &prog, groups, size, args, sizeof(args), 2000, NULL)) ||
	    (kr = rdna4_read(gpu, &c, 0, fc, sizeof(fc)))) {
		printf("  FAIL  wmma16: %s\n", rdna4_error(kr));
		fails++;
	} else {
		int bad = 0;
		for (int l = 0; l < 32; l++) {
			for (int i = 0; i < 8; i++) {
				const int row = (l / 16) * 8 + i, col = l % 16;
				if (fc[l][i] == D[row][col])
					continue;
				if (bad++ < 6) {
					// Which element of C this is nearest to, and what remains
					// of the product once that is taken off.
					const int q = (int)(fc[l][i] / 64.0f + (fc[l][i] < 0 ? -0.5f : 0.5f));
					printf("        lane %2d value %d: got %g (want %g = D[%d][%d]); nearest C[%d][%d], "
					       "product part %g\n", l, i, fc[l][i], D[row][col], row, col, q / 16, q % 16,
					       fc[l][i] - 64.0f * (float)q);
				}
			}
		}
		if (bad) {
			printf("  FAIL  matrix units (WMMA 16x16x16 FP16): %d of 256 results wrong\n", bad);
			fails++;
		} else {
			printf("  ok    matrix units: one WMMA 16x16x16 FP16 product, all 256 results exact\n");
		}
	}
	rdna4_free(gpu, &a);
	rdna4_free(gpu, &b);
	rdna4_free(gpu, &c);
	rdna4_unload(gpu, &prog);
	return fails;
}

static int cmdSelftest(rdna4_t *gpu, uint32_t items, int badIb) {
	int fails = 0;
	if (!items || items % 64) {
		fprintf(stderr, "selftest: items must be a positive multiple of 64\n");
		return 1;
	}
	if (cmdInfo(gpu))
		return 1;
	rdna4_info_t initial;
	const int gpuVm = rdna4_info(gpu, &initial) == KERN_SUCCESS &&
	                  (initial.flags & RDNA4_FLAG_VM);
	if (gpuVm)
		fails += testVmIsolationAndPeers(gpu);
	else {
		rdna4_buffer_t host = {};
		void *cpu = NULL;
		fails += checkFailed("AllocHost without GPUVM",
		                     rdna4_alloc_host(gpu, 4096, &host, &cpu));
		rdna4_buffer_t ib = { 0, 0x1000, 4 };
		uint64_t fence = 0;
		fails += checkFailed("SubmitIb without GPUVM",
		                     rdna4_submit_ib(gpu, &ib, 0, 1, &fence));
	}

	rdna4_program_t prog;
	kern_return_t kr = rdna4_load(gpu, kVaddCodeObject, sizeof(kVaddCodeObject), "vadd", &prog);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "selftest: load vadd: %s\n", rdna4_error(kr));
		return 1;
	}
	printf("vadd: %llu-byte image, %llu bytes of kernargs, RSRC1 0x%08llx RSRC2 0x%08llx\n",
	       prog.imageBytes, prog.kernargBytes, prog.rsrc1, prog.rsrc2);
	if (gpuVm)
		fails += testHostZeroCopy(gpu, &prog, items);
	if (gpuVm)
		fails += testIndirectBuffers(gpu, &prog, badIb);

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

	// 3. LDS and barriers, on a second kernel from a multi-kernel file; the
	//    matrix units' layout.
	fails += testLds(gpu);
	fails += testWmma(gpu);
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
// partial sum of up to 8192 of them fits a float's 24-bit mantissa, so the
// GPU must match the CPU bit for bit, whatever its summation order. They
// are exact as FP16 and BF16 too.
static float mval(uint32_t i, uint32_t seed) {
	uint32_t h = (i + seed) * 2654435761u;
	return (float)((int)((h >> 16) % 17) - 8) / 8.0f;
}

// The matrix units: C = A x Bt^T with 16-bit inputs (bench.cl's hgemm for
// FP16, bf16gemm for BF16) and FP32 accumulation, both checked exactly
// against the CPU (mval()'s values are exact in both formats). Next to
// the FP32 SGEMM on the GPU and Accelerate at the same n, from the SGEMM
// pass (sgemmGflops / cpuGflops, 0 where it did not run). Returns the
// failures; *bestTflops gets the fastest FP16 result.
static const uint32_t kGemmSizes[] = { 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
#define GEMM_SIZES (sizeof(kGemmSizes) / sizeof(kGemmSizes[0]))

static int benchGemm16(rdna4_t *gpu, rdna4_program_t prog[2], uint32_t maxN, uint64_t heapFree,
                       int small, const double *sgemmGflops, const double *cpuGflops,
                       double *bestTflops, uint32_t *bestN) {
	static const char *const name[2] = { "hgemm FP16", "hgemm BF16" };
	int fails = 0;
	kern_return_t kr = KERN_SUCCESS;
	for (unsigned si = 1; si < GEMM_SIZES && kGemmSizes[si] <= maxN; si++) {
		const uint32_t n = kGemmSizes[si];
		const uint64_t nn = (uint64_t)n * n;
		if (8 * nn > heapFree)
			break;                                  // 2 + 2 + 4 bytes per element
		const uint32_t rows = n <= 512 ? n : n >= 4096 ? 16 : 64;
		float *A = malloc(nn * 4), *Bt = malloc(nn * 4), *C = malloc((uint64_t)n * 4);
		float *ref = malloc((uint64_t)rows * n * 4);
		uint16_t *h = malloc(nn * 2);
		rdna4_buffer_t a, b, c;
		if (!A || !Bt || !C || !ref || !h || (kr = rdna4_alloc(gpu, nn * 2, &a)) ||
		    (kr = rdna4_alloc(gpu, nn * 2, &b)) || (kr = rdna4_alloc(gpu, nn * 4, &c))) {
			printf("  FAIL  hgemm n=%u: buffers: %s\n", n, rdna4_error(kr));
			return fails + 1;
		}
		for (uint64_t i = 0; i < nn; i++) {
			A[i] = mval((uint32_t)i, 3);
			Bt[i] = mval((uint32_t)i, 4);
		}
		// The exact reference for the checked rows: C[r][j] = A[r] . Bt[j].
		for (uint32_t k = 0; k < rows; k++) {
			const uint32_t r = rows == n ? k : (uint32_t)(((uint64_t)k * 2654435761u) % n);
			const float *ar = A + (uint64_t)r * n;
			for (uint32_t j = 0; j < n; j++) {
				const float *bj = Bt + (uint64_t)j * n;
				float s = 0;
				for (uint32_t q = 0; q < n; q++)
					s += ar[q] * bj[q];
				ref[(uint64_t)k * n + j] = s;
			}
		}
		uint8_t args[32] = { 0 };
		memcpy(args, &a.gpu, 8);
		memcpy(args + 8, &b.gpu, 8);
		memcpy(args + 16, &c.gpu, 8);
		memcpy(args + 24, &n, 4);
		const uint32_t groups[3] = { n / 128, n / 128, 1 }, size[3] = { 256, 1, 1 };
		for (int bf = 0; bf < 2 && !kr; bf++) {
			for (uint64_t i = 0; i < nn; i++)
				h[i] = bf ? toBf16(A[i]) : toHalf(A[i]);
			kr = rdna4_write(gpu, &a, 0, h, nn * 2);
			for (uint64_t i = 0; i < nn && !kr; i++)
				h[i] = bf ? toBf16(Bt[i]) : toHalf(Bt[i]);
			if (!kr)
				kr = rdna4_write(gpu, &b, 0, h, nn * 2);
			uint64_t best = ~0ull, us = 0;
			for (int r = 0; r < (small ? 1 : 5) && !kr; r++) {
				kr = rdna4_dispatch(gpu, &prog[bf], groups, size, args, 28, 10000, &us);
				if (us < best)
					best = us;
			}
			uint32_t bad = 0, br = 0, bc = 0;
			float got = 0, want = 0;
			for (uint32_t k = 0; k < rows && !kr; k++) {
				const uint32_t r = rows == n ? k : (uint32_t)(((uint64_t)k * 2654435761u) % n);
				kr = rdna4_read(gpu, &c, (uint64_t)r * n * 4, C, (uint64_t)n * 4);
				for (uint32_t j = 0; j < n && !kr; j++) {
					if (C[j] != ref[(uint64_t)k * n + j] && !bad++) {
						br = r;
						bc = j;
						got = C[j];
						want = ref[(uint64_t)k * n + j];
					}
				}
			}
			const char *checked = rows == n ? "every element" : rows == 64 ? "64 sampled rows"
			                                                                 : "16 sampled rows";
			const double gflops = 2.0 * n * n * (double)n / best / 1000.0;
			if (kr || bad) {
				if (kr)
					printf("  FAIL  %s n=%u: %s\n", name[bf], n, rdna4_error(kr));
				else
					printf("  FAIL  %s n=%u: %u wrong, first C[%u][%u] = %g (want %g)\n", name[bf], n,
					       bad, br, bc, got, want);
				fails++;
				break;
			}
			printf("  ok    %s n=%-4u GPU %8.1f GFLOPS (%8.3f ms)", name[bf], n, gflops, best / 1000.0);
			if (sgemmGflops[si] > 0)
				printf(" | %.1fx FP32 sgemm", gflops / sgemmGflops[si]);
			if (cpuGflops[si] > 0)
				printf(gflops / cpuGflops[si] < 10 ? " | %.1fx Accelerate" : " | %.0fx Accelerate",
				       gflops / cpuGflops[si]);
			printf("; %s exact\n", checked);
			if (!bf && gflops / 1000.0 > *bestTflops) {
				*bestTflops = gflops / 1000.0;
				*bestN = n;
			}
		}
		rdna4_free(gpu, &a);
		rdna4_free(gpu, &b);
		rdna4_free(gpu, &c);
		free(A);
		free(Bt);
		free(C);
		free(ref);
		free(h);
		if (kr || fails)
			return fails ? fails : 1;
	}
	return fails;
}

static int cmdBench(rdna4_t *gpu, int small) {
	int fails = 0;
	kern_return_t kr;
	if (cmdInfo(gpu))
		return 1;
	rdna4_program_t copy, sgemm, gemm16[2];
	if ((kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "copy", &copy)) ||
	    (kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "sgemm", &sgemm)) ||
	    (kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "hgemm", &gemm16[0])) ||
	    (kr = rdna4_load(gpu, kBenchCodeObject, sizeof(kBenchCodeObject), "bf16gemm", &gemm16[1]))) {
		fprintf(stderr, "bench: load: %s\n", rdna4_error(kr));
		return 1;
	}
	printf("sgemm: 64x64 tiles, 16x16 work-items, %llu bytes of LDS per work-group\n",
	       sgemm.ldsBytes);
	printf("hgemm: 128x128 tiles, 8 waves of WMMA 16x16x16 (FP16/BF16 in, FP32 sums), %llu bytes "
	       "of LDS per work-group\n", gemm16[0].ldsBytes);
	int inVm = 0;
	size_t vmLen = sizeof(inVm);
	if (!sysctlbyname("kern.hv_vmm_present", &inVm, &vmLen, NULL, 0) && inVm)
		printf("note: running in a VM — the GPU is emulated there, its speeds are not real\n");
	rdna4_info_t info = {};
	const int gpuVm = rdna4_info(gpu, &info) == KERN_SUCCESS &&
	                  (info.flags & RDNA4_FLAG_VM);
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

	/* GPU-side read from a system-memory PTE into a VRAM destination. */
	if (gpuVm) {
		const uint64_t hostBytes = small ? (4u << 20) : (64u << 20);
		rdna4_buffer_t host = {}, device = {};
		void *cpu = NULL;
		if ((kr = rdna4_alloc_host(gpu, hostBytes, &host, &cpu)) ||
		    (kr = rdna4_alloc(gpu, hostBytes, &device))) {
			printf("  FAIL  host-memory benchmark setup: %s\n", rdna4_error(kr));
			fails++;
			if (device.handle) rdna4_free(gpu, &device);
			if (host.handle) rdna4_free(gpu, &host);
		} else {
			for (uint64_t i = 0; i < hostBytes; i++)
				((uint8_t *)cpu)[i] = (uint8_t)(i * 17 + 11);
			const uint64_t args[2] = { host.gpu, device.gpu };
			const uint32_t groups[3] = { (uint32_t)(hostBytes / 16 / 256), 1, 1 };
			const uint32_t size[3] = { 256, 1, 1 };
			uint64_t best = ~0ull;
			for (int r = 0; r < (small ? 1 : 5) && !kr; r++) {
				uint64_t us = 0;
				kr = rdna4_dispatch(gpu, &copy, groups, size, args, sizeof(args), 10000, &us);
				if (us < best) best = us;
			}
			uint8_t *check = malloc(64u << 10);
			int same = check && !kr && !rdna4_read(gpu, &device, 0, check, 64u << 10) &&
			           !memcmp(check, cpu, 64u << 10);
			if (kr || !same) {
				printf("  FAIL  GPU read from host memory: %s\n",
				       kr ? rdna4_error(kr) : "data differs");
				fails++;
			} else {
				printf("  ok    GPU read host->VRAM %llu MiB: %.1f GB/s (%llu us)\n",
				       hostBytes >> 20, hostBytes / (double)best / 1000.0, best);
			}
			free(check);
			rdna4_free(gpu, &device);
			rdna4_free(gpu, &host);
		}
	}

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
	const uint32_t maxN = small ? 128 : dma ? 8192 : 2048;
	double bestSpeedup = 0, sgemmAt[GEMM_SIZES] = { 0 }, cpuAt[GEMM_SIZES] = { 0 };
	uint32_t bestSpeedupN = 0;
	for (unsigned si = 0; si < GEMM_SIZES && kGemmSizes[si] <= maxN; si++) {
		const uint32_t n = kGemmSizes[si];
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
			sgemmAt[si] = gpuGflops;
			cpuAt[si] = cpuGflops;
			if (gpuGflops / cpuGflops > bestSpeedup) {
				bestSpeedup = gpuGflops / cpuGflops;
				bestSpeedupN = n;
			}
		} else {
			printf("  ok    sgemm n=%-4u GPU %8.1f GFLOPS (%.3f ms), %s exact\n", n, gpuGflops,
			       best / 1000.0,
			       rows == n ? "every element" : rows == 64 ? "64 sampled rows" : "16 sampled rows");
			sgemmAt[si] = gpuGflops;
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
	// 4. The matrix units.
	double bestTflops = 0;
	uint32_t bestTflopsN = 0;
	fails += benchGemm16(gpu, gemm16, maxN, heapFree, small, sgemmAt, cpuAt, &bestTflops,
	                     &bestTflopsN);

	rdna4_unload(gpu, &copy);
	rdna4_unload(gpu, &sgemm);
	rdna4_unload(gpu, &gemm16[0]);
	rdna4_unload(gpu, &gemm16[1]);
	if (bestSpeedupN)
		printf("bench: SGEMM on the GPU is up to %.1fx the CPU with Accelerate (n=%u)\n",
		       bestSpeedup, bestSpeedupN);
	if (bestTflopsN)
		printf("bench: the matrix units reach %.1f TFLOPS in FP16 (n=%u)\n", bestTflops, bestTflopsN);
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
	                "       rdna4-run sleeptest\n"
	                "       rdna4-run quiesce\n"
	                "       rdna4-run sensors\n"
	                "       rdna4-run selftest [items]\n"
	                "       rdna4-run selftest hang\n"
	                "       rdna4-run selftest ibbad\n"
	                "       rdna4-run hangtest\n"
	                "       rdna4-run bench [small]\n"
	                "       rdna4-run vsync [n]\n"
	                "       rdna4-run show [seconds]\n"
	                "       rdna4-run anim [seconds]\n"
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
	} else if (!strcmp(argv[1], "sleeptest") && argc == 2) {
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdSleepTest(&gpu);
	} else if (!strcmp(argv[1], "quiesce") && argc == 2) {
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdQuiesce(&gpu);
	} else if (!strcmp(argv[1], "sensors") && argc == 2) {
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdSensors(&gpu);
	} else if (!strcmp(argv[1], "selftest") && argc <= 3) {
		if (!openRuntime(&gpu))
			return 1;
		if (argc == 3 && !strcmp(argv[2], "hang")) {
			rc = cmdSelftest(&gpu, 65536, 0);
			if (!rc)
				rc = cmdHangtest(&gpu);
		} else if (argc == 3 && !strcmp(argv[2], "ibbad")) {
			rc = cmdSelftest(&gpu, 65536, 1);
		} else {
			rc = cmdSelftest(&gpu, argc == 3 ? (uint32_t)strtoul(argv[2], NULL, 0) : 65536, 0);
		}
	} else if (!strcmp(argv[1], "hangtest") && argc == 2) {
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdHangtest(&gpu);
	} else if (!strcmp(argv[1], "bench") && argc <= 3) {
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdBench(&gpu, argc == 3 && !strcmp(argv[2], "small"));
	} else if (!strcmp(argv[1], "vsync") && argc <= 3) {
		uint32_t frames = argc == 3 ? (uint32_t)strtoul(argv[2], NULL, 0) : 120;
		if (!frames || frames > 100000) {
			fprintf(stderr, "vsync: n must be 1..100000\n");
			return 2;
		}
		if (!openRuntime(&gpu))
			return 1;
		rc = cmdVsync(&gpu, frames);
	} else if (!strcmp(argv[1], "show") && argc <= 3) {
		if (!openRuntime(&gpu))
			return 1;
		const uint32_t seconds = argc == 3 ? (uint32_t)strtoul(argv[2], NULL, 0) : 5;
		rc = cmdShow(&gpu, seconds);
	} else if (!strcmp(argv[1], "anim") && argc <= 3) {
		if (!openRuntime(&gpu))
			return 1;
		const uint32_t seconds = argc == 3 ? (uint32_t)strtoul(argv[2], NULL, 0) : 5;
		rc = cmdAnim(&gpu, seconds);
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
