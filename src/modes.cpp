//
//  modes.cpp
//  RDNA4FB
//
//  See modes.hpp for the ordering, priority and filtering rules.
//

#include "modes.hpp"
#include "otgtiming.hpp"

namespace Modes {

namespace {

uint32_t roundedMilliHz(const Edid::DetailedTiming &t) {
	uint64_t total = static_cast<uint64_t>(t.hTotal()) * t.vTotal();
	if (!total)
		return 0;
	return static_cast<uint32_t>((static_cast<uint64_t>(t.pixelClockKHz) * 1000000ULL + total / 2) / total);
}

uint32_t wholeHz(uint32_t milliHz) {
	return (milliHz + 500) / 1000;
}

// The deduplication key: what the Displays pane shows for a mode.
bool sameSlot(const Mode &m, const Edid::DetailedTiming &t, uint32_t milliHz) {
	return m.t.hActive == t.hActive && m.t.vActive == t.vActive &&
	       wholeHz(m.refreshMilliHz) == wholeHz(milliHz);
}

int findSlot(const Mode *modes, size_t n, const Edid::DetailedTiming &t, uint32_t milliHz) {
	for (size_t i = 0; i < n; i++)
		if (sameSlot(modes[i], t, milliHz))
			return static_cast<int>(i);
	return -1;
}

struct Builder {
	Mode    *out;
	size_t   cap;
	size_t   n;
	Limits   lim;
	// The sink's envelope for table-derived modes. The EDID horizontal
	// range is deliberately not used: real sinks get it wrong (the Samsung
	// fixture's is 324-324 kHz, which would exclude every mode it has).
	uint16_t sinkMinVHz, sinkMaxVHz;   // 0/0 = none
	uint32_t sinkMaxPclkKHz;           // 0 = none

