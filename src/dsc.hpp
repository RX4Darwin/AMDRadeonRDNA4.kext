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

// The picture parameter set the sink is sent (dsc2_get_packed_pps), and what the encoder's registers take beside it.
struct Pps {
	uint8_t  bytes[128];
	uint32_t bytesPerPixel;       // the OPTC's: slice_chunk_size << 28 / slice_width, rounded up
	uint32_t rcBufferModelSize;   // rc_bits
	uint16_t chunkSize;           // slice_chunk_size
};
bool pps(const Config &c, const Timing &t, Pps &out);

}  // namespace Dsc

#endif
