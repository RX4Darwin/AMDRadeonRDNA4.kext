//
//  flip.hpp
//  RDNA4FB
//
//  The display page-flip smoke test. The implementation is kept separate
//  from the compute bring-up; compute only supplies the one post-stage-7
//  entry point and its DMA/device-heap state.
//

#ifndef RDNA4FB_FLIP_HPP
#define RDNA4FB_FLIP_HPP

#include <stdint.h>

class RDNA4Compute;

namespace Flip {

constexpr uint8_t kNone = 0xff;

// The active DCN path and its desktop surface. Register accessors do not
// take rtLock; callers serialize hardware access around them.
struct Surface {
	uint8_t otg { kNone };
	uint8_t hubp { kNone };
	uint32_t width { 0 };
	uint32_t height { 0 };
	uint32_t pitch { 0 };
	uint64_t desktop { 0 };
};

// Pure helpers are also exercised by the host harness. A primary linear
// surface is pitch pixels wide, height rows high and four bytes per pixel.
constexpr uint64_t surfaceBytes(uint32_t pitchPixels, uint32_t height) {
	return pitchPixels && height &&
	       static_cast<uint64_t>(pitchPixels) <= (~0ull / 4ull) / height
		? static_cast<uint64_t>(pitchPixels) * height * 4ull
		: 0;
}

constexpr uint32_t addressLo(uint64_t address) {
	return static_cast<uint32_t>(address);
}

constexpr uint32_t addressHi(uint64_t address) {
	return static_cast<uint32_t>(address >> 32);
}

// Discover the lit OPP/HUBP and its scanout surface. No trail is written;
// this function is also used by post-boot clients. `log` false: only a
// failure is logged (a client that presents asks once a frame).
bool findPipe(RDNA4Compute &compute, Surface &out, bool log = true);

// Poll the OTG frame counter until it advances. This is the temporary W5c
// vblank interface; replace its polling body with ihWaitVblank when W1b lands.
bool waitNextVblank(RDNA4Compute &compute, uint8_t otg, uint32_t timeoutMs,
                    uint64_t &frame);

// Latch one address and verify its vblank advance and readback. The caller
// serializes this with rtLock and supplies any boot trail before the call.
bool flipTo(RDNA4Compute &compute, const Surface &surface, uint64_t target,
			const char *name, uint64_t *latencyUs = nullptr, bool async = false);

// Run the rdna4-flip=<mode> test once. False means the requested test failed
// and the feature has been disabled after attempting to restore the desktop.
bool run(RDNA4Compute &compute);

} // namespace Flip

#endif /* RDNA4FB_FLIP_HPP */
