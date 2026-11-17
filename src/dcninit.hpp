//
//  dcninit.hpp
//  AMDRadeonRDNA4
//
//  The display hardware put into the state Linux's dcn401_init_hw leaves it in, from nothing running: the
//  second part of lighting the displays when nothing set them up (after a power loss). The steps are Linux's
//  own, recorded by tools/gen/pipegen (run.sh init) into src/dcn_init_linux.inc: the DCCG stream clocks off, the
//  four link encoders' AUX channels set up, plane power gating on, every pipe blanked and power gated, the DSCs
//  gated, clock gating, the HPO block, the watermark sets and the CRB. Not in it: the display clocks (src/clk.cpp)
//  and the DCHUB global timer, which Linux expects running and the kext starts itself (timerSteps).
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

// Init: dcn401_init_hw from nothing. OtgOff: OTG `otg` stopped as Linux stops a timing generator
// (optc401_disable_crtc), for a warm card where it still runs (the firmware's boot display, a pipe put to sleep).
enum class Table { Init, OtgOff };
constexpr uint32_t kOtgs = 4;

// How many steps a table has, and the table as steps (false: `max` is too small, `otg` is not one of the four or
// a step is not one of the above).
size_t count(Table table = Table::Init);
bool steps(ModeSet::Step *out, size_t max, size_t &n, Table table = Table::Init, uint32_t otg = 0);

// The DCHUB global timer (DCHUBBUB_GLOBAL_TIMER_CNTL), which dcn401_init_hw requires running and only reads
// (hubbub2_get_dchub_ref_freq): nothing in Linux starts it on DCN 4, nor does the VBIOS's asic_init (it waits for the
// PSP and the SMU, no display register). Its divider by DC's rule for the DCCG reference `refKHz` (1 or 2, the hub's
// clock 40-60 MHz; 0: neither fits), and the two steps that start it as dcn10_hubbub_global_timer_enable does: the
// divider, then the enable bit (the other bits as they are).
constexpr uint16_t kTimerCntl = 0x0527;   // segment 2
constexpr uint32_t kTimerRefDivMask = 0xf, kTimerEnable = 1u << 12;
uint32_t timerRefDiv(uint32_t refKHz);
bool timerSteps(uint32_t refKHz, ModeSet::Step *out, size_t max, size_t &n);

} // namespace DcnInit

#endif
