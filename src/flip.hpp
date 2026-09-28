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

// Run the rdna4-flip=<mode> test once. False means the requested test failed
// and the feature has been disabled after attempting to restore the desktop.
bool run(RDNA4Compute &compute);

} // namespace Flip

#endif /* RDNA4FB_FLIP_HPP */
