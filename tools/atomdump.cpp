//
//  atomdump.cpp
//  RDNA4FB
//
//  Host-side harness for the kext's AtomBios parser. Compiles the exact same
//  src/atombios.cpp the kext uses and runs it against a ROM dump, so the
//  parsing logic is verified on the developer machine without GPU hardware.
//
//    make atomdump
//    ./build/atomdump firmware/Sapphire.RX9070XT.16384.241213.rom
//
//  Exits nonzero if the image fails to parse or expected tables are missing,
//  so `make test` can gate on it.
//

#include "../src/atombios.hpp"
#include "../src/ipdiscovery.hpp"
#include "../src/edid.hpp"
#include "../src/otgtiming.hpp"
#include "../src/modes.hpp"
#include "../src/dmub.hpp"
#include "../src/pipe.hpp"
#include "../src/ndrv.hpp"
#include "../src/modeset.hpp"
#include "../src/pipe2.hpp"
#include "../src/amdfw.hpp"
#include "../src/psp.hpp"
#include "../src/sdma.hpp"
#include "../src/ih.hpp"
#include "../src/smu_metrics.h"
#include "../src/pm4.hpp"
#include "../src/codeobj.hpp"
#include "../src/gpuheap.hpp"
#include "../src/flip.hpp"
#include "../src/gpuvm.hpp"
#include "../src/vmid.hpp"
#include "../src/ptpages.hpp"
#include "../src/gpuvmtable.hpp"
#include "../src/vadd_codeobj.h"
#include "../src/bench_codeobj.h"
#include "../src/gfxregs.hpp"
#include "../src/linuxref.hpp"
#include "rdna4compute.h"

#include <cstdarg>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <utility>

// The Samsung LS27DG702 (Odyssey G70D) full 256-byte EDID (base + CTA-861
// extension) captured from the live system's IODisplayEDID (2026-07-11) —
// fixture for the DTD/CTA parsers and the OTG timing computation.
static const char kSamsungEdidHex[] =
	"00ffffffffffff004c2d6fe0000000000e220104b53c22783b32b5ad5045a125"
	"0e505421880081c0810081809500a9c0b300010101014dd000a0f0703e803020"
	"3500ba882100001a000000fd0c309045458f010a202020202020000000fc004f"
	"64797373657920473730440a000000ff004831414b3530303030300a2020027a"
	"02033af04761103f04035f762309070783010000e305c301741a0000030f3090"
	"000060085b3690000000000000e6060501605b00e5018b84903d565e00a0a0a0"
	"295030203500ba882100001a6fc200a0a0a0555030203500ba882100001a0474"
	"801871382d40582c4500ba882100001e00000000000000000000000000000005";

// The Lenovo G25-10 EDID base block captured from EDID,DDC2 in ioreg
// (read over the DC_I2C engine, 2026-07-12) — the HDMI sink the second-pipe
// mode-set must drive.
static const char kLenovoEdidHex[] =
	"00ffffffffffff0030aefe6500000000321e010380361e782a9055a75553a028"
	"135054a10800d1c0b30081c081809500a9c001010101023a801871382d40582c"
	"4500202f2100001e000000fd0030901eaa22000a202020202020000000fc004c"
	"454e204732352d31300a2020000000ff005534423433304e390a202020200171";

// Synthetic 1080p 60/75 Hz HDMI monitor — the target user's class of sink.
// Hand-assembled so every field the mode table depends on is known; the
// checksums (bytes 127 and 255) are filled in at runtime.
static const char kSynthBaseHex[] =
	"00ffffffffffff00"                        // 0-7    header
	"4f2e3412010000000122"                    // 8-17   "SYN", product, serial, week 1 2024
	"0103"                                    // 18-19  EDID 1.3 (so digital => CVT-RB accepted)
	"80351e780a"                              // 20-24  digital, 53x30 cm, gamma 2.2, RGB + preferred
	"9055a75553a028135054"                    // 25-34  chromaticity (Lenovo's)
	"010800"                                  // 35-37  established: 800x600@60, 1024x768@60
	"8180b300010101010101010101010101"        // 38-53  standard: 1280x1024@60, 1680x1050@60
	"023a801871382d40582c45000f282100001e"    // 54     DTD 1920x1080@60 148.5 MHz (CEA), 527x296 mm
	"2a4480a070382740302035000f282100001a"    // 72     DTD 1920x1080@75 174.5 MHz CVT-RB, +h -v
	"000000fd00384b1e5512000a202020202020"    // 90     range: 56-75 Hz, 30-85 kHz, 180 MHz
	"000000fc0053594e313038305037350a2020"    // 108    name "SYN1080P75"
	"0100";                                   // 126    1 extension, checksum
static const char kSynthCtaHex[] =            // rest of the block is zero
	"02030f71"                                // CTA rev 3, DTDs at 15, audio/444/422, 1 native
	"4490041f02"                              // video: VIC 16 (native), 4, 31, 2
	"65030c001000"                            // HDMI VSDB, phys addr 1.0.0.0, no TMDS cap
	"011d007251d01e206e2855000f282100001e";   // DTD 1280x720@60 74.25 MHz (= VIC 4)

static void fixChecksum(uint8_t *block) {
	uint8_t sum = 0;
	for (size_t i = 0; i < 127; i++)
		sum = static_cast<uint8_t>(sum + block[i]);
	block[127] = static_cast<uint8_t>(0x100 - sum);
}

static bool hexToBytes(const char *hex, uint8_t *out, size_t outLen) {
	for (size_t i = 0; i < outLen; i++) {
		unsigned v;
		if (sscanf(hex + 2 * i, "%2x", &v) != 1)
			return false;
		out[i] = static_cast<uint8_t>(v);
	}
	return true;
}

// Verify the EDID DTD parser against the known 4K60 preferred timing.
static int testEdidParser() {
	int failures = 0;
	uint8_t edid[256];
	if (!hexToBytes(kSamsungEdidHex, edid, sizeof(edid))) {
		fprintf(stderr, "FAIL: EDID fixture is malformed\n");
		return 1;
	}

	Edid::DetailedTiming t {};
	if (!Edid::preferredTiming(edid, sizeof(edid), t)) {
		fprintf(stderr, "FAIL: preferred timing did not parse\n");
		return 1;
	}

	printf("\nedid fixture (Samsung Odyssey G70D):\n");
	printf("  preferred: %ux%u@%u.%03uHz pclk=%ukHz\n", t.hActive, t.vActive,
	       t.refreshMilliHz() / 1000, t.refreshMilliHz() % 1000, t.pixelClockKHz);
	printf("  h: blank=%u fp=%u sw=%u pol=%c   v: blank=%u fp=%u sw=%u pol=%c\n",
	       t.hBlank, t.hSyncOffset, t.hSyncWidth, t.hSyncPositive ? '+' : '-',
	       t.vBlank, t.vSyncOffset, t.vSyncWidth, t.vSyncPositive ? '+' : '-');

	struct { const char *name; uint32_t got, want; } checks[] = {
		{ "hActive",  t.hActive,        3840 },
		{ "vActive",  t.vActive,        2160 },
		{ "pclk",     t.pixelClockKHz,  533250 },
		{ "hBlank",   t.hBlank,         160 },
		{ "vBlank",   t.vBlank,         62 },
		{ "hSyncOff", t.hSyncOffset,    48 },
		{ "hSyncW",   t.hSyncWidth,     32 },
		{ "vSyncOff", t.vSyncOffset,    3 },
		{ "vSyncW",   t.vSyncWidth,     5 },
		{ "hTotal",   t.hTotal(),       4000 },
		{ "vTotal",   t.vTotal(),       2222 },
	};
	for (auto &c : checks) {
		if (c.got != c.want) {
			fprintf(stderr, "FAIL: EDID %s = %u, expected %u\n", c.name, c.got, c.want);
			failures++;
		}
	}
	// 533250000 / (4000*2222) = 59.99 Hz
	if (t.refreshMilliHz() / 100 != 599) {
		fprintf(stderr, "FAIL: refresh %u mHz, expected ~59.99 Hz\n", t.refreshMilliHz());
		failures++;
	}

	// OTG register images per optc1_program_timing. These exact raw values
	// must appear in the OTG0 line of a rdna4-modedump log (the GOP
	// programmed the same timing) — hardware validation of the math before
	// any OTG is written by us.
	OtgTiming::Regs r {};
	if (!OtgTiming::compute(t, r)) {
		fprintf(stderr, "FAIL: OTG computation rejected the 4K60 timing\n");
		return failures + 1;
	}
	printf("  expected OTG0 (diff vs modedump): h_total=0x%08x h_blank=0x%08x "
	       "h_sync=0x%08x\n                                    v_total=0x%08x "
	       "v_blank=0x%08x v_sync=0x%08x\n",
	       r.hTotal, r.hBlankStartEnd, r.hSyncA, r.vTotal, r.vBlankStartEnd, r.vSyncA);
	struct { const char *name; uint32_t got, want; } otgChecks[] = {
		{ "OTG_H_TOTAL",           r.hTotal,         3999 },
		{ "OTG_H_SYNC_A",          r.hSyncA,         32u << 16 },
		{ "OTG_H_BLANK_START_END", r.hBlankStartEnd, 3952u | (112u << 16) },
		{ "OTG_V_TOTAL",           r.vTotal,         2221 },
		{ "OTG_V_SYNC_A",          r.vSyncA,         5u << 16 },
		{ "OTG_V_BLANK_START_END", r.vBlankStartEnd, 2219u | (59u << 16) },
	};
	for (auto &c : otgChecks) {
		if (c.got != c.want) {
			fprintf(stderr, "FAIL: %s = 0x%08x, expected 0x%08x\n", c.name, c.got, c.want);
			failures++;
		}
	}
	if (r.hSyncPolInvert != false || r.vSyncPolInvert != true) {
		fprintf(stderr, "FAIL: sync polarity inversion (h=%d v=%d), expected h=0 v=1\n",
		        r.hSyncPolInvert, r.vSyncPolInvert);
		failures++;
	}

	// CTA-861 extension: capabilities the HDMI mode-set must respect.
	Edid::CtaCaps cta {};
	if (!Edid::parseCtaBlock(edid + 128, cta)) {
		fprintf(stderr, "FAIL: CTA extension did not parse\n");
		return failures + 1;
	}
	printf("  cta: rev %u underscan=%d audio=%d ycbcr444=%d ycbcr422=%d "
	       "vics=%zu hdmi_vsdb=%d max_tmds=%ukHz dtds=%zu\n",
	       cta.revision, cta.underscan, cta.basicAudio, cta.ycbcr444,
	       cta.ycbcr422, cta.vicCount, cta.hasHdmiVsdb, cta.maxTmdsKHz,
	       cta.dtdCount);
	if (cta.dtdCount)
		printf("  cta first dtd: %ux%u pclk=%ukHz\n",
		       cta.firstDtd.hActive, cta.firstDtd.vActive, cta.firstDtd.pixelClockKHz);
	if (cta.revision != 3 || !cta.basicAudio || !cta.ycbcr444 || !cta.ycbcr422) {
		fprintf(stderr, "FAIL: CTA flags/revision mismatch\n");
		failures++;
	}
	bool has4k60 = false, has1080p60 = false;
	for (size_t i = 0; i < cta.vicCount; i++) {
		if (cta.vics[i] == 97) has4k60 = true;    // 3840x2160p60
		if (cta.vics[i] == 16) has1080p60 = true; // 1920x1080p60
	}
	if (!has4k60 || !has1080p60) {
		fprintf(stderr, "FAIL: expected VICs 97 and 16 in the video block\n");
		failures++;
	}
	// This is a DisplayPort sink: no HDMI LLC vendor block expected.
	if (cta.hasHdmiVsdb) {
		fprintf(stderr, "FAIL: unexpected HDMI VSDB on a DP sink\n");
		failures++;
	}

	// --- Lenovo G25-10 (the HDMI sink the second pipe must light) ---
	uint8_t lenovo[128];
	Edid::DetailedTiming lt {};
	if (!hexToBytes(kLenovoEdidHex, lenovo, sizeof(lenovo)) ||
	    !Edid::preferredTiming(lenovo, sizeof(lenovo), lt)) {
		fprintf(stderr, "FAIL: Lenovo fixture did not parse\n");
		return failures + 1;
	}
	OtgTiming::Regs lr {};
	if (!OtgTiming::compute(lt, lr)) {
		fprintf(stderr, "FAIL: OTG computation rejected the 1080p60 timing\n");
		return failures + 1;
	}
	printf("\nedid fixture (Lenovo G25-10, HDMI):\n");
	printf("  preferred: %ux%u@%u.%03uHz pclk=%ukHz\n", lt.hActive, lt.vActive,
	       lt.refreshMilliHz() / 1000, lt.refreshMilliHz() % 1000, lt.pixelClockKHz);
	printf("  OTG1 program values: h_total=0x%08x h_blank=0x%08x h_sync=0x%08x\n"
	       "                       v_total=0x%08x v_blank=0x%08x v_sync=0x%08x\n",
	       lr.hTotal, lr.hBlankStartEnd, lr.hSyncA,
	       lr.vTotal, lr.vBlankStartEnd, lr.vSyncA);
	struct { const char *name; uint32_t got, want; } lchecks[] = {
		{ "pclk",      lt.pixelClockKHz,   148500 },
		{ "hActive",   lt.hActive,         1920 },
		{ "vActive",   lt.vActive,         1080 },
		{ "OTG_H_TOTAL",           lr.hTotal,         2199 },
		{ "OTG_H_SYNC_A",          lr.hSyncA,         44u << 16 },
		{ "OTG_H_BLANK_START_END", lr.hBlankStartEnd, 2112u | (192u << 16) },
		{ "OTG_V_TOTAL",           lr.vTotal,         1124 },
		{ "OTG_V_SYNC_A",          lr.vSyncA,         5u << 16 },
		{ "OTG_V_BLANK_START_END", lr.vBlankStartEnd, 1121u | (41u << 16) },
	};
	for (auto &c : lchecks) {
		if (c.got != c.want) {
			fprintf(stderr, "FAIL: Lenovo %s = 0x%x, expected 0x%x\n",
			        c.name, c.got, c.want);
			failures++;
		}
	}
	if (lr.hSyncPolInvert || lr.vSyncPolInvert) {
		fprintf(stderr, "FAIL: Lenovo sync polarity should be +/+\n");
		failures++;
	}

	// --- DMUB command header encoding ---
	// type QUERY_FEATURE_CAPS(6), subtype 0, payload 60 -> 0x3c000006
	if (Dmub::headerWord(Dmub::CmdQueryFeatureCaps, 0, 60) != 0x3c000006u) {
		fprintf(stderr, "FAIL: DMUB header encode (query caps)\n");
		failures++;
	}
	// type VBIOS(128), subtype SET_PIXEL_CLOCK(2), payload 28 -> 0x1c000280
	if (Dmub::headerWord(Dmub::CmdVbios, Dmub::VbiosSetPixelClock, 28) != 0x1c000280u) {
		fprintf(stderr, "FAIL: DMUB header encode (vbios)\n");
		failures++;
	}
	return failures;
}

// --- EDID -> mode table ------------------------------------------------------

static int check(bool ok, const char *fmt, ...) {
	if (ok)
		return 0;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	return 1;
}

static bool timingIs(const Edid::DetailedTiming &t, uint32_t pclk,
                     uint16_t ha, uint16_t hfp, uint16_t hsw, uint16_t hbp,
                     uint16_t va, uint16_t vfp, uint16_t vsw, uint16_t vbp,
                     bool hpos, bool vpos) {
	return t.pixelClockKHz == pclk &&
	       t.hActive == ha && t.hSyncOffset == hfp && t.hSyncWidth == hsw &&
	       t.hBlank == hfp + hsw + hbp &&
	       t.vActive == va && t.vSyncOffset == vfp && t.vSyncWidth == vsw &&
	       t.vBlank == vfp + vsw + vbp &&
	       t.hSyncPositive == hpos && t.vSyncPositive == vpos && !t.interlaced;
}

static void printModes(const char *title, const Modes::Mode *m, size_t n) {
	printf("  %s: %zu modes\n", title, n);
	for (size_t i = 0; i < n; i++) {
		const Edid::DetailedTiming &t = m[i].t;
		printf("    #%-2u %4ux%-4u @%3u.%03u Hz %6u kHz  h %u/%u/%u/%u %c  v %u/%u/%u/%u %c  "
		       "16.16=0x%07x %-7s%s\n",
		       m[i].id, t.hActive, t.vActive,
		       m[i].refreshMilliHz / 1000, m[i].refreshMilliHz % 1000, t.pixelClockKHz,
		       t.hActive, t.hSyncOffset, t.hSyncWidth,
		       static_cast<unsigned>(t.hBlank - t.hSyncOffset - t.hSyncWidth),
		       t.hSyncPositive ? '+' : '-',
		       t.vActive, t.vSyncOffset, t.vSyncWidth,
		       static_cast<unsigned>(t.vBlank - t.vSyncOffset - t.vSyncWidth),
		       t.vSyncPositive ? '+' : '-',
		       Modes::refresh1616(t), Modes::sourceName(m[i].source),
		       m[i].native ? " native" : "");
	}
}

// Structural rules every built table must obey, whatever the EDID.
static int checkModeTable(const char *name, const Modes::Mode *m, size_t n,
                          const Modes::Limits &lim) {
	int f = 0;
	for (size_t i = 0; i < n; i++) {
		const Modes::Mode &a = m[i];
		f += check(a.id == i + 1, "%s: mode %zu has id %u", name, i, a.id);
		f += check(Modes::usable(a.t), "%s: mode %u is not usable", name, a.id);
		f += check(!a.native || i == 0, "%s: native mode %u is not first", name, a.id);
		f += check((!lim.maxHActive || a.t.hActive <= lim.maxHActive) &&
		           (!lim.maxVActive || a.t.vActive <= lim.maxVActive) &&
		           (!lim.maxPixelClockKHz || a.t.pixelClockKHz <= lim.maxPixelClockKHz),
		           "%s: mode %u (%ux%u %u kHz) exceeds the limits", name, a.id,
		           a.t.hActive, a.t.vActive, a.t.pixelClockKHz);
		for (size_t j = 0; j < i; j++) {
			const Modes::Mode &b = m[j];
			f += check(!(a.t.hActive == b.t.hActive && a.t.vActive == b.t.vActive &&
			             (a.refreshMilliHz + 500) / 1000 == (b.refreshMilliHz + 500) / 1000),
			           "%s: modes %u and %u are duplicates (%ux%u@%u)", name, b.id, a.id,
			           a.t.hActive, a.t.vActive, (a.refreshMilliHz + 500) / 1000);
		}
		if (i > 0 && !m[i - 1].native) {
			const Modes::Mode &p = m[i - 1];
			bool ordered = p.t.hActive != a.t.hActive ? p.t.hActive > a.t.hActive :
			               p.t.vActive != a.t.vActive ? p.t.vActive > a.t.vActive :
			               p.refreshMilliHz > a.refreshMilliHz;
			f += check(ordered, "%s: modes %u and %u out of order", name, p.id, a.id);
		}
	}
	return f;
}

struct ExpectedMode {
	uint16_t h, v;
	uint32_t hz;        // whole Hz
	uint32_t pclk;
	uint8_t  source;
};

static int expectTable(const char *name, const Modes::Mode *m, size_t n,
                       const ExpectedMode *e, size_t ne, bool firstNative) {
	int f = check(n == ne, "%s: %zu modes, expected %zu", name, n, ne);
	for (size_t i = 0; i < n && i < ne; i++) {
		uint32_t hz = (m[i].refreshMilliHz + 500) / 1000;
		f += check(m[i].t.hActive == e[i].h && m[i].t.vActive == e[i].v && hz == e[i].hz &&
		           m[i].t.pixelClockKHz == e[i].pclk && m[i].source == e[i].source,
		           "%s: id %u is %ux%u@%u %u kHz (%s), expected %ux%u@%u %u kHz (%s)",
		           name, m[i].id, m[i].t.hActive, m[i].t.vActive, hz, m[i].t.pixelClockKHz,
		           Modes::sourceName(m[i].source), e[i].h, e[i].v, e[i].hz, e[i].pclk,
		           Modes::sourceName(e[i].source));
		f += check(m[i].native == (firstNative && i == 0), "%s: id %u native=%d",
		           name, m[i].id, m[i].native);
	}
	return f;
}

static bool buildSynthEdid(uint8_t out[256]) {
	memset(out, 0, 256);
	if (strlen(kSynthBaseHex) != 256 || strlen(kSynthCtaHex) != 66)
		return false;
	if (!hexToBytes(kSynthBaseHex, out, 128) || !hexToBytes(kSynthCtaHex, out + 128, 33))
		return false;
	fixChecksum(out);
	fixChecksum(out + 128);
	return true;
}

