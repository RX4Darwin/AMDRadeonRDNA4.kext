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
constexpr uint32_t kMaxPages = 2048;                  // backed pages one client may hold (mode 2): about 4 GiB of VA at one PT page per 2 MiB
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

// Insert-only hash from a 64-bit key to a dense page id (clients never free a page-table page while they are open, so nothing is ever removed).
class Directory {
public:
	static constexpr uint32_t kCapacity = 4096;          // twice kMaxPages: at most half full
	void init();
	int find(uint64_t key) const;                        // id, or -1
	bool insert(uint64_t key, uint16_t id);
private:
	uint64_t keys[kCapacity];
	uint16_t ids[kCapacity];
	bool     used[kCapacity];
};

struct Page {
	uint64_t *entries;       // the 512 shadow entries
	uint64_t phys;           // GPU-physical address of the page (what a PDE holds)
	uint32_t id;
};

struct Host {
	Backend be;                                          // 64 KiB chunk allocator
	uint64_t *(*allocShadow)(void *context);             // a zeroed 4 KiB page of host memory, or nullptr
	void (*freeShadow)(void *context, uint64_t *page);
	bool (*physOf)(void *context, uint64_t heapOffset, uint64_t &physical);
	void *context;
};

// One client's whole multi-level table (rdna4-vmshared=2): the root (level 0), PDB1 pages (level 1, one per 512 GiB), PDB0 pages (level 2, one per
// GiB of VA), PT pages (level 3, one per 2 MiB), all created on demand. A page is identified by (level, key): key = va >> 39, >> 30, >> 21 for
// levels 1-3, 0 for the root. The legacy layout could only address the first GiB (one PDB1 entry); this one addresses as many GiB as the quota
// allows. Bookkeeping only: the memory comes through Host.
class Sparse {
public:
	void init(uint32_t quotaPages);
	// The page (level, key); with `create`, backed on first use (VRAM page from a chunk + a zeroed shadow) and marked dirty. False: not there,
	// or over quota / out of memory (nothing half-made).
	bool get(const Host &h, uint32_t level, uint64_t key, bool create, Page &out);
	void markDirty(uint32_t id) { if (id < kMaxPages) dirtyBits[id >> 3] |= static_cast<uint8_t>(1u << (id & 7)); }
	// The next page whose shadow still has to go to VRAM; false when none. The flag stays set until clean(): a failed sync loses nothing.
	bool nextDirty(uint32_t &id, uint64_t *&shadowPage, uint64_t &heapOffset) const;
	void clean(uint32_t id) { if (id < kMaxPages) dirtyBits[id >> 3] &= static_cast<uint8_t>(~(1u << (id & 7))); }
	uint32_t pages() const { return table.pages(); }
	uint32_t chunksHeld() const { return table.chunksHeld(); }
	uint32_t quota() const { return table.quota(); }
	// Every shadow page back to the host and, with `chunks`, every VRAM chunk to the allocator (after a sleep the VRAM is gone, so the chunks are not returned).
	void releaseAll(const Host &h, bool chunks);
	// Iterate the backed pages (teardown): id in [0, idCount()), false when the id holds nothing.
	uint32_t idCount() const { return nextId; }
	bool pageAt(uint32_t id, uint64_t *&shadowPage, uint64_t &heapOffset) const;
private:
	Table table;
	Directory dir;
	uint64_t *shadow[kMaxPages];
	uint8_t  dirtyBits[kMaxPages / 8];
	uint32_t nextId { 0 };
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
