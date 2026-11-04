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
 * Blit fills and buffer copies run on the CPU, on the client's memory
 * through its page table. A compute dispatch goes to the compiler service
 * (pvjob.hpp), which runs it on the card. The rest is counted and logged.
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

/* Render commands (docs/m1-opcodes.md): the pass's attachments, the pipeline, buffers and draws. */
constexpr uint32_t kRenderPass = 0x1a;
constexpr uint32_t kSetRenderPipeline = 0x74;
constexpr uint32_t kSetVertexBuffers = 0x7d;
constexpr uint32_t kSetFragmentBuffers = 0x6e;
constexpr uint32_t kSetFragmentSamplers = 0x70;
constexpr uint32_t kSetFragmentTextures = 0x72;
constexpr uint32_t kSetScissor = 0x75;
constexpr uint32_t kSetViewport = 0x82;
constexpr uint32_t kSetVertexBufferOffset = 0x7e;
constexpr uint32_t kSetFragmentBufferOffset = 0x6f;
constexpr uint32_t kSetCullMode = 0x6b;
constexpr uint32_t kSetFrontFacing = 0x73;
constexpr uint32_t kSetFillMode = 0x7c;
constexpr uint32_t kSetState17 = 0x17;    /* a u32 (WindowServer: 0x02020004), not learnt yet */
constexpr uint32_t kDraw = 0x000;
constexpr uint32_t kDrawShort = 0x001;
constexpr uint32_t kDrawInstanced = 0x002;
constexpr uint32_t kDrawInstancedShort = 0x003;
constexpr uint32_t kDrawIndexed = 0x006;
constexpr uint32_t kDrawIndexedShort = 0x007;
constexpr uint32_t kDrawIndexedInstancedShort = 0x009;
constexpr uint32_t kFirstAttachment = 0x4c;
constexpr uint32_t kAttachmentBytes = 0x3c;
constexpr uint32_t kColourAttachments = 8;
constexpr uint32_t kLoadClear = 2;        /* MTLLoadActionClear */

/* MTLPixelFormat values the CPU clear knows. */
constexpr uint32_t kRGBA8Unorm = 70;
constexpr uint32_t kRGBA8UnormSrgb = 71;
constexpr uint32_t kBGRA8Unorm = 80;
constexpr uint32_t kBGRA8UnormSrgb = 81;

/* Compute commands (docs/m1-opcodes.md). */
constexpr uint32_t kDispatchThreadgroups = 0xc8;
constexpr uint32_t kDispatchThreads = 0xca;
constexpr uint32_t kSetBuffers = 0xcb;
constexpr uint32_t kSetBufferOffset = 0xcf;
constexpr uint32_t kSetComputePipeline = 0xd0;
constexpr uint32_t kComputeBarrier = 0xd7;

/* Blit commands (docs/m1-opcodes.md). */
constexpr uint32_t kCopyBufferToBuffer = 0x12d;
constexpr uint32_t kFillBuffer = 0x132;
constexpr uint32_t kFillBufferPattern = 0x13f;
constexpr uint32_t kCopyTextureToBuffer = 0x12e;

#ifdef KERNEL
/* At plugin start: the sysctls that log command buffers. */
void start();

/* Run the command buffer of `len` bytes at `va` in `task`. */
void run(uint32_t task, uint64_t va, uint64_t len);
#endif

} /* namespace PvRun */

#endif /* PvRun_hpp */
