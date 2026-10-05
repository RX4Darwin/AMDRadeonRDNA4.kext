//
//  modeset.hpp
//  RDNA4FB
//
//  Mode change on the lit pipe, as an ordered list of register writes, polls
//  and DMUB commands. For HDMI (TMDS) the sequence is amdgpu DC's full
//  modeset for DCN 4.0.1 (dc_commit_state_no_check on a stream whose timing
//  changes), reduced to what a single pipe with an unchanged surface needs:
//
//    blank (DPG solid black) -> AVMUTE, 3 frames -> stream encoder off ->
//    optc401_disable_crtc -> OPTC clocks off -> TRANSMITTER_CONTROL disable
//    -> SET_PIXEL_CLOCK -> OPTC clocks on -> optc1_program_timing + VTG ->
//    blank at the new size -> optc401_enable_crtc -> link encoder setup ->
//    DIGX_ENCODER_CONTROL stream setup -> stream encoder enable + FIFO reset
//    -> TRANSMITTER_CONTROL enable -> viewport / recout / MPC size under the
//    OTG update lock -> unblank (DPG video mode) -> AVMUTE off.
//
//  From 340 MHz up the link runs HDMI 2.0 scrambled with the clock channel at
//  a quarter of the character rate: the stream encoder's scrambler is turned
//  on and the sink is told through its SCDC register before the transmitter
//  comes up (enc401_stream_encoder_hdmi_set_stream_attribute,
//  write_scdc_data). Below it both are turned off again, which matters when
//  the firmware lit the sink scrambled.
//
//  DisplayPort keeps its link: amdgpu takes the link down for a new timing
//  and trains it again, which needs the AUX channel protocol; the stream on
//  a trained link can be retimed without that, as long as it needs no more
//  of the link than before. Between the same blank and unblank:
//
//    DP video stream off at the vertical blank, 60 ms of idle pattern ->
//    optc401_disable_crtc -> the pixel-rate DTO (dccg401_set_dp_dto) ->
//    timing -> optc401_enable_crtc -> the MSA
//    (enc401_stream_encoder_dp_set_stream_attribute) -> FIFO reset ->
//    enc401_stream_encoder_dp_unblank.
//
//  The link encoder, the PHY and DMUB are not touched. tools/pipegen's "dp"
//  scenario runs those Linux functions; tools/atomdump.cpp holds this plan
//  against its output.
//
//  Not programmed (kept as the GOP left them): DCHUBBUB watermarks and the
//  HUBP DLG/TTU/RQ request parameters, which amdgpu takes from DML; the
//  AVI infoframe; FAMS2 (DMUB-managed MCLK switching). VSTARTUP is placed
//  in the vertical blank by rule, not by DML. See docs in README.
//
//  Freestanding: builds a Plan; RDNA4Device::applyMode executes it and the
//  host harness checks it.
//

#ifndef ModeSet_hpp
#define ModeSet_hpp

#include <stddef.h>
#include <stdint.h>

#include "dmub.hpp"
#include "edid.hpp"

namespace ModeSet {

enum class Op : uint8_t {
	Write,        // reg = value
	Update,       // reg = (reg & ~mask) | value
	WaitSet,      // poll until (reg & mask) == mask, up to arg microseconds
	WaitClear,    // poll until (reg & mask) == 0, up to arg microseconds
	Dmub,         // submit cmds[arg] and wait for the firmware to consume it
	WaitFrames,   // let arg frames of the OTG pass (frame counter)
	// Used by the second-pipe plan (pipe2.hpp):
	WaitValue,    // poll until (reg & mask) == value, up to arg microseconds
	Delay,        // arg microseconds
	Copy,         // reg = the register at dword `arg` of the same segment
	Require,      // (reg & mask) must equal value, or the plan is not run
	Scdc,         // write `value` to the sink's SCDC TMDS_CONFIG over DDC line `arg`
};

struct Step {
	Op          op;
	uint8_t     seg;          // DMU register segment (base_idx)
	bool        optional;     // a wait that may time out without failing
	uint32_t    dword;        // offset within the segment
	uint32_t    mask;
	uint32_t    value;
	uint32_t    arg;
	const char *what;         // for the log
};

constexpr size_t kMaxSteps = 128;
constexpr size_t kMaxCmds  = 4;

struct Plan {
	Step      steps[kMaxSteps];
	size_t    count;
	Dmub::Cmd cmds[kMaxCmds];
	size_t    ncmds;
};

struct DpDto {
	uint32_t integer, phase, modulo;   // DPDTOn_INT, DP_DTOn_PHASE, DP_DTOn_MODULO
};

// The pipe (from Pipe::discover) and board wiring (from the VBIOS path whose
// HPD pin matches the pipe's).
struct Target {
	uint8_t  otg, dig, link, hpd, opp, hubp;
	uint16_t encoderObjId;    // e.g. 0x2120 (UNIPHY1 enum 1 = UNIPHYC)
	uint16_t connectorObjId;  // e.g. 0x330c (HDMI type A)
	Edid::DetailedTiming from, to;
	bool     sinkScdc;        // the sink's EDID announces SCDC (Edid::hdmi2Caps)
	uint8_t  ddcLine;         // the connector's DDC line, for the SCDC write
	// DisplayPort instead of TMDS: the pixel-rate DTO for `to` (scaleDpDto)
	// and the first DP_VID_M, before the hardware measures it.
	bool     dp;
	uint32_t dpMaxKHz;        // the pixel clock the link was trained for
	DpDto    dto;
	uint32_t vidM;
	// More steps for the OTG update lock, after the plan's own: what else
	// has to change with the timing on this pipe (Pipe2::modeSteps).
	const Step *extra;
	size_t      nextra;
};

// The DP pixel-rate DTO for a new pixel clock, from the one running now. The
// DTO makes its reference clock times (integer + phase / modulo). amdgpu
// programs it in Hz (modulo = the reference in Hz, so integer x modulo +
// phase is the pixel clock in Hz); if the running one reads that way the new
// one is exact, otherwise it is scaled by the ratio of the two clocks.
// False if there is no running DTO or the result does not fit.
bool scaleDpDto(const DpDto &now, uint32_t fromKHz, uint32_t toKHz, DpDto &out);

// HDMI 2.0 over TMDS. The scrambler goes on from kScrambleFromKHz
// (HDMI_CLOCK_CHANNEL_RATE_MORE_340M); the sink's TMDS_CONFIG says 3
// (scrambling on, clock at a quarter rate) above it, as write_scdc_data has it.
constexpr uint32_t kMaxTmdsKHz      = 600000;
constexpr uint32_t kScrambleFromKHz = 340000;
constexpr uint32_t scdcTmdsConfig(uint32_t pixelClockKHz) { return pixelClockKHz > kScrambleFromKHz ? 3 : 0; }

// Build the plan for `t`. Returns false (and `why`) if the target or the
// timings cannot be expressed.
bool build(const Target &t, Plan &out, const char **why);

// VSTARTUP line count the plan uses for timing `to`.
uint32_t vstartupLines(const Edid::DetailedTiming &to);

} // namespace ModeSet

#endif /* ModeSet_hpp */
