//
//  flip.cpp
//  RDNA4FB
//
//  The opt-in DCN page-flip smoke test.  It borrows the already-running
//  display pipe and the stage-7 SDMA/device heap, and never changes modeset
//  state.
//

#include "flip.hpp"

#include "compute.hpp"
#include "pipe.hpp"
#include "sdma.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#define FLOG(fmt, ...) IOLog("RDNA4FB: flip: " fmt "\n", ## __VA_ARGS__)

namespace Flip {

namespace {

constexpr uint32_t kBad = 0xffffffffu;

constexpr uint32_t kHubpAddressLo = 0x060a;
constexpr uint32_t kHubpAddressHi = 0x060b;
constexpr uint32_t kHubpFlipControl = 0x061b;
constexpr uint32_t kHubpFlipPending = 1u << 8;
constexpr uint32_t kHubpFlipType = 1u << 1;
constexpr uint32_t kHubpStereoMode = 3u << 12;
constexpr uint32_t kHubpStereoIn = 1u << 16;

constexpr uint32_t kOtgStatusPosition = 0x1b4a;
constexpr uint32_t kOtgMasterUpdateLock = 0x1b89;
constexpr uint32_t kMaxAddressBytes = 0xffffffffu;

} // namespace

bool run(RDNA4Compute &compute) {
	uint32_t mode = 0;
	if (!PE_parse_boot_argn("rdna4-flip", &mode, sizeof(mode)) || !mode)
		return true;
	if (mode > 2) {
		FLOG("invalid mode %u; feature disabled", mode);
		return true;
	}
	if (!compute.dmaReady || !compute.devHeap.size()) {
		FLOG("skip: SDMA/device heap unavailable");
		return true;
	}

	auto dmuRead = [&](uint32_t dword) {
		return compute.rd(IpDiscovery::HwDmu, GfxReg::Reg { 2, dword });
	};
	auto dmuWrite = [&](uint32_t dword, uint32_t value) {
		compute.wr(IpDiscovery::HwDmu, GfxReg::Reg { 2, dword }, value);
	};

	uint8_t otg = Pipe::kNone;
	for (uint8_t i = 0; i < Pipe::kMaxOtg; i++) {
		const uint32_t control = dmuRead(Pipe::Reg::kOtgControl + i * Pipe::Reg::kOtgStride);
		if (control != kBad && (control & 1) && (control & (1u << 16))) {
			otg = i;
			break;
		}
	}
	if (otg == Pipe::kNone) {
		FLOG("skip: no running OTG");
		return true;
	}

	const uint32_t oppReg = Pipe::Reg::kOptcDataSource + otg * Pipe::Reg::kOdmStride;
	const uint32_t oppImage = dmuRead(oppReg);
	if (oppImage == kBad) {
		FLOG("skip: OTG%u OPP source unreadable", otg);
		return true;
	}
	const uint8_t opp = static_cast<uint8_t>((oppImage >> 16) & 0xf);
	uint8_t hubp = Pipe::kNone;
	for (uint8_t i = 0; i < Pipe::kMaxOtg; i++) {
		const uint32_t id = dmuRead(0x0002 + i * Pipe::Reg::kMpccStride);
		if (id != kBad && (id & 0xf) == opp) {
			hubp = i;
			break;
		}
	}
	if (hubp == Pipe::kNone) {
		FLOG("skip: OTG%u OPP%u has no HUBP", otg, opp);
		return true;
	}

	const uint32_t hubpBase = static_cast<uint32_t>(hubp) * Pipe::Reg::kHubpStride;
	const uint32_t viewport = dmuRead(Pipe::Reg::kHubpViewportDim + hubpBase);
	const uint32_t pitchImage = dmuRead(Pipe::Reg::kHubpSurfacePitch + hubpBase);
	const uint32_t oldLo = dmuRead(kHubpAddressLo + hubpBase);
	const uint32_t oldHi = dmuRead(kHubpAddressHi + hubpBase);
	if (viewport == kBad || pitchImage == kBad || oldLo == kBad || oldHi == kBad) {
		FLOG("skip: HUBP%u surface registers unreadable", hubp);
		return true;
	}
	const uint32_t width = viewport & 0x3fffu;
	const uint32_t height = (viewport >> 16) & 0x3fffu;
	const uint32_t pitch = (pitchImage & 0xffffu) + 1;
	const uint64_t original = static_cast<uint64_t>(oldLo) |
	                          (static_cast<uint64_t>(oldHi) << 32);
	const uint64_t bytes = surfaceBytes(pitch, height);
	if (!width || !height || !pitch || !bytes || bytes > kMaxAddressBytes) {
		FLOG("skip: invalid surface %ux%u pitch %u", width, height, pitch);
		return true;
	}

	FLOG("pipe OTG%u OPP%u HUBP%u surface 0x%llx, %ux%u pitch %u (%llu bytes)",
	     otg, opp, hubp, original, width, height, pitch, bytes);

	uint64_t bufferOffset = 0;
	if (!compute.devHeap.alloc(bytes, bufferOffset)) {
		FLOG("skip: device heap allocation of %llu bytes failed", bytes);
		return true;
	}
	const uint64_t buffer = compute.vramMc(bufferOffset);
	FLOG("allocate: VRAM offset 0x%llx address 0x%llx size %llu", bufferOffset, buffer, bytes);

	bool touchedSurface = false;
	bool ok = true;
	uint32_t packet[Sdma::kCopyDwords];
	compute.trail("flip: SDMA copy");
	if (!Sdma::copyLinear(packet, original, buffer, static_cast<uint32_t>(bytes)) ||
	    !compute.sdmaRun(packet, Sdma::kCopyDwords, 2000)) {
		FLOG("failure: SDMA VRAM-to-VRAM copy did not complete");
		ok = false;
	} else {
		FLOG("copy: VRAM 0x%llx -> 0x%llx (%llu bytes)", original, buffer, bytes);
	}

	if (ok && mode == 2) {
		constexpr uint32_t colours[8] = {
			0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffff00u,
			0xff00ffffu, 0xffff00ffu, 0xffffffffu, 0xff000000u,
		};
		const uint64_t rowBytes = static_cast<uint64_t>(pitch) * 4;
		for (uint32_t bar = 0; bar < 8 && ok; bar++) {
			const uint32_t first = height * bar / 8;
			const uint32_t last = height * (bar + 1) / 8;
			const uint64_t fillBytes = static_cast<uint64_t>(last - first) * rowBytes;
			uint32_t fill[Sdma::kFillDwords];
			compute.trail("flip: SDMA pattern fill");
			if (!Sdma::constFill(fill, buffer + static_cast<uint64_t>(first) * rowBytes,
			                    colours[bar], static_cast<uint32_t>(fillBytes)) ||
			    !compute.sdmaRun(fill, Sdma::kFillDwords, 2000)) {
				FLOG("failure: pattern bar %u did not complete", bar);
				ok = false;
			} else {
				FLOG("pattern: bar %u rows %u..%u", bar, first, last);
			}
		}
	}

	auto readAddress = [&](uint64_t &address) {
		const uint32_t lo = dmuRead(kHubpAddressLo + hubpBase);
		const uint32_t hi = dmuRead(kHubpAddressHi + hubpBase);
		if (lo == kBad || hi == kBad)
			return false;
		address = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
		return true;
	};
	auto readPosition = [&](uint32_t &frame, uint32_t &line) {
		const uint32_t f = dmuRead(Pipe::Reg::kOtgFrameCount + otg * Pipe::Reg::kOtgStride);
		const uint32_t p = dmuRead(kOtgStatusPosition + otg * Pipe::Reg::kOtgStride);
		if (f == kBad || p == kBad)
			return false;
		frame = f & 0xffffffu;
		line = (p >> 16) & 0xffffu;
		return true;
	};
	auto nowNs = [&]() {
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		return ns;
	};
	auto waitForPendingClear = [&]() {
		uint64_t span = 0;
		nanoseconds_to_absolutetime(100000000, &span);
		const uint64_t start = mach_absolute_time();
		for (;;) {
			const uint32_t control = dmuRead(kHubpFlipControl + hubpBase);
			if (control != kBad && !(control & kHubpFlipPending))
				return true;
			if (mach_absolute_time() - start >= span)
				return false;
			IODelay(10);
		}
	};
	auto writeAddress = [&](uint64_t target, const char *step) {
		compute.trail(step);
		const uint32_t before = dmuRead(kOtgMasterUpdateLock + otg * Pipe::Reg::kOtgStride);
		if (before == kBad)
			return false;
		const uint32_t lockReg = kOtgMasterUpdateLock + otg * Pipe::Reg::kOtgStride;
		dmuWrite(lockReg, before | 1);
		uint64_t span = 0;
		nanoseconds_to_absolutetime(1000000, &span);
		const uint64_t lockStart = mach_absolute_time();
		bool locked = false;
		while (mach_absolute_time() - lockStart < span) {
			const uint32_t lock = dmuRead(lockReg);
			if (lock != kBad && (lock & (1u << 8))) {
				locked = true;
				break;
			}
			IODelay(10);
		}
		if (!locked) {
			dmuWrite(lockReg, before & ~1u);
			return false;
		}

		bool wrote = false;
		const uint32_t flipReg = kHubpFlipControl + hubpBase;
		uint32_t flipControl = dmuRead(flipReg);
		if (flipControl != kBad) {
			flipControl &= ~(kHubpFlipType | kHubpStereoMode | kHubpStereoIn);
			dmuWrite(flipReg, flipControl);
			dmuWrite(kHubpAddressHi + hubpBase, addressHi(target));
			dmuWrite(kHubpAddressLo + hubpBase, addressLo(target));
			wrote = true;
		}
		dmuWrite(lockReg, before & ~1u);
		return wrote && waitForPendingClear();
	};
	auto flipAndVerify = [&](uint64_t target, const char *step, const char *name) {
		uint32_t beforeFrame = 0, beforeLine = 0;
		if (!readPosition(beforeFrame, beforeLine)) {
			FLOG("failure: %s could not read OTG position", name);
			return false;
		}
		const uint64_t started = nowNs();
		if (!writeAddress(target, step)) {
			FLOG("failure: %s did not latch within 100 ms", name);
			return false;
		}
		uint64_t readback = 0;
		if (!readAddress(readback) || readback != target) {
			FLOG("failure: %s readback 0x%llx expected 0x%llx", name, readback, target);
			return false;
		}
		uint32_t afterFrame = 0, afterLine = 0;
		if (!readPosition(afterFrame, afterLine)) {
			FLOG("failure: %s could not read post-latch OTG position", name);
			return false;
		}
		const uint32_t frameDelta = (afterFrame - beforeFrame) & 0xffffffu;
		const uint32_t vtotalImage = dmuRead(Pipe::Reg::kOtgVTotal + otg * Pipe::Reg::kOtgStride);
		const uint32_t vtotal = vtotalImage == kBad ? 0 : (vtotalImage & 0x7fffu) + 1;
		const uint64_t scanlines = static_cast<uint64_t>(frameDelta) * vtotal +
		                          (afterLine >= beforeLine ? afterLine - beforeLine : 0);
		const uint64_t latencyUs = (nowNs() - started) / 1000;
		if (!frameDelta) {
			FLOG("failure: %s latched without a vblank frame advance", name);
			return false;
		}
		FLOG("%s latched: address 0x%llx, latency %llu us, %llu scanlines, frame %u->%u",
		     name, readback, latencyUs, scanlines, beforeFrame, afterFrame);
		return true;
	};
	auto logFailure = [&](const char *why) {
		uint64_t current = 0;
		const bool haveAddress = readAddress(current);
		const uint32_t lock = dmuRead(kOtgMasterUpdateLock + otg * Pipe::Reg::kOtgStride);
		const uint32_t pending = dmuRead(kHubpFlipControl + hubpBase);
		const uint32_t status = dmuRead(Pipe::Reg::kOtgStatus + otg * Pipe::Reg::kOtgStride);
		const uint32_t position = dmuRead(kOtgStatusPosition + otg * Pipe::Reg::kOtgStride);
		FLOG("failure: %s; lock 0x%08x pending 0x%08x original 0x%llx current %s0x%llx "
		     "OTG status 0x%08x position 0x%08x",
		     why, lock, pending, original, haveAddress ? "" : "?", current, status, position);
	};

	if (ok) {
		touchedSurface = true;
		ok = flipAndVerify(buffer, "flip: surface address", "flip");
	}
	if (ok && mode == 2) {
		FLOG("pattern displayed; holding for 1 second");
		IOSleep(1000);
	}
	if (ok) {
		touchedSurface = true;
		ok = flipAndVerify(original, "flip: flip back", "flip back");
	}
	if (!ok) {
		logFailure("page flip test failed");
		if (touchedSurface && !writeAddress(original, "flip: restore original"))
			FLOG("failure: restoring original surface did not latch");
		FLOG("feature disabled for this boot");
	}

	if (!compute.devHeap.free(bufferOffset))
		FLOG("failure: device heap free of offset 0x%llx failed", bufferOffset);
	else
		FLOG("free: device heap offset 0x%llx", bufferOffset);
	return ok;
}

} // namespace Flip

