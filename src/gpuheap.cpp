//
//  gpuheap.cpp
//  RDNA4FB
//
//  See gpuheap.hpp.
//

#include "gpuheap.hpp"

namespace GpuHeap {

void Heap::init(uint64_t base, uint64_t size, uint32_t granule, uint8_t *m, uint32_t mapBytes) {
	start = base;
	gran = granule ? granule : 4096;
	map = m;
	uint64_t n = m ? size / gran : 0;
	count = static_cast<uint32_t>(n > mapBytes ? mapBytes : n);
	freeCount = count;
	for (uint32_t g = 0; g < count; g++)
		map[g] = Free;
}

bool Heap::granuleOf(uint64_t offset, uint32_t &g) const {
	if (offset < start || (offset - start) % gran)
		return false;
	uint64_t i = (offset - start) / gran;
	if (i >= count)
		return false;
	g = static_cast<uint32_t>(i);
	return true;
}

bool Heap::alloc(uint64_t bytes, uint64_t &offset) {
	if (!bytes || bytes > size())
		return false;
	const uint64_t need64 = (bytes + gran - 1) / gran;
	if (need64 > freeCount)
		return false;
	const uint32_t need = static_cast<uint32_t>(need64);
	uint32_t run = 0;
	for (uint32_t g = 0; g < count; g++) {
		run = map[g] == Free ? run + 1 : 0;
		if (run == need) {
			const uint32_t first = g + 1 - need;
			map[first] = Head;
			for (uint32_t i = first + 1; i <= g; i++)
				map[i] = Body;
			freeCount -= need;
			offset = start + static_cast<uint64_t>(first) * gran;
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
	return static_cast<uint64_t>(n) * gran;
}

} // namespace GpuHeap
