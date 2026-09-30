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
	OpContextControl = 0x28,
	OpDispatchDirect = 0x15,
	OpCopyData       = 0x40,
	OpWriteData      = 0x37,
	OpEventWrite     = 0x46,
	OpReleaseMem     = 0x49,
	OpAcquireMem     = 0x58,
	OpIndirectBuffer = 0x3f,
	OpSetContextReg  = 0x69,
	OpSetShReg       = 0x76,
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
constexpr uint32_t kReleaseIntSel2      = 2u << 24; // INT_SEL(2): interrupt after write

constexpr uint32_t kShStart             = 0x2c00;   // PACKET3_SET_SH_REG_START

// COMPUTE_DISPATCH_INITIATOR
constexpr uint32_t kDispatchShaderEn    = 1u << 0;
constexpr uint32_t kDispatchForceStart0 = 1u << 2;  // FORCE_START_AT_000
constexpr uint32_t kDispatchWave32      = 1u << 15; // CS_W32_EN

// gfx_v12_0_emit_mem_sync's GCR_CNTL: invalidate/write back GL2, GLM, GL1,
// GLV, GLK and GLI, so what the CPU just wrote (e.g. shader code) is fetched.
constexpr uint32_t kGcrMemSync = (1u << 0) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8) |
                                 (1u << 9) | (1u << 14) | (1u << 15);

// Build into `out`; return the dwords written.
// `reg` is the absolute register dword offset (UCONFIG space, >= 0xc000).
uint32_t setUconfigReg(uint32_t *out, uint32_t reg, uint32_t value);
uint32_t writeData(uint32_t *out, uint64_t addr, uint32_t value);
// SET_CONTEXT_REG of one register: `offset` is the register's dword offset in the context space (reg - 0xa000).
uint32_t setContextReg(uint32_t *out, uint32_t offset, uint32_t value);
// A NOP packet with one payload dword (header + 1).
uint32_t nop(uint32_t *out);
// EVENT_WRITE with no address (PIPELINESTAT_START): header C0004600-style, one payload dword = `eventDw`.
uint32_t eventWrite(uint32_t *out, uint32_t eventDw);
// EVENT_WRITE that writes to memory (SAMPLE_PIPELINESTAT: si_query.c:854-857): `eventDw` = event type | index << 8, then the address.
uint32_t eventWriteAddr(uint32_t *out, uint32_t eventDw, uint64_t addr);
// SET_CONTEXT_REG of `count` consecutive registers from context offset `offset` (reg - 0xa000).
uint32_t setContextRegs(uint32_t *out, uint32_t offset, const uint32_t *values, uint32_t count);
// gfx_v12_0_ring_emit_rreg (gfx_v12_0.c:4697-4706): COPY_DATA of one register (src_sel 0, the MMIO
// dword offset the CP reads it at) to memory (dst_sel 5) with write confirm. The CP executes it in
// order on the ring, so it reports the state the CP itself holds at that point of the stream.
uint32_t copyDataRegToMem(uint32_t *out, uint32_t regDword, uint64_t addr);
uint32_t releaseMem(uint32_t *out, uint64_t addr, uint32_t seq, bool interrupt = false);
// `n` consecutive SH registers from absolute dword `reg` (0x2c00..0x2fff).
uint32_t setShReg(uint32_t *out, uint32_t reg, const uint32_t *values, uint32_t n);
uint32_t dispatchDirect(uint32_t *out, uint32_t x, uint32_t y, uint32_t z, uint32_t initiator);
uint32_t acquireMem(uint32_t *out, uint32_t gcrCntl);
// CONTEXT_CONTROL (gfx_v12_0_ring_emit_cntxcntl): `load` / `shadow` are the two operand dwords. W12k emits 0x80000000, 0x80000000
// (UPDATE_LOAD_ENABLES / UPDATE_SHADOW_ENABLES set, nothing loaded or shadowed): what gfx12_draw.h's phase 0 sets too.
uint32_t contextControl(uint32_t *out, uint32_t load, uint32_t shadow);
// A gfx-ring INDIRECT_BUFFER (gfx_v12_0_ring_emit_ib_gfx, no VALID bit; the
// compute form sets it): `dwords` at `addr` (dword aligned) under `vmid`.
uint32_t indirectBufferGfx(uint32_t *out, uint64_t addr, uint32_t dwords, uint32_t vmid);
// gfx_v12_0_ring_emit_ib_compute: VALID, length and VMID, with CHAIN and PRIV
// clear. Compute IBs are unprivileged and execute in the client's VMID.
uint32_t indirectBufferCompute(uint32_t *out, uint64_t addr, uint32_t dwords, uint32_t vmid);

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
