//
//  pipe2.cpp
//  RDNA4FB
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
// connectors of this project's card; tools/pipegen/run.sh).
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
};
constexpr size_t kTableCount = sizeof(kTables) / sizeof(kTables[0]);

const Table *cur = &kTables[0];   // the table in use (use())

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

bool use(uint8_t hpd) {
	for (const Table &t : kTables)
		if (t.config.hpd == hpd) {
			cur = &t;
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
		if (which == ModeRegs::All || r.who == 'D' || (r.who == 'S' && row >= 0))
			out[n++] = ModeSet::Step { ModeSet::Op::Update, r.seg, false, r.dword, r.mask,
			                           cur->values[(row < 0 ? 0 : row) * cur->nregs + i], 0, r.name };
	}
	return n;
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
	// The scrambler setting in these parts is the plan's.
	if ((khz > ModeSet::kScrambleFromKHz) != (c.timing.pixelClockKHz > ModeSet::kScrambleFromKHz)) {
		err = "sleep and wake are generated for the plan's side of 340 MHz";
		return false;
	}

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

	Part part = Part::Light;   // of the entry: the requirements at the top belong to lighting
	for (size_t i = 0; i < cur->ngen; i++) {
		const Gen &g = cur->gen[i];
		if (g.kind >= '4' && g.kind <= '6')
			part = static_cast<Part>(g.kind - '3');
		if (part != t.part)
			continue;
		bool ok = true;
		switch (g.kind) {
		case '1': case '2': case '4': case '5': case '6':
			break;
		case '3':
			if (t.depth == Depth::Stream)
				return true;
			break;
		case 'R': ok = step(ModeSet::Op::Require, g, g.mask, g.value, 0); break;
		case 'W': ok = step(ModeSet::Op::Write, g, 0, g.value, 0); break;
		case 'U': ok = step(ModeSet::Op::Update, g, g.mask, g.value, 0); break;
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
				Dmub::SetPixelClock pc {};
				pc.pixclk100Hz = khz * 10;
				pc.pllId = static_cast<uint8_t>(0x14 + c.link);   // ATOM_COMBOPHY_PLL0 + the link's PHY
				pc.encoderObjId = static_cast<uint8_t>(t.encoderObjId & 0xff);
				pc.encoderMode = Dmub::EncoderModeHdmi;
				pc.crtcId = c.pipe;
				Dmub::buildSetPixelClock(*cmd, pc);
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
