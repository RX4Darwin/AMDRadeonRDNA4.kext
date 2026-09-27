//
//  sdma.cpp
//  RDNA4FB
//
//  See sdma.hpp.
//

#include "sdma.hpp"

namespace Sdma {

uint32_t writeDword(uint32_t *out, uint64_t addr, uint32_t value) {
	out[0] = header(OpWrite, kSubLinear);
	out[1] = static_cast<uint32_t>(addr);
	out[2] = static_cast<uint32_t>(addr >> 32);
	out[3] = 0;                          // count: dwords - 1
	out[4] = value;
	return kWriteDwords;
}

uint32_t fence(uint32_t *out, uint64_t addr, uint32_t value) {
	out[0] = header(OpFence) | kFenceMtypeUc;
	out[1] = static_cast<uint32_t>(addr) & ~3u;
	out[2] = static_cast<uint32_t>(addr >> 32);
	out[3] = value;
	return kFenceDwords;
}

uint32_t constFill(uint32_t *out, uint64_t addr, uint32_t pattern, uint32_t bytes) {
	out[0] = header(OpConstFill) | kFillSizeDword;
	out[1] = static_cast<uint32_t>(addr);
	out[2] = static_cast<uint32_t>(addr >> 32);
	out[3] = pattern;
	out[4] = bytes - 1;
	return kFillDwords;
}

uint32_t copyLinear(uint32_t *out, uint64_t src, uint64_t dst, uint32_t bytes) {
	out[0] = header(OpCopy, kSubLinear) | kCopyCpv;
	out[1] = bytes - 1;                  // count
	out[2] = 0;                          // parameter (swap modes)
	out[3] = static_cast<uint32_t>(src);
	out[4] = static_cast<uint32_t>(src >> 32);
	out[5] = static_cast<uint32_t>(dst);
	out[6] = static_cast<uint32_t>(dst >> 32);
	out[7] = 0;                          // DCC format: uncompressed
	return kCopyDwords;
}

bool Ring::init(volatile uint32_t *cpu, uint64_t mc, uint32_t sizeBytes) {
	if (!cpu || sizeBytes < 256 || (sizeBytes & (sizeBytes - 1)) || (mc & 0xff))
		return false;
	ring = cpu;
	base = mc;
	size = sizeBytes;
	wp = 0;
	for (uint32_t i = 0; i < size / 4; i++)
		ring[i] = 0;                     // NOPs
	return true;
}

uint32_t Ring::sizeLog2Dwords() const {
	uint32_t n = 0;
	for (uint32_t v = size / 4; v > 1; v >>= 1)
		n++;
	return n;
}

bool Ring::emit(const uint32_t *dw, uint32_t count) {
	if (!ring || count * 4 >= size)
		return false;
	for (uint32_t i = 0; i < count; i++) {
		ring[wp / 4] = dw[i];
		wp = (wp + 4) & (size - 1);
	}
	return true;
}

} // namespace Sdma
