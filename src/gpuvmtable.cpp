//
//  gpuvmtable.cpp
//  RDNA4FB
//
//  See gpuvmtable.hpp. The bodies are the loops of the pre-S8 vmMap / vmMapHost / vmUnmap (runtime.cpp at 0cfa416), statement for statement, with the
//  table access going through Access.
//

#include "gpuvmtable.hpp"
#include "gpuvm.hpp"

namespace GpuVmTable {

// The leaf PTE of a VRAM mapping of [va, end) at address `at` (physical `phys`): shared by the contiguous and the tree walkers.
static uint64_t vramLeaf(const Policy &pol, uint64_t va, uint64_t end, uint64_t at, uint64_t phys, bool executable) {
	/* amdgpu's composition for a VRAM BO on GC 12: VALID, READABLE, WRITEABLE, IS_PTE
	 * (gart_pte_flags, amdgpu_ttm.c:1477; gmc_v12_0.c:794-796), EXECUTABLE when asked
	 * (gmc_v12_0_get_vm_pte), MTYPE NC = 0, and no SNOOPED: amdgpu_ttm_tt_pde_flags adds it for
	 * VRAM only when the BO is cached (amdgpu_ttm.c:1456-1458). */
	uint64_t flags = GpuVm::kValid | GpuVm::kReadable | GpuVm::kWritable |
	                 (pol.isPteOff ? 0 : GpuVm::kIsPte);
	/* Linux's own tables on this card (rdna4-groundtruth vm-walk.txt) have EXE, READ and
	 * WRITE on every leaf (0x...5f1 / 0x...3f1): the CP fetches the EOP buffer, the ring and
	 * IBs with EXECUTE (IV src_data[1] 0x50 = READ|EXE; round 5: PERMISSION_FAULTS 8 on the EOP
	 * page mapped R|W).  Mesa maps every BO R|W|X the same way (ac_linux_drm.c:235).
	 * rdna4-vm-exec=0 restores the old R|W-only pages (negative control). */
	if (executable || !pol.execOff)
		flags |= GpuVm::kExecutable;
	flags = (flags | pol.pteSet) & ~pol.pteClear;   /* W22 diagnostics only, 0 otherwise */
	/* FRAG=4 says this PTE is part of a contiguous, 64 KiB-aligned run of
	 * sixteen: only when the whole aligned block lies inside this mapping
	 * (amdgpu_vm_pte_fragment); a lone 4 KiB page that merely happens to
	 * be 64 KiB-aligned must stay FRAG=0. */
	const uint64_t block = at & ~0xffffull;
	const bool fragment64k = block >= va && block + 0x10000 <= end &&
		((phys - (at - block)) & 0xffff) == 0;
	return GpuVm::encodePte(phys, flags, fragment64k);
}

static uint64_t hostLeaf(const Policy &pol, uint64_t bus, bool executable) {
	uint64_t flags = GpuVm::kSystem | GpuVm::kSnooped | GpuVm::kValid |
	                 GpuVm::kReadable | GpuVm::kWritable | (pol.isPteOff ? 0 : GpuVm::kIsPte);
	if (executable || !pol.execOff)
		flags |= GpuVm::kExecutable;
	/* Cached GTT on gfx12.0 uses MTYPE_NC, encoded as zero. */
	return GpuVm::encodePte(bus, flags, false);
}

bool mapVram(const Access &a, const Policy &pol, uint64_t tableBytes, uint64_t va, uint64_t end, uint64_t physical, bool executable, Span &out) {
	out = Span {};
	for (uint64_t at = va, phys = physical; at < end;
	     at += GpuVm::kPageBytes, phys += GpuVm::kPageBytes) {
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > tableBytes)
			return false;
		const uint32_t pdeIndex = GpuVm::index(at, 2);
		const uint64_t pdeOff = 0x2000 + static_cast<uint64_t>(pdeIndex) * 8;
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		uint64_t *pde = a.entry(a.context, pdeOff), *pte = a.entry(a.context, pteOff);
		uint64_t ptPhysical = 0;
		if (!pde || !pte || !a.phys(a.context, ptOff, ptPhysical))
			return false;
		if (!*pde)
			*pde = GpuVm::encodePde(ptPhysical, GpuVm::kValid, 0);
		if (out.firstPt == ~0ull)
			out.firstPt = ptOff;
		out.lastPt = ptOff;
		*pte = vramLeaf(pol, va, end, at, phys, executable);
	}
	return true;
}

