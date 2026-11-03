/*
 * pvrun.hpp
 * AMDRadeonRDNA4
 *
 * Runs what an ExecIndirect asks for: the command buffer the Metal bundle's
 * encoders wrote into the client's memory (docs/m1-stream.md s.4). It is a
 * list of segments {u32 length, u8 type, u8 continuation, u8 flags, u8 0},
 * the length counting the header; inside a segment, commands {u32 id,
 * u32 size} with the size counting their own header (measured, card).
 *
 * The first executor is the CPU: blit fills and buffer copies, done on the
 * client's memory through its page table. The rest is counted and logged.
 */

#ifndef PvRun_hpp
#define PvRun_hpp

#include <stdint.h>

namespace PvRun
{

/* Segment types (beginSegment in the encoders). */
enum Segment : uint8_t {
	kRender = 0,
	kCompute = 1,
	kBlit = 2,
	kInfo = 4,
};

/* Blit commands (docs/m1-opcodes.md). */
constexpr uint32_t kCopyBufferToBuffer = 0x12d;
constexpr uint32_t kFillBuffer = 0x132;
constexpr uint32_t kFillBufferPattern = 0x13f;

#ifdef KERNEL
/* Run the command buffer of `len` bytes at `va` in `task`. */
void run(uint32_t task, uint64_t va, uint64_t len);
#endif

} /* namespace PvRun */

#endif /* PvRun_hpp */