static int testModes() {
	using Modes::Mode;
	using Modes::Limits;
	int failures = 0;
	const Limits unlimited { 0, 0, 0 };
	const Limits hdmi1080 { 1920, 1080, 340000 };   // boot fb size, HDMI 1.4 TMDS

	// VIC 16 must be exactly the CEA-861 1080p60 raster (= DMT 0x52).
	Edid::DetailedTiming v16 {};
	failures += check(Edid::vicTiming(16, v16) &&
	                  timingIs(v16, 148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true),
	                  "VIC 16 is not 148500 kHz 1920/88/44/148 1080/4/5/36 +/+");
	Edid::DetailedTiming dmt52 {};
	failures += check(Edid::dmtTiming(1920, 1080, 60, false, dmt52) &&
	                  timingIs(dmt52, 148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true),
	                  "DMT 1920x1080@60 differs from VIC 16");
	Edid::DetailedTiming tmp {};
	failures += check(!Edid::vicTiming(5, tmp) && !Edid::vicTiming(20, tmp),
	                  "interlaced VICs 5/20 must not map to a timing");
	// DMT RB preference: 1680x1050 has both variants; 1600x900 only the RB one.
	failures += check(Edid::dmtTiming(1680, 1050, 60, true, tmp) && tmp.pixelClockKHz == 119000 &&
	                  Edid::dmtTiming(1680, 1050, 60, false, tmp) && tmp.pixelClockKHz == 146250 &&
	                  Edid::dmtTiming(1600, 900, 60, false, tmp) && tmp.pixelClockKHz == 108000,
	                  "DMT reduced-blanking variant selection");
	// Every table row must itself be a sane raster (catches a typo'd porch).
	for (uint16_t vic = 1; vic < 256; vic++)
		if (Edid::vicTiming(static_cast<uint8_t>(vic), tmp))
			failures += check(Modes::usable(tmp), "VIC %u table row is not usable", vic);

	// --- Samsung Odyssey G70D (4K DP) ---
	uint8_t samsung[256];
	if (!hexToBytes(kSamsungEdidHex, samsung, sizeof(samsung)))
		return failures + check(false, "Samsung fixture is malformed");
	Edid::BaseInfo sb {};
	failures += check(Edid::parseBaseBlock(samsung, sizeof(samsung), sb), "Samsung base block rejected");
	failures += check(Edid::blockChecksumOk(samsung + 128), "Samsung CTA checksum");
	printf("\nmode table (Samsung Odyssey G70D, EDID %u.%u):\n", sb.version, sb.revision);
	printf("  image %ux%u mm (base %ux%u cm)  range V %u-%u Hz H %u-%u kHz max %u kHz  "
	       "dtds=%zu ext=%u rb=%d\n", sb.widthMm, sb.heightMm, sb.screenWidthCm,
	       sb.screenHeightCm, sb.range.minVHz, sb.range.maxVHz, sb.range.minHKHz,
	       sb.range.maxHKHz, sb.range.maxPixelClockKHz, sb.dtdCount, sb.extensionCount,
	       sb.reducedBlanking);
	// The DTD's 698x392 mm is a 31.5" panel; this is a 27": the cm size wins.
	failures += check(sb.widthMm == 600 && sb.heightMm == 340, "Samsung image size %ux%u mm",
	                  sb.widthMm, sb.heightMm);
	// EDID 1.4 offset flags 0x0c: both H rates +255 kHz.
	failures += check(sb.range.present && sb.range.minVHz == 48 && sb.range.maxVHz == 144 &&
	                  sb.range.minHKHz == 324 && sb.range.maxHKHz == 324 &&
	                  sb.range.maxPixelClockKHz == 1430000 && !sb.reducedBlanking,
	                  "Samsung range limits");
	Edid::CtaCaps sc {};
	failures += check(Edid::parseCtaBlock(samsung + 128, sc) && sc.dtdCount == 3 &&
	                  sc.dtds[1].pixelClockKHz == 497750 && sc.dtds[2].pixelClockKHz == 297000,
	                  "Samsung CTA DTDs (all three)");

	Mode sm[Modes::MaxModes];
	size_t sn = Modes::build(samsung, sizeof(samsung), unlimited, sm, Modes::MaxModes);
	printModes("unlimited", sm, sn);
	failures += checkModeTable("Samsung", sm, sn, unlimited);
	// VIC 97 (= the preferred 4K60) and VIC 63 (= CTA DTD 3) are deduplicated;
	// VIC 95 (2160p30) falls below the 48 Hz range floor; VIC 118 is not in
	// the CEA table.
	static const ExpectedMode samsungExpect[] = {
		{ 3840, 2160,  60, 533250, Modes::SourceDtd },
		{ 2560, 1440, 120, 497750, Modes::SourceCtaDtd },
		{ 2560, 1440,  60, 241500, Modes::SourceCtaDtd },
		{ 1920, 1080, 120, 297000, Modes::SourceCtaDtd },
		{ 1920, 1080,  60, 148500, Modes::SourceVic },
		{ 1680, 1050,  60, 146250, Modes::SourceStd },   // EDID 1.4, no CVT-RB flag
		{ 1600,  900,  60, 108000, Modes::SourceStd },
		{ 1440,  900,  60, 106500, Modes::SourceStd },
		{ 1280, 1024,  60, 108000, Modes::SourceStd },
		{ 1280,  800,  60,  83500, Modes::SourceStd },
		{ 1280,  720,  60,  74250, Modes::SourceVic },
		{ 1024,  768,  60,  65000, Modes::SourceEst },
		{  800,  600,  72,  50000, Modes::SourceEst },
		{  800,  600,  60,  40000, Modes::SourceEst },
		{  720,  480,  60,  27000, Modes::SourceVic },
		{  640,  480,  60,  25175, Modes::SourceEst },
	};
	failures += expectTable("Samsung", sm, sn, samsungExpect,
	                        sizeof(samsungExpect) / sizeof(samsungExpect[0]), true);
	for (size_t i = 0; i < sn; i++)
		if (sm[i].t.hActive == 1920 && sm[i].t.vActive == 1080 && sm[i].refreshMilliHz == 60000)
			failures += check(timingIs(sm[i].t, 148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true),
			                  "Samsung 1080p60 (VIC 16) raster");

	Mode sl[Modes::MaxModes];
	size_t sln = Modes::build(samsung, sizeof(samsung), hdmi1080, sl, Modes::MaxModes);
	printModes("limits 1920x1080 / 340 MHz", sl, sln);
	failures += checkModeTable("Samsung/limited", sl, sln, hdmi1080);
	failures += check(sln == sn - 3, "Samsung/limited: %zu modes, expected %zu", sln, sn - 3);
	for (size_t i = 0; i < sln; i++) {
		failures += check(sl[i].t.hActive <= 1920 && sl[i].t.vActive <= 1080,
		                  "Samsung/limited: %ux%u survived", sl[i].t.hActive, sl[i].t.vActive);
		// The preferred 4K timing is gone and no SVD is flagged native.
		failures += check(!sl[i].native, "Samsung/limited: mode %u marked native", sl[i].id);
	}

	// --- Lenovo G25-10 (1080p HDMI, EDID 1.3, base block only) ---
	uint8_t lenovo[128];
	if (!hexToBytes(kLenovoEdidHex, lenovo, sizeof(lenovo)))
		return failures + check(false, "Lenovo fixture is malformed");
	Edid::BaseInfo lb {};
	failures += check(Edid::parseBaseBlock(lenovo, sizeof(lenovo), lb), "Lenovo base block rejected");
	printf("\nmode table (Lenovo G25-10, EDID %u.%u):\n", lb.version, lb.revision);
	printf("  image %ux%u mm (base %ux%u cm)  range V %u-%u Hz H %u-%u kHz max %u kHz  "
	       "dtds=%zu ext=%u rb=%d\n", lb.widthMm, lb.heightMm, lb.screenWidthCm,
	       lb.screenHeightCm, lb.range.minVHz, lb.range.maxVHz, lb.range.minHKHz,
	       lb.range.maxHKHz, lb.range.maxPixelClockKHz, lb.dtdCount, lb.extensionCount,
	       lb.reducedBlanking);
	failures += check(lb.widthMm == 544 && lb.heightMm == 303, "Lenovo image size %ux%u mm",
	                  lb.widthMm, lb.heightMm);
	failures += check(lb.range.minVHz == 48 && lb.range.maxVHz == 144 && lb.range.minHKHz == 30 &&
	                  lb.range.maxHKHz == 170 && lb.range.maxPixelClockKHz == 340000 &&
	                  lb.reducedBlanking, "Lenovo range limits / RB inference");
	Mode lm[Modes::MaxModes];
	// Only the base block was captured although byte 126 announces one
	// extension: build() must use just what it was given.
	size_t ln = Modes::build(lenovo, sizeof(lenovo), hdmi1080, lm, Modes::MaxModes);
	printModes("limits 1920x1080 / 340 MHz", lm, ln);
	failures += checkModeTable("Lenovo", lm, ln, hdmi1080);
	static const ExpectedMode lenovoExpect[] = {
		{ 1920, 1080, 60, 148500, Modes::SourceDtd },    // STD 1920x1080@60 deduplicated
		{ 1680, 1050, 60, 119000, Modes::SourceStd },    // digital EDID 1.3: CVT-RB
		{ 1600,  900, 60, 108000, Modes::SourceStd },
		{ 1440,  900, 60,  88750, Modes::SourceStd },
		{ 1280, 1024, 60, 108000, Modes::SourceStd },
		{ 1280,  720, 60,  74250, Modes::SourceStd },
		{ 1024,  768, 60,  65000, Modes::SourceEst },
		{  800,  600, 60,  40000, Modes::SourceEst },
		{  640,  480, 60,  25175, Modes::SourceEst },    // 720x400@70 (IBM) skipped
	};
	failures += expectTable("Lenovo", lm, ln, lenovoExpect,
	                        sizeof(lenovoExpect) / sizeof(lenovoExpect[0]), true);

	// --- synthetic 1080p 60/75 Hz monitor ---
	uint8_t syn[256];
	if (!buildSynthEdid(syn))
		return failures + check(false, "synthetic EDID fixture is malformed");
	Edid::BaseInfo yb {};
	failures += check(Edid::parseBaseBlock(syn, sizeof(syn), yb) && yb.dtdCount == 2 &&
	                  yb.hasPreferred && yb.range.minVHz == 56 && yb.range.maxVHz == 75 &&
	                  yb.range.maxPixelClockKHz == 180000 && yb.widthMm == 527 &&
	                  yb.heightMm == 296 && yb.reducedBlanking,
	                  "synthetic base block fields");
	Edid::CtaCaps yc {};
	failures += check(Edid::parseCtaBlock(syn + 128, yc) && yc.vicCount == 4 &&
	                  yc.vics[0] == 16 && yc.vicNative[0] && yc.vics[1] == 4 && !yc.vicNative[1] &&
	                  yc.vics[2] == 31 && yc.vics[3] == 2 && yc.hasHdmiVsdb && yc.maxTmdsKHz == 0 &&
	                  yc.dtdCount == 1, "synthetic CTA block fields");
	// HDMI 2.0: without an HDMI Forum block the sink has no SCDC; with one
	// (version 1, 600 MHz, SCDC_Present) it does, and the mode table's TMDS
	// limit follows it past the HDMI 1.4 block's.
	const Edid::Hdmi2Caps none = Edid::hdmi2Caps(syn, sizeof(syn));
	uint8_t hf[256];
	memcpy(hf, syn, sizeof(hf));
	static const uint8_t hfCta[] = {
		0x02, 0x03, 0x18, 0x71,                               // data blocks up to offset 24
		0x41, 0x61,                                           // video: VIC 97 (3840x2160@60, 594 MHz)
		0x67, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x00, 0x3c,       // HDMI 1.4 block, 300 MHz
		0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78, 0x80, 0x00,       // HDMI Forum block, 600 MHz, SCDC
	};
	memset(hf + 128, 0, 128);
	memcpy(hf + 128, hfCta, sizeof(hfCta));
	fixChecksum(hf + 128);
	hf[99] = 60;                                              // range limits: 600 MHz, not 180
	fixChecksum(hf);
	Edid::CtaCaps hc {};
	const Edid::Hdmi2Caps two = Edid::hdmi2Caps(hf, sizeof(hf));
	failures += check(!none.scdc && none.maxTmdsKHz == 0 && Edid::parseCtaBlock(hf + 128, hc) &&
	                  hc.maxTmdsKHz == 300000 && hc.hfMaxTmdsKHz == 600000 && hc.scdcPresent &&
	                  two.scdc && two.maxTmdsKHz == 600000,
	                  "HDMI Forum block: scdc %d/%d max %u/%u kHz", none.scdc, two.scdc, none.maxTmdsKHz,
	                  two.maxTmdsKHz);
	static Mode hfModes[Modes::MaxModes];
	const Limits hdmi2 { 0, 0, 600000 };
	size_t hn = Modes::build(hf, sizeof(hf), hdmi2, hfModes, Modes::MaxModes);
	bool has594 = false;
	for (size_t i = 0; i < hn; i++)
		has594 = has594 || hfModes[i].t.pixelClockKHz == 594000;
	failures += check(has594, "HDMI Forum block: the 594 MHz mode is not offered (%zu modes)", hn);

	// The 75 Hz DTD is `cvt -r 1920 1080 75`: 1080/3/5/31, i.e. 74.97 Hz.
	const Edid::DetailedTiming &t75 = yb.dtds[1];
	failures += check(timingIs(t75, 174500, 1920, 48, 32, 80, 1080, 3, 5, 31, true, false),
	                  "synthetic 1080p75 DTD did not parse back");
	uint32_t r75 = Modes::refresh1616(t75);
	printf("\nmode table (synthetic 1080p 60/75 Hz HDMI monitor):\n");
	printf("  1080p75: %u mHz, 16.16 = 0x%08x (%u.%05u Hz)\n", t75.refreshMilliHz(), r75,
	       r75 >> 16, static_cast<unsigned>((static_cast<uint64_t>(r75 & 0xffff) * 100000) >> 16));
	failures += check(t75.refreshMilliHz() >= 74950 && t75.refreshMilliHz() <= 75000,
	                  "1080p75 refresh %u mHz", t75.refreshMilliHz());
	uint64_t r75mHz = (static_cast<uint64_t>(r75) * 1000 + 32768) >> 16;
	failures += check(static_cast<uint64_t>(r75) * 1000 >= 74950ull * 65536 && r75 <= (75u << 16) &&
	                  (r75mHz == t75.refreshMilliHz() || r75mHz == t75.refreshMilliHz() + 1),
	                  "1080p75 16.16 refresh 0x%08x inconsistent with %u mHz", r75, t75.refreshMilliHz());
	failures += check(Modes::refresh1616(yb.dtds[0]) == (60u << 16),
	                  "1080p60 16.16 refresh 0x%08x, expected 0x003c0000",
	                  Modes::refresh1616(yb.dtds[0]));

	Mode ym[Modes::MaxModes];
	size_t yn = Modes::build(syn, sizeof(syn), hdmi1080, ym, Modes::MaxModes);
	printModes("limits 1920x1080 / 340 MHz", ym, yn);
	failures += checkModeTable("synthetic", ym, yn, hdmi1080);
	// VIC 16 = DTD 1 and VIC 4 = the CTA DTD are deduplicated; VIC 31
	// (1080p50) is below the 56 Hz range floor.
	static const ExpectedMode synthExpect[] = {
		{ 1920, 1080, 60, 148500, Modes::SourceDtd },
		{ 1920, 1080, 75, 174500, Modes::SourceDtd },
		{ 1680, 1050, 60, 119000, Modes::SourceStd },
		{ 1280, 1024, 60, 108000, Modes::SourceStd },
		{ 1280,  720, 60,  74250, Modes::SourceCtaDtd },
		{ 1024,  768, 60,  65000, Modes::SourceEst },
		{  800,  600, 60,  40000, Modes::SourceEst },
		{  720,  480, 60,  27000, Modes::SourceVic },
	};
	const size_t synthCount = sizeof(synthExpect) / sizeof(synthExpect[0]);
	failures += expectTable("synthetic", ym, yn, synthExpect, synthCount, true);

	// Same EDID, same limits: same table (IDs are persisted by WindowServer).
	Mode ym2[Modes::MaxModes];
	size_t yn2 = Modes::build(syn, sizeof(syn), hdmi1080, ym2, Modes::MaxModes);
	failures += check(yn2 == yn && memcmp(ym, ym2, yn * sizeof(Mode)) == 0,
	                  "synthetic: second build differs");

	// A short output array keeps the highest-priority sources.
	Mode small[3];
	size_t smalln = Modes::build(syn, sizeof(syn), hdmi1080, small, 3);
	failures += check(smalln == 3 && small[0].native && small[1].refreshMilliHz / 1000 == 74 &&
	                  small[2].source == Modes::SourceCtaDtd, "synthetic: cap=3 table");

	// Untrusted input: a corrupt extension is skipped, a corrupt base block
	// or a truncated blob yields nothing.
	uint8_t bad[256];
	memcpy(bad, syn, sizeof(bad));
	bad[140] ^= 0x40;
	Mode bm[Modes::MaxModes];
	size_t bn = Modes::build(bad, sizeof(bad), hdmi1080, bm, Modes::MaxModes);
	failures += check(bn == synthCount - 2, "bad CTA checksum: %zu modes, expected %zu",
	                  bn, synthCount - 2);
	for (size_t i = 0; i < bn; i++)
		failures += check(bm[i].source != Modes::SourceCtaDtd && bm[i].source != Modes::SourceVic,
		                  "bad CTA checksum: mode %u came from the extension", bm[i].id);
	failures += check(Modes::build(syn, 200, hdmi1080, bm, Modes::MaxModes) == synthCount - 2,
	                  "partial extension must be ignored");
	failures += check(Modes::build(syn, 127, hdmi1080, bm, Modes::MaxModes) == 0,
	                  "truncated base block must yield no modes");
	memcpy(bad, syn, sizeof(bad));
	bad[60] ^= 0x01;
	failures += check(Modes::build(bad, sizeof(bad), hdmi1080, bm, Modes::MaxModes) == 0,
	                  "bad base checksum must yield no modes");
	failures += check(Modes::build(nullptr, 256, hdmi1080, bm, Modes::MaxModes) == 0,
	                  "null EDID must yield no modes");

	// --- OTG images for the 1080p75 timing (optc1_program_timing math) ---
	OtgTiming::Regs r {};
	failures += check(OtgTiming::compute(t75, r), "OTG computation rejected 1080p75");
	printf("  1080p75 OTG: h_total=0x%08x h_blank=0x%08x h_sync=0x%08x\n"
	       "              v_total=0x%08x v_blank=0x%08x v_sync=0x%08x pol_inv h=%d v=%d\n",
	       r.hTotal, r.hBlankStartEnd, r.hSyncA, r.vTotal, r.vBlankStartEnd, r.vSyncA,
	       r.hSyncPolInvert, r.vSyncPolInvert);
	uint32_t hbs = r.hBlankStartEnd & 0xffff, hbe = r.hBlankStartEnd >> 16;
	uint32_t vbs = r.vBlankStartEnd & 0xffff, vbe = r.vBlankStartEnd >> 16;
	failures += check(r.hTotal == 2079 && r.hSyncA == (32u << 16) && hbs == 2032 && hbe == 112 &&
	                  r.vTotal == 1118 && r.vSyncA == (5u << 16) && vbs == 1116 && vbe == 36 &&
	                  !r.hSyncPolInvert && r.vSyncPolInvert, "1080p75 OTG register images");
	// Self-consistency with the counter-origin-at-sync model: active spans
	// blank_end..blank_start, front porch follows it, sync + back porch
	// precede it.
	failures += check(hbs - hbe == t75.hActive && r.hTotal + 1 - hbs == t75.hSyncOffset &&
	                  hbe == (r.hSyncA >> 16) + (t75.hBlank - t75.hSyncOffset - t75.hSyncWidth) &&
	                  vbs - vbe == t75.vActive && r.vTotal + 1 - vbs == t75.vSyncOffset &&
	                  vbe == (r.vSyncA >> 16) + (t75.vBlank - t75.vSyncOffset - t75.vSyncWidth),
	                  "1080p75 OTG images inconsistent with the timing");

	// --- match / ensure ---
	Edid::DetailedTiming probe = t75;
	probe.pixelClockKHz = t75.pixelClockKHz + t75.pixelClockKHz * 3 / 1000;   // +0.3%
	failures += check(Modes::match(ym, yn, probe) == 1, "match: +0.3%% clock must match mode 2");
	probe.pixelClockKHz = t75.pixelClockKHz - t75.pixelClockKHz * 3 / 1000;   // -0.3%
	failures += check(Modes::match(ym, yn, probe) == 1, "match: -0.3%% clock must match mode 2");
	probe.pixelClockKHz = t75.pixelClockKHz + t75.pixelClockKHz / 100;        // +1%
	failures += check(Modes::match(ym, yn, probe) == -1, "match: +1%% clock must not match");
	probe = t75;
	probe.vBlank++;
	failures += check(Modes::match(ym, yn, probe) == -1, "match: different v_total must not match");

	Mode em[Modes::MaxModes];
	memcpy(em, ym, sizeof(em));
	size_t en = yn;
	failures += check(Modes::ensure(em, en, Modes::MaxModes, t75, Modes::SourceBoot) == 1 &&
	                  en == yn && em[1].source == Modes::SourceDtd,
	                  "ensure: an existing timing must be found, not added");
	Edid::DetailedTiming uxga {};
	Edid::dmtTiming(1600, 1200, 60, false, uxga);
	int ui = Modes::ensure(em, en, Modes::MaxModes, uxga, Modes::SourceBoot);
	failures += check(ui == static_cast<int>(yn) && en == yn + 1 && em[ui].id == yn + 1 &&
	                  em[ui].source == Modes::SourceBoot && !em[ui].native &&
	                  timingIs(em[ui].t, 162000, 1600, 64, 192, 304, 1200, 1, 3, 46, true, true),
	                  "ensure: absent boot timing must be appended as id %zu", yn + 1);
	for (size_t i = 0; i < yn; i++)
		failures += check(em[i].id == ym[i].id && em[i].t.pixelClockKHz == ym[i].t.pixelClockKHz,
		                  "ensure: existing mode %zu changed", i);
	// GOP running 1080p60 as CVT-RB (138.5 MHz, 2080x1111): same size and
	// whole-Hz rate as the native DTD, so it replaces it in place.
	Edid::DetailedTiming cvtrb60 { 138500, 1920, 160, 48, 32, 1080, 31, 3, 5, true, false, false };
	int ci = Modes::ensure(em, en, Modes::MaxModes, cvtrb60, Modes::SourceBoot);
	failures += check(ci == 0 && en == yn + 1 && em[0].id == 1 && em[0].native &&
	                  em[0].source == Modes::SourceBoot && em[0].t.pixelClockKHz == 138500,
	                  "ensure: same-slot boot timing must replace mode 1 in place");
	failures += checkModeTable("synthetic+boot", em, 1, hdmi1080);
	// Full table: the last (lowest-priority) entry makes room, keeping its id.
	size_t fn = yn;
	memcpy(em, ym, sizeof(em));
	int fi = Modes::ensure(em, fn, yn, uxga, Modes::SourceBoot);
	failures += check(fi == static_cast<int>(yn - 1) && fn == yn && em[yn - 1].id == yn &&
	                  em[yn - 1].t.vActive == 1200, "ensure: full table must replace the last entry");
	Edid::DetailedTiming interlaced = t75;
	interlaced.interlaced = true;
	failures += check(Modes::ensure(em, fn, Modes::MaxModes, interlaced, Modes::SourceBoot) == -1,
	                  "ensure: interlaced timing must be rejected");

	// --- robustness: mutated EDIDs (checksums re-fixed so parsing goes
	// deep) must never produce a table that breaks the invariants ---
	uint32_t seed = 0x9e3779b9u;
	auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
	int fuzzFailures = 0;
	for (int iter = 0; iter < 20000 && fuzzFailures < 5; iter++) {
		uint8_t fz[512];
		memcpy(fz, syn, 256);
		memcpy(fz + 256, syn + 128, 128);   // a third block for multi-extension paths
		memset(fz + 384, 0, 128);
		fz[126] = static_cast<uint8_t>(rnd() % 5);
		int flips = 1 + static_cast<int>(rnd() % 12);
		for (int k = 0; k < flips; k++)
			fz[8 + rnd() % 504] = static_cast<uint8_t>(rnd());
		for (size_t blk = 0; blk < 4; blk++)
			if (rnd() % 8)
				fixChecksum(fz + blk * 128);
		size_t flen = rnd() % 4 ? 512 : rnd() % 513;
		Limits fl { rnd() % 2 ? 0u : 640 + rnd() % 3200, rnd() % 2 ? 0u : 480 + rnd() % 1800,
		            rnd() % 2 ? 0u : rnd() % 700000 };
		Mode fm[Modes::MaxModes];
		size_t fcap = 1 + rnd() % Modes::MaxModes;
		size_t fnm = Modes::build(fz, flen, fl, fm, fcap);
		int e = check(fnm <= fcap, "fuzz %d: %zu modes > cap %zu", iter, fnm, fcap);
		e += checkModeTable("fuzz", fm, fnm, fl);
		if (e)
			fprintf(stderr, "  (fuzz iteration %d)\n", iter);
		fuzzFailures += e;
	}
	failures += fuzzFailures;
	printf("  fuzz: 20000 mutated EDIDs, invariants %s\n", fuzzFailures ? "VIOLATED" : "held");
	return failures;
}

static const char *tableNames[AtomBios::MaxDataTable] = {
	"utilitypipeline", "multimedia_info", "smc_dpm_info", "sw_datatable3",
	"firmwareinfo", "sw_datatable5", "lcd_info", "sw_datatable7", "smu_info",
	"sw_datatable9", "sw_datatable10", "vram_usagebyfirmware", "gpio_pin_lut",
	"sw_datatable13", "gfx_info", "powerplayinfo", "sw_datatable16",
	"sw_datatable17", "sw_datatable18", "sw_datatable19", "sw_datatable20",
	"sw_datatable21", "displayobjectinfo", "indirectioaccess", "umc_info",
	"sw_datatable24", "dce_info", "vram_info", "sw_datatable27",
	"integratedsysteminfo", "asic_profiling_info", "voltageobject_info",
	"sw_datatable32", "sw_datatable33", "sw_datatable34",
};

// ---------------------------------------------------------------------------
// Pipe discovery against a synthetic HDMI pipe, and DMUB payload encodings.
// ---------------------------------------------------------------------------

// Sparse DCN register image: unknown registers read 0, like idle hardware.
struct FakeReg { uint8_t base; uint32_t dword; uint32_t value; };
struct FakeDcn {
	FakeReg regs[64];
	size_t  n = 0;
	void set(uint8_t base, uint32_t dword, uint32_t value) { regs[n++] = { base, dword, value }; }
	static uint32_t read(void *ctx, uint8_t base, uint32_t dword) {
		auto *self = static_cast<FakeDcn *>(ctx);
		for (size_t i = 0; i < self->n; i++)
			if (self->regs[i].base == base && self->regs[i].dword == dword)
				return self->regs[i].value;
		return 0;
	}
};

// The GOP put the HDMI monitor on OTG1 <- OPP1 <- MPCC/HUBP1, DIG2 front-end
// mapped to link 2 (HPD3), CEA 1080p60. Discovery must find all of it and
// invert the OTG images back to the exact timing.
static int testPipeDiscovery() {
	using namespace Pipe;
	Edid::DetailedTiming t {};
	t.pixelClockKHz = 148500;
	t.hActive = 1920; t.hBlank = 280; t.hSyncOffset = 88; t.hSyncWidth = 44;
	t.vActive = 1080; t.vBlank = 45;  t.vSyncOffset = 4;  t.vSyncWidth = 5;
	t.hSyncPositive = true; t.vSyncPositive = true;
	OtgTiming::Regs otg {};
	if (!OtgTiming::compute(t, otg)) {
		fprintf(stderr, "FAIL: pipe fixture timing did not compute\n");
		return 1;
	}

	FakeDcn dcn;
	const uint32_t o = 1 * Reg::kOtgStride, d = 2 * Reg::kDigStride;
	dcn.set(2, Reg::kOtgControl, 0);                       // OTG0 off
	dcn.set(2, Reg::kOtgControl + o, 0x00010001);          // OTG1 running
	dcn.set(2, Reg::kOtgHTotal + o, otg.hTotal);
	dcn.set(2, Reg::kOtgHBlank + o, otg.hBlankStartEnd);
	dcn.set(2, Reg::kOtgHSyncA + o, otg.hSyncA);
	dcn.set(2, Reg::kOtgVTotal + o, otg.vTotal);
	dcn.set(2, Reg::kOtgVBlank + o, otg.vBlankStartEnd);
	dcn.set(2, Reg::kOtgVSyncA + o, otg.vSyncA);
	dcn.set(2, Reg::kDigFeCntl, 0x0);                      // DIG0 stale, OTG0, disabled
	dcn.set(2, Reg::kDigFeCntl + d, 0x1);                  // DIG2 <- OTG1
	dcn.set(2, Reg::kDigFeEnCntl + d, 0x1);
	dcn.set(2, Reg::kDigFeClkCntl + d, 0x13);              // FE mode HDMI, clk en
	dcn.set(2, Reg::kStreamMapper + 2, 0x2);               // DIG2 -> link 2
	dcn.set(2, Reg::kDigBeCntl + d, (1u << (8 + 2)) | (2u << 28));   // DIG_HPD_SELECT 2 = HPD3
	dcn.set(2, Reg::kDigBeClkCntl + d, 0x13);              // BE mode HDMI
	dcn.set(2, Reg::kOptcDataSource + 1 * Reg::kOdmStride, 1u << 16);   // seg0 <- OPP1
	for (uint32_t m = 0; m < 4; m++)
		dcn.set(3, Reg::kMpccOppId + m * Reg::kMpccStride, m == 1 ? 1 : 0xf);
	dcn.set(2, Reg::kHubpViewportDim + 1 * Reg::kHubpStride, 1920 | (1080u << 16));
	dcn.set(2, Reg::kHubpSurfacePitch + 1 * Reg::kHubpStride, 1920 - 1);

	State s;
	int fails = 0;
	if (!discover(&FakeDcn::read, &dcn, s)) {
		fprintf(stderr, "FAIL: pipe discovery found no running OTG\n");
		return 1;
	}
	printf("\npipe discovery (synthetic HDMI on OTG1/DIG2):\n");
	printf("  otg=%u dig=%u link=%u hpd=%u opp=%u hubp=%u signal=%s viewport=%ux%u pitch=%u\n",
	       s.otg, s.dig, s.link, s.hpd, s.opp, s.hubp, signalName(s.signal),
	       s.viewportW, s.viewportH, s.pitchPx);
	auto expect = [&](const char *what, uint32_t got, uint32_t want) {
		if (got != want) {
			fprintf(stderr, "FAIL: pipe %s = %u, expected %u\n", what, got, want);
			fails++;
		}
	};
	expect("otg", s.otg, 1);
	expect("dig", s.dig, 2);
	expect("link", s.link, 2);
	expect("hpd", s.hpd, 3);
	expect("opp", s.opp, 1);
	expect("hubp", s.hubp, 1);
	expect("signal", static_cast<uint32_t>(s.signal), static_cast<uint32_t>(Signal::Hdmi));
	expect("viewportW", s.viewportW, 1920);
	expect("viewportH", s.viewportH, 1080);
	expect("pitch", s.pitchPx, 1920);
	if (!s.isTmds()) {
		fprintf(stderr, "FAIL: HDMI pipe not reported as TMDS\n");
		fails++;
	}

	Edid::DetailedTiming back {};
	if (!timingFromOtg(s, back)) {
		fprintf(stderr, "FAIL: timingFromOtg rejected the OTG images\n");
		return fails + 1;
	}
	expect("hActive", back.hActive, t.hActive);
	expect("hBlank", back.hBlank, t.hBlank);
	expect("hSyncOffset", back.hSyncOffset, t.hSyncOffset);
	expect("hSyncWidth", back.hSyncWidth, t.hSyncWidth);
	expect("vActive", back.vActive, t.vActive);
	expect("vBlank", back.vBlank, t.vBlank);
	expect("vSyncOffset", back.vSyncOffset, t.vSyncOffset);
	expect("vSyncWidth", back.vSyncWidth, t.vSyncWidth);
	if (!back.hSyncPositive || !back.vSyncPositive) {
		fprintf(stderr, "FAIL: timingFromOtg polarity should be +/+\n");
		fails++;
	}

	// 60 Hz frame = 16666667 ns -> 2200 * 1125 * 1e6 / period = 148500 kHz.
	uint32_t pclk = pixelClockFromFramePeriod(s, 16666667ULL);
	printf("  inverted timing %ux%u h %u/%u/%u v %u/%u/%u; 16.67 ms frame -> %u kHz\n",
	       back.hActive, back.vActive, back.hSyncOffset, back.hSyncWidth, back.hBlank,
	       back.vSyncOffset, back.vSyncWidth, back.vBlank, pclk);
	if (pclk < 148499 || pclk > 148501) {
		fprintf(stderr, "FAIL: pixel clock from frame period = %u kHz, expected 148500\n", pclk);
		fails++;
	}

	// No OTG running -> discovery must decline.
	FakeDcn idle;
	State none;
	if (discover(&FakeDcn::read, &idle, none) || none.valid()) {
		fprintf(stderr, "FAIL: discovery claimed a pipe on idle hardware\n");
		fails++;
	}
	return fails;
}

// Byte placement of the DMUB VBIOS payloads (dmub_cmd.h / atomfirmware.h).
static int testDmubPayloads() {
	int fails = 0;
	auto expect = [&](const char *what, uint32_t got, uint32_t want) {
		if (got != want) {
			fprintf(stderr, "FAIL: dmub %s = 0x%08x, expected 0x%08x\n", what, got, want);
			fails++;
		}
	};
	Dmub::Cmd c;

	Dmub::TransmitterControl tx {};
	tx.phyId = 2; tx.action = Dmub::TransmitterActionEnable;
	tx.digMode = Dmub::EncoderModeHdmi; tx.laneNum = 4;
	tx.symclk10kHz = 14850; tx.hpdSel = 3;
	Dmub::buildTransmitterControl(c, tx);
	expect("tx header", c[0], 0x3c000180);
	expect("tx phy/action/mode/lanes", c[1], 0x04030102);
	expect("tx symclk", c[2], 14850);
	expect("tx hpd/fe/conn/hpo", c[3], 0x00000003);
	for (int i = 4; i < 16; i++)
		expect("tx reserved", c[i], 0);

	Dmub::SetPixelClock pc {};
	pc.pixclk100Hz = 1485000; pc.pllId = 22; pc.encoderObjId = 0x21;
	pc.encoderMode = Dmub::EncoderModeHdmi; pc.crtcId = 1;
	Dmub::buildSetPixelClock(c, pc);
	expect("pclk header", c[0], 0x10000280);
	expect("pclk 100Hz", c[1], 1485000);
	expect("pclk pll/obj/mode/misc", c[2], 0x00032116);
	expect("pclk crtc/deep", c[3], 0x00000001);

	Dmub::DigEncoderStreamSetup enc {};
	enc.digId = 2; enc.action = Dmub::EncoderActionStreamSetup;
	enc.digMode = Dmub::EncoderModeHdmi; enc.laneNum = 4; enc.pclk10kHz = 14850;
	Dmub::buildDigEncoderStreamSetup(c, enc);
	expect("enc header", c[0], 0x0c000080);
	expect("enc dig/action/mode/lanes", c[1], 0x04030f02);
	expect("enc pclk", c[2], 14850);
	expect("enc bpc/linkrate", c[3], 0);

	printf("\ndmub payloads: transmitter/pixel-clock/encoder headers "
	       "0x3c000180/0x10000280/0x0c000080 %s\n", fails ? "MISMATCH" : "ok");
	return fails;
}

