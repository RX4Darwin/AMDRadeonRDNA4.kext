//
//  dptrain.hpp
//  AMDRadeonRDNA4
//
//  DisplayPort link training (8b/10b): what brings a link up before a stream
//  can be sent over it. The firmware does this for the boot display and the
//  driver has never had to; a second DisplayPort display, or the boot
//  display after an unplug, needs it.
//
//  The procedure is amdgpu DC's (dp_enable_link_phy, dp_perform_link_training
//  and perform_link_training_with_retries in dc/link/protocols): transmitter
//  on, the sink woken and told the rate and lane count, clock recovery on
//  TPS1, channel equalisation on TPS4, TPS3 or TPS2, each with the lanes'
//  voltage swing and pre-emphasis raised as the sink asks, then the pattern
//  off and one look at the link status. tools/gen/pipegen runs Linux's own code
//  against simulated sinks (its "dplink" scenario) and test/atomdump.cpp
//  holds this one's sequence of DPCD transfers, patterns, drive settings and
//  delays against Linux's, sink by sink (test/ref/dp_train_linux.inc).
//
//  Left out, unlike amdgpu: link training tunable PHY repeaters (LTTPR), FEC,
//  128b/132b (UHBR) links, granting POST_LT_ADJ_REQ, and falling back to a
//  lower rate or lane count: amdgpu does that while it detects the display,
//  not here.
//
//  Freestanding: everything the hardware and the sink do comes through Io.
//  Shared by the kext and the host test.
//

#ifndef DpTrain_hpp
#define DpTrain_hpp

#include <stddef.h>
#include <stdint.h>

