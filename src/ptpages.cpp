//
//  ptpages.cpp
//  RDNA4FB
//
//  See ptpages.hpp.
//

#include "ptpages.hpp"

namespace PtPages {

void Table::init(uint32_t quotaPages) {
	for (uint32_t i = 0; i < kMaxPages; i++)
		handle[i] = 0;
	for (uint32_t i = 0; i < kMaxChunks; i++) {
		chunkOff[i] = 0;
		chunkMask[i] = 0;
		chunkLive[i] = false;
	}
	chunkCount = backed = 0;
	limit = quotaPages > kMaxPages ? kMaxPages : quotaPages;
}

uint64_t Table::offsetOf(uint32_t logical) const {
	if (logical >= kMaxPages || !handle[logical])
		return 0;
	const uint32_t h = handle[logical] - 1;
	return chunkOff[h >> 4] + static_cast<uint64_t>(h & 15) * kPageBytes;
}

bool Table::page(uint32_t logical, const Backend &b, uint64_t &heapOffset) {
	if (logical >= kMaxPages)
		return false;
	if (handle[logical]) {
		heapOffset = offsetOf(logical);
		return true;
	}
	if (backed >= limit)
		return false;
	uint32_t c = kMaxChunks;
	for (uint32_t i = 0; i < kMaxChunks; i++) {
		if (chunkLive[i] && chunkMask[i] != 0xffff) {
			c = i;
			break;
		}
	}
	if (c == kMaxChunks) {
		uint32_t freeIdx = kMaxChunks;
		for (uint32_t i = 0; i < kMaxChunks; i++) {
			if (!chunkLive[i]) {
				freeIdx = i;
				break;
			}
		}
		uint64_t off = 0;
		if (freeIdx == kMaxChunks || !b.allocChunk || !b.allocChunk(b.context, off))
			return false;
		chunkLive[freeIdx] = true;
		chunkOff[freeIdx] = off;
		chunkMask[freeIdx] = 0;
		chunkCount++;
		c = freeIdx;
	}
	uint32_t slot = 0;
	while (chunkMask[c] & (1u << slot))
		slot++;
	chunkMask[c] |= static_cast<uint16_t>(1u << slot);
	handle[logical] = ((c << 4) | slot) + 1;
	backed++;
	heapOffset = chunkOff[c] + static_cast<uint64_t>(slot) * kPageBytes;
	return true;
}

void Table::drop(uint32_t logical, const Backend &b) {
	if (logical >= kMaxPages || !handle[logical])
		return;
	const uint32_t h = handle[logical] - 1, c = h >> 4, slot = h & 15;
	handle[logical] = 0;
	chunkMask[c] &= static_cast<uint16_t>(~(1u << slot));
	backed--;
	if (!chunkMask[c]) {
		chunkLive[c] = false;
		chunkCount--;
		if (b.freeChunk)
			b.freeChunk(b.context, chunkOff[c]);
		chunkOff[c] = 0;
	}
}

void Table::release(const Backend &b) {
	for (uint32_t i = 0; i < kMaxChunks; i++) {
		if (chunkLive[i] && b.freeChunk)
			b.freeChunk(b.context, chunkOff[i]);
		chunkLive[i] = false;
		chunkMask[i] = 0;
		chunkOff[i] = 0;
	}
	for (uint32_t i = 0; i < kMaxPages; i++)
		handle[i] = 0;
	chunkCount = backed = 0;
}

} // namespace PtPages