// --- NDRV csc translator -----------------------------------------------------

// --- HDMI modeset plan -------------------------------------------------------

// A register file the plan runs against: writes land, polls are skipped.
struct PlanRegs {
	struct R { uint8_t seg; uint32_t dw, v; };
	std::vector<R> regs;
	uint32_t get(uint8_t seg, uint32_t dw) const {
		for (const R &r : regs)
			if (r.seg == seg && r.dw == dw)
				return r.v;
		return 0;
	}
	void set(uint8_t seg, uint32_t dw, uint32_t v) {
		for (R &r : regs)
			if (r.seg == seg && r.dw == dw) {
				r.v = v;
				return;
			}
		regs.push_back({ seg, dw, v });
	}
	static uint32_t read(void *ctx, uint8_t seg, uint32_t dw) {
		return static_cast<PlanRegs *>(ctx)->get(seg, dw);
	}
	void run(const ModeSet::Plan &p) {
		for (size_t i = 0; i < p.count; i++) {
			const ModeSet::Step &s = p.steps[i];
			if (s.op == ModeSet::Op::Write)
				set(s.seg, s.dword, s.value);
			else if (s.op == ModeSet::Op::Update)
				set(s.seg, s.dword, (get(s.seg, s.dword) & ~s.mask) | s.value);
		}
	}
};

// The last write to `dw` in the plan (its final value).
static const ModeSet::Step *findWrite(const ModeSet::Plan &p, uint32_t dw, size_t *at = nullptr) {
	const ModeSet::Step *last = nullptr;
	for (size_t i = 0; i < p.count; i++)
		if (p.steps[i].op == ModeSet::Op::Write && p.steps[i].dword == dw) {
			if (at)
				*at = i;
			last = &p.steps[i];
		}
	return last;
}

static int testModeSet() {
	int failures = 0;
	Edid::DetailedTiming t1080 {}, t720 {};
	t1080.pixelClockKHz = 148500;
	t1080.hActive = 1920; t1080.hBlank = 280; t1080.hSyncOffset = 88; t1080.hSyncWidth = 44;
	t1080.vActive = 1080; t1080.vBlank = 45; t1080.vSyncOffset = 4; t1080.vSyncWidth = 5;
	t1080.hSyncPositive = t1080.vSyncPositive = true;
	t720.pixelClockKHz = 74250;                 // CEA VIC 4
	t720.hActive = 1280; t720.hBlank = 370; t720.hSyncOffset = 110; t720.hSyncWidth = 40;
	t720.vActive = 720; t720.vBlank = 30; t720.vSyncOffset = 5; t720.vSyncWidth = 5;
	t720.hSyncPositive = t720.vSyncPositive = true;

	// The emulated card's (and the recorded board's) HDMI pipe: OTG0 / OPP0 /
	// HUBP0, DIG2 -> link 2 (UNIPHYC), HPD3, VBIOS path 0x330c / 0x2120.
	ModeSet::Target t {};
	t.otg = 0; t.dig = 2; t.link = 2; t.hpd = 3; t.opp = 0; t.hubp = 0;
	t.encoderObjId = 0x2120; t.connectorObjId = 0x330c;
	t.from = t1080; t.to = t720;

	static ModeSet::Plan plan;
	const char *why = "";
	if (!ModeSet::build(t, plan, &why))
		return check(false, "modeset: 1080p -> 720p plan refused: %s", why);
	failures += check(plan.ncmds == 4, "modeset: %zu DMUB commands, expected 4", plan.ncmds);

	// DMUB payloads, in amdgpu's order: transmitter off, PLL, stream setup,
	// transmitter on.
	const Dmub::Cmd &off = plan.cmds[0], &pll = plan.cmds[1], &enc = plan.cmds[2], &on = plan.cmds[3];
	failures += check(off[0] == 0x3c000180 && off[1] == 0x00030002 && (off[3] & 0xffff) == 0x0003 &&
	                  ((off[3] >> 16) & 0xff) == 0x0c,
	                  "modeset: transmitter disable %08x %08x %08x %08x", off[0], off[1], off[2], off[3]);
	failures += check(pll[0] == 0x10000280 && pll[1] == 742500 && pll[2] == 0x00032016 &&
	                  (pll[3] & 0xff) == 0,
	                  "modeset: set pixel clock %08x %08x %08x %08x", pll[0], pll[1], pll[2], pll[3]);
	failures += check(enc[0] == 0x0c000080 && enc[1] == 0x04030f02 && enc[2] == 7425,
	                  "modeset: stream setup %08x %08x %08x", enc[0], enc[1], enc[2]);
	failures += check(on[0] == 0x3c000180 && on[1] == 0x04030102 && on[2] == 7425 &&
	                  (on[3] & 0xff) == 3,
	                  "modeset: transmitter enable %08x %08x %08x %08x", on[0], on[1], on[2], on[3]);

	// Timing images match amdgpu's for VIC 4 (optc1_program_timing).
	struct { uint32_t dw, want; const char *name; } timing[] = {
		{ 0x1b2a, 0x00000671, "h total" },   { 0x1b2b, 0x01040604, "h blank" },
		{ 0x1b2c, 0x00280000, "h sync" },    { 0x1b2f, 0x000002ed, "v total" },
		{ 0x1b38, 0x001902e9, "v blank" },   { 0x1b39, 0x00050000, "v sync" },
		{ 0x05eb, 0x02d00500, "viewport" },  { 0x0d1f, 0x02d00500, "recout" },
		{ 0x0d20, 0x02d00500, "mpc size" },  { 0x1856, 0x050002d0, "dpg size" },
	};
	for (auto &e : timing) {
		const ModeSet::Step *s = findWrite(plan, e.dw);
		failures += check(s && s->value == e.want, "modeset: %s = 0x%08x, expected 0x%08x",
		                  e.name, s ? s->value : 0, e.want);
	}
	// VTG: VCOUNT_INIT = active end (745), FP2 = VSTARTUP - (blank end + 1).
	const ModeSet::Step *vtg = findWrite(plan, 0x0530);
	uint32_t vstartup = ModeSet::vstartupLines(t720);
	failures += check(vtg && vtg->value == ((745u << 16) | (vstartup - 26)),
	                  "modeset: vtg params 0x%08x (vstartup %u)", vtg ? vtg->value : 0, vstartup);

	// Timing is written with the OTG stopped and the DMUB PLL command sent
	// before the OTG runs again.
	size_t hTotalAt = 0, otgOff = SIZE_MAX, otgOn = SIZE_MAX, pllAt = SIZE_MAX;
	findWrite(plan, 0x1b2a, &hTotalAt);
	for (size_t i = 0; i < plan.count; i++) {
		const ModeSet::Step &s = plan.steps[i];
		if (s.op == ModeSet::Op::Update && s.dword == 0x1b43 && (s.mask & 1))
			(s.value & 1 ? otgOn : otgOff) = i;
		if (s.op == ModeSet::Op::Dmub && s.arg == 1)
			pllAt = i;
	}
	failures += check(otgOff < pllAt && pllAt < hTotalAt && hTotalAt < otgOn,
	                  "modeset: order otg-off %zu, pll %zu, timing %zu, otg-on %zu",
	                  otgOff, pllAt, hTotalAt, otgOn);

	// Run it against the GOP-lit pipe: discovery must find the same pipe
	// with the new raster.
	PlanRegs regs;
	OtgTiming::Regs rg {};
	OtgTiming::compute(t1080, rg);
	regs.set(2, 0x1b43, 0x00010001);
	regs.set(2, 0x1b2a, rg.hTotal); regs.set(2, 0x1b2b, rg.hBlankStartEnd); regs.set(2, 0x1b2c, rg.hSyncA);
	regs.set(2, 0x1b2f, rg.vTotal); regs.set(2, 0x1b38, rg.vBlankStartEnd); regs.set(2, 0x1b39, rg.vSyncA);
	const uint32_t d2 = 2 * Pipe::Reg::kDigStride;
	regs.set(2, Pipe::Reg::kDigFeCntl + d2, 0);
	regs.set(2, Pipe::Reg::kDigFeEnCntl + d2, 1);
	regs.set(2, Pipe::Reg::kDigFeClkCntl + d2, 0x13);
	regs.set(2, Pipe::Reg::kStreamMapper + 2, 2);
	regs.set(2, Pipe::Reg::kDigBeCntl + d2, (1u << 10) | (2u << 28));   // DIG_HPD_SELECT 2 = HPD3
	regs.set(2, Pipe::Reg::kDigBeClkCntl + d2, 0x13);
	for (uint32_t m = 0; m < 4; m++)
		regs.set(3, Pipe::Reg::kMpccOppId + m * Pipe::Reg::kMpccStride, m == 0 ? 0 : 0xf);
	regs.set(2, Pipe::Reg::kHubpViewportDim, 1920 | (1080u << 16));
	regs.set(2, Pipe::Reg::kHubpSurfacePitch, 1919);
	regs.run(plan);
	// The model's OTG_CONTROL mirrors MASTER_EN into CURRENT_MASTER_EN_STATE.
	regs.set(2, 0x1b43, regs.get(2, 0x1b43) | ((regs.get(2, 0x1b43) & 1) << 16));

	Pipe::State s;
	Edid::DetailedTiming back {};
	bool found = Pipe::discover(&PlanRegs::read, &regs, s) && Pipe::timingFromOtg(s, back);
	failures += check(found && s.otg == 0 && s.dig == 2 && s.link == 2 && s.hpd == 3 &&
	                  s.opp == 0 && s.hubp == 0 && s.signal == Pipe::Signal::Hdmi,
	                  "modeset: pipe after the plan otg=%u dig=%u link=%u hpd=%u opp=%u hubp=%u",
	                  s.otg, s.dig, s.link, s.hpd, s.opp, s.hubp);
	failures += check(found && back.hActive == 1280 && back.vActive == 720 &&
	                  back.hBlank == 370 && back.hSyncOffset == 110 && back.hSyncWidth == 40 &&
	                  back.vBlank == 30 && back.vSyncOffset == 5 && back.vSyncWidth == 5 &&
	                  s.viewportW == 1280 && s.viewportH == 720 && s.pitchPx == 1920,
	                  "modeset: pipe reads back %ux%u (viewport %ux%u pitch %u)",
	                  back.hActive, back.vActive, s.viewportW, s.viewportH, s.pitchPx);
	failures += check((regs.get(2, 0x1854) & 1) == 0 && (regs.get(2, 0x20a8 + d2) & 1) == 0 &&
	                  (regs.get(2, 0x209b + d2) & 1) == 1 && (regs.get(2, 0x1b89) & 1) == 0,
	                  "modeset: left blanked/muted/locked (dpg 0x%x gc 0x%x fifo 0x%x lock 0x%x)",
	                  regs.get(2, 0x1854), regs.get(2, 0x20a8 + d2), regs.get(2, 0x209b + d2),
	                  regs.get(2, 0x1b89));

	const size_t steps = plan.count, cmds = plan.ncmds;

	// Below 340 MHz the scrambler is turned off, and a sink without SCDC gets
	// no SCDC write.
	auto scdcStep = [&](const ModeSet::Plan &p, size_t *at) -> const ModeSet::Step * {
		for (size_t i = 0; i < p.count; i++)
			if (p.steps[i].op == ModeSet::Op::Scdc) {
				*at = i;
				return &p.steps[i];
			}
		return nullptr;
	};
	size_t at = 0;
	failures += check((regs.get(2, 0x209e + d2) & 0x6) == 0 && !scdcStep(plan, &at),
	                  "modeset: 720p without SCDC: HDMI_CONTROL 0x%x, SCDC step %d",
	                  regs.get(2, 0x209e + d2), scdcStep(plan, &at) != nullptr);

	// HDMI 2.0: 3840x2160@60 (VIC 97, 594 MHz) on a sink with SCDC. The
	// scrambler and the quarter-rate clock go on, and the sink is told (3)
	// before the transmitter is enabled; back at 720p it is told 0.
	static ModeSet::Plan plan2;
	ModeSet::Target hdmi2 = t;
	hdmi2.sinkScdc = true;
	hdmi2.ddcLine = 2;
	failures += check(Edid::vicTiming(97, hdmi2.to) && hdmi2.to.pixelClockKHz == 594000, "VIC 97 is not 594 MHz");
	if (!ModeSet::build(hdmi2, plan2, &why)) {
		failures += check(false, "modeset: 594 MHz plan refused on an SCDC sink: %s", why);
	} else {
		PlanRegs r2;
		r2.run(plan2);
		const ModeSet::Step *sc = scdcStep(plan2, &at);
		size_t txOn = 0;
		for (size_t i = 0; i < plan2.count; i++)
			if (plan2.steps[i].op == ModeSet::Op::Dmub && plan2.steps[i].arg == 3)
				txOn = i;
		failures += check((r2.get(2, 0x209e + d2) & 0x6) == 0x6 && sc && sc->value == 3 && sc->arg == 2 &&
		                  at < txOn && plan2.cmds[1][1] == 5940000 && plan2.cmds[3][2] == 59400,
		                  "modeset: 594 MHz: HDMI_CONTROL 0x%x, SCDC %u on DDC%u at step %zu (transmitter on at "
		                  "%zu), pixel clock %u, symclk %u", r2.get(2, 0x209e + d2), sc ? sc->value : 99,
		                  sc ? sc->arg : 99, at, txOn, plan2.cmds[1][1], plan2.cmds[3][2]);
		hdmi2.from = hdmi2.to;
		hdmi2.to = t720;
		failures += check(ModeSet::build(hdmi2, plan2, &why) && (sc = scdcStep(plan2, &at)) && sc->value == 0,
		                  "modeset: back to 720p on an SCDC sink does not clear TMDS_CONFIG");
	}

	// Refusals: interlaced targets, more than 340 MHz without SCDC, more than
	// HDMI 2.0 TMDS carries.
	ModeSet::Target bad = t;
	bad.to.interlaced = true;
	failures += check(!ModeSet::build(bad, plan, &why), "modeset: interlaced target accepted");
	bad = t;
	bad.to.pixelClockKHz = 594000;
	failures += check(!ModeSet::build(bad, plan, &why), "modeset: 594 MHz accepted for a sink without SCDC");
	bad.sinkScdc = true;
	bad.to.pixelClockKHz = 600001;
	failures += check(!ModeSet::build(bad, plan, &why), "modeset: 600.001 MHz target accepted");

	printf("\nmodeset: 1080p60 -> 720p60 on OTG0/DIG2/UNIPHYC: %zu steps, %zu DMUB commands, "
	       "vstartup %u %s\n", steps, cmds, vstartup, failures ? "MISMATCH" : "ok");
	return failures;
}

// A DisplayPort mode switch retimes the stream on a link that stays trained.
// The reference is what Linux's own functions write for the same job
// (tools/pipegen's "dp" scenario): 3840x2160@60 down to 2560x1440@60 on pipe 0,
// DIG0. Every bit both write has to end up the same.
static int testDpRetime() {
	int failures = 0;
	static const struct { uint8_t seg; uint32_t dw, mask, value; } linux[] = {
#include "dp_retime_linux.inc"
	};
	Edid::DetailedTiming t4k {}, t1440 {};
	t4k.pixelClockKHz = 533250;                 // the GOP's 4K raster (CVT reduced blanking)
	t4k.hActive = 3840; t4k.hBlank = 160; t4k.hSyncOffset = 48; t4k.hSyncWidth = 32;
	t4k.vActive = 2160; t4k.vBlank = 62; t4k.vSyncOffset = 3; t4k.vSyncWidth = 5;
	t4k.hSyncPositive = true;
	t1440.pixelClockKHz = 241500;
	t1440.hActive = 2560; t1440.hBlank = 160; t1440.hSyncOffset = 48; t1440.hSyncWidth = 32;
	t1440.vActive = 1440; t1440.vBlank = 41; t1440.vSyncOffset = 3; t1440.vSyncWidth = 5;
	t1440.hSyncPositive = true;

	// The DTO, from one programmed amdgpu's way for the 4K mode on a 720 MHz
	// reference: exact. From one that counts in other units: by ratio.
	ModeSet::DpDto now { 0, 533250000, 720000000 }, dto {};
	failures += check(ModeSet::scaleDpDto(now, 533250, 241500, dto) && dto.integer == 0 &&
	                  dto.phase == 241500000 && dto.modulo == 720000000,
	                  "dp: DTO in Hz: %u + %u / %u", dto.integer, dto.phase, dto.modulo);
	ModeSet::DpDto ratio { 1, 0x400000, 0x1000000 }, scaled {};     // 1.25 x the reference
	failures += check(ModeSet::scaleDpDto(ratio, 500000, 300000, scaled) && scaled.integer == 0 &&
	                  scaled.phase == 0xc00000 && scaled.modulo == 0x1000000,   // 0.75 x
	                  "dp: DTO by ratio: %u + 0x%x / 0x%x", scaled.integer, scaled.phase, scaled.modulo);
	failures += check(!ModeSet::scaleDpDto(ModeSet::DpDto { 0, 1, 0 }, 533250, 241500, scaled) &&
	                  !ModeSet::scaleDpDto(ratio, 500000, 8000000, scaled),
	                  "dp: a DTO with no modulo or an integer past 15 accepted");

	ModeSet::Target t {};
	t.otg = 0; t.dig = 0; t.link = 0; t.hpd = 1; t.opp = 0; t.hubp = 0;
	t.from = t4k; t.to = t1440;
	t.dp = true;
	t.dpMaxKHz = 533250;
	t.dto = dto;
	t.vidM = static_cast<uint32_t>(0x8000ull * 241500 / 810000);   // four lanes of HBR3: 810 MHz
	static ModeSet::Plan plan;
	const char *why = "";
	if (!ModeSet::build(t, plan, &why))
		return failures + check(false, "dp: 4K -> 1440p plan refused: %s", why);
	failures += check(plan.ncmds == 0, "dp: %zu DMUB commands: the link is not to be touched", plan.ncmds);

	// What each side wrote, and with which bits.
	struct Written { uint8_t seg; uint32_t dw, mask, value; };
	auto apply = [](std::vector<Written> &regs, uint8_t seg, uint32_t dw, uint32_t mask, uint32_t value) {
		for (Written &r : regs)
			if (r.seg == seg && r.dw == dw) {
				r.value = (r.value & ~mask) | (value & mask);
				r.mask |= mask;
				return;
			}
		regs.push_back({ seg, dw, mask, value & mask });
	};
	std::vector<Written> ours, theirs;
	for (size_t i = 0; i < plan.count; i++) {
		const ModeSet::Step &s = plan.steps[i];
		if (s.op == ModeSet::Op::Write)
			apply(ours, s.seg, s.dword, 0xffffffffu, s.value);
		else if (s.op == ModeSet::Op::Update)
			apply(ours, s.seg, s.dword, s.mask, s.value);
	}
	for (const auto &l : linux)
		apply(theirs, l.seg, l.dw, l.mask, l.value);

	// Where the plan knowingly differs from that run of Linux:
	auto differs = [](uint8_t seg, uint32_t dw) -> uint32_t {
		if (seg == 2 && dw >= 0x1b85 && dw <= 0x1b87)
			return 0xffffffffu;      // VSTARTUP, VUPDATE, VREADY: by rule here, from DML there
		if (seg == 2 && dw == 0x0530)
			return 0x00007fffu;      // VTG FP2 follows VSTARTUP
		if (seg == 2 && (dw == 0x1b30 || dw == 0x1b31))
			return 0xffffffffu;      // V_TOTAL_MIN/MAX: unused with V_TOTAL_CONTROL 0; the plan keeps them at v total
		if (seg == 2 && dw >= 0x1854 && dw <= 0x185a)
			return 0xffffffffu;      // the pattern generator: the plan ends unblanked, Linux's stream part blanked
		return 0;
	};
	size_t compared = 0;
	for (const Written &o : ours)
		for (const Written &l : theirs) {
			if (o.seg != l.seg || o.dw != l.dw)
				continue;
			const uint32_t both = o.mask & l.mask & ~differs(o.seg, o.dw);
			compared += both != 0;
			failures += check(((o.value ^ l.value) & both) == 0, "dp: %u:0x%04x: plan 0x%08x, Linux 0x%08x "
			                  "(bits both write: 0x%08x)", o.seg, o.dw, o.value & both, l.value & both, both);
		}
	// The registers a retime is about must be among those compared.
	static const struct { uint8_t seg; uint32_t dw; const char *name; } must[] = {
		{ 1, 0x0081, "DP_DTO0_PHASE" }, { 1, 0x0082, "DP_DTO0_MODULO" }, { 1, 0x006f, "OTG_PIXEL_RATE_DIV" },
		{ 2, 0x1b2a, "OTG0 h total" }, { 2, 0x1b2f, "OTG0 v total" }, { 2, 0x1b2b, "OTG0 h blank" },
		{ 2, 0x1b38, "OTG0 v blank" }, { 2, 0x1b43, "OTG0 control" },
		{ 2, 0x2162, "MSA 1" }, { 2, 0x2163, "MSA 2" }, { 2, 0x2164, "MSA 3" }, { 2, 0x2165, "MSA 4" },
		{ 2, 0x2122, "DP_VID_STREAM_CNTL" }, { 2, 0x2123, "DP_STEER_FIFO" }, { 2, 0x2126, "DP_VID_TIMING" },
		{ 2, 0x2127, "DP_VID_N" }, { 2, 0x2128, "DP_VID_M" }, { 2, 0x209b, "DIG_FIFO_CTRL0" },
	};
	for (const auto &m : must) {
		bool inOurs = false, inTheirs = false;
		for (const Written &o : ours)
			inOurs = inOurs || (o.seg == m.seg && o.dw == m.dw);
		for (const Written &l : theirs)
			inTheirs = inTheirs || (l.seg == m.seg && l.dw == m.dw);
		failures += check(inOurs && inTheirs, "dp: %s is written by the plan %d, by Linux %d", m.name, inOurs, inTheirs);
	}
	// The link encoder and the PHY side of the DIG stay as they are.
	for (const Written &o : ours)
		failures += check(!(o.seg == 2 && o.dw >= 0x20bb && o.dw <= 0x20bd),
		                  "dp: the plan writes the link encoder (0x%04x)", o.dw);

	// Refusals: more than the link was trained for; no DTO.
	ModeSet::Target bad = t;
	bad.to.pixelClockKHz = 533251;
	failures += check(!ModeSet::build(bad, plan, &why), "dp: a pixel clock above the trained one accepted");
	bad = t;
	bad.dto = {};
	failures += check(!ModeSet::build(bad, plan, &why), "dp: a target without a DTO accepted");

	printf("\ndp: 3840x2160@60 -> 2560x1440@60 on OTG0/DIG0: %zu registers written, %zu also by Linux and "
	       "equal %s\n", ours.size(), compared, failures ? "MISMATCH" : "ok");
	return failures;
}

