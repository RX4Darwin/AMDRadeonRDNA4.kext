//
//  dcninit.hpp
//  AMDRadeonRDNA4
//
//  The display hardware put into the state Linux's dcn401_init_hw leaves it in, from nothing running: the
//  second part of lighting the displays when nothing set them up (after a power loss). The steps are Linux's
//  own, recorded by tools/gen/pipegen (run.sh init) into src/dcn_init_linux.inc: the DCCG stream clocks off, the
//  four link encoders' AUX channels set up, plane power gating on, every pipe blanked and power gated, the DSCs
//  gated, clock gating, the HPO block, the watermark sets and the CRB. Not in it: the display clocks (src/clk.cpp)
//  and the DCHUB global timer, which Linux expects running (the VBIOS's asic_init sets it).
//
//  Freestanding: the table becomes ModeSet steps, which the display device runs. Shared with the host test.
//

#ifndef DcnInit_hpp
#define DcnInit_hpp

#include <stddef.h>
#include <stdint.h>

#include "modeset.hpp"

namespace DcnInit {

// One recorded step: 'W' reg = value; 'U' reg = (reg & ~mask) | value; 'T' wait until (reg & mask) == value,
// arg microseconds at most; 'C' reg = the register at dword arg of the same segment; 'D' wait arg microseconds.
struct Gen {
	char        op;
	uint8_t     seg;
	uint16_t    dword;
	uint32_t    mask, value, arg;
	const char *what;
};

// Init: dcn401_init_hw from nothing. OtgOff: OTG0 stopped as Linux stops a timing generator (optc401_disable_crtc),
// for a warm card whose firmware left the boot display's running.
enum class Table { Init, OtgOff };

// How many steps a table has, and the table as steps (false: `max` is too small or a step is not one of the
// above).
size_t count(Table table = Table::Init);
bool steps(ModeSet::Step *out, size_t max, size_t &n, Table table = Table::Init);

} // namespace DcnInit

#endif
