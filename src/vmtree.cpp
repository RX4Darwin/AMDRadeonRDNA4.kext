//
//  vmtree.cpp
//  RDNA4FB
//
//  See vmtree.hpp.
//

#include "vmtree.hpp"
#include "gpuvm.hpp"

namespace VmTree {

namespace {

// The last-level table of `va`, made on the way if `make`; null if it is not there or could not be made.
uint64_t *leafTable(const Pages &p, uint64_t root, uint64_t va, bool make) {
	uint64_t *table = p.entries(p.context, root);
	for (uint32_t level = 0; table && level + 1 < GpuVm::kLevels; level++) {
		uint64_t &entry = table[GpuVm::index(va, level)];
		if (!(entry & GpuVm::kValid)) {
			uint64_t page = 0;
			if (!make || !p.alloc(p.context, page))
				return nullptr;
			entry = GpuVm::encodePde(page, GpuVm::kValid, level);
		}
		table = p.entries(p.context, GpuVm::entryPhysical(entry));
	}
	return table;
}

void destroyLevel(const Pages &p, uint64_t page, uint32_t level) {
	if (level + 1 < GpuVm::kLevels)
		if (const uint64_t *table = p.entries(p.context, page))
			for (uint32_t i = 0; i < GpuVm::kEntries; i++)
				if (table[i] & GpuVm::kValid)
					destroyLevel(p, GpuVm::entryPhysical(table[i]), level + 1);
	p.free(p.context, page);
}

} // namespace

bool map(const Pages &p, uint64_t root, uint64_t va, uint64_t physical, uint64_t pages, uint64_t leafFlags) {
	for (uint64_t i = 0; i < pages; i++, va += GpuVm::kPageBytes, physical += GpuVm::kPageBytes) {
		uint64_t *table = leafTable(p, root, va, true);
		if (!table)
			return false;
		// Not GpuVm::encodePte: that one drops the memory type, which a caller of this interface chooses.
		table[GpuVm::index(va, GpuVm::kLevels - 1)] =
			(physical & GpuVm::kPhysicalMask) | (leafFlags & ~GpuVm::kPhysicalMask) | GpuVm::kIsPte;
	}
	return true;
}

void unmap(const Pages &p, uint64_t root, uint64_t va, uint64_t pages) {
	for (uint64_t i = 0; i < pages; i++, va += GpuVm::kPageBytes)
		if (uint64_t *table = leafTable(p, root, va, false))
			table[GpuVm::index(va, GpuVm::kLevels - 1)] = 0;
}

void destroy(const Pages &p, uint64_t root) { destroyLevel(p, root, 0); }

} // namespace VmTree
