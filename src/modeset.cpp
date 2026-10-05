//
//  modeset.cpp
//  RDNA4FB
//
//  See modeset.hpp. Register offsets are dcn_4_1_0_offset.h (instance 0,
//  base_idx 2 unless noted), fields dcn_4_1_0_sh_mask.h; the DC function
//  each group mirrors is named above it.
//

#include "modeset.hpp"
#include "otgtiming.hpp"

namespace ModeSet {

namespace {

// Instance strides (dwords).
constexpr uint32_t kOtgStride  = 0x80;
constexpr uint32_t kOdmStride  = 0x10;
constexpr uint32_t kOppStride  = 0x5a;
constexpr uint32_t kHubpStride = 0xdc;
constexpr uint32_t kDppStride  = 0x16b;
constexpr uint32_t kDigStride  = 0x124;

// OTG
constexpr uint32_t kOtgHTotal        = 0x1b2a;
constexpr uint32_t kOtgHBlank        = 0x1b2b;
constexpr uint32_t kOtgHSyncA        = 0x1b2c;
constexpr uint32_t kOtgHSyncACntl    = 0x1b2d;
constexpr uint32_t kOtgHTimingCntl   = 0x1b2e;   // DIV_MODE [1:0], DIV_MODE_MANUAL [8]
constexpr uint32_t kOtgVTotal        = 0x1b2f;
constexpr uint32_t kOtgVTotalMin     = 0x1b30;
constexpr uint32_t kOtgVTotalMax     = 0x1b31;
constexpr uint32_t kOtgVTotalControl = 0x1b33;
constexpr uint32_t kOtgVBlank        = 0x1b38;
constexpr uint32_t kOtgVSyncA        = 0x1b39;
constexpr uint32_t kOtgVSyncACntl    = 0x1b3a;
constexpr uint32_t kOtgControl       = 0x1b43;   // MASTER_EN [0], DISABLE_POINT_CNTL [9:8],
                                                 // CURRENT_MASTER_EN_STATE [16], OUT_MUX [21:20]
constexpr uint32_t kOtgInterlace     = 0x1b45;
constexpr uint32_t kOtgClockControl  = 0x1b84;   // EN [0], GATE_DIS [1], CLOCK_ON [8], BUSY [16]
constexpr uint32_t kOtgVStartup      = 0x1b85;   // VSTARTUP_START [9:0]
constexpr uint32_t kOtgVUpdate       = 0x1b86;   // OFFSET [15:0], WIDTH [25:16]
constexpr uint32_t kOtgUpdateLock    = 0x1b89;   // LOCK [0], UPDATE_LOCK_STATUS [8]
constexpr uint32_t kOtgGlobalCtrl2   = 0x1b90;   // MASTER_UPDATE_LOCK_SEL [27:25]
// OPTC / ODM
constexpr uint32_t kOptcDataSource   = 0x1acb;   // SEG0..3_SRC_SEL [19:16..31:28], NUM_SEG [1:0]
constexpr uint32_t kOptcDataFormat   = 0x1acc;
constexpr uint32_t kOptcInputClock   = 0x1ad0;   // GATE_DIS [0], EN [1], CLK_ON [2]
constexpr uint32_t kOptcMemoryConfig = 0x1ad1;
constexpr uint32_t kVtgControl       = 0x0530;   // + otg: VCOUNT_INIT [30:16], FP2 [14:0], ENABLE [31]
// OPP display pattern generator
constexpr uint32_t kDpgControl       = 0x1854;   // EN [0], MODE [6:4]
constexpr uint32_t kDpgRamp          = 0x1855;
constexpr uint32_t kDpgDimensions    = 0x1856;   // WIDTH [29:16], HEIGHT [13:0]
constexpr uint32_t kDpgColourRCr     = 0x1857;
constexpr uint32_t kDpgColourGY      = 0x1858;
constexpr uint32_t kDpgColourBCb     = 0x1859;
constexpr uint32_t kDpgOffsetSegment = 0x185a;
constexpr uint32_t kDpgStatus        = 0x185b;   // DOUBLE_BUFFER_PENDING [0]
// HUBP / DPP
constexpr uint32_t kHubpViewportStart = 0x05e9;
constexpr uint32_t kHubpViewportDim   = 0x05eb;  // WIDTH [13:0], HEIGHT [29:16]
constexpr uint32_t kDsclRecoutStart   = 0x0d1e;
constexpr uint32_t kDsclRecoutSize    = 0x0d1f;  // WIDTH [13:0], HEIGHT [29:16]
constexpr uint32_t kDsclMpcSize       = 0x0d20;
// DIG front-/back-end
constexpr uint32_t kDigFeCntl     = 0x2093;      // SOURCE_SELECT [2:0]
constexpr uint32_t kDigFeClkCntl  = 0x2094;      // FE_MODE [2:0], FE_CLK_EN [4]
constexpr uint32_t kDigFeEnCntl   = 0x2095;      // FE_ENABLE [0]
constexpr uint32_t kDigFifoCtrl0  = 0x209b;      // ENABLE [0], RESET [1], READ_START_LEVEL [6:2],
                                                 // OUTPUT_PIXEL_PER_CYCLE [9:8], RESET_DONE [20]
constexpr uint32_t kHdmiControl   = 0x209e;      // DATA_SCRAMBLE_EN [1], CLOCK_CHANNEL_RATE [2]
constexpr uint32_t kHdmiGc        = 0x20a8;      // AVMUTE [0]
constexpr uint32_t kDigBeClkCntl  = 0x20bb;      // BE_MODE [2:0], BE_CLK_EN [4]
constexpr uint32_t kDigBeCntl     = 0x20bc;      // FE_SOURCE_SELECT [14:8]
constexpr uint32_t kDigBeEnCntl   = 0x20bd;      // BE_ENABLE [0]
constexpr uint32_t kStreamMapper  = 0x1f0d;      // + dig: LINK_TARGET [2:0]
// DP stream encoder (same instance stride as the DIG)
constexpr uint32_t kDpVidStreamCntl = 0x2122;    // ENABLE [0], DIS_DEFER [9:8], STATUS [16]
constexpr uint32_t kDpSteerFifo     = 0x2123;    // ENABLE [0], RESET [1]
constexpr uint32_t kDpVidTiming     = 0x2126;    // M_N_GEN_EN [8]
constexpr uint32_t kDpVidN          = 0x2127;
constexpr uint32_t kDpVidM          = 0x2128;
constexpr uint32_t kDpMsaParam1     = 0x2162;    // HTOTAL [31:16], VTOTAL [15:0]; PARAM2..4 follow
// DCCG (base_idx 1)
constexpr uint32_t kOtgPixelRateDiv = 0x006f;    // DPDTOn_INT [4:1] + 5 per OTG
constexpr uint32_t kDpDtoPhase      = 0x0081;    // + 4 per OTG
constexpr uint32_t kDpDtoModulo     = 0x0082;

constexpr uint32_t kModeHdmi      = 3;           // DIG_FE/BE_MODE, atom digmode
constexpr uint32_t kDpgModeBars   = 4;           // TEST_PATTERN_MODE_HORIZONTALBARS (solid colour)
constexpr uint32_t kMs            = 1000;        // wait budgets are in microseconds

struct Builder {
	Plan &p;
	bool full { false };
	uint8_t seg { 2 };   // the DMU segment of the steps being added

