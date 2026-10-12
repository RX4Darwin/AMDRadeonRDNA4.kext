//
//  n48n.cpp
//  RDNA4FB
//
//  See n48n.hpp.
//

#include "n48n.hpp"
#include "gpuvm.hpp"

namespace N48N {

namespace {

constexpr uint64_t kPage = GpuVm::kPageBytes;
constexpr uint64_t kVaMask = (1ull << kVaBits) - 1;
constexpr uint64_t kMaxAlign = 2ull << 20;
// AMDGPU_PTE_MTYPE_GFX12(MTYPE_UC): uncached. Everything else is NC, zero.
constexpr uint64_t kMtypeUc = 2ull << 54;

// Bits 63..47 of a GPU address all equal: it is one of the two halves of the 48-bit space.
constexpr bool canonical(uint64_t va) { return (va >> 47) == 0 || (va >> 47) == 0x1ffff; }

// What a mapping's flags and its buffer make of a page-table entry (gmc_v12_0_get_vm_pte):
// permissions as asked, uncached if either the mapping or the buffer says so, and system
// memory marked as such.
constexpr uint64_t leafFlags(uint32_t vmFlags, bool uncached, bool system) {
	return GpuVm::kValid | ((vmFlags & N48N_VM_PAGE_READABLE) ? GpuVm::kReadable : 0) |
	       ((vmFlags & N48N_VM_PAGE_WRITEABLE) ? GpuVm::kWritable : 0) |
	       ((vmFlags & N48N_VM_PAGE_EXECUTABLE) ? GpuVm::kExecutable : 0) |
	       (((vmFlags & N48N_VM_MTYPE_MASK) == N48N_VM_MTYPE_UC || uncached) ? kMtypeUc : 0) |
	       (system ? GpuVm::kSystem | GpuVm::kSnooped : 0);
}

template <typename T> T read(const void *p) {
	T v;
	__builtin_memcpy(&v, p, sizeof(v));
	return v;
}

} // namespace

bool Client::open(const Backend &backend, uint32_t id, uint32_t kextBuild, uint32_t gc) {
	if (opened)
		return false;
	reset();
	be = backend;
	vmid = id;
	build = kextBuild;
	gcVersion = gc;
	if (!be.tables.alloc(be.tables.context, rootPage))
		return false;
	opened = true;
	return true;
}

void Client::close() {
	if (!opened)
		return;
	(void)quiesce();
	(void)scanGiveBack();
	for (uint32_t h = 1; h < N48N_MAX_BOS; h++)
		if (buffers[h].bytes)
			be.free(be.context, buffers[h].memory, buffers[h].bytes);
	VmTree::destroy(be.tables, rootPage);
	be.flush(be.context);
	reset();
}

// In place: the tables are a few hundred KiB, more than a kernel stack holds as a temporary.
void Client::reset() { __builtin_memset(static_cast<void *>(this), 0, sizeof(*this)); }

const Buffer *Client::buffer(uint32_t handle) const {
	return handle && handle < N48N_MAX_BOS && buffers[handle].bytes ? &buffers[handle] : nullptr;
}

uint32_t Client::create(const n48n_gem_create_in &in, uint64_t *out) {
	if (in.bo_size == 0 || in.bo_size > (1ull << 40))
		return kBadArgument;
	if (in.alignment && ((in.alignment & (in.alignment - 1)) || in.alignment > kMaxAlign))
		return kBadArgument;
	const uint64_t bytes = (in.bo_size + kPage - 1) & ~(kPage - 1);
	const uint64_t align = in.alignment < kPage ? kPage : in.alignment;
	const uint64_t flags = in.domain_flags;
	if (in.domains == 0 || (in.domains & ~static_cast<uint64_t>(N48N_GEM_DOMAIN_GTT | N48N_GEM_DOMAIN_VRAM)))
		return kUnsupported;
	if (flags & (N48N_GEM_ENCRYPTED | N48N_GEM_CP_MQD_GFX9 | N48N_GEM_PREEMPTIBLE))
		return kUnsupported;

	uint32_t handle = 1;
	while (handle < N48N_MAX_BOS && buffers[handle].bytes)
		handle++;
	if (handle >= N48N_MAX_BOS)
		return kNoResources;

	Buffer b {};
	if (in.domains & N48N_GEM_DOMAIN_VRAM) {
		// The pool the CPU can reach first; the other only for a buffer the CPU will not touch.
		const bool highAllowed = (flags & N48N_GEM_NO_CPU_ACCESS) && !(flags & N48N_GEM_CPU_ACCESS_REQUIRED);
		if (!be.allocVram(be.context, bytes, align, highAllowed, b.memory))
			return kNoMemory;
	} else {
		if (bytes > kSystemMaxBuffer || systemUsed + bytes > kSystemCap || !be.allocSystem(be.context, bytes, b.memory))
			return kNoMemory;
		systemUsed += bytes;
	}
	b.bytes = bytes;
	b.uncached = flags & N48N_GEM_UNCACHED;
	buffers[handle] = b;
	out[0] = handle;
	out[1] = bytes;
	out[2] = b.memory.system ? N48N_GEM_DOMAIN_GTT : N48N_GEM_DOMAIN_VRAM;
	out[3] = b.memory.system ? N48N_PLACED_ZEROED
	         : b.memory.high ? N48N_PLACED_HI_POOL : (N48N_PLACED_CPU_MAPPABLE | N48N_PLACED_ZEROED);
	return kSuccess;
}

void Client::unmapTable(const Map &m) { VmTree::unmap(be.tables, rootPage, m.va, m.bytes / kPage); }

uint32_t Client::release(uint32_t handle) {
	if (!buffer(handle))
		return kNotFound;
	// ponytail: work on the queue may use any buffer, so all of it is waited for; a list of each submission's
	// buffers if freeing while the queue is busy turns out to matter.
	(void)quiesce();
	// A buffer registered for the display may be the one shown: the display goes back first, all of it.
	for (const Slot &slot : slots)
		if (slot.used && slot.handle == handle) {
			(void)scanGiveBack();
			break;
		}
	// Its mappings go first, and the GPU must have forgotten them before the memory is anyone else's.
	bool mapped = false;
	for (Map &m : maps)
		if (m.bytes && m.handle == handle) {
			unmapTable(m);
			m = Map {};
			mapped = true;
		}
	if (mapped)
		be.flush(be.context);
	Buffer &b = buffers[handle];
	if (b.memory.system)
		systemUsed -= b.bytes;
	be.free(be.context, b.memory, b.bytes);
	b = Buffer {};
	return kSuccess;
}

uint32_t Client::mapping(const n48n_gem_va &v) {
	if (v._pad || v.vm_timeline_point || v.vm_timeline_syncobj_out || v.num_syncobj_handles ||
	    v.input_fence_syncobj_handles)
		return kBadArgument;
	if (v.handle == 0 || (v.flags & N48N_VM_PAGE_PRT))
		return kUnsupported;
	if (v.operation == N48N_VA_OP_CLEAR || v.operation == N48N_VA_OP_REPLACE)
		return kUnsupported;
	if (v.operation != N48N_VA_OP_MAP && v.operation != N48N_VA_OP_UNMAP)
		return kBadArgument;
	const Buffer *b = buffer(v.handle);
	if (!b)
		return kNotFound;
	if (!canonical(v.va_address) || ((v.va_address | v.offset_in_bo | v.map_size) & (kPage - 1)) || v.map_size == 0)
		return kBadArgument;
	const uint64_t va = v.va_address & kVaMask;
	// Inside one half of the address space: never across the seam at bit 47.
	if (va < kVaFirst || v.map_size > kVaMask + 1 - va || (va >> 47) != ((va + v.map_size - 1) >> 47))
		return kBadArgument;
	if (v.offset_in_bo > b->bytes || v.map_size > b->bytes - v.offset_in_bo)
		return kBadArgument;

	if (v.operation == N48N_VA_OP_UNMAP) {
		for (Map &m : maps)
			if (m.bytes && m.handle == v.handle && m.va == va && m.bytes == v.map_size) {
				(void)quiesce();
				unmapTable(m);
				m = Map {};
				be.flush(be.context);
				return kSuccess;
			}
		return kBadArgument;
	}

	const uint32_t known = N48N_VM_DELAY_UPDATE | N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE |
	                       N48N_VM_PAGE_EXECUTABLE | N48N_VM_MTYPE_MASK | N48N_VM_PAGE_NOALLOC;
	if ((v.flags & ~known) || ((v.flags & N48N_VM_MTYPE_MASK) >> 5) > 5)
		return kBadArgument;
	Map *slot = nullptr;
	for (Map &m : maps) {
		if (!m.bytes) {
			if (!slot)
				slot = &m;
		} else if (va < m.va + m.bytes && m.va < va + v.map_size) {
			return kBadArgument;   // over a mapping that is there
		}
	}
	if (!slot)
		return kNoMemory;

	const Map m { va, v.map_size, v.offset_in_bo, v.handle, v.flags };
	const uint64_t leaf = leafFlags(v.flags, b->uncached, b->memory.system);
	const uint64_t pages = v.map_size / kPage, first = v.offset_in_bo / kPage;
	bool ok = true;
	if (!b->memory.system) {
		ok = VmTree::map(be.tables, rootPage, va, b->memory.physical + v.offset_in_bo, pages, leaf);
	} else {
		for (uint64_t i = 0; ok && i < pages; i++)
			ok = VmTree::map(be.tables, rootPage, va + i * kPage, be.systemPage(be.context, b->memory, first + i), 1, leaf);
	}
	if (!ok) {
		unmapTable(m);
		be.flush(be.context);
		return kNoMemory;
	}
	*slot = m;
	be.flush(be.context);
	return kSuccess;
}

uint32_t Client::context(const n48n_ctx &in, n48n_ctx &out) {
	out = n48n_ctx {};
	if (in.op == N48N_CTX_OP_ALLOC) {
		uint32_t id = 1;
		while (id <= N48N_MAX_CTX && contexts[id])
			id++;
		if (id > N48N_MAX_CTX)
			return kNoResources;
		contexts[id] = true;
		out.op = id;   // the id comes back in the first dword
		return kSuccess;
	}
	if (in.op == N48N_CTX_OP_FREE) {
		if (in.ctx_id == 0 || in.ctx_id > N48N_MAX_CTX || !contexts[in.ctx_id])
			return kNotFound;
		contexts[in.ctx_id] = false;
		return kSuccess;
	}
	// Whether the GPU was reset under this context: it was not.
	return in.op == N48N_CTX_OP_QUERY_STATE2 ? kSuccess : kBadArgument;
}

void Client::info(n48n_info &out) {
	Pools pools {};
	be.pools(be.context, pools);
	retire();
	out = n48n_info {};
	out.flags = lost ? N48N_INFO_HUNG : 0;
	out.seq_emitted = emitted;
	out.seq_retired = retired;
	out.abi_version = N48N_ABI_VERSION;
	out.kext_build = build;
	out.reserved[2] = N48N_ABI_MINOR;
	out.vmid = vmid;
	out.gc_version = gcVersion;
	out.max_ibs = N48N_MAX_IBS;
	be.readRegs(be.context, 0x263e, 1, &out.gb_addr_config);
	out.vram_vis_total = pools.visibleTotal;
	out.vram_vis_free = pools.visibleFree;
	out.vram_hi_total = pools.highTotal;
	out.vram_hi_free = pools.highFree;
	out.gtt_cap = kSystemCap;
	out.gtt_used = systemUsed;
	out.gtt_max_bo = kSystemMaxBuffer;
	out.va_low_first = kVaFirst;
	out.va_low_last = 0x7fffffffffffull;
	out.va_high_first = 0xffff800000000000ull;
	out.va_high_last = ~0ull;
	out.max_bos = N48N_MAX_BOS;
	out.fence_slots = N48N_FENCE_SLOTS;
}

// Whether [va, va + bytes) lies in mappings the client made executable, end to end: a command buffer the card
// could not read would stop the queue for everyone.
bool Client::covered(uint64_t va, uint64_t bytes) const {
	for (uint64_t at = va, end = va + bytes; at < end;) {
		const Map *in = nullptr;
		for (const Map &m : maps)
			if (m.bytes && (m.flags & N48N_VM_PAGE_EXECUTABLE) && at >= m.va && at < m.va + m.bytes) {
				in = &m;
				break;
			}
		if (!in)
			return false;
		at = in->va + in->bytes;
	}
	return true;
}

// Take note of what the card has finished, and write the fences those submissions asked for.
void Client::retire() {
	if (retired == emitted)
		return;
	// The card reports 32 bits of a sequence; far fewer than 2^31 submissions are ever on the queue.
	const uint64_t now = emitted - static_cast<uint32_t>(static_cast<uint32_t>(emitted) - be.finished(be.context));
	if (now <= retired)
		return;
	retired = now;
	progressAt = be.now(be.context);
	uint32_t done = 0;
	for (; done < jobCount && jobs[done].sequence <= retired; done++)
		if (const Buffer *b = buffer(jobs[done].fenceHandle))
			be.store(be.context, b->memory, jobs[done].fenceOffset, jobs[done].sequence);
	for (uint32_t i = done; i < jobCount; i++)
		jobs[i - done] = jobs[i];
	jobCount -= done;
}

// Until the card has finished `sequence` or `timeoutNs` have passed: kSuccess either way, the caller compares.
// kTimeout for the call that finds the work lost, kAborted after that.
uint32_t Client::waitFor(uint64_t sequence, uint64_t timeoutNs) {
	const uint64_t start = be.now(be.context);
	for (;;) {
		retire();
		if (retired >= sequence)
			return kSuccess;
		if (lost)
			return kAborted;
		const uint64_t now = be.now(be.context);
		if (now - progressAt >= kLostAfterNs) {
			lost = true;
			jobCount = 0;          // their fences are never written
			be.lost(be.context);
			return kTimeout;
		}
		if (now - start >= timeoutNs)
			return kSuccess;
		be.pause(be.context);
	}
}

bool Client::quiesce() { return opened && waitFor(emitted, ~0ull) == kSuccess; }

uint32_t Client::submit(const void *structIn, size_t structInSize, uint64_t *out) {
	if (lost)
		return kAborted;
	if (structInSize < sizeof(n48n_cs_in))
		return kBadArgument;
	const n48n_cs_in h = read<n48n_cs_in>(structIn);
	if (h.abi != N48N_ABI_VERSION || h.num_ibs < 1 || h.num_ibs > N48N_MAX_IBS ||
	    structInSize != sizeof(h) + h.num_ibs * sizeof(n48n_cs_ib) || h.reserved || (h.flags & ~N48N_CS_HAS_FENCE))
		return kBadArgument;
	if (h.ctx_id == 0 || h.ctx_id > N48N_MAX_CTX || !contexts[h.ctx_id])
		return kNotFound;

	Ib ibs[N48N_MAX_IBS];
	for (uint32_t i = 0; i < h.num_ibs; i++) {
		const n48n_cs_ib ib =
			read<n48n_cs_ib>(static_cast<const uint8_t *>(structIn) + sizeof(h) + i * sizeof(n48n_cs_ib));
		if (ib._pad || ib.ip_type != N48N_HW_IP_GFX || ib.ip_instance || ib.ring)
			return kBadArgument;
		if (!ib.ib_bytes || (ib.ib_bytes & 3) || ib.ib_bytes > 0xfffffu * 4)   // the packet's length field is 20 bits of dwords
			return kBadArgument;
		if (!canonical(ib.va_start) || (ib.va_start & 3))
			return kBadArgument;
		if (ib.flags & (N48N_IB_FLAG_CE | N48N_IB_FLAG_RESET_GDS_MAX_WAVE_ID | N48N_IB_FLAG_SECURE | N48N_IB_FLAG_EMIT_MEM_SYNC))
			return kUnsupported;
		if (ib.flags & ~(N48N_IB_FLAG_PREAMBLE | N48N_IB_FLAG_PREEMPT | N48N_IB_FLAG_TC_WB_NOT_INVALIDATE))
			return kBadArgument;
		if (!covered(ib.va_start & kVaMask, ib.ib_bytes))
			return kBadArgument;
		ibs[i] = Ib { ib.va_start, ib.ib_bytes / 4 };
	}

	if (h.flags & N48N_CS_HAS_FENCE) {
		const Buffer *b = buffer(h.fence_handle);
		if (!b)
			return kNotFound;
		// The fence is written by the CPU when the card reports the work done (retire), so the CPU must reach it.
		if ((h.fence_offset & 7) || h.fence_offset + 8ull > b->bytes || !b->memory.cpuVisible)
			return kBadArgument;
	} else if (h.fence_handle || h.fence_offset) {
		return kBadArgument;
	}

	retire();
	uint32_t r = jobCount == N48N_FENCE_SLOTS ? waitFor(jobs[0].sequence, ~0ull) : kSuccess;
	if (r != kSuccess)
		return r;
	const uint32_t sequence = static_cast<uint32_t>(emitted + 1);
	r = be.submit(be.context, ibs, h.num_ibs, sequence);
	if (r == kBusy && (r = waitFor(emitted, ~0ull)) == kSuccess)
		r = be.submit(be.context, ibs, h.num_ibs, sequence);
	if (r != kSuccess)
		return r;
	if (retired == emitted)
		progressAt = be.now(be.context);    // the clock for "lost" starts when work goes onto an idle queue
	emitted++;
	jobs[jobCount++] = Job { emitted, (h.flags & N48N_CS_HAS_FENCE) ? h.fence_handle : 0, h.fence_offset };
	out[0] = emitted;
	return kSuccess;
}

// The plane back to what it showed before the client took it, and every registration dropped. True if there was
// nothing to give back or the display confirms it.
bool Client::scanGiveBack() {
	const bool was = scanAcquired;
	scanAcquired = false;
	scanFront = N48N_SCAN_NO_SLOT;
	for (Slot &slot : slots)
		slot = Slot {};
	return !was || be.scanRelease(be.context);
}

// Selectors 9 to 14, their shapes already checked. A buffer is shown by the display's own address for it, not
// through the client's page table: it has to be VRAM, and of exactly the shape the plane had when it was taken.
uint32_t Client::scan(uint32_t selector, const uint64_t *in, const void *structIn, uint64_t *out, void *structOut) {
	n48n_scan_query q {};
	const bool live = be.scanQuery(be.context, q);
	switch (selector) {
	case N48N_SEL_SCAN_QUERY:
		if (!live)
			return kNotReady;
		q.acquired = scanAcquired;
		q.flags |= scanAcquired ? N48N_SCANQ_ACQUIRED : 0;
		if (scanAcquired)
			q.console_mc = scanConsole;
		__builtin_memcpy(structOut, &q, sizeof(q));
		return kSuccess;
	case N48N_SEL_SCAN_ACQUIRE: {
		if (in[0] != 0)
			return kBadArgument;
		if (scanAcquired)
			return kBusy;
		if (!live || !(q.flags & N48N_SCANQ_LIT) || !(q.flags & N48N_SCANQ_GEOM_OK))
			return kNotReady;
		const uint32_t r = be.scanAcquire(be.context);
		if (r != kSuccess)
			return r;
		scanAcquired = true;
		scanWidth = q.plane_w;
		scanHeight = q.plane_h;
		scanPitchBytes = q.pitch_px * 4;
		scanConsole = q.plane_mc;
		out[0] = scanConsole;
		out[1] = q.frame_count;
		return kSuccess;
	}
	case N48N_SEL_SCAN_REGISTER: {
		if (!scanAcquired)
			return kNotReady;
		const n48n_scan_reg r = read<n48n_scan_reg>(structIn);
		if (r.reserved0 || r.format != N48N_SCAN_FMT_ARGB8888 || r.width != scanWidth || r.height != scanHeight ||
		    r.pitch_bytes != scanPitchBytes)
			return kBadArgument;
		const Buffer *b = buffer(r.handle);
		if (!b)
			return kNotFound;
		const uint64_t bytes = static_cast<uint64_t>(r.pitch_bytes) * r.height;
		if (!b->memory.scanout || r.offset > b->bytes || bytes > b->bytes - r.offset)
			return kBadArgument;
		const uint64_t address = b->memory.scanout + r.offset;
		if (address & (kPage - 1))
			return kBadArgument;
		Slot *slot = nullptr;
		for (Slot &s : slots) {
			if (s.used && s.address == address)
				return kBadArgument;
			if (!s.used && !slot)
				slot = &s;
		}
		if (!slot)
			return kNoResources;
		*slot = Slot { address, 0, r.handle, 0, true };
		out[0] = static_cast<uint64_t>(slot - slots);
		out[1] = address;
		return kSuccess;
	}
	case N48N_SEL_SCAN_PRESENT: {
		if (!scanAcquired)
			return kNotReady;
		if (in[1] != 0 || in[0] >= N48N_SCAN_MAX_SLOTS || !slots[in[0]].used) {
			scanRefused++;
			return kBadArgument;
		}
		Slot &slot = slots[in[0]];
		uint64_t frame = 0;
		const uint32_t r = be.scanShow(be.context, slot.address, scanWidth, scanHeight, scanPitchBytes, frame);
		if (r != kSuccess) {
			scanRefused++;
			return r;
		}
		scanFront = static_cast<uint32_t>(in[0]);
		slot.presents++;
		slot.latchedFrame = frame;
		out[0] = ++scanPresents;
		out[1] = frame;
		out[2] = 0;       // no count of vertical-blank interrupts is kept: the call returns when the picture is up
		return kSuccess;
	}
	case N48N_SEL_SCAN_STATUS: {
		n48n_scan_status st {};
		st.acquired = scanAcquired;
		st.front_slot = scanFront;
		st.pending_slot = N48N_SCAN_NO_SLOT;      // a present is on the display when its call returns
		st.frame_count = q.frame_count;
		st.console_mc = scanAcquired ? scanConsole : q.plane_mc;
		st.plane_mc = q.plane_mc;
		st.earliest_mc = q.earliest_mc;
		st.presents = st.latched = st.latch_poll = scanPresents;
		st.refused = scanRefused;
		for (uint32_t i = 0; i < N48N_SCAN_MAX_SLOTS; i++) {
			const Slot &slot = slots[i];
			st.slot[i].mc = slot.address;
			st.slot[i].latched_frame = slot.latchedFrame;
			st.slot[i].used = slot.used;
			st.slot[i].flags = !slot.used ? 0 : i == scanFront ? N48N_SCANSLOT_INUSE : N48N_SCANSLOT_REUSABLE;
			st.slot[i].presents = st.slot[i].latches = slot.presents;
		}
		__builtin_memcpy(structOut, &st, sizeof(st));
		return kSuccess;
	}
	default: {   // N48N_SEL_SCAN_RELEASE: also when nothing is held
		out[0] = scanGiveBack();
		n48n_scan_query after {};
		out[1] = be.scanQuery(be.context, after) ? after.plane_mc : 0;
		return kSuccess;
	}
	}
}

uint32_t Client::call(uint32_t selector, const uint64_t *in, uint32_t nIn, const void *structIn, size_t structInSize,
                      uint64_t *out, uint32_t *nOut, void *structOut, size_t *structOutSize) {
	const uint32_t outRoom = nOut ? *nOut : 0;
	const size_t structRoom = structOutSize ? *structOutSize : 0;
	// Counts and sizes are checked exactly, as the contract has it.
	auto shape = [&](uint32_t scalarsIn, uint32_t scalarsOut, size_t bytesIn, size_t bytesOut) {
		return nIn == scalarsIn && outRoom == scalarsOut && structInSize == bytesIn && structRoom == bytesOut &&
		       (!scalarsIn || in) && (!scalarsOut || out) && (!bytesIn || structIn) && (!bytesOut || structOut);
	};
	if (!opened || (selector != N48N_SEL_HELLO && !greeted))
		return kNotReady;

	switch (selector) {
	case N48N_SEL_HELLO:
		if (!shape(2, 4, 0, 0) || (in[1] != 0 && in[1] != N48N_HELLO_F_MINOR))
			return kBadArgument;
		if (in[0] != N48N_ABI_VERSION)
			return kUnsupported;
		greeted = true;
		out[0] = N48N_ABI_VERSION | (in[1] ? static_cast<uint64_t>(N48N_ABI_MINOR) << 16 : 0);
		out[1] = build;
		out[2] = N48N_MAX_IBS;
		out[3] = vmid;
		return kSuccess;
	case N48N_SEL_QUERYINFO:
		if (!shape(0, 0, 0, sizeof(n48n_info)))
			return kBadArgument;
		info(*static_cast<n48n_info *>(structOut));
		return kSuccess;
	case N48N_SEL_READREGS:
		// GB_ADDR_CONFIG and its three neighbours, nothing else of the card.
		if (nIn != 3 || !in || in[1] < 1 || in[1] > 16 || in[2] != 0xffffffffull || !shape(3, 0, 0, in[1] * 4) ||
		    in[0] < 0x263e || in[0] + in[1] - 1 > 0x2641)
			return kBadArgument;
		return be.readRegs(be.context, static_cast<uint32_t>(in[0]), static_cast<uint32_t>(in[1]),
		                   static_cast<uint32_t *>(structOut)) ? kSuccess : kNotReady;
	case N48N_SEL_BOCREATE:
		if (!shape(0, 4, sizeof(n48n_gem_create_in), 0))
			return kBadArgument;
		return create(read<n48n_gem_create_in>(structIn), out);
	case N48N_SEL_BOFREE:
		return shape(1, 0, 0, 0) ? release(static_cast<uint32_t>(in[0])) : kBadArgument;
	case N48N_SEL_GEMVA:
		return shape(0, 0, sizeof(n48n_gem_va), 0) ? mapping(read<n48n_gem_va>(structIn)) : kBadArgument;
	case N48N_SEL_CTX: {
		if (!shape(0, 0, sizeof(n48n_ctx), sizeof(n48n_ctx)))
			return kBadArgument;
		n48n_ctx reply {};
		const uint32_t r = context(read<n48n_ctx>(structIn), reply);
		if (r == kSuccess)
			__builtin_memcpy(structOut, &reply, sizeof(reply));
		return r;
	}
	case N48N_SEL_SUBMIT:
		return nIn == 0 && outRoom == 1 && out && structRoom == 0 && structIn ? submit(structIn, structInSize, out)
		                                                                    : kBadArgument;
	case N48N_SEL_WAITSEQ: {
		if (!shape(3, 3, 0, 0))
			return kBadArgument;
		const uint64_t target = in[0] == N48N_SEQ_LAST ? emitted : in[0];
		if (target > emitted)
			return kBadArgument;
		const uint32_t r = waitFor(target, in[1] < N48N_WAIT_CAP_NS ? in[1] : N48N_WAIT_CAP_NS);
		if (r != kSuccess)
			return r;
		out[0] = retired < target;   // still busy
		out[1] = retired;
		out[2] = emitted;
		return kSuccess;
	}
	case N48N_SEL_SCAN_QUERY:
		return shape(0, 0, 0, sizeof(n48n_scan_query)) ? scan(selector, in, structIn, out, structOut) : kBadArgument;
	case N48N_SEL_SCAN_ACQUIRE:
		return shape(1, 2, 0, 0) ? scan(selector, in, structIn, out, structOut) : kBadArgument;
	case N48N_SEL_SCAN_REGISTER:
		return shape(0, 2, sizeof(n48n_scan_reg), 0) ? scan(selector, in, structIn, out, structOut) : kBadArgument;
	case N48N_SEL_SCAN_PRESENT:
		return shape(2, 3, 0, 0) ? scan(selector, in, structIn, out, structOut) : kBadArgument;
	case N48N_SEL_SCAN_STATUS:
		return shape(0, 0, 0, sizeof(n48n_scan_status)) ? scan(selector, in, structIn, out, structOut) : kBadArgument;
	case N48N_SEL_SCAN_RELEASE:
		return shape(0, 2, 0, 0) ? scan(selector, in, structIn, out, structOut) : kBadArgument;
	default:
		// The rest of the interface (clock and mode experiments, importing memory) is not here.
		return kUnsupported;
	}
}

} // namespace N48N
