//
//  pm4.cpp
//  RDNA4FB
//
//  See pm4.hpp.
//

#include "pm4.hpp"

namespace Pm4 {

uint32_t setUconfigReg(uint32_t *out, uint32_t reg, uint32_t value) {
	out[0] = header(OpSetUconfigReg, 1);
	out[1] = reg - kUconfigStart;
	out[2] = value;
	return 3;
}

uint32_t setContextReg(uint32_t *out, uint32_t offset, uint32_t value) {
	out[0] = header(OpSetContextReg, 1);
	out[1] = offset;
	out[2] = value;
	return 3;
}

uint32_t nop(uint32_t *out) {
	out[0] = header(OpNop, 0);
	out[1] = 0;
	return 2;
}

uint32_t writeData(uint32_t *out, uint64_t addr, uint32_t value) {
	out[0] = header(OpWriteData, 3);
	out[1] = kWriteDstMemory | kWriteConfirm;
	out[2] = static_cast<uint32_t>(addr) & ~3u;
	out[3] = static_cast<uint32_t>(addr >> 32);
	out[4] = value;
	return 5;
}

uint32_t copyDataRegToMem(uint32_t *out, uint32_t regDword, uint64_t addr) {
	out[0] = header(OpCopyData, 4);
	out[1] = (0u << 0) | (5u << 8) | kWriteConfirm;   // src: register, dst: memory, wait for the write
	out[2] = regDword;
	out[3] = 0;
	out[4] = static_cast<uint32_t>(addr) & ~3u;
	out[5] = static_cast<uint32_t>(addr >> 32);
	return 6;
}

// gfx_v12_0_ring_emit_fence: end-of-pipe event with GL2 write-back, then
// the sequence number to `addr`, no interrupt.
uint32_t releaseMem(uint32_t *out, uint64_t addr, uint32_t seq, bool interrupt) {
	out[0] = header(OpReleaseMem, 6);
	out[1] = kReleaseGcrSeq | kReleaseGcrGl2Wb | kReleaseCachePolicy3 | kEventCacheFlushTs |
	         kReleaseEventIndex5;
	out[2] = kReleaseData32 | (interrupt ? kReleaseIntSel2 : 0);
	out[3] = static_cast<uint32_t>(addr) & ~3u;
	out[4] = static_cast<uint32_t>(addr >> 32);
	out[5] = seq;
	out[6] = 0;
	out[7] = 0;
	return 8;
}

uint32_t setShReg(uint32_t *out, uint32_t reg, const uint32_t *values, uint32_t n) {
	out[0] = header(OpSetShReg, n);
	out[1] = reg - kShStart;
	for (uint32_t i = 0; i < n; i++)
		out[2 + i] = values[i];
	return 2 + n;
}

uint32_t dispatchDirect(uint32_t *out, uint32_t x, uint32_t y, uint32_t z, uint32_t initiator) {
	out[0] = header(OpDispatchDirect, 3);
	out[1] = x;
	out[2] = y;
	out[3] = z;
	out[4] = initiator;
	return 5;
}

// gfx_v12_0_emit_mem_sync: whole address range, poll interval 10.
uint32_t acquireMem(uint32_t *out, uint32_t gcrCntl) {
	out[0] = header(OpAcquireMem, 6);
	out[1] = 0;            // CP_COHER_CNTL
	out[2] = 0xffffffff;   // CP_COHER_SIZE
	out[3] = 0x00ffffff;   // CP_COHER_SIZE_HI
	out[4] = 0;            // CP_COHER_BASE
	out[5] = 0;            // CP_COHER_BASE_HI
	out[6] = 0x0000000a;   // POLL_INTERVAL
	out[7] = gcrCntl;
	return 8;
}

uint32_t indirectBufferGfx(uint32_t *out, uint64_t addr, uint32_t dwords, uint32_t vmid) {
	out[0] = header(OpIndirectBuffer, 2);
	out[1] = static_cast<uint32_t>(addr) & ~3u;
	out[2] = static_cast<uint32_t>(addr >> 32);
	out[3] = (dwords & 0xfffff) | ((vmid & 0xf) << 24);
	return 4;
}

uint32_t indirectBufferCompute(uint32_t *out, uint64_t addr, uint32_t dwords, uint32_t vmid) {
	out[0] = header(OpIndirectBuffer, 2);
	out[1] = static_cast<uint32_t>(addr) & ~3u;
	out[2] = static_cast<uint32_t>(addr >> 32);
	/* INDIRECT_BUFFER_VALID is bit 23; CHAIN, OFFLOAD_POLLING and PRIV stay 0. */
	out[3] = (dwords & 0xfffff) | (1u << 23) | ((vmid & 0xf) << 24);
	return 4;
}

bool Queue::init(volatile uint32_t *cpu, uint64_t mc, uint32_t sizeBytes) {
	if (!cpu || sizeBytes < 256 || (sizeBytes & (sizeBytes - 1)) || (mc & 0xff))
		return false;
	q = cpu;
	base = mc;
	size = sizeBytes / 4;
	wp = 0;
	for (uint32_t i = 0; i < size; i++)
		q[i] = header(OpNop, 0x3fff);        // NOP filler, as amdgpu clears rings
	return true;
}

uint32_t Queue::queueSizeField() const {
	uint32_t n = 0;
	for (uint32_t v = size; v > 1; v >>= 1)
		n++;
	return n - 1;                            // 2^(QUEUE_SIZE+1) dwords
}

bool Queue::emit(const uint32_t *dw, uint32_t count) {
	if (!q || count >= size)
		return false;
	for (uint32_t i = 0; i < count; i++) {
		q[wp % size] = dw[i];
		wp++;
	}
	return true;
}

} // namespace Pm4
