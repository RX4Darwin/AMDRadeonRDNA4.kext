//
//  pipe.cpp
//  RDNA4FB
//

#include "pipe.hpp"

namespace Pipe {

namespace {

constexpr uint32_t kUnreadable = 0xFFFFFFFF;

struct Reader {
	ReadFn rd;
	void  *ctx;
	uint32_t operator()(uint8_t baseIdx, uint32_t dword) const { return rd(ctx, baseIdx, dword); }
};

} // namespace

bool discover(ReadFn rd, void *ctx, State &out) {
	out = State {};
	if (!rd)
		return false;
	Reader r { rd, ctx };

	// The lit OTG: master enabled and actually running. The GOP drives one
	// display, so the first such OTG is the boot pipe.
	for (uint8_t i = 0; i < kMaxOtg; i++) {
		uint32_t ctl = r(2, Reg::kOtgControl + i * Reg::kOtgStride);
		if (ctl == kUnreadable)
			continue;
		if ((ctl & 0x1) && (ctl & (1u << 16))) {
			out.otg = i;
			break;
		}
	}
	if (out.otg == kNone)
		return false;
	const uint32_t o = out.otg * Reg::kOtgStride;

	out.hTotal   = r(2, Reg::kOtgHTotal + o);
	out.hBlank   = r(2, Reg::kOtgHBlank + o);
	out.hSync    = r(2, Reg::kOtgHSyncA + o);
	out.hSyncNeg = r(2, Reg::kOtgHSyncACntl + o) & 0x1;
	out.vTotal   = r(2, Reg::kOtgVTotal + o);
	out.vBlank   = r(2, Reg::kOtgVBlank + o);
	out.vSync    = r(2, Reg::kOtgVSyncA + o);
	out.vSyncNeg = r(2, Reg::kOtgVSyncACntl + o) & 0x1;

	// DIG front-end sourcing this OTG. An enabled FE wins over a stale one
	// that merely still points at the same OTG.
	for (uint8_t d = 0; d < kMaxDig; d++) {
		const uint32_t base = d * Reg::kDigStride;
		uint32_t fe = r(2, Reg::kDigFeCntl + base);
		if (fe == kUnreadable || (fe & 0x7) != out.otg)
			continue;
		bool enabled = r(2, Reg::kDigFeEnCntl + base) & 0x1;
		if (out.dig == kNone || enabled)
			out.dig = d;
		if (enabled)
			break;
	}

	if (out.dig != kNone) {
		const uint32_t fe = out.dig * Reg::kDigStride;
		uint32_t map = r(2, Reg::kStreamMapper + out.dig);
		out.link = (map == kUnreadable) ? out.dig : static_cast<uint8_t>(map & 0x7);

		uint32_t feClk = r(2, Reg::kDigFeClkCntl + fe);
		Signal feMode = feClk == kUnreadable ? Signal::Unknown
		                                     : static_cast<Signal>(feClk & 0x7);
		out.signal = feMode;

		// The back-end's own mode is authoritative when its block exists and
		// really carries this front-end (DIG_FE_SOURCE_SELECT bitmask).
		if (out.link < kMaxDig) {
			const uint32_t be = out.link * Reg::kDigStride;
			uint32_t beCntl = r(2, Reg::kDigBeCntl + be);
			uint32_t beClk  = r(2, Reg::kDigBeClkCntl + be);
			if (beCntl != kUnreadable && beClk != kUnreadable &&
			    (beCntl & (1u << (8 + out.dig)))) {
				out.signal = static_cast<Signal>(beClk & 0x7);
				// DIG_HPD_SELECT counts from 0 (0 = HPD1, Linux's hpd_source_id;
				// 6 and 7 are not sources). The VBIOS path records and the DMUB
				// transmitter command count pins from 1. Both card logs show the
				// field one below the lit sink's pin: 3 for hpd-pin 4 (HDMI,
				// 2026-09-28), 0 for hpd-pin 1 (DP, 2026-10-03).
				const uint32_t sel = (beCntl >> 28) & 0x7;
				out.hpd    = sel < 6 ? static_cast<uint8_t>(sel + 1) : 0;
			}
		}
		switch (out.signal) {
		case Signal::DpSst: case Signal::Dvi: case Signal::Hdmi: case Signal::DpMst:
			break;
		default:
			out.signal = Signal::Unknown;
		}
	}

	// OTG <- ODM segment 0 <- OPP <- MPCC tree. MPCC n is fed by DPP/HUBP n
	// in every single-plane configuration the GOP sets up.
	uint32_t src = r(2, Reg::kOptcDataSource + out.otg * Reg::kOdmStride);
	if (src != kUnreadable)
		out.opp = static_cast<uint8_t>((src >> 16) & 0xf);
	if (out.opp < kMaxOtg) {
		for (uint8_t m = 0; m < kMaxOtg; m++) {
			uint32_t id = r(3, Reg::kMpccOppId + m * Reg::kMpccStride);
			if (id != kUnreadable && (id & 0xf) == out.opp) {
				out.hubp = m;
				break;
			}
		}
	}
	if (out.hubp == kNone)
		out.hubp = out.otg;

	const uint32_t h = out.hubp * Reg::kHubpStride;
	uint32_t vp = r(2, Reg::kHubpViewportDim + h);
	if (vp != kUnreadable) {
		out.viewportW = vp & 0x3fff;
		out.viewportH = (vp >> 16) & 0x3fff;
	}
	uint32_t pitch = r(2, Reg::kHubpSurfacePitch + h);
	if (pitch != kUnreadable)
		out.pitchPx = (pitch & 0xffff) + 1;
	return true;
}

bool timingFromOtg(const State &s, Edid::DetailedTiming &t) {
	t = Edid::DetailedTiming {};
	if (!s.valid())
		return false;

	// OtgTiming::compute: counters start at the sync pulse, so
	//   blank_start = total - front_porch, blank_end = blank_start - active.
	uint32_t hTotal      = (s.hTotal & 0x7fff) + 1;
	uint32_t hBlankStart = s.hBlank & 0x7fff;
	uint32_t hBlankEnd   = (s.hBlank >> 16) & 0x7fff;
	uint32_t hSyncW      = ((s.hSync >> 16) & 0x7fff) - (s.hSync & 0x7fff);
	uint32_t vTotal      = (s.vTotal & 0x7fff) + 1;
	uint32_t vBlankStart = s.vBlank & 0x7fff;
	uint32_t vBlankEnd   = (s.vBlank >> 16) & 0x7fff;
	uint32_t vSyncW      = ((s.vSync >> 16) & 0x7fff) - (s.vSync & 0x7fff);

	if (hBlankStart <= hBlankEnd || hBlankStart > hTotal ||
	    vBlankStart <= vBlankEnd || vBlankStart > vTotal)
		return false;
	uint32_t hActive = hBlankStart - hBlankEnd;
	uint32_t vActive = vBlankStart - vBlankEnd;
	uint32_t hFront  = hTotal - hBlankStart;
	uint32_t vFront  = vTotal - vBlankStart;
	if (hActive >= hTotal || vActive >= vTotal ||
	    hFront + hSyncW > hTotal - hActive || vFront + vSyncW > vTotal - vActive)
		return false;

	t.hActive       = static_cast<uint16_t>(hActive);
	t.hBlank        = static_cast<uint16_t>(hTotal - hActive);
	t.hSyncOffset   = static_cast<uint16_t>(hFront);
	t.hSyncWidth    = static_cast<uint16_t>(hSyncW);
	t.vActive       = static_cast<uint16_t>(vActive);
	t.vBlank        = static_cast<uint16_t>(vTotal - vActive);
	t.vSyncOffset   = static_cast<uint16_t>(vFront);
	t.vSyncWidth    = static_cast<uint16_t>(vSyncW);
	// OTG POL=1 means negative (see OtgTiming::compute).
	t.hSyncPositive = !s.hSyncNeg;
	t.vSyncPositive = !s.vSyncNeg;
	t.interlaced    = false;
	return true;
}

uint32_t pixelClockFromFramePeriod(const State &s, uint64_t framePeriodNs) {
	if (!s.valid() || framePeriodNs == 0)
		return 0;
	uint64_t pixels = static_cast<uint64_t>((s.hTotal & 0x7fff) + 1) * ((s.vTotal & 0x7fff) + 1);
	// kHz = pixels / period_s / 1000 = pixels * 1e6 / period_ns
	return static_cast<uint32_t>((pixels * 1000000ULL + framePeriodNs / 2) / framePeriodNs);
}

const char *signalName(Signal s) {
	switch (s) {
	case Signal::DpSst:   return "DP";
	case Signal::Dvi:     return "DVI";
	case Signal::Hdmi:    return "HDMI";
	case Signal::DpMst:   return "DP-MST";
	case Signal::None:    return "none";
	default:              return "unknown";
	}
}

} // namespace Pipe