// The second-pipe plan (pipe2.hpp) lights pipe 1 next to a lit pipe 0. What the
// generated table may address is checked here against the block layout, not
// against the register names tools/pipegen/mkinc.py went by: every per-instance
// block is `stride` dwords per instance from `first`, and a step inside a
// family has to be in the instance the plan is for. Outside the families only
// the listed shared registers, and only the listed bits.
static int testPipe2() {
	int failures = 0;
	const Pipe2::Config &c = Pipe2::config();
	enum Who { ThePipe, TheDig, TheLink };
	struct Family { uint8_t seg; uint32_t first, stride; Who who; const char *name; };
	static const Family families[] = {
		{ 2, 0x05e5, 0xdc,  ThePipe, "HUBP" },        { 2, 0x0cc5, 0x16b, ThePipe, "DPP" },
		{ 2, 0x183c, 0x5a,  ThePipe, "OPP" },         { 2, 0x1aca, 0x10,  ThePipe, "ODM" },
		{ 2, 0x1b2a, 0x80,  ThePipe, "OTG" },         { 1, 0x0080, 0x04,  ThePipe, "OTG pixel rate" },
		{ 2, 0x0530, 0x01,  ThePipe, "VTG" },         { 2, 0x2068, 0x124, TheDig,  "DIO" },
		{ 2, 0x1f0d, 0x01,  TheDig,  "stream mapper" }, { 3, 0x0000, 0x15, ThePipe, "MPCC" },
		{ 3, 0x007e, 0x5e,  ThePipe, "MPCC OGAM" },   { 3, 0x0453, 0xb0,  ThePipe, "MPCC MCM" },
		{ 3, 0x02f2, 0x04,  ThePipe, "MPC out mux" }, { 3, 0x030b, 0x0d,  ThePipe, "MPC out CSC" },
		{ 2, 0x0080, 0x02,  ThePipe, "PG domain" },   { 2, 0x04bb, 0x01,  ThePipe, "DET" },
		{ 1, 0x0099, 0x01,  ThePipe, "DPPCLK DTO" },  { 1, 0x0040, 0x01,  TheLink, "PHY PLL resync" },
		{ 3, 0x02d5, 0x02,  ThePipe, "HUBP 3D LUT" },
	};
	struct Shared { uint8_t seg; uint32_t dword, mask, waitMask; const char *name; };
	static const Shared shared[] = {
		{ 1, 0x006f, 1u << (5 * c.pipe), 0, "OTG_PIXEL_RATE_DIV" },
		{ 1, 0x0064, 0, 0x00080000, "DENTIST_DISPCLK_CNTL" },
		{ 1, 0x00a8, 1u << (3 * c.pipe), 0, "DPPCLK_CTRL" },
		{ 2, 0x00a0, 1, 0, "DC_IP_REQUEST_CNTL" },
		{ 2, 0x04fe, 0, 0, "DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A" },
		{ 3, 0x030a, 1u << c.pipe, 0, "MPC_OUT_CSC_COEF_FORMAT" },
	};
	// The instance of a family a register is in, or -1; -2 if in no family.
	auto instanceOf = [&](uint8_t seg, uint32_t dw, const Family **fam) -> int {
		for (const Family &f : families)
			if (seg == f.seg && dw >= f.first && dw < f.first + 4 * f.stride) {
				*fam = &f;
				return static_cast<int>((dw - f.first) / f.stride);
			}
		return -2;
	};
	auto own = [&](const Family &f) -> int { return f.who == ThePipe ? c.pipe : f.who == TheDig ? c.dig : c.link; };

	Pipe2::Target t {};
	t.litHubp = 0;
	t.encoderObjId = 0x2120;
	t.surface = 0x8002100000ull;
	t.depth = Pipe2::Depth::Plane;
	static Pipe2::Plan plan, stream;
	const char *why = "";
	if (!Pipe2::build(t, plan, &why))
		return check(false, "pipe2: plan refused: %s", why);

	size_t requires = 0, writes = 0, outside = 0;
	bool wrote = false;
	for (size_t i = 0; i < plan.count; i++) {
		const ModeSet::Step &s = plan.steps[i];
		if (s.op == ModeSet::Op::Dmub || s.op == ModeSet::Op::Delay || s.op == ModeSet::Op::Scdc)
			continue;
		if (s.op == ModeSet::Op::Require) {
			requires++;
			failures += check(!wrote, "pipe2: step %zu: a requirement after the first write", i);
		} else if (s.op != ModeSet::Op::WaitValue) {
			wrote = true;
			writes++;
		}
		const bool reads = s.op == ModeSet::Op::Require || s.op == ModeSet::Op::WaitValue;
		const Family *fam = nullptr;
		const int inst = instanceOf(s.seg, s.dword, &fam);
		if (inst >= 0) {
			if (inst != own(*fam)) {
				outside++;
				failures += check(false, "pipe2: step %zu (%s) is in %s instance %d, the plan's is %d", i,
				                  s.what, fam->name, inst, own(*fam));
			}
		} else {
			const Shared *sh = nullptr;
			for (const Shared &x : shared)
				if (x.seg == s.seg && x.dword == s.dword)
					sh = &x;
			const uint32_t mask = s.op == ModeSet::Op::Write || s.op == ModeSet::Op::Copy ? 0xffffffffu : s.mask;
			if (!sh || (mask & ~(reads ? sh->waitMask : sh->mask))) {
				outside++;
				failures += check(false, "pipe2: step %zu (%s) touches %u:0x%04x mask 0x%08x: not the pipe's "
				                  "and not an allowed shared bit", i, s.what, s.seg, s.dword, mask);
			}
		}
		if (s.op == ModeSet::Op::Copy) {
			const Family *src = nullptr;
			failures += check(instanceOf(s.seg, s.arg, &src) == t.litHubp && src == &families[0] &&
			                  s.arg + (c.pipe - t.litHubp) * 0xdc == s.dword,
			                  "pipe2: step %zu copies from 0x%04x, not the lit HUBP's twin of 0x%04x", i,
			                  s.arg, s.dword);
		}
	}
	failures += check(requires == 4, "pipe2: %zu requirements, expected 4", requires);

	// A sink with SCDC is told the link is not scrambled (148.5 MHz), before
	// the transmitter is enabled; one without is left alone (above).
	Pipe2::Target scdc = t;
	scdc.sinkScdc = true;
	scdc.ddcLine = 2;
	bool told = false, before = false;
	if (Pipe2::build(scdc, stream, &why))
		for (size_t i = 0; i < stream.count; i++) {
			const ModeSet::Step &s = stream.steps[i];
			if (s.op == ModeSet::Op::Scdc)
				told = s.value == 0 && s.arg == 2 && !before;
			before = before || (s.op == ModeSet::Op::Dmub && s.arg == 2);
		}
	failures += check(told && stream.count == plan.count + 1, "pipe2: SCDC step for a sink with SCDC: %d, %zu steps",
	                  told, stream.count);

	// The three kinds of DMUB command, as amdgpu sends them for OTG1 on PLL 2.
	failures += check(plan.ncmds == 4, "pipe2: %zu DMUB commands, expected 4", plan.ncmds);
	if (plan.ncmds == 4) {
		const Dmub::Cmd &pll = plan.cmds[0], &enc = plan.cmds[1], &on = plan.cmds[2], &enc2 = plan.cmds[3];
		failures += check(pll[0] == 0x10000280 && pll[1] == 1485000 && pll[2] == 0x00032016 &&
		                  (pll[3] & 0xffff) == c.pipe,
		                  "pipe2: set pixel clock %08x %08x %08x %08x", pll[0], pll[1], pll[2], pll[3]);
		failures += check(enc[0] == 0x0c000080 && enc[1] == 0x04030f02 && enc[2] == 14850 &&
		                  memcmp(enc, enc2, sizeof(Dmub::Cmd)) == 0,
		                  "pipe2: stream setup %08x %08x %08x", enc[0], enc[1], enc[2]);
		failures += check(on[0] == 0x3c000180 && on[1] == 0x04030102 && on[2] == 14850 && (on[3] & 0xff) == 3,
		                  "pipe2: transmitter enable %08x %08x %08x %08x", on[0], on[1], on[2], on[3]);
	}

	// What the registers hold afterwards: timing of VIC 16, the surface, the
	// blender feeding OPP1 from DPP1, the OTG enabled and the lock released.
	PlanRegs regs;
	for (size_t i = 0; i < plan.count; i++) {
		const ModeSet::Step &s = plan.steps[i];
		if (s.op == ModeSet::Op::Write)
			regs.set(s.seg, s.dword, s.value);
		else if (s.op == ModeSet::Op::Update)
			regs.set(s.seg, s.dword, (regs.get(s.seg, s.dword) & ~s.mask) | s.value);
	}
	struct { uint8_t seg; uint32_t dw, mask, want; const char *name; } after[] = {
		{ 2, 0x1baa, 0xffffffff, 2199, "OTG1 h total" },         { 2, 0x1baf, 0xffffffff, 1124, "OTG1 v total" },
		{ 2, 0x1bab, 0x7fff7fff, 0x00c00840, "OTG1 h blank" },   { 2, 0x1bb8, 0x7fff7fff, 0x00290461, "OTG1 v blank" },
		{ 2, 0x1c05, 0x3ff, c.vstartup, "OTG1 vstartup" },       { 2, 0x1bc3, 0x1, 0x1, "OTG1 master enable" },
		{ 2, 0x1c09, 0x1, 0x0, "OTG1 update lock released" },    { 2, 0x1adb, 0xf0000, 0x10000, "ODM1 source OPP1" },
		{ 2, 0x06e6, 0xffffffff, 0x02100000, "surface low" },    { 2, 0x06e7, 0xffffffff, 0x80, "surface high" },
		{ 2, 0x06e3, 0xffff, 1919, "pitch" },                    { 2, 0x06c7, 0xffffffff, 0x04380780, "viewport" },
		{ 3, 0x0015, 0xf, 1, "MPCC1 top = DPP1" },               { 3, 0x0017, 0xf, 1, "MPCC1 OPP id" },
		{ 3, 0x02f6, 0xf, 1, "OPP1 out mux = MPCC1" },           { 2, 0x18ae, 0x1, 0, "DPG1 off (video)" },
		{ 2, 0x22dd, 0x1, 1, "DIG2 front-end enabled" },         { 2, 0x2305, 0x1, 1, "DIG2 back-end enabled" },
		{ 2, 0x1f0f, 0x7, 2, "DIG2 mapped to link 2" },          { 2, 0x22db, 0x7, 1, "DIG2 sourced by OTG1" },
		{ 2, 0x04bc, 0x1f, c.detSegments, "DET1 segments" },     { 1, 0x006f, 0x20, 0x20, "OTG1 TMDS divider /4" },
	};
	for (const auto &e : after)
		failures += check((regs.get(e.seg, e.dw) & e.mask) == e.want, "pipe2: %s: %u:0x%04x = 0x%08x, want 0x%08x "
		                  "under 0x%08x", e.name, e.seg, e.dw, regs.get(e.seg, e.dw), e.want, e.mask);

	// Stream only: stops before the plane, shows the pattern colour, and never
	// reaches the HUBP.
	t.depth = Pipe2::Depth::Stream;
	t.surface = 0;
	bool ok = Pipe2::build(t, stream, &why);
	failures += check(ok && stream.count < plan.count && stream.ncmds == 4, "pipe2: stream-only plan: %s, %zu steps",
	                  ok ? "built" : why, stream.count);
	bool hubp = false, colour = false;
	for (size_t i = 0; ok && i < stream.count; i++) {
		const ModeSet::Step &s = stream.steps[i];
		const Family *fam = nullptr;
		if (s.op != ModeSet::Op::Dmub && s.op != ModeSet::Op::Delay && instanceOf(s.seg, s.dword, &fam) >= 0 &&
		    fam == &families[0])
			hubp = true;
		if (s.op == ModeSet::Op::Write && s.dword == 0x18b2)      // DPG1_DPG_COLOUR_G_Y
			colour = s.value == ((static_cast<uint32_t>(Pipe2::kPatternColour[1]) << 16) | Pipe2::kPatternColour[1]);
	}
	failures += check(!hubp && colour, "pipe2: stream-only plan: HUBP touched %d, pattern colour %d", hubp, colour);

	// Refusals: the lit pipe on the plan's HUBP, no surface for a plane.
	Pipe2::Target bad = t;
	bad.litHubp = c.pipe;
	failures += check(!Pipe2::build(bad, stream, &why), "pipe2: lit pipe on the plan's HUBP accepted");
	bad = t;
	bad.depth = Pipe2::Depth::Plane;
	failures += check(!Pipe2::build(bad, stream, &why), "pipe2: plane without a surface accepted");

	printf("\npipe2: pipe %u DIG%u link %u HPD%u %ux%u at %u kHz: %zu steps (%zu register writes, %zu "
	       "requirements), %zu DMUB commands, %zu outside the pipe %s\n", c.pipe, c.dig, c.link, c.hpd,
	       c.timing.hActive, c.timing.vActive, c.timing.pixelClockKHz, plan.count, writes, requires, plan.ncmds,
	       outside, failures ? "MISMATCH" : "ok");
	return failures;
}

struct FakeBackend {
	uint64_t base = 0xc0000000ull;
	uint32_t pitch = 1920 * 4, w = 1920, h = 1080;
	int      switches = 0;
	int32_t  switchRet = Ndrv::kSuccess;
	int      powerCalls = 0;
	bool     powerOn = true;

	static bool surfaceFor(void *ctx, const Modes::Mode &m, bool, Ndrv::Surface &out) {
		auto *b = static_cast<FakeBackend *>(ctx);
		if (m.t.hActive > b->w || m.t.vActive > b->h)
			return false;
		out = { b->base, b->pitch, m.t.hActive, m.t.vActive };
		return true;
	}
	static int32_t switchTo(void *ctx, const Modes::Mode &, bool) {
		auto *b = static_cast<FakeBackend *>(ctx);
		b->switches++;
		return b->switchRet;
	}
	static void setPower(void *ctx, bool on) {
		auto *b = static_cast<FakeBackend *>(ctx);
		b->powerCalls++;
		b->powerOn = on;
	}
	Ndrv::Backend backend() {
		return { this, surfaceFor, switchTo, setPower };
	}
};

static int testNdrv() {
	int failures = 0;

	// Record layouts (LP64; the kext build also static_asserts them against
	// the SDK types).
	failures += check(sizeof(Ndrv::VPBlock) == 44, "VPBlock size %zu", sizeof(Ndrv::VPBlock));
	failures += check(sizeof(Ndrv::VDSwitchInfoRec) == 32, "VDSwitchInfoRec size %zu",
	                  sizeof(Ndrv::VDSwitchInfoRec));
	failures += check(sizeof(Ndrv::VDTimingInfoRec) == 32, "VDTimingInfoRec size %zu",
	                  sizeof(Ndrv::VDTimingInfoRec));
	failures += check(sizeof(Ndrv::VDDisplayConnectInfoRec) == 24, "VDDisplayConnectInfoRec size %zu",
	                  sizeof(Ndrv::VDDisplayConnectInfoRec));
	failures += check(sizeof(Ndrv::VDResolutionInfoRec) == 40 &&
	                  offsetof(Ndrv::VDResolutionInfoRec, csResolutionFlags) == 24,
	                  "VDResolutionInfoRec size %zu", sizeof(Ndrv::VDResolutionInfoRec));
	failures += check(sizeof(Ndrv::VDVideoParametersInfoRec) == 32 &&
	                  offsetof(Ndrv::VDVideoParametersInfoRec, csVPBlockPtr) == 8,
	                  "VDVideoParametersInfoRec size %zu", sizeof(Ndrv::VDVideoParametersInfoRec));
	failures += check(sizeof(Ndrv::VDDDCBlockRec) == 144, "VDDDCBlockRec size %zu",
	                  sizeof(Ndrv::VDDDCBlockRec));
	failures += check(sizeof(Ndrv::VDDetailedTimingRec) == 160, "VDDetailedTimingRec size %zu",
	                  sizeof(Ndrv::VDDetailedTimingRec));

	// The VM test path of RDNA4Device::buildModeTable: Lenovo fixture, 1080p
	// console, the EDID's preferred timing as the live mode.
	uint8_t lenovo[128];
	if (!hexToBytes(kLenovoEdidHex, lenovo, sizeof(lenovo)))
		return failures + check(false, "Lenovo fixture is malformed");
	Modes::Limits lim { 1920, 1080, 340000 };
	Modes::Mode table[Modes::MaxModes];
	size_t n = Modes::build(lenovo, sizeof(lenovo), lim, table, Modes::MaxModes);
	Edid::DetailedTiming pref {};
	if (!Edid::preferredTiming(lenovo, sizeof(lenovo), pref))
		return failures + check(false, "Lenovo preferred timing");
	int bootIdx = Modes::match(table, n, pref);
	if (bootIdx < 0)
		bootIdx = Modes::ensure(table, n, Modes::MaxModes, pref, Modes::SourceBoot);
	if (bootIdx < 0 || n < 2)
		return failures + check(false, "Lenovo table: %zu modes, boot %d", n, bootIdx);
	const uint32_t bootId = table[bootIdx].id;

	FakeBackend fb;
	Ndrv::Translator tr;
	tr.init(table, n, bootId, lenovo, sizeof(lenovo), fb.backend());
	int32_t ret = 0;

	// Mode list: FindFirst walks every table entry once, boot mode as 100.
	Ndrv::VDResolutionInfoRec res {};
	res.csPreviousDisplayModeID = Ndrv::kDisplayModeIDFindFirst;
	size_t walked = 0;
	bool sawBoot = false, idsOk = true;
	while (tr.status(Ndrv::cscGetNextResolution, &res, ret) && ret == Ndrv::kSuccess &&
	       res.csDisplayModeID > 0 && walked <= n) {
		const Modes::Mode &m = table[walked];
		idsOk &= res.csDisplayModeID == tr.idFor(m) && res.csHorizontalPixels == m.t.hActive &&
		         res.csVerticalLines == m.t.vActive && res.csMaxDepthMode == Ndrv::kDepthMode1 &&
		         res.csRefreshRate == Modes::refresh1616(m.t);
		sawBoot |= res.csDisplayModeID == Ndrv::kBootModeId;
		idsOk &= res.csDisplayModeID == Ndrv::kBootModeId ||
		         (res.csDisplayModeID > Ndrv::kBootModeId && res.csDisplayModeID < 0x10000);
		res.csPreviousDisplayModeID = res.csDisplayModeID;
		walked++;
	}
	failures += check(walked == n && sawBoot && idsOk && ret == Ndrv::kSuccess &&
	                  res.csDisplayModeID == Ndrv::kDisplayModeIDNoMore,
	                  "ndrv: resolution walk %zu/%zu modes, boot %d, ids %d, end 0x%x", walked, n,
	                  sawBoot, idsOk, res.csDisplayModeID);
	res.csPreviousDisplayModeID = Ndrv::kDisplayModeIDCurrent;
	failures += check(tr.status(Ndrv::cscGetNextResolution, &res, ret) && ret == Ndrv::kSuccess &&
	                  res.csDisplayModeID == Ndrv::kBootModeId && res.csHorizontalPixels == 1920,
	                  "ndrv: kDisplayModeIDCurrent -> 0x%x", res.csDisplayModeID);
	res.csPreviousDisplayModeID = -4;   // kDisplayModeIDFindFirstProgrammable
	failures += check(tr.status(Ndrv::cscGetNextResolution, &res, ret) && ret == Ndrv::kBadArgument &&
	                  res.csDisplayModeID == Ndrv::kDisplayModeIDInvalid,
	                  "ndrv: programmable iteration not refused");
	res.csPreviousDisplayModeID = 99;
	failures += check(tr.status(Ndrv::cscGetNextResolution, &res, ret) && ret == Ndrv::kBadArgument,
	                  "ndrv: unknown previous mode not refused");

	// Pick a non-boot mode for the per-mode checks.
	const Modes::Mode *other = nullptr;
	for (size_t i = 0; i < n && !other; i++)
		if (table[i].id != bootId)
			other = &table[i];
	const int32_t otherId = tr.idFor(*other);

	// Video parameters: boot pitch, the mode's size, 32 bpp direct.
	Ndrv::VPBlock vp;
	memset(&vp, 0xa5, sizeof(vp));
	Ndrv::VDVideoParametersInfoRec vpr {};
	vpr.csDisplayModeID = otherId;
	vpr.csDepthMode = Ndrv::kDepthMode1;
	vpr.csVPBlockPtr = &vp;
	failures += check(tr.status(Ndrv::cscGetVideoParameters, &vpr, ret) && ret == Ndrv::kSuccess &&
	                  vp.vpBounds.right == other->t.hActive && vp.vpBounds.bottom == other->t.vActive &&
	                  vp.vpBounds.left == 0 && vp.vpRowBytes == fb.pitch && vp.vpPixelSize == 32 &&
	                  vp.vpPixelType == Ndrv::kRGBDirectPixels && vp.vpCmpCount == 3 &&
	                  vp.vpCmpSize == 8 && vp.vpPlaneBytes == 0,
	                  "ndrv: video parameters for mode %d", otherId);
	vpr.csDepthMode = Ndrv::kDepthMode1 + 1;
	failures += check(tr.status(Ndrv::cscGetVideoParameters, &vpr, ret) && ret == Ndrv::kBadArgument,
	                  "ndrv: second depth not refused");

	// Mode timing: valid + safe everywhere, default on the boot mode only
	// (without "safe", macOS 26 scales the default mode instead of switching).
	Ndrv::VDTimingInfoRec ti {};
	ti.csTimingMode = Ndrv::kBootModeId;
	failures += check(tr.status(Ndrv::cscGetModeTiming, &ti, ret) && ret == Ndrv::kSuccess &&
	                  ti.csTimingFormat == Ndrv::kDeclROMtables &&
	                  ti.csTimingFlags == (Ndrv::kDisplayModeValidFlag | Ndrv::kDisplayModeSafeFlag |
	                                       Ndrv::kDisplayModeDefaultFlag),
	                  "ndrv: boot mode timing flags 0x%x", ti.csTimingFlags);
	ti.csTimingMode = otherId;
	failures += check(tr.status(Ndrv::cscGetModeTiming, &ti, ret) && ret == Ndrv::kSuccess &&
	                  ti.csTimingFlags == (Ndrv::kDisplayModeValidFlag | Ndrv::kDisplayModeSafeFlag),
	                  "ndrv: mode %d timing flags 0x%x", otherId, ti.csTimingFlags);

	// Detailed timing mirrors the table entry.
	Ndrv::VDDetailedTimingRec dt {};
	dt.csTimingSize = sizeof(dt);
	dt.csDisplayModeID = Ndrv::kBootModeId;
	const Edid::DetailedTiming &bt = table[bootIdx].t;
	failures += check(tr.status(Ndrv::cscGetDetailedTiming, &dt, ret) && ret == Ndrv::kSuccess &&
	                  dt.csPixelClock == 148500000ull && dt.csHorizontalActive == 1920 &&
	                  dt.csHorizontalBlanking == bt.hBlank && dt.csHorizontalSyncOffset == bt.hSyncOffset &&
	                  dt.csHorizontalSyncPulseWidth == bt.hSyncWidth && dt.csVerticalActive == 1080 &&
	                  dt.csVerticalBlanking == bt.vBlank && dt.csVerticalSyncOffset == bt.vSyncOffset &&
	                  dt.csVerticalSyncPulseWidth == bt.vSyncWidth &&
	                  dt.csHorizontalSyncConfig == (bt.hSyncPositive ? 1u : 0u) &&
	                  dt.csDisplayModeID == Ndrv::kBootModeId && dt.csTimingSize == sizeof(dt),
	                  "ndrv: detailed timing of the boot mode (%llu Hz)",
	                  static_cast<unsigned long long>(dt.csPixelClock));
	dt = Ndrv::VDDetailedTimingRec {};
	dt.csDisplayModeID = -5;   // kDisplayModeIDBootProgrammable
	failures += check(tr.status(Ndrv::cscGetDetailedTiming, &dt, ret) && ret == Ndrv::kBadArgument,
	                  "ndrv: boot-programmable detailed timing not refused");

	// Current mode: boot, physical base with the low bit set, page 1.
	Ndrv::VDSwitchInfoRec sw {};
	failures += check(tr.status(Ndrv::cscGetCurMode, &sw, ret) && ret == Ndrv::kSuccess &&
	                  sw.csData == Ndrv::kBootModeId && sw.csMode == Ndrv::kDepthMode1 &&
	                  sw.csPage == 1 && sw.csBaseAddr == (1 | fb.base),
	                  "ndrv: current mode 0x%x base 0x%llx", sw.csData,
	                  static_cast<unsigned long long>(sw.csBaseAddr));

	// Connection and EDID.
	Ndrv::VDDisplayConnectInfoRec ci {};
	const uint32_t ddc = Ndrv::kReportsDDCConnection | Ndrv::kHasDDCConnection;
	failures += check(tr.status(Ndrv::cscGetConnection, &ci, ret) && ret == Ndrv::kSuccess &&
	                  (ci.csConnectFlags & ddc) == ddc, "ndrv: connection flags 0x%x",
	                  ci.csConnectFlags);
	// The Lenovo base block announces a CTA extension the fixture (like the
	// VM build's fake EDID) does not carry: the count is trimmed to what is
	// cached, with a fixed checksum, and the rest is served unchanged.
	Ndrv::VDDDCBlockRec ddcRec {};
	ddcRec.ddcBlockNumber = 1;
	failures += check(tr.status(Ndrv::cscGetDDCBlock, &ddcRec, ret) && ret == Ndrv::kSuccess &&
	                  lenovo[126] == 1 && memcmp(ddcRec.ddcBlockData, lenovo, 126) == 0 &&
	                  ddcRec.ddcBlockData[126] == 0 && Edid::blockChecksumOk(ddcRec.ddcBlockData),
	                  "ndrv: EDID block 1 (extension count %u)", ddcRec.ddcBlockData[126]);
	ddcRec.ddcBlockNumber = 2;
	failures += check(tr.status(Ndrv::cscGetDDCBlock, &ddcRec, ret) && ret == Ndrv::kBadArgument,
	                  "ndrv: EDID block 2 of a 128-byte EDID not refused");

	// An EDID that matches what was read is served byte for byte.
	uint8_t plain[128];
	memcpy(plain, lenovo, sizeof(plain));
	plain[126] = 0;
	fixChecksum(plain);
	Ndrv::Translator tr2;
	tr2.init(table, n, bootId, plain, sizeof(plain), fb.backend());
	ddcRec.ddcBlockNumber = 1;
	failures += check(tr2.status(Ndrv::cscGetDDCBlock, &ddcRec, ret) && ret == Ndrv::kSuccess &&
	                  memcmp(ddcRec.ddcBlockData, plain, 128) == 0,
	                  "ndrv: complete EDID not served unchanged");

	// Mode switch: a refused switch keeps the current mode.
	fb.switchRet = Ndrv::kUnsupported;
	sw = Ndrv::VDSwitchInfoRec {};
	sw.csData = otherId;
	sw.csMode = Ndrv::kDepthMode1;
	failures += check(tr.control(Ndrv::cscSwitchMode, &sw, ret) && ret == Ndrv::kUnsupported &&
	                  tr.currentModeId() == Ndrv::kBootModeId && fb.switches == 1,
	                  "ndrv: refused switch");
	fb.switchRet = Ndrv::kSuccess;
	failures += check(tr.control(Ndrv::cscSwitchMode, &sw, ret) && ret == Ndrv::kSuccess &&
	                  tr.currentModeId() == otherId && sw.csBaseAddr == (1 | fb.base) &&
	                  fb.switches == 2, "ndrv: switch to mode %d", otherId);
	Ndrv::VDSwitchInfoRec cur {};
	failures += check(tr.status(Ndrv::cscGetCurMode, &cur, ret) && cur.csData == otherId,
	                  "ndrv: current mode after switch 0x%x", cur.csData);
	failures += check(tr.control(Ndrv::cscSwitchMode, &sw, ret) && ret == Ndrv::kSuccess &&
	                  fb.switches == 2, "ndrv: switch to the current mode reprogrammed");
	sw.csData = 99;
	failures += check(tr.control(Ndrv::cscSwitchMode, &sw, ret) && ret == Ndrv::kBadArgument,
	                  "ndrv: switch to an unknown mode not refused");

	// DPMS: capabilities, off, state, on.
	Ndrv::VDSyncInfoRec sy { 0xff, 0 };
	failures += check(tr.status(Ndrv::cscGetSync, &sy, ret) && ret == Ndrv::kSuccess &&
	                  sy.csMode == Ndrv::kDPMSSyncMask, "ndrv: sync capabilities 0x%x", sy.csMode);
	sy = { Ndrv::kDPMSSyncOff, Ndrv::kDPMSSyncMask };
	failures += check(tr.control(Ndrv::cscSetSync, &sy, ret) && ret == Ndrv::kSuccess &&
	                  fb.powerCalls == 1 && !fb.powerOn, "ndrv: DPMS off");
	sy = { 0, 0 };
	failures += check(tr.status(Ndrv::cscGetSync, &sy, ret) && sy.csMode == Ndrv::kDPMSSyncOff,
	                  "ndrv: sync state 0x%x after off", sy.csMode);
	sy = { Ndrv::kDPMSSyncOff, Ndrv::kDPMSSyncMask };
	tr.control(Ndrv::cscSetSync, &sy, ret);
	failures += check(fb.powerCalls == 1, "ndrv: repeated DPMS off reached the backend");
	sy = { Ndrv::kDPMSSyncOn, Ndrv::kDPMSSyncMask };
	failures += check(tr.control(Ndrv::cscSetSync, &sy, ret) && fb.powerCalls == 2 && fb.powerOn,
	                  "ndrv: DPMS on");

	// Everything else stays with IOBootNDRV.
	failures += check(!tr.status(8 /* cscGetGamma */, &sy, ret) &&
	                  !tr.control(3 /* cscSetEntries */, &sy, ret) &&
	                  !tr.status(Ndrv::cscGetCurMode, nullptr, ret),
	                  "ndrv: answered a request it does not own");

	// Without a mode table only the connection, EDID and sync requests are
	// answered.
	Ndrv::Translator none;
	none.init(table, 0, bootId, lenovo, sizeof(lenovo), fb.backend());
	failures += check(!none.status(Ndrv::cscGetNextResolution, &res, ret) &&
	                  !none.status(Ndrv::cscGetCurMode, &cur, ret) &&
	                  !none.control(Ndrv::cscSwitchMode, &sw, ret) &&
	                  none.status(Ndrv::cscGetDDCBlock, &ddcRec, ret),
	                  "ndrv: empty mode table not passed through");

	// A head with no IOBootNDRV behind it: the fallback replies and where its
	// surface goes.
	failures += check(Ndrv::bootReply(false, Ndrv::cscSetEntries) == Ndrv::kSuccess &&
	                  Ndrv::bootReply(false, Ndrv::cscSetGamma) == Ndrv::kSuccess &&
	                  Ndrv::bootReply(true, Ndrv::cscSetGamma) == Ndrv::kUnsupported &&
	                  Ndrv::bootReply(false, Ndrv::cscSetSync) == Ndrv::kUnsupported,
	                  "ndrv: boot fallback replies");
	const uint64_t kBar = 0x840000000ull, kMiB = 1ull << 20;
	const uint64_t k4k = 3840ull * 2160 * 4, k1080 = 1920ull * 1080 * 4, k720 = 1280ull * 720 * 4;
	Ndrv::Surface spare {};
	// A 4K console ends inside MiB 31; one clear MiB after it puts the surface at MiB 33.
	failures += check(Ndrv::spareSurface(kBar, k4k, kBar, 256 * kMiB, 1920, 1080, spare) &&
	                  spare.physBase == kBar + 33 * kMiB && spare.rowBytes == 7680 &&
	                  spare.width == 1920 && spare.height == 1080,
	                  "ndrv: spare surface behind a 4K console at 0x%llx",
	                  static_cast<unsigned long long>(spare.physBase));
	// A 16 MiB range (the VM): 1080p console, surface at MiB 9; 1080p does not fit, 720p does.
	failures += check(!Ndrv::spareSurface(kBar, k1080, kBar, 16 * kMiB, 1920, 1080, spare) &&
	                  Ndrv::spareSurface(kBar, k1080, kBar, 16 * kMiB, 1280, 720, spare) &&
	                  spare.physBase == kBar + 9 * kMiB,
	                  "ndrv: spare surface in a 16 MiB range");
	// The last byte counts (the surface plus the 128 bytes getApertureRange adds).
	failures += check(Ndrv::spareSurface(kBar, k1080, kBar, 9 * kMiB + k720 + 128, 1280, 720, spare) &&
	                  !Ndrv::spareSurface(kBar, k1080, kBar, 9 * kMiB + k720 + 127, 1280, 720, spare),
	                  "ndrv: spare surface exact fit");
	// A console outside the range, or larger than it, gives no surface.
	failures += check(!Ndrv::spareSurface(kBar - kMiB, k1080, kBar, 256 * kMiB, 1280, 720, spare) &&
	                  !Ndrv::spareSurface(kBar + 250 * kMiB, k1080, kBar, 256 * kMiB, 1280, 720, spare) &&
	                  !Ndrv::spareSurface(kBar, k1080, kBar, 256 * kMiB, 0, 720, spare),
	                  "ndrv: spare surface refuses a console outside the range");

	printf("\nndrv: %zu modes (boot = 100 = %ux%u, other e.g. %d = %ux%u), EDID/DPMS %s\n", n,
	       table[bootIdx].t.hActive, table[bootIdx].t.vActive, otherId, other->t.hActive,
	       other->t.vActive, failures ? "MISMATCH" : "ok");
	return failures;
}

// --- AMD firmware containers + PSP protocol ------------------------------------

static bool readFile(const char *path, std::vector<uint8_t> &out) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(static_cast<size_t>(len));
	bool ok = fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	return ok;
}

