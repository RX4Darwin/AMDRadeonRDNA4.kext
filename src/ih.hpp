//
//  ih.hpp
//  RDNA4FB
//
//  Freestanding helpers for the OSSSYS IH v7 ring.  The kext-facing setup
//  and interrupt delivery live in ih.cpp; these pieces are also used by the
//  host tests without pulling in IOKit.
//

#ifndef Rdna4Ih_hpp
#define Rdna4Ih_hpp

#include <stdint.h>

namespace Ih {

constexpr uint32_t kEntryDwords = 8;
constexpr uint32_t kEntryBytes = kEntryDwords * 4;

// gfx_v12_0_eop_irq packs ring_id as queue [6:4], ME [3:2], pipe [1:0].
// EOP is a CP source from MEC1 on every pipe and queue; ring 4 is only the
// first queue used by the original self-test.
constexpr bool isMec1Ring(uint8_t ringId) {
	return ((ringId & 0x0cu) >> 2) == 1;
}

struct Entry {
	uint8_t  clientId;
	uint8_t  srcId;
	uint8_t  ringId;
	uint8_t  vmid;
	bool     vmidSrc;
	uint64_t timestamp;
	uint16_t pasid;
	uint8_t  vmidSrcNode;
	uint32_t srcData[4];
};

// Decode one IH 7.0 eight-dword entry, matching amdgpu_ih_decode_iv_helper.
void decode(const uint32_t dw[kEntryDwords], Entry &out);

constexpr uint32_t advance(uint32_t ptr, uint32_t bytes, uint32_t ringBytes) {
	return (ptr + bytes) & (ringBytes - 1);
}

constexpr bool hasEntries(uint32_t rptr, uint32_t wptr, uint32_t ringBytes) {
	return (rptr & (ringBytes - 1)) != (wptr & (ringBytes - 1));
}

// A completed fence is a missed interrupt only after the waiter slept and a
// bounded recheck still finds no source vector five milliseconds later.
bool missEligible(bool slept, bool completed, bool sourceAdvanced, bool recheckElapsed);

// On overflow amdgpu starts at the entry after the vector that was
// overwritten by the producer's write pointer.
constexpr uint32_t overflowRecovery(uint32_t wptr, uint32_t ringBytes) {
	return advance(wptr, kEntryBytes, ringBytes);
}

} // namespace Ih

#endif /* Rdna4Ih_hpp */
