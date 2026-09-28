//
//  gpuvm.cpp
//  RDNA4FB
//
//  See gpuvm.hpp.
//

#include "gpuvm.hpp"

namespace GpuVm {

uint64_t encodePte(uint64_t physical, uint64_t flags, bool fragment64K) {
	uint64_t value = physical & kPhysicalMask;
	value |= flags & ~(kPhysicalMask | kFragMask | kMtypeMask);
	/* VRAM mappings use gfx12's NC MTYPE (zero); never inherit an old field. */
	value &= ~kMtypeMask;
	if (fragment64K)
		value |= static_cast<uint64_t>(kFragment64K) << 7;
	return value;
}

uint64_t encodePde(uint64_t physical, uint64_t flags, uint32_t level) {
	uint64_t value = physical & kPhysicalMask;
	value |= flags & ~(kPhysicalMask | kPdeBfsMask | kPdePte);
	/* gmc_v12_0_get_vm_pde leaves PDE_PTE set through PDB1 and clears it at PDB0. */
	if (level >= 1)
		value |= kPdePte;
	if (level == 1)
		value |= static_cast<uint64_t>(9) << 58;
	return value;
}

uint32_t index(uint64_t va, uint32_t level) {
	if (level >= kLevels)
		return 0;
	return static_cast<uint32_t>((va >> (kPageShift + kIndexBits * (kLevels - 1 - level))) &
	                             (kEntries - 1));
}

uint64_t entryPhysical(uint64_t entry) { return entry & kPhysicalMask; }

bool walk(uint64_t root, uint64_t va, ReadEntry read, void *context,
          uint64_t &physical, uint64_t &flags) {
	if (!read || va >= kVaEnd || root & (kPageBytes - 1))
		return false;
	uint64_t table = root;
	for (uint32_t level = 0; level < kLevels; level++) {
		uint64_t entry = 0;
		const uint64_t address = table + static_cast<uint64_t>(index(va, level)) * 8;
		if (!read(context, address, entry) || !(entry & kValid))
			return false;
		if (level + 1 == kLevels) {
			flags = entry;
			physical = entryPhysical(entry) | (va & (kPageBytes - 1));
			return true;
		}
		table = entryPhysical(entry);
		if (table & (kPageBytes - 1))
			return false;
	}
	return false;
}

} // namespace GpuVm