// A PSP as the driver sees it: MP0 scratch registers plus the VRAM window.
// Bootloader commands re-raise "ready"; the sOS command brings the sOS up;
// ring creation latches the ring; each write-pointer update consumes frames,
// answers LOAD_TOC / LOAD_IP_FW and writes the fence.
struct FakePsp {
	uint32_t regs[0x100] {};
	std::vector<uint8_t> vram;
	uint64_t mcBase { 0 };
	std::vector<uint32_t> blCmds, blFirstDword, blBufferMc;
	uint64_t ringMc { 0 };
	uint32_t ringSize { 0 }, rptr { 0 };
	std::vector<uint32_t> gpcom;
	uint32_t fwType { 0 }, fwSize { 0 };
	const uint8_t *expect { nullptr };
	uint32_t expectSize { 0 };
	bool payloadOk { false };
	bool bootloaderStuck { false };
	uint32_t forceStatus { 0 };   // response status the sOS writes
	int flushes { 0 };

	uint8_t *at(uint64_t mc) { return vram.data() + (mc - mcBase); }
	uint32_t get(uint64_t mc) { uint32_t v; memcpy(&v, at(mc), 4); return v; }
	void set(uint64_t mc, uint32_t v) { memcpy(at(mc), &v, 4); }

	static uint32_t rd(void *c, uint32_t r) { return static_cast<FakePsp *>(c)->regs[r & 0xff]; }
	static void delay(void *, uint32_t) {}
	static void flush(void *c) { static_cast<FakePsp *>(c)->flushes++; }
	static void wr(void *c, uint32_t r, uint32_t v) {
		auto *p = static_cast<FakePsp *>(c);
		p->regs[r & 0xff] = v;
		if (r == Psp::Reg::kC2p35) {
			uint64_t buf = static_cast<uint64_t>(p->regs[Psp::Reg::kC2p36]) << 20;
			p->blCmds.push_back(v);
			p->blBufferMc.push_back(p->regs[Psp::Reg::kC2p36]);
			p->blFirstDword.push_back(p->get(buf));
			if (v == Psp::BlSosDrv) {
				p->regs[Psp::Reg::kC2p81] = 0x003a1214;
				p->regs[Psp::Reg::kC2p64] = 0x80000000;
			}
			p->regs[Psp::Reg::kC2p35] = p->bootloaderStuck ? 0 : 0x80000000;
		} else if (r == Psp::Reg::kC2p64 && v == (2u << 16)) {
			p->ringMc = p->regs[Psp::Reg::kC2p69] |
			            (static_cast<uint64_t>(p->regs[Psp::Reg::kC2p70]) << 32);
			p->ringSize = p->regs[Psp::Reg::kC2p71];
			p->rptr = 0;
			p->regs[Psp::Reg::kC2p64] = 0x80000000;
			p->regs[Psp::Reg::kC2p67] = 0;
		} else if (r == Psp::Reg::kC2p67) {
			const uint32_t ringDw = p->ringSize / 4;
			while (p->rptr != v) {
				uint64_t frame = p->ringMc + p->rptr * 4ull;
				uint64_t cmd = p->get(frame) | (static_cast<uint64_t>(p->get(frame + 4)) << 32);
				uint64_t fence = p->get(frame + 12) | (static_cast<uint64_t>(p->get(frame + 16)) << 32);
				uint32_t fenceValue = p->get(frame + 20);
				uint32_t id = p->get(cmd + Psp::Layout::kCmdId);
				p->gpcom.push_back(id);
				if (id == Psp::CmdLoadToc)
					p->set(cmd + Psp::Layout::kRespTmrSize, 0x1400000);
				if (id == Psp::CmdLoadIpFw) {
					uint64_t fw = p->get(cmd + 28) | (static_cast<uint64_t>(p->get(cmd + 32)) << 32);
					p->fwSize = p->get(cmd + 36);
					p->fwType = p->get(cmd + 40);
					p->payloadOk = p->expect && p->fwSize == p->expectSize &&
					               !memcmp(p->at(fw), p->expect, p->expectSize);
				}
				p->set(cmd + Psp::Layout::kRespStatus, p->forceStatus);
				p->set(fence, fenceValue);
				p->rptr = (p->rptr + Psp::Layout::kFrameSize / 4) % ringDw;
			}
		}
	}
};

static int testPsp() {
	int failures = 0;
	std::vector<uint8_t> sosFile, smuFile;
	if (!readFile("firmware/amdgpu/psp_14_0_3_sos.bin", sosFile) ||
	    !readFile("firmware/amdgpu/smu_14_0_3.bin", smuFile)) {
		printf("\npsp: firmware/amdgpu/ blobs absent, skipped\n");
		return 0;
	}

	AmdFw::PspPackage pkg;
	failures += check(AmdFw::parsePsp(sosFile.data(), sosFile.size(), pkg), "psp: sOS package rejected");
	failures += check(pkg.count == 11 && pkg.part[AmdFw::PspKdb].valid() &&
	                  pkg.part[AmdFw::PspToc].size == 2304 && pkg.part[AmdFw::PspSos].size == 125472 &&
	                  pkg.version[AmdFw::PspSos] == 0x003a1214,
	                  "psp: sOS package contents (count %u)", pkg.count);
	failures += check(!AmdFw::parsePsp(sosFile.data(), 200, pkg), "psp: truncated package accepted");
	AmdFw::parsePsp(sosFile.data(), sosFile.size(), pkg);
	AmdFw::Blob smu;
	failures += check(AmdFw::payload(smuFile.data(), smuFile.size(), smu) && smu.size == 327168,
	                  "psp: SMU payload (%u bytes)", smu.size);

	// The whole stage-2 sequence against the fake.
	FakePsp fake;
	fake.mcBase = 0x8008000000ull;
	fake.vram.assign(8u << 20, 0xcc);
	fake.regs[Psp::Reg::kC2p35] = 0x80000000;       // bootloader ready, sOS down
	Psp::Bus bus { &fake, FakePsp::rd, FakePsp::wr, FakePsp::delay, FakePsp::flush };
	Psp::Window win { fake.vram.data(), fake.mcBase, static_cast<uint32_t>(fake.vram.size()) };
	Psp::Driver psp;
	failures += check(psp.init(bus, win), "psp: init refused");
	failures += check(!psp.sosAlive(), "psp: fake sOS alive before load");

	uint32_t loaded = 0;
	Psp::Result r = psp.loadSos(pkg, loaded);
	failures += check(r.ok && loaded == 9 && psp.sosAlive(), "psp: loadSos %s (0x%x), %u loaded",
	                  r.what, r.value, loaded);
	static const uint32_t order[] = { Psp::BlKdb, Psp::BlSplTable, Psp::BlSysDrv, Psp::BlSocDrv,
	                                  Psp::BlIntfDrv, Psp::BlHadDrv, Psp::BlRasDrv,
	                                  Psp::BlIpKeyMgr, Psp::BlSosDrv };
	static const uint32_t parts[] = { AmdFw::PspKdb, AmdFw::PspSpl, AmdFw::PspSysDrv,
	                                  AmdFw::PspSocDrv, AmdFw::PspIntfDrv, AmdFw::PspDbgDrv,
	                                  AmdFw::PspRasDrv, AmdFw::PspIpKeyMgrDrv, AmdFw::PspSos };
	bool seqOk = fake.blCmds.size() == 9;
	for (size_t i = 0; seqOk && i < 9; i++) {
		uint32_t first;
		memcpy(&first, pkg.part[parts[i]].data, 4);
		seqOk = fake.blCmds[i] == order[i] && fake.blFirstDword[i] == first &&
		        fake.blBufferMc[i] == static_cast<uint32_t>(fake.mcBase >> 20);
	}
	failures += check(seqOk, "psp: bootloader command sequence / buffer contents");
	failures += check(psp.loadSos(pkg, loaded).ok && loaded == 0 && fake.blCmds.size() == 9,
	                  "psp: second loadSos should be a no-op");

	r = psp.createRing();
	failures += check(r.ok && fake.ringMc == fake.mcBase + Psp::kRingOffset &&
	                  fake.ringSize == Psp::kRingSize, "psp: createRing %s (0x%x)", r.what, r.value);

	uint32_t tmr = 0;
	r = psp.loadToc(pkg.part[AmdFw::PspToc], tmr);
	failures += check(r.ok && tmr == 0x1400000, "psp: LOAD_TOC %s, tmr 0x%x", r.what, tmr);

	fake.expect = smu.data;
	fake.expectSize = smu.size;
	Psp::Response resp;
	r = psp.loadIpFw(smu, Psp::FwSmu, resp);
	failures += check(r.ok && fake.fwType == Psp::FwSmu && fake.payloadOk,
	                  "psp: LOAD_IP_FW(SMU) %s type %u size %u payload %s", r.what, fake.fwType,
	                  fake.fwSize, fake.payloadOk ? "ok" : "MISMATCH");

	// Run the ring past its 64 frames: wrap-around keeps fences in step.
	bool wrapOk = true;
	for (int i = 0; i < 70 && wrapOk; i++)
		wrapOk = psp.submit(Psp::CmdFbFwReservAddr, nullptr, 0, resp).ok;
	failures += check(wrapOk && fake.gpcom.size() == 72 && psp.fenceValue() == 72,
	                  "psp: ring wrap (%zu commands, fence %u)", fake.gpcom.size(), psp.fenceValue());
	failures += check(fake.flushes >= 12, "psp: HDP flushes %d", fake.flushes);

	// psp_cmd_submit_buf: a nonzero response status is a warning, not a failure.
	fake.forceStatus = 0xa;
	r = psp.submit(Psp::CmdFbFwReservAddr, nullptr, 0, resp);
	failures += check(r.ok && r.value == 0xa, "psp: nonzero status should warn, not fail (%s)",
	                  r.what);
	fake.forceStatus = 0;

	// A bootloader that never comes back stops at the first component.
	FakePsp stuck;
	stuck.mcBase = fake.mcBase;
	stuck.vram.assign(8u << 20, 0);
	stuck.regs[Psp::Reg::kC2p35] = 0x80000000;
	stuck.bootloaderStuck = true;
	Psp::Bus bus2 { &stuck, FakePsp::rd, FakePsp::wr, FakePsp::delay, FakePsp::flush };
	Psp::Window win2 { stuck.vram.data(), stuck.mcBase, static_cast<uint32_t>(stuck.vram.size()) };
	Psp::Driver psp2;
	psp2.init(bus2, win2);
	r = psp2.loadSos(pkg, loaded);
	failures += check(!r.ok && loaded == 0 && !strcmp(r.what, "KDB"),
	                  "psp: stuck bootloader should stop at KDB (got %s)", r.what);

	printf("\npsp: sOS package %u parts (sOS 0x%08x), SMU %u bytes; bootloader, ring, LOAD_TOC, "
	       "LOAD_IP_FW and wrap %s\n", pkg.count, pkg.version[AmdFw::PspSos], smu.size,
	       failures ? "FAILED" : "ok");
	return failures;
}

// The GC 12 firmware set the PSP loads for the RLC autoload (stage 3).
static int testGfxImages() {
	int failures = 0;
	static const char *const names[] = {
		"firmware/amdgpu/sdma_7_0_1.bin", "firmware/amdgpu/gc_12_0_1_pfp.bin",
		"firmware/amdgpu/gc_12_0_1_me.bin", "firmware/amdgpu/gc_12_0_1_mec.bin",
		"firmware/amdgpu/gc_12_0_1_uni_mes.bin", "firmware/amdgpu/gc_12_0_1_imu.bin",
		"firmware/amdgpu/gc_12_0_1_rlc.bin",
	};
	std::vector<uint8_t> files[7];
	for (int i = 0; i < 7; i++)
		if (!readFile(names[i], files[i])) {
			printf("\ngfx images: %s absent, skipped\n", names[i]);
			return 0;
		}
	auto blob = [&](int i) { return AmdFw::Blob { files[i].data(), static_cast<uint32_t>(files[i].size()) }; };
	AmdFw::GfxBlobs b { blob(0), blob(1), blob(2), blob(3), blob(4), blob(5), blob(6) };

	AmdFw::GfxImage img[AmdFw::kMaxGfxImages];
	const char *why = nullptr;
	uint32_t n = AmdFw::buildGfxImages(b, img, AmdFw::kMaxGfxImages, &why);
	failures += check(n == 19, "gfx images: %u built (%s)", n, why ? why : "-");

	// amdgpu's load order and types; RLC_G last (it triggers the autoload).
	// Both MES pipes load uni_mes.bin (pipe 1 as the MES KIQ types 81/82).
	static const struct { const char *name; uint32_t type; } expect[] = {
		{ "SDMA_RS64", 71 }, { "RS64_PFP", 87 }, { "RS64_ME", 88 }, { "RS64_MEC", 89 },
		{ "RS64_PFP_P0_STACK", 90 }, { "RS64_ME_P0_STACK", 92 }, { "RS64_MEC_P0_STACK", 94 },
		{ "RS64_MEC_P1_STACK", 95 }, { "CP_MES", 33 }, { "CP_MES_DATA", 34 },
		{ "CP_MES1", 81 }, { "CP_MES1_DATA", 82 }, { "IMU_I", 68 }, { "IMU_D", 69 },
	};
	for (uint32_t i = 0; i < 14 && i < n; i++)
		failures += check(!strcmp(img[i].name, expect[i].name) && img[i].pspType == expect[i].type,
		                  "gfx images: [%u] %s/%u, expected %s/%u", i, img[i].name,
		                  img[i].pspType, expect[i].name, expect[i].type);
	failures += check(n && !strcmp(img[n - 1].name, "RLC_G") && img[n - 1].pspType == 8 &&
	                  img[n - 1].payload.size == 25088, "gfx images: RLC_G must come last");
	failures += check(n >= 3 && !strcmp(img[n - 3].name, "RLC_IRAM") &&
	                  !strcmp(img[n - 2].name, "RLC_DRAM"), "gfx images: RLC IRAM/DRAM before RLC_G");

	// Every payload lies inside its blob, is dword-sized and fits the PSP
	// staging area; IMU I+D cover the IMU ucode.
	bool inside = true;
	for (uint32_t i = 0; i < n; i++) {
		bool ok = false;
		for (auto &f : files)
			ok |= img[i].payload.data >= f.data() &&
			      img[i].payload.data + img[i].payload.size <= f.data() + f.size();
		inside &= ok && img[i].payload.size % 4 == 0 &&
		          img[i].payload.size <= 62u * 1024 * 1024;
	}
	failures += check(inside, "gfx images: payload outside its blob or misaligned");
	failures += check(img[12].payload.size + img[13].payload.size == 132096,
	                  "gfx images: IMU I+D = %u bytes", img[12].payload.size + img[13].payload.size);
	failures += check(img[10].payload.data == img[8].payload.data &&
	                  img[11].payload.data == img[9].payload.data,
	                  "gfx images: MES1 must reuse the uni_mes ucode/data payloads");

	// Truncated blobs are refused, not over-read.
	AmdFw::GfxBlobs cut = b;
	cut.rlc.size = 150;
	failures += check(AmdFw::buildGfxImages(cut, img, AmdFw::kMaxGfxImages, &why) == 0,
	                  "gfx images: truncated RLC accepted");

	n = AmdFw::buildGfxImages(b, img, AmdFw::kMaxGfxImages, &why);
	printf("\ngfx images: %u to load before AUTOLOAD_RLC:", n);
	for (uint32_t i = 0; i < n; i++)
		printf("%s %s(%u)", i % 4 ? "" : "\n ", img[i].name, img[i].payload.size);
	printf("\n");
	return failures;
}

// SDMA 7 packets, as sdma_v7_0.c emits them (ring test, fence, fill, copy).
static int testSdmaPackets() {
	int failures = 0;
	uint32_t p[8];
	const uint64_t a = 0x8009002000ull, b = 0x8010000000ull;

	failures += check(Sdma::writeDword(p, a, 0xDEADBEEF) == 5 && p[0] == 0x00000002 &&
	                  p[1] == 0x09002000 && p[2] == 0x80 && p[3] == 0 && p[4] == 0xDEADBEEF,
	                  "sdma: WRITE_LINEAR %08x %08x %08x %08x %08x", p[0], p[1], p[2], p[3], p[4]);
	failures += check(Sdma::fence(p, a + 0x41, 7) == 4 && p[0] == 0x00030005 &&
	                  p[1] == 0x09002040 && p[2] == 0x80 && p[3] == 7,
	                  "sdma: FENCE %08x %08x %08x %08x", p[0], p[1], p[2], p[3]);
	failures += check(Sdma::constFill(p, b, 0x5A5AC0DE, 1u << 20) == 5 && p[0] == 0x8000000b &&
	                  p[1] == 0x10000000 && p[2] == 0x80 && p[3] == 0x5A5AC0DE && p[4] == 0xfffff,
	                  "sdma: CONST_FILL %08x .. %08x", p[0], p[4]);
	failures += check(Sdma::copyLinear(p, a, b, 4096) == 8 && p[0] == 0x00080001 && p[1] == 4095 &&
	                  p[3] == 0x09002000 && p[5] == 0x10000000 && p[7] == 0,
	                  "sdma: COPY_LINEAR %08x %08x", p[0], p[1]);
	failures += check(Sdma::trap(p) == Sdma::kTrapDwords && p[0] == 0x00000006 && p[1] == 0,
	                  "sdma: TRAP %08x %08x", p[0], p[1]);

	// Ring: 256-byte ring; packets wrap in memory, the wptr never does
	// (SDMA 7's 64-bit pointers: a wptr back at the start stalls the card).
	uint32_t mem[64];
	Sdma::Ring r;
	failures += check(r.init(mem, 0x8008800000ull, sizeof(mem)) && r.sizeLog2Dwords() == 6,
	                  "sdma: ring init");
	bool ok = true;
	for (int i = 0; i < 20 && ok; i++)
		ok = r.emit(p, Sdma::writeDword(p, a, static_cast<uint32_t>(i)));
	failures += check(ok && r.wptr() == 20 * 5 * 4 && mem[((19 * 5 + 4) * 4 % 256) / 4] == 19,
	                  "sdma: ring wrap (wptr %llu)", static_cast<unsigned long long>(r.wptr()));
	failures += check(!r.init(mem, 0x8008800010ull, sizeof(mem)), "sdma: unaligned ring accepted");
	// P7: a ring that RESUMES at the engine's own 64-bit pointers (a wake without power loss): the wptr continues monotonically from there and the packets
	// land at the pointer modulo the ring size.
	{
		uint32_t rm[64];
		Sdma::Ring rr;
		const uint64_t start = 0x17604;            // the emulator's 11s run: 95748 bytes
		ok = rr.init(rm, 0x8008800000ull, sizeof(rm), start) && rr.wptr() == start && rm[0] == 0;
		ok = ok && rr.emit(p, Sdma::writeDword(p, a, 0x77)) && rr.wptr() == start + 5 * 4;
		failures += check(ok && rm[(start & 255) / 4] == p[0] && rr.wptr() > start,
		                  "sdma: ring resumed at the engine's pointers (wptr 0x%llx)", static_cast<unsigned long long>(rr.wptr()));
		failures += check(rr.init(rm, 0x8008800000ull, sizeof(rm), 0x17607) && rr.wptr() == 0x17604, "sdma: a resume pointer is dword aligned");
	}

	printf("\nsdma: WRITE_LINEAR/FENCE/CONST_FILL/COPY_LINEAR encodings and ring wrap %s\n",
	       failures ? "FAILED" : "ok");
	return failures;
}

// IH 7.0 vector fields and producer/consumer arithmetic.
static int testIhRing() {
	int failures = 0;
	uint32_t dw[Ih::kEntryDwords] = {
		0x8004030a, 0x11223344, 0x00005678, 0x00ab1234,
		0xfeed0001, 0xfeed0002, 0xfeed0003, 0xfeed0004,
	};
	Ih::Entry e {};
	Ih::decode(dw, e);
	failures += check(e.clientId == 0x0a && e.srcId == 3 && e.ringId == 4 && e.vmid == 0 &&
	                  e.vmidSrc && e.timestamp == 0x567811223344ull && e.pasid == 0x1234 &&
	                  e.vmidSrcNode == 0xab && e.srcData[0] == 0xfeed0001 &&
	                  e.srcData[3] == 0xfeed0004,
	                  "ih: decode fields client=%u src=%u ring=%u timestamp=0x%llx",
	                  e.clientId, e.srcId, e.ringId,
	                  static_cast<unsigned long long>(e.timestamp));
	constexpr uint32_t size = 256u << 10;
	failures += check(Ih::advance(size - 16, 32, size) == 16 && Ih::hasEntries(size - 32, 0, size) &&
	                  Ih::overflowRecovery(size - 32, size) == 0,
	                  "ih: ring wrap/overflow arithmetic");
        failures += check(!Ih::missEligible(false, true, false, true) &&
                          !Ih::missEligible(true, true, false, false) &&
                          !Ih::missEligible(true, true, true, true) &&
                          Ih::missEligible(true, true, false, true),
                          "ih: miss qualification requires sleep, completion, and a 5 ms recheck");
		failures += check(Ih::isMec1Ring(4) && Ih::isMec1Ring(5) &&
		                  Ih::isMec1Ring(0x74) && !Ih::isMec1Ring(0) &&
		                  !Ih::isMec1Ring(0x10),
		                  "ih: EOP accepts every MEC1 pipe/queue ring");
	printf("\nih: v7 decode, ring wrap/overflow arithmetic, and wait-miss qualification %s\n",
	       failures ? "FAILED" : "ok");
	return failures;
}

// SmuMetrics_t as smu14_driver_if_v14_0.h:1649-1727 declares it, with the
// enum counts of that header (PPCLK_COUNT :467, SVI_PLANE_COUNT :563,
// TEMP_COUNT :549, THROTTLER_COUNT :216). The compiler computes the offsets,
// so this test checks the constants against the layout, not against themselves.
namespace {
struct SmuMetricsMirror {
	uint32_t CurrClock[11];
	uint16_t AverageGfxclkFrequencyTarget, AverageGfxclkFrequencyPreDs,
	         AverageGfxclkFrequencyPostDs, AverageFclkFrequencyPreDs,
	         AverageFclkFrequencyPostDs, AverageMemclkFrequencyPreDs,
	         AverageMemclkFrequencyPostDs, AverageVclk0Frequency, AverageDclk0Frequency,
	         AverageVclk1Frequency, AverageDclk1Frequency, AveragePCIeBusy, dGPU_W_MAX, padding;
	uint16_t MovingAverageGfxclkFrequencyTarget, MovingAverageGfxclkFrequencyPreDs,
	         MovingAverageGfxclkFrequencyPostDs, MovingAverageFclkFrequencyPreDs,
	         MovingAverageFclkFrequencyPostDs, MovingAverageMemclkFrequencyPreDs,
	         MovingAverageMemclkFrequencyPostDs, MovingAverageVclk0Frequency,
	         MovingAverageDclk0Frequency, MovingAverageGfxActivity, MovingAverageUclkActivity,
	         MovingAverageVcn0ActivityPercentage, MovingAveragePCIeBusy,
	         MovingAverageUclkActivity_MAX, MovingAverageSocketPower, MovingAveragePadding;
	uint32_t MetricsCounter;
	uint16_t AvgVoltage[4];
	uint16_t AvgCurrent[4];
	uint16_t AverageGfxActivity, AverageUclkActivity, AverageVcn0ActivityPercentage,
	         Vcn1ActivityPercentage;
	uint32_t EnergyAccumulator;
	uint16_t AverageSocketPower, AverageTotalBoardPower;
	uint16_t AvgTemperature[12];
	uint16_t AvgTemperatureFanIntake;
	uint8_t  PcieRate, PcieWidth, AvgFanPwm, Padding[1];
	uint16_t AvgFanRpm;
	uint8_t  ThrottlingPercentage[21];
	uint8_t  VmaxThrottlingPercentage, padding1[2];
};
}

static int testSmuMetricsPmOffsets() {
	int failures = 0;
	failures += check(offsetof(SmuMetricsMirror, CurrClock) == RDNA4_SMU_METRICS_CURR_CLOCK &&
	                  offsetof(SmuMetricsMirror, AverageGfxclkFrequencyPreDs) == RDNA4_SMU_METRICS_AVG_GFXCLK_PRE_DS &&
	                  offsetof(SmuMetricsMirror, AverageGfxclkFrequencyPostDs) == RDNA4_SMU_METRICS_AVG_GFXCLK_POST_DS &&
	                  offsetof(SmuMetricsMirror, AverageMemclkFrequencyPostDs) == RDNA4_SMU_METRICS_AVG_MEMCLK_POST_DS &&
	                  offsetof(SmuMetricsMirror, MovingAverageGfxActivity) == RDNA4_SMU_METRICS_MOVING_AVG_GFX_ACT &&
	                  offsetof(SmuMetricsMirror, MetricsCounter) == RDNA4_SMU_METRICS_COUNTER &&
	                  offsetof(SmuMetricsMirror, AvgVoltage) == RDNA4_SMU_METRICS_AVG_VOLTAGE &&
	                  offsetof(SmuMetricsMirror, AvgCurrent) == RDNA4_SMU_METRICS_AVG_CURRENT &&
	                  offsetof(SmuMetricsMirror, AverageGfxActivity) == RDNA4_SMU_METRICS_AVG_GFX_ACTIVITY &&
	                  offsetof(SmuMetricsMirror, AverageUclkActivity) == RDNA4_SMU_METRICS_AVG_UCLK_ACTIVITY &&
	                  offsetof(SmuMetricsMirror, AverageSocketPower) == RDNA4_SMU_METRICS_AVG_SOCKET_POWER &&
	                  offsetof(SmuMetricsMirror, AvgTemperature) == RDNA4_SMU_METRICS_AVG_TEMPERATURE &&
	                  offsetof(SmuMetricsMirror, AvgFanRpm) == RDNA4_SMU_METRICS_AVG_FAN_RPM &&
	                  offsetof(SmuMetricsMirror, ThrottlingPercentage) == RDNA4_SMU_METRICS_THROTTLING_PCT &&
	                  sizeof(((SmuMetricsMirror *)0)->ThrottlingPercentage) == RDNA4_SMU_METRICS_THROTTLER_COUNT,
	                  "smu: pm metrics offsets vs the SmuMetrics_t layout (counter at %u, activity at %u)",
	                  RDNA4_SMU_METRICS_COUNTER, RDNA4_SMU_METRICS_AVG_GFX_ACTIVITY);
	printf("\nsmu: SmuMetrics_t power-management offsets %s\n", failures ? "FAILED" : "ok");
	return failures;
}

static int testSmuMetricsOffsets();
static int testSmuMetricsOffsets() {
	int failures = 0;
	failures += check(RDNA4_SMU_METRICS_AVG_GFXCLK_POST_DS == 48u &&
	                  RDNA4_SMU_METRICS_AVG_MEMCLK_POST_DS == 56u &&
	                  RDNA4_SMU_METRICS_AVG_SOCKET_POWER == 136u &&
	                  RDNA4_SMU_METRICS_AVG_TEMPERATURE == 140u &&
	                  RDNA4_SMU_METRICS_AVG_FAN_RPM == 170u,
	                  "smu: SmuMetrics_t offsets (fan at byte %u)",
	                  RDNA4_SMU_METRICS_AVG_FAN_RPM);
	printf("\nsmu: SmuMetrics_t telemetry offsets %s\n", failures ? "FAILED" : "ok");
	failures += testSmuMetricsPmOffsets();
	return failures;
}

