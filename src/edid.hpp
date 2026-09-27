//
//  edid.hpp
//  RDNA4FB
//
//  Freestanding EDID parser. A detailed timing descriptor (DTD) carries
//  everything the OTG timing generator needs to be programmed with (totals,
//  blanking, sync position/width/polarity and the pixel clock), so this is
//  the bridge from "EDID read" to "mode set". Beyond the DTDs it decodes the
//  rest of what a sink advertises — established/standard timings (mapped to
//  VESA DMT), CTA-861 short video descriptors (mapped to CEA-861 timings),
//  the display range limits and the physical image size — so src/modes can
//  build the mode list offered to macOS.
//
//  The EDID comes from the monitor, i.e. it is untrusted input: every read
//  is bounds-checked and malformed fields are treated as absent.
//
//  No kernel or libc dependencies; usable from both the kext and the host
//  test harness (tools/atomdump).
//

#ifndef Edid_hpp
#define Edid_hpp

#include <stdint.h>
#include <stddef.h>

namespace Edid {

constexpr size_t BlockSize = 128;
constexpr size_t MaxBlocks = 4;   // base + 3 extensions: all the kext reads

struct DetailedTiming {
	uint32_t pixelClockKHz;   // e.g. 533250 for this Samsung's 4K60
	uint16_t hActive, hBlank; // hTotal = hActive + hBlank
	uint16_t hSyncOffset;     // front porch (active end -> sync start)
	uint16_t hSyncWidth;
	uint16_t vActive, vBlank;
	uint16_t vSyncOffset;
	uint16_t vSyncWidth;
	bool     hSyncPositive;
	bool     vSyncPositive;
	bool     interlaced;

	uint32_t hTotal() const { return static_cast<uint32_t>(hActive) + hBlank; }
	uint32_t vTotal() const { return static_cast<uint32_t>(vActive) + vBlank; }
	// Field/frame rate in millihertz (60000 = 60 Hz).
	uint32_t refreshMilliHz() const {
		uint64_t total = static_cast<uint64_t>(hTotal()) * vTotal();
		if (!total) return 0;
		return static_cast<uint32_t>((static_cast<uint64_t>(pixelClockKHz) * 1000000ULL) / total);
	}
};

// Parse one 18-byte descriptor. Returns false if it is not a detailed timing
// (pixel clock 0 marks display/monitor descriptors) or is malformed.
bool parseDetailedTiming(const uint8_t d[18], DetailedTiming &out);

// Parse the preferred (first) DTD of a 128-byte EDID base block. Validates
// the block header only, not the checksum — the kext's boot-time EDID log
// uses this and should still show what a sink with a bad checksum sent.
bool preferredTiming(const uint8_t *edid, size_t len, DetailedTiming &out);

// --- block validation --------------------------------------------------------

// All 128 bytes of a block sum to 0 mod 256 (E-EDID 1.4 §3.11).
bool blockChecksumOk(const uint8_t *block);

// Base block: 00 FF FF FF FF FF FF 00 header, and checksum.
bool baseBlockValid(const uint8_t *edid, size_t len);

// --- base block --------------------------------------------------------------

// Display range limits descriptor (tag 0xFD), with the EDID 1.4 +255 rate
// offsets already applied.
struct RangeLimits {
	bool     present           { false };
	uint16_t minVHz            { 0 }, maxVHz  { 0 };
	uint16_t minHKHz           { 0 }, maxHKHz { 0 };
	uint32_t maxPixelClockKHz  { 0 };      // 0 = not given
	uint8_t  timingSupport     { 0 };      // byte 10: 0 GTF, 1 limits only, 2 GTF2, 4 CVT
	bool     cvtReducedBlanking { false }; // CVT flags byte 15 bit 4
};

constexpr size_t EstablishedCount = 17;   // bytes 35..37: 8 + 8 + 1 defined bits
constexpr size_t StandardCount    = 8;    // bytes 38..53: 2-byte codes

struct BaseInfo {
	uint8_t  version     { 0 }, revision { 0 };
	bool     digital     { false };       // byte 20 bit 7
	uint8_t  extensionCount { 0 };        // byte 126 (may exceed what was read)

