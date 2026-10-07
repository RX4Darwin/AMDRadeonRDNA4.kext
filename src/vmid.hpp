//
//  vmid.hpp
//  RDNA4FB
//
//  W13 (docs/w13-vmid.md): the VMID pool. A freestanding, hardware-free policy
//  object like gpuvm.cpp, so the host test exercises the exact rules the kext
//  will use. It decides WHICH hardware VMID a client's next submission uses and
//  whether that needs a rebind (page-directory base rewrite) and/or a TLB
//  flush; the caller performs them.
//
//  The rules are amdgpu's (amdgpu_ids.c):
//    - reuse: a VMID already owned by this client with the same page directory
//      is reused as is (amdgpu_vmid_grab_used); it needs a flush only when the
//      client removed PTEs since the VMID was last flushed (tlb_seq,
//      flushed_updates);
//    - else the least recently used IDLE VMID is taken and rebound
//      (amdgpu_vmid_grab_idle + job->vm_needs_flush);
//    - a VMID with work in flight is never taken from its owner;
//    - when none is idle the caller gets Busy plus the fence to wait on, and
//      while that fence is unsignalled every request that needs a new VMID is
//      refused too (amdgpu's ring->vmid_wait: "let everybody wait for fairness").
//
//  Deviation from amdgpu, on purpose: amdgpu looks for an idle VMID before it
//  considers reuse, so a client whose own VMID is bound and busy still waits
//  when nothing else is idle. This kext submits synchronously under a lock and
//  has no scheduler to park the job in, so reuse is tried first.
//
//  "In flight" is tracked per fence domain (the gfx ring, each shared compute
//  queue): the pool records the last sequence number submitted on each domain
//  for each VMID and asks the caller's callback whether it was reached.
//

#ifndef RDNA4Vmid_hpp
#define RDNA4Vmid_hpp

#include <stdint.h>

namespace Vmid {

constexpr uint32_t kFirst = 1;            // VMID 0 is the kernel's physical context
constexpr uint32_t kLast = 15;
constexpr uint32_t kSlots = kLast + 1;    // indexed by VMID; slot 0 is never granted
constexpr uint32_t kMaxDomains = 4;       // gfx ring + shared compute queues
constexpr uint32_t kDomainGfx = 2;        // the gfx ring (W12k client IBs); domains 0 and 1 are the two shared compute queues
static_assert(kDomainGfx < kMaxDomains, "the gfx ring is a fence domain of the pool");

// Has `seq` been reached on fence domain `domain`? (wrap-aware compare is the caller's)
using FenceReached = bool (*)(void *context, uint32_t domain, uint32_t seq);

enum class Result {
	Ok,          // out.vmid is valid
	Busy,        // every usable VMID has work in flight (or someone is already waiting); wait on out.waitDomain/out.waitSeq
	Exhausted,   // pin() only: nothing free and idle to pin
};

struct Grant {
	uint32_t vmid { 0 };
	bool rebind { false };     // the caller must write this client's page directory into the VMID's context (and IH LUT)
	bool flush { false };      // the caller must invalidate the VMID's TLB (always true with rebind)
	bool stolen { false };     // rebind took the VMID from another owner (for logs)
	uintptr_t prevOwner { 0 }; // who owned the VMID before a rebind (0 = nobody): a fault latched for it belongs to that owner
	uint32_t waitDomain { 0 }; // Busy: the fence to wait on
	uint32_t waitSeq { 0 };
};

struct Stats {
	uint32_t reuses, binds, steals, flushes, busy, unbinds;
};

class Pool {
public:
	// `reservedMask`: bit n set = VMID n is never granted (kernel self-tests, VMID 0).
	void init(FenceReached reached, void *context, uint32_t reservedMask = 0);

	// The submission path: the VMID `owner` should use for a job on its page directory `pd`
	// (any value that identifies the tables, e.g. the root's physical address) whose TLB
	// sequence is `tlbSeq` (bumped by the client each time it removes PTEs).
	// `owner` must be non-zero.
	Result grab(uintptr_t owner, uint64_t pd, uint64_t tlbSeq, Grant &out);

	// A job was queued on `vmid` and will signal `seq` on `domain`.
	void noteSubmit(uint32_t vmid, uint32_t domain, uint32_t seq);

	// Dedicated binding (static VMIDs, step S7): take an idle VMID for `owner` for good. A pinned
	// VMID is never stolen; release it with forget(). Same Grant as grab (rebind is true).
	Result pin(uintptr_t owner, uint64_t pd, uint64_t tlbSeq, Grant &out);

	// Client closed. Releases every VMID it owns and returns the first in `vmid` (0 if none)
	// so the caller can clear the context and invalidate. Refuses (false) while any of them has
	// work in flight, unless `force` (after a hang recovery has killed the work).
	bool forget(uintptr_t owner, bool force, uint32_t &vmid);

	// After a wake or a GPU reset every context is gone: nothing is bound any more, pins included.
	void unbindAll();

	// Inspection (tests, logs).
	bool idle(uint32_t vmid);
	uintptr_t ownerOf(uint32_t vmid) const { return vmid < kSlots ? slot[vmid].owner : 0; }
	bool pinned(uint32_t vmid) const { return vmid < kSlots && slot[vmid].pinned; }
	const Stats &stats() const { return counters; }

private:
	struct Slot {
		uintptr_t owner;           // 0 = unbound
		uint64_t pd;
		uint64_t boundSeq;         // client tlbSeq the last flush covered
		uint64_t lastUse;
		bool pinned;
		bool pending[kMaxDomains];
		uint32_t pendingSeq[kMaxDomains];
	};
	Slot slot[kSlots] {};
	uint32_t reserved { 1 };
	FenceReached reached { nullptr };
	void *ctx { nullptr };
	uint64_t tick { 0 };
	bool starving { false };       // a request got Busy and its fence is not reached yet
	uint32_t starveDomain { 0 }, starveSeq { 0 };
	Stats counters {};

	bool usable(uint32_t v) const { return v >= kFirst && v <= kLast && !(reserved & (1u << v)); }
	bool firstPending(uint32_t v, uint32_t &domain, uint32_t &seq);
	bool starved(Grant &out);
	void bind(uint32_t v, uintptr_t owner, uint64_t pd, uint64_t tlbSeq, Grant &out);
	void dropOtherSlotsOf(uintptr_t owner, uint32_t keep);
	Result takeNew(uintptr_t owner, uint64_t pd, uint64_t tlbSeq, bool pinIt, Grant &out);
};

} // namespace Vmid

#endif /* RDNA4Vmid_hpp */
