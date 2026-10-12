// The pre-S8 page-table writing of runtime.cpp (vmMap, vmMapHost, vmUnmap), copied from commit 0cfa416 (git show 0cfa416:src/runtime.cpp, lines
// 492-596 and 614-635). It is the REFERENCE the differential host test (tools/atomdump.cpp, testVmTableDifferential) compares the code the kext now
// runs (src/gpuvmtable.cpp) against. Only the plumbing around the statements changed, so that it compiles on the host:
//   c.tableShadow -> ref.shadow, c.rootPhys -> ref.rootPhys, kVmTableBytes -> ref.tableBytes, vmIsPteOff/vmExecOff/vmPteSet/vmPteClear -> ref.pol.*,
//   gpuPhysical(mc, physical) -> GpuVm::mcToPhysical(mc, ref.fbMcBase, ref.fbOffset, physical), and vmTableSync(...) -> ref.sync(offset, bytes).
// Do not "fix" or modernise anything in here: its value is that it is the old code.

struct LegacyRef {
	uint64_t *shadow;
	uint64_t rootPhys;
	uint64_t tableBytes;
	struct { bool isPteOff, execOff; uint64_t pteSet, pteClear; } pol;
	uint64_t fbMcBase;
	uint32_t fbOffset;
	std::vector<std::pair<uint32_t, uint32_t>> syncs;       // (offset, bytes) the old code asked vmTableSync for, in order
	bool failSync { false };
	bool sync(uint32_t offset, uint32_t bytes) {
		syncs.push_back({ offset, bytes });
		return !failSync;
	}
};

static bool legacyVmMap(LegacyRef &ref, uint64_t va, uint64_t mc, uint64_t bytes, bool executable) {
	if (!ref.shadow || !bytes || (va & (GpuVm::kPageBytes - 1)) ||
	    (mc & (GpuVm::kPageBytes - 1)))
		return false;
	uint64_t physical = 0;
	if (!GpuVm::mcToPhysical(mc, ref.fbMcBase, ref.fbOffset, physical))
		return false;
	const uint64_t end = va + ((bytes + GpuVm::kPageBytes - 1) & ~(GpuVm::kPageBytes - 1));
	if (end < va || end > GpuVm::kVaEnd)
		return false;
	auto entry = [&ref](uint64_t off) -> uint64_t * {
		return ref.shadow + off / sizeof(uint64_t);
	};
	uint64_t firstPt = ~0ull, lastPt = 0;
	for (uint64_t at = va, phys = physical; at < end;
	     at += GpuVm::kPageBytes, phys += GpuVm::kPageBytes) {
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > ref.tableBytes)
			return false;
		const uint32_t pdeIndex = GpuVm::index(at, 2);
		const uint64_t pdeOff = 0x2000 + static_cast<uint64_t>(pdeIndex) * 8;
		if (!*entry(pdeOff))
			*entry(pdeOff) = GpuVm::encodePde(ref.rootPhys + ptOff, GpuVm::kValid, 0);
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		if (firstPt == ~0ull)
			firstPt = ptOff;
		lastPt = ptOff;
		uint64_t flags = GpuVm::kValid | GpuVm::kReadable | GpuVm::kWritable |
		                 (ref.pol.isPteOff ? 0 : GpuVm::kIsPte);
		if (executable || !ref.pol.execOff)
			flags |= GpuVm::kExecutable;
		flags = (flags | ref.pol.pteSet) & ~ref.pol.pteClear;   /* W22 diagnostics only, 0 otherwise */
		const uint64_t block = at & ~0xffffull;
		const bool fragment64k = block >= va && block + 0x10000 <= end &&
			((phys - (at - block)) & 0xffff) == 0;
		*entry(pteOff) = GpuVm::encodePte(phys, flags, fragment64k);
	}
	if (firstPt == ~0ull || !ref.sync(0, 0x3000))
		return false;
	for (uint64_t pt = firstPt; pt <= lastPt; pt += 0x1000)
		if (!ref.sync(static_cast<uint32_t>(pt), 0x1000))
			return false;
	return true;
}

static bool legacyVmMapHost(LegacyRef &ref, uint64_t va, const uint64_t *pageBuses, uint64_t bytes, bool executable) {
	if (!ref.shadow || !pageBuses || !bytes ||
	    (va & (GpuVm::kPageBytes - 1)))
		return false;
	const uint64_t mapped = (bytes + GpuVm::kPageBytes - 1) & ~(GpuVm::kPageBytes - 1);
	const uint64_t end = va + mapped;
	if (mapped < bytes || end < va || end > GpuVm::kVaEnd)
		return false;
	auto entry = [&ref](uint64_t off) -> uint64_t * {
		return ref.shadow + off / sizeof(uint64_t);
	};
	uint64_t firstPt = ~0ull, lastPt = 0;
	uint64_t page = 0;
	for (uint64_t at = va; at < end; at += GpuVm::kPageBytes, page++) {
		if (pageBuses[page] & (GpuVm::kPageBytes - 1))
			return false;
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > ref.tableBytes)
			return false;
		const uint32_t pdeIndex = GpuVm::index(at, 2);
		const uint64_t pdeOff = 0x2000 + static_cast<uint64_t>(pdeIndex) * 8;
		if (!*entry(pdeOff))
			*entry(pdeOff) = GpuVm::encodePde(ref.rootPhys + ptOff, GpuVm::kValid, 0);
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		if (firstPt == ~0ull)
			firstPt = ptOff;
		lastPt = ptOff;
		uint64_t flags = GpuVm::kSystem | GpuVm::kSnooped | GpuVm::kValid |
		                 GpuVm::kReadable | GpuVm::kWritable | (ref.pol.isPteOff ? 0 : GpuVm::kIsPte);
		if (executable || !ref.pol.execOff)
			flags |= GpuVm::kExecutable;
		/* Cached GTT on gfx12.0 uses MTYPE_NC, encoded as zero. */
		*entry(pteOff) = GpuVm::encodePte(pageBuses[page], flags, false);
	}
	if (firstPt == ~0ull || !ref.sync(0, 0x3000))
		return false;
	for (uint64_t pt = firstPt; pt <= lastPt; pt += 0x1000)
		if (!ref.sync(static_cast<uint32_t>(pt), 0x1000))
			return false;
	return true;
}

static void legacyVmUnmap(LegacyRef &ref, uint64_t va, uint64_t bytes) {
	if (!ref.shadow || !bytes || va & (GpuVm::kPageBytes - 1))
		return;
	const uint64_t end = va + ((bytes + GpuVm::kPageBytes - 1) & ~(GpuVm::kPageBytes - 1));
	uint64_t firstPt = ~0ull, lastPt = 0;
	for (uint64_t at = va; at < end && at >= va; at += GpuVm::kPageBytes) {
		const uint64_t relative = (at - GpuVm::kVaStart) >> 21;
		const uint64_t ptOff = 0x3000 + relative * 0x1000;
		if (at < GpuVm::kVaStart || ptOff + 0x1000 > ref.tableBytes)
			break;
		const uint64_t pteOff = ptOff + static_cast<uint64_t>(GpuVm::index(at, 3)) * 8;
		ref.shadow[pteOff / sizeof(uint64_t)] = 0;
		if (firstPt == ~0ull)
			firstPt = ptOff;
		lastPt = ptOff;
	}
	for (uint64_t pt = firstPt; pt != ~0ull && pt <= lastPt; pt += 0x1000)
		ref.sync(static_cast<uint32_t>(pt), 0x1000);
}
