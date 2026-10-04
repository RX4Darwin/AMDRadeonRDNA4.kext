//
//  modeset.hpp
//  RDNA4FB
//
//  HDMI (TMDS) mode change on the lit pipe, as an ordered list of register
//  writes, polls and DMUB commands. The sequence is amdgpu DC's full
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

constexpr size_t kMaxSteps = 96;
constexpr size_t kMaxCmds  = 4;

struct Plan {
	Step      steps[kMaxSteps];
	size_t    count;
	Dmub::Cmd cmds[kMaxCmds];
	size_t    ncmds;
};

// The pipe (from Pipe::discover) and board wiring (from the VBIOS path whose
// HPD pin matches the pipe's).
struct Target {
	uint8_t  otg, dig, link, hpd, opp, hubp;
	uint16_t encoderObjId;    // e.g. 0x2120 (UNIPHY1 enum 1 = UNIPHYC)
	uint16_t connectorObjId;  // e.g. 0x330c (HDMI type A)
	Edid::DetailedTiming from, to;
};

// Build the plan for `t`. Returns false (and `why`) if the target or the
// timings cannot be expressed.
bool build(const Target &t, Plan &out, const char **why);

// VSTARTUP line count the plan uses for timing `to`.
uint32_t vstartupLines(const Edid::DetailedTiming &to);

} // namespace ModeSet

#endif /* ModeSet_hpp */
