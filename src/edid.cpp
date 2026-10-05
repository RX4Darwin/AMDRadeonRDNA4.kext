//
//  edid.cpp
//  RDNA4FB
//
//  DTD field packing per VESA E-EDID 1.4 §3.10.2; base-block layout per
//  E-EDID 1.4 §3; CTA extension per CTA-861-G §7.
//

#include "edid.hpp"

namespace Edid {

namespace {

// A fixed timing from a standard, stored as porches (not DTD blank/offset
// fields) so each row can be compared against the published table directly.
struct FixedTiming {
	uint8_t  id;        // DMT ID or CEA VIC
	uint8_t  hz;        // nominal refresh (lookup key: 60 for 59.94)
	uint8_t  flags;
	uint32_t pclkKHz;
	uint16_t hActive, hFront, hSync, hBack;
	uint16_t vActive, vFront, vSync, vBack;
};

constexpr uint8_t kPosH = 0x01;   // +hsync
constexpr uint8_t kPosV = 0x02;   // +vsync
constexpr uint8_t kRb   = 0x04;   // DMT lists it as a (CVT) reduced-blanking timing

// VESA DMT v1.0 rev 13 subset: every mode the established timings can name
// plus the sizes that standard timings commonly carry. Borders (only the
// 640x480@72 row has them, 8 px/lines) are folded into the porches, as
// Linux drm_dmt_modes does — on a digital link they are blanking anyway.
const FixedTiming kDmt[] = {
	//  id   Hz  flags            pclk   hAct  hFP hSync  hBP  vAct vFP vSync vBP
	{ 0x04, 60, 0,              25175,  640,  16,  96,  48,  480, 10, 2, 33 },
	{ 0x05, 72, 0,              31500,  640,  24,  40, 128,  480,  9, 3, 28 },
	{ 0x06, 75, 0,              31500,  640,  16,  64, 120,  480,  1, 3, 16 },
	{ 0x08, 56, kPosH | kPosV,  36000,  800,  24,  72, 128,  600,  1, 2, 22 },
	{ 0x09, 60, kPosH | kPosV,  40000,  800,  40, 128,  88,  600,  1, 4, 23 },
	{ 0x0a, 72, kPosH | kPosV,  50000,  800,  56, 120,  64,  600, 37, 6, 23 },
	{ 0x0b, 75, kPosH | kPosV,  49500,  800,  16,  80, 160,  600,  1, 3, 21 },
	{ 0x10, 60, 0,              65000, 1024,  24, 136, 160,  768,  3, 6, 29 },
	{ 0x11, 70, 0,              75000, 1024,  24, 136, 144,  768,  3, 6, 29 },
	{ 0x12, 75, kPosH | kPosV,  78750, 1024,  16,  96, 176,  768,  1, 3, 28 },
	{ 0x15, 75, kPosH | kPosV, 108000, 1152,  64, 128, 256,  864,  1, 3, 32 },
	{ 0x55, 60, kPosH | kPosV,  74250, 1280, 110,  40, 220,  720,  5, 5, 20 },
	{ 0x1b, 60, kPosH | kRb,    71000, 1280,  48,  32,  80,  800,  3, 6, 14 },
	{ 0x1c, 60, kPosV,          83500, 1280,  72, 128, 200,  800,  3, 6, 22 },
	{ 0x20, 60, kPosH | kPosV, 108000, 1280,  96, 112, 312,  960,  1, 3, 36 },
	{ 0x23, 60, kPosH | kPosV, 108000, 1280,  48, 112, 248, 1024,  1, 3, 38 },
	{ 0x24, 75, kPosH | kPosV, 135000, 1280,  16, 144, 248, 1024,  1, 3, 38 },
	{ 0x27, 60, kPosH | kPosV,  85500, 1360,  64, 112, 256,  768,  3, 6, 18 },
	{ 0x51, 60, kPosH | kPosV,  85500, 1366,  70, 143, 213,  768,  3, 3, 24 },
	{ 0x56, 60, kPosH | kPosV | kRb, 72000, 1366, 14, 56, 64, 768,  1, 3, 28 },
	{ 0x29, 60, kPosH | kRb,   101000, 1400,  48,  32,  80, 1050,  3, 4, 23 },
	{ 0x2a, 60, kPosV,         121750, 1400,  88, 144, 232, 1050,  3, 4, 32 },
	{ 0x2e, 60, kPosH | kRb,    88750, 1440,  48,  32,  80,  900,  3, 6, 17 },
	{ 0x2f, 60, kPosV,         106500, 1440,  80, 152, 232,  900,  3, 6, 25 },
	// DMT has 1600x900@60 only as its "RB" timing (108 MHz, not CVT-RB).
	{ 0x53, 60, kPosH | kPosV | kRb, 108000, 1600, 24, 80, 96, 900,  1, 3, 96 },
	{ 0x33, 60, kPosH | kPosV, 162000, 1600,  64, 192, 304, 1200,  1, 3, 46 },
	{ 0x39, 60, kPosH | kRb,   119000, 1680,  48,  32,  80, 1050,  3, 6, 21 },
	{ 0x3a, 60, kPosV,         146250, 1680, 104, 176, 280, 1050,  3, 6, 30 },
	// DMT 0x52 is the CEA-861 VIC 16 timing.
	{ 0x52, 60, kPosH | kPosV, 148500, 1920,  88,  44, 148, 1080,  4, 5, 36 },
	{ 0x44, 60, kPosH | kRb,   154000, 1920,  48,  32,  80, 1200,  3, 6, 26 },
	{ 0x45, 60, kPosV,         193250, 1920, 136, 200, 336, 1200,  3, 6, 36 },
};

// CEA-861-F / CTA-861-G Table 1 subset: the progressive formats a PC
// monitor or TV plausibly lists. Interlaced VICs (5, 6, 7, 20, 21, 22, ...)
// are deliberately absent — the OTG is only ever programmed progressive.
// The 59.94/60 Hz pairs share one VIC; the integer-rate clock is used.
const FixedTiming kCea[] = {
	//  VIC  Hz  flags            pclk   hAct   hFP hSync  hBP  vAct vFP vSync vBP
	{   1, 60, 0,              25175,  640,   16,  96,  48,  480, 10,  2, 33 },
	{   2, 60, 0,              27000,  720,   16,  62,  60,  480,  9,  6, 30 },
	{   3, 60, 0,              27000,  720,   16,  62,  60,  480,  9,  6, 30 },
	{   4, 60, kPosH | kPosV,  74250, 1280,  110,  40, 220,  720,  5,  5, 20 },
	{  16, 60, kPosH | kPosV, 148500, 1920,   88,  44, 148, 1080,  4,  5, 36 },
	{  17, 50, 0,              27000,  720,   12,  64,  68,  576,  5,  5, 39 },
	{  18, 50, 0,              27000,  720,   12,  64,  68,  576,  5,  5, 39 },
	{  19, 50, kPosH | kPosV,  74250, 1280,  440,  40, 220,  720,  5,  5, 20 },
	{  31, 50, kPosH | kPosV, 148500, 1920,  528,  44, 148, 1080,  4,  5, 36 },
	{  32, 24, kPosH | kPosV,  74250, 1920,  638,  44, 148, 1080,  4,  5, 36 },
	{  33, 25, kPosH | kPosV,  74250, 1920,  528,  44, 148, 1080,  4,  5, 36 },
	{  34, 30, kPosH | kPosV,  74250, 1920,   88,  44, 148, 1080,  4,  5, 36 },
	{  60, 24, kPosH | kPosV,  59400, 1280, 1760,  40, 220,  720,  5,  5, 20 },
	{  61, 25, kPosH | kPosV,  74250, 1280, 2420,  40, 220,  720,  5,  5, 20 },
	{  62, 30, kPosH | kPosV,  74250, 1280, 1760,  40, 220,  720,  5,  5, 20 },
	{  63, 120, kPosH | kPosV, 297000, 1920,  88,  44, 148, 1080,  4,  5, 36 },
	{  64, 100, kPosH | kPosV, 297000, 1920, 528,  44, 148, 1080,  4,  5, 36 },
	{  93, 24, kPosH | kPosV, 297000, 3840, 1276,  88, 296, 2160,  8, 10, 72 },
	{  94, 25, kPosH | kPosV, 297000, 3840, 1056,  88, 296, 2160,  8, 10, 72 },
	{  95, 30, kPosH | kPosV, 297000, 3840,  176,  88, 296, 2160,  8, 10, 72 },
	{  96, 50, kPosH | kPosV, 594000, 3840, 1056,  88, 296, 2160,  8, 10, 72 },
	{  97, 60, kPosH | kPosV, 594000, 3840,  176,  88, 296, 2160,  8, 10, 72 },
};

// Established timings I/II (+ the one defined bit of byte 37), in bit order
// (index 0 = byte 35 bit 7): the DMT ID each names, 0 where the mode is not
// a DMT timing (IBM/Apple legacy) or is interlaced (1024x768@87i).
const uint8_t kEstablishedDmt[EstablishedCount] = {
	0,    0,    0x04, 0,    0x05, 0x06, 0x08, 0x09,   // 35: 720x400@70/88, 640x480@60/67/72/75, 800x600@56/60
	0x0a, 0x0b, 0,    0,    0x10, 0x11, 0x12, 0x24,   // 36: 800x600@72/75, 832x624@75, 1024x768@87i/60/70/75, 1280x1024@75
	0,                                                // 37: 1152x870@75 (Apple)
};

void toDetailed(const FixedTiming &f, DetailedTiming &out) {
	out = DetailedTiming {};
	out.pixelClockKHz = f.pclkKHz;
	out.hActive       = f.hActive;
	out.hBlank        = static_cast<uint16_t>(f.hFront + f.hSync + f.hBack);
	out.hSyncOffset   = f.hFront;
	out.hSyncWidth    = f.hSync;
	out.vActive       = f.vActive;
	out.vBlank        = static_cast<uint16_t>(f.vFront + f.vSync + f.vBack);
	out.vSyncOffset   = f.vFront;
	out.vSyncWidth    = f.vSync;
	out.hSyncPositive = (f.flags & kPosH) != 0;
	out.vSyncPositive = (f.flags & kPosV) != 0;
	out.interlaced    = false;
}

bool within10(uint16_t a, uint16_t b) {
	return (a > b ? a - b : b - a) <= 10;
}

bool headerOk(const uint8_t *edid) {
	static const uint8_t sig[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	for (int i = 0; i < 8; i++)
		if (edid[i] != sig[i])
			return false;
	return true;
}

// Display range limits descriptor, E-EDID 1.4 §3.10.3.3. The byte 4 rate
// offsets exist only in 1.4 (reserved 0 in 1.3); the CVT pixel clock
// refinement and flags only when byte 10 says CVT.
void parseRangeLimits(const uint8_t d[18], uint8_t revision, RangeLimits &r) {
	uint8_t off = revision >= 4 ? d[4] : 0;
	r.present  = true;
	r.minVHz   = static_cast<uint16_t>(d[5] + ((off & 0x03) == 0x03 ? 255 : 0));
	r.maxVHz   = static_cast<uint16_t>(d[6] + ((off & 0x02) ? 255 : 0));
	r.minHKHz  = static_cast<uint16_t>(d[7] + ((off & 0x0c) == 0x0c ? 255 : 0));
	r.maxHKHz  = static_cast<uint16_t>(d[8] + ((off & 0x08) ? 255 : 0));
	r.maxPixelClockKHz = static_cast<uint32_t>(d[9]) * 10000;
	r.timingSupport    = d[10];
	if (revision >= 4 && d[10] == 0x04) {
		// Byte 12 [7:2]: 0.25 MHz steps to subtract from the 10 MHz figure.
		uint32_t trim = static_cast<uint32_t>(d[12] >> 2) * 250;
		if (trim < r.maxPixelClockKHz)
			r.maxPixelClockKHz -= trim;
		r.cvtReducedBlanking = (d[15] & 0x10) != 0;
	}
}

} // namespace

bool parseDetailedTiming(const uint8_t d[18], DetailedTiming &out) {
	uint32_t pclk10k = static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8);
	if (pclk10k == 0)
		return false;   // a display descriptor, not a timing

