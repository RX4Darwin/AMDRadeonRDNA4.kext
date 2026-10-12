//
//  dpphy.cpp
//  RDNA4FB
//
//  See dpphy.hpp. Register offsets are dcn_4_1_0_offset.h (DP0's, base_idx 2;
//  one block per link encoder), fields dcn_4_1_0_sh_mask.h.
//

#include "dpphy.hpp"

namespace DpPhy {

namespace {

constexpr uint32_t kStride = 0x124;
constexpr uint32_t kLinkCntl           = 0x211e;   // DP_LINK_TRAINING_COMPLETE [4]
constexpr uint32_t kConfig             = 0x2121;   // DP_UDI_LANES [1:0] = lanes - 1
constexpr uint32_t kDphyInternalCtrl   = 0x2125;   // the alternative scrambler reset of an eDP panel: none
constexpr uint32_t kLinkFramingCntl    = 0x2129;   // IDLE_BS_INTERVAL [17:0], VBID_DISABLE [24], ENHANCED_FRAME_MODE [28]
constexpr uint32_t kDphyCntl           = 0x212d;   // DPHY_BYPASS [16]
constexpr uint32_t kDphyTrainingPattern = 0x212e;  // 0..3 = TPS1..TPS4
constexpr uint32_t kDphyPrbsCntl       = 0x2133;   // DPHY_PRBS_EN [0]
constexpr uint32_t kDphyScramCntl      = 0x2134;   // SCRAMBLER_ADVANCE [4], SCRAMBLER_BS_COUNT [17:8]

struct Steps {
	ModeSet::Step *out;
	uint32_t       base;
	size_t         n { 0 };
	void write(uint32_t reg, uint32_t value, const char *what) {
		out[n++] = ModeSet::Step { ModeSet::Op::Write, 2, false, reg + base, 0, value, 0, what };
	}
	void update(uint32_t reg, uint32_t mask, uint32_t value, const char *what) {
		out[n++] = ModeSet::Step { ModeSet::Op::Update, 2, false, reg + base, mask, value, 0, what };
	}
	// set_link_training_complete, enable_phy_bypass_mode(false), disable_prbs_mode
	void sendIt(bool trained) {
		update(kLinkCntl, 1u << 4, trained ? 1u << 4 : 0, "dp link training complete");
		update(kDphyCntl, 1u << 16, 0, "dp phy bypass off");
		update(kDphyPrbsCntl, 1u << 0, 0, "dp prbs off");
	}
};

Dmub::TransmitterControl transmitter(uint8_t link, uint8_t hpd, uint8_t action) {
	Dmub::TransmitterControl tx {};
	tx.phyId = link;
	tx.action = action;
	tx.digMode = Dmub::EncoderModeDp;
	tx.hpdSel = hpd;
	return tx;
}

} // namespace

// enc1_configure_encoder
size_t configureSteps(uint8_t link, uint8_t lanes, ModeSet::Step *out) {
	Steps s { out, link * kStride };
	s.write(kConfig, lanes - 1u, "dp lane count");
	s.update(kDphyScramCntl, 1u << 4, 1u << 4, "dp scrambler advance");
	return s.n;
}

// dcn10_link_encoder_dp_set_phy_pattern: set_dp_phy_pattern_training_pattern,
// or for video set_dp_phy_pattern_passthrough_mode
size_t patternSteps(uint8_t link, DpTrain::Pattern p, ModeSet::Step *out) {
	Steps s { out, link * kStride };
	if (p == DpTrain::Video) {
		s.write(kDphyInternalCtrl, 0, "dp panel mode");
		// what a compliance pattern may have changed: back to enhanced framing
		// with a BS every 0x2000 symbols, and the scrambler reset every 512th
		s.update(kLinkFramingCntl, 0x1103ffff, 0x10002000, "dp link framing");
		s.update(kDphyScramCntl, 0x3ffu << 8, 0x1ffu << 8, "dp scrambler bs count");
		s.sendIt(true);
		return s.n;
	}
	s.write(kDphyTrainingPattern, p == DpTrain::Tps4 ? 3u : p - 1u, "dp training pattern");
	s.sendIt(false);
	return s.n;
}

// link_encoder_disable, then the panel mode for the next attempt
size_t offSteps(uint8_t link, ModeSet::Step *out) {
	Steps s { out, link * kStride };
	s.write(kDphyTrainingPattern, 0, "dp training pattern");
	s.update(kLinkCntl, 1u << 4, 0, "dp link training complete");
	s.write(kDphyInternalCtrl, 0, "dp panel mode");
	return s.n;
}

size_t streamSteps(uint8_t dig, uint8_t link, ModeSet::Step *out) {
	constexpr uint32_t kDigFeClkCntl = 0x2094, kDigFeEnCntl = 0x2095;                       // per stream encoder
	constexpr uint32_t kDigBeClkCntl = 0x20bb, kDigBeCntl = 0x20bc, kDigBeEnCntl = 0x20bd;   // per link encoder
	constexpr uint32_t kStreamMapper = 0x1f0d, kSymclkEnable = 0x00a0;                       // + dig; the second base_idx 1
	const uint32_t fe = dig * kStride, be = link * kStride;
	size_t n = 0;
	auto update = [&](uint8_t seg, uint32_t reg, uint32_t mask, uint32_t value, const char *what) {
		out[n++] = ModeSet::Step { ModeSet::Op::Update, seg, false, reg, mask, value, 0, what };
	};
	update(2, kDigBeClkCntl + be, 0x7, 0, "dig be dp mode");
	update(2, kDigBeClkCntl + be, 0x10, 0x10, "dig be clock on");
	update(2, kDigBeEnCntl + be, 0x1, 0x1, "dig be on");
	update(1, kSymclkEnable + dig, 0x710, (static_cast<uint32_t>(link) << 8) | 0x10, "stream encoder symbol clock from the link");
	update(2, kDigBeCntl + be, 0x100u << dig, 0x100u << dig, "dig be fed by the stream encoder");
	update(2, kDigFeClkCntl + fe, 0x17, 0x10, "dig fe clock on");
	update(2, kDigFeEnCntl + fe, 0x1, 0x1, "dig fe on");
	update(2, kStreamMapper + dig, 0x7, link, "stream encoder mapped to the link");
	return n;
}

// dcn10_link_encoder_enable_dp_output
void buildEnable(Dmub::Cmd cmd, uint8_t link, uint8_t hpd, const DpTrain::Link &l) {
	Dmub::TransmitterControl tx = transmitter(link, hpd, Dmub::TransmitterActionEnable);
	tx.laneNum = l.lanes;
	tx.symclk10kHz = DpTrain::symbolClockKHz(l.rate) / 10;
	Dmub::buildTransmitterControl(cmd, tx);
}

// dcn10_link_encoder_dp_set_lane_settings
void buildDrive(Dmub::Cmd cmd, uint8_t link, uint8_t hpd, uint8_t connectorObjId, const DpTrain::Link &l,
                uint8_t swing, uint8_t preEmphasis) {
	Dmub::TransmitterControl tx = transmitter(link, hpd, kActionSetDrive);
	// For this action the mode byte is the drive, as TRAINING_LANEx_SET has it.
	tx.digMode = static_cast<uint8_t>((swing & 3) | ((preEmphasis & 3) << 3));
	tx.laneNum = l.lanes;
	tx.symclk10kHz = DpTrain::symbolClockKHz(l.rate) / 10;
	tx.connObjId = connectorObjId;
	Dmub::buildTransmitterControl(cmd, tx);
}

// dcn10_link_encoder_disable_output
void buildDisable(Dmub::Cmd cmd, uint8_t link, uint8_t hpd, uint8_t connectorObjId) {
	Dmub::TransmitterControl tx = transmitter(link, hpd, Dmub::TransmitterActionDisable);
	tx.connObjId = connectorObjId;
	Dmub::buildTransmitterControl(cmd, tx);
}

} // namespace DpPhy
