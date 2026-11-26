//
// dsc.hpp: Display Stream Compression for a DisplayPort stream (docs/dsc-plan.md step 2): the configuration Linux's amdgpu
// picks for a sink, a timing and a link (dc_dsc_compute_config, dc/dsc/dc_dsc.c), and the 128-byte picture parameter set it
// sends (dsc_prepare_config, dcn20_dsc.c; _do_calc_rc_params, dml/dsc/rc_calc_fpu.c; drm_dsc_compute_rc_parameters and
// drm_dsc_pps_payload_pack, drm_dsc_helper.c). RGB only (the desktop's format), DP SST, no eDP or HDMI FRL. Integer only:
// bits per pixel are sixteenths, where Linux uses float in the RC parameters (held equal on the host, test/dsc-test.cpp).
// Freestanding: no kernel headers.
//

#ifndef Dsc_hpp
#define Dsc_hpp

#include <stddef.h>
#include <stdint.h>

#include "dptrain.hpp"

namespace Dsc {

struct Timing {
	uint32_t hActive, vActive, hTotal, vTotal;
	uint32_t pixelClockKHz;
	uint8_t  bpc;                 // 8, 10 or 12
};

// What the source can do: DCN 4.01's encoder (dsc401_get_single_enc_caps) with four engines and ODM combine up to 4, and
// the clocks that bound it (clk_mgr get_max_clock_khz: DSCCLK for the encoders' throughput, DISPCLK for the pipe's).
struct Source {
	uint32_t maxDscclkKHz;
	uint32_t maxDispclkKHz;
};

struct Config {
	uint32_t slicesH, slicesV;
	uint32_t sliceWidth, sliceHeight;
	uint32_t bpp16;               // target bits per pixel, in sixteenths
	bool     blockPrediction;
	uint8_t  lineBufferBits;
	uint8_t  versionMinor;        // 1 or 2
};

// A DP link's data rate (dp_link_bandwidth_kbps, 8b/10b): 27 MHz x 10 bits per DP byte per lane x rate code, 80 % of it, and
// 97 % of that with FEC (which DSC needs).
uint32_t linkKbps(uint8_t lanes, uint8_t rateCode, bool fec);

// The stream's uncompressed bandwidth (dc_bandwidth_in_kbps_from_timing, RGB): pixel clock x 3 x bpc.
uint32_t streamKbps(const Timing &t);

// Linux's choice for the sink and the link (dc_dsc_compute_config with its default policy and options): false when DSC is
// not possible or not needed (the stream fits the link uncompressed: Linux does not enable it then).
bool choose(const DpTrain::DscCaps &sink, const Timing &t, uint32_t linkKbps, const Source &src, Config &out);

// The computed rate-control parameters (drm_dsc_config's, those that depend on the mode): the PPS and the encoder's
// registers both take them.
struct Params {
	uint16_t sliceWidth, sliceHeight;
	uint16_t initialXmitDelay, initialDecDelay;
	uint8_t  initialScaleValue;
	uint16_t scaleIncrement, scaleDecrement;
	uint8_t  firstLineBpgOffset;
	uint16_t nflBpgOffset, sliceBpgOffset;
	uint16_t initialOffset, finalOffset;
	uint8_t  flatnessMin, flatnessMax;
	uint8_t  quantLimit;          // rc_quant_incr_limit0 and 1
	int8_t   qpMin[15], qpMax[15], bpgOffset[15];   // rc_range_params
};

// The picture parameter set the sink is sent (dsc2_get_packed_pps), and what the encoder's registers take beside it.
struct Pps {
	uint8_t  bytes[128];
	uint32_t bytesPerPixel;       // the OPTC's: slice_chunk_size << 28 / slice_width, rounded up
	uint32_t rcBufferModelSize;   // rc_bits
	uint16_t chunkSize;           // slice_chunk_size
	Params   params;
};
bool pps(const Config &c, const Timing &t, Pps &out);

// DCN 4.01's DSC engine registers for the stream (dsc_write_to_registers), in the order Linux writes them; the
// offsets are engine 0's (the engines are kEngineStride dwords apart, segment 2). `odm`: the engine is one of several.
constexpr size_t kEngineRegs = 28;
constexpr uint32_t kEngineStride = 0x5c;
extern const uint16_t kEngineReg[kEngineRegs];
void engineRegs(const Config &c, const Timing &t, const Pps &p, bool odm, uint32_t (&out)[kEngineRegs]);

// The PPS as the stream encoder sends it: four secondary-data packets of a header dword (HB0-3) and eight payload
// dwords, little endian (enc3_dp_set_dsc_pps_info_packet through the VPG's generic packets).
void ppsPackets(const Pps &p, uint32_t (&out)[4][9]);

}  // namespace Dsc

#endif
