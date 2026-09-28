//
//  gpuheap.hpp
//  RDNA4FB
//
//  A first-fit allocator over a range of VRAM, in fixed granules: user
//  buffers and uploaded kernel code. One byte of state per granule (free /
//  first of an allocation / continuation), kept in a map the caller
//  provides, so an allocation's length is implied and free() needs only its
//  offset. The compute pool's CPU-visible heap uses 4 KiB granules; the heap
//  over the rest of VRAM, gigabytes, uses 64 KiB ones.
//
//  Freestanding and not thread-safe: the runtime (runtime.cpp) holds its
//  lock around every call; the host tests drive it directly.
//

#ifndef GpuHeap_hpp
#define GpuHeap_hpp

#include <stdint.h>

namespace GpuHeap {

class Heap {
public:
	// Manage [base, base + size) (offsets in the caller's space) in granules
	// of `granule` bytes (a power of two, base aligned to it), with `map`
	// holding one byte per granule; `size` is cut to whole granules and to
	// `mapBytes`. A heap without a map is empty.
	void init(uint64_t base, uint64_t size, uint32_t granule, uint8_t *map, uint32_t mapBytes);

	// Granule-aligned offset of a new allocation of at least `bytes` (> 0).
	bool alloc(uint64_t bytes, uint64_t &offset);

	// Release the allocation starting at `offset`; false if none does.
	bool free(uint64_t offset);

	// Length of the allocation starting at `offset` (0 if none).
	uint64_t lengthOf(uint64_t offset) const;

	uint64_t base() const { return start; }
	uint32_t granule() const { return gran; }
	uint64_t size() const { return static_cast<uint64_t>(count) * gran; }
	uint64_t freeBytes() const { return static_cast<uint64_t>(freeCount) * gran; }

private:
	enum : uint8_t { Free = 0, Head = 1, Body = 2 };
	uint8_t  *map { nullptr };
	uint64_t start { 0 };
	uint32_t gran { 4096 };
	uint32_t count { 0 };
	uint32_t freeCount { 0 };

	bool granuleOf(uint64_t offset, uint32_t &g) const;
};

} // namespace GpuHeap

#endif /* GpuHeap_hpp */