// PM4 packets as gfx_v12_0 emits them (compute ring test, fence).
static int testPm4Packets() {
	int failures = 0;
	uint32_t p[8];
	const uint64_t a = 0x800c003080ull;

	// PACKET3(SET_UCONFIG_REG, 1), SCRATCH_REG0 at 0xc040 -> 0x40.
	failures += check(Pm4::setUconfigReg(p, 0xc040, 0xDEADBEEF) == 3 && p[0] == 0xc0017900 &&
	                  p[1] == 0x40 && p[2] == 0xDEADBEEF,
	                  "pm4: SET_UCONFIG_REG %08x %08x", p[0], p[1]);
	// PACKET3(WRITE_DATA, 3), DST_SEL(5) | WR_CONFIRM.
	failures += check(Pm4::writeData(p, a, 7) == 5 && p[0] == 0xc0033700 && p[1] == 0x00100500 &&
	                  p[2] == 0x0c003080 && p[3] == 0x80 && p[4] == 7,
	                  "pm4: WRITE_DATA %08x %08x", p[0], p[1]);
	// PACKET3(RELEASE_MEM, 6): GCR_SEQ | GCR_GL2_WB | CACHE_POLICY(3) |
	// EVENT_TYPE(0x14) | EVENT_INDEX(5); DATA_SEL(1).
	failures += check(Pm4::releaseMem(p, a, 9) == 8 && p[0] == 0xc0064900 &&
	                  p[1] == 0x06600514 && p[2] == 0x20000000 && p[5] == 9 && p[7] == 0,
	                  "pm4: RELEASE_MEM %08x %08x %08x", p[0], p[1], p[2]);
	failures += check(Pm4::releaseMem(p, a, 10, true) == 8 &&
	                  p[2] == (Pm4::kReleaseData32 | Pm4::kReleaseIntSel2),
	                  "pm4: RELEASE_MEM interrupt select %08x", p[2]);

	// PACKET3(SET_SH_REG, 2): COMPUTE_PGM_LO/HI (GC seg0 0x1260 + 0x1bac).
	const uint32_t pgm[2] = { 0x80080e00, 0 };
	failures += check(Pm4::setShReg(p, 0x1260 + 0x1bac, pgm, 2) == 4 && p[0] == 0xc0027600 &&
	                  p[1] == 0x20c && p[2] == 0x80080e00, "pm4: SET_SH_REG %08x %08x", p[0], p[1]);
	failures += check(Pm4::dispatchDirect(p, 4, 1, 1, Pm4::kDispatchShaderEn | Pm4::kDispatchWave32) == 5 &&
	                  p[0] == 0xc0031500 && p[1] == 4 && p[4] == 0x8001,
	                  "pm4: DISPATCH_DIRECT %08x %08x", p[0], p[4]);
	// gfx_v12_0_emit_mem_sync's GCR_CNTL = 0xc3b1.
	failures += check(Pm4::acquireMem(p, Pm4::kGcrMemSync) == 8 && p[0] == 0xc0065800 &&
	                  p[2] == 0xffffffff && p[3] == 0xffffff && p[6] == 0xa && p[7] == 0xc3b1,
	                  "pm4: ACQUIRE_MEM %08x gcr %08x", p[0], p[7]);
	failures += check(Pm4::indirectBufferCompute(p, a, 37, 8) == 4 && p[0] == 0xc0023f00 &&
	                  p[1] == 0x0c003080 && p[2] == 0x80 && p[3] == 0x08800025,
	                  "pm4: compute INDIRECT_BUFFER VALID/VMID %08x", p[3]);
	failures += check(!(p[3] & ((1u << 20) | (1u << 21) | (1u << 31))),
	                  "pm4: compute INDIRECT_BUFFER is unprivileged and unchained");

	uint32_t mem[256];
	Pm4::Queue q;
	failures += check(q.init(mem, 0x800c002000ull, sizeof(mem)) && q.queueSizeField() == 7 &&
	                  mem[0] == 0xffff1000u, "pm4: queue init (QUEUE_SIZE %u, fill %08x)",
	                  q.queueSizeField(), mem[0]);
	bool ok = true;
	for (int i = 0; i < 60 && ok; i++)
		ok = q.emit(p, Pm4::releaseMem(p, a, static_cast<uint32_t>(i)));
	failures += check(ok && q.wptr() == 480 && mem[(480 - 8) % 256] == 0xc0064900,
	                  "pm4: queue wrap (wptr %llu)", static_cast<unsigned long long>(q.wptr()));

	printf("\npm4: SET_UCONFIG_REG/WRITE_DATA/RELEASE_MEM encodings and queue wrap %s\n",
	       failures ? "FAILED" : "ok");
	return failures;
}

// The code-object loader against clang's own gfx1201 output (shaders/vadd.cl).
static int testCodeObject() {
	int failures = 0;
	CodeObj::Kernel k;
	const char *why = nullptr;
	bool ok = CodeObj::findKernel(kVaddCodeObject, sizeof(kVaddCodeObject), "vadd", k, &why);
	failures += check(ok, "codeobj: vadd not found (%s)", why ? why : "-");
	const uint64_t entryVa = k.entryVa;
	if (ok) {
		// llvm-readelf: .text at file offset 0x600, 0x280 bytes; the descriptor
		// says 24 bytes of kernargs, RSRC1 0x600f0040, RSRC2 0x84 (2 user SGPRs,
		// TGID_X_EN), kernarg pointer and wave32 enabled.
		uint32_t first = static_cast<uint32_t>(kVaddCodeObject[k.codeOffset]) |
		                 (kVaddCodeObject[k.codeOffset + 1] << 8) |
		                 (kVaddCodeObject[k.codeOffset + 2] << 16) |
		                 (static_cast<uint32_t>(kVaddCodeObject[k.codeOffset + 3]) << 24);
		failures += check(k.codeOffset == 0x600 && k.codeSize == 0x280 && first == 0xf4004100,
		                  "codeobj: code at 0x%x (%u bytes), first dword 0x%08x", k.codeOffset,
		                  k.codeSize, first);
		failures += check(k.kernargSize == 24 && k.rsrc1 == 0x600f0040 && k.rsrc2 == 0x84 &&
		                  k.rsrc3 == 0 && k.userSgprCount() == 2 && k.wave32() &&
		                  k.wantsKernargPtr() && !k.wantsDispatchPtr() && !k.groupSegmentSize &&
		                  !k.privateSegmentSize,
		                  "codeobj: descriptor kernarg %u rsrc1 0x%08x rsrc2 0x%08x props",
		                  k.kernargSize, k.rsrc1, k.rsrc2);
	}
	failures += check(!CodeObj::findKernel(kVaddCodeObject, sizeof(kVaddCodeObject), "vsub", k, &why),
	                  "codeobj: a missing kernel was found");
	failures += check(!CodeObj::findKernel(kVaddCodeObject, 200, "vadd", k, &why),
	                  "codeobj: a truncated file was accepted");

	// The whole image (llvm-readelf -l): PT_LOADs at 0 (0x5c4 bytes),
	// 0x1600 (.text, from file 0x600, 0x280) and 0x2880 (0x70 in the file,
	// 0x780 in memory): 0x3000 bytes, code at +0x1600.
	CodeObj::Image img;
	ok = CodeObj::parseImage(kVaddCodeObject, sizeof(kVaddCodeObject), img, &why);
	failures += check(ok && img.count == 3 && img.size == 0x3000 && entryVa == 0x1600,
	                  "codeobj: image %s: %u segments, 0x%llx bytes, entry 0x%llx",
	                  ok ? "ok" : why, img.count, (unsigned long long)img.size,
	                  (unsigned long long)entryVa);
	if (ok && img.count == 3) {
		const auto &s = img.seg[1];
		failures += check(s.fileOffset == 0x600 && s.fileSize == 0x280 && s.vaddr == 0x1600 &&
		                  img.seg[2].fileSize == 0x70 && img.seg[2].memSize == 0x780,
		                  "codeobj: segment layout");
	}
	failures += check(!CodeObj::parseImage(kVaddCodeObject, 200, img, &why),
	                  "codeobj: image of a truncated file accepted");

	// bench.cl: eight kernels in one file, each with its own descriptor and
	// LDS (llvm-readelf --notes: group_segment_fixed_size).
	struct { const char *name; uint32_t lds, kernarg; } bench[] = {
		{ "lds_reverse", 256, 24 }, { "spin", 0, 8 }, { "copy", 0, 16 }, { "sgemm", 8320, 28 },
		{ "wmma16", 0, 24 }, { "hgemm", 20480, 28 }, { "bf16gemm", 20480, 28 },
		{ "mandelbrot", 0, 40 }, { "mandelbrot_zoom", 0, 40 },
	};
	const int nBench = sizeof(bench) / sizeof(bench[0]);
	uint64_t entries[nBench] = {};
	for (int i = 0; i < nBench; i++) {
		ok = CodeObj::findKernel(kBenchCodeObject, sizeof(kBenchCodeObject), bench[i].name, k, &why);
		entries[i] = k.entryVa;
		failures += check(ok && k.groupSegmentSize == bench[i].lds && k.kernargSize == bench[i].kernarg &&
		                  !k.privateSegmentSize && k.wave32() && !(k.entryVa & 0xff),
		                  "codeobj: bench %s: %s, LDS %u kernarg %u entry 0x%llx", bench[i].name,
		                  ok ? "found" : why, k.groupSegmentSize, k.kernargSize,
		                  (unsigned long long)k.entryVa);
	}
	ok = CodeObj::parseImage(kBenchCodeObject, sizeof(kBenchCodeObject), img, &why);
	bool distinct = true;
	for (int i = 0; i < nBench; i++) {
		distinct = distinct && entries[i] < img.size;
		for (int j = i + 1; j < nBench; j++)
			distinct = distinct && entries[i] != entries[j];
	}
	failures += check(ok && distinct,
	                  "codeobj: bench image %s, 0x%llx bytes", ok ? "ok" : why,
	                  (unsigned long long)img.size);

	// RSRC2.LDS_SIZE as Mesa encodes it for gfx12 compute: 1 KiB-aligned,
	// in 512-byte units.
	failures += check(GfxReg::ldsSizeField(0) == 0 && GfxReg::ldsSizeField(256) == (2u << 15) &&
	                  GfxReg::ldsSizeField(8320) == (18u << 15) &&
	                  GfxReg::ldsSizeField(65536) == (128u << 15),
	                  "lds: RSRC2.LDS_SIZE encoding");
	printf("\ncodeobj: clang's vadd and bench code objects: kernels, descriptors, LDS and image "
	       "located %s\n", failures ? "FAILED" : "ok");
	return failures;
}

static int testGpuHeap() {
	int failures = 0;
	GpuHeap::Heap h;
	static uint8_t map[64];
	constexpr uint64_t G = 4096, base = 32ull << 20;
	h.init(base, 64 * G + 100, G, map, sizeof(map));   // the partial granule is dropped
	failures += check(h.size() == 64 * G && h.freeBytes() == 64 * G, "heap: size %llu",
	                  (unsigned long long)h.size());

	uint64_t a = 0, b = 0, c = 0, d = 0;
	bool ok = h.alloc(1, a) && h.alloc(3 * G, b) && h.alloc(G + 1, c);
	failures += check(ok && a == base && b == base + G && c == base + 4 * G &&
	                  h.lengthOf(b) == 3 * G && h.lengthOf(c) == 2 * G &&
	                  h.freeBytes() == 58 * G, "heap: first-fit offsets");
	failures += check(!h.alloc(0, d) && !h.alloc(65 * G, d), "heap: empty/oversized allocation");
	failures += check(!h.free(b + G) && !h.free(base + 100) && !h.free(base + 64 * G),
	                  "heap: free of a non-allocation accepted");
	// A hole between a and c: a larger request goes past c, a fitting one
	// fills the hole.
	failures += check(h.free(b) && !h.free(b), "heap: free / double free");
	ok = h.alloc(4 * G, d) && d == base + 6 * G && h.alloc(2 * G, b) && b == base + G;
	failures += check(ok, "heap: hole reuse (d 0x%llx b 0x%llx)", (unsigned long long)d,
	                  (unsigned long long)b);
	// Adjacent allocations stay separate: freeing one leaves its neighbours.
	failures += check(h.free(b) && h.lengthOf(c) == 2 * G && h.lengthOf(a) == G,
	                  "heap: neighbours after a free");
	h.free(a);
	h.free(c);
	h.free(d);
	failures += check(h.freeBytes() == h.size() && h.alloc(64 * G, a) && a == base,
	                  "heap: everything back in one piece");

	// The device heap's shape: gigabytes in 64 KiB granules, the map cut
	// short by its capacity, and allocations far from the base.
	GpuHeap::Heap big;
	static uint8_t bigMap[131072];
	constexpr uint64_t BG = 64 << 10, vbase = 256ull << 20;
	big.init(vbase, 8ull << 30, BG, bigMap, sizeof(bigMap));
	failures += check(big.size() == 8ull << 30 && big.granule() == BG, "heap: big size %llu",
	                  (unsigned long long)big.size());
	uint64_t x = 0, y = 0, z = 0;
	ok = big.alloc(768ull << 20, x) && big.alloc(1, y) && big.alloc(5ull << 30, z);
	failures += check(ok && x == vbase && y == vbase + (768ull << 20) &&
	                  z == y + BG && big.lengthOf(z) == 5ull << 30 && !big.alloc(3ull << 30, x),
	                  "heap: big allocations (x 0x%llx y 0x%llx z 0x%llx)",
	                  (unsigned long long)x, (unsigned long long)y, (unsigned long long)z);
	GpuHeap::Heap none;
	none.init(0, 1 << 20, 4096, nullptr, 0);
	failures += check(!none.size() && !none.alloc(1, x), "heap: a heap without a map is empty");
	// The struct the user client copies in is the one user space sends.
	failures += check(sizeof(RDNA4Dispatch) == 2088 && RDNA4_MAX_KERNARG == 2048,
	                  "abi: RDNA4Dispatch is %zu bytes", sizeof(RDNA4Dispatch));
	printf("\nheap: first-fit VRAM heap allocates, frees and refuses %s\n",
	       failures ? "FAILED" : "correctly");
	return failures;
}

// W43: the `linux diff:` formatter is bounded. Worst-case 'why' strings and every capacity from 1 up must leave the canary bytes
// after the buffer untouched, keep the string NUL-terminated inside the capacity, and never report a length past it.
static int testLinuxRefFormat() {
	int failures = 0;
	static char longWhy[4096];
	memset(longWhy, 'w', sizeof(longWhy) - 1);
	longWhy[sizeof(longWhy) - 1] = '\0';
	const LinuxRefTable::Ref worst = { "A_VERY_LONG_REGISTER_NAME_FOR_THE_TEST", { 1, 0xffff }, 0xffffffffu, 0xffffffffu, longWhy };
	const LinuxRefTable::Ref noWhy = { "R", { 0, 1 }, 1, 0xffffffffu, "" };
	const LinuxRefTable::Ref nullWhy = { "R", { 0, 1 }, 1, 0xffffffffu, nullptr };
	bool ok = true;
	size_t maxReal = 0;
	for (size_t cap = 1; cap <= 600 && ok; cap++) {
		char buf[700];
		memset(buf, 0xA5, sizeof(buf));
		for (int which = 0; which < 3 && ok; which++) {
			const LinuxRefTable::Ref &r = which == 0 ? worst : which == 1 ? noWhy : nullWhy;
			memset(buf, 0xA5, sizeof(buf));
			const size_t len = LinuxRefTable::format(buf, cap, r, 0x12345678u);
			bool canary = true;
			for (size_t i = cap; i < sizeof(buf); i++)
				canary = canary && static_cast<unsigned char>(buf[i]) == 0xA5;
			ok = canary && len < cap && buf[len] == '\0' && strlen(buf) == len;
			if (!ok)
				printf("  linuxref: cap %zu entry %d: len %zu canary %d\n", cap, which, len, (int)canary);
		}
	}
	failures += check(ok, "linuxref: every capacity 1..600 stays inside its buffer with a %zu-char why", strlen(longWhy));
	char zero[4] = { 'x', 'x', 'x', 'x' };
	failures += check(LinuxRefTable::format(zero, 0, worst, 1) == 0 && zero[0] == 'x', "linuxref: capacity 0 writes nothing");
	failures += check(LinuxRefTable::format(nullptr, 8, worst, 1) == 0, "linuxref: a null buffer is refused");
	// The real table: every entry fits the line the kext uses without truncation.
	bool fits = true;
	for (size_t i = 0; i < LinuxRefTable::kCount; i++) {
		char line[LinuxRefTable::kLineMax];
		const size_t len = LinuxRefTable::format(line, sizeof(line), LinuxRefTable::kRefs[i], 0xffffffffu);
		maxReal = len > maxReal ? len : maxReal;
		const size_t full = strlen(LinuxRefTable::kRefs[i].name) + strlen(LinuxRefTable::kRefs[i].why) + 64;
		fits = fits && len < sizeof(line) - 1 && full < sizeof(line) * 2;
		fits = fits && (len == 0 || line[len] == '\0');
	}
	failures += check(fits && maxReal + 1 < LinuxRefTable::kLineMax, "linuxref: the real table's longest line is %zu of %zu bytes (not truncated)",
	                  maxReal, LinuxRefTable::kLineMax);
	printf("\nlinuxref: the linux-diff formatter is bounded %s\n", failures ? "FAILED" : "ok");
	return failures;
}

static int testFlipArithmetic() {
	int failures = 0;
	failures += check(Flip::surfaceBytes(1920, 1080) == 1920ull * 1080 * 4,
	                  "flip: 1920x1080 surface size");
	failures += check(Flip::surfaceBytes(3840, 2160) == 3840ull * 2160 * 4,
	                  "flip: 3840x2160 surface size");
	failures += check(Flip::surfaceBytes(0, 1080) == 0 && Flip::surfaceBytes(1920, 0) == 0,
	                  "flip: zero-sized surface accepted");
	const uint64_t address = 0x123456789abcde00ull;
	failures += check(Flip::addressLo(address) == 0x9abcde00u &&
	                  Flip::addressHi(address) == 0x12345678u,
	                  "flip: address split");
	printf("\nflip: surface arithmetic and address split %s\n",
	       failures ? "FAILED" : "ok");
	return failures;
}

// A synthetic v1 discovery binary (binary_header + IPDS + one die + a GC
// table) for IpDiscovery::gcInfo. init() needs a >= 512 byte buffer whose
// checksum, IPDS and die header are right, so build those, then vary the GC
// table: its offset, table_id, version and where it ends.
static void buildDiscovery(uint8_t *b, uint16_t gcOff, uint32_t tableId, uint16_t gcMajor,
                           uint32_t se, uint32_t rbPerSe) {
	memset(b, 0, 512);
	auto p16 = [&](size_t o, uint16_t v) { b[o] = v & 0xff; b[o + 1] = v >> 8; };
	auto p32 = [&](size_t o, uint32_t v) { p16(o, v & 0xffff); p16(o + 2, v >> 16); };
	p32(0, 0x28211407);                        // binary_header: signature
	p16(4, 1); p16(6, 3);                      // version 1.3 (this card's ROM)
	p16(10, 512);                              // binary_size
	p16(12, 0x40); p16(16, 0x60);              // table_list[0] IP_DISCOVERY: offset, size
	p16(20, gcOff);                            // table_list[1] GC: offset
	p32(0x40, 0x53445049);                     // "IPDS"
	p16(0x40 + 12, 1);                         // num_dies
	p16(0x40 + 16, 0xb0);                      // die_info[0].die_offset
	p16(0xb0 + 2, 1);                          // die_header.num_ips
	if (gcOff) {
		p32(gcOff, tableId);                   // gpu_info_header
		p16(gcOff + 4, gcMajor); p16(gcOff + 6, 0);
		if (gcOff + 12 < 512) p32(gcOff + 12, se);
		if (gcOff + 24 < 512) p32(gcOff + 24, rbPerSe);
	}
	uint16_t sum = 0;
	for (size_t i = 10; i < 512; i++)
		sum = static_cast<uint16_t>(sum + b[i]);
	p16(8, sum);
}

static int testGcInfo() {
	int failures = 0;
	uint8_t buf[512];
	IpDiscovery d;
	uint32_t se = 99, rb = 99, ver = 0;

	buildDiscovery(buf, 0xc0, 0x4347, 1, 4, 4);
	failures += check(d.init(buf, sizeof(buf)), "gc_info: the synthetic binary initialises");
	failures += check(d.gcInfo(se, rb, &ver) && se == 4 && rb == 4 && ver == (1u << 16),
	                  "gc_info: good v1.0 table gives 4 SEs, 4 RBs per SE, version 1.0");
	buildDiscovery(buf, 0xc0, 0x4347, 2, 4, 4);
	d.init(buf, sizeof(buf));
	failures += check(d.gcInfo(se, rb, &ver) && ver == (2u << 16), "gc_info: v2 table accepted");

	se = rb = 99;
	buildDiscovery(buf, 0, 0x4347, 1, 4, 4);
	d.init(buf, sizeof(buf));
	failures += check(!d.gcInfo(se, rb) && se == 99 && rb == 99, "gc_info: zero table offset refused");
	buildDiscovery(buf, 0xc0, 0x4347, 3, 4, 4);
	d.init(buf, sizeof(buf));
	failures += check(!d.gcInfo(se, rb) && se == 99, "gc_info: version 3 refused (amdgpu: Unhandled GC info table)");
	buildDiscovery(buf, 0xc0, 0x4348, 1, 4, 4);
	d.init(buf, sizeof(buf));
	failures += check(!d.gcInfo(se, rb) && se == 99, "gc_info: wrong table_id refused (GC_TABLE_ID 0x4347)");
	buildDiscovery(buf, 512 - 16, 0x4347, 1, 4, 4);   // gc_num_rb_per_se falls past the buffer
	d.init(buf, sizeof(buf));
	failures += check(!d.gcInfo(se, rb) && se == 99, "gc_info: table truncated by the buffer refused");
	IpDiscovery none;
	failures += check(!none.gcInfo(se, rb), "gc_info: no discovery refused");

	printf("\ngc_info: %s\n", failures ? "FAILED" : "ok");
	return failures;
}

struct VmTestTable {
	uint64_t base;
	uint64_t entries[4096];
};

static bool readVmTestEntry(void *ctx, uint64_t address, uint64_t &entry) {
	VmTestTable *t = static_cast<VmTestTable *>(ctx);
	if (address < t->base || address >= t->base + sizeof(t->entries) || (address & 7))
		return false;
	entry = t->entries[(address - t->base) / 8];
	return true;
}

static int testGpuVm() {
	int failures = 0;
	const uint64_t physical = 0x0000123400000000ull;
	const uint64_t pte = GpuVm::encodePte(physical,
		GpuVm::kValid | GpuVm::kReadable | GpuVm::kWritable | GpuVm::kIsPte,
		true);
	failures += check((pte & GpuVm::kPhysicalMask) == physical && (pte & GpuVm::kValid) &&
	                  ((pte >> 7) & 0x1f) == GpuVm::kFragment64K,
	                  "gfx12 PTE encodes physical address, valid/write and 64 KiB fragment");
	const uint64_t pde2 = GpuVm::encodePde(0x0000000000400000ull, GpuVm::kValid | GpuVm::kSnooped, 2);
	const uint64_t pde1 = GpuVm::encodePde(0x0000000000410000ull, GpuVm::kValid | GpuVm::kSnooped, 1);
	const uint64_t pde0 = GpuVm::encodePde(0x0000000000420000ull, GpuVm::kValid | GpuVm::kSnooped, 0);
	failures += check(pde2 == (0x0000000000400000ull | GpuVm::kValid) &&
	                  pde1 == (0x0000000000410000ull | GpuVm::kValid) &&
	                  pde0 == (0x0000000000420000ull | GpuVm::kValid),
	                  "gfx12 regular PDEs encode GPU physical address and VALID only");
	const uint64_t bus = 0x00000012345000ull;
	const uint64_t systemPte = GpuVm::encodePte(
		bus, GpuVm::kSystem | GpuVm::kSnooped | GpuVm::kValid |
		GpuVm::kReadable | GpuVm::kWritable, false);
	failures += check(systemPte == ((bus & GpuVm::kPhysicalMask) |
		                              GpuVm::kSystem | GpuVm::kSnooped |
		                              GpuVm::kValid | GpuVm::kReadable | GpuVm::kWritable) &&
	                  !(systemPte & GpuVm::kFragMask) && !(systemPte & GpuVm::kMtypeMask),
	                  "gfx12 system PTE encodes bus address, SYSTEM/SNOOPED and cached MTYPE_NC");
	uint64_t converted = 0;
	failures += check(GpuVm::mcToPhysical(0x0000008012345000ull, 0x0000008000000000ull,
	                                      0x12, converted) &&
	                  converted == 0x0000000024345000ull,
	                  "gfx12 MC VRAM address converts to FB_OFFSET GPU physical address");
	failures += check(!GpuVm::mcToPhysical(0x0000007ffff00000ull, 0x0000008000000000ull,
	                                       0x12, converted),
	                  "gfx12 MC conversion rejects an address below the VRAM aperture");

	VmTestTable table { 0x0000000000400000ull, {} };
	/* One compact table image, laid out at 4 KiB boundaries. */
	const uint64_t root = table.base;
	const uint64_t pdb1 = root + 0x1000, pdb0 = root + 0x2000, ptb = root + 0x3000;
	auto put = [&table](uint64_t address, uint64_t value) {
		table.entries[(address - table.base) / 8] = value;
	};
	const uint64_t va = GpuVm::kVaStart + 0x12345000ull;
	put(root + GpuVm::index(va, 0) * 8, GpuVm::encodePde(pdb1, GpuVm::kValid | GpuVm::kSnooped, 2));
	put(pdb1 + GpuVm::index(va, 1) * 8, GpuVm::encodePde(pdb0, GpuVm::kValid | GpuVm::kSnooped, 1));
	put(pdb0 + GpuVm::index(va, 2) * 8, GpuVm::encodePde(ptb, GpuVm::kValid | GpuVm::kSnooped, 0));
	put(ptb + GpuVm::index(va, 3) * 8,
	    GpuVm::encodePte(physical, GpuVm::kValid | GpuVm::kReadable | GpuVm::kWritable | GpuVm::kIsPte,
	                      true));
	uint64_t got = 0, flags = 0;
	failures += check(GpuVm::walk(root, va + 0x345, readVmTestEntry, &table, got, flags) &&
	                  got == physical + 0x345 && (flags & GpuVm::kWritable),
	                  "gfx12 page-table walk returns the mapped physical address");
	failures += check(!GpuVm::walk(root, 0x2000, readVmTestEntry, &table, got, flags),
	                  "gfx12 page-table walk rejects an unmapped VA");
	/* Negative control: the round 2-4 leaf (no bit 63) is a directory entry to GFX12 and faults. */
	put(ptb + GpuVm::index(va, 3) * 8,
	    GpuVm::encodePte(physical, GpuVm::kValid | GpuVm::kSnooped | GpuVm::kReadable | GpuVm::kWritable,
	                      true));
	failures += check(!GpuVm::walk(root, va + 0x345, readVmTestEntry, &table, got, flags),
	                  "gfx12 page-table walk faults on a leaf PTE without IS_PTE (bit 63)");
	return failures;
}

// --- W13: the VMID pool (src/vmid.cpp) -----------------------------------------

namespace {
struct FakeFences {
	uint32_t now[Vmid::kMaxDomains] {};
};
bool fakeReached(void *context, uint32_t domain, uint32_t seq) {
	// Wrap-aware "now >= seq", as the kext's fenceReached does.
	return static_cast<int32_t>(static_cast<FakeFences *>(context)->now[domain] - seq) >= 0;
}
} // namespace