	// Physical image size. Taken from the preferred DTD (mm precision)
	// unless that is absent or disagrees with the base block's cm size by
	// more than the cm rounding — some sinks ship garbage there (the Samsung
	// G70D fixture claims 698x392 mm, i.e. 31.5", for a 27" panel) — then
	// base bytes 21/22 x 10. 0 = unknown (projector, or aspect-ratio-only).
	uint16_t widthMm     { 0 }, heightMm { 0 };
	uint8_t  screenWidthCm { 0 }, screenHeightCm { 0 };   // raw bytes 21/22

	// All DTDs among the 4 descriptor slots, in slot order. When
	// hasPreferred, dtds[0] came from slot 0: the preferred timing mode.
	DetailedTiming dtds[4] {};
	size_t   dtdCount    { 0 };
	bool     hasPreferred { false };

	RangeLimits range {};

	uint8_t  established[3] {};               // raw bytes 35..37
	uint8_t  standard[StandardCount][2] {};   // raw bytes 38..53

	// The sink accepts CVT reduced-blanking timings: explicit in EDID 1.4
	// (CVT range descriptor flag); EDID 1.3 has no flag, and every digital
	// sink handles RB in practice (same inference as Linux drm_edid).
	bool     reducedBlanking { false };
};

// Parse and validate (header, checksum, version 1.x) the 128-byte base block.
bool parseBaseBlock(const uint8_t *edid, size_t len, BaseInfo &out);

// Established timing bit `index` (0 = byte 35 bit 7 ... 16 = byte 37 bit 7)
// -> DMT timing. False when the bit is clear or names a mode outside the
// DMT table (IBM 720x400, Apple 640x480@67/832x624/1152x870, 1024x768i).
bool establishedTiming(const BaseInfo &info, size_t index, DetailedTiming &out);

// Standard timing `index` (0..7) -> DMT timing. False for unused slots and
// for size/refresh combinations absent from the DMT table (no CVT/GTF
// formula here). Picks the CVT-RB DMT variant when info.reducedBlanking,
// else the classic one, falling back to whichever exists.
bool standardTiming(const BaseInfo &info, size_t index, DetailedTiming &out);

// VESA DMT lookup by active size and nominal refresh (e.g. 60 for 59.94).
bool dmtTiming(uint16_t hActive, uint16_t vActive, uint16_t hz,
               bool preferReducedBlanking, DetailedTiming &out);

// CEA-861 VIC -> timing. False for unknown and for interlaced VICs.
bool vicTiming(uint8_t vic, DetailedTiming &out);

// --- CTA-861 extension block ------------------------------------------------
// Sink capabilities needed to pick a legal signal for mode setting: pixel
// repertoire (short video descriptors), color format support, and — for HDMI
// sinks — the maximum TMDS character clock from the HDMI VSDB.

constexpr size_t MaxCtaDtds = 6;   // (127 - 4) / 18: all that fit in a block

struct CtaCaps {
	uint8_t  revision   { 0 };
	bool     underscan  { false };
	bool     basicAudio { false };
	bool     ycbcr444   { false };
	bool     ycbcr422   { false };
	uint8_t  vics[32]   {};     // short video descriptors (VIC codes, native flag stripped)
	bool     vicNative[32] {};  // SVD native flag (only defined for VIC 1-64)
	size_t   vicCount   { 0 };
	bool     hasHdmiVsdb { false };  // IEEE OUI 00-0C-03 vendor block present
	uint32_t maxTmdsKHz { 0 };       // 0 = not advertised
	size_t   dtdCount   { 0 };       // additional 18-byte timings in the block
	DetailedTiming firstDtd {};      // valid when dtdCount > 0
	DetailedTiming dtds[MaxCtaDtds] {};   // dtds[0..dtdCount)
};

// Parse one 128-byte CTA-861 extension (tag 0x02). Returns false if the tag
// or structure is invalid. Does not verify the checksum (see blockChecksumOk).
bool parseCtaBlock(const uint8_t ext[128], CtaCaps &out);

} // namespace Edid

#endif /* Edid_hpp */
