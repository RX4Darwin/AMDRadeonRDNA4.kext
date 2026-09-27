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

#include <cstdarg>
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

	// Mode timing: valid + safe everywhere, default on the boot mode only.
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

	if (failures) {
		fprintf(stderr, "\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
