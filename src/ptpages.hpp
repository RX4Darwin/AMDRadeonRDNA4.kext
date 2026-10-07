//
//  ptpages.hpp
//  RDNA4FB
//
//  W13 S6/S8 (docs/w13-vmid.md s.5.2): demand-allocated page tables. A client's table is still addressed as the logical 4 MiB image runtime.cpp's
//  vmMap uses (root, PDB1, PDB0, then one PT page per 2 MiB of VA at 0x3000 + n * 0x1000), but only the logical pages that hold something are backed,
//  four KiB at a time, carved out of 64 KiB heap chunks (the device heap's granule) so a client costs 12-16 KiB of VRAM instead of 4 MiB. This file is the
//  bookkeeping only (no memory is touched), so the host test can exercise it; the kernel supplies the chunk allocator through Backend.
//
//  Also the pool-slot layout check: a per-client pool area must stay below the gfx region (the old layout put slot 64 on top of it, unchecked).
//

#ifndef RDNA4PtPages_hpp
#define RDNA4PtPages_hpp

#include <stdint.h>

namespace PtPages {

constexpr uint32_t kPageBytes = 0x1000;
constexpr uint32_t kChunkPages = 16;                  // one 64 KiB heap granule
constexpr uint32_t kMaxPages = 1024;                  // the logical image: 4 MiB
constexpr uint32_t kMaxChunks = kMaxPages / kChunkPages * 2;   // slack: a chunk can be partly used for a long time

struct Backend {
	bool (*allocChunk)(void *context, uint64_t &heapOffset);   // 64 KiB, kChunkPages * kPageBytes, granule aligned
	void (*freeChunk)(void *context, uint64_t heapOffset);
	void *context;
};

class Table {
public:
	// `quotaPages`: the most backed pages this table may hold (<= kMaxPages).
	void init(uint32_t quotaPages);

	// Heap offset of logical page `logical`, backing it if it has none. False: over quota, or the chunk allocator refused
	// (nothing is left half done: a page either has its slot or does not exist).
	bool page(uint32_t logical, const Backend &b, uint64_t &heapOffset);

	bool has(uint32_t logical) const { return logical < kMaxPages && handle[logical] != 0; }
	uint64_t offsetOf(uint32_t logical) const;           // 0 if not backed
	uint32_t pages() const { return backed; }
	uint32_t chunksHeld() const { return chunkCount; }
	uint32_t quota() const { return limit; }

	// Drop one page's backing (its slot becomes reusable; a chunk with no slot in use is freed). The caller zeroed the memory.
	void drop(uint32_t logical, const Backend &b);

	// Every chunk back to the allocator.
	void release(const Backend &b);

private:
	uint32_t handle[kMaxPages];                 // (chunk index << 4 | slot) + 1, 0 = not backed
	uint64_t chunkOff[kMaxChunks];
	uint16_t chunkMask[kMaxChunks];             // slots in use
	bool     chunkLive[kMaxChunks];
	uint32_t chunkCount { 0 }, backed { 0 }, limit { 0 };
};

// The pool area of client `index` in a region [base, limit) of `stride`-byte areas: false if it does not end inside the region.
inline bool areaFor(uint32_t base, uint32_t stride, uint32_t index, uint32_t limit, uint32_t &out) {
	const uint64_t start = static_cast<uint64_t>(base) + static_cast<uint64_t>(index) * stride;
	if (!stride || start + stride > limit)
		return false;
	out = static_cast<uint32_t>(start);
	return true;
}

} // namespace PtPages

#endif /* RDNA4PtPages_hpp */