	void add(Op op, uint32_t dword, uint32_t mask, uint32_t value, uint32_t arg,
	         const char *what, bool optional = false) {
		if (p.count >= kMaxSteps) {
			full = true;
			return;
		}
		p.steps[p.count++] = Step { op, seg, optional, dword, mask, value, arg, what };
	}
	void delay(uint32_t us, const char *what) { add(Op::Delay, 0, 0, 0, us, what); }
	void write(uint32_t dw, uint32_t v, const char *what) { add(Op::Write, dw, 0, v, 0, what); }
	void update(uint32_t dw, uint32_t mask, uint32_t v, const char *what) {
		add(Op::Update, dw, mask, v & mask, 0, what);
	}
	void waitSet(uint32_t dw, uint32_t mask, uint32_t us, const char *what, bool optional = false) {
		add(Op::WaitSet, dw, mask, 0, us, what, optional);
	}
	void waitClear(uint32_t dw, uint32_t mask, uint32_t us, const char *what, bool optional = false) {
		add(Op::WaitClear, dw, mask, 0, us, what, optional);
	}
	void frames(uint32_t n, const char *what) { add(Op::WaitFrames, 0, 0, 0, n, what); }
	Dmub::Cmd *dmub(const char *what) {
		if (p.ncmds >= kMaxCmds) {
			full = true;
			return nullptr;
		}
		add(Op::Dmub, 0, 0, 0, static_cast<uint32_t>(p.ncmds), what);
		return &p.cmds[p.ncmds++];
	}
};

constexpr uint32_t sizeWH(uint32_t w, uint32_t h) {          // WIDTH [13:0], HEIGHT [29:16]
	return (w & 0x3fff) | ((h & 0x3fff) << 16);
}
constexpr uint32_t dpgDims(uint32_t w, uint32_t h) {         // WIDTH [29:16], HEIGHT [13:0]
	return ((w & 0x3fff) << 16) | (h & 0x3fff);
}

} // namespace

bool scaleDpDto(const DpDto &now, uint32_t fromKHz, uint32_t toKHz, DpDto &out) {
	if (!now.modulo || !fromKHz || !toKHz)
		return false;
	const uint64_t total = static_cast<uint64_t>(now.integer) * now.modulo + now.phase;
	const uint64_t fromHz = static_cast<uint64_t>(fromKHz) * 1000;
	const uint64_t off = total > fromHz ? total - fromHz : fromHz - total;
	// Within half a percent of the pixel clock in Hz: programmed the way amdgpu does.
	const uint64_t next = off * 200 <= fromHz ? static_cast<uint64_t>(toKHz) * 1000
	                                          : (total * toKHz + fromKHz / 2) / fromKHz;
	out.modulo = now.modulo;
	out.integer = static_cast<uint32_t>(next / now.modulo);
	out.phase = static_cast<uint32_t>(next % now.modulo);
	return out.integer <= 0xf;
}

uint32_t vstartupLines(const Edid::DetailedTiming &to) {
	// amdgpu places VSTARTUP from DML (prefetch time); without DML keep it
	// just inside the vertical blank, 2 lines clear of its start.
	uint32_t lines = to.vBlank > 4 ? to.vBlank - 2u : 2u;
	return lines > 0x3ff ? 0x3ff : lines;
}

bool build(const Target &t, Plan &out, const char **why) {
	const char *dummy;
	const char *&err = why ? *why : dummy;
	out.count = 0;
	out.ncmds = 0;

	if (t.otg >= 4 || t.dig >= 4 || t.link >= 6 || t.opp >= 4 || t.hubp >= 4) {
		err = "pipe instance out of range";
		return false;
	}
	OtgTiming::Regs rg {};
	if (!OtgTiming::compute(t.to, rg) || t.to.interlaced) {
		err = "target timing not programmable";
		return false;
	}
	if (t.dp) {
		// More pixels per second than the link was trained for may not fit it.
		if (t.to.pixelClockKHz == 0 || t.to.pixelClockKHz > t.dpMaxKHz) {
			err = "DisplayPort: pixel clock above the one the link was brought up for";
			return false;
		}
		if (!t.dto.modulo || t.dto.integer > 0xf) {
			err = "DisplayPort: no pixel-rate DTO for the target";
			return false;
		}
	} else if (t.to.pixelClockKHz == 0 || t.to.pixelClockKHz > kMaxTmdsKHz) {
		err = "pixel clock outside HDMI TMDS (<= 600 MHz)";
		return false;
	} else if (t.to.pixelClockKHz > kScrambleFromKHz && !t.sinkScdc) {
		err = "above 340 MHz needs a sink with SCDC";
		return false;
	}

	Builder b { out };
	const uint32_t otg  = t.otg * kOtgStride;
	const uint32_t odm  = t.otg * kOdmStride;
	const uint32_t opp  = t.opp * kOppStride;
	const uint32_t hubp = t.hubp * kHubpStride;
	const uint32_t dpp  = t.hubp * kDppStride;
	const uint32_t dig  = t.dig * kDigStride;
	const uint32_t w = t.to.hActive, h = t.to.vActive;
	const uint32_t symclk10kHz = t.to.pixelClockKHz / 10;   // TMDS 8 bpc: symbol clock = pixel clock

	// --- opp2_set_disp_pattern_generator: solid black at the old size ---
	b.write(kDpgColourRCr + opp, 0, "dpg colour r");
	b.write(kDpgColourGY + opp, 0, "dpg colour g");
	b.write(kDpgColourBCb + opp, 0, "dpg colour b");
	b.write(kDpgDimensions + opp, dpgDims(t.from.hActive, t.from.vActive), "dpg dimensions (old)");
	b.write(kDpgOffsetSegment + opp, 0, "dpg offset segment");
	b.update(kDpgControl + opp, 0x71, (kDpgModeBars << 4) | 1, "dpg blank");

	if (t.dp) {
		// --- dce110_blank_stream: enc1_stream_encoder_dp_blank, then idle pattern for the sink ---
		b.update(kDpVidStreamCntl + dig, 0x300, 0x200, "dp stream off at the next vblank");
		b.update(kDpVidStreamCntl + dig, 0x1, 0, "dp video stream off");
		b.waitClear(kDpVidStreamCntl + dig, 1u << 16, 100200, "dp stream idle", true);
		b.update(kDpSteerFifo + dig, 0x2, 0x2, "dp steer fifo reset");
		b.delay(60 * kMs, "idle pattern");
	} else {
		// --- dcn30_set_avmute(true), then let the sink see it for 3 frames ---
		b.update(kHdmiGc + dig, 0x1, 0x1, "hdmi avmute on");
		b.frames(3, "avmute settle");

		// --- dce110_disable_stream: stream encoder off ---
		b.update(kDigFifoCtrl0 + dig, 0x1, 0, "dig fifo disable");
		b.write(kDigFeEnCntl + dig, 0, "dig fe disable");
		b.update(kDigFeClkCntl + dig, 0x10, 0, "dig fe clock off");
		b.update(kDigBeCntl + t.link * kDigStride, 1u << (8 + t.dig), 0, "dig be disconnect fe");
	}

	// --- optc401_disable_crtc ---
	b.write(kOptcDataSource + odm, 0xffff0000, "optc data source none");
	b.update(kOptcMemoryConfig + odm, 0xffff, 0, "optc memory config");
	b.update(kOtgControl + otg, 0x1, 0, "otg master disable");
	b.update(kVtgControl + t.otg, 1u << 31, 0, "vtg disable");
	b.waitClear(kOtgControl + otg, 1u << 16, 150 * kMs, "otg stopped");
	b.waitClear(kOtgClockControl + otg, 1u << 16, 150 * kMs, "otg not busy");
	// --- optc1_enable_optc_clock(false) ---
	b.update(kOtgClockControl + otg, 0x3, 0, "otg clock off");
	b.update(kOptcInputClock + odm, 0x3, 0, "optc input clock off");

	if (t.dp) {
		// --- dcn401_program_pix_clk: dccg401_set_dp_dto, the DTO already selected and enabled ---
		b.seg = 1;
		b.write(kDpDtoPhase + 4 * t.otg, t.dto.phase, "dp dto phase");
		b.write(kDpDtoModulo + 4 * t.otg, t.dto.modulo, "dp dto modulo");
		b.update(kOtgPixelRateDiv, 0xfu << (1 + 5 * t.otg), t.dto.integer << (1 + 5 * t.otg), "dp dto integer");
		b.seg = 2;
	}

	// --- dcn10_link_encoder_disable_output: transmitter off ---
	if (t.dp) {
		// the link stays up
	} else if (Dmub::Cmd *c = b.dmub("transmitter disable")) {
		Dmub::TransmitterControl tx {};
		tx.phyId = t.link;
		tx.action = Dmub::TransmitterActionDisable;
		tx.digMode = Dmub::EncoderModeHdmi;
		tx.hpdSel = t.hpd;
		tx.connObjId = static_cast<uint8_t>(t.connectorObjId & 0xff);
		Dmub::buildTransmitterControl(*c, tx);
	}

	// --- dcn401_program_pix_clk: PHY PLL at the new pixel clock ---
	if (t.dp) {
		// the PHY PLL carries the link clock, not the pixel clock
	} else if (Dmub::Cmd *c = b.dmub("set pixel clock")) {
		Dmub::SetPixelClock pc {};
		pc.pixclk100Hz = t.to.pixelClockKHz * 10;
		// Each combo PHY has its own PLL and amdgpu takes the one of the
		// link's transmitter (find_matching_pll): ATOM_COMBOPHY_PLL0 (0x14)
		// + the link. Captured from amdgpu on this card: pll_id 0x16 with
		// phyid 2 (the HDMI sink on UNIPHY C, 2026-07-17).
		pc.pllId = static_cast<uint8_t>(0x14 + t.link);
		pc.encoderObjId = static_cast<uint8_t>(t.encoderObjId & 0xff);
		pc.encoderMode = Dmub::EncoderModeHdmi;
		pc.crtcId = t.otg;
		Dmub::buildSetPixelClock(*c, pc);
	}

	// --- optc1_enable_optc_clock(true) ---
	b.update(kOptcInputClock + odm, 0x3, 0x3, "optc input clock on");
	b.waitSet(kOptcInputClock + odm, 1u << 2, 1 * kMs, "optc input clock running");
	b.update(kOtgClockControl + otg, 0x3, 0x3, "otg clock on");
	b.waitSet(kOtgClockControl + otg, 1u << 8, 1 * kMs, "otg clock running");

	// --- optc1_program_timing (OTG stopped: takes effect at once) ---
	b.write(kOtgHTotal + otg, rg.hTotal, "h total");
	b.write(kOtgHSyncA + otg, rg.hSyncA, "h sync");
	b.write(kOtgHBlank + otg, rg.hBlankStartEnd, "h blank");
	b.write(kOtgHSyncACntl + otg, rg.hSyncPolInvert ? 1 : 0, "h sync polarity");
	b.write(kOtgVTotal + otg, rg.vTotal, "v total");
	b.write(kOtgVTotalMin + otg, rg.vTotal, "v total min");
	b.write(kOtgVTotalMax + otg, rg.vTotal, "v total max");
	b.write(kOtgVTotalControl + otg, 0, "v total control (no DRR)");
	b.write(kOtgVSyncA + otg, rg.vSyncA, "v sync");
	b.write(kOtgVBlank + otg, rg.vBlankStartEnd, "v blank");
	b.write(kOtgVSyncACntl + otg, rg.vSyncPolInvert ? 1 : 0, "v sync polarity");
	b.write(kOtgInterlace + otg, 0, "progressive");
	b.update(kOtgHTimingCntl + otg, 0x103, 0, "h timing div 1");
	b.update(kOptcDataFormat + odm, 0x3, 0, "optc data format rgb");
	const uint32_t vstartup = vstartupLines(t.to);
	b.write(kOtgVStartup + otg, vstartup, "vstartup");
	b.write(kOtgVUpdate + otg, 2u << 16, "vupdate width 2");
	// optc1_set_vtg_params: VCOUNT_INIT = vtotal - vfp (the active end),
	// FP2 = lines between the blank end and VSTARTUP.
	const uint32_t vBlankStart = rg.vBlankStartEnd & 0x7fff;
	const uint32_t vBlankEnd = (rg.vBlankStartEnd >> 16) & 0x7fff;
	const uint32_t fp2 = vstartup > vBlankEnd + 1 ? vstartup - (vBlankEnd + 1) : 0;
	b.write(kVtgControl + t.otg, (vBlankStart << 16) | (fp2 & 0x7fff), "vtg params");

	// --- blank at the new size, then optc401_enable_crtc ---
	b.write(kDpgDimensions + opp, dpgDims(w, h), "dpg dimensions (new)");
	b.write(kOptcDataSource + odm, 0xfff00000 | (static_cast<uint32_t>(t.opp) << 16), "optc source opp");
	b.update(kVtgControl + t.otg, 1u << 31, 1u << 31, "vtg enable");
	b.update(kOtgControl + otg, 0x301 | (3u << 20), (2u << 8) | 1, "otg master enable");
	b.waitClear(kDpgStatus + opp, 0x1, 100 * kMs, "dpg latched", true);
	b.update(kDigFeCntl + dig, 0x7, t.otg, "dig source otg");

	if (t.dp) {
		// --- enc401_stream_encoder_dp_set_stream_attribute: the MSA. The MSA counts from the
		// leading edge of sync: start = sync width + back porch.
		const uint32_t msa = kDpMsaParam1 + dig;
		const uint32_t hStart = t.to.hBlank - t.to.hSyncOffset, vStart = t.to.vBlank - t.to.vSyncOffset;
		b.write(msa, (t.to.hTotal() << 16) | t.to.vTotal(), "msa totals");
		b.write(msa + 1, (hStart << 16) | vStart, "msa active start");
		b.write(msa + 2, (static_cast<uint32_t>(!t.to.hSyncPositive) << 31) | (static_cast<uint32_t>(t.to.hSyncWidth) << 16) |
		                 (static_cast<uint32_t>(!t.to.vSyncPositive) << 15) | t.to.vSyncWidth, "msa sync");
		b.write(msa + 3, (w << 16) | h, "msa active size");

		// --- setup_dio_stream_encoder: enc35_enable_fifo ---
		b.update(kDigFifoCtrl0 + dig, 0x1fu << 2, 7u << 2, "dig fifo start level 7");
		b.update(kDigFifoCtrl0 + dig, 0x2, 0x2, "dig fifo reset");
		b.delay(10, "dig fifo reset");
		b.update(kDigFifoCtrl0 + dig, 0x2, 0, "dig fifo reset release");
		b.delay(10, "dig fifo reset release");
		b.update(kDigFifoCtrl0 + dig, 0x1, 0x1, "dig fifo enable");

		// --- enc401_stream_encoder_dp_unblank ---
		b.update(kDpVidTiming + dig, 1u << 8, 0, "dp m/n measurement off");
		b.update(kDpVidN + dig, 0xffffff, 0x8000, "dp vid n");
		b.update(kDpVidM + dig, 0xffffff, t.vidM & 0xffffff, "dp vid m (first value)");
		b.update(kDpVidTiming + dig, 1u << 8, 1u << 8, "dp m/n measurement on");
		b.update(kDpVidStreamCntl + dig, 0x1, 0, "dp video stream off");
		b.waitClear(kDpVidStreamCntl + dig, 1u << 16, 50 * kMs, "dp stream idle", true);
		b.update(kDpSteerFifo + dig, 0x2, 0x2, "dp steer fifo reset");
		b.delay(10, "dp steer fifo reset");
		b.update(kDpSteerFifo + dig, 0x2, 0, "dp steer fifo reset release");
		b.update(kDpSteerFifo + dig, 0x1, 0x1, "dp steer fifo enable");
		b.update(kDpVidStreamCntl + dig, 0x301, 0x201, "dp video stream on");
		b.delay(200, "dp video stream on");
		b.update(kDigFifoCtrl0 + dig, 0x1fu << 2, 7u << 2, "dig fifo start level 7");
		b.update(kDigFifoCtrl0 + dig, 0x2, 0x2, "dig fifo reset");
		b.waitSet(kDigFifoCtrl0 + dig, 1u << 20, 50 * kMs, "dig fifo reset done", true);
		b.update(kDigFifoCtrl0 + dig, 0x2, 0, "dig fifo reset release");
		b.waitClear(kDigFifoCtrl0 + dig, 1u << 20, 50 * kMs, "dig fifo reset done clear", true);
		b.update(kDigFifoCtrl0 + dig, 0x1, 0x1, "dig fifo enable");
		b.delay(100, "dp encoder primes");
		b.update(kDpVidStreamCntl + dig, 0x1, 0x1, "dp video stream on");
	} else {
		// --- dcn401_link_encoder_setup: back-end in HDMI mode ---
		b.update(kDigBeClkCntl + t.link * kDigStride, 0x17, 0x10 | kModeHdmi, "dig be hdmi + clock");
		b.update(kDigBeEnCntl + t.link * kDigStride, 0x1, 0x1, "dig be enable");

		// --- enc401_stream_encoder_hdmi_set_stream_attribute: STREAM_SETUP ---
		if (Dmub::Cmd *c = b.dmub("encoder stream setup")) {
			Dmub::DigEncoderStreamSetup es {};
			es.digId = t.dig;
			es.action = Dmub::EncoderActionStreamSetup;
			es.digMode = Dmub::EncoderModeHdmi;
			es.laneNum = 4;
			es.pclk10kHz = symclk10kHz;
			Dmub::buildDigEncoderStreamSetup(*c, es);
		}
		b.update(kHdmiControl + dig, 0x6, t.to.pixelClockKHz >= kScrambleFromKHz ? 0x6 : 0, "hdmi scrambler");

		// --- write_scdc_data: the sink's side of the scrambling, before the link is enabled ---
		if (t.sinkScdc)
			b.add(Op::Scdc, 0, 0, scdcTmdsConfig(t.to.pixelClockKHz), t.ddcLine, "scdc tmds config", true);

		// --- setup_dio_stream_encoder: connect, enable, map, FIFO reset ---
		b.update(kDigBeCntl + t.link * kDigStride, 1u << (8 + t.dig), 1u << (8 + t.dig), "dig be source fe");
		b.update(kDigFeClkCntl + dig, 0x17, 0x10 | kModeHdmi, "dig fe hdmi + clock");
		b.write(kDigFeEnCntl + dig, 1, "dig fe enable");
		b.update(kStreamMapper + t.dig, 0x7, t.link, "stream mapper link");
		b.update(kDigFifoCtrl0 + dig, (0x1fu << 2) | (0x3u << 8), 7u << 2, "dig fifo 1 px/clk, start level 7");
		b.update(kDigFifoCtrl0 + dig, 0x2, 0x2, "dig fifo reset");
		b.waitSet(kDigFifoCtrl0 + dig, 1u << 20, 1 * kMs, "dig fifo reset done", true);
		b.update(kDigFifoCtrl0 + dig, 0x2, 0, "dig fifo reset release");
		b.waitClear(kDigFifoCtrl0 + dig, 1u << 20, 1 * kMs, "dig fifo reset done clear", true);
		b.update(kDigFifoCtrl0 + dig, 0x1, 0x1, "dig fifo enable");

		// --- dcn10_link_encoder_enable_tmds_output: transmitter on ---
		if (Dmub::Cmd *c = b.dmub("transmitter enable")) {
			Dmub::TransmitterControl tx {};
			tx.phyId = t.link;
			tx.action = Dmub::TransmitterActionEnable;
			tx.digMode = Dmub::EncoderModeHdmi;
			tx.laneNum = 4;
			tx.symclk10kHz = symclk10kHz;
			tx.hpdSel = t.hpd;
			Dmub::buildTransmitterControl(*c, tx);
		}
	}

	// --- dcn401_program_pipe under the OTG update lock ---
	b.update(kOtgGlobalCtrl2 + otg, 0x7u << 25, static_cast<uint32_t>(t.otg) << 25, "update lock sel");
	b.update(kOtgUpdateLock + otg, 0x1, 0x1, "update lock");
	b.waitSet(kOtgUpdateLock + otg, 1u << 8, 10 * kMs, "update lock held", true);
	b.write(kHubpViewportStart + hubp, 0, "viewport start");
	b.write(kHubpViewportDim + hubp, sizeWH(w, h), "viewport size");
	b.write(kDsclRecoutStart + dpp, 0, "recout start");
	b.write(kDsclRecoutSize + dpp, sizeWH(w, h), "recout size");
	b.write(kDsclMpcSize + dpp, sizeWH(w, h), "mpc size");
	for (size_t i = 0; i < t.nextra; i++) {
		b.seg = t.extra[i].seg;
		b.add(t.extra[i].op, t.extra[i].dword, t.extra[i].mask, t.extra[i].value, t.extra[i].arg, t.extra[i].what);
	}
	b.seg = 2;
	b.update(kOtgUpdateLock + otg, 0x1, 0, "update unlock");

	// --- blank_pixel_data(false): DPG video mode; set_avmute(false) ---
	b.update(kDpgControl + opp, 0x71, 0, "dpg video mode");
	b.write(kDpgRamp + opp, 0, "dpg ramp");
	if (!t.dp)
		b.update(kHdmiGc + dig, 0x1, 0, "hdmi avmute off");

	if (b.full) {
		err = "plan too long";
		return false;
	}
	return true;
}

} // namespace ModeSet
