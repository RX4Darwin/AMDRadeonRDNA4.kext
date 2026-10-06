//
//  dptrain.cpp
//  RDNA4FB
//
//  See dptrain.hpp. The DC function each part mirrors is named above it.
//

#include "dptrain.hpp"

namespace DpTrain {

namespace {

// DPCD addresses.
constexpr uint32_t kTrainingAuxRdInterval = 0x00e;
constexpr uint32_t kLinkBwSet             = 0x100;
constexpr uint32_t kLaneCountSet          = 0x101;   // [4:0] lanes, [7] enhanced framing
constexpr uint32_t kTrainingPatternSet    = 0x102;   // [3:0] pattern, [5] scrambling off; the lanes' drive follows
constexpr uint32_t kTrainingLane0Set      = 0x103;
constexpr uint32_t kDownspreadCtrl        = 0x107;
constexpr uint32_t kChannelCodingSet      = 0x108;
constexpr uint32_t kSinkCount             = 0x200;
constexpr uint32_t kLane01Status          = 0x202;   // .. 0x207: status, alignment, sink status, adjust requests
constexpr uint32_t kSetPower              = 0x600;

constexpr uint8_t kMaxLevel = 3;
// A lane's pre-emphasis is limited by its voltage swing (voltage_swing_to_pre_emphasis).
constexpr uint8_t kMaxPreForSwing[4] = { 3, 2, 1, 0 };

// The six bytes from LANE0_1_STATUS on.
struct Status {
	uint8_t b[6];
	uint8_t lane(uint8_t l) const { return (b[l / 2] >> (4 * (l & 1))) & 0xf; }        // [0] CR, [1] EQ, [2] symbol lock
	uint8_t swing(uint8_t l) const { return (b[4 + l / 2] >> (4 * (l & 1))) & 0x3; }
	uint8_t pre(uint8_t l) const { return (b[4 + l / 2] >> (4 * (l & 1) + 2)) & 0x3; }
	bool all(uint8_t lanes, uint8_t bits) const {
		for (uint8_t l = 0; l < lanes; l++)
			if ((lane(l) & bits) != bits)
				return false;
		return true;
	}
	bool aligned() const { return b[2] & 1; }
};

struct Trainer {
	const Io   &io;
	const Sink &sink;
	const Link &link;
	uint8_t  swing { 0 }, pre { 0 };       // every lane is driven alike (disallow_per_lane_settings)
	uint32_t crUs { 100 }, eqUs { 400 };   // what the sink wants between a change and a status read
	Status   st {};

	bool read(uint32_t address, uint8_t *data, size_t len) { return io.read(io.ctx, address, data, len); }
	bool write(uint32_t address, const uint8_t *data, size_t len) { return io.write(io.ctx, address, data, len); }
	bool write1(uint32_t address, uint8_t value) { return write(address, &value, 1); }

	// dp_hw_to_dpcd_lane_settings: the drive as TRAINING_LANEx_SET has it.
	uint8_t laneSet() const {
		return static_cast<uint8_t>(swing | (swing == kMaxLevel ? 0x04 : 0) | (pre << 3) | (pre == kMaxLevel ? 0x20 : 0));
	}

	// dp_set_hw_lane_settings, then dpcd_set_lt_pattern_and_lane_settings the
	// first time round and dpcd_set_lane_settings after; the sink's reading
	// interval; dp_get_lane_status_and_lane_adjust.
	bool round(bool first, Pattern p, uint32_t waitUs) {
		for (uint8_t l = 0; l < link.lanes; l++)
			if (!io.drive(io.ctx, l, swing, pre))
				return false;
		uint8_t buf[5];
		// TPS4 is sent scrambled; the others are not.
		buf[0] = static_cast<uint8_t>(p | (p == Tps4 ? 0 : 0x20));
		for (uint8_t l = 0; l < link.lanes; l++)
			buf[1 + l] = laneSet();
		if (!(first ? write(kTrainingPatternSet, buf, 1u + link.lanes) : write(kTrainingLane0Set, buf + 1, link.lanes)))
			return false;
		io.delayUs(io.ctx, waitUs);
		return read(kLane01Status, st.b, sizeof(st.b));
	}

