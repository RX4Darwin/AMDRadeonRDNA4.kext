//
//  modes.hpp
//  RDNA4FB
//
//  The display-mode table offered to macOS, built from the sink's EDID.
//  IOFramebuffer identifies modes by driver-chosen IDs that WindowServer
//  persists in its display preferences, so the table must be deterministic:
//  the same EDID and limits always yield the same modes under the same IDs,
//  and no two entries may look identical in the Displays pane (same size,
//  same whole-Hz refresh) — macOS would show both and the user could not
//  tell which timing they picked.
//
//  Sources, in the priority order used when two of them describe the same
//  size and rate (the first one wins): base-block DTDs (the first is the
//  preferred timing), CTA-861 extension DTDs, CTA short video descriptors
//  (CEA-861 timings), standard timings and established timings (VESA DMT).
//  Detailed timings are exact rasters the sink declared; everything else is
//  a table lookup, so it is additionally held to the EDID's own range
//  limits (vertical rate, max pixel clock) and the HDMI VSDB TMDS limit.
//
//  Freestanding: shared by the kext and the host test harness; no heap —
//  the caller owns the Mode array.
//

#ifndef Modes_hpp
#define Modes_hpp

#include "edid.hpp"

namespace Modes {

constexpr size_t MaxModes = 32;

// Where a mode came from, for logging (and, for the EDID sources, the
// deduplication priority described above).
enum Source : uint8_t {
	SourceDtd    = 0,   // base-block detailed timing
	SourceCtaDtd = 1,   // detailed timing in a CTA-861 extension
	SourceVic    = 2,   // CTA-861 short video descriptor -> CEA-861 table
	SourceStd    = 3,   // standard timing -> DMT table
	SourceEst    = 4,   // established timing -> DMT table
	SourceBoot   = 5,   // read back from the OTG the GOP programmed
};

// Short lowercase tag ("dtd", "cta-dtd", "vic", "std", "est", "boot").
const char *sourceName(uint8_t source);

struct Mode {
	uint32_t id;                 // stable 1..N, assigned in table order
	Edid::DetailedTiming t;
	uint32_t refreshMilliHz;     // rounded to the nearest mHz
	bool native;                 // EDID preferred timing
	uint8_t source;              // Source
};

struct Limits {
	uint32_t maxHActive, maxVActive;   // e.g. boot framebuffer size; 0 = no limit
	// min(TMDS 340000, DISPCLK cap, ...); 0 = no limit. build() already
	// holds table-derived modes to the EDID range-limit clock; folding that
	// in here as well also applies it to the sink's own DTDs.
	uint32_t maxPixelClockKHz;
};

// Build a deterministic, deduplicated, filtered table from an EDID blob
// (base block + up to 3 extensions; extensions with a bad checksum or an
// unknown tag are skipped, a bad base block yields 0 modes). Order: the
// native mode first, then descending hActive, vActive, refresh. Returns the
// number of modes written (at most cap). A native mode exists only if the
// preferred timing (or, failing that, a CTA SVD flagged native) survived
// the filters.
size_t build(const uint8_t *edid, size_t len, const Limits &lim, Mode *out, size_t cap);

// Index of the mode whose timing matches `t` (same active size and totals,
// pixel clock within 0.5%), or -1.
int match(const Mode *modes, size_t n, const Edid::DetailedTiming &t);

// Insert a timing (e.g. the timing read back from the GOP-programmed OTG)
// if no match exists; returns its index, or -1 if `t` is unusable. A new
// timing is appended with the next free ID so the EDID modes keep theirs;
// if an entry with the same size and whole-Hz rate but a different raster
// exists, `t` replaces its timing in place (ID and native flag kept): the
// running timing is proven on this sink, and adding it would create a
// duplicate. When the table is full the last entry is replaced.
int ensure(Mode *modes, size_t &n, size_t cap, const Edid::DetailedTiming &t, uint8_t source);

// 16.16 refresh for IODisplayModeInformation.
uint32_t refresh1616(const Edid::DetailedTiming &t);

// The sanity rules every table entry passes: progressive, even width,
// non-zero sync pulses, a raster OtgTiming::compute accepts, 1-1000 Hz.
bool usable(const Edid::DetailedTiming &t);

} // namespace Modes

#endif /* Modes_hpp */
