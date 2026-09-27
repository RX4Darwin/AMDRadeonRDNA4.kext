//
//  psp.cpp
//  RDNA4FB
//
//  See psp.hpp.
//

#include "psp.hpp"

namespace Psp {

namespace {
constexpr uint32_t kReady        = 0x80000000u;   // GFX_FLAG_RESPONSE / bootloader ready
constexpr uint32_t kRespMask     = 0x8000ffffu;   // response flag + status
constexpr uint32_t kRingTypeKm   = 2;
constexpr uint32_t kPollUs       = 10;
constexpr uint32_t kBlWaitUs     = 100000;        // per try, amdgpu's usec_timeout
constexpr uint32_t kBlTries      = 10;
} // namespace

bool Driver::init(const Bus &b, const Window &w) {
	bus = b;
	win = w;
	fence = 0;
	return bus.read && bus.write && bus.delayUs && bus.flushHdp && win.cpu &&
	       win.size >= kMinWindow && (win.mc & ((1ull << 20) - 1)) == 0;
}

bool Driver::sosAlive() const { return rd(Reg::kC2p81) != 0; }
uint32_t Driver::sosVersion() const { return rd(Reg::kC2p81); }

bool Driver::waitMask(uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeoutUs,
                      uint32_t &last) {
	for (uint32_t t = 0;; t += kPollUs) {
		last = rd(reg);
		if ((last & mask) == want)
			return true;
		if (t >= timeoutUs)
			return false;
		bus.delayUs(bus.ctx, kPollUs);
	}
}

bool Driver::waitChange(uint32_t reg, uint32_t from, uint32_t timeoutUs, uint32_t &last) {
	for (uint32_t t = 0;; t += kPollUs) {
		last = rd(reg);
		if (last != from)
			return true;
		if (t >= timeoutUs)
			return false;
		bus.delayUs(bus.ctx, kPollUs);
	}
}

void Driver::put32(uint32_t off, uint32_t v) {
	volatile uint8_t *p = win.cpu + off;
	*reinterpret_cast<volatile uint32_t *>(p) = v;
}

uint32_t Driver::get32(uint32_t off) const {
	const volatile uint8_t *p = win.cpu + off;
	return *reinterpret_cast<const volatile uint32_t *>(p);
}

// The window is device memory: dword stores only, no memset/memcpy (which
// may use wider or unaligned accesses the aperture does not promise).
void Driver::zero(uint32_t off, uint32_t len) {
	for (uint32_t i = 0; i + 4 <= len; i += 4)
		put32(off + i, 0);
}

void Driver::copy(uint32_t off, const uint8_t *src, uint32_t len) {
	uint32_t i = 0;
	for (; i + 4 <= len; i += 4)
		put32(off + i, static_cast<uint32_t>(src[i]) | (static_cast<uint32_t>(src[i + 1]) << 8) |
		                   (static_cast<uint32_t>(src[i + 2]) << 16) |
		                   (static_cast<uint32_t>(src[i + 3]) << 24));
	if (i < len) {
		uint32_t tail = 0;
		for (uint32_t k = 0; i + k < len; k++)
			tail |= static_cast<uint32_t>(src[i + k]) << (8 * k);
		put32(off + i, tail);
	}
}

// ---------------------------------------------------------------------------
// Bootloader
// ---------------------------------------------------------------------------

Result Driver::loadComponent(const AmdFw::Blob &part, uint32_t cmd, const char *name) {
	uint32_t last = 0;
	bool ready = false;
	for (uint32_t i = 0; i < kBlTries && !ready; i++)
		ready = waitMask(Reg::kC2p35, kReady, kReady, kBlWaitUs, last);
	if (!ready)
		return { false, name, last };
	if (part.size > kFwPriSize)
		return { false, name, part.size };

	zero(kFwPriOffset, kFwPriSize);
	copy(kFwPriOffset, part.data, part.size);
	bus.flushHdp(bus.ctx);

	wr(Reg::kC2p36, static_cast<uint32_t>((win.mc + kFwPriOffset) >> 20));
	wr(Reg::kC2p35, cmd);

	ready = false;
	for (uint32_t i = 0; i < kBlTries && !ready; i++)
		ready = waitMask(Reg::kC2p35, kReady, kReady, kBlWaitUs, last);
	return { ready, name, last };
}

Result Driver::loadSos(const AmdFw::PspPackage &pkg, uint32_t &loaded) {
	loaded = 0;
	if (sosAlive())
		return { true, "sOS already running", sosVersion() };

	// psp_hw_start's order; absent components are skipped.
	struct Step { uint32_t part; uint32_t cmd; };
	static const Step order[] = {
		{ AmdFw::PspKdb, BlKdb },           { AmdFw::PspSpl, BlSplTable },
		{ AmdFw::PspSysDrv, BlSysDrv },     { AmdFw::PspSocDrv, BlSocDrv },
		{ AmdFw::PspIntfDrv, BlIntfDrv },   { AmdFw::PspDbgDrv, BlHadDrv },
		{ AmdFw::PspRasDrv, BlRasDrv },     { AmdFw::PspIpKeyMgrDrv, BlIpKeyMgr },
	};
	for (const Step &s : order) {
		if (!pkg.part[s.part].valid())
			continue;
		Result r = loadComponent(pkg.part[s.part], s.cmd, AmdFw::pspPartName(s.part));
		if (!r.ok)
			return r;
		loaded++;
	}

	// The sOS itself: same handshake, then its sign of life changes.
	const AmdFw::Blob &sos = pkg.part[AmdFw::PspSos];
	uint32_t last = 0;
	bool ready = false;
	for (uint32_t i = 0; i < kBlTries && !ready; i++)
		ready = waitMask(Reg::kC2p35, kReady, kReady, kBlWaitUs, last);
	if (!ready)
		return { false, "SOS (bootloader not ready)", last };
	if (sos.size > kFwPriSize)
		return { false, "SOS (too large)", sos.size };
	zero(kFwPriOffset, kFwPriSize);
	copy(kFwPriOffset, sos.data, sos.size);
	bus.flushHdp(bus.ctx);
	uint32_t before = rd(Reg::kC2p81);
	wr(Reg::kC2p36, static_cast<uint32_t>((win.mc + kFwPriOffset) >> 20));
	wr(Reg::kC2p35, BlSosDrv);
	bus.delayUs(bus.ctx, 20000);
	if (!waitChange(Reg::kC2p81, before, 2000000, last))
		return { false, "SOS (no sign of life)", last };
	loaded++;
	return { true, "sOS running", last };
}

// ---------------------------------------------------------------------------
// GPCOM ring
// ---------------------------------------------------------------------------

Result Driver::createRing() {
	uint32_t last = 0;
	if (!waitMask(Reg::kC2p64, kRespMask, kReady, 1000000, last))
		return { false, "ring: sOS not ready for a ring", last };

	zero(kRingOffset, kRingSize);
	zero(kCmdOffset, kCmdSize);
	zero(kFenceOffset, 0x1000);
	bus.flushHdp(bus.ctx);

	const uint64_t ring = win.mc + kRingOffset;
	wr(Reg::kC2p69, static_cast<uint32_t>(ring));
	wr(Reg::kC2p70, static_cast<uint32_t>(ring >> 32));
	wr(Reg::kC2p71, kRingSize);
	wr(Reg::kC2p64, kRingTypeKm << 16);
	bus.delayUs(bus.ctx, 20000);
	if (!waitMask(Reg::kC2p64, kRespMask, kReady, 1000000, last))
		return { false, "ring: create not acknowledged", last };
	fence = 0;
	return { true, "ring created", last };
}

Result Driver::submit(uint32_t cmdId, const uint32_t *args, uint32_t nargs, Response &resp,
                      uint32_t timeoutMs) {
	resp = Response {};
	if (nargs > (Layout::kResp - Layout::kCmdArgs) / 4)
		return { false, "submit: too many arguments", nargs };

	// Command buffer.
	zero(kCmdOffset, kCmdSize);
	put32(kCmdOffset + Layout::kCmdId, cmdId);
	for (uint32_t i = 0; i < nargs; i++)
		put32(kCmdOffset + Layout::kCmdArgs + 4 * i, args[i]);

	// Frame at the current write pointer (in dwords).
	const uint32_t ringDw = kRingSize / 4, frameDw = Layout::kFrameSize / 4;
	uint32_t wptr = rd(Reg::kC2p67);
	uint32_t slot = (wptr % ringDw) / frameDw;
	uint32_t frame = kRingOffset + slot * Layout::kFrameSize;
	zero(frame, Layout::kFrameSize);
	const uint64_t cmdMc = win.mc + kCmdOffset, fenceMc = win.mc + kFenceOffset;
	uint32_t index = ++fence;
	put32(frame + Layout::kFrameCmdLo, static_cast<uint32_t>(cmdMc));
	put32(frame + Layout::kFrameCmdHi, static_cast<uint32_t>(cmdMc >> 32));
	put32(frame + Layout::kFrameFenceLo, static_cast<uint32_t>(fenceMc));
	put32(frame + Layout::kFrameFenceHi, static_cast<uint32_t>(fenceMc >> 32));
	put32(frame + Layout::kFrameFenceValue, index);
	bus.flushHdp(bus.ctx);

	wr(Reg::kC2p67, (wptr + frameDw) % ringDw);

	// Wait for the fence.
	uint32_t seen = 0;
	for (uint32_t t = 0;; t += 50) {
		seen = get32(kFenceOffset);
		if (seen == index)
			break;
		if (t >= timeoutMs * 1000u)
			return { false, "submit: fence timeout", seen };
		bus.delayUs(bus.ctx, 50);
	}
	resp.status   = get32(kCmdOffset + Layout::kRespStatus);
	resp.fwAddrLo = get32(kCmdOffset + Layout::kRespFwAddrLo);
	resp.fwAddrHi = get32(kCmdOffset + Layout::kRespFwAddrHi);
	resp.tmrSize  = get32(kCmdOffset + Layout::kRespTmrSize);
	for (uint32_t i = 0; i < 8; i++)
		resp.uresp[i] = get32(kCmdOffset + Layout::kRespUresp + 4 * i);
	return { resp.status == 0, resp.status == 0 ? "ok" : "PSP returned an error", resp.status };
}

Result Driver::loadToc(const AmdFw::Blob &toc, uint32_t &tmrSize) {
	tmrSize = 0;
	if (!toc.valid() || toc.size > kFwPriSize)
		return { false, "LOAD_TOC: no TOC", 0 };
	zero(kFwPriOffset, kFwPriSize);
	copy(kFwPriOffset, toc.data, toc.size);
	bus.flushHdp(bus.ctx);
	const uint64_t mc = win.mc + kFwPriOffset;
	const uint32_t args[] = { static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), toc.size };
	Response resp;
	Result r = submit(CmdLoadToc, args, 3, resp);
	if (r.ok)
		tmrSize = resp.tmrSize;
	r.what = r.ok ? "LOAD_TOC" : r.what;
	return r;
}

Result Driver::loadIpFw(const AmdFw::Blob &ucode, uint32_t fwType, Response &resp) {
	if (!ucode.valid() || ucode.size > win.size - kStageOffset)
		return { false, "LOAD_IP_FW: payload does not fit", ucode.size };
	copy(kStageOffset, ucode.data, ucode.size);
	bus.flushHdp(bus.ctx);
	const uint64_t mc = win.mc + kStageOffset;
	const uint32_t args[] = { static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32),
	                          ucode.size, fwType };
	Result r = submit(CmdLoadIpFw, args, 4, resp, 5000);
	r.what = r.ok ? "LOAD_IP_FW" : r.what;
	return r;
}

} // namespace Psp
