//
//  vmtree.hpp
//  RDNA4FB
//
//  A process's GPU page table for the Vulkan interface (n48n.hpp): four
//  levels, 48 bits, pages mapped wherever the caller asks. Unlike the
//  runtime's own table (gpuvmtable.hpp: a fixed layout over a few GiB from
//  kVaStart), nothing about the layout is decided here: directory and table
//  pages are taken one at a time, as the mappings need them, from whoever
//  owns the memory. Entries are GpuVm's.
//
//  Freestanding and not thread-safe: the caller holds its lock. Shared by
//  the kext and the host test (tools/atomdump.cpp).
//

#ifndef RDNA4VmTree_hpp
#define RDNA4VmTree_hpp

#include <stdint.h>

namespace VmTree {

// Where the tree's 4 KiB pages live. `physical` is what a directory entry
// holds: the page's GPU-physical address.
struct Pages {
	void *context;
	bool      (*alloc)(void *context, uint64_t &physical);     // one page, zeroed
	void      (*free)(void *context, uint64_t physical);
	uint64_t *(*entries)(void *context, uint64_t physical);    // its 512 entries, for the CPU
};

// Map `pages` 4 KiB pages at `va` (48-bit, page aligned) to `physical`
// onwards, each leaf carrying `leafFlags` (GpuVm::kValid and so on; the
// leaf bit is added). False if a table page could not be had: what was
// mapped of the range stays, for the caller to unmap.
bool map(const Pages &p, uint64_t root, uint64_t va, uint64_t physical, uint64_t pages, uint64_t leafFlags);

// Clear the leaves of a range. Directory and table pages are kept.
// ponytail: they are only given back with the whole tree (destroy); count
// and release empty ones here if a client's address space churns enough to
// matter.
void unmap(const Pages &p, uint64_t root, uint64_t va, uint64_t pages);

// Give back every page of the tree, the root included.
void destroy(const Pages &p, uint64_t root);

} // namespace VmTree

#endif /* RDNA4VmTree_hpp */
