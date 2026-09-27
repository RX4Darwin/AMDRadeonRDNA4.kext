//
//  sdma.hpp
//  RDNA4FB
//
//  SDMA 7 (RDNA 4 copy engine) command packets and a ring writer. The
//  encodings follow the SDMA packet layout amdgpu's sdma_v7_0 uses
//  (sdma_v6_0_0_pkt_open.h): an op/sub-op header dword and the packet body.
//
//  Freestanding: the ring is a caller-provided CPU window of VRAM; the
//  kernel posts the write pointer, the host test decodes the dwords.
//

#ifndef Sdma_hpp
#define Sdma_hpp

#include <stddef.h>
#include <stdint.h>

namespace Sdma {

enum Op : uint32_t {
	OpNop       = 0,
	OpCopy      = 1,
	OpWrite     = 2,
	OpFence     = 5,
	OpTrap      = 6,
	OpConstFill = 11,
};

constexpr uint32_t kSubLinear = 0;

// Header fields.
constexpr uint32_t header(uint32_t op, uint32_t sub = 0) { return (op & 0xff) | ((sub & 0xff) << 8); }
constexpr uint32_t kFenceMtypeUc    = 3u << 16;   // FENCE: uncached write
constexpr uint32_t kFillSizeDword   = 2u << 30;   // CONST_FILL: 4-byte pattern
constexpr uint32_t kCopyCpv         = 1u << 28;   // COPY_LINEAR: CPV, set by sdma_v7_0

// Packet sizes in dwords.
constexpr uint32_t kWriteDwords = 5;   // header, addr lo/hi, count, 1 data dword
constexpr uint32_t kFenceDwords = 4;   // header, addr lo/hi, value
constexpr uint32_t kFillDwords  = 5;   // header, addr lo/hi, pattern, bytes - 1
constexpr uint32_t kCopyDwords  = 8;   // header, count, param, src lo/hi, dst lo/hi, DCC

// Build into `out`; return the dwords written.
uint32_t writeDword(uint32_t *out, uint64_t addr, uint32_t value);
uint32_t fence(uint32_t *out, uint64_t addr, uint32_t value);
uint32_t constFill(uint32_t *out, uint64_t addr, uint32_t pattern, uint32_t bytes);
uint32_t copyLinear(uint32_t *out, uint64_t src, uint64_t dst, uint32_t bytes);

// A ring of `sizeBytes` (power of two) at `cpu` (the GPU sees it at `mc`).
// The write pointer counts bytes, as SDMA0_QUEUE0_RB_WPTR does.
class Ring {
public:
	bool init(volatile uint32_t *cpu, uint64_t mc, uint32_t sizeBytes);
	uint64_t mc() const { return base; }
	uint32_t sizeBytes() const { return size; }
	uint32_t sizeLog2Dwords() const;          // RB_SIZE field
	uint32_t wptr() const { return wp; }      // bytes

	// Append a packet (dwords); wraps around the end of the ring.
	bool emit(const uint32_t *dw, uint32_t count);

private:
	volatile uint32_t *ring { nullptr };
	uint64_t base { 0 };
	uint32_t size { 0 };
	uint32_t wp { 0 };
};

} // namespace Sdma

#endif /* Sdma_hpp */
