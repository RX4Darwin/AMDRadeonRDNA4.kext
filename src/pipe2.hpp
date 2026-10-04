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
//  the lit pipe's VM aperture.
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
// Op::Require step comes before the first write.
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

} // namespace Pipe2

#endif /* Pipe2_hpp */
