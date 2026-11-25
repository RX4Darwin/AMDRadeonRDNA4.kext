#ifndef RDNA4_DESKTOP_TRIAL_HPP
#define RDNA4_DESKTOP_TRIAL_HPP
#include "modes.hpp"
#include "pipe2.hpp"
#include "dptrain.hpp"

namespace DesktopTrial {
constexpr uint16_t width = 2560, height = 1440;
constexpr unsigned baseHz = 60;
constexpr uint32_t standardClockKHz = 650000, raisedClockKHz = 720000;

struct Choice {
	Edid::DetailedTiming timing {};
	Modes::Source source {};
	uint8_t block {};
	unsigned hz() const { return (timing.refreshMilliHz() + 500) / 1000; }
};

// The dedicated RGB8 DML plan has only been modeled for this exact raster.
inline bool raisedClockMode(const Edid::DetailedTiming &t) {
	return Edid::sameTiming(t, {699500, width, 160, 48, 32, height, 119, 3, 5, true, false, false});
}

inline bool receiverCaps(const uint8_t (&base)[16], const uint8_t (&extended)[16], DpTrain::Sink &sink) {
	return base[0x0e] & 0x80 ? DpTrain::parseExtendedCaps(base, extended, sink)
	                        : DpTrain::parseCaps(base, sizeof(base), sink);
}

inline bool chooseRgb10Link(const DpTrain::Sink &sink, const DpTrain::Link &current,
                            uint32_t pixelClockKHz, DpTrain::Link &target) {
	// Only change link parameters on the modeled four-lane transmitter path.
	if (!pixelClockKHz || current.lanes != 4 || sink.maxLanes < current.lanes ||
	    (current.rate != DpTrain::kHbr2 && current.rate != DpTrain::kHbr3) ||
	    current.rate > sink.maxRate)
		return false;
	target = current;
	const uint64_t required = 30ull * pixelClockKHz;
	if (DpTrain::bandwidthKbps(target) < required)
		target.rate = DpTrain::kHbr3;
	return target.rate <= sink.maxRate && (target.rate != DpTrain::kHbr3 || sink.tps4) &&
	       DpTrain::bandwidthKbps(target) >= required;
}

// The caller selects the primary DP pipe's table. Filter before ranking so an
// unusable preferred timing cannot hide a usable descriptor at the same rate.
inline void chooseModes(const uint8_t *edid, size_t len, uint32_t lowMaxKHz, uint32_t fastMaxKHz,
                        bool raisedClock, Choice &low, Choice &fast) {
	low = fast = {};
	Edid::BaseInfo base {};
	if (!Edid::parseBaseBlock(edid, len, base) || !base.digital)
		return;
	auto consider = [&](const Edid::DetailedTiming &t, Modes::Source source, uint8_t block) {
		if (t.hActive != width || t.vActive != height || !Modes::usable(t))
			return;
		const Choice mode {t, source, block};
		if (mode.hz() == baseHz && !low.hz() && t.pixelClockKHz <= lowMaxKHz && Pipe2::modeKnown(t))
			low = mode;
		if (mode.hz() > baseHz && mode.hz() > fast.hz() && t.pixelClockKHz <= fastMaxKHz &&
		    ((t.pixelClockKHz <= standardClockKHz && Pipe2::modeKnown(t)) || (raisedClock && raisedClockMode(t))))
			fast = mode;
	};
	for (size_t i = 0; i < base.dtdCount; ++i)
		consider(base.dtds[i], Modes::SourceDtd, 0);
	for (uint8_t i = 1; i <= base.extensionCount && i < Edid::MaxBlocks && (i + 1u) * Edid::BlockSize <= len; ++i) {
		const uint8_t *block = edid + i * Edid::BlockSize;
		if (!Edid::blockChecksumOk(block))
			continue;
		Edid::CtaCaps cta {};
		if (Edid::parseCtaBlock(block, cta)) {
			for (size_t j = 0; j < cta.dtdCount; ++j)
				consider(cta.dtds[j], Modes::SourceCtaDtd, i);
		} else {
			Edid::DetailedTiming timings[Edid::MaxCtaDtds];
			const size_t n = Edid::displayIdTimings(block, timings, Edid::MaxCtaDtds);
			for (size_t j = 0; j < n; ++j)
				consider(timings[j], Modes::SourceDisplayId, i);
		}
	}
	if (!low.hz()) fast = {};
}

struct Work {
	ModeSet::Step change[Pipe2::kMaxModeSteps + 3], saved[Pipe2::kMaxModeSteps + 12];
};
// Both plans are self-contained after build. Capture every overwritten plane field,
// including DML's masked updates, before either plan can reach the register executor.
template <class Read>
bool build(ModeSet::Target target, ModeSet::Target undo, uint64_t mc, uint32_t pitch, uint64_t bytes, Work &work, size_t n,
           Read read, ModeSet::Plan &forward, ModeSet::Plan &back) {
	auto &change = work.change;
	auto &saved = work.saved;
	// The register snapshot below is for pipe zero. Never apply it to a second head.
	if (!target.dp || !undo.dp || target.otg || target.opp || target.hubp || target.dig || undo.otg || undo.opp ||
	    undo.hubp || undo.dig || !target.to.vActive || !pitch || pitch > 65536 || pitch < target.to.hActive ||
	    bytes < static_cast<uint64_t>(pitch) * 4 * target.to.vActive || !n || n >= Pipe2::kMaxModeSteps || !mc ||
	    (mc & 255) || (mc >> 48) || bytes > (1ull << 48) - mc)
		return false;
	auto op = [](uint32_t reg, uint32_t mask, uint32_t val) {
		return ModeSet::Step{ModeSet::Op::Update, 2, false, reg, mask, val & mask, 0, "desktop trial plane"};
	};
	change[n++] = op(0x0607, 0xffff, pitch - 1);
	change[n++] = op(0x060b, 0xffff, static_cast<uint32_t>(mc >> 32));
	change[n++] = op(0x060a, 0xffffff00, static_cast<uint32_t>(mc));
	size_t ns = 0;
	for (size_t i = 0; i < n; ++i) {
		const auto &s = change[i];
		const uint32_t old = read(s.seg, s.dword);
		if (s.op != ModeSet::Op::Update || old == 0xffffffff)
			return false;
		saved[ns] = s;
		saved[ns++].value = old & s.mask;
	}
	const uint32_t geometry[] = {0x05e9, 0x05eb, 0x063b, 0x0d19, 0x0d0a, 0x0d08, 0x0d1e, 0x0d1f, 0x0d20};
	for (uint32_t reg : geometry) {
		const uint32_t old = read(2, reg);
		if (old == 0xffffffff)
			return false;
		saved[ns++] = op(reg, 0xffffffff, old);
	}
	target.extra = change;
	target.nextra = n;
	undo.extra = saved;
	undo.nextra = ns;
	if (!ModeSet::build(undo, back, nullptr) || !ModeSet::build(target, forward, nullptr))
		return false;
	ModeSet::Plan *plans[] = {&back, &forward};
	for (auto *plan : plans) {
		for (size_t i = 0; i < plan->count; ++i)
			if (plan->steps[i].op == ModeSet::Op::WaitSet && plan->steps[i].dword == 0x1b89)
				plan->steps[i].optional = false;
	}
	return true;
}
} // namespace DesktopTrial
#endif
