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

void Directory::init() {
	for (uint32_t i = 0; i < kCapacity; i++) {
		keys[i] = 0;
		ids[i] = 0;
		used[i] = false;
	}
}

static uint32_t dirHash(uint64_t key) {
	return static_cast<uint32_t>((key * 0x9e3779b97f4a7c15ull) >> 52) & (Directory::kCapacity - 1);
}

int Directory::find(uint64_t key) const {
	for (uint32_t n = 0, i = dirHash(key); n < kCapacity; n++, i = (i + 1) & (kCapacity - 1)) {
		if (!used[i])
			return -1;
		if (keys[i] == key)
			return ids[i];
	}
	return -1;
}

bool Directory::insert(uint64_t key, uint16_t id) {
	for (uint32_t n = 0, i = dirHash(key); n < kCapacity; n++, i = (i + 1) & (kCapacity - 1)) {
		if (!used[i]) {
			used[i] = true;
			keys[i] = key;
			ids[i] = id;
			return true;
		}
		if (keys[i] == key)
			return false;
	}
	return false;
}

void Sparse::init(uint32_t quotaPages) {
	table.init(quotaPages);
	dir.init();
	for (uint32_t i = 0; i < kMaxPages; i++)
		shadow[i] = nullptr;
	for (uint32_t i = 0; i < kMaxPages / 8; i++)
		dirtyBits[i] = 0;
	nextId = 0;
}

static uint64_t sparseKey(uint32_t level, uint64_t key) {
	return (static_cast<uint64_t>(level) << 56) | (key & 0x00ffffffffffffffull);
}

bool Sparse::get(const Host &h, uint32_t level, uint64_t key, bool create, Page &out) {
	const uint64_t k = sparseKey(level, key);
	int id = dir.find(k);
	if (id < 0) {
		if (!create || nextId >= kMaxPages)
			return false;
		const uint32_t fresh = nextId;
		uint64_t heapOffset = 0;
		if (!table.page(fresh, h.be, heapOffset))
			return false;
		uint64_t *mem = h.allocShadow ? h.allocShadow(h.context) : nullptr;
		uint64_t physical = 0;
		if (!mem || !h.physOf || !h.physOf(h.context, heapOffset, physical) || !dir.insert(k, static_cast<uint16_t>(fresh))) {
			if (mem && h.freeShadow)
				h.freeShadow(h.context, mem);
			table.drop(fresh, h.be);
			return false;
		}
		shadow[fresh] = mem;
		nextId++;
		markDirty(fresh);       // a new page must reach VRAM (zeros) before anything points at it
		id = static_cast<int>(fresh);
	}
	uint64_t heapOffset = table.offsetOf(static_cast<uint32_t>(id));
	uint64_t physical = 0;
	if (!h.physOf(h.context, heapOffset, physical))
		return false;
	out.entries = shadow[id];
	out.phys = physical;
	out.id = static_cast<uint32_t>(id);
	return true;
}

bool Sparse::nextDirty(uint32_t &id, uint64_t *&shadowPage, uint64_t &heapOffset) const {
	for (uint32_t i = 0; i < nextId; i++) {
		if (dirtyBits[i >> 3] & (1u << (i & 7))) {
			id = i;
			shadowPage = shadow[i];
			heapOffset = table.offsetOf(i);
			return true;
		}
	}
	return false;
}

bool Sparse::pageAt(uint32_t id, uint64_t *&shadowPage, uint64_t &heapOffset) const {
	if (id >= nextId || !shadow[id])
		return false;
	shadowPage = shadow[id];
	heapOffset = table.offsetOf(id);
	return true;
}

void Sparse::releaseAll(const Host &h, bool chunks) {
	for (uint32_t i = 0; i < nextId; i++) {
		if (shadow[i] && h.freeShadow)
			h.freeShadow(h.context, shadow[i]);
		shadow[i] = nullptr;
	}
	if (chunks)
		table.release(h.be);
	dir.init();
	nextId = 0;
	for (uint32_t i = 0; i < kMaxPages / 8; i++)
		dirtyBits[i] = 0;
}

} // namespace PtPages