	void add(const Edid::DetailedTiming &t, uint8_t source, bool native) {
		if (n >= cap || !usable(t))
			return;
		if ((lim.maxHActive && t.hActive > lim.maxHActive) ||
		    (lim.maxVActive && t.vActive > lim.maxVActive) ||
		    (lim.maxPixelClockKHz && t.pixelClockKHz > lim.maxPixelClockKHz))
			return;
		uint32_t mHz = roundedMilliHz(t);
		// A DTD is the exact raster the sink was built for; a VIC/STD/EST
		// is only a code we expanded from a table, so it must also fit the
		// sink's declared envelope (a 50 Hz VIC on a 56-75 Hz monitor
		// would just show "out of range").
		if (source == SourceVic || source == SourceStd || source == SourceEst) {
			uint32_t hz = wholeHz(mHz);
			if (sinkMaxVHz && (hz < sinkMinVHz || hz > sinkMaxVHz))
				return;
			if (sinkMaxPclkKHz && t.pixelClockKHz > sinkMaxPclkKHz)
				return;
		}
		// Sources arrive in priority order: an existing entry for this
		// size and rate is the better description of it.
		if (findSlot(out, n, t, mHz) >= 0)
			return;
		Mode &m = out[n++];
		m = Mode {};
		m.t = t;
		m.refreshMilliHz = mHz;
		m.native = native;
		m.source = source;
	}
};

void capPclk(uint32_t &cap, uint32_t limit) {
	if (limit && (!cap || limit < cap))
		cap = limit;
}

// Table order: native first, then larger, then faster.
bool before(const Mode &a, const Mode &b) {
	if (a.native != b.native)
		return a.native;
	if (a.t.hActive != b.t.hActive)
		return a.t.hActive > b.t.hActive;
	if (a.t.vActive != b.t.vActive)
		return a.t.vActive > b.t.vActive;
	return a.refreshMilliHz > b.refreshMilliHz;
}

} // namespace

const char *sourceName(uint8_t source) {
	switch (source) {
	case SourceDtd:    return "dtd";
	case SourceCtaDtd: return "cta-dtd";
	case SourceVic:    return "vic";
	case SourceStd:    return "std";
	case SourceEst:    return "est";
	case SourceBoot:   return "boot";
	default:           return "?";
	}
}

bool usable(const Edid::DetailedTiming &t) {
	if (t.interlaced || t.pixelClockKHz == 0)
		return false;
	// DCN's two-pixels-per-clock paths (ODM combine, 4:2:0/4:2:2) need an
	// even width, and no real mode has an odd one (1366 is even).
	if (t.hActive & 1)
		return false;
	// Without a sync pulse the sink has nothing to lock to.
	if (t.hSyncWidth == 0 || t.vSyncWidth == 0)
		return false;
	OtgTiming::Regs r;
	if (!OtgTiming::compute(t, r))
		return false;
	uint32_t mHz = roundedMilliHz(t);
	return mHz >= 1000 && mHz <= 1000000;
}

uint32_t refresh1616(const Edid::DetailedTiming &t) {
	uint64_t total = static_cast<uint64_t>(t.hTotal()) * t.vTotal();
	if (!total)
		return 0;
	uint64_t v = ((static_cast<uint64_t>(t.pixelClockKHz) * 1000ULL << 16) + total / 2) / total;
	return v > 0xffffffffULL ? 0xffffffffu : static_cast<uint32_t>(v);
}

size_t build(const uint8_t *edid, size_t len, const Limits &lim, Mode *out, size_t cap) {
	if (!out || !cap)
		return 0;
	Edid::BaseInfo base {};
	if (!Edid::parseBaseBlock(edid, len, base))
		return 0;

	// Only blocks that were both read and announced by the base block.
	size_t blocks = len / Edid::BlockSize;
	if (blocks > 1u + base.extensionCount)
		blocks = 1u + base.extensionCount;
	if (blocks > Edid::MaxBlocks)
		blocks = Edid::MaxBlocks;

	Builder b { out, cap, 0, lim, 0, 0, 0 };
	if (base.range.present && base.range.maxVHz && base.range.minVHz <= base.range.maxVHz) {
		b.sinkMinVHz = base.range.minVHz;
		b.sinkMaxVHz = base.range.maxVHz;
	}
	capPclk(b.sinkMaxPclkKHz, base.range.maxPixelClockKHz);

	// 1. Base-block DTDs; slot 0 is the preferred (native) timing.
	for (size_t i = 0; i < base.dtdCount; i++)
		b.add(base.dtds[i], SourceDtd, base.hasPreferred && i == 0);

	// 2. CTA DTDs from every valid extension (also collecting the HDMI
	//    TMDS limit before any VIC is expanded).
	for (size_t blk = 1; blk < blocks; blk++) {
		const uint8_t *ext = edid + blk * Edid::BlockSize;
		Edid::CtaCaps cta {};
		if (!Edid::blockChecksumOk(ext) || !Edid::parseCtaBlock(ext, cta))
			continue;
		capPclk(b.sinkMaxPclkKHz, cta.maxTmdsKHz);
		size_t nd = cta.dtdCount < Edid::MaxCtaDtds ? cta.dtdCount : Edid::MaxCtaDtds;
		for (size_t i = 0; i < nd; i++)
			b.add(cta.dtds[i], SourceCtaDtd, false);
	}

	// 3. SVDs, in the sink's listed order.
	uint8_t nativeVic = 0;
	for (size_t blk = 1; blk < blocks; blk++) {
		const uint8_t *ext = edid + blk * Edid::BlockSize;
		Edid::CtaCaps cta {};
		if (!Edid::blockChecksumOk(ext) || !Edid::parseCtaBlock(ext, cta))
			continue;
		for (size_t i = 0; i < cta.vicCount; i++) {
			if (cta.vicNative[i] && !nativeVic)
				nativeVic = cta.vics[i];
			Edid::DetailedTiming t {};
			if (Edid::vicTiming(cta.vics[i], t))
				b.add(t, SourceVic, false);
		}
	}

	// 4./5. Standard, then established timings (DMT).
	for (size_t i = 0; i < Edid::StandardCount; i++) {
		Edid::DetailedTiming t {};
		if (Edid::standardTiming(base, i, t))
			b.add(t, SourceStd, false);
	}
	for (size_t i = 0; i < Edid::EstablishedCount; i++) {
		Edid::DetailedTiming t {};
		if (Edid::establishedTiming(base, i, t))
			b.add(t, SourceEst, false);
	}

	size_t n = b.n;

	// Preferred timing filtered out (e.g. a 4K sink behind a 1080p limit):
	// the CTA native format, if one survived, is the next best statement
	// of what the panel is.
	bool haveNative = false;
	for (size_t i = 0; i < n; i++)
		haveNative = haveNative || out[i].native;
	Edid::DetailedTiming nt {};
	if (!haveNative && nativeVic && Edid::vicTiming(nativeVic, nt)) {
		int idx = findSlot(out, n, nt, roundedMilliHz(nt));
		if (idx >= 0)
			out[idx].native = true;
	}

	// Stable insertion sort (n <= cap is small); equal keys cannot occur
	// after deduplication, so the order is fully determined by the EDID.
	for (size_t i = 1; i < n; i++) {
		Mode key = out[i];
		size_t j = i;
		while (j > 0 && before(key, out[j - 1])) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = key;
	}
	for (size_t i = 0; i < n; i++)
		out[i].id = static_cast<uint32_t>(i + 1);
	return n;
}

int match(const Mode *modes, size_t n, const Edid::DetailedTiming &t) {
	if (!modes)
		return -1;
	int best = -1;
	uint32_t bestDiff = 0;
	for (size_t i = 0; i < n; i++) {
		const Edid::DetailedTiming &m = modes[i].t;
		if (m.hActive != t.hActive || m.vActive != t.vActive ||
		    m.hTotal() != t.hTotal() || m.vTotal() != t.vTotal())
			continue;
		uint32_t diff = m.pixelClockKHz > t.pixelClockKHz ? m.pixelClockKHz - t.pixelClockKHz
		                                                  : t.pixelClockKHz - m.pixelClockKHz;
		// Within 0.5% of the table's clock: absorbs the DCCG DTO rounding
		// of a clock read back from hardware, never a different rate.
		if (static_cast<uint64_t>(diff) * 200 > m.pixelClockKHz)
			continue;
		if (best < 0 || diff < bestDiff) {
			best = static_cast<int>(i);
			bestDiff = diff;
		}
	}
	return best;
}

int ensure(Mode *modes, size_t &n, size_t cap, const Edid::DetailedTiming &t, uint8_t source) {
	if (!modes || !cap || !usable(t))
		return -1;
	if (n > cap)
		n = cap;
	int idx = match(modes, n, t);
	if (idx >= 0)
		return idx;

	uint32_t mHz = roundedMilliHz(t);
	idx = findSlot(modes, n, t, mHz);
	if (idx >= 0) {
		modes[idx].t = t;
		modes[idx].refreshMilliHz = mHz;
		modes[idx].source = source;
		return idx;
	}

	uint32_t id = 0;
	for (size_t i = 0; i < n; i++)
		if (modes[i].id > id)
			id = modes[i].id;
	id++;
	size_t slot;
	if (n < cap) {
		slot = n++;
	} else {
		// The mode the display is running must be offered; the last entry
		// is the lowest-priority one.
		slot = n - 1;
		id = modes[slot].id;
	}
	Mode &m = modes[slot];
	m = Mode {};
	m.id = id;
	m.t = t;
	m.refreshMilliHz = mHz;
	m.native = false;
	m.source = source;
	return static_cast<int>(slot);
}

} // namespace Modes