static int testVmidPool() {
	using namespace Vmid;
	int f = 0;
	FakeFences fences;
	Pool pool;
	Grant g;

	// Reuse: the same owner and page directory keep their VMID; no rebind, no flush.
	pool.init(fakeReached, &fences);
	f += check(pool.grab(1, 0x1000, 0, g) == Result::Ok && g.rebind && g.flush && !g.stolen &&
	           g.vmid >= kFirst && g.vmid <= kLast, "vmid: first grab binds a fresh VMID");
	const uint32_t v1 = g.vmid;
	f += check(pool.grab(1, 0x1000, 0, g) == Result::Ok && g.vmid == v1 && !g.rebind && !g.flush,
	           "vmid: same owner and PD reuse the VMID with no rebind and no flush");

	// A flush is owed only when the client removed PTEs (its tlbSeq moved) since the last flush.
	f += check(pool.grab(1, 0x1000, 3, g) == Result::Ok && g.vmid == v1 && !g.rebind && g.flush,
	           "vmid: tlbSeq advanced -> flush, still no rebind");
	f += check(pool.grab(1, 0x1000, 3, g) == Result::Ok && !g.flush, "vmid: the flush is owed once");
	// Negative control: a pool that ignored tlbSeq would fail the two checks above.

	// A flush does not need an idle VMID (an invalidation only drops cached translations).
	pool.noteSubmit(v1, 0, 10);
	f += check(pool.grab(1, 0x1000, 4, g) == Result::Ok && g.vmid == v1 && g.flush && !g.rebind,
	           "vmid: a busy VMID may be flushed");
	fences.now[0] = 10;

	// A changed page directory is not compatible: the client gets a rebind and the idle old slot is released.
	f += check(pool.grab(1, 0x2000, 0, g) == Result::Ok && g.rebind && g.flush, "vmid: new PD -> rebind");
	uint32_t owned = 0;
	for (uint32_t v = kFirst; v <= kLast; v++)
		owned += pool.ownerOf(v) == 1;
	f += check(owned == 1, "vmid: the stale slot of the same owner is released (owns %u)", owned);

	// Fifteen owners get fifteen distinct VMIDs.
	pool.init(fakeReached, &fences);
	fences = FakeFences {};
	bool seen[kSlots] {};
	bool distinct = true;
	for (uintptr_t o = 1; o <= 15; o++) {
		if (pool.grab(o, o * 0x1000, 0, g) != Result::Ok || seen[g.vmid] || g.stolen)
			distinct = false;
		seen[g.vmid] = true;
	}
	f += check(distinct && !seen[0], "vmid: 15 owners, 15 distinct VMIDs, VMID 0 never granted");

	// Steal: all idle, the 16th takes the LEAST recently used. Owner 1 is older than 2..15; touch 1
	// again and owner 2 becomes the victim.
	pool.grab(1, 0x1000, 0, g);
	const uint32_t v2 = [&] { for (uint32_t v = kFirst; v <= kLast; v++) if (pool.ownerOf(v) == 2) return v; return 0u; }();
	f += check(pool.grab(16, 0x16000, 0, g) == Result::Ok && g.rebind && g.stolen && g.vmid == v2,
	           "vmid: steal takes the least recently used idle VMID (got %u want %u)", g.vmid, v2);
	f += check(pool.ownerOf(v2) == 16, "vmid: the thief owns the stolen VMID");
	f += check(g.prevOwner == 2, "vmid: a steal reports the previous owner (a fault latched for it is attributed to it, not to the thief)");
	f += check(pool.grab(2, 0x2000, 0, g) == Result::Ok && g.rebind && g.vmid != v2,
	           "vmid: the victim comes back through a rebind on another VMID");

	// Never steal a busy VMID; exhaustion returns the fence to wait on.
	pool.init(fakeReached, &fences);
	fences = FakeFences {};
	for (uintptr_t o = 1; o <= 15; o++) {
		pool.grab(o, o * 0x1000, 0, g);
		pool.noteSubmit(g.vmid, 0, static_cast<uint32_t>(o));      // owner o: fence seq o on domain 0
	}
	f += check(pool.grab(16, 0x16000, 0, g) == Result::Busy && g.waitDomain == 0 && g.waitSeq == 1,
	           "vmid: all busy -> Busy on the oldest job's fence (seq %u)", g.waitSeq);
	bool untouched = true;
	for (uintptr_t o = 1; o <= 15; o++) {
		bool has = false;
		for (uint32_t v = kFirst; v <= kLast; v++)
			has |= pool.ownerOf(v) == o;
		untouched &= has;
	}
	f += check(untouched, "vmid: a Busy grab takes nothing from anyone");

	// Fairness: while that fence is pending a different newcomer waits too, but an owner that still
	// has its VMID is served (the deliberate deviation from amdgpu).
	f += check(pool.grab(17, 0x17000, 0, g) == Result::Busy && g.waitSeq == 1, "vmid: a second newcomer waits for the same fence");
	f += check(pool.grab(5, 5 * 0x1000, 0, g) == Result::Ok && !g.rebind, "vmid: a bound owner is not starved by the wait");

	// Fence 5 reached: owners 1..5 are idle; the thief takes the LRU among them (owner 1) and only them.
	fences.now[0] = 5;
	f += check(pool.grab(16, 0x16000, 0, g) == Result::Ok && g.stolen, "vmid: fence reached -> the waiter gets a VMID");
	f += check(pool.ownerOf(g.vmid) == 16, "vmid: the waiter owns it");
	bool busyKept = true;
	for (uintptr_t o = 6; o <= 15; o++) {
		bool has = false;
		for (uint32_t v = kFirst; v <= kLast; v++)
			has |= pool.ownerOf(v) == o;
		busyKept &= has;
	}
	f += check(busyKept, "vmid: owners with jobs in flight (seq 6..15) keep their VMIDs");
	f += check(pool.ownerOf(g.vmid) == 16 && pool.grab(1, 0x1000, 0, g) == Result::Ok, "vmid: the victim is idle owner 1 (it rebinds)");

	// Fairness across domains: the waiter is on domain 0; a VMID that frees up on domain 1 meanwhile
	// must NOT go to a newcomer who arrived later (amdgpu's vmid_wait).
	pool.init(fakeReached, &fences);
	fences = FakeFences {};
	for (uintptr_t o = 1; o <= 14; o++) {
		pool.grab(o, o * 0x1000, 0, g);
		pool.noteSubmit(g.vmid, 0, static_cast<uint32_t>(o));
	}
	pool.grab(15, 15 * 0x1000, 0, g);
	const uint32_t v15 = g.vmid;
	pool.noteSubmit(v15, 1, 1);
	f += check(pool.grab(16, 0x16000, 0, g) == Result::Busy && g.waitDomain == 0, "vmid: waiter parks on domain 0");
	fences.now[1] = 1;                       // owner 15's job finished: a VMID is idle now
	f += check(pool.grab(17, 0x17000, 0, g) == Result::Busy && g.waitDomain == 0,
	           "vmid: an idle VMID is not handed to a newcomer while an earlier waiter is pending");
	fences.now[0] = 1;                       // the waiter's fence
	f += check(pool.grab(16, 0x16000, 0, g) == Result::Ok, "vmid: the waiter is served once its fence is reached");

	// Several fence domains and sequence wrap.
	pool.init(fakeReached, &fences);
	fences = FakeFences {};
	pool.grab(1, 0x1000, 0, g);
	const uint32_t vm = g.vmid;
	pool.noteSubmit(vm, 0, 0xfffffffeu);
	pool.noteSubmit(vm, 1, 7);
	fences.now[0] = 1;                       // wrapped past 0xfffffffe
	f += check(!pool.idle(vm), "vmid: busy while domain 1 has not reached its seq");
	fences.now[1] = 7;
	f += check(pool.idle(vm), "vmid: idle once every domain reached its seq, wrap included");

	// Exhaustion by pinning (static VMIDs, step S7).
	pool.init(fakeReached, &fences);
	fences = FakeFences {};
	bool pinsOk = true;
	for (uintptr_t o = 1; o <= 15; o++)
		pinsOk &= pool.pin(o, o * 0x1000, 0, g) == Result::Ok && pool.pinned(g.vmid);
	f += check(pinsOk, "vmid: 15 pins succeed");
	f += check(pool.pin(16, 0x16000, 0, g) == Result::Exhausted, "vmid: the 16th pin is Exhausted");
	f += check(pool.grab(16, 0x16000, 0, g) == Result::Exhausted, "vmid: nothing can be stolen from pinned VMIDs");
	f += check(pool.grab(3, 3 * 0x1000, 0, g) == Result::Ok && !g.rebind, "vmid: a pinned owner still reuses its VMID");
	uint32_t released = 0;
	f += check(pool.forget(3, false, released) && released != 0 && pool.ownerOf(released) == 0,
	           "vmid: forget releases a pinned VMID and reports it");
	f += check(pool.grab(16, 0x16000, 0, g) == Result::Ok && g.vmid == released, "vmid: the released VMID is granted again");

	// forget: refused while work is in flight, forced after a recovery.
	pool.init(fakeReached, &fences);
	fences = FakeFences {};
	pool.grab(1, 0x1000, 0, g);
	pool.noteSubmit(g.vmid, 0, 5);
	f += check(!pool.forget(1, false, released) && pool.ownerOf(g.vmid) == 1, "vmid: forget is refused with a job in flight");
	f += check(pool.forget(1, true, released) && released == g.vmid && pool.ownerOf(g.vmid) == 0 && pool.idle(g.vmid),
	           "vmid: a forced forget releases it and clears the pending work");

	// Reserved VMIDs are never granted.
	pool.init(fakeReached, &fences, 1u << 8);
	fences = FakeFences {};
	bool reservedHit = false;
	for (uintptr_t o = 1; o <= 14; o++)
		reservedHit |= pool.grab(o, o * 0x1000, 0, g) != Result::Ok || g.vmid == 8;
	f += check(!reservedHit, "vmid: 14 grants with VMID 8 reserved never return it");
	f += check(pool.grab(15, 0x15000, 0, g) == Result::Ok && g.stolen && g.vmid != 8,
	           "vmid: the 15th owner steals an idle VMID; the reserved one stays out");

	// unbindAll (wake / GPU reset): nothing is bound any more, pins included.
	pool.unbindAll();
	bool none = true;
	for (uint32_t v = kFirst; v <= kLast; v++)
		none &= pool.ownerOf(v) == 0 && !pool.pinned(v);
	f += check(none && pool.grab(1, 0x1000, 0, g) == Result::Ok && g.rebind, "vmid: after unbindAll the next grab rebinds");

	f += check(pool.grab(0, 0x1000, 0, g) == Result::Exhausted, "vmid: owner 0 is invalid");
	return f;
}

// --- W13 S6: demand-allocated page tables (src/ptpages.cpp) ---------------------------------

namespace {
struct FakeChunks {
	uint64_t next { 0x1000000 };
	uint32_t live { 0 }, allocs { 0 }, frees { 0 };
	uint32_t failAfter { 0xffffffffu };     // refuse the allocation after this many
	uint64_t freed[256];
};
bool fakeAllocChunk(void *ctx, uint64_t &off) {
	auto *f = static_cast<FakeChunks *>(ctx);
	if (f->allocs >= f->failAfter)
		return false;
	off = f->next;
	f->next += 0x10000;
	f->allocs++;
	f->live++;
	return true;
}
void fakeFreeChunk(void *ctx, uint64_t off) {
	auto *f = static_cast<FakeChunks *>(ctx);
	if (f->frees < 256)
		f->freed[f->frees] = off;
	f->frees++;
	f->live--;
}
} // namespace

static int testPtPages() {
	int f = 0;
	FakeChunks chunks;
	PtPages::Backend be { fakeAllocChunk, fakeFreeChunk, &chunks };
	static PtPages::Table t;
	t.init(PtPages::kMaxPages);
	uint64_t off = 0;

	// A client's first pages (root, PDB1, PDB0, one PT) live in ONE 64 KiB chunk, 4 KiB apart.
	bool ok = true;
	uint64_t first = 0;
	for (uint32_t i = 0; i < 4; i++) {
		ok &= t.page(i, be, off);
		if (i == 0)
			first = off;
		ok &= off == first + 0x1000ull * i;
	}
	f += check(ok && t.pages() == 4 && t.chunksHeld() == 1 && chunks.allocs == 1, "ptpages: four pages share one chunk, packed 4 KiB apart");
	f += check(t.page(2, be, off) && off == first + 0x2000 && t.pages() == 4, "ptpages: asking again returns the same page and allocates nothing");
	f += check(t.offsetOf(3) == first + 0x3000 && t.offsetOf(9) == 0 && !t.has(9), "ptpages: offsetOf / has for backed and unbacked pages");

	// Sparse: a far PT page costs one page, not the distance.
	ok = t.page(900, be, off) && off == first + 0x4000 && t.pages() == 5 && t.chunksHeld() == 1;
	f += check(ok, "ptpages: a far-away logical page takes the next free slot of the chunk (no 4 MiB image)");

	// The 17th page opens a second chunk.
	for (uint32_t i = 10; i < 21; i++)
		t.page(i, be, off);
	f += check(t.pages() == 16 && t.chunksHeld() == 1, "ptpages: 16 pages fill the first chunk exactly");
	f += check(t.page(21, be, off) && t.chunksHeld() == 2 && chunks.allocs == 2, "ptpages: the 17th page opens a second chunk");

	// Dropping: a slot is reused; an emptied chunk goes back to the allocator.
	t.drop(21, be);
	f += check(t.chunksHeld() == 1 && chunks.frees == 1, "ptpages: dropping the only page of a chunk frees that chunk");
	t.drop(3, be);
	f += check(t.page(700, be, off) && off == first + 0x3000, "ptpages: a dropped slot is reused first");

	// Quota and allocator failure leave no half-made page.
	PtPages::Table q;
	FakeChunks c2;
	PtPages::Backend b2 { fakeAllocChunk, fakeFreeChunk, &c2 };
	q.init(3);
	bool three = q.page(0, b2, off) && q.page(1, b2, off) && q.page(2, b2, off);
	f += check(three && !q.page(3, b2, off) && q.pages() == 3 && !q.has(3), "ptpages: the quota refuses the 4th page and leaves no trace");
	PtPages::Table r;
	FakeChunks c3;
	c3.failAfter = 1;
	PtPages::Backend b3 { fakeAllocChunk, fakeFreeChunk, &c3 };
	r.init(PtPages::kMaxPages);
	bool filled = true;
	for (uint32_t i = 0; i < 16; i++)
		filled &= r.page(i, b3, off);
	f += check(filled && !r.page(16, b3, off) && r.pages() == 16 && !r.has(16) && r.chunksHeld() == 1,
	           "ptpages: an allocator that refuses the second chunk fails the page cleanly");

	// Release gives every chunk back exactly once.
	t.release(be);
	f += check(t.pages() == 0 && t.chunksHeld() == 0 && chunks.live == 0, "ptpages: release returns every chunk (none left live)");
	bool unique = true;
	for (uint32_t i = 0; i < chunks.frees && i < 256; i++)
		for (uint32_t j = i + 1; j < chunks.frees && j < 256; j++)
			unique &= chunks.freed[i] != chunks.freed[j];
	f += check(unique, "ptpages: no chunk was freed twice");

	// Worst case: a client holding its whole quota, kMaxPages pages = kMaxPages / 16 chunks.
	PtPages::Table w;
	FakeChunks c4;
	PtPages::Backend b4 { fakeAllocChunk, fakeFreeChunk, &c4 };
	w.init(PtPages::kMaxPages);
	bool all = true;
	for (uint32_t i = 0; i < PtPages::kMaxPages; i++)
		all &= w.page(i, b4, off);
	f += check(all && w.pages() == PtPages::kMaxPages && w.chunksHeld() == PtPages::kMaxPages / PtPages::kChunkPages,
	           "ptpages: a client holding its whole quota takes kMaxPages / 16 chunks");
	w.release(b4);
	f += check(c4.live == 0, "ptpages: and gives them all back");

	// Pool-slot layout: the old layout (26 MiB, 64 KiB stride) runs into the gfx region at 30 MiB with slot 64.
	uint32_t area = 0;
	const uint32_t base = 26u << 20, gfx = 30u << 20;
	f += check(PtPages::areaFor(base, 0x10000, 0, gfx, area) && area == base, "layout: slot 0 at the base");
	f += check(PtPages::areaFor(base, 0x10000, 63, gfx, area) && area + 0x10000 == gfx, "layout: slot 63 ends exactly at the gfx region");
	f += check(!PtPages::areaFor(base, 0x10000, 64, gfx, area), "layout: slot 64 (which the old code would have placed on the gfx ring) is refused");
	f += check(PtPages::areaFor(27u << 20, 0x3000, 255, gfx, area) && !PtPages::areaFor(27u << 20, 0x3000, 256, gfx, area),
	           "layout: the compact 12 KiB client area fits 256 clients below the gfx region, 257 is refused");
	f += check(PtPages::areaFor(0, 10, 1, 20, area) && !PtPages::areaFor(0, 10, 1, 19, area), "layout: an area that overruns the limit by one byte is refused");
	f += check(!PtPages::areaFor(base, 0, 0, gfx, area) && !PtPages::areaFor(0xfffff000u, 0x10000, 1, 0xffffffffu, area),
	           "layout: a zero stride and a 32-bit overflow are refused");
	return f;
}

// --- hub-task-340: differential test of the page-table writing ---------------------------------------------------------------
// The pre-S8 code (tools/legacy-vmtable-ref.inc, verbatim from commit 0cfa416) against src/gpuvmtable.cpp, which the kext now runs for vmMap, vmMapHost
// and vmUnmap, on the contiguous 4 MiB image of modes 0 and 1: return values, the PT pages handed to vmTableSync, and the whole image byte for byte
// (PDEs, PTE flags: IS_PTE bit 63, EXECUTABLE, SNOOPED/SYSTEM, FRAG, the W22 set/clear masks). A third image is built with the sparse accessor of
// rdna4-vmshared=2 and compared logically (every PTE equal, PDE validity equal).

#include "legacy-vmtable-ref.inc"

namespace {

constexpr uint64_t kTableBytes = 4u << 20;
constexpr uint64_t kRootPhys = 0x10000000ull;
constexpr uint64_t kFbMcBase = 0x8000000000ull;

struct PolicySet { bool isPteOff, execOff; uint64_t pteSet, pteClear; const char *name; };
const PolicySet kPolicies[] = {
	{ false, false, 0, 0, "default" },
	{ false, true, 0, 0, "rdna4-vm-exec=0" },
	{ true, false, 0, 0, "rdna4-vm-ispte=0" },
	{ false, false, 1ull << 54, GpuVm::kExecutable, "set MTYPE bit, clear EXECUTABLE" },
	{ true, true, GpuVm::kSnooped, 0, "ispte=0, exec=0, SNOOPED set" },
};

// The new code behind an accessor, with the kernel wrappers' (runtime.cpp vmMap/vmMapHost/vmUnmap) argument checks and sync order mirrored.
struct NewImage {
	std::vector<uint64_t> words;              // contiguous image (legacy accessor)
	uint64_t rootPhys { kRootPhys };
	GpuVmTable::Policy pol {};
	uint64_t fbMcBase { kFbMcBase };
	uint32_t fbOffset { 0 };
	std::vector<std::pair<uint32_t, uint32_t>> syncs;
	bool failSync { false };
	bool sync(uint32_t offset, uint32_t bytes) { syncs.push_back({ offset, bytes }); return !failSync; }
};

uint64_t *newEntry(void *ctx, uint64_t off) {
	auto *n = static_cast<NewImage *>(ctx);
	return off < kTableBytes ? GpuVmTable::legacyEntry(n->words.data(), off) : nullptr;
}

bool newPhys(void *ctx, uint64_t off, uint64_t &phys) {
	phys = GpuVmTable::legacyPhys(static_cast<NewImage *>(ctx)->rootPhys, off);
	return true;
}

bool newVmMap(NewImage &n, uint64_t va, uint64_t mc, uint64_t bytes, bool executable) {
	if (!bytes || (va & (GpuVm::kPageBytes - 1)) || (mc & (GpuVm::kPageBytes - 1)))
		return false;
	uint64_t physical = 0;
	if (!GpuVm::mcToPhysical(mc, n.fbMcBase, n.fbOffset, physical))
		return false;
	const uint64_t end = va + ((bytes + GpuVm::kPageBytes - 1) & ~(GpuVm::kPageBytes - 1));
	if (end < va || end > GpuVm::kVaEnd)
		return false;
	const GpuVmTable::Access access { newEntry, newPhys, &n };
	GpuVmTable::Span span;
	if (!GpuVmTable::mapVram(access, n.pol, kTableBytes, va, end, physical, executable, span))
		return false;
	if (span.firstPt == ~0ull || !n.sync(0, 0x3000))
		return false;
	for (uint64_t pt = span.firstPt; pt <= span.lastPt; pt += 0x1000)
		if (!n.sync(static_cast<uint32_t>(pt), 0x1000))
			return false;
	return true;
}

bool newVmMapHost(NewImage &n, uint64_t va, const uint64_t *pageBuses, uint64_t bytes, bool executable) {
	if (!pageBuses || !bytes || (va & (GpuVm::kPageBytes - 1)))
		return false;
	const uint64_t mapped = (bytes + GpuVm::kPageBytes - 1) & ~(GpuVm::kPageBytes - 1);
	const uint64_t end = va + mapped;
	if (mapped < bytes || end < va || end > GpuVm::kVaEnd)
		return false;
	const GpuVmTable::Access access { newEntry, newPhys, &n };
	GpuVmTable::Span span;
	if (!GpuVmTable::mapHost(access, n.pol, kTableBytes, va, end, pageBuses, executable, span))
		return false;
	if (span.firstPt == ~0ull || !n.sync(0, 0x3000))
		return false;
	for (uint64_t pt = span.firstPt; pt <= span.lastPt; pt += 0x1000)
		if (!n.sync(static_cast<uint32_t>(pt), 0x1000))
			return false;
	return true;
}

void newVmUnmap(NewImage &n, uint64_t va, uint64_t bytes) {
	if (!bytes || va & (GpuVm::kPageBytes - 1))
		return;
	const uint64_t end = va + ((bytes + GpuVm::kPageBytes - 1) & ~(GpuVm::kPageBytes - 1));
	const GpuVmTable::Access access { newEntry, newPhys, &n };
	GpuVmTable::Span span;
	GpuVmTable::unmap(access, kTableBytes, va, end, span);
	for (uint64_t pt = span.firstPt; pt != ~0ull && pt <= span.lastPt; pt += 0x1000)
		n.sync(static_cast<uint32_t>(pt), 0x1000);
}

// rtOpen's initial image: root -> PDB1 -> PDB0.
void initLegacy(std::vector<uint64_t> &w, uint64_t rootPhys) {
	std::fill(w.begin(), w.end(), 0);
	w[0] = GpuVm::encodePde(rootPhys + 0x1000, GpuVm::kValid, 2);
	w[0x1000 / 8 + GpuVm::index(GpuVm::kVaStart, 1)] = GpuVm::encodePde(rootPhys + 0x2000, GpuVm::kValid, 1);
}

struct Pair {
	LegacyRef ref;
	NewImage nw;
	std::vector<uint64_t> refWords;
	int failures { 0 };
	const char *policy { "" };
	uint32_t ops { 0 }, okMaps { 0 }, failedMaps { 0 }, okHost { 0 }, failedHost { 0 }, unmaps { 0 };

	void setup(const PolicySet &p, uint32_t fbOffset) {
		refWords.assign(kTableBytes / 8, 0);
		ref.shadow = refWords.data();
		ref.rootPhys = kRootPhys;
		ref.tableBytes = kTableBytes;
		ref.pol = { p.isPteOff, p.execOff, p.pteSet, p.pteClear };
		ref.fbMcBase = kFbMcBase;
		ref.fbOffset = fbOffset;
		nw.pol = GpuVmTable::Policy { p.isPteOff, p.execOff, p.pteSet, p.pteClear };
		nw.fbMcBase = kFbMcBase;
		nw.fbOffset = fbOffset;
		nw.words.assign(kTableBytes / 8, 0);
		policy = p.name;
		reopen();
	}
	void reopen() {
		initLegacy(refWords, kRootPhys);
		nw.words = refWords;
		ref.syncs.clear();
		nw.syncs.clear();
	}
	void setFailSync(bool f) { ref.failSync = nw.failSync = f; }

	// After one operation: same result, same syncs, same bytes.
	void compare(const char *what, bool rr, bool rn) {
		ops++;
		auto fail = [&](const char *why) {
			if (failures < 5)
				fprintf(stderr, "FAIL: vmtable diff [%s] op %u %s: %s\n", policy, ops, what, why);
			failures++;
		};
		if (rr != rn)
			fail("return value differs from the pre-S8 code");
		if (ref.syncs != nw.syncs)
			fail("the PT pages handed to vmTableSync differ");
		if (memcmp(refWords.data(), nw.words.data(), kTableBytes) != 0) {
			uint64_t i = 0;
			while (refWords[i] == nw.words[i])
				i++;
			char buf[160];
			snprintf(buf, sizeof(buf), "image differs at byte 0x%llx: old 0x%016llx new 0x%016llx", (unsigned long long)(i * 8),
			         (unsigned long long)refWords[i], (unsigned long long)nw.words[i]);
			fail(buf);
		}
		ref.syncs.clear();
		nw.syncs.clear();
	}

	void map(uint64_t va, uint64_t mc, uint64_t bytes, bool exec) {
		const bool a = legacyVmMap(ref, va, mc, bytes, exec), b = newVmMap(nw, va, mc, bytes, exec);
		(a ? okMaps : failedMaps)++;
		compare("map", a, b);
	}
	void mapHost(uint64_t va, const std::vector<uint64_t> &buses, uint64_t bytes, bool exec) {
		const bool a = legacyVmMapHost(ref, va, buses.data(), bytes, exec), b = newVmMapHost(nw, va, buses.data(), bytes, exec);
		(a ? okHost : failedHost)++;
		compare("mapHost", a, b);
	}
	void unmap(uint64_t va, uint64_t bytes) {
		legacyVmUnmap(ref, va, bytes);
		newVmUnmap(nw, va, bytes);
		unmaps++;
		compare("unmap", true, true);
	}
};

struct Rng {
	uint64_t s;
	uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
	uint64_t below(uint64_t n) { return next() % n; }
};

} // namespace

static int testVmTableDifferential() {
	int failures = 0;
	uint32_t totOps = 0, totOk = 0, totBad = 0, totHostOk = 0, totHostBad = 0, totUnmap = 0;
	const uint64_t V = GpuVm::kVaStart;
	const uint64_t MC = kFbMcBase;
	for (const PolicySet &pol : kPolicies) {
		for (uint32_t fbOffset : { 0u, 0x10u }) {
			static Pair pair;
			pair = Pair {};
			pair.setup(pol, fbOffset);
			Pair &P = pair;

			// 1. The boot self-test / rtOpen layout: ring (executable), EOP, rptr, wptr, fence, kernarg, then user buffers.
			P.map(V + 0x0000, MC + 0x1a000000, 0x1000, true);
			for (uint32_t i = 1; i < 6; i++)
				P.map(V + 0x1000ull * i, MC + 0x1a000000 + 0x1000ull * i, 0x1000, false);
			// a program image (12 KiB, executable), a 256 KiB device buffer on a 64 KiB boundary (FRAG=4), one that is not aligned to it
			P.map(V + 0x10000, MC + 0x20000000, 0x3000, true);
			P.map(V + 0x40000, MC + 0x30000000, 0x40000, false);
			P.map(V + 0x80000 + 0x1000, MC + 0x31000000 + 0x1000, 0x40000, false);
			// 2. host buffers: 64 KiB of 16 bus pages mapped, unmapped, then mapped again elsewhere (rdna4-run's host vadd and freed-VA sequence)
			std::vector<uint64_t> buses(16);
			for (uint32_t i = 0; i < 16; i++)
				buses[i] = 0x104048000ull + 0x3000ull * i;
			P.mapHost(V + 0xb0000, buses, 0x10000, false);
			P.unmap(V + 0xb0000, 0x10000);                       // the freed buffer; the next dispatch through it must fault
			P.mapHost(V + 0xd0000, buses, 0x10000, true);
			P.unmap(V + 0x10000, 0x3000);                        // the program is unloaded
			P.map(V + 0x10000, MC + 0x21000000, 0x3000, true);   // and its VA reused
			// 3. across a 2 MiB boundary (two PT pages) and a large device buffer
			P.map(V + 0x1ff000, MC + 0x40000000, 0x3000, false);
			P.map(V + 0x400000, MC + 0x50000000, 0x600000, false);
			// 3b. the legacy layout's first-writer-wins quirk: PT pages 88 and 88 + 512 (1.2 GiB up) share PDB0[88]; whichever is mapped first owns the PDE
			P.map(V + 600ull * 0x200000, MC + 0x60000000, 0x1000, false);
			P.map(V + 88ull * 0x200000, MC + 0x61000000, 0x1000, false);
			// 4. client close and reopen on the same tables
			P.reopen();
			P.map(V + 0x0000, MC + 0x1a000000, 0x1000, true);
			P.map(V + 0x7000, MC + 0x1a007000, 0x2000, false);
			P.unmap(V + 0x7000, 0x2000);
			// 5. argument failures, and a vmTableSync failure
			P.map(V + 0x1001, MC, 0x1000, false);                // unaligned va
			P.map(V, MC + 0x1001, 0x1000, false);                // unaligned mc
			P.map(V, MC - 0x1000, 0x1000, false);                // mc below the FB base
			P.map(V, MC, 0, false);                              // zero bytes
			P.map(V - 0x1000, MC, 0x2000, false);                // starts below kVaStart
			P.map(V + 2046ull * 0x100000 + 0x10000, MC, 0x400000, false);   // runs past the end of the 4 MiB image
			P.map(~0ull - 0xfff, MC, 0x2000, false);             // overflow
			P.setFailSync(true);
			P.map(V + 0x300000, MC + 0x1000000, 0x2000, false);
			P.setFailSync(false);

			// 6. fuzz: random histories, seeded
			Rng r { 0x9e3779b97f4a7c15ull ^ (static_cast<uint64_t>(fbOffset) << 7) ^ reinterpret_cast<uintptr_t>(pol.name) };
			P.reopen();
			for (uint32_t op = 0; op < 160; op++) {
				const uint64_t kind = r.below(100);
				uint64_t va = V + r.below(0x40000000 / 0x1000) * 0x1000;       // anywhere in the first GiB
				if (r.below(25) == 0)
					va = V + r.below(0x1000) * 0x1000 + 0x1ff000;            // straddle the first 2 MiB boundary
				if (r.below(40) == 0)
					va += 0x123;                                              // unaligned
				if (r.below(60) == 0)
					va = V - 0x1000 * (1 + r.below(4));                        // below the start
				uint64_t pages = 1 + r.below(16);
				if (r.below(4) == 0)
					pages = 16 + r.below(600);
				if (r.below(25) == 0)
					pages = 1024 + r.below(512);
				const uint64_t bytes = pages * 0x1000 - (r.below(8) == 0 ? r.below(0x1000) : 0);
				if (kind < 40) {
					uint64_t mc = MC + (r.below(0x100000) << 12);
					if (r.below(30) == 0)
						mc = MC - 0x1000 * (1 + r.below(8));                  // below the FB base
					if (r.below(50) == 0)
						mc += 0x100;                                          // unaligned
					P.map(va, mc, bytes, r.below(2) != 0);
				} else if (kind < 60) {
					std::vector<uint64_t> bus(pages + 1);
					for (uint64_t i = 0; i < bus.size(); i++)
						bus[i] = (r.below(1ull << 28) << 12);
					if (r.below(40) == 0)
						bus[r.below(pages)] |= 0x40;                          // a misaligned bus address
					P.mapHost(va, bus, bytes, r.below(2) != 0);
				} else if (kind < 92) {
					P.unmap(va, bytes);
				} else if (kind < 97) {
					P.reopen();
				} else {
					P.setFailSync(!P.ref.failSync);
				}
			}
			failures += P.failures;
			totOps += P.ops; totOk += P.okMaps; totBad += P.failedMaps; totHostOk += P.okHost; totHostBad += P.failedHost; totUnmap += P.unmaps;
		}
	}
	printf("vmtable differential: %u operations (%u maps ok, %u refused, %u host maps ok, %u refused, %u unmaps), "
	       "pre-S8 code vs src/gpuvmtable.cpp (contiguous image of modes 0/1): identical\n", totOps, totOk, totBad, totHostOk, totHostBad, totUnmap);
	return failures + check(totOk > 200 && totBad > 50 && totHostOk > 100 && totHostBad > 10 && totUnmap > 100,
	                        "vmtable differential: the histories cover successful and refused maps, host maps and unmaps");
}

