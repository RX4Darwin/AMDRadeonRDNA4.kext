//
//  pm4.hpp
//  RDNA4FB
//
//  PM4 type-3 packets for the GFX12 command processor (MEC compute queues),
//  as amdgpu's gfx_v12_0 emits them (nvd.h encodings), and a queue writer
//  whose write pointer counts dwords, as CP_HQD_PQ_WPTR and the compute
//  doorbell do.
//
//  Freestanding: the queue is a caller-provided CPU window of VRAM.
//

#ifndef Pm4_hpp
#define Pm4_hpp

#include <stddef.h>
#include <stdint.h>

namespace Pm4 {

enum Op : uint32_t {
	OpNop            = 0x10,
	OpWriteData      = 0x37,
	OpReleaseMem     = 0x49,
	OpSetUconfigReg  = 0x79,
};

// Type-3 header: `count` = body dwords - 1.
constexpr uint32_t header(uint32_t op, uint32_t count) {
	return (3u << 30) | ((op & 0xff) << 8) | ((count & 0x3fff) << 16);
}

constexpr uint32_t kUconfigStart        = 0xc000;   // PACKET3_SET_UCONFIG_REG_START
constexpr uint32_t kWriteDstMemory      = 5u << 8;  // WRITE_DATA_DST_SEL(5)
constexpr uint32_t kWriteConfirm        = 1u << 20;
constexpr uint32_t kEventCacheFlushTs   = 0x14;     // CACHE_FLUSH_AND_INV_TS_EVENT
constexpr uint32_t kReleaseGcrGl2Wb     = 1u << 21;
constexpr uint32_t kReleaseGcrSeq       = 1u << 22;
constexpr uint32_t kReleaseCachePolicy3 = 3u << 25;
constexpr uint32_t kReleaseEventIndex5  = 5u << 8;
constexpr uint32_t kReleaseData32       = 1u << 29; // DATA_SEL(1): low 32 bits of seq
constexpr uint32_t kReleaseData64       = 2u << 29;

// Build into `out`; return the dwords written.
// `reg` is the absolute register dword offset (UCONFIG space, >= 0xc000).
uint32_t setUconfigReg(uint32_t *out, uint32_t reg, uint32_t value);
uint32_t writeData(uint32_t *out, uint64_t addr, uint32_t value);
uint32_t releaseMem(uint32_t *out, uint64_t addr, uint32_t seq);

// A queue of `sizeBytes` (power of two) at `cpu`; write pointer in dwords.
class Queue {
public:
	bool init(volatile uint32_t *cpu, uint64_t mc, uint32_t sizeBytes);
	uint64_t mc() const { return base; }
	uint32_t sizeDwords() const { return size; }
	uint32_t queueSizeField() const;         // CP_HQD_PQ_CONTROL.QUEUE_SIZE
	uint64_t wptr() const { return wp; }     // dwords, never wraps (64-bit)

	bool emit(const uint32_t *dw, uint32_t count);

private:
	volatile uint32_t *q { nullptr };
	uint64_t base { 0 };
	uint32_t size { 0 };                     // dwords
	uint64_t wp { 0 };
};

} // namespace Pm4

#endif /* Pm4_hpp */
