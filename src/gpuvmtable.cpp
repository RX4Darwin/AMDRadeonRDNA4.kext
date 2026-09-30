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
		*pte = GpuVm::encodePte(phys, flags, fragment64k);
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
		uint64_t flags = GpuVm::kSystem | GpuVm::kSnooped | GpuVm::kValid |
		                 GpuVm::kReadable | GpuVm::kWritable | (pol.isPteOff ? 0 : GpuVm::kIsPte);
		if (executable || !pol.execOff)
			flags |= GpuVm::kExecutable;
		/* Cached GTT on gfx12.0 uses MTYPE_NC, encoded as zero. */
		*pte = GpuVm::encodePte(pageBuses[page], flags, false);
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

} // namespace GpuVmTable