// ---- the multi-level layout of rdna4-vmshared=2 (hub-task-346): VA beyond 1 GiB ----
namespace {
// A fake device for PtPages::Sparse: chunks from a counter, shadow pages from the host, and a "VRAM" that only ever holds what sync() copied, so a walk over
// it proves the dirty tracking too (a page that was changed but not marked never reaches it).
struct TreeEnv {
	PtPages::Sparse sp;
	uint64_t nextChunk { 0x2000000 };
	uint32_t chunksLive { 0 }, shadowLive { 0 }, shadowLimit { 0xffffffffu };
	std::map<uint64_t, std::vector<uint64_t>> vram;           // heap offset -> synced page
	std::vector<uint64_t> dirtyLog;
	uint64_t rootPhys { 0 };
	bool dropDirty { false };                                   // mutation switch: the tree forgets to mark pages
	PtPages::Host host;
	static constexpr uint64_t kBase = 0x4000000000ull;

	static bool allocChunk(void *c, uint64_t &off) { auto *e = static_cast<TreeEnv *>(c); off = e->nextChunk; e->nextChunk += 0x10000; e->chunksLive++; return true; }
	static void freeChunk(void *c, uint64_t) { static_cast<TreeEnv *>(c)->chunksLive--; }
	static uint64_t *allocShadow(void *c) {
		auto *e = static_cast<TreeEnv *>(c);
		if (e->shadowLive >= e->shadowLimit)
			return nullptr;
		e->shadowLive++;
		return static_cast<uint64_t *>(calloc(512, 8));
	}
	static void freeShadow(void *c, uint64_t *p) { static_cast<TreeEnv *>(c)->shadowLive--; free(p); }
	static bool physOf(void *, uint64_t heapOffset, uint64_t &phys) { phys = kBase + heapOffset; return true; }
	static bool treePage(void *c, uint32_t level, uint64_t key, bool create, uint64_t *&entries, uint64_t &phys, uint32_t &id) {
		auto *e = static_cast<TreeEnv *>(c);
		PtPages::Page p;
		if (!e->sp.get(e->host, level, key, create, p))
			return false;
		entries = p.entries; phys = p.phys; id = p.id;
		return true;
	}
	static void treeDirty(void *c, uint32_t id) {
		auto *e = static_cast<TreeEnv *>(c);
		e->dirtyLog.push_back(id);
		if (!e->dropDirty)
			e->sp.markDirty(id);
	}
	GpuVmTable::Tree tree() { return GpuVmTable::Tree { treePage, treeDirty, this }; }

	void open(uint32_t quota) {
		host = PtPages::Host { PtPages::Backend { allocChunk, freeChunk, this }, allocShadow, freeShadow, physOf, this };
		sp.init(quota);
		PtPages::Page root;
		sp.get(host, 0, 0, true, root);
		rootPhys = root.phys;
		sync();
	}
	void sync() {
		uint32_t id; uint64_t *shadow, off;
		while (sp.nextDirty(id, shadow, off)) {
			vram[off] = std::vector<uint64_t>(shadow, shadow + 512);
			sp.clean(id);
		}
	}
	static bool readEntry(void *c, uint64_t address, uint64_t &entry) {
		auto *e = static_cast<TreeEnv *>(c);
		auto it = e->vram.find((address & ~0xfffull) - kBase);
		if (it == e->vram.end())
			return false;                                       // the walker reads a VRAM page that never got written: a fault
		entry = it->second[(address & 0xfff) / 8];
		return true;
	}
	// The GPU's view: walk the SYNCED pages from the root.
	bool translate(uint64_t va, uint64_t &physical) {
		uint64_t flags = 0;
		return GpuVm::walk(rootPhys, va, readEntry, this, physical, flags);
	}
	void close() {
		sp.releaseAll(host, true);
		vram.clear();
		dirtyLog.clear();
	}
};

bool treeMap(TreeEnv &e, uint64_t va, uint64_t mc, uint64_t bytes) {
	const GpuVmTable::Policy pol {};
	uint64_t physical = 0;
	GpuVm::mcToPhysical(mc, kFbMcBase, 0, physical);
	const uint64_t end = va + ((bytes + 0xfff) & ~0xfffull);
	const bool ok = GpuVmTable::treeMapVram(e.tree(), pol, va, end, physical, false);
	e.sync();
	return ok;
}

// Every page of [va, va + bytes) translates to physical + offset (and nothing else does outside).
bool treeResolves(TreeEnv &e, uint64_t va, uint64_t mc, uint64_t bytes) {
	uint64_t physical0 = 0;
	GpuVm::mcToPhysical(mc, kFbMcBase, 0, physical0);
	for (uint64_t off = 0; off < bytes; off += 0x1000) {
		uint64_t phys = 0;
		if (!e.translate(va + off, phys) || phys != physical0 + off)
			return false;
	}
	return true;
}
} // namespace

static int testSparseTree() {
	int f = 0;
	const uint64_t V = GpuVm::kVaStart, G = 1ull << 30;
	const uint64_t MC = kFbMcBase + 0x1000000;
	static TreeEnv e;
	e = TreeEnv {};
	e.open(PtPages::kMaxPages);
	f += check(e.sp.pages() == 1 && e.sp.chunksHeld() == 1 && e.shadowLive == 1, "tree: a fresh client is one page (the root), nothing below it");
	uint64_t phys = 0;
	f += check(!e.translate(V, phys), "tree: before any mapping every VA faults");

	// A map inside the first GiB: root + PDB1 + PDB0 + one PT.
	f += check(treeMap(e, V + 0x1000, MC, 0x3000) && treeResolves(e, V + 0x1000, MC, 0x3000) && e.sp.pages() == 4,
	           "tree: a first mapping backs root, PDB1, PDB0 and one PT page (4 pages)");
	f += check(!e.translate(V, phys) && !e.translate(V + 0x4000, phys), "tree: the neighbouring unmapped pages still fault");
	// A second map into the SAME, already synced PT page: only the dirty mark gets it to VRAM (a new page is dirty from birth, an old one is not).
	f += check(treeMap(e, V + 0x8000, MC + 0x50000, 0x2000) && treeResolves(e, V + 0x8000, MC + 0x50000, 0x2000) && e.sp.pages() == 4,
	           "tree: a later map into an existing PT page reaches VRAM and backs no new page");

	// THE FIX: a map that crosses 1 GiB. It needs a second PDB0 page (PDB1 entry 5 of the same PDB1 page).
	const uint64_t cross = V + G - 0x10000, crossMc = MC + 0x100000;
	bool ok = treeMap(e, cross, crossMc, 0x20000);
	f += check(ok && treeResolves(e, cross, crossMc, 0x20000), "tree: a 128 KiB map across the 1 GiB line succeeds and every page translates");
	f += check(e.sp.pages() == 4 + 1 /*PT of the far side*/ + 1 /*PT of the near side*/ + 1 /*second PDB0*/, "tree: the crossing cost exactly two PT pages and one more PDB0");
	// The walk above already proves the far side is reachable; check the structure too: PDB1 entries 4 and 5 name two different PDB0 pages.
	{
		uint64_t *p1 = nullptr, *p0a = nullptr, *p0b = nullptr, ph1 = 0, ph2 = 0, ph3 = 0;
		uint32_t i1 = 0, i2 = 0, i3 = 0;
		const bool got = TreeEnv::treePage(&e, 1, V >> 39, false, p1, ph1, i1) && TreeEnv::treePage(&e, 2, V >> 30, false, p0a, ph2, i2) &&
		                 TreeEnv::treePage(&e, 2, (V + G) >> 30, false, p0b, ph3, i3);
		f += check(got && i2 != i3 && GpuVm::entryPhysical(p1[GpuVm::index(V, 1)]) == ph2 && GpuVm::entryPhysical(p1[GpuVm::index(V + G, 1)]) == ph3,
		           "tree: PDB1 entries 4 and 5 name two different PDB0 pages");
	}

	// NEGATIVE CONTROL: the contiguous layout of modes 0/1 accepts the same map and then cannot reach the far side. This is the defect being fixed;
	// without it the test above would pass for a walker that never reads PDB1.
	{
		NewImage n;
		n.words.assign(kTableBytes / 8, 0);
		initLegacy(n.words, kRootPhys);
		const bool mapped = newVmMap(n, cross, crossMc, 0x20000, false);
		auto legacyRead = [](void *c, uint64_t address, uint64_t &entry) {
			auto *img = static_cast<NewImage *>(c);
			const uint64_t off = address - kRootPhys;
			if (off >= kTableBytes)
				return false;
			entry = img->words[off / 8];
			return true;
		};
		uint64_t pa = 0, fl = 0;
		const bool nearOk = GpuVm::walk(kRootPhys, cross, legacyRead, &n, pa, fl);
		const bool farOk = GpuVm::walk(kRootPhys, cross + 0x10000, legacyRead, &n, pa, fl);
		f += check(mapped && nearOk && !farOk, "negative control: the contiguous layout takes a map across 1 GiB but the far side does not translate (PDB1 entry 5 is empty)");
	}

	// Far apart: another PDB1 page (512 GiB up), and the top of the 48-bit space.
	const uint64_t far = V + (600ull << 30);
	f += check(treeMap(e, far, MC + 0x200000, 0x1000) && treeResolves(e, far, MC + 0x200000, 0x1000), "tree: a map 600 GiB up (root entry 1, its own PDB1 page) translates");
	const uint64_t top = GpuVm::kVaEnd - 0x2000;
	f += check(treeMap(e, top, MC + 0x300000, 0x2000) && treeResolves(e, top, MC + 0x300000, 0x2000), "tree: the last two pages of the 48-bit space translate");
	f += check(!GpuVmTable::treeMapVram(e.tree(), GpuVmTable::Policy {}, V - 0x1000, V + 0x1000, 0x1000, false), "tree: a map starting below kVaStart is refused");
	f += check(treeResolves(e, cross, crossMc, 0x20000) && treeResolves(e, V + 0x1000, MC, 0x3000), "tree: the earlier mappings are undisturbed by the later ones");

	// Unmap: PTEs go (through the dirty path), pages stay, nothing is created for a range that was never mapped.
	const uint32_t before = e.sp.pages();
	GpuVmTable::treeUnmap(e.tree(), cross, cross + 0x20000);
	e.sync();
	f += check(!e.translate(cross, phys) && !e.translate(cross + 0x10000, phys) && treeResolves(e, V + 0x1000, MC, 0x3000),
	           "tree: unmap clears exactly its PTEs, on both sides of the line");
	GpuVmTable::treeUnmap(e.tree(), V + 40 * G, V + 40 * G + 0x100000);
	f += check(e.sp.pages() == before, "tree: unmapping a range that was never backed backs nothing");
	// Host mapping through the same tree.
	{
		std::vector<uint64_t> buses(8);
		for (uint32_t i = 0; i < 8; i++)
			buses[i] = 0x104048000ull + 0x3000ull * i;
		const uint64_t hv = V + 3 * G + 0x7000;
		const bool hm = GpuVmTable::treeMapHost(e.tree(), GpuVmTable::Policy {}, hv, hv + 0x8000, buses.data(), false);
		e.sync();
		bool all = hm;
		for (uint32_t i = 0; i < 8; i++)
			all &= e.translate(hv + 0x1000ull * i, phys) && phys == buses[i];
		f += check(all, "tree: a host mapping in the fourth GiB translates to its bus addresses");
		// and a second one into the same, already synced PT page
		const uint64_t hv2 = hv + 0x10000;
		const bool hm2 = GpuVmTable::treeMapHost(e.tree(), GpuVmTable::Policy {}, hv2, hv2 + 0x2000, buses.data(), false);
		e.sync();
		f += check(hm2 && e.translate(hv2, phys) && phys == buses[0] && e.translate(hv2 + 0x1000, phys) && phys == buses[1],
		           "tree: a later host mapping into an existing PT page reaches VRAM");
		buses[3] |= 0x40;
		f += check(!GpuVmTable::treeMapHost(e.tree(), GpuVmTable::Policy {}, hv + 0x20000, hv + 0x28000, buses.data(), false), "tree: a misaligned bus address is refused");
	}

	// Quota: the VA limit is the page quota. Nothing half-made, earlier mappings survive.
	{
		static TreeEnv q;
		q = TreeEnv {};
		q.open(8);
		// root + PDB1 + PDB0 + 5 PT pages = 8
		bool fine = true;
		for (uint32_t i = 0; i < 5; i++)
			fine &= treeMap(q, V + i * 0x200000ull, MC + i * 0x10000, 0x1000);
		f += check(fine && q.sp.pages() == 8, "quota: root, PDB1, PDB0 and five PT pages fill a quota of 8");
		f += check(!treeMap(q, V + 5 * 0x200000ull, MC, 0x1000) && q.sp.pages() == 8, "quota: the sixth PT page is refused and leaves no page behind");
		f += check(!treeMap(q, V + G, MC, 0x1000) && q.sp.pages() == 8, "quota: a map into a new GiB (needs PDB0 + PT) is refused too");
		bool still = true;
		for (uint32_t i = 0; i < 5; i++)
			still &= treeResolves(q, V + i * 0x200000ull, MC + i * 0x10000, 0x1000);
		f += check(still, "quota: what was mapped before the refusal still translates");
		q.close();
		f += check(q.chunksLive == 0 && q.shadowLive == 0, "quota: close returns every chunk and shadow page");
		// host memory exhaustion behaves like the quota
		q = TreeEnv {};
		q.shadowLimit = 3;
		q.open(PtPages::kMaxPages);
		f += check(!treeMap(q, V, MC, 0x1000) && q.sp.pages() <= 3, "quota: running out of host shadow pages fails the map cleanly");
		q.close();
		f += check(q.chunksLive == 0 && q.shadowLive == 0, "quota: and close still returns everything");
	}

	// A wide client: many GiB at once, including straddles of each 1 GiB line; everything resolves.
	{
		static TreeEnv w;
		w = TreeEnv {};
		w.open(PtPages::kMaxPages);
		bool all = true;
		for (uint32_t g = 0; g < 40; g++)
			all &= treeMap(w, V + (g + 1ull) * G - 0x2000, MC + 0x400000 + g * 0x10000ull, 0x4000);
		for (uint32_t g = 0; g < 40; g++)
			all &= treeResolves(w, V + (g + 1ull) * G - 0x2000, MC + 0x400000 + g * 0x10000ull, 0x4000);
		f += check(all && w.sp.pages() > 40 * 2, "tree: 40 straddles of consecutive 1 GiB lines (a 40 GiB client) all translate");
		w.close();
		f += check(w.chunksLive == 0 && w.shadowLive == 0, "tree: close returns everything");
	}

	// The directory: collisions, duplicates, misses.
	{
		static PtPages::Directory d;
		d.init();
		bool ok2 = true;
		for (uint16_t i = 0; i < 2048; i++)
			ok2 &= d.insert(0x1000000000ull + i * 512ull, i);
		for (uint16_t i = 0; i < 2048; i++)
			ok2 &= d.find(0x1000000000ull + i * 512ull) == i;
		f += check(ok2 && d.find(12345) == -1 && !d.insert(0x1000000000ull, 7), "directory: 2048 keys found, a miss is -1, a duplicate is refused");
	}

	// Mutation switch (the checker of the checks): a tree that forgets to mark pages dirty must be caught by the walk over the synced VRAM.
	{
		static TreeEnv m;
		m = TreeEnv {};
		m.open(PtPages::kMaxPages);
		m.dropDirty = true;
		const bool mapped = treeMap(m, V, MC, 0x2000);
		f += check(mapped && !treeResolves(m, V, MC, 0x2000), "mutation check: if the walker forgot Tree::dirty the GPU's view would fault (the test sees it)");
		m.close();
	}
	e.close();
	f += check(e.chunksLive == 0 && e.shadowLive == 0, "tree: close returns every chunk and shadow page");
	printf("sparse tree: multi-level layout ok (VA beyond 1 GiB, quota, dirty sync, negative control)\n");
	return f;
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <vbios.rom>\n", argv[0]);
		return 2;
	}

	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> rom(static_cast<size_t>(len));
	if (fread(rom.data(), 1, rom.size(), f) != rom.size()) {
		fprintf(stderr, "short read\n");
		fclose(f);
		return 2;
	}
	fclose(f);

	AtomBios bios;
	if (!bios.init(rom.data(), rom.size())) {
		fprintf(stderr, "FAIL: no valid AtomBIOS image found in %s\n", argv[1]);
		return 1;
	}

	char name[64];
	bios.configName(name, sizeof(name));
	printf("VBIOS image     : offset 0x%zx, length %zu bytes, checksum OK\n",
	       bios.imageOffset(), bios.imageLength());
	printf("config name     : %s\n", name);
	printf("subsystem       : %04x:%04x\n", bios.subsystemVendorId(), bios.subsystemId());

	printf("\ndata tables:\n");
	for (uint32_t i = 0; i < AtomBios::MaxDataTable; i++) {
		AtomBios::TableHeader th;
		size_t off = bios.dataTable(static_cast<AtomBios::DataTable>(i), &th);
		if (off)
			printf("  [%2u] %-22s @0x%05zx size=%-5u rev %u.%u\n",
			       i, tableNames[i], off, th.size, th.formatRev, th.contentRev);
	}

	int failures = 0;

	AtomBios::FirmwareInfo3 fw;
	if (bios.getFirmwareInfo(fw)) {
		printf("\nfirmwareinfo v%u.%u:\n", fw.header.formatRev, fw.header.contentRev);
		printf("  firmware revision : 0x%08x\n", fw.firmwareRevision);
		printf("  bootup sclk       : %u kHz\n", fw.bootupSclk10KHz * 10);
		printf("  bootup mclk       : %u kHz\n", fw.bootupMclk10KHz * 10);
		printf("  capability flags  : 0x%08x\n", fw.firmwareCapability);
	} else {
		fprintf(stderr, "FAIL: firmwareinfo not parsed\n");
		failures++;
	}

	AtomBios::DisplayPath paths[AtomBios::MaxDisplayPaths];
	size_t n = bios.getDisplayPaths(paths, AtomBios::MaxDisplayPaths);
	if (n) {
		printf("\ndisplay paths (%zu):\n", n);
		for (size_t i = 0; i < n; i++) {
			auto type = AtomBios::connectorType(paths[i].connectorObjId);
			printf("  %zu: %-11s objid=0x%04x encoder=0x%04x devtag=0x%04x\n",
			       i, AtomBios::connectorName(type), paths[i].connectorObjId,
			       paths[i].encoderObjId, paths[i].deviceTag);

			AtomBios::PathRecords rec;
			if (bios.getPathRecords(paths[i], rec)) {
				printf("     i2c: id=0x%02x hw=%d ddc-line=%u   hpd: pin=%u state=%u\n",
				       rec.i2cId, rec.i2cHwCapable, rec.ddcLine, rec.hpdPin, rec.hpdPlugState);
				AtomBios::GpioPin ddc, hpd;
				if (rec.hasI2c && bios.findGpioPin(static_cast<uint8_t>(0x90 | rec.ddcLine), ddc))
					printf("     ddc gpio: reg_index=0x%05x (byte 0x%06x) shift=%u\n",
					       ddc.regIndex, ddc.regIndex * 4, ddc.shift);
				if (rec.hasHpd && bios.findGpioPin(rec.hpdPin, hpd))
					printf("     hpd gpio: reg_index=0x%05x (byte 0x%06x) shift=%u\n",
					       hpd.regIndex, hpd.regIndex * 4, hpd.shift);
			} else {
				fprintf(stderr, "FAIL: path %zu has no I2C/HPD records\n", i);
				failures++;
			}
		}
	} else {
		fprintf(stderr, "FAIL: no display paths parsed\n");
		failures++;
	}

	AtomBios::GpioPin pins[AtomBios::MaxGpioPins];
	size_t np = bios.getGpioPins(pins, AtomBios::MaxGpioPins);
	if (np) {
		printf("\ngpio pin lut (%zu pins):\n", np);
		for (size_t i = 0; i < np; i++)
			printf("  gpio_id=0x%02x reg_index=0x%05x shift=%-2u mask_shift=%u\n",
			       pins[i].gpioId, pins[i].regIndex, pins[i].shift, pins[i].maskShift);
	} else {
		fprintf(stderr, "FAIL: gpio pin lut not parsed\n");
		failures++;
	}

	IpDiscovery disc;
	if (disc.init(rom.data(), rom.size())) {
		printf("\nip discovery binary at 0x%zx, %u IPs on die 0:\n",
		       disc.binaryOffset(), disc.ipCount());
		static const struct { uint16_t id; const char *name; } wanted[] = {
			{ IpDiscovery::HwGc, "GC (gfx)" }, { IpDiscovery::HwDmu, "DMU (DCN)" },
			{ IpDiscovery::HwNbif, "NBIF" },   { IpDiscovery::HwMp0, "MP0 (PSP)" },
			{ IpDiscovery::HwMmhub, "MMHUB" }, { IpDiscovery::HwSdma0, "SDMA0" },
		};
		for (auto &w : wanted) {
			IpDiscovery::IpEntry ip;
			if (disc.findIp(w.id, 0, ip)) {
				printf("  %-9s v%u.%u.%u  bases:", w.name, ip.major, ip.minor, ip.revision);
				for (uint8_t b = 0; b < ip.numBases; b++)
					printf(" 0x%08x", ip.bases[b]);
				printf("\n");
			} else {
				fprintf(stderr, "FAIL: IP hw_id %u missing from discovery\n", w.id);
				failures++;
			}
		}

		// The register this maps is the kext's first MMIO smoke-test read:
		// RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE (NBIF seg 2, dword 0x00c3) —
		// VRAM size in MiB, expected 16384 on this card.
		uint32_t memsize;
		if (disc.regByteOffset(IpDiscovery::HwNbif, 0, 2, 0x00c3, memsize)) {
			printf("  RCC_CONFIG_MEMSIZE MMIO byte offset: 0x%x\n", memsize);
			if (memsize != 0x378c) {
				fprintf(stderr, "FAIL: unexpected RCC_CONFIG_MEMSIZE offset\n");
				failures++;
			}
		} else {
			fprintf(stderr, "FAIL: cannot derive RCC_CONFIG_MEMSIZE offset\n");
			failures++;
		}

		IpDiscovery::IpEntry gc;
		if (disc.findIp(IpDiscovery::HwGc, 0, gc) && gc.major != 12) {
			fprintf(stderr, "FAIL: GC major %u, expected 12 (RDNA4)\n", gc.major);
			failures++;
		}
	} else {
		fprintf(stderr, "FAIL: no IP discovery binary found\n");
		failures++;
	}

	// Command-function inventory. Finding (2026-07-12, this ROM): the display
	// bytecode routines (setpixelclock, transmitter control, encoder control)
	// are ABSENT — on DCN 3.1+ they were replaced by DMUB firmware mailbox
	// commands (DMUB_CMD__VBIOS_*), so mode setting cannot go through an
	// AtomBIOS interpreter on this card. Only asic_init survives.
	static const struct { uint8_t idx; const char *name; bool required; } cmds[] = {
		{ AtomBios::CmdAsicInit,               "asic_init",               true  },
		{ AtomBios::CmdDigEncoderControl,      "digxencodercontrol",      false },
		{ AtomBios::CmdSetPixelClock,          "setpixelclock",           false },
		{ AtomBios::CmdEnableDispPowerGating,  "enabledisppowergating",   false },
		{ AtomBios::CmdBlankCrtc,              "blankcrtc",               false },
		{ AtomBios::CmdEnableCrtc,             "enablecrtc",              false },
		{ AtomBios::CmdSelectCrtcSource,       "selectcrtc_source",       false },
		{ AtomBios::CmdSetDceClock,            "setdceclock",             false },
		{ AtomBios::CmdSetCrtcUsingDtdTiming,  "setcrtc_usingdtdtiming",  false },
		{ AtomBios::CmdDig1TransmitterControl, "dig1transmittercontrol",  false },
		{ AtomBios::CmdProcessAuxChannel,      "processauxchanneltransaction", false },
	};
	printf("\ncommand functions (bytecode):\n");
	for (auto &c : cmds) {
		AtomBios::CmdTableInfo info;
		if (bios.getCommandTable(c.idx, info)) {
			printf("  [%2u] %-28s @0x%05zx size=%-5u rev %u.%u\n", c.idx, c.name,
			       info.offset - bios.imageOffset(), info.size,
			       info.formatRev, info.contentRev);
		} else {
			printf("  [%2u] %-28s absent\n", c.idx, c.name);
			if (c.required) {
				fprintf(stderr, "FAIL: required command function %s missing\n", c.name);
				failures++;
			}
		}
	}

	failures += testEdidParser();
	failures += testModes();
	failures += testPipeDiscovery();
	failures += testDmubPayloads();
	failures += testNdrv();
	failures += testModeSet();
	failures += testDpRetime();
	failures += testPipe2();
	failures += testPsp();
	failures += testGfxImages();
	failures += testSdmaPackets();
	failures += testIhRing();
	failures += testSmuMetricsOffsets();
	failures += testPm4Packets();
	failures += testCodeObject();
	failures += testGpuHeap();
	failures += testFlipArithmetic();
	failures += testGcInfo();
	failures += testGpuVm();
	failures += testVmidPool();
	failures += testPtPages();
	failures += testVmTableDifferential();
	failures += testSparseTree();
	failures += testLinuxRefFormat();

	if (failures) {
		fprintf(stderr, "\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
