//
//  pipe2.cpp
//  RDNA4FB
//
//  See pipe2.hpp.
//

#include "pipe2.hpp"

namespace Pipe2 {

namespace {

// One entry of the generated table. kind:
//   W write, U update, T wait for (reg & mask) == value, D delay arg us,
//   R require (reg & mask) == value before anything is written,
//   1 2 3 start of the init / stream / plane part of lighting, 4 5 of sleep / wake,
//   P E X DMUB set pixel clock / encoder stream setup / transmitter enable,
//   S the sink's SCDC TMDS_CONFIG = arg (if the sink has SCDC),
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

#include "pipe2_linux.inc"

} // namespace

const Config &config() { return kConfig; }

bool build(const Target &t, Plan &out, const char **why) {
	const char *dummy;
	const char *&err = why ? *why : dummy;
	const Config &c = kConfig;
	out.count = 0;
	out.ncmds = 0;

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
	const uint32_t khz = c.timing.pixelClockKHz;

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
	for (const Gen &g : kGen) {
		if (g.kind == '4' || g.kind == '5')
			part = g.kind == '4' ? Part::Sleep : Part::Wake;
		if (part != t.part)
			continue;
		bool ok = true;
		switch (g.kind) {
		case '1': case '2': case '4': case '5':
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