namespace DpTrain {

// Rates as the sink's LINK_BW_SET takes them: the lane's bit rate / 0.27 Gbit/s.
constexpr uint8_t kRbr = 0x06, kHbr = 0x0a, kHbr2 = 0x14, kHbr3 = 0x1e;

struct Link {
	uint8_t lanes;   // 1, 2 or 4
	uint8_t rate;    // kRbr .. kHbr3
	bool    fec;     // forward error correction (a DSC stream needs it): made ready on both ends before training
};

// The symbol clock of a rate (what the transmitter is enabled with), and what
// a link carries: 8 of every 10 bits on each lane.
constexpr uint32_t symbolClockKHz(uint8_t rate) { return rate * 27000u; }
constexpr uint32_t bandwidthKbps(const Link &l) { return l.rate * 270000u / 10 * 8 * l.lanes; }
static_assert(symbolClockKHz(kHbr2) == 540000 && bandwidthKbps(Link { 4, kHbr2 }) == 17280000, "HBR2");

// The training pattern on the main link; the numbers are the sink's
// TRAINING_PATTERN_SELECT values.
enum Pattern : uint8_t { Video = 0, Tps1 = 1, Tps2 = 2, Tps3 = 3, Tps4 = 7 };

// What training needs to know of the sink: its receiver capability field.
// A DP 1.4 sink's Display Stream Compression decoder, from DPCD 0x060..0x06F (DP_DSC_SUPPORT on), as Linux's
// dc_dsc_parse_dsc_dpcd (dc/dsc/dc_dsc.c) reads it; a branch device's extra caps (0x0A0) are not read.
struct DscCaps {
	uint8_t  version;            // DP_DSC_REV: major in [3:0], minor in [7:4] (0x11 = 1.1, 0x21 = 1.2)
	uint32_t rcBufferBytes;      // the rate buffer: (RC_BUF_SIZE + 1) blocks of 1, 4, 16 or 64 KiB
	uint8_t  sliceCaps1;         // DP_DSC_SLICE_CAP_1: 1, 2, 4, 6, 8, 10, 12 slices per line, bit 0 on
	uint8_t  sliceCaps2;         // DP_DSC_SLICE_CAP_2: 16, 20, 24 slices
	uint8_t  lineBufferBits;     // line buffer bit depth, 8 to 16
	bool     blockPrediction;
	uint16_t maxBitsPerPixel16;  // DP_DSC_MAX_BITS_PER_PIXEL (eDP: bits per pixel in 1/16, 0 for DP)
	uint8_t  colorFormats;       // DP_DSC_DEC_COLOR_FORMAT_CAP: RGB 1, 4:4:4 2, simple 4:2:2 4, native 4:2:2 8, native 4:2:0 16
	uint8_t  colorDepths;        // DP_DSC_DEC_COLOR_DEPTH_CAP: 8 bpc 2, 10 bpc 4, 12 bpc 8
	uint32_t throughputMode0;    // MPixels/s with one pixel per clock (plus the DP 2.1 delta)
	uint32_t throughputMode1;    // ... with two (4:2:0 and native 4:2:2)
	uint32_t maxSliceWidth;      // pixels (DP_DSC_MAX_SLICE_WIDTH x 320)
	uint8_t  bppIncrementDiv;    // the bits-per-pixel step is 1 / this: 16, 8, 4, 2 or 1
};
// False if the sink has no DSC decoder or a field holds a value Linux refuses.

struct Sink {
	uint8_t rev;        // DPCD_REV, 0x12 = 1.2
	uint8_t maxRate;    // MAX_LINK_RATE
	uint8_t maxLanes;   // MAX_LANE_COUNT
	bool    tps3, tps4; // training patterns it supports beyond TPS2
	bool    hasDsc;     // a DSC decoder (DPCD 1.4 and DP_DSC_SUPPORT), described by dsc
	DscCaps dsc;
};
// From DPCD 0x000 on (at least four bytes). False if they are not a sink's.
bool parseCaps(const uint8_t *caps, size_t len, Sink &out);
// DPCD 0x2200 must be advertised and describe a legacy 8b/10b link that this driver can train.
bool parseExtendedCaps(const uint8_t (&base)[16], const uint8_t (&extended)[16], Sink &out);
// From DPCD 0x060 on: false if the sink has no DSC decoder or a field holds a value Linux refuses.
bool parseDscCaps(const uint8_t dsc[16], DscCaps &out);



// Whether a link is up, from the sink's LANE0_1_STATUS, LANE2_3_STATUS and
// LANE_ALIGN_STATUS_UPDATED (DPCD 0x202..0x204): clock recovery, equalisation
// and symbol lock on every lane, and the lanes aligned (what
// dp_check_link_loss_status looks at).
bool linkUp(const uint8_t status[3], uint8_t lanes);

// The smallest link of the sink's that carries a stream: amdgpu's order
// (decide_dp_link_settings), more lanes before a higher rate. False if even
// the sink's largest is too small.
bool pickLink(const Sink &sink, uint32_t pixelClockKHz, uint32_t bitsPerPixel, Link &out);

// The hardware and the sink. Each returns false if it could not be done,
// which ends the training.
struct Io {
	void *ctx;
	bool (*read)(void *ctx, uint32_t address, uint8_t *data, size_t len);          // the sink's DPCD, over AUX
	bool (*write)(void *ctx, uint32_t address, const uint8_t *data, size_t len);
	bool (*phyOn)(void *ctx, const Link &link);     // transmitter on at this rate and lane count
	bool (*phyOff)(void *ctx);
	bool (*pattern)(void *ctx, Pattern p);          // what the transmitter sends
	bool (*drive)(void *ctx, uint8_t lane, uint8_t swing, uint8_t preEmphasis);   // one lane, levels 0..3
	void (*delayUs)(void *ctx, uint32_t us);
	bool (*fecReady)(void *ctx, bool ready);        // the link encoder's FEC ready (Link::fec only)
};

enum class Result : uint8_t {
	Ok,
	BadLink,         // not a link this sink has
	Io,              // the hardware or the AUX channel failed
	ClockRecovery,   // no lock on TPS1 at any drive the sink asked for
	Equalisation,    // no equalisation, symbol lock or alignment in six tries
	ClockLost,       // clock recovery went away during equalisation
	LinkLost,        // trained, but the status afterwards said otherwise
};
const char *resultName(Result r);

struct Report {
	Result  result;
	uint8_t attempts;              // 1..kAttempts
	Result  tries[4];              // what each attempt ended in
	uint8_t swing, preEmphasis;    // the drive the lanes ended on
	uint8_t status[6];             // the last LANE0_1_STATUS .. ADJUST_REQUEST_LANE2_3 read
};

// amdgpu trains up to four times at the same settings, with the sink put to
// sleep, the transmitter off and a growing pause in between.
constexpr uint8_t kAttempts = 4;

// Bring `link` up. After Result::Ok the link carries the idle pattern and
// the transmitter is on; after any other the transmitter is as the last
// attempt left it (on, for a training failure), for the caller to turn off.
Result bringUp(const Io &io, const Sink &sink, const Link &link, Report *report = nullptr);

} // namespace DpTrain

#endif /* DpTrain_hpp */
