//
//  gpuheap.cpp
//  RDNA4FB
//
//  See gpuheap.hpp.
//

#include "gpuheap.hpp"

namespace GpuHeap {

void Heap::init(uint64_t base, uint64_t size) {
	start = base;
	uint64_t n = size / kGranule;
	count = static_cast<uint32_t>(n > kMaxGranules ? kMaxGranules : n);
	freeCount = count;
	for (uint32_t g = 0; g < kMaxGranules; g++)
		map[g] = Free;
}

bool Heap::granuleOf(uint64_t offset, uint32_t &g) const {
	if (offset < start || (offset - start) % kGranule)
		return false;
	uint64_t i = (offset - start) / kGranule;
	if (i >= count)
		return false;
	g = static_cast<uint32_t>(i);
	return true;
}

bool Heap::alloc(uint64_t bytes, uint64_t &offset) {
	if (!bytes || bytes > size())
		return false;
	const uint32_t need = static_cast<uint32_t>((bytes + kGranule - 1) / kGranule);
	if (need > freeCount)
		return false;
	uint32_t run = 0;
	for (uint32_t g = 0; g < count; g++) {
		run = map[g] == Free ? run + 1 : 0;
		if (run == need) {
			const uint32_t first = g + 1 - need;
			map[first] = Head;
			for (uint32_t i = first + 1; i <= g; i++)
				map[i] = Body;
			freeCount -= need;
			offset = start + static_cast<uint64_t>(first) * kGranule;
			return true;
		}
	}
	return false;
}

bool Heap::free(uint64_t offset) {
	uint32_t g;
	if (!granuleOf(offset, g) || map[g] != Head)
		return false;
	map[g++] = Free;
	freeCount++;
	for (; g < count && map[g] == Body; g++) {
		map[g] = Free;
		freeCount++;
	}
	return true;
}

uint64_t Heap::lengthOf(uint64_t offset) const {
	uint32_t g;
	if (!granuleOf(offset, g) || map[g] != Head)
		return 0;
	uint32_t n = 1;
	while (g + n < count && map[g + n] == Body)
		n++;
	return static_cast<uint64_t>(n) * kGranule;
}

} // namespace GpuHeap
