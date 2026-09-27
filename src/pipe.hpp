//
//  pipe.hpp
//  RDNA4FB
//
//  Identify the display pipe the GOP lit — timing generator (OTG), DIG
//  front-end, DIG back-end / PHY, OPP and HUBP — plus its signal type (DP
//  or HDMI TMDS) and the timing it programmed, from DCN 4.1.0 register
//  state. Upstream only ever booted on a DP monitor wired to OTG0/DP0 and
//  hard-coded that pipe; an HDMI boot display lands wherever the GOP put it.
//
//  Register offsets and field layouts are from Linux
//  drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_{offset,sh_mask}.h;
//  mode encodings from dcn401_dio_{stream,link}_encoder.c.
//
//  Freestanding: register reads go through a caller-supplied function, so
//  the host test harness can run discovery against synthetic register
//  images. Shared by the kext and tools/atomdump.cpp.
//

#ifndef Pipe_hpp
#define Pipe_hpp

#include <stdint.h>
#include "edid.hpp"

namespace Pipe {

constexpr uint8_t kNone   = 0xff;
constexpr uint8_t kMaxOtg = 4;
constexpr uint8_t kMaxDig = 4;   // DIG0..3 front/back-ends exist in dcn_4_1_0

// DIG_FE_MODE / DIG_BE_MODE values.
enum class Signal : uint8_t {
	DpSst   = 0,
	Dvi     = 2,
	Hdmi    = 3,
	DpMst   = 5,
	Unknown = 0xfe,
	None    = 0xff,
};

// DCN register read: base segment index + dword offset within the DMU IP.
// Must return 0xFFFFFFFF for anything unreadable.
using ReadFn = uint32_t (*)(void *ctx, uint8_t baseIdx, uint32_t dword);

// dword offsets (instance 0) and per-instance strides. base_idx in comments.
namespace Reg {
// OTG (base 2, stride 0x80)
constexpr uint32_t kOtgStride        = 0x80;
constexpr uint32_t kOtgHTotal        = 0x1b2a;
constexpr uint32_t kOtgHBlank        = 0x1b2b;   // START [14:0], END [30:16]
constexpr uint32_t kOtgHSyncA        = 0x1b2c;   // START [14:0], END [30:16]
constexpr uint32_t kOtgHSyncACntl    = 0x1b2d;   // POL [0]
constexpr uint32_t kOtgVTotal        = 0x1b2f;
constexpr uint32_t kOtgVBlank        = 0x1b38;
constexpr uint32_t kOtgVSyncA        = 0x1b39;
constexpr uint32_t kOtgVSyncACntl    = 0x1b3a;   // POL [0]
constexpr uint32_t kOtgControl       = 0x1b43;   // MASTER_EN [0], CURRENT_MASTER_EN_STATE [16]
constexpr uint32_t kOtgStatus        = 0x1b49;   // V_BLANK [0]
constexpr uint32_t kOtgFrameCount    = 0x1b4d;   // OTG_FRAME_COUNT [23:0]
// ODM / OPTC (base 2, stride 0x10)
constexpr uint32_t kOdmStride        = 0x10;
constexpr uint32_t kOptcDataSource   = 0x1acb;   // OPTC_SEG0_SRC_SEL [19:16] = OPP
// MPC (base 3, stride 0x15)
constexpr uint32_t kMpccStride       = 0x15;
constexpr uint32_t kMpccOppId        = 0x0002;   // MPCC_OPP_ID [3:0], 0xf = none
// OPP DPG (base 2, stride 0x5a)
constexpr uint32_t kOppStride        = 0x5a;
constexpr uint32_t kDpgControl       = 0x1854;   // DPG_EN [0]
// HUBP (base 2, stride 0xdc)
constexpr uint32_t kHubpStride       = 0xdc;
constexpr uint32_t kHubpViewportDim  = 0x05eb;   // WIDTH [13:0], HEIGHT [29:16]
constexpr uint32_t kHubpSurfacePitch = 0x0607;   // PITCH [15:0], pixels - 1
// DIG front/back-end (base 2, stride 0x124)
constexpr uint32_t kDigStride        = 0x124;
constexpr uint32_t kDigFeCntl        = 0x2093;   // DIG_SOURCE_SELECT [2:0] = OTG
constexpr uint32_t kDigFeClkCntl     = 0x2094;   // DIG_FE_MODE [2:0], DIG_FE_CLK_EN [4]
constexpr uint32_t kDigFeEnCntl      = 0x2095;   // DIG_FE_ENABLE [0]
constexpr uint32_t kHdmiControl      = 0x209e;
constexpr uint32_t kHdmiGc           = 0x20a8;   // HDMI_GC_AVMUTE [0]
constexpr uint32_t kDigBeClkCntl     = 0x20bb;   // DIG_BE_MODE [2:0], DIG_BE_CLK_EN [4]
constexpr uint32_t kDigBeCntl        = 0x20bc;   // DIG_FE_SOURCE_SELECT [14:8], DIG_HPD_SELECT [30:28]
constexpr uint32_t kDigBeEnCntl      = 0x20bd;   // DIG_BE_ENABLE [0]
// DIGn_STREAM_MAPPER_CONTROL (base 2, stride 1, DIG0..6)
constexpr uint32_t kStreamMapper     = 0x1f0d;   // DIG_STREAM_LINK_TARGET [2:0]
} // namespace Reg

struct State {
	uint8_t otg  { kNone };   // lit timing generator
	uint8_t dig  { kNone };   // DIG front-end with DIG_SOURCE_SELECT == otg
	uint8_t link { kNone };   // back-end / PHY the front-end is mapped to
	uint8_t hpd  { 0 };       // DIG_HPD_SELECT of the back-end (1-based, 0 none)
	uint8_t opp  { kNone };   // OPTC_SEG0_SRC_SEL of the OTG's ODM
	uint8_t hubp { kNone };   // MPCC whose MPCC_OPP_ID == opp (MPCC n <-> HUBP n)
	Signal  signal { Signal::None };   // from DIG_BE_MODE (FE mode if BE unreadable)

	// Raw OTG images, as the GOP programmed them.
	uint32_t hTotal { 0 }, hBlank { 0 }, hSync { 0 };
	uint32_t vTotal { 0 }, vBlank { 0 }, vSync { 0 };
	bool     hSyncNeg { false }, vSyncNeg { false };
	// Scanout surface as HUBP sees it.
	uint32_t viewportW { 0 }, viewportH { 0 }, pitchPx { 0 };

	bool valid() const { return otg != kNone; }
	bool isTmds() const { return signal == Signal::Hdmi || signal == Signal::Dvi; }
};

// Locate the lit pipe. Returns false if no OTG is running.
bool discover(ReadFn rd, void *ctx, State &out);

// Inverse of OtgTiming::compute: active / blank / porch / sync widths and
// polarity from the OTG images. The pixel clock is not in these registers
// (for TMDS it lives in the PHY PLL), so pixelClockKHz is left 0 for the
// caller to fill from the EDID or a frame-rate measurement.
bool timingFromOtg(const State &s, Edid::DetailedTiming &t);

// Pixel clock from a measured frame period: htotal * vtotal / period.
uint32_t pixelClockFromFramePeriod(const State &s, uint64_t framePeriodNs);

const char *signalName(Signal s);

} // namespace Pipe

#endif /* Pipe_hpp */
