// HDR output on DisplayPort (docs/hdr-plan.md): the HDR static metadata SDP and where the stream encoder sends it.
#ifndef Hdr_hpp
#define Hdr_hpp

#include <stdint.h>

namespace Hdr {

// The VPG generic packets Linux uses (enc3_stream_encoder_update_dp_info_packets: vsc = 0, hdrsmd = 3), and the
// VPG0 / DP0 registers (DMU base 2; the other instances follow at Pipe::Reg::kDigStride).
constexpr uint32_t kVscPacket = 0, kPacket = 3;
constexpr uint32_t kVpgAccessCtrl = 0x2068, kVpgData = 0x2069, kVpgImmediate = 0x206b, kVpgStatus = 0x206c,
                   kVpgMemPwr = 0x206d, kDpSecCntl = 0x2141;
constexpr uint32_t kStatusConflict = 0x02, kStatusConflictClr = 0x10, kMemLightSleepDis = 0x01, kMemLightSleepForce = 0x10;
constexpr uint32_t kSecStreamEnable = 0x01, kSecGsp0Enable = 1u << 20, kSecGsp3Enable = 1u << 23;
// DP_MSA_MISC's MISC1 bit 6: the colorimetry is in the VSC SDP, MISC0/MISC1's own colorimetry bits are ignored.
constexpr uint32_t kMsaMisc = 0x2124, kMisc1VscColorimetry = 0x40;
// 10 bits a component on the link (DIG0/OPP0 offsets, the DIG and OPP strides apart), as Linux sets a DP stream up at
// PIPEGEN_BPC=10 (3840x2160@60): FMT's dynamic expansion (opp1_set_dyn_expansion: mode 0 = 10 bits, 1 = 8), DP_PIXEL_FORMAT's
// UNCOMPRESSED_COMPONENT_DEPTH (1 = 8, 2 = 10) and the MSA's MISC0 colour depth (bits 7:5, 1 = 8, 2 = 10) in
// DP_MSA_COLORIMETRY's top byte; DP_PIXEL_FORMAT bit 0 set is a compressed (DSC) stream, which carries 10 already.
constexpr uint32_t kFmtDynExp = 0x183f, kFmtDynExpMode = 0x10, kDpPixelFormat = 0x211f, kPixelDepthShift = 8,
                   kPixelDepthMask = 0x700, kCompressed = 1, kMsaColorimetry = 0x2120, kMisc0DepthShift = 29,
                   kMisc0DepthMask = 0xe0000000;
// A new depth is set with the video stream re-armed, as enc401_stream_encoder_dp_unblank does after the attributes:
// DP_VID_STREAM_CNTL's enable off and its status (bit 16) waited out, DP_STEER_FIFO's reset (bit 1) pulsed, the FIFO and the
// stream on again (enable with bit 9, bit 8 clear). (Whether a running stream needs it is not known: the stripes seen on the
// card were the VSC SDP's depth, vscBt2020Sdp.)
constexpr uint32_t kDpVidStreamCntl = 0x2122, kVidStreamEnable = 1, kVidStreamStatus = 0x10000, kVidStreamOn = 0x201,
                   kVidStreamMask = 0x301, kDpSteerFifo = 0x2123, kSteerFifoReset = 2, kSteerFifoEnable = 1;
// DPCD DPRX_FEATURE_ENUMERATION_LIST (0x2210) bit 3: the sink takes colorimetry from a VSC SDP.
constexpr uint32_t kDpcdFeatureList = 0x2210;
constexpr uint8_t kVscColorimetrySupported = 0x08;

// The HDR static metadata SDP as the VPG takes it: a header dword (HB0-3) and eight payload dwords, little endian, as
// amdgpu_dm_fill_hdr_info_packet builds it for DP: SDP type 0x87, version 0x13, then the CTA-861-G Dynamic Range and
// Mastering InfoFrame (version 1, 26 bytes) with EOTF = SMPTE ST 2084 (PQ).
// ponytail: mastering primaries, luminance, MaxCLL and MaxFALL stay 0 ("unknown", the sink uses its own); fill them
// from macOS's IOFBHDRMetaData or the EDID if a monitor needs them.
inline void staticMetadataSdp(uint32_t (&out)[9]) {
	for (uint32_t &d : out)
		d = 0;
	out[0] = 0x00u | 0x87u << 8 | 0x1du << 16 | (0x13u << 2) << 24;   // HB0 0, type, payload length - 1, version
	out[1] = 0x01u | 0x1au << 8 | 0x02u << 16;                         // InfoFrame version, length, EOTF = PQ
}

// A VSC SDP revision 5 (pixel encoding and colorimetry, mod_build_vsc_infopacket + set_vsc_packet_colorimetry_data):
// DB16 = RGB (0) << 4 | ITU-R BT.2020 RGB (6), DB17 = the bits a component (1 = 8, 2 = 10), VESA full range (bit 7 clear).
// With MSA MISC1 bit 6 set the sink takes the format from here, not from the MSA: a 10-bit stream announced as 8 came out
// as stripes of wrong colours (card, 2026-11-28).
inline void vscBt2020Sdp(uint32_t (&out)[9], uint32_t depth = 1) {
	for (uint32_t &d : out)
		d = 0;
	out[0] = 0x00u | 0x07u << 8 | 0x05u << 16 | 0x13u << 24;   // HB0 0, type VSC, revision 5, 0x13 payload bytes
	out[1 + 16 / 4] = 0x06u | (depth & 7u) << 8;               // DB16, DB17
}

}  // namespace Hdr

#endif
