//
//  vmid.cpp
//  RDNA4FB
//
//  See vmid.hpp.
//

#include "vmid.hpp"

namespace Vmid {

void Pool::init(FenceReached cb, void *context, uint32_t reservedMask) {
	for (Slot &s : slot)
		s = Slot {};
	reached = cb;
	ctx = context;
	reserved = reservedMask | 1u;      // VMID 0 is never a client's
	tick = 0;
	starving = false;
	counters = Stats {};
}

bool Pool::idle(uint32_t v) {
	if (v >= kSlots)
		return true;
	Slot &s = slot[v];
	for (uint32_t d = 0; d < kMaxDomains; d++) {
		if (!s.pending[d])
			continue;
		if (!reached || !reached(ctx, d, s.pendingSeq[d]))
			return false;
		s.pending[d] = false;
	}
	return true;
}

bool Pool::firstPending(uint32_t v, uint32_t &domain, uint32_t &seq) {
	Slot &s = slot[v];
	for (uint32_t d = 0; d < kMaxDomains; d++) {
		if (s.pending[d] && !(reached && reached(ctx, d, s.pendingSeq[d]))) {
			domain = d;
			seq = s.pendingSeq[d];
			return true;
		}
	}
	return false;
}

// amdgpu's ring->vmid_wait: once somebody is waiting for a VMID, everybody who needs a new
// one waits for the same fence, so a stream of newcomers cannot starve the waiter.
bool Pool::starved(Grant &out) {
	if (!starving)
		return false;
	if (reached && reached(ctx, starveDomain, starveSeq)) {
		starving = false;
		return false;
	}
	out.waitDomain = starveDomain;
	out.waitSeq = starveSeq;
	counters.busy++;
	return true;
}

void Pool::dropOtherSlotsOf(uintptr_t owner, uint32_t keep) {
	for (uint32_t v = kFirst; v <= kLast; v++) {
		Slot &s = slot[v];
		if (v != keep && s.owner == owner && !s.pinned && idle(v)) {
			s.owner = 0;
			s.pd = 0;
			counters.unbinds++;
		}
	}
}

void Pool::bind(uint32_t v, uintptr_t owner, uint64_t pd, uint64_t tlbSeq, Grant &out) {
	Slot &s = slot[v];
	out.stolen = s.owner != 0 && s.owner != owner;
	out.prevOwner = s.owner != owner ? s.owner : 0;
	s.owner = owner;
	s.pd = pd;
	s.boundSeq = tlbSeq;
	s.lastUse = ++tick;
	for (uint32_t d = 0; d < kMaxDomains; d++)
		s.pending[d] = false;           // idle by construction
	out.vmid = v;
	out.rebind = true;
	out.flush = true;
	counters.binds++;
	counters.flushes++;
	if (out.stolen)
		counters.steals++;
	dropOtherSlotsOf(owner, v);
}

Result Pool::takeNew(uintptr_t owner, uint64_t pd, uint64_t tlbSeq, bool pinIt, Grant &out) {
	// Candidates are the usable, unpinned VMIDs. Unbound ones first (least recently used), then
	// bound-but-idle ones (least recently used); a VMID with work in flight is never taken.
	uint32_t bestFree = 0, bestIdle = 0, oldestBusy = 0;
	bool any = false;
	for (uint32_t v = kFirst; v <= kLast; v++) {
		if (!usable(v) || slot[v].pinned)
			continue;
		any = true;
		Slot &s = slot[v];
		if (!s.owner) {
			if (!bestFree || s.lastUse < slot[bestFree].lastUse)
				bestFree = v;
		} else if (idle(v)) {
			if (!bestIdle || s.lastUse < slot[bestIdle].lastUse)
				bestIdle = v;
		} else if (!oldestBusy || s.lastUse < slot[oldestBusy].lastUse) {
			oldestBusy = v;
		}
	}
	const uint32_t pick = bestFree ? bestFree : bestIdle;
	if (pick) {
		bind(pick, owner, pd, tlbSeq, out);
		slot[pick].pinned = pinIt;
		return Result::Ok;
	}
	if (!any || !oldestBusy)
		return Result::Exhausted;       // everything is pinned or reserved
	uint32_t d = 0, q = 0;
	if (!firstPending(oldestBusy, d, q)) {
		// It finished between the two looks: take it.
		bind(oldestBusy, owner, pd, tlbSeq, out);
		slot[oldestBusy].pinned = pinIt;
		return Result::Ok;
	}
	starving = true;
	starveDomain = out.waitDomain = d;
	starveSeq = out.waitSeq = q;
	counters.busy++;
	return Result::Busy;
}

Result Pool::grab(uintptr_t owner, uint64_t pd, uint64_t tlbSeq, Grant &out) {
	out = Grant {};
	if (!owner)
		return Result::Exhausted;
	// Reuse first (see the header: the deviation from amdgpu). Flushing a VMID that has work in
	// flight is allowed: an invalidation only drops cached translations.
	for (uint32_t v = kFirst; v <= kLast; v++) {
		Slot &s = slot[v];
		if (!usable(v) || s.owner != owner || s.pd != pd)
			continue;
		out.vmid = v;
		out.flush = s.boundSeq < tlbSeq;
		if (out.flush) {
			s.boundSeq = tlbSeq;
			counters.flushes++;
		}
		s.lastUse = ++tick;
		counters.reuses++;
		return Result::Ok;
	}
	if (starved(out))
		return Result::Busy;
	return takeNew(owner, pd, tlbSeq, false, out);
}

Result Pool::pin(uintptr_t owner, uint64_t pd, uint64_t tlbSeq, Grant &out) {
	out = Grant {};
	if (!owner)
		return Result::Exhausted;
	if (starved(out))
		return Result::Busy;
	return takeNew(owner, pd, tlbSeq, true, out);
}

void Pool::noteSubmit(uint32_t vmid, uint32_t domain, uint32_t seq) {
	if (vmid < kFirst || vmid > kLast || domain >= kMaxDomains)
		return;
	Slot &s = slot[vmid];
	s.pending[domain] = true;
	s.pendingSeq[domain] = seq;
	s.lastUse = ++tick;
}

bool Pool::forget(uintptr_t owner, bool force, uint32_t &vmid) {
	vmid = 0;
	if (!owner)
		return true;
	if (!force) {
		for (uint32_t v = kFirst; v <= kLast; v++)
			if (slot[v].owner == owner && !idle(v))
				return false;
	}
	for (uint32_t v = kFirst; v <= kLast; v++) {
		Slot &s = slot[v];
		if (s.owner != owner)
			continue;
		if (!vmid)
			vmid = v;
		s = Slot {};
		counters.unbinds++;
	}
	return true;
}

void Pool::unbindAll() {
	for (uint32_t v = kFirst; v <= kLast; v++)
		slot[v] = Slot {};
	starving = false;
}

} // namespace Vmid