	// dp_decide_lane_settings with maximize_lane_settings: the highest swing
	// and pre-emphasis any lane asks for, for all of them.
	void adjust() {
		uint8_t s = 0, p = 0;
		for (uint8_t l = 0; l < link.lanes; l++) {
			if (st.swing(l) > s)
				s = st.swing(l);
			if (st.pre(l) > p)
				p = st.pre(l);
		}
		swing = s;
		pre = p > kMaxPreForSwing[s] ? kMaxPreForSwing[s] : p;
	}

	// dp_enable_link_phy and dp_perform_link_training: one attempt.
	Result attempt() {
		swing = pre = 0;
		if (!io.phyOn(io.ctx, link))
			return Result::Io;
		if (!write1(kSetPower, 0x01))                       // D0
			return Result::Io;

		// decide_8b_10b_training_settings: the reading intervals, for
		// equalisation and for clock recovery.
		crUs = 100;
		eqUs = 400;
		if (sink.rev >= 0x12) {
			uint8_t interval = 0;
			if (!read(kTrainingAuxRdInterval, &interval, 1))
				return Result::Io;
			const uint8_t n = interval & 0x7f;
			eqUs = n >= 1 && n <= 4 ? n * 4000u : n == 5 ? 32000 : n == 6 ? 64000 : 400;
			if (!read(kTrainingAuxRdInterval, &interval, 1))
				return Result::Io;
			if (interval)
				crUs = (interval & 0x7f) ? (interval & 0x7f) * 4000u : 400;
		}

		// dpcd_exit_training_mode, dpcd_configure_channel_coding, dpcd_set_link_settings
		if (!write1(kTrainingPatternSet, 0) || !write1(kChannelCodingSet, 0x01) ||   // 8b/10b
		    !write1(kDownspreadCtrl, 0x10) ||                                       // 0.5 % down-spread
		    !write1(kLaneCountSet, static_cast<uint8_t>(link.lanes | 0x80)) ||      // enhanced framing
		    !write1(kLinkBwSet, link.rate))
			return Result::Io;

		Result r = clockRecovery();
		if (r == Result::Ok)
			r = equalisation();
		if (r == Result::Io || !write1(kTrainingPatternSet, 0))   // dpcd_exit_training_mode
			return Result::Io;
		if (r != Result::Ok)
			return r;

		// dp_transition_to_video_idle, dp_check_link_loss_status
		if (!io.pattern(io.ctx, Video))
			return Result::Io;
		io.delayUs(io.ctx, 5000);
		uint8_t after[6];
		if (!read(kSinkCount, after, sizeof(after)))
			return Result::Io;
		for (uint8_t i = 0; i < 4; i++)
			st.b[i] = after[2 + i];                         // the same layout from LANE0_1_STATUS on
		return st.all(link.lanes, 0x7) && st.aligned() ? Result::Ok : Result::LinkLost;
	}

	// perform_8b_10b_clock_recovery_sequence
	Result clockRecovery() {
		if (!io.pattern(io.ctx, Tps1))
			return Result::Io;
		// Five rounds at one voltage swing, a hundred in all.
		for (uint32_t same = 0, rounds = 0; same < 5 && rounds < 100; rounds++) {
			if (!round(rounds == 0, Tps1, crUs))
				return Result::Io;
			if (st.all(link.lanes, 0x1))
				return Result::Ok;
			if (swing == kMaxLevel)
				break;
			same = st.swing(0) == swing ? same + 1 : 0;
			adjust();
		}
		return Result::ClockRecovery;
	}

