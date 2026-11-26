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

struct DscRegRef {
	uint32_t ha, hf, hs, hb, va, vf, vs, vb, khz, lanes, rate;
	uint32_t engine[Dsc::kEngineRegs];
	uint32_t optcBytesPerPixel, sliceWidth;
	uint32_t pps[4][9];
};
#include "ref/dsc_regs_linux.inc"

// The register values against Linux lighting the stream (pipegen's "dsc" scenario: its sink kind d is kDscSinks[0], DCN
// 4.01's top DISPCLK of the bounding box and DSCCLK a third of it).
static int registerCases() {
	int failures = 0;
	DpTrain::DscCaps caps;
	DpTrain::parseDscCaps(kDscSinks[0], caps);
	for (const DscRegRef &r : kDscRegRef) {
		const Dsc::Timing t { r.ha, r.va, r.ha + r.hf + r.hs + r.hb, r.va + r.vf + r.vs + r.vb, r.khz, 8 };
		Dsc::Config c;
		Dsc::Pps p;
		uint32_t regs[Dsc::kEngineRegs], packets[4][9];
		if (!Dsc::choose(caps, t, Dsc::linkKbps(static_cast<uint8_t>(r.lanes), static_cast<uint8_t>(r.rate), true),
		                 Dsc::Source { 2000000 / 3, 2000000 }, c) || !Dsc::pps(c, t, p)) {
			fprintf(stderr, "dsc: %ux%u %u kHz x%u 0x%02x: no configuration, Linux has one\n", r.ha, r.va, r.khz, r.lanes, r.rate);
			failures++;
			continue;
		}
		Dsc::engineRegs(c, t, p, false, regs);
		Dsc::ppsPackets(p, packets);
		bool same = p.bytesPerPixel == r.optcBytesPerPixel && c.sliceWidth == r.sliceWidth && !memcmp(packets, r.pps, sizeof(packets));
		for (size_t i = 0; i < Dsc::kEngineRegs; i++)
			if (regs[i] != r.engine[i]) {
				if (same)
					fprintf(stderr, "dsc: %ux%u %u kHz x%u 0x%02x: register 0x%04x 0x%08x, Linux 0x%08x\n", r.ha, r.va, r.khz,
					        r.lanes, r.rate, Dsc::kEngineReg[i], regs[i], r.engine[i]);
				same = false;
			}
		if (!same) {
			fprintf(stderr, "dsc: %ux%u %u kHz x%u 0x%02x: OPTC 0x%08x/%u, Linux 0x%08x/%u; packets %s\n", r.ha, r.va, r.khz,
			        r.lanes, r.rate, p.bytesPerPixel, c.sliceWidth, r.optcBytesPerPixel, r.sliceWidth,
			        memcmp(packets, r.pps, sizeof(packets)) ? "differ" : "same");
			failures++;
		}
	}
	return failures;
}

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
	const size_t nRegs = sizeof(kDscRegRef) / sizeof(kDscRegRef[0]);
	if (const int bad = registerCases()) {
		fprintf(stderr, "dsc: %d of %zu register cases differ from Linux\n", bad, nRegs);
		return 1;
	}
	printf("dsc: %zu cases as Linux (%d with DSC); engine registers, OPTC and PPS packets as Linux for %zu streams\n", n, on, nRegs);
	return 0;
}
