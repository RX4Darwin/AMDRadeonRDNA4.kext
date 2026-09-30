//
//  gpuvmtable.hpp
//  RDNA4FB
//
//  The page-table writing of runtime.cpp's vmMap / vmMapHost / vmUnmap, freestanding (like gpuvm.cpp) so that a host test runs the SAME code the kext
//  runs. Entries are reached through an accessor (contiguous 4 MiB image for modes 0/1, sparse pages for rdna4-vmshared=2), so one body serves both.
//  tools/atomdump.cpp compares it byte for byte against the pre-S8 code (tools/legacy-vmtable-ref.inc, from commit 0cfa416) for the contiguous
//  image: docs/w13-vmid.md s.5.10 (hub-task-340).
//
//  The logical image: root at 0, PDB1 at 0x1000, PDB0 at 0x2000, one PT page per 2 MiB of VA from kVaStart at 0x3000 + n * 0x1000.
//

#ifndef RDNA4GpuVmTable_hpp
#define RDNA4GpuVmTable_hpp

#include <stdint.h>

namespace GpuVmTable {

struct Access {
	// The entry at logical byte offset `off` of the image (8-byte aligned); nullptr if it cannot be provided (sparse: quota or memory).
	uint64_t *(*entry)(void *context, uint64_t off);
	// The GPU-physical address of logical offset `off` (what a PDE holds for a PT page); false if the page is not backed.
	bool (*phys)(void *context, uint64_t off, uint64_t &physical);
	void *context;
};

// The W22/W36/W40 leaf-flag diagnostics (boot-args rdna4-vm-ispte, rdna4-vm-exec and the ladder's set/clear masks).
struct Policy {
	bool isPteOff;       // rdna4-vm-ispte=0: leave bit 63 off the leaf
	bool execOff;        // rdna4-vm-exec=0: only `executable` mappings get EXECUTABLE
	uint64_t pteSet, pteClear;
};

// The PT pages a call touched: the caller syncs the top three pages and [firstPt, lastPt] step 0x1000. firstPt == ~0 when none.
struct Span {
	uint64_t firstPt { ~0ull }, lastPt { 0 };
};

// The contiguous image of modes 0 and 1: entries live in one shadow array, PT pages at rootPhys + their logical offset. runtime.cpp's ptEntry/ptPhys use
// exactly these, so the differential test covers them too.
inline uint64_t *legacyEntry(uint64_t *shadow, uint64_t off) { return shadow ? shadow + off / sizeof(uint64_t) : nullptr; }
inline uint64_t legacyPhys(uint64_t rootPhys, uint64_t off) { return rootPhys + off; }

// ---- the multi-level layout of rdna4-vmshared=2 (docs/w13-vmid.md s.5.10) ----
// The contiguous image above has ONE PDB1 entry and ONE PDB0 page, so a client's VA is limited to 1 GiB above kVaStart (and a map across that line
// leaves the far side unreachable). Here every directory is a page of its own that exists only when something lives below it: the root (level 0, key 0),
// PDB1 pages (level 1, key va >> 39), PDB0 pages (level 2, key va >> 30), PT pages (level 3, key va >> 21). The VA limit is the client's page quota.
struct Tree {
	// The page (level, key): its 512 shadow entries, GPU-physical address and id. With `create` it is backed on first use. False: absent, or over quota.
	bool (*page)(void *context, uint32_t level, uint64_t key, bool create, uint64_t *&entries, uint64_t &phys, uint32_t &id);
	void (*dirty)(void *context, uint32_t id);       // the page's shadow changed and has to reach VRAM
	void *context;
};

// Same contract as mapVram / mapHost / unmap, for a Tree. The pages that changed are reported through Tree::dirty; the caller syncs them.
bool treeMapVram(const Tree &t, const Policy &p, uint64_t va, uint64_t end, uint64_t physical, bool executable);
bool treeMapHost(const Tree &t, const Policy &p, uint64_t va, uint64_t end, const uint64_t *pageBuses, bool executable);
void treeUnmap(const Tree &t, uint64_t va, uint64_t end);

// Map [va, end) to `physical` upward (VRAM, no SNOOPED). The caller validated the alignment, `end` and the VA range. False on a failure that the old code
// also returned false for (VA below kVaStart or past the image, `tableBytes`), or an entry the accessor refused; entries written before it stay written.
bool mapVram(const Access &a, const Policy &p, uint64_t tableBytes, uint64_t va, uint64_t end, uint64_t physical, bool executable, Span &out);

// Map [va, va + pages) to the bus addresses `pageBuses` (system memory, SNOOPED).
bool mapHost(const Access &a, const Policy &p, uint64_t tableBytes, uint64_t va, uint64_t end, const uint64_t *pageBuses, bool executable, Span &out);

// Clear the leaf PTEs of [va, end). Stops at the first address outside the image, as before.
void unmap(const Access &a, uint64_t tableBytes, uint64_t va, uint64_t end, Span &out);

} // namespace GpuVmTable

#endif /* RDNA4GpuVmTable_hpp */