	// perform_8b_10b_channel_equalization_sequence
	Result equalisation() {
		const Pattern p = sink.tps4 ? Tps4 : sink.tps3 ? Tps3 : Tps2;   // the DCN 4.01 encoder sends all of them
		if (!io.pattern(io.ctx, p))
			return Result::Io;
		for (uint32_t tries = 0; tries <= 5; tries++) {
			if (!round(tries == 0, p, eqUs))
				return Result::Io;
			if (!st.all(link.lanes, 0x1))
				return Result::ClockLost;
			if (st.all(link.lanes, 0x6) && st.aligned())
				return Result::Ok;
			adjust();
		}
		return Result::Equalisation;
	}
};

} // namespace

const char *resultName(Result r) {
	switch (r) {
	case Result::Ok:            return "trained";
	case Result::BadLink:       return "not a link of this sink";
	case Result::Io:            return "hardware or AUX failure";
	case Result::ClockRecovery: return "no clock recovery";
	case Result::Equalisation:  return "no channel equalisation";
	case Result::ClockLost:     return "clock recovery lost during equalisation";
	case Result::LinkLost:      return "link lost after training";
	}
	return "?";
}

bool parseCaps(const uint8_t *caps, size_t len, Sink &out) {
	if (!caps || len < 4)
		return false;
	out.rev = caps[0];
	out.maxRate = caps[1];
	out.maxLanes = caps[2] & 0x1f;
	out.tps3 = caps[2] & 0x40;
	out.tps4 = caps[3] & 0x80;
	const bool rate = out.maxRate == kRbr || out.maxRate == kHbr || out.maxRate == kHbr2 || out.maxRate == kHbr3;
	const bool lanes = out.maxLanes == 1 || out.maxLanes == 2 || out.maxLanes == 4;
	return out.rev >= 0x10 && rate && lanes;
}

bool pickLink(const Sink &sink, uint32_t pixelClockKHz, uint32_t bitsPerPixel, Link &out) {
	static constexpr uint8_t rates[] = { kRbr, kHbr, kHbr2, kHbr3 };
	const uint64_t need = static_cast<uint64_t>(pixelClockKHz) * bitsPerPixel;
	for (uint8_t rate : rates) {
		if (rate > sink.maxRate)
			break;
		for (uint8_t lanes = 1; lanes <= sink.maxLanes; lanes *= 2) {
			const Link l { lanes, rate };
			if (bandwidthKbps(l) >= need) {
				out = l;
				return true;
			}
		}
	}
	return false;
}

// perform_link_training_with_retries
Result bringUp(const Io &io, const Sink &sink, const Link &link, Report *report) {
	Report dummy;
	Report &rep = report ? *report : dummy;
	rep = Report {};
	const bool lanes = link.lanes == 1 || link.lanes == 2 || link.lanes == 4;
	if (!lanes || link.lanes > sink.maxLanes || link.rate < kRbr || link.rate > sink.maxRate)
		return rep.result = Result::BadLink;

	Trainer t { io, sink, link };
	uint32_t pauseMs = 50;
	for (rep.attempts = 1;; rep.attempts++) {
		rep.result = t.attempt();
		rep.swing = t.swing;
		rep.preEmphasis = t.pre;
		for (size_t i = 0; i < sizeof(rep.status); i++)
			rep.status[i] = t.st.b[i];
		// A sink that stopped answering will not start again by itself.
		if (rep.result == Result::Ok || rep.result == Result::Io || rep.attempts == kAttempts)
			return rep.result;
		// dp_disable_link_phy: the sink to D3 (amdgpu does not look at whether
		// it took that), the transmitter off.
		uint8_t d3 = 0x02;
		io.write(io.ctx, kSetPower, &d3, 1);
		if (!io.phyOff(io.ctx))
			return rep.result = Result::Io;
		pauseMs += 50;
		io.delayUs(io.ctx, pauseMs * 1000);
	}
}

} // namespace DpTrain