bool mapHost(const Access &a, const Policy &pol, uint64_t tableBytes, uint64_t va, uint64_t end, const uint64_t *pageBuses, bool executable, Span &out) {
	out = Span {};
	uint64_t page = 0;
	for (uint64_t at = va; at < end; at += GpuVm::kPageBytes, page++) {
		if (pageBuses[page] & (GpuVm::kPageBytes - 1))
			return false;
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > tableBytes)
			return false;
		const uint32_t pdeIndex = GpuVm::index(at, 2);
		const uint64_t pdeOff = 0x2000 + static_cast<uint64_t>(pdeIndex) * 8;
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		uint64_t *pde = a.entry(a.context, pdeOff), *pte = a.entry(a.context, pteOff);
		uint64_t ptPhysical = 0;
		if (!pde || !pte || !a.phys(a.context, ptOff, ptPhysical))
			return false;
		if (!*pde)
			*pde = GpuVm::encodePde(ptPhysical, GpuVm::kValid, 0);
		if (out.firstPt == ~0ull)
			out.firstPt = ptOff;
		out.lastPt = ptOff;
		*pte = hostLeaf(pol, pageBuses[page], executable);
	}
	return true;
}

void unmap(const Access &a, uint64_t tableBytes, uint64_t va, uint64_t end, Span &out) {
	out = Span {};
	for (uint64_t at = va; at < end && at >= va; at += GpuVm::kPageBytes) {
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > tableBytes)
			break;
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		if (uint64_t *pte = a.entry(a.context, pteOff))   // sparse: a page that was never backed holds no PTE to clear
			*pte = 0;
		if (out.firstPt == ~0ull)
			out.firstPt = ptOff;
		out.lastPt = ptOff;
	}
}

// ---- tree layout ----

// The four pages on the way to the PT page of `at`, created as needed and chained (PDE written only where empty, like the contiguous code). The
// leaf PT page is returned in `pt`; false when any of them could not be provided.
static bool treePath(const Tree &t, uint64_t at, bool create, uint64_t *&ptEntries, uint32_t &ptId) {
	uint64_t *e[4];
	uint64_t phys[4];
	uint32_t id[4];
	const uint64_t key[4] = { 0, at >> 39, at >> 30, at >> 21 };
	for (uint32_t level = 0; level < 4; level++)
		if (!t.page(t.context, level, key[level], create, e[level], phys[level], id[level]))
			return false;
	for (uint32_t level = 0; level < 3; level++) {
		uint64_t &pde = e[level][GpuVm::index(at, level)];
		if (!pde) {
			pde = GpuVm::encodePde(phys[level + 1], GpuVm::kValid, 2 - level);
			t.dirty(t.context, id[level]);
		}
	}
	ptEntries = e[3];
	ptId = id[3];
	return true;
}

bool treeMapVram(const Tree &t, const Policy &pol, uint64_t va, uint64_t end, uint64_t physical, bool executable) {
	uint64_t *pt = nullptr;
	uint32_t ptId = 0;
	uint64_t ptKey = ~0ull;
	for (uint64_t at = va, phys = physical; at < end; at += GpuVm::kPageBytes, phys += GpuVm::kPageBytes) {
		if (at < GpuVm::kVaStart || at >= GpuVm::kVaEnd)
			return false;
		if ((at >> 21) != ptKey) {
			if (!treePath(t, at, true, pt, ptId))
				return false;
			ptKey = at >> 21;
		}
		pt[GpuVm::index(at, 3)] = vramLeaf(pol, va, end, at, phys, executable);
		t.dirty(t.context, ptId);
	}
	return true;
}

bool treeMapHost(const Tree &t, const Policy &pol, uint64_t va, uint64_t end, const uint64_t *pageBuses, bool executable) {
	uint64_t *pt = nullptr;
	uint32_t ptId = 0;
	uint64_t ptKey = ~0ull;
	uint64_t page = 0;
	for (uint64_t at = va; at < end; at += GpuVm::kPageBytes, page++) {
		if (pageBuses[page] & (GpuVm::kPageBytes - 1))
			return false;
		if (at < GpuVm::kVaStart || at >= GpuVm::kVaEnd)
			return false;
		if ((at >> 21) != ptKey) {
			if (!treePath(t, at, true, pt, ptId))
				return false;
			ptKey = at >> 21;
		}
		pt[GpuVm::index(at, 3)] = hostLeaf(pol, pageBuses[page], executable);
		t.dirty(t.context, ptId);
	}
	return true;
}

void treeUnmap(const Tree &t, uint64_t va, uint64_t end) {
	uint64_t *pt = nullptr;
	uint64_t phys = 0;
	uint32_t ptId = 0;
	uint64_t ptKey = ~0ull;
	bool have = false;
	for (uint64_t at = va; at < end && at >= va; at += GpuVm::kPageBytes) {
		if (at < GpuVm::kVaStart || at >= GpuVm::kVaEnd)
			break;
		if ((at >> 21) != ptKey) {
			ptKey = at >> 21;
			have = t.page(t.context, 3, ptKey, false, pt, phys, ptId);   // a PT page that was never backed holds no PTE to clear
		}
		if (have && pt[GpuVm::index(at, 3)]) {
			pt[GpuVm::index(at, 3)] = 0;
			t.dirty(t.context, ptId);
		}
	}
}

} // namespace GpuVmTable
