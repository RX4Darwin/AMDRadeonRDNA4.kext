//
//  pipe2.hpp
//  RDNA4FB
//
//  Light a display pipe the firmware left dark: an HDMI stream on its own
//  timing generator and encoder, then one plane scanning a 32-bit linear
//  surface. This is what a second monitor needs; the lit pipe is not touched.
//
//  The register sequence is not written here. It is Linux's: tools/pipegen
//  runs amdgpu DC's own DCN 4.01 functions (dce110_apply_single_controller_
//  ctx_to_hw, link_set_dpms_on, dcn401_program_pipe, with the HUBP timing
//  values from DML 2.1) against a recorder and writes the result to
//  pipe2_linux.inc, for one pipe, encoder, link and timing. build() adds what
//  only exists at run time: the three DMUB commands, the surface address and
//  the lit pipe's VM aperture. Display sleep and wake of the pipe come from
//  the same run (link_set_dpms_off, then link_set_dpms_on again).
//
//  Freestanding; shared by the kext and tools/atomdump.cpp.
//

#ifndef Pipe2_hpp
#define Pipe2_hpp

#include <stddef.h>
#include <stdint.h>

#include "dmub.hpp"
#include "edid.hpp"
#include "modeset.hpp"

namespace Pipe2 {

// What pipe2_linux.inc was generated for.
struct Config {
	uint8_t  pipe;            // OTG = OPP = HUBP = DPP = MPCC instance
	uint8_t  dig;             // stream encoder (DIG front-end)
	uint8_t  link;            // link encoder / PHY / PLL
	uint8_t  hpd;             // HPD pin, counting from 1 like the VBIOS path records
	Edid::DetailedTiming timing;
	uint16_t vstartup;        // lines, from DML
	uint16_t detSegments;     // DET buffer segments DML gives the plane
	uint8_t  detSeg;          // DCHUBBUB_DET0..3_CTRL: segment and dwords
	uint16_t detCtrl[4];
};
const Config &config();

// The DET buffer is 1344 KiB in 64 KiB segments, shared by all planes
// (DCN4_01_CRB_SIZE_KB / DCN4_01_CRB_SEGMENT_SIZE_KB).
constexpr uint32_t kDetSegmentsTotal = 21;

enum class Depth : uint8_t {
	Stream,   // timing, encoder and PHY only: the sink shows kPatternColour
	Plane,    // and the plane: the sink shows the surface
};

// Which part of the table: lighting the dark pipe, or display sleep and wake
// of the lit one. Sleep leaves the timing generator and the plane running and
// takes the stream encoder and the transmitter away, as amdgpu does for DPMS
// off; wake is the link half of lighting again.
enum class Part : uint8_t { Light, Sleep, Wake };

// 16-bit R, G, B of the solid colour the pattern generator shows at
// Depth::Stream. Not black, so that a lit pipe can be told from a dead one.
constexpr uint16_t kPatternColour[3] = { 0x1000, 0x6000, 0x9000 };

struct Target {
	uint8_t  litHubp;          // HUBP of the pipe the firmware lit: its VM aperture is copied
	uint16_t encoderObjId;     // VBIOS path of the connector on Config::hpd
	uint64_t surface;          // scanout address, as the lit HUBP's address register counts
	Depth    depth;
	bool     sinkScdc;         // the sink's EDID announces SCDC (Edid::hdmi2Caps)
	uint8_t  ddcLine;          // the connector's DDC line, for the SCDC write
	Part     part;             // Sleep and Wake need neither litHubp nor surface
};

constexpr size_t kMaxSteps = 448;
constexpr size_t kMaxCmds  = 4;

struct Plan {
	ModeSet::Step steps[kMaxSteps];
	size_t        count;
	Dmub::Cmd     cmds[kMaxCmds];
	size_t        ncmds;
};

// Returns false (and `why`) if the target cannot be expressed. Every
// Op::Require step comes before the first write; Sleep and Wake have none
// (they are for the pipe Part::Light lit).
bool build(const Target &t, Plan &out, const char **why);

// The link (0 = UNIPHY A) a VBIOS encoder object drives, or -1: object ids
// 0x1e, 0x20, 0x21 are UNIPHY, UNIPHY1, UNIPHY2 with two links each, picked
// by the enum id in bits 11:8. 0x2120 is link 2.
constexpr int linkOfEncoder(uint16_t objId) {
	const int pair = (objId & 0xff) == 0x1e ? 0 : (objId & 0xff) == 0x20 ? 1 : (objId & 0xff) == 0x21 ? 2 : -1;
	const int which = ((objId >> 8) & 0xf) - 1;
	return pair < 0 || which < 0 || which > 1 ? -1 : pair * 2 + which;
}
static_assert(linkOfEncoder(0x2120) == 2 && linkOfEncoder(0x211e) == 0 && linkOfEncoder(0x2220) == 3 &&
              linkOfEncoder(0x2114) == -1, "encoder object to link");

// The colour path, one block instance per pipe: the DPP and the blender follow
// the HUBP, the formatter and the output CSC the OPP. `whole` blocks are small
// and have no LUT data port, so every register of them can be read.
struct ColourBlock { uint8_t seg; uint16_t first, stride; bool perOpp, whole; };
constexpr ColourBlock kColourBlocks[] = {
	{ 2, 0x0cc5, 0x16b, false, false },   // DPP: CNVC, DSCL, CM
	{ 2, 0x183c, 0x05a, true,  false },   // OPP: FMT, DPG, OPP_PIPE
	{ 3, 0x0000, 0x015, false, true  },   // MPCC
	{ 3, 0x007e, 0x05e, false, false },   // MPCC OGAM
	{ 3, 0x0453, 0x0b0, false, false },   // MPCC MCM
	{ 3, 0x02f2, 0x004, true,  true  },   // MPC out mux, denormalisation and clamp
	{ 3, 0x030b, 0x00d, true,  true  },   // MPC out CSC
};

// A colour-path register of pipe `pipe`, in the instance another pipe uses
// (its HUBP and OPP numbers); -1 if `dword` is not one.
constexpr int colourTwin(uint8_t seg, uint32_t dword, uint8_t pipe, uint8_t hubp, uint8_t opp) {
	for (const ColourBlock &b : kColourBlocks)
		if (seg == b.seg && dword >= b.first + pipe * b.stride && dword < b.first + (pipe + 1u) * b.stride)
			return static_cast<int>(dword) - (pipe - (b.perOpp ? opp : hubp)) * b.stride;
	return -1;
}
// CM1_CM_CONTROL, FMT1_FMT_CONTROL, MPC_OUT1_CSC_MODE, MPCC1_MPCC_TOP_SEL; HUBPREQ1 is not colour
static_assert(colourTwin(2, 0x0ed2, 1, 0, 0) == 0x0d67 && colourTwin(2, 0x189a, 1, 0, 0) == 0x1840 &&
              colourTwin(3, 0x0318, 1, 0, 0) == 0x030b && colourTwin(3, 0x0015, 1, 0, 0) == 0 &&
              colourTwin(3, 0x0015, 1, 2, 0) == 0x002a && colourTwin(2, 0x06e6, 1, 0, 0) == -1, "colour twins");

} // namespace Pipe2

#endif /* Pipe2_hpp */
