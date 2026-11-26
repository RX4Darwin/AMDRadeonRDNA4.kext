// dsc-test: src/dsc.cpp against Linux's own run (test/ref/dsc_linux.inc, tools/gen/pipegen/dscgen.sh): for each sink
// (its DPCD 0x060-0x06F through DpTrain::parseDscCaps), timing, bit depth, link and clocks, the same decision, the same
// configuration, the same 128-byte PPS and the same OPTC and encoder values.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/dsc.hpp"

struct DscRef {
	uint32_t sink, ha, va, ht, vt, khz, bpc, lanes, rate, dscclk, dispclk, on;
	uint32_t slicesH, slicesV, sliceHeight, bpp16, blockPred, lineBuffer, versionMinor, bytesPerPixel, rcBits, chunk;
	uint8_t  pps[128];
};
#include "ref/dsc_linux.inc"

int main() {
	int failures = 0, on = 0;
	const size_t n = sizeof(kDscRef) / sizeof(kDscRef[0]);
	for (size_t i = 0; i < n; i++) {
		const DscRef &r = kDscRef[i];
		DpTrain::DscCaps caps;
		if (!DpTrain::parseDscCaps(kDscSinks[r.sink], caps)) {
			fprintf(stderr, "dsc: case %zu: sink %u not parsed\n", i, r.sink);
			return 1;
		}
		const Dsc::Timing t { r.ha, r.va, r.ht, r.vt, r.khz, static_cast<uint8_t>(r.bpc) };
		const Dsc::Source src { r.dscclk, r.dispclk };
		Dsc::Config c;
		const bool got = Dsc::choose(caps, t, Dsc::linkKbps(static_cast<uint8_t>(r.lanes), static_cast<uint8_t>(r.rate), true),
		                             src, c);
		Dsc::Pps p {};
		const bool packed = got && Dsc::pps(c, t, p);
		bool same = got == (r.on != 0);
		if (same && got) {
			same = c.slicesH == r.slicesH && c.slicesV == r.slicesV && c.sliceHeight == r.sliceHeight &&
			       c.bpp16 == r.bpp16 && c.blockPrediction == (r.blockPred != 0) && c.lineBufferBits == r.lineBuffer &&
			       c.versionMinor == r.versionMinor && packed && p.bytesPerPixel == r.bytesPerPixel &&
			       p.rcBufferModelSize == r.rcBits && p.chunkSize == r.chunk && !memcmp(p.bytes, r.pps, 128);
			on++;
		}
		if (!same && failures++ < 8) {
			fprintf(stderr, "dsc: case %zu (sink %u, %ux%u %u kHz, %u bpc, x%u 0x%02x, DSCCLK %u): ", i, r.sink, r.ha, r.va,
			        r.khz, r.bpc, r.lanes, r.rate, r.dscclk);
			fprintf(stderr, "got %d %ux%u h%u bpp16 %u bp %d lb %u v1.%u bpp 0x%08x rc %u chunk %u; Linux %u %ux%u h%u bpp16 %u "
			        "bp %u lb %u v1.%u bpp 0x%08x rc %u chunk %u\n", got, c.slicesH, c.slicesV, c.sliceHeight, c.bpp16,
			        c.blockPrediction, c.lineBufferBits, c.versionMinor, p.bytesPerPixel, p.rcBufferModelSize, p.chunkSize,
			        r.on, r.slicesH, r.slicesV, r.sliceHeight, r.bpp16, r.blockPred, r.lineBuffer, r.versionMinor,
			        r.bytesPerPixel, r.rcBits, r.chunk);
			for (int b = 0, shown = 0; packed && r.on && b < 128 && shown < 4; b++)
				if (p.bytes[b] != r.pps[b] && ++shown)
					fprintf(stderr, "    PPS byte %d: 0x%02x, Linux 0x%02x\n", b, p.bytes[b], r.pps[b]);
		}
	}
	if (failures) {
		fprintf(stderr, "dsc: %d of %zu cases differ from Linux\n", failures, n);
		return 1;
	}
	printf("dsc: %zu cases as Linux (%d with DSC)\n", n, on);
	return 0;
}