	out.pixelClockKHz = pclk10k * 10;
	out.hActive = static_cast<uint16_t>(d[2] | ((d[4] & 0xf0) << 4));
	out.hBlank  = static_cast<uint16_t>(d[3] | ((d[4] & 0x0f) << 8));
	out.vActive = static_cast<uint16_t>(d[5] | ((d[7] & 0xf0) << 4));
	out.vBlank  = static_cast<uint16_t>(d[6] | ((d[7] & 0x0f) << 8));
	out.hSyncOffset = static_cast<uint16_t>(d[8]  | ((d[11] & 0xc0) << 2));
	out.hSyncWidth  = static_cast<uint16_t>(d[9]  | ((d[11] & 0x30) << 4));
	out.vSyncOffset = static_cast<uint16_t>((d[10] >> 4)  | ((d[11] & 0x0c) << 2));
	out.vSyncWidth  = static_cast<uint16_t>((d[10] & 0xf) | ((d[11] & 0x03) << 4));
	out.interlaced  = (d[17] & 0x80) != 0;

	// Sync polarity bits are only defined for digital separate sync
	// (d[17] bits [4:3] == 11); analog/composite variants report positive.
	if ((d[17] & 0x18) == 0x18) {
		out.hSyncPositive = (d[17] & 0x02) != 0;
		out.vSyncPositive = (d[17] & 0x04) != 0;
	} else {
		out.hSyncPositive = true;
		out.vSyncPositive = true;
	}

