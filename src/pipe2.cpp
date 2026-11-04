//
//  pipe2.cpp
//  AMDRadeonRDNA4
//
//  See pipe2.hpp.
//

#include "pipe2.hpp"
#include "dpphy.hpp"

namespace Pipe2 {

namespace {

// One entry of the generated table. kind:
//   W write, U update, T wait for (reg & mask) == value, D delay arg us,
//   R require (reg & mask) == value before anything is written,
//   1 2 3 start of the init / stream / plane part of lighting, 4 5 6 of sleep / wake / avi,
//   P E X DMUB set pixel clock / encoder stream setup / transmitter enable,
//   S the sink's SCDC TMDS_CONFIG = arg (if the sink has SCDC),
//   A word arg of the AVI infoframe, for the timing the pipe runs (aviWord),
//   a t O DisplayPort: write the table's AUX entry arg to the sink / train
//   the link / DMUB transmitter disable,
//   M m DisplayPort: MSA word arg of the timing the pipe runs / the first
//   Mvid of that timing on the link's rate (ModeSet::dpMsa, dpVidM),
//   K pattern generator colour arg (0 R, 1 G, 2 B),
//   C copy from the lit pipe's register, arg dwords per HUBP instance below,
//   H L surface address high / low.
struct Gen {
	char        kind;
	uint8_t     seg;
	uint16_t    dword;
	uint32_t    mask, value, arg;
	const char *what;
};

// A register that changes with the mode. who: M the mode-set engine programs
// it to the same value, S the engine programs it by rule and Linux's value
// replaces that, D only the table has it.
struct ModeReg {
	char        who;
	uint8_t     seg;
	uint16_t    dword;
	uint32_t    mask;
	const char *name;
};

// One generated table per connector a second display can be on (the HDMI
// connectors of this project's card; tools/gen/pipegen/run.sh).
namespace Hpd3 {
#include "pipe2_linux.inc"
}
namespace Hpd4 {
#include "pipe2_linux_hpd4.inc"
}
// ... and the DisplayPort connectors.
namespace Hpd1 {
#include "pipe2_linux_dp1.inc"
}
namespace Hpd2 {
#include "pipe2_linux_dp2.inc"
}
// ... and those again for an HDMI monitor behind a passive DP++ adapter: the
// connector is DisplayPort, the stream HDMI.
namespace Hpd1Tmds {
#include "pipe2_linux_dp1_tmds.inc"
}
namespace Hpd2Tmds {
#include "pipe2_linux_dp2_tmds.inc"
}
// ... and all six again for pipe 2, the third display.
namespace P2Hpd3 {
#include "pipe2_linux_p2_hpd3.inc"
}
namespace P2Hpd4 {
#include "pipe2_linux_p2_hpd4.inc"
}
namespace P2Hpd1 {
#include "pipe2_linux_p2_dp1.inc"
}
namespace P2Hpd2 {
#include "pipe2_linux_p2_dp2.inc"
}
namespace P2Hpd1Tmds {
#include "pipe2_linux_p2_dp1_tmds.inc"
}
namespace P2Hpd2Tmds {
#include "pipe2_linux_p2_dp2_tmds.inc"
}
// ... and all six again for pipe 3, the fourth display.
namespace P3Hpd3 {
#include "pipe2_linux_p3_hpd3.inc"
}
namespace P3Hpd4 {
#include "pipe2_linux_p3_hpd4.inc"
}
namespace P3Hpd1 {
#include "pipe2_linux_p3_dp1.inc"
}
namespace P3Hpd2 {
#include "pipe2_linux_p3_dp2.inc"
}
namespace P3Hpd1Tmds {
#include "pipe2_linux_p3_dp1_tmds.inc"
}
namespace P3Hpd2Tmds {
#include "pipe2_linux_p3_dp2_tmds.inc"
}

// The HDMI connectors lit at 3840x2160@60, for each pipe.
namespace P1Hpd3Uhd {
#include "pipe2_linux_p1_hpd3_uhd.inc"
}
namespace P1Hpd4Uhd {
#include "pipe2_linux_p1_hpd4_uhd.inc"
}
namespace P2Hpd3Uhd {
#include "pipe2_linux_p2_hpd3_uhd.inc"
}
namespace P2Hpd4Uhd {
#include "pipe2_linux_p2_hpd4_uhd.inc"
}
namespace P3Hpd3Uhd {
#include "pipe2_linux_p3_hpd3_uhd.inc"
}
namespace P3Hpd4Uhd {
#include "pipe2_linux_p3_hpd4_uhd.inc"
}

// The DisplayPort connectors lit at 3840x2160@60 for an HDMI sink behind a
// passive DP++ adapter, for each pipe.
namespace P1Hpd1TmdsUhd {
#include "pipe2_linux_p1_dp1_tmds_uhd.inc"
}
namespace P1Hpd2TmdsUhd {
#include "pipe2_linux_p1_dp2_tmds_uhd.inc"
}
namespace P2Hpd1TmdsUhd {
#include "pipe2_linux_p2_dp1_tmds_uhd.inc"
}
namespace P2Hpd2TmdsUhd {
#include "pipe2_linux_p2_dp2_tmds_uhd.inc"
}
namespace P3Hpd1TmdsUhd {
#include "pipe2_linux_p3_dp1_tmds_uhd.inc"
}
namespace P3Hpd2TmdsUhd {
#include "pipe2_linux_p3_dp2_tmds_uhd.inc"
}

struct Table {
	const Config               &config;
	const Gen                  *gen;        // the plan
	size_t                      ngen;
	const ModeReg              *regs;       // what changes with the mode ...
	size_t                      nregs;
	const Edid::DetailedTiming *timings;    // ... for these modes, the plan's first ...
	size_t                      nmodes;
	const uint32_t             *values;     // ... one row of nregs values per mode
	const AuxWrite             *aux;        // DisplayPort: what 'a' entries write
	size_t                      naux;
};

template <size_t G, size_t R, size_t M>
constexpr Table table(const Config &config, const Gen (&gen)[G], const ModeReg (&regs)[R],
                      const Edid::DetailedTiming (&timings)[M], const uint32_t (&values)[M][R],
                      const AuxWrite *aux = nullptr, size_t naux = 0) {
	static_assert(R <= kMaxModeSteps, "the mode table");
	return { config, gen, G, regs, R, timings, M, &values[0][0], aux, naux };
}
template <size_t N> constexpr size_t countOf(const AuxWrite (&)[N]) { return N; }

constexpr Table kTables[] = {
	table(Hpd3::kConfig, Hpd3::kGen, Hpd3::kModeRegs, Hpd3::kModeTimings, Hpd3::kModeValues),
	table(Hpd4::kConfig, Hpd4::kGen, Hpd4::kModeRegs, Hpd4::kModeTimings, Hpd4::kModeValues),
	table(Hpd1::kConfig, Hpd1::kGen, Hpd1::kModeRegs, Hpd1::kModeTimings, Hpd1::kModeValues, Hpd1::kAux,
	      countOf(Hpd1::kAux)),
	table(Hpd2::kConfig, Hpd2::kGen, Hpd2::kModeRegs, Hpd2::kModeTimings, Hpd2::kModeValues, Hpd2::kAux,
	      countOf(Hpd2::kAux)),
	table(Hpd1Tmds::kConfig, Hpd1Tmds::kGen, Hpd1Tmds::kModeRegs, Hpd1Tmds::kModeTimings, Hpd1Tmds::kModeValues),
	table(Hpd2Tmds::kConfig, Hpd2Tmds::kGen, Hpd2Tmds::kModeRegs, Hpd2Tmds::kModeTimings, Hpd2Tmds::kModeValues),
	table(P2Hpd3::kConfig, P2Hpd3::kGen, P2Hpd3::kModeRegs, P2Hpd3::kModeTimings, P2Hpd3::kModeValues),
	table(P2Hpd4::kConfig, P2Hpd4::kGen, P2Hpd4::kModeRegs, P2Hpd4::kModeTimings, P2Hpd4::kModeValues),
	table(P2Hpd1::kConfig, P2Hpd1::kGen, P2Hpd1::kModeRegs, P2Hpd1::kModeTimings, P2Hpd1::kModeValues, P2Hpd1::kAux,
	      countOf(P2Hpd1::kAux)),
	table(P2Hpd2::kConfig, P2Hpd2::kGen, P2Hpd2::kModeRegs, P2Hpd2::kModeTimings, P2Hpd2::kModeValues, P2Hpd2::kAux,
	      countOf(P2Hpd2::kAux)),
	table(P2Hpd1Tmds::kConfig, P2Hpd1Tmds::kGen, P2Hpd1Tmds::kModeRegs, P2Hpd1Tmds::kModeTimings, P2Hpd1Tmds::kModeValues),
	table(P2Hpd2Tmds::kConfig, P2Hpd2Tmds::kGen, P2Hpd2Tmds::kModeRegs, P2Hpd2Tmds::kModeTimings, P2Hpd2Tmds::kModeValues),
	table(P3Hpd3::kConfig, P3Hpd3::kGen, P3Hpd3::kModeRegs, P3Hpd3::kModeTimings, P3Hpd3::kModeValues),
	table(P3Hpd4::kConfig, P3Hpd4::kGen, P3Hpd4::kModeRegs, P3Hpd4::kModeTimings, P3Hpd4::kModeValues),
	table(P3Hpd1::kConfig, P3Hpd1::kGen, P3Hpd1::kModeRegs, P3Hpd1::kModeTimings, P3Hpd1::kModeValues, P3Hpd1::kAux,
	      countOf(P3Hpd1::kAux)),
	table(P3Hpd2::kConfig, P3Hpd2::kGen, P3Hpd2::kModeRegs, P3Hpd2::kModeTimings, P3Hpd2::kModeValues, P3Hpd2::kAux,
	      countOf(P3Hpd2::kAux)),
	table(P3Hpd1Tmds::kConfig, P3Hpd1Tmds::kGen, P3Hpd1Tmds::kModeRegs, P3Hpd1Tmds::kModeTimings, P3Hpd1Tmds::kModeValues),
	table(P3Hpd2Tmds::kConfig, P3Hpd2Tmds::kGen, P3Hpd2Tmds::kModeRegs, P3Hpd2Tmds::kModeTimings, P3Hpd2Tmds::kModeValues),
	table(P1Hpd3Uhd::kConfig, P1Hpd3Uhd::kGen, P1Hpd3Uhd::kModeRegs, P1Hpd3Uhd::kModeTimings, P1Hpd3Uhd::kModeValues),
	table(P1Hpd4Uhd::kConfig, P1Hpd4Uhd::kGen, P1Hpd4Uhd::kModeRegs, P1Hpd4Uhd::kModeTimings, P1Hpd4Uhd::kModeValues),
	table(P2Hpd3Uhd::kConfig, P2Hpd3Uhd::kGen, P2Hpd3Uhd::kModeRegs, P2Hpd3Uhd::kModeTimings, P2Hpd3Uhd::kModeValues),
	table(P2Hpd4Uhd::kConfig, P2Hpd4Uhd::kGen, P2Hpd4Uhd::kModeRegs, P2Hpd4Uhd::kModeTimings, P2Hpd4Uhd::kModeValues),
	table(P3Hpd3Uhd::kConfig, P3Hpd3Uhd::kGen, P3Hpd3Uhd::kModeRegs, P3Hpd3Uhd::kModeTimings, P3Hpd3Uhd::kModeValues),
	table(P3Hpd4Uhd::kConfig, P3Hpd4Uhd::kGen, P3Hpd4Uhd::kModeRegs, P3Hpd4Uhd::kModeTimings, P3Hpd4Uhd::kModeValues),
	table(P1Hpd1TmdsUhd::kConfig, P1Hpd1TmdsUhd::kGen, P1Hpd1TmdsUhd::kModeRegs, P1Hpd1TmdsUhd::kModeTimings, P1Hpd1TmdsUhd::kModeValues),
	table(P1Hpd2TmdsUhd::kConfig, P1Hpd2TmdsUhd::kGen, P1Hpd2TmdsUhd::kModeRegs, P1Hpd2TmdsUhd::kModeTimings, P1Hpd2TmdsUhd::kModeValues),
	table(P2Hpd1TmdsUhd::kConfig, P2Hpd1TmdsUhd::kGen, P2Hpd1TmdsUhd::kModeRegs, P2Hpd1TmdsUhd::kModeTimings, P2Hpd1TmdsUhd::kModeValues),
	table(P2Hpd2TmdsUhd::kConfig, P2Hpd2TmdsUhd::kGen, P2Hpd2TmdsUhd::kModeRegs, P2Hpd2TmdsUhd::kModeTimings, P2Hpd2TmdsUhd::kModeValues),
	table(P3Hpd1TmdsUhd::kConfig, P3Hpd1TmdsUhd::kGen, P3Hpd1TmdsUhd::kModeRegs, P3Hpd1TmdsUhd::kModeTimings, P3Hpd1TmdsUhd::kModeValues),
	table(P3Hpd2TmdsUhd::kConfig, P3Hpd2TmdsUhd::kGen, P3Hpd2TmdsUhd::kModeRegs, P3Hpd2TmdsUhd::kModeTimings, P3Hpd2TmdsUhd::kModeValues),
};
constexpr size_t kTableCount = sizeof(kTables) / sizeof(kTables[0]);

const Table *cur = &kTables[0];   // the table in use (use())

// DIGn_HDMI_CONTROL: DATA_SCRAMBLE_EN and CLOCK_CHANNEL_RATE are the mode
// set's to write for the mode it switches to (ModeSet, "hdmi scrambler").
constexpr uint32_t kHdmiControl = 0x209e, kDigStride = 0x124, kHdmiScrambler = 0x6;

// What a mode-table register may write for a switch: all of its mask but the
// scrambler bits, which a row of another mode would set for that mode.
uint32_t applyMask(const ModeReg &r) {
	if (r.seg == 2 && r.dword == kHdmiControl + cur->config.dig * kDigStride)
		return r.mask & ~kHdmiScrambler;

	return r.mask;
}

// Row of a timing in the mode table, or -1.
int modeRow(const Edid::DetailedTiming &t) {
	for (size_t i = 0; i < cur->nmodes; i++)
		if (Edid::sameTiming(cur->timings[i], t))
			return static_cast<int>(i);
	return -1;
}

// The words of the AVI infoframe (version 2, 13 bytes) that depend on the
// timing, as amdgpu's set_avi_info_frame fills them for 8 bpc full-range RGB:
// word 1 is the checksum and PB1..3 (PB2 holds the picture aspect), 2 the VIC,
// 3 and 4 the bottom and right bar ends (v total + 1, h total + 1).
uint32_t aviWord(const Edid::DetailedTiming &t, uint32_t word) {
	const uint32_t aspect = t.hActive * 9 == t.vActive * 16 ? 2 : t.hActive * 3 == t.vActive * 4 ? 1 : 0;
	const uint32_t pb[4] = { 0x1e | (((aspect << 4) | 8) << 8) | (0x88u << 16), Edid::vicOf(t),
	                         t.vTotal() + 1, t.hTotal() + 1 };
	if (word != 1)
		return pb[word - 1];
	uint32_t sum = 0x82 + 0x02 + 0x0d;   // the header: type, version, length
	for (uint32_t v : pb)
		sum += (v & 0xff) + ((v >> 8) & 0xff) + ((v >> 16) & 0xff);
	return ((0x100 - (sum & 0xff)) & 0xff) | (pb[0] << 8);
}

} // namespace

const Config &config() { return cur->config; }
size_t configCount() { return kTableCount; }
const Config &configAt(size_t i) { return kTables[i].config; }

bool use(uint8_t hpd, bool tmds, uint8_t pipe, bool uhd) {
	const Table *fallback = nullptr;

	for (const Table &t : kTables) {
		if (t.config.hpd != hpd || t.config.pipe != pipe || isUhd(t.config) != uhd)
			continue;

		if (t.config.dp != tmds) {
			cur = &t;
			return true;
		}

		if (!fallback)
			fallback = &t;   // an HDMI connector's only table
	}

	if (!fallback)
		return false;

	cur = fallback;
	return true;
}

bool isUhd(const Config &c) { return c.timing.hActive >= kUhdWidth; }

bool hasUhdTable(uint8_t hpd) {
	for (const Table &t : kTables) {
		if (t.config.hpd == hpd && isUhd(t.config))
			return true;
	}

	return false;
}

bool hasDpTable(uint8_t hpd) {
	for (const Table &t : kTables) {
		if (t.config.hpd == hpd && t.config.dp)
			return true;
	}

	return false;
}

bool modeKnown(const Edid::DetailedTiming &t) { return modeRow(t) >= 0; }

const Edid::DetailedTiming *knownModes(size_t &count) {
	count = cur->nmodes;
	return cur->timings;
}

uint32_t modeDetSegments(const Edid::DetailedTiming &to) {
	const Config &c = cur->config;
	const int row = modeRow(to);
	for (size_t i = 0; i < cur->nregs; i++)
		if (cur->regs[i].seg == c.detSeg && cur->regs[i].dword == c.detCtrl[c.pipe])
			return cur->values[(row < 0 ? 0 : row) * cur->nregs + i] & 0x1f;
	return c.detSegments;   // the same in every mode
}

size_t modeSteps(const Edid::DetailedTiming &to, ModeSet::Step *out, size_t cap, ModeRegs which) {
	const int row = modeRow(to);
	if (row < 0 && which == ModeRegs::All)
		return 0;
	size_t n = 0;
	for (size_t i = 0; i < cur->nregs && n < cap; i++) {
		const ModeReg &r = cur->regs[i];
		// Row 0 is the mode the pipe was lit in.
		if (which == ModeRegs::All || r.who == 'D' || (r.who == 'S' && row >= 0)) {
			const uint32_t mask = which == ModeRegs::All ? r.mask : applyMask(r);
			const uint32_t value = cur->values[(row < 0 ? 0 : row) * cur->nregs + i];
			out[n++] = ModeSet::Step { ModeSet::Op::Update, r.seg, false, r.dword, mask, value & mask, 0, r.name };
		}
	}
	return n;
}

// What a plan step writes, but for this pipe's DET size when the target
// asks for another one.
static uint32_t detValue(const Target &t, const Gen &g) {
	const Config &c = cur->config;

	if (!t.detSegments || g.seg != c.detSeg || g.dword != c.detCtrl[c.pipe])
		return g.value;

	return (g.value & ~g.mask) | (t.detSegments & g.mask);
}

bool build(const Target &t, Plan &out, const char **why) {
	const char *dummy;
	const char *&err = why ? *why : dummy;
	const Config &c = cur->config;
	out.count = 0;
	out.ncmds = 0;
	out.naux = 0;

	if (t.part == Part::Light) {
		if (t.litHubp >= 4 || t.litHubp == c.pipe) {
			err = "the lit pipe uses the HUBP this plan lights";
			return false;
		}
		if (t.depth == Depth::Plane && (t.surface == 0 || (t.surface >> 48) != 0)) {
			err = "no scanout address";
			return false;
		}
	}
	const bool moved = t.part != Part::Light && t.now.pixelClockKHz;
	const Edid::DetailedTiming &timing = moved ? t.now : c.timing;
	const uint32_t khz = timing.pixelClockKHz;
	// The scrambler setting in sleep and wake is the plan's (the infoframe has none).
	if (!c.dp && t.part != Part::Avi &&
	    (khz > ModeSet::kScrambleFromKHz) != (c.timing.pixelClockKHz > ModeSet::kScrambleFromKHz)) {
		err = "sleep and wake are generated for the plan's side of 340 MHz";
		return false;
	}
	if (c.dp && t.part != Part::Sleep && !t.linkRate) {
		err = "DisplayPort: no link chosen";
		return false;
	}
	uint32_t msa[4];
	ModeSet::dpMsa(timing, msa);

	auto step = [&](ModeSet::Op op, const Gen &g, uint32_t mask, uint32_t value, uint32_t arg,
	                bool optional = false) {
		if (out.count >= kMaxSteps)
			return false;
		out.steps[out.count++] = ModeSet::Step { op, g.seg, optional, g.dword, mask, value, arg, g.what };
		return true;
	};
	auto dmub = [&](const Gen &g) -> Dmub::Cmd * {
		if (out.ncmds >= kMaxCmds || !step(ModeSet::Op::Dmub, g, 0, 0, static_cast<uint32_t>(out.ncmds)))
			return nullptr;
		return &out.cmds[out.ncmds++];
	};

	// SET_PIXEL_CLOCK: the link's PHY PLL at the pixel clock.
	auto pixelClock = [&](Dmub::Cmd &cmd) {
		Dmub::SetPixelClock pc {};
		pc.pixclk100Hz = khz * 10;
		pc.pllId = static_cast<uint8_t>(0x14 + c.link);   // ATOM_COMBOPHY_PLL0 + the link's PHY
		pc.encoderObjId = static_cast<uint8_t>(t.encoderObjId & 0xff);
		pc.encoderMode = Dmub::EncoderModeHdmi;
		pc.crtcId = c.pipe;
		Dmub::buildSetPixelClock(cmd, pc);
	};
	Part part = Part::Light;   // of the entry: the requirements at the top belong to lighting
	for (size_t i = 0; i < cur->ngen; i++) {
		const Gen &g = cur->gen[i];
		if (g.kind >= '4' && g.kind <= '6')
			part = static_cast<Part>(g.kind - '3');
		if (part != t.part)
			continue;
		bool ok = true;
		switch (g.kind) {
		case '1': case '2': case '4': case '6':
			break;
		case '5':
			// HDMI: the PHY PLL first, as lighting does. The generated table
			// has the command at the end of the sleep part only, and a wake
			// at 594 MHz left the display dark after the transmitter was off
			// (both 4K displays, card, 2026-11-04).
			if (!c.dp) {
				Dmub::Cmd *cmd = dmub(g);
				if (cmd)
					pixelClock(*cmd);
				ok = cmd != nullptr;
			}
			break;
		case '3':
			if (t.depth == Depth::Stream)
				return true;
			break;
		case 'R': ok = step(ModeSet::Op::Require, g, g.mask, g.value, 0); break;
		case 'W': ok = step(ModeSet::Op::Write, g, 0, g.value, 0); break;
		case 'U': ok = step(ModeSet::Op::Update, g, g.mask, detValue(t, g), 0); break;
		// Linux carries on past a wait that times out, with a warning.
		case 'T': ok = step(ModeSet::Op::WaitValue, g, g.mask, g.value, g.arg, true); break;
		case 'D': ok = step(ModeSet::Op::Delay, g, 0, 0, g.arg); break;
		case 'A': ok = step(ModeSet::Op::Write, g, 0, aviWord(timing, g.arg), 0); break;
		case 'K': {
			// The register holds the 16-bit colour twice (DPG_COLOUR0 and 1).
			const uint32_t v = t.depth == Depth::Stream ? kPatternColour[g.arg] * 0x10001u : g.value;
			ok = step(ModeSet::Op::Write, g, 0, v, 0);
			break;
		}
		case 'C':
			ok = step(ModeSet::Op::Copy, g, 0, 0, g.dword - (c.pipe - t.litHubp) * g.arg);
			break;
		case 'H': ok = step(ModeSet::Op::Write, g, 0, static_cast<uint32_t>(t.surface >> 32), 0); break;
		case 'L': ok = step(ModeSet::Op::Write, g, 0, static_cast<uint32_t>(t.surface), 0); break;
		case 'S':
			if (t.sinkScdc)
				ok = step(ModeSet::Op::Scdc, g, 0, g.arg, t.ddcLine, true);
			break;
		case 'a':
			ok = g.arg < cur->naux && out.naux < kMaxAux &&
			     step(ModeSet::Op::Aux, g, 0, 0, static_cast<uint32_t>(out.naux));
			if (ok)
				out.aux[out.naux++] = cur->aux[g.arg];
			break;
		case 't': ok = step(ModeSet::Op::Train, g, 0, 0, 0); break;
		case 'M': ok = step(ModeSet::Op::Write, g, 0, msa[g.arg & 3], 0); break;
		case 'm':
			ok = step(ModeSet::Op::Update, g, g.mask, ModeSet::dpVidM(khz, DpTrain::symbolClockKHz(t.linkRate)), 0);
			break;
		case 'O':
			if (Dmub::Cmd *cmd = dmub(g))
				DpPhy::buildDisable(*cmd, c.link, c.hpd, t.connectorObjId);
			else
				ok = false;
			break;
		case 'P': case 'E': case 'X': {
			Dmub::Cmd *cmd = dmub(g);
			if (!cmd) {
				ok = false;
			} else if (g.kind == 'P') {
				pixelClock(*cmd);
			} else if (g.kind == 'E') {
				Dmub::DigEncoderStreamSetup es {};
				es.digId = c.dig;
				es.action = Dmub::EncoderActionStreamSetup;
				es.digMode = Dmub::EncoderModeHdmi;
				es.laneNum = 4;
				es.pclk10kHz = khz / 10;
				Dmub::buildDigEncoderStreamSetup(*cmd, es);
			} else {
				Dmub::TransmitterControl tx {};
				tx.phyId = c.link;
				tx.action = Dmub::TransmitterActionEnable;
				tx.digMode = Dmub::EncoderModeHdmi;
				tx.laneNum = 4;
				tx.symclk10kHz = khz / 10;
				tx.hpdSel = c.hpd;
				Dmub::buildTransmitterControl(*cmd, tx);
			}
			break;
		}
		default:
			err = "unknown entry in the generated table";
			return false;
		}
		if (!ok) {
			err = "plan too long";
			return false;
		}
	}
	return true;
}

} // namespace Pipe2
