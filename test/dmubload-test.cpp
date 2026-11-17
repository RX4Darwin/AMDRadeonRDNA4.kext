// dmubload-test: src/dmubload.cpp against Linux's own run (test/ref/dmub_start_linux.inc, tools/gen/pipegen/
// dmubgen.sh): the same windows, and the same register reads and writes in the same order, each read answered
// with what Linux read there.
#include <stdint.h>
#include <stdio.h>
#include "../src/dmubload.hpp"
#include "ref/dmub_start_linux.inc"

static const uint32_t kSeg2 = 0x34c0;   // dmub_dcn401.c DCN_BASE__INST0_SEG2
static const size_t kSteps = sizeof(kDmubLinuxSteps) / sizeof(kDmubLinuxSteps[0]);
static size_t at;
static int failures;

static void fail(const char *what, uint32_t reg, uint32_t value) {
	if (failures++ < 5)
		fprintf(stderr, "dmubload: step %zu: %s 0x%05x 0x%08x, Linux: %c 0x%05x 0x%08x\n", at, what, reg + kSeg2, value,
		        at < kSteps ? kDmubLinuxSteps[at].op : '-', at < kSteps ? kDmubLinuxSteps[at].reg : 0,
		        at < kSteps ? kDmubLinuxSteps[at].value : 0);
}

static void expect(char op) {   // skip to the next step, past a section mark that should be there
	while (at < kSteps && kDmubLinuxSteps[at].op == 'M' && op != 'M')
		at++;
}

static uint32_t rd(void *, uint32_t reg) {
	expect('R');
	if (at >= kSteps || kDmubLinuxSteps[at].op != 'R' || kDmubLinuxSteps[at].reg != reg + kSeg2) {
		fail("read", reg, 0);
		return 0;
	}
	return kDmubLinuxSteps[at++].value;
}

static void wr(void *, uint32_t reg, uint32_t value) {
	expect('W');
	if (at >= kSteps || kDmubLinuxSteps[at].op != 'W' || kDmubLinuxSteps[at].reg != reg + kSeg2 ||
	    kDmubLinuxSteps[at].value != value)
		fail("write", reg, value);
	at++;
}

static void delay(void *, uint32_t) {}

int main() {
	using namespace DmubLoad;
	const Io io { nullptr, rd, wr, delay };
	uint32_t status = 0;

	if (kDmubLinuxFwVersion != kFwVersion || kDmubLinuxFbSize != kBlockBytes)
		fail("version or block size", kFwVersion, kBlockBytes);
	for (int w = 0; w < kWindows; w++)
		if (kLayout[w].offset != kDmubLinuxWindows[w].offset || kLayout[w].size != kDmubLinuxWindows[w].size)
			fail("window", w, kLayout[w].offset);

	if (!reset(io))
		fail("reset timed out", 0, 0);
	if (!start(io, kDmubLinuxFb, status) || status != 3)
		fail("start", 0, status);
	expect('M');
	if (at != kSteps)
		fail("steps left", 0, static_cast<uint32_t>(kSteps - at));

	if (failures) {
		fprintf(stderr, "dmubload: %d difference(s) from Linux\n", failures);
		return 1;
	}
	printf("dmubload: all %zu steps as Linux's\n", kSteps);
	return 0;
}