	if (out.hActive == 0 || out.vActive == 0)
		return false;
	return true;
}

bool preferredTiming(const uint8_t *edid, size_t len, DetailedTiming &out) {
	if (!edid || len < BlockSize || !headerOk(edid))
		return false;
	return parseDetailedTiming(edid + 54, out);
}

bool blockChecksumOk(const uint8_t *block) {
	if (!block)
		return false;
	uint8_t sum = 0;
	for (size_t i = 0; i < BlockSize; i++)
		sum = static_cast<uint8_t>(sum + block[i]);
	return sum == 0;
}

bool baseBlockValid(const uint8_t *edid, size_t len) {
	return edid && len >= BlockSize && headerOk(edid) && blockChecksumOk(edid);
}

bool parseBaseBlock(const uint8_t *edid, size_t len, BaseInfo &out) {
	out = BaseInfo {};
	if (!baseBlockValid(edid, len))
		return false;
	// EDID 2.0 (version byte 2) was a different, withdrawn layout.
	if (edid[18] != 1)
		return false;

	out.version        = edid[18];
	out.revision       = edid[19];
	out.digital        = (edid[20] & 0x80) != 0;
	out.screenWidthCm  = edid[21];
	out.screenHeightCm = edid[22];
	out.extensionCount = edid[126];
	for (size_t i = 0; i < 3; i++)
		out.established[i] = edid[35 + i];
	for (size_t i = 0; i < StandardCount; i++) {
		out.standard[i][0] = edid[38 + 2 * i];
		out.standard[i][1] = edid[39 + 2 * i];
	}

	uint16_t dtdWidthMm = 0, dtdHeightMm = 0;
	for (size_t slot = 0; slot < 4; slot++) {
		const uint8_t *d = edid + 54 + 18 * slot;
		if (d[0] | d[1]) {
			DetailedTiming t {};
			if (!parseDetailedTiming(d, t))
				continue;
			if (slot == 0) {
				out.hasPreferred = true;
				dtdWidthMm  = static_cast<uint16_t>(d[12] | ((d[14] & 0xf0) << 4));
				dtdHeightMm = static_cast<uint16_t>(d[13] | ((d[14] & 0x0f) << 8));
			}
			out.dtds[out.dtdCount++] = t;
			continue;
		}
		// Display descriptor: 00 00 00 <tag> ...; byte 2 must be 0 too.
		if (d[2] != 0)
			continue;
		if (d[3] == 0xfd && !out.range.present)
			parseRangeLimits(d, out.revision, out.range);
	}

	// Image size. Bytes 21/22 are whole cm, so a correct DTD mm figure lies
	// within ~5 mm of them; 10 mm of slack still rejects the garbage some
	// sinks put in the DTD (sizes of another model, or an aspect ratio).
	bool cmValid  = out.screenWidthCm && out.screenHeightCm;   // one 0 = aspect ratio
	bool dtdValid = dtdWidthMm && dtdHeightMm;
	uint16_t cmW = static_cast<uint16_t>(out.screenWidthCm * 10);
	uint16_t cmH = static_cast<uint16_t>(out.screenHeightCm * 10);
	if (dtdValid && (!cmValid || (within10(dtdWidthMm, cmW) && within10(dtdHeightMm, cmH)))) {
		out.widthMm  = dtdWidthMm;
		out.heightMm = dtdHeightMm;
	} else if (cmValid) {
		out.widthMm  = cmW;
		out.heightMm = cmH;
	}

	if (out.revision >= 4)
		out.reducedBlanking = out.range.present && out.range.timingSupport == 0x04 &&
		                      out.range.cvtReducedBlanking;
	else
		out.reducedBlanking = out.digital;
	return true;
}

bool dmtTiming(uint16_t hActive, uint16_t vActive, uint16_t hz,
               bool preferReducedBlanking, DetailedTiming &out) {
	const FixedTiming *fallback = nullptr;
	for (const FixedTiming &f : kDmt) {
		if (f.hActive != hActive || f.vActive != vActive || f.hz != hz)
			continue;
		if (((f.flags & kRb) != 0) == preferReducedBlanking) {
			toDetailed(f, out);
			return true;
		}
		if (!fallback)
			fallback = &f;
	}
	if (!fallback)
		return false;
	toDetailed(*fallback, out);
	return true;
}

bool vicTiming(uint8_t vic, DetailedTiming &out) {
	for (const FixedTiming &f : kCea) {
		if (f.id == vic) {
			toDetailed(f, out);
			return true;
		}
	}
	return false;
}

uint8_t vicOf(const DetailedTiming &t) {
	for (const FixedTiming &f : kCea) {
		DetailedTiming c {};
		toDetailed(f, c);
		if (c.pixelClockKHz == t.pixelClockKHz && c.hActive == t.hActive && c.hBlank == t.hBlank &&
		    c.hSyncOffset == t.hSyncOffset && c.hSyncWidth == t.hSyncWidth && c.vActive == t.vActive &&
		    c.vBlank == t.vBlank && c.vSyncOffset == t.vSyncOffset && c.vSyncWidth == t.vSyncWidth &&
		    c.hSyncPositive == t.hSyncPositive && c.vSyncPositive == t.vSyncPositive && !t.interlaced)
			return f.id;
	}
	return 0;
}

bool establishedTiming(const BaseInfo &info, size_t index, DetailedTiming &out) {
	if (index >= EstablishedCount)
		return false;
	if (!(info.established[index / 8] & (0x80 >> (index % 8))))
		return false;
	uint8_t id = kEstablishedDmt[index];
	if (!id)
		return false;
	for (const FixedTiming &f : kDmt) {
		if (f.id == id) {
			toDetailed(f, out);
			return true;
		}
	}
	return false;
}

bool standardTiming(const BaseInfo &info, size_t index, DetailedTiming &out) {
	if (index >= StandardCount)
		return false;
	uint8_t b0 = info.standard[index][0];
	uint8_t b1 = info.standard[index][1];
	// 01 01 marks an unused slot; 00 00 and 20 20 are common padding in the
	// wild. Byte 0 of 0/1 would be a 248/256-pixel mode: never real.
	if (b0 <= 0x01 || (b0 == 0x20 && b1 == 0x20))
		return false;

	uint16_t h  = static_cast<uint16_t>((b0 + 31) * 8);
	uint16_t hz = static_cast<uint16_t>((b1 & 0x3f) + 60);
	uint16_t v;
	switch (b1 >> 6) {
	case 0:  v = info.revision < 3 ? h : static_cast<uint16_t>(h * 10 / 16); break;  // 1:1 before 1.3
	case 1:  v = static_cast<uint16_t>(h * 3 / 4);   break;
	case 2:  v = static_cast<uint16_t>(h * 4 / 5);   break;
	default: v = static_cast<uint16_t>(h * 9 / 16);  break;
	}
	// 1366x768 cannot be expressed (width is not a multiple of 8, and 16:9
	// of 1360 is 765): sinks send 1360x765 or 1368x769 for it (as Linux).
	if (hz == 60 && ((h == 1360 && v == 765) || (h == 1368 && v == 769))) {
		h = 1366;
		v = 768;
	}
	return dmtTiming(h, v, hz, info.reducedBlanking, out);
}

bool parseCtaBlock(const uint8_t ext[128], CtaCaps &out) {
	if (!ext || ext[0] != 0x02)
		return false;
	out = CtaCaps {};
	out.revision   = ext[1];
	uint8_t dtdOff = ext[2];          // 0 = no DTDs and no data blocks
	out.underscan  = (ext[3] & 0x80) != 0;
	out.basicAudio = (ext[3] & 0x40) != 0;
	out.ycbcr444   = (ext[3] & 0x20) != 0;
	out.ycbcr422   = (ext[3] & 0x10) != 0;

	// Data block collection: [4, dtdOff). Each block: tag [7:5], length [4:0].
	if (out.revision >= 3 && dtdOff >= 4) {
		size_t pos = 4;
		size_t end = dtdOff < 128 ? dtdOff : 127;
		while (pos < end) {
			uint8_t tag = ext[pos] >> 5;
			uint8_t len = ext[pos] & 0x1f;
			if (pos + 1 + len > end)
				break;
			const uint8_t *p = ext + pos + 1;
			if (tag == 2) {                     // video block: SVDs
				// CTA-861-F §7.5.1: codes 129-192 are VIC 1-64 with the
				// native flag; 65-127 and 193-253 are the VIC itself;
				// 0, 128, 254, 255 are reserved.
				for (uint8_t i = 0; i < len && out.vicCount < 32; i++) {
					uint8_t b = p[i];
					if (b == 0 || b == 128 || b >= 254)
						continue;
					bool native = b >= 129 && b <= 192;
					out.vics[out.vicCount]      = native ? static_cast<uint8_t>(b & 0x7f) : b;
					out.vicNative[out.vicCount] = native;
					out.vicCount++;
				}
			} else if (tag == 3 && len >= 5 &&  // vendor block, HDMI LLC OUI
			           p[0] == 0x03 && p[1] == 0x0c && p[2] == 0x00) {
				out.hasHdmiVsdb = true;
				if (len >= 7 && p[6])
					out.maxTmdsKHz = static_cast<uint32_t>(p[6]) * 5000;
			} else if (tag == 3 && len >= 6 &&  // vendor block, HDMI Forum OUI
			           p[0] == 0xd8 && p[1] == 0x5d && p[2] == 0xc4) {
				// HDMI 2.0 table 10-6: version, Max_TMDS_Character_Rate / 5 MHz,
				// then SCDC_Present in bit 7.
				out.hfMaxTmdsKHz = static_cast<uint32_t>(p[4]) * 5000;
				out.scdcPresent = (p[5] & 0x80) != 0;
			}
			pos += 1u + len;
		}
	}

	// 18-byte detailed timings from dtdOff to the checksum byte.
	if (dtdOff >= 4) {
		size_t pos = dtdOff;
		while (pos + 18 <= 127) {
			DetailedTiming t {};
			if (!parseDetailedTiming(ext + pos, t))
				break;
			if (out.dtdCount == 0)
				out.firstDtd = t;
			if (out.dtdCount < MaxCtaDtds)
				out.dtds[out.dtdCount] = t;
			out.dtdCount++;
			pos += 18;
		}
	}
	return true;
}

Hdmi2Caps hdmi2Caps(const uint8_t *edid, size_t len) {
	Hdmi2Caps caps { false, 0 };
	for (size_t off = BlockSize; edid && off + BlockSize <= len; off += BlockSize) {
		CtaCaps cta {};
		if (!blockChecksumOk(edid + off) || !parseCtaBlock(edid + off, cta))
			continue;
		caps.scdc = caps.scdc || cta.scdcPresent;
		if (cta.hfMaxTmdsKHz > caps.maxTmdsKHz)
			caps.maxTmdsKHz = cta.hfMaxTmdsKHz;
	}
	return caps;
}

} // namespace Edid
