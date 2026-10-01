//
//  pvstream.hpp
//  RDNA4FB
//
//  M1 prep (docs/m1-stream.md, hub-task-440): decoding of the three command layers of Apple's paravirtual GPU, as measured in AppleParavirtGPU.kext and
//  AppleParavirtGPUMetal.bundle. Host-testable (tools/pvstream-test.cpp), no kernel dependencies: the polling host (pvgpu.cpp) logs the FIFO with it.
//
//    FIFO (kernel driver -> host; root FIFO in guest RAM, child rings per channel)   {u16 id, u16 barriers, u32 length, u32 signal, barriers..., payload}
//    stream (Metal bundle -> host; command buffers in guest resources)             {u32 id, u32 size} + payload, grouped in segments
//    operations (Metal bundle -> kernel -> host; 4 KiB "operation" buffer)          same {u32 id, u32 size} framing, different id space
//
//  Everything here is log-and-describe: nothing is executed.
//

#ifndef RDNA4PvStream_hpp
#define RDNA4PvStream_hpp

#include <stddef.h>
#include <stdint.h>

namespace pvstream {

struct PvOpcode {
	uint32_t id;
	int32_t len;        // payload bytes after the 8-byte command header; -1: variable or not a single constant
	const char *name;
};
struct PvFifoCommand {
	uint16_t id;
	const char *name;
};

// Names; nullptr when the id is not known.
const char *streamName(uint32_t id, int32_t *payloadLen = nullptr);
const char *operationName(uint32_t id, int32_t *payloadLen = nullptr);
const char *fifoName(uint16_t id);

// ---- FIFO layer ----
constexpr uint32_t kFifoHeaderBytes = 12;
struct FifoCommand {
	uint16_t id;
	uint16_t barriers;      // number of {u32, u32} barrier pairs right after the header
	uint32_t length;        // total bytes of this command including the header
	uint32_t signal;
	uint32_t payloadOffset; // header + barriers
};
// Validate and read the header at p (avail bytes available). false when it cannot be a command of this driver: length below the header plus its barriers, not
// a multiple of 4, or larger than `maxLength`. (Apple's command allocator holds 0x400 bytes unless a command needs more; the FIFO ring is 60 KiB.)
bool fifoHeader(const uint8_t *p, size_t avail, FifoCommand *out, uint32_t maxLength = 0xf000);
// One line describing the command and its payload where the layout is known (see docs/m1-stream.md, section FIFO); `p` points at the command start.
void describeFifo(const uint8_t *p, const FifoCommand &c, char *out, size_t cap);

// ---- stream / operation layer ----
constexpr uint32_t kStreamHeaderBytes = 8;
struct StreamCommand {
	uint32_t id;
	uint32_t size;          // total bytes including the 8-byte header (a multiple of 4)
};
bool streamHeader(const uint8_t *p, size_t avail, StreamCommand *out);
// Walk consecutive commands in [p, p+n). `operations` selects the id space. Calls sink(user, id, name, payload, payloadBytes, offset) for each; returns the
// number of bytes that decoded cleanly (n when all of it did), so a caller can report where a stream stops making sense.
typedef void (*StreamSink)(void *user, uint32_t id, const char *name, const uint8_t *payload, uint32_t payloadBytes, size_t offset);
size_t walkCommands(const uint8_t *p, size_t n, bool operations, StreamSink sink, void *user);

// Segment header of a command-buffer chunk: 8 bytes, {u32 length, u8 type, u8 continuation, u8 flags, u8 reserved}. Types measured in the encoders'
// beginSegment:: render 0, compute 1, blit 2, info 4; 5 is the protection-options envelope (a raw u64 follows).
const char *segmentTypeName(uint8_t type);

} // namespace pvstream

#endif /* RDNA4PvStream_hpp */
