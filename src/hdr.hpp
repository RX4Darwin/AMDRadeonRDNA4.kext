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
// DB16 = RGB (0) << 4 | ITU-R BT.2020 RGB (6), DB17 = 8 bits a component (1), VESA full range (bit 7 clear).
inline void vscBt2020Sdp(uint32_t (&out)[9]) {
	for (uint32_t &d : out)
		d = 0;
	out[0] = 0x00u | 0x07u << 8 | 0x05u << 16 | 0x13u << 24;   // HB0 0, type VSC, revision 5, 0x13 payload bytes
	out[1 + 16 / 4] = 0x06u | 0x01u << 8;                      // DB16, DB17
}

}  // namespace Hdr

#endif
