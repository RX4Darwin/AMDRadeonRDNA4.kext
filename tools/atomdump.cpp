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
#include "../src/amdfw.hpp"
#include "../src/psp.hpp"
#include "../src/sdma.hpp"
#include "../src/pm4.hpp"
#include "../src/codeobj.hpp"
#include "../src/gpuheap.hpp"
#include "../src/gpuvm.hpp"
#include "../src/vadd_codeobj.h"
#include "../src/bench_codeobj.h"
#include "../src/gfxregs.hpp"
#include "rdna4compute.h"

#include <cstdarg>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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
	dcn.set(2, Reg::kDigBeCntl + d, (1u << (8 + 2)) | (3u << 28));
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
	t.encoderObjId = 0x2120; t.connectorObjId = 0x330c; t.pllId = 0x14;
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
	failures += check(pll[0] == 0x10000280 && pll[1] == 742500 && pll[2] == 0x00032014 &&
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
	regs.set(2, Pipe::Reg::kDigBeCntl + d2, (1u << 10) | (3u << 28));
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

	// Refusals: interlaced targets and clocks beyond single-link TMDS.
	ModeSet::Target bad = t;
	bad.to.interlaced = true;
	failures += check(!ModeSet::build(bad, plan, &why), "modeset: interlaced target accepted");
	bad = t;
	bad.to.pixelClockKHz = 594000;
	failures += check(!ModeSet::build(bad, plan, &why), "modeset: 594 MHz target accepted");

	printf("\nmodeset: 1080p60 -> 720p60 on OTG0/DIG2/UNIPHYC: %zu steps, %zu DMUB commands, "
	       "vstartup %u %s\n", steps, cmds, vstartup, failures ? "MISMATCH" : "ok");
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

	printf("\nsdma: WRITE_LINEAR/FENCE/CONST_FILL/COPY_LINEAR encodings and ring wrap %s\n",
	       failures ? "FAILED" : "ok");
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

	// bench.cl: six kernels in one file, each with its own descriptor and
	// LDS (llvm-readelf --notes: group_segment_fixed_size).
	struct { const char *name; uint32_t lds, kernarg; } bench[] = {
		{ "lds_reverse", 256, 24 }, { "copy", 0, 16 }, { "sgemm", 8320, 28 },
		{ "wmma16", 0, 24 }, { "hgemm", 20480, 28 }, { "bf16gemm", 20480, 28 },
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
		GpuVm::kValid | GpuVm::kSnooped | GpuVm::kReadable | GpuVm::kWritable,
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
	    GpuVm::encodePte(physical, GpuVm::kValid | GpuVm::kSnooped | GpuVm::kReadable | GpuVm::kWritable,
	                      true));
	uint64_t got = 0, flags = 0;
	failures += check(GpuVm::walk(root, va + 0x345, readVmTestEntry, &table, got, flags) &&
	                  got == physical + 0x345 && (flags & GpuVm::kWritable),
	                  "gfx12 page-table walk returns the mapped physical address");
	failures += check(!GpuVm::walk(root, 0x2000, readVmTestEntry, &table, got, flags),
	                  "gfx12 page-table walk rejects an unmapped VA");
	return failures;
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
	failures += testPsp();
	failures += testGfxImages();
	failures += testSdmaPackets();
	failures += testPm4Packets();
	failures += testCodeObject();
	failures += testGpuHeap();
	failures += testGpuVm();

	if (failures) {
		fprintf(stderr, "\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
