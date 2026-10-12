//
//  dmub.hpp
//  RDNA4FB
//
//  Minimal DMUB (display microcontroller) mailbox ABI, from Linux
//  drivers/gpu/drm/amd/display/dmub/inc/dmub_cmd.h. The GOP leaves DMUB
//  running with an initialized inbox1 ring (verified on hardware
//  2026-07-12); commands are 64-byte entries written at the ring write
//  pointer, submitted by advancing DMCUB_INBOX1_WPTR, consumed when the
//  firmware advances RPTR.
//
//  On DCN 3.1+ the display bring-up that used to be AtomBIOS bytecode
//  (transmitter control, pixel clock) is issued through these commands —
//  this is the mode-setting path for the second display.
//
//  Freestanding; shared by the kext and the host test harness.
//

#ifndef dmub_hpp
#define dmub_hpp

#include <stdint.h>

namespace Dmub {

constexpr uint32_t kCmdSize = 64;   // every ring entry, header included

// enum dmub_cmd_type (stable ABI per the header)
constexpr uint8_t CmdNull             = 0;
constexpr uint8_t CmdQueryFeatureCaps = 6;    // harmless: FW reports caps
constexpr uint8_t CmdUpdateCursorInfo = 68;   // PSR cursor mirror: FW programs cursor regs
constexpr uint8_t CmdVbios            = 128;  // "VBIOS" services in FW

// enum dmub_cmd_vbios_type
constexpr uint8_t VbiosDigxEncoderControl     = 0;
constexpr uint8_t VbiosDig1TransmitterControl = 1;
constexpr uint8_t VbiosSetPixelClock          = 2;
constexpr uint8_t VbiosEnableDispPowerGating  = 3;
constexpr uint8_t VbiosLvtmaControl           = 15;
constexpr uint8_t VbiosTransmitterQueryDpAlt  = 26;
constexpr uint8_t VbiosDomainControl          = 28;
constexpr uint8_t VbiosTransmitterSetPhyFsm   = 29;

// Human-readable label for a command's (type, sub_type) — for decoding the
// GOP's own recorded command ring. Covers the display-bring-up commands;
// unknown ids fall through to the caller's numeric print.
inline const char *cmdLabel(uint8_t type, uint8_t subType) {
	if (type != CmdVbios) {
		switch (type) {
		case CmdNull:             return "NULL";
		case CmdQueryFeatureCaps: return "QUERY_FEATURE_CAPS";
		case CmdUpdateCursorInfo: return "UPDATE_CURSOR_INFO";
		default:                  return "?";
		}
	}
	switch (subType) {
	case VbiosDigxEncoderControl:     return "VBIOS/DIGX_ENCODER_CONTROL";
	case VbiosDig1TransmitterControl: return "VBIOS/DIG1_TRANSMITTER_CONTROL";
	case VbiosSetPixelClock:          return "VBIOS/SET_PIXEL_CLOCK";
	case VbiosEnableDispPowerGating:  return "VBIOS/ENABLE_DISP_POWER_GATING";
	case VbiosLvtmaControl:           return "VBIOS/LVTMA_CONTROL";
	case VbiosTransmitterQueryDpAlt:  return "VBIOS/TRANSMITTER_QUERY_DP_ALT";
	case VbiosDomainControl:          return "VBIOS/DOMAIN_CONTROL";
	case VbiosTransmitterSetPhyFsm:   return "VBIOS/TRANSMITTER_SET_PHY_FSM";
	default:                          return "VBIOS/?";
	}
}

// GPINT, the register command channel (dmub_srv_send_gpint_command): the host
// writes DMCUB_GPINT_DATAIN1 = status 1 [31:28] | command [27:16] | param
// [15:0]; the firmware clears the status nibble once it has taken the
// command and leaves a reply in DMCUB_SCRATCH7.
constexpr uint32_t GpintGetFwVersion = 1;   // DMUB_GPINT__GET_FW_VERSION
constexpr uint32_t gpintWord(uint32_t command, uint16_t param) {
	return (1u << 28) | ((command & 0xfff) << 16) | param;
}
constexpr uint32_t gpintAcked(uint32_t word) { return word & 0x0fffffff; }
static_assert(gpintWord(GpintGetFwVersion, 0) == 0x10010000 &&
              gpintAcked(0x10010000) == 0x00010000, "GPINT encoding");

// struct dmub_cmd_header, encoded manually to avoid bitfield ABI surprises:
//   type[7:0] | sub_type[15:8] | ret_status[16] | multi_cmd_pending[17] |
//   is_reg_based[18] | reserved[23:19] | payload_bytes[29:24] | rsvd[31:30]
inline uint32_t headerWord(uint8_t type, uint8_t subType, uint8_t payloadBytes,
                           bool regBased = false, bool multiPending = false) {
	return static_cast<uint32_t>(type) |
	       (static_cast<uint32_t>(subType) << 8) |
	       (multiPending ? (1u << 17) : 0) |
	       (regBased ? (1u << 18) : 0) |
	       ((static_cast<uint32_t>(payloadBytes) & 0x3f) << 24);
}

// A full ring entry: header dword + 15 payload dwords.
using Cmd = uint32_t[kCmdSize / 4];

// Little-endian byte packing into payload dwords (payload starts at dword 1).
inline void putByte(Cmd cmd, uint32_t payloadOffset, uint8_t v) {
	uint32_t &w = cmd[1 + payloadOffset / 4];
	uint32_t shift = (payloadOffset % 4) * 8;
	w = (w & ~(0xffu << shift)) | (static_cast<uint32_t>(v) << shift);
}
inline void putWord(Cmd cmd, uint32_t payloadOffset, uint32_t v) {
	cmd[1 + payloadOffset / 4] = v;   // callers only use aligned offsets
}
inline void clear(Cmd cmd) {
	for (uint32_t i = 0; i < kCmdSize / 4; i++)
		cmd[i] = 0;
}

// Values used by the VBIOS-family payloads (atomfirmware.h).
constexpr uint8_t EncoderModeDp   = 0;
constexpr uint8_t EncoderModeDvi  = 2;
constexpr uint8_t EncoderModeHdmi = 3;
constexpr uint8_t TransmitterActionDisable = 0;
constexpr uint8_t TransmitterActionEnable  = 1;
constexpr uint8_t EncoderActionStreamSetup = 0x0f;

// DMUB_CMD__VBIOS / DIG1_TRANSMITTER_CONTROL, payload
// dmub_dig_transmitter_control_data_v1_7 (60 bytes). What
// transmitter_control_dmcub_v1_7 sends: phyid 0 = UNIPHYA, digmode HDMI = 3,
// 4 lanes for TMDS, symclk in 10 kHz already including the deep-colour
// ratio, hpdsel 1-based, digfe_sel/connobj_id 0 as Linux sends them.
struct TransmitterControl {
	uint8_t  phyId;
	uint8_t  action;
	uint8_t  digMode;
	uint8_t  laneNum;
	uint32_t symclk10kHz;
	uint8_t  hpdSel;
	uint8_t  digFeSel;
	uint8_t  connObjId;
	uint8_t  hpoInstance;
};
inline void buildTransmitterControl(Cmd cmd, const TransmitterControl &p) {
	clear(cmd);
	cmd[0] = headerWord(CmdVbios, VbiosDig1TransmitterControl, 60);
	putByte(cmd, 0, p.phyId);
	putByte(cmd, 1, p.action);
	putByte(cmd, 2, p.digMode);
	putByte(cmd, 3, p.laneNum);
	putWord(cmd, 4, p.symclk10kHz);
	putByte(cmd, 8, p.hpdSel);
	putByte(cmd, 9, p.digFeSel);
	putByte(cmd, 10, p.connObjId);
	putByte(cmd, 11, p.hpoInstance);
}

// DMUB_CMD__VBIOS / SET_PIXEL_CLOCK, payload set_pixel_clock_parameter_v1_7
// (16 bytes). pll_id ATOM_COMBOPHY_PLL0..5 = 20..25; crtc_id = OTG instance;
// deep_color_ratio 0 = 8 bpc.
struct SetPixelClock {
	uint32_t pixclk100Hz;
	uint8_t  pllId;
	uint8_t  encoderObjId;
	uint8_t  encoderMode;
	uint8_t  miscInfo;
	uint8_t  crtcId;
	uint8_t  deepColorRatio;
};
inline void buildSetPixelClock(Cmd cmd, const SetPixelClock &p) {
	clear(cmd);
	cmd[0] = headerWord(CmdVbios, VbiosSetPixelClock, 16);
	putWord(cmd, 0, p.pixclk100Hz);
	putByte(cmd, 4, p.pllId);
	putByte(cmd, 5, p.encoderObjId);
	putByte(cmd, 6, p.encoderMode);
	putByte(cmd, 7, p.miscInfo);
	putByte(cmd, 8, p.crtcId);
	putByte(cmd, 9, p.deepColorRatio);
}

// DMUB_CMD__VBIOS / DIGX_ENCODER_CONTROL, payload
// dig_encoder_stream_setup_parameters_v1_5 (12-byte union). Linux leaves
// bitpercolor 0 on DCN401 (enc401 never sets color_depth).
struct DigEncoderStreamSetup {
	uint8_t  digId;
	uint8_t  action;
	uint8_t  digMode;
	uint8_t  laneNum;
	uint32_t pclk10kHz;
	uint8_t  bitPerColor;
	uint8_t  dpLinkRate270MHz;
};
inline void buildDigEncoderStreamSetup(Cmd cmd, const DigEncoderStreamSetup &p) {
	clear(cmd);
	cmd[0] = headerWord(CmdVbios, VbiosDigxEncoderControl, 12);
	putByte(cmd, 0, p.digId);
	putByte(cmd, 1, p.action);
	putByte(cmd, 2, p.digMode);
	putByte(cmd, 3, p.laneNum);
	putWord(cmd, 4, p.pclk10kHz);
	putByte(cmd, 8, p.bitPerColor);
	putByte(cmd, 9, p.dpLinkRate270MHz);
}

} // namespace Dmub

#endif /* dmub_hpp */
