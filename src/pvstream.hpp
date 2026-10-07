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

// ---- GetDeviceInfo (FIFO 0x3a) reply, docs/m0-pvgpu.md "The GetDeviceInfo reply" ----
// The guest announces a reply buffer in record 0x2d {u32 0x2d, u32 bytes/8, u32 page}; AppleParavirtAccelerator::setupDeviceInfo then waits for the command's stamp
// and parses `bytes/8` pairs {u32 key, u32 value} into a 224-byte APVDeviceInfoStruct (parseDeviceInfo). Keys 0 and 0x2b and anything above 0x2c are ignored.
constexpr uint32_t kDeviceInfoBytes = 224;
constexpr uint32_t kDeviceInfoRecord = 0x2d;
struct DeviceInfoKey {
	uint8_t key;
	uint8_t value;      // offset of the u32 value in the struct
	uint8_t defined;    // offset of the "defined" byte the parser sets
	const char *name;
};
const DeviceInfoKey *deviceInfoKeys(size_t *count);
// Fill `buf` (bytes long, a multiple of 8) with the reply: the pairs of kReplyValues (gpuCores = the card's compute-unit count for GpuCoreCount), the rest zero
// (key 0: ignored by the parser). Returns the number of pairs that carry a value.
size_t buildDeviceInfoReply(uint8_t *buf, size_t bytes, uint32_t gpuCores);
// Mirror of Apple's parseDeviceInfo for host tests and the self-test: `pairs` pairs from buf into out[kDeviceInfoBytes] (which the caller zeroed), including the
// post-processing of MaxMetalShaderVersion (default 0x20002, clamp to 0x20007, split into major/minor at +0xd8/+0xdc).
void parseDeviceInfoReference(const uint8_t *buf, uint32_t pairs, uint8_t out[kDeviceInfoBytes]);
// Describe the values we answer with: calls fn(key, name, value, why) for each.
typedef void (*ReplyValueSink)(void *user, uint8_t key, const char *name, uint32_t value, const char *why);
void forEachReplyValue(ReplyValueSink fn, void *user, uint32_t gpuCores);

// ---- display pipe (FIFO 0x01/0x02/0x04-0x07/0x1e), docs/m1-stream.md s.1b ----
// The shared-state page of a display pipe: offsets, the event bits of its pending/enabled words, and the host's half of its contents.
namespace disp {
constexpr uint32_t kMaxPorts = 8;            // DisplayPipe::init refuses a port >= 8
constexpr uint32_t kFirstChannel = 5;        // the pipe's channel number (and [INFER] stamp index) is port + 5
constexpr uint32_t kId = 0x00, kName = 0x04, kNameBytes = 14, kPort = 0x12, kWidth = 0x14, kHeight = 0x16;
constexpr uint32_t kCursorWidth = 0x18, kCursorHeight = 0x1a, kTxnProtocol = 0x1c, kFlags = 0x20;
constexpr uint32_t kOriginX = 0x4c, kOriginY = 0x4e, kScale = 0x50;
constexpr uint32_t kPending = 0x100, kEnabled = 0x104;      // host sets bits in kPending; the guest enables them in kEnabled
constexpr uint32_t kCookie = 0x200, kModeCount = 0x208, kModes = 0x210, kModeStride = 16, kMaxModes = 8;
constexpr uint32_t kCursorPos = 0xe00, kCursorVisible = 0xe04;
constexpr uint32_t kEventVbl = 1, kEventOnline = 4, kEventOffline = 8;   // bits of kPending / kEnabled (enable() stores 0xc; enableVBLInterrupt ORs 1)
constexpr uint32_t kFlagCursorVisibleKick = 1, kFlagCursorMoveKick = 2;  // kFlags: tell the host (ctrl+0x220 = port) when the cursor appears / moves

struct Mode {
	uint16_t width, height;
	uint32_t refresh;       // 16.16 fixed-point Hz [INFER: edidGenerateModeTiming divides by 65536]
};
// What the host puts into a pipe's page for process_online (all of it is what AppleParavirtDisplayPipe::process_online and its callbacks read).
struct Info {
	uint32_t id { 0 };
	char name[kNameBytes] { 'R', 'D', 'N', 'A', '4', 'F', 'B', ' ', 'P', 'V', 0 };
	uint16_t width { 1920 }, height { 1080 };
	uint16_t cursorWidth { 64 }, cursorHeight { 64 };
	uint32_t txnProtocol { 0 };         // 0: SubmitTransaction is FIFO 0x06; non-zero: 0x07 (with the gamma table)
	uint32_t flags { 0 };               // kFlag*: host wants a kick for cursor changes
	uint8_t scale { 1 };                // scaleFactor of ParavirtDisplayPrefs
	uint32_t cookie { 1 };              // echoed in FIFO 0x02
	uint32_t modeCount { 1 };
	Mode modes[kMaxModes] { { 1920, 1080, 60u << 16 } };
};
// Write the host's fields of the page (everything but the pending/enabled words, the chromaticities and the origin, which the guest pre-fills, and the cursor
// state, which the guest owns). `page` is the 4 KiB page; the port is written at +0x12.
void fillInfo(uint8_t *page, uint32_t port, const Info &info);
// What process_online reads back (mirror, for host tests and the self-test): the page into `out` plus the mode i callback {u16 w, u16 h, u32 refresh}.
struct InfoRead {
	uint32_t id, flags, cookie, count, txnProtocol;
	uint16_t port, width, height, cursorWidth, cursorHeight;
	char name[kNameBytes + 1];
	Mode modes[kMaxModes];
};
void readInfo(const uint8_t *page, InfoRead *out);

} // namespace disp

} // namespace pvstream

#endif /* RDNA4PvStream_hpp */
