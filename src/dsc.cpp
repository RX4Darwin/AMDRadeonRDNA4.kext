//
// dsc.cpp: see dsc.hpp. Each function names the Linux one it follows (Linux 6addb4f38557, lib/linux).
//

#include "dsc.hpp"
#include "amdgpu/dsc_qp444.h"

namespace Dsc {

namespace {

// Slice counts as dc_dsc.c's dsc_enc_slice_caps sees them: 1, 2, 4, 8, 12, 16 (get_available_dsc_slices)
constexpr int kCounts[6] = { 1, 2, 4, 8, 12, 16 };

int available(uint8_t mask, int *out) {
	int n = 0;
	for (int i = 0; i < 6; i++)
		if (mask >> i & 1)
			out[n++] = kCounts[i];
	return n;
}

int maxSlices(uint8_t mask) {
	int a[6];
	const int n = available(mask, a);
	return n ? a[n - 1] : 0;
}

int decSlices(uint8_t mask, int num) {   // dec_num_slices
	int a[6];
	const int n = available(mask, a);
	if (n == 0 && num > 0)
		return num + 1;
	int next = num;
	for (int i = n - 1; i >= 0; i--)
		if (num > a[i]) {
			next = a[i];
			break;
		}
	if (next == num)
		next = num - 1 < 0 ? 0 : num - 1;
	return next;
}

int incSlices(uint8_t mask, int num) {   // inc_num_slices
	int a[6];
	const int n = available(mask, a);
	if (n == 0)
		return num + 1;
	int next = num;
	for (int i = 0; i < n; i++)
		if (num < a[i]) {
			next = a[i];
			break;
		}
	return next == num ? num + 1 : next;
}

int fitSlicesUp(uint8_t mask, int num) {   // fit_num_slices_up
	int a[6];
	const int n = available(mask, a);
	if (n == 0)
		return num + 1;
	for (int i = 0; i < n; i++)
		if (num <= a[i])
			return a[i];
	return num;
}

uint64_t ceilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

// dc_dsc_stream_bandwidth_overhead_in_kbps (DP): slices x v_total x 256 bits a frame, at the refresh rate
uint32_t overheadKbps(const Timing &t, uint32_t slices) {
	return static_cast<uint32_t>(ceilDiv(static_cast<uint64_t>(slices) * 256 * t.pixelClockKHz, t.hTotal));
}

// dc_dsc_stream_bandwidth_in_kbps
uint32_t dscStreamKbps(const Timing &t, uint32_t bpp16, uint32_t slices) {
	return static_cast<uint32_t>(ceilDiv(static_cast<uint64_t>(t.pixelClockKHz) * bpp16, 16)) + overheadKbps(t, slices);
}

// compute_bpp_x16_from_target_bandwidth
uint32_t bppFromBandwidth(uint32_t kbps, const Timing &t, uint32_t slices, uint32_t div) {
	const uint32_t over = overheadKbps(t, slices);
	if (kbps <= over)
		return 0;
	const uint64_t steps = static_cast<uint64_t>(kbps - over) * div / t.pixelClockKHz;
	return static_cast<uint32_t>(steps * 16 / div);
}

}  // namespace

uint32_t linkKbps(uint8_t lanes, uint8_t rateCode, bool fec) {
	uint32_t efficiency = 8000;                          // DATA_EFFICIENCY_8b_10b_x10000
	if (fec)
		efficiency = efficiency / 100 * 97;              // DATA_EFFICIENCY_8b_10b_FEC_EFFICIENCY_x100
	const uint32_t perLane = rateCode * 27000u * 10;     // LINK_RATE_REF_FREQ_IN_KHZ x BITS_PER_DP_BYTE
	return perLane * lanes / 10000 * efficiency;
}

uint32_t streamKbps(const Timing &t) {
	return t.pixelClockKHz * 3u * t.bpc;
}

bool choose(const DpTrain::DscCaps &sink, const Timing &t, uint32_t link, const Source &src, Config &out) {
	out = Config {};
	const int picWidth = static_cast<int>(t.hActive), picHeight = static_cast<int>(t.vActive);
	const int pclk = static_cast<int>(t.pixelClockKHz);
	if (!picWidth || !picHeight || !pclk || !t.hTotal)
		return false;

	// dsc401_get_single_enc_caps and build_dsc_enc_caps: DSC 1.2, 1-4 slices an engine and four engines (ODM 4:1), a
	// 13-bit line buffer, slices up to 5184 wide, 1/16 bpp steps, 3 pixels a DSCCLK an engine
	constexpr int kEncMaxSliceWidth = 5184;
	const uint32_t encThroughputMps = src.maxDscclkKHz * 3 / 1000 * 4;

	// get_min_dsc_slice_count_for_odm: what DISPCLK and the encoder's slice width need
	const uint32_t dispclk = src.maxDispclkKHz ? src.maxDispclkKHz : encThroughputMps * 1000;
	int minSlices = 1;
	if (dispclk) {
		const int byClock = static_cast<int>(ceilDiv(static_cast<uint64_t>(pclk), dispclk));
		const int byWidth = static_cast<int>(ceilDiv(static_cast<uint64_t>(picWidth), kEncMaxSliceWidth));
		minSlices = byClock > byWidth ? byClock : byWidth;
	}

	// intersect_dsc_caps (the encoder has every count below, 1/2/4 on one engine and 8/12/16 across them)
	const uint8_t common = static_cast<uint8_t>((sink.sliceCaps1 & 0x01 ? 1 : 0) | (sink.sliceCaps1 & 0x02 ? 2 : 0) |
	                                            (sink.sliceCaps1 & 0x08 ? 4 : 0) | (sink.sliceCaps1 & 0x20 ? 8 : 0) |
	                                            (sink.sliceCaps1 & 0x80 ? 16 : 0) | (sink.sliceCaps2 & 0x01 ? 32 : 0));
	const uint8_t version = sink.version < 0x21 ? sink.version : 0x21;
	const uint8_t lineBuffer = sink.lineBufferBits < 13 ? sink.lineBufferBits : 13;
	const int maxSliceWidth = static_cast<int>(sink.maxSliceWidth) < kEncMaxSliceWidth ? static_cast<int>(sink.maxSliceWidth) : kEncMaxSliceWidth;
	const uint32_t div = sink.bppIncrementDiv < 16 ? sink.bppIncrementDiv : 16;
	if (!version || !common || !lineBuffer || !(sink.colorFormats & 0x1f) || !(sink.colorDepths & 0x0e) || !maxSliceWidth)
		return false;

	// setup_dsc_config, RGB
	if (!(sink.colorFormats & 0x01))
		return false;
	const uint8_t depthBit = t.bpc == 8 ? 0x02 : t.bpc == 10 ? 0x04 : t.bpc == 12 ? 0x08 : 0;
	if (!(sink.colorDepths & depthBit))
		return false;
	int maxH = maxSlices(common);
	while (maxH > 0 && picWidth % maxH != 0)
		maxH = decSlices(common, maxH);
	int minH = static_cast<int>(ceilDiv(static_cast<uint64_t>(picWidth), static_cast<uint64_t>(maxSliceWidth)));
	minH = fitSlicesUp(common, minH > minSlices ? minH : minSlices);
	while (minH <= maxH && pclk / minH > static_cast<int>(sink.throughputMode0) * 1000)
		minH = incSlices(common, minH);
	while (minH <= maxH && picWidth % minH != 0)
		minH = incSlices(common, minH);
	if (!(minH <= maxH && maxH != 0) || minH <= 0)
		return false;
	const int sliceWidth = picWidth / minH;                       // the policy: the fewest slices that work
	if (sliceWidth > maxSliceWidth)
		return false;
	int sliceHeight = picHeight < 108 ? picHeight : 108;          // dc_dsc_get_policy_for_timing: min_slice_height 108
	while (sliceHeight < picHeight && picHeight % sliceHeight != 0)
		sliceHeight++;

	// decide_dsc_target_bpp_x16, decide_dsc_bandwidth_range: 8 to min(3 x bpc, 16) bits per pixel (or the sink's limit)
	const uint32_t min16 = 8 * 16;
	uint32_t max16 = (3u * t.bpc < 16 ? 3u * t.bpc : 16) * 16;
	if (sink.maxBitsPerPixel16)
		max16 = sink.maxBitsPerPixel16 < max16 ? sink.maxBitsPerPixel16 : max16;
	if (max16 < min16)
		return false;
	const uint32_t maxKbps = dscStreamKbps(t, max16, static_cast<uint32_t>(minH));
	const uint32_t minKbps = dscStreamKbps(t, min16, static_cast<uint32_t>(minH));
	uint32_t bpp16 = 0;
	if (link >= streamKbps(t))
		bpp16 = 0;                                                 // fits uncompressed: not needed
	else if (link >= maxKbps)
		bpp16 = max16;
	else if (link >= minKbps)
		bpp16 = bppFromBandwidth(link, t, static_cast<uint32_t>(minH), div);
	if (!bpp16)
		return false;

	out.slicesH = static_cast<uint32_t>(minH);
	out.slicesV = static_cast<uint32_t>(picHeight / sliceHeight);
	out.sliceWidth = static_cast<uint32_t>(sliceWidth);
	out.sliceHeight = static_cast<uint32_t>(sliceHeight);
	out.bpp16 = bpp16;
	out.blockPrediction = sink.blockPrediction;
	out.lineBufferBits = lineBuffer;
	out.versionMinor = static_cast<uint8_t>((version & 0xf0) >> 4);
	return true;
}

namespace {

int median3(int a, int b, int c) {
	if (a > b) { const int x = a; a = b; b = x; }
	if (b > c) { const int x = b; b = c; c = x; }
	if (a > b) { const int x = a; a = b; b = x; }
	return b;
}

// The fixed rate-control values of _do_calc_rc_params (rc_model_size, rc_edge_factor, rc_tgt_offset_*) and its buffer
// thresholds, already >> 6 as the PPS and the registers take them (896, 1792, ... 8064).
constexpr uint16_t kRcModelSize = 8192;
constexpr uint8_t kRcEdgeFactor = 6, kRcTgtOffset = 3;
constexpr uint8_t kRcBufThresh6[14] = { 14, 28, 42, 56, 70, 84, 98, 105, 112, 119, 121, 123, 125, 126 };

// dsc_roundf(n / d) for n >= 0, d > 0: round half up
int roundDiv(int64_t n, int64_t d) { return static_cast<int>((2 * n + d) / (2 * d)); }

// get_ofs_set, RGB / 4:4:4: each ramp `base + round((bpp - from) * num / den)` in sixteenths of a bit
int ramp(uint32_t b16, int base, int from, int num, int den) {
	return base + roundDiv((static_cast<int64_t>(b16) - from * 16) * num, static_cast<int64_t>(den) * 16);
}

void offsets(uint32_t b, int ofs[15]) {
	const bool le6 = b <= 96, in8to12 = b >= 128 && b <= 192, ge15 = b >= 240, in6to8 = b > 96 && b < 128;
	const bool le12 = b <= 192;
	auto lo = [&](int at6, int at8, int at15, int n12) {   // the first five entries' shape
		return le6 ? at6 : in8to12 ? at8 : ge15 ? at15 : in6to8 ? ramp(b, at6, 6, 2, 2) : ramp(b, at8, 12, n12, 3);
	};
	ofs[0] = lo(0, 2, 10, 8);
	ofs[1] = lo(-2, 0, 8, 8);
	ofs[2] = lo(-2, 0, 6, 6);
	ofs[3] = lo(-4, -2, 4, 6);
	ofs[4] = lo(-6, -4, 2, 6);
	ofs[5] = le12 ? -6 : ge15 ? 0 : ramp(b, -6, 12, 6, 3);
	ofs[6] = le12 ? -8 : ge15 ? -2 : ramp(b, -8, 12, 6, 3);
	ofs[7] = le12 ? -8 : ge15 ? -4 : ramp(b, -8, 12, 4, 3);
	ofs[8] = le12 ? -8 : ge15 ? -6 : ramp(b, -8, 12, 2, 3);
	ofs[9] = le12 ? -10 : ge15 ? -8 : ramp(b, -10, 12, 2, 3);
	ofs[10] = -10;
	ofs[11] = le6 ? -12 : b >= 128 ? -10 : ramp(b, -12, 6, 2, 2);
	ofs[12] = ofs[13] = ofs[14] = -12;
}

bool qpSet(uint8_t bpc, bool max, uint32_t b16, int out[15]) {
	const QpRow *table = nullptr;
	size_t size = 0;
#define DSC_TABLE(n, m) { table = kQp444_##n##_##m; size = sizeof(kQp444_##n##_##m) / sizeof(QpRow); }
	if (bpc == 8) { if (max) DSC_TABLE(8, max) else DSC_TABLE(8, min) }
	else if (bpc == 10) { if (max) DSC_TABLE(10, max) else DSC_TABLE(10, min) }
	else if (bpc == 12) { if (max) DSC_TABLE(12, max) else DSC_TABLE(12, min) }
#undef DSC_TABLE
	if (!table || b16 < table[0].bpp16)
		return false;
	const size_t index = (b16 - table[0].bpp16) / 8;   // (bpp - first) x 2
	if (index >= size)
		return false;
	for (int i = 0; i < 15; i++)
		out[i] = table[index].qp[i];
	return true;
}

void be16(uint8_t *p, uint32_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }

}  // namespace

bool pps(const Config &c, const Timing &t, Pps &out) {
	out = Pps {};
	const uint32_t b = c.bpp16, bpc = t.bpc;
	if (!c.slicesH || !c.slicesV || b < 96 || b > 0x3ff || (bpc != 8 && bpc != 10 && bpc != 12) ||
	    (c.versionMinor != 1 && c.versionMinor != 2))
		return false;
	const uint32_t sw = (t.hActive + c.slicesH - 1) / c.slicesH, sh = t.vActive / c.slicesV;
	if (sh * c.slicesV != t.vActive)
		return false;

	// _do_calc_rc_params, CM_RGB
	const int limit = bpc == 8 ? 11 : bpc == 10 ? 15 : 19;
	const int initialOffset = b >= 192 ? 2048 : b <= 128 ? 6144 : b <= 160 ? 6144 - static_cast<int>(b - 128) * 16
	                                                                   : 5632 - static_cast<int>(b - 160) * 112;
	const int lessSh = (static_cast<int>(sh) - 8) < 34 ? static_cast<int>(sh) - 8 : 34;
	const int firstLine = median3(0, 12 + lessSh * 9 / 100, (static_cast<int>((3 * bpc + 2) * 3 * 16) - static_cast<int>(3 * b)) / 16);
	int xmit = roundDiv(4096 * 16, b);
	const int padding = sw % 3 ? static_cast<int>(3 - sw % 3) * (xmit / static_cast<int>(sw)) : 0;
	if (3 * b >= static_cast<uint32_t>((xmit + 2) / 3 * 3) * 16 && (xmit + padding) % 3 == 1)
		xmit++;
	const int flatMin = bpc == 8 ? 3 : bpc == 10 ? 7 : 11, flatMax = bpc == 8 ? 12 : bpc == 10 ? 16 : 20;
	int qpMin[15], qpMax[15], ofs[15];
	if (!qpSet(static_cast<uint8_t>(bpc), false, b, qpMin) || !qpSet(static_cast<uint8_t>(bpc), true, b, qpMax))
		return false;
	offsets(b, ofs);
	constexpr int kModel = kRcModelSize;

	// dscc_compute_dsc_parameters and drm_dsc_compute_rc_parameters (not native 4:2:x, convert_rgb)
	uint32_t scaleValue = static_cast<uint8_t>(8 * kModel / (kModel - initialOffset));
	const uint32_t mux = bpc <= 10 ? 48 : 64;
	const uint32_t groupsPerLine = static_cast<uint32_t>(ceilDiv(sw, 3));
	const uint32_t chunk = static_cast<uint32_t>(ceilDiv(static_cast<uint64_t>(sw) * b, 8 * 16));
	uint32_t extraMux = 3 * (mux + (4 * bpc + 4) - 2);
	const uint64_t sliceBits = 8ull * chunk * sh;
	while (extraMux > 0 && (sliceBits - extraMux) % mux)
		extraMux--;
	if (groupsPerLine < scaleValue - 8)
		scaleValue = groupsPerLine + 8;
	const uint32_t scaleDecrement = scaleValue > 8 ? groupsPerLine / (scaleValue - 8) : 4095;
	const int64_t finalOffset = kModel - (static_cast<int64_t>(xmit) * b + 8) / 16 + extraMux;
	if (finalOffset >= kModel)
		return false;
	const uint32_t finalOffset16 = static_cast<uint16_t>(finalOffset);
	const uint32_t finalScale = static_cast<uint32_t>(kModel * 8 / (kModel - static_cast<int64_t>(finalOffset16)));
	const uint32_t nfl = sh > 1 ? static_cast<uint16_t>(ceilDiv(static_cast<uint64_t>(firstLine) << 11, sh - 1)) : 0;
	const uint32_t groupsTotal = groupsPerLine * sh;
	const uint32_t sliceBpg = static_cast<uint16_t>(ceilDiv((static_cast<uint64_t>(kModel - initialOffset) + extraMux) << 11, groupsTotal));
	const uint32_t scaleIncrement = finalScale > 9 ? static_cast<uint16_t>(static_cast<uint64_t>(finalOffset16) * (1 << 11) /
	                                                                       ((nfl + sliceBpg) * (finalScale - 9))) : 0;
	const uint64_t rbsMin = static_cast<uint64_t>(kModel - initialOffset) + ceilDiv(static_cast<uint64_t>(xmit) * b, 16) +
	                        static_cast<uint64_t>(groupsPerLine) * static_cast<uint32_t>(firstLine);
	const uint64_t hrd = ceilDiv(rbsMin * 16, b);
	out.rcBufferModelSize = static_cast<uint32_t>(hrd * b / 16);
	out.chunkSize = static_cast<uint16_t>(chunk);
	out.bytesPerPixel = static_cast<uint32_t>((static_cast<uint64_t>(chunk) * 0x10000000 + (sw - 1)) / sw);

	Params &m = out.params;
	m.sliceWidth = static_cast<uint16_t>(sw);
	m.sliceHeight = static_cast<uint16_t>(sh);
	m.initialXmitDelay = static_cast<uint16_t>(xmit);
	m.initialDecDelay = static_cast<uint16_t>(hrd - static_cast<uint64_t>(xmit));
	m.initialScaleValue = static_cast<uint8_t>(scaleValue);
	m.scaleIncrement = static_cast<uint16_t>(scaleIncrement);
	m.scaleDecrement = static_cast<uint16_t>(scaleDecrement);
	m.firstLineBpgOffset = static_cast<uint8_t>(firstLine);
	m.nflBpgOffset = static_cast<uint16_t>(nfl);
	m.sliceBpgOffset = static_cast<uint16_t>(sliceBpg);
	m.initialOffset = static_cast<uint16_t>(initialOffset);
	m.finalOffset = static_cast<uint16_t>(finalOffset16);
	m.flatnessMin = static_cast<uint8_t>(flatMin);
	m.flatnessMax = static_cast<uint8_t>(flatMax);
	m.quantLimit = static_cast<uint8_t>(limit);
	for (int i = 0; i < 15; i++)
		m.qpMin[i] = static_cast<int8_t>(qpMin[i]), m.qpMax[i] = static_cast<int8_t>(qpMax[i]), m.bpgOffset[i] = static_cast<int8_t>(ofs[i]);

	// drm_dsc_pps_payload_pack (the thresholds >> 6, as dsc_update_from_dsc_parameters leaves them)
	uint8_t *p = out.bytes;
	p[0] = static_cast<uint8_t>(c.versionMinor | 1 << 4);
	p[3] = static_cast<uint8_t>(c.lineBufferBits | bpc << 4);
	p[4] = static_cast<uint8_t>((b & 0x300) >> 8 | 1 << 4 | (c.blockPrediction ? 1 << 5 : 0));
	p[5] = static_cast<uint8_t>(b);
	be16(p + 6, t.vActive);
	be16(p + 8, t.hActive);
	be16(p + 10, m.sliceHeight);
	be16(p + 12, m.sliceWidth);
	be16(p + 14, chunk);
	p[16] = static_cast<uint8_t>((m.initialXmitDelay & 0x300) >> 8);
	p[17] = static_cast<uint8_t>(m.initialXmitDelay);
	be16(p + 18, m.initialDecDelay);
	p[21] = m.initialScaleValue;
	be16(p + 22, m.scaleIncrement);
	p[24] = static_cast<uint8_t>((m.scaleDecrement & 0xf00) >> 8);
	p[25] = static_cast<uint8_t>(m.scaleDecrement);
	p[27] = m.firstLineBpgOffset;
	be16(p + 28, m.nflBpgOffset);
	be16(p + 30, m.sliceBpgOffset);
	be16(p + 32, m.initialOffset);
	be16(p + 34, m.finalOffset);
	p[36] = m.flatnessMin;
	p[37] = m.flatnessMax;
	be16(p + 38, kRcModelSize);
	p[40] = kRcEdgeFactor;
	p[41] = m.quantLimit;
	p[42] = m.quantLimit;
	p[43] = kRcTgtOffset | kRcTgtOffset << 4;
	for (int i = 0; i < 14; i++)
		p[44 + i] = kRcBufThresh6[i];
	for (int i = 0; i < 15; i++)
		be16(p + 58 + 2 * i, static_cast<uint32_t>(m.qpMin[i] << 11 | m.qpMax[i] << 6 | (m.bpgOffset[i] & 0x3f)) & 0xffff);
	return true;
}

// DSC_TOP0_DSC_DEBUG_CONTROL, DSCCIF0_DSCCIF_CONFIG0, DSCC0_DSCC_CONFIG0, _CONFIG1, _INTERRUPT_CONTROL0, _PPS_CONFIG0..22
// (dcn_4_1_0_offset.h).
const uint16_t kEngineReg[kEngineRegs] = { 0x3001, 0x3005, 0x300a, 0x300b, 0x300e, 0x3012, 0x3013, 0x3014, 0x3015, 0x3016,
                                           0x3017, 0x3018, 0x3019, 0x301a, 0x301b, 0x301c, 0x301d, 0x301e, 0x301f, 0x3020,
                                           0x3021, 0x3022, 0x3023, 0x3024, 0x3025, 0x3026, 0x3027, 0x3028 };

void engineRegs(const Config &c, const Timing &t, const Pps &pps, bool odm, uint32_t (&out)[kEngineRegs]) {
	const Params &m = pps.params;
	const uint32_t bpc = t.bpc;
	auto range = [&m](int i) {   // RANGE_MIN_QPn [4:0], RANGE_MAX_QPn [9:5], RANGE_BPG_OFFSETn [15:10]
		return static_cast<uint32_t>(m.qpMin[i] & 0x1f) | static_cast<uint32_t>(m.qpMax[i] & 0x1f) << 5 |
		       static_cast<uint32_t>(m.bpgOffset[i] & 0x3f) << 10;
	};
	auto thresh = [](int i) {
		return static_cast<uint32_t>(kRcBufThresh6[i]) | kRcBufThresh6[i + 1] << 8 | kRcBufThresh6[i + 2] << 16 |
		       static_cast<uint32_t>(kRcBufThresh6[i + 3]) << 24;
	};
	out[0] = 0;                                                       // DSC_DBG_EN
	out[1] = bpc << 16;                                               // INPUT_PIXEL_FORMAT RGB (0), BITS_PER_COMPONENT
	out[2] = ((odm || c.slicesH > 1) ? 0xfu : 0) | (c.slicesH - 1) << 4 | (c.versionMinor == 1 ? 0 : 1u << 8) |
	         (c.slicesV - 1) << 16;                                   // ICH reset at EOL, slices, alternate ICH
	out[3] = pps.rcBufferModelSize;
	out[4] = 0;                                                       // no overflow interrupts
	out[5] = c.versionMinor | static_cast<uint32_t>(c.lineBufferBits) << 24 | bpc << 28;
	out[6] = (c.bpp16 & 0x3ff) | 1u << 12 | (c.blockPrediction ? 1u << 13 : 0) | static_cast<uint32_t>(pps.chunkSize) << 16;
	out[7] = t.hActive | t.vActive << 16;
	out[8] = m.sliceWidth | static_cast<uint32_t>(m.sliceHeight) << 16;
	out[9] = m.initialXmitDelay;                                      // (INITIAL_DEC_DELAY is not written)
	out[10] = m.initialScaleValue | static_cast<uint32_t>(m.scaleIncrement) << 16;
	out[11] = m.scaleDecrement | static_cast<uint32_t>(m.firstLineBpgOffset) << 16;
	out[12] = m.nflBpgOffset | static_cast<uint32_t>(m.sliceBpgOffset) << 16;
	out[13] = 0;                                                      // NSL_BPG_OFFSET, SECOND_LINE_OFFSET_ADJ
	out[14] = m.initialOffset | static_cast<uint32_t>(m.finalOffset) << 16;
	out[15] = m.flatnessMin | static_cast<uint32_t>(m.flatnessMax) << 8 | static_cast<uint32_t>(kRcModelSize) << 16;
	out[16] = kRcEdgeFactor | static_cast<uint32_t>(m.quantLimit) << 8 | static_cast<uint32_t>(m.quantLimit) << 16 |
	          static_cast<uint32_t>(kRcTgtOffset) << 24 | static_cast<uint32_t>(kRcTgtOffset) << 28;
	out[17] = thresh(0);
	out[18] = thresh(4);
	out[19] = thresh(8);
	out[20] = kRcBufThresh6[12] | static_cast<uint32_t>(kRcBufThresh6[13]) << 8 | range(0) << 16;
	for (int i = 0; i < 7; i++)
		out[21 + i] = range(1 + 2 * i) | range(2 + 2 * i) << 16;
}

void ppsPackets(const Pps &p, uint32_t (&out)[4][9]) {
	for (int k = 0; k < 4; k++) {
		out[k][0] = 0x007f1000;                                       // HB1 0x10 (PPS), HB2 0x7f: 128 bytes
		for (int i = 0; i < 8; i++) {
			const uint8_t *d = p.bytes + 32 * k + 4 * i;
			out[k][1 + i] = d[0] | d[1] << 8 | d[2] << 16 | static_cast<uint32_t>(d[3]) << 24;
		}
	}
}

}  // namespace Dsc
