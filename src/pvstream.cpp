//
//  pvstream.cpp
//  RDNA4FB
//
//  See pvstream.hpp.
//

#include "pvstream.hpp"

#include <string.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

namespace pvstream {

#include "pvopcodes.inc"
#include "pvdevinfo.inc"

namespace {

template <typename T, size_t N>
const T *findId(const T (&table)[N], uint32_t id) {
	// the tables are sorted by id
	size_t lo = 0, hi = N;
	while (lo < hi) {
		const size_t mid = (lo + hi) / 2;
		if (table[mid].id == id)
			return &table[mid];
		if (table[mid].id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return nullptr;
}

uint32_t rd32(const uint8_t *p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}
uint64_t rd64(const uint8_t *p) {
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

} // namespace

const char *streamName(uint32_t id, int32_t *payloadLen) {
	const PvOpcode *o = findId(kPvStreamOpcodes, id);
	if (o && payloadLen)
		*payloadLen = o->len;
	return o ? o->name : nullptr;
}
const char *operationName(uint32_t id, int32_t *payloadLen) {
	const PvOpcode *o = findId(kPvOperations, id);
	if (o && payloadLen)
		*payloadLen = o->len;
	return o ? o->name : nullptr;
}
const char *fifoName(uint16_t id) {
	for (const PvFifoCommand &c : kPvFifoCommands)
		if (c.id == id)
			return c.name;
	return nullptr;
}

bool fifoHeader(const uint8_t *p, size_t avail, FifoCommand *out, uint32_t maxLength) {
	if (avail < kFifoHeaderBytes)
		return false;
	FifoCommand c;
	memcpy(&c.id, p, 2);
	memcpy(&c.barriers, p + 2, 2);
	c.length = rd32(p + 4);
	c.signal = rd32(p + 8);
	c.payloadOffset = kFifoHeaderBytes + 8u * c.barriers;
	if (c.length < c.payloadOffset || (c.length & 3) || c.length > maxLength)
		return false;
	if (out)
		*out = c;
	return true;
}

void describeFifo(const uint8_t *p, const FifoCommand &c, char *out, size_t cap) {
	const char *name = fifoName(c.id);
	const uint8_t *pl = p + c.payloadOffset;
	const uint32_t plen = c.length - c.payloadOffset;
	int n = snprintf(out, cap, "cmd 0x%02x %s, %u bytes (%u payload), %u barrier(s), signal 0x%x", c.id, name ? name : "?", c.length, plen, c.barriers, c.signal);
	if (n < 0 || static_cast<size_t>(n) >= cap)
		return;
	char *w = out + n;
	size_t room = cap - static_cast<size_t>(n);
	// Per-command payload layouts that were read in the driver (docs/m1-stream.md); the rest is only counted. Field meanings marked [INFER] there.
	switch (c.id) {
	case 0x30: // DefineChannel: {u32 channel}
	case 0x31: // FreeChannel
		if (plen >= 4)
			snprintf(w, room, ": channel %u", rd32(pl));
		break;
	case 0x38: // DefineHostTask: {u32 task<<1, u64 address-space size, u32 page-table root page}
		if (plen >= 16)
			snprintf(w, room, ": task %u (raw 0x%x), space 0x%llx, root page 0x%x", rd32(pl) >> 1, rd32(pl), static_cast<unsigned long long>(rd64(pl + 4)), rd32(pl + 12));
		break;
	case 0x39: // CommitIntoGPUPageTable: {u32 task, u64 gpu address, u64 length}
		if (plen >= 20)
			snprintf(w, room, ": task %u, gpu va 0x%llx, length 0x%llx", rd32(pl), static_cast<unsigned long long>(rd64(pl + 4)), static_cast<unsigned long long>(rd64(pl + 12)));
		break;
	case 0x3a: // GetDeviceInfo: record 0x2d {u32 0x2d, u32 bytes/8, u32 page} = where the host writes the reply
		if (plen >= 12 && rd32(pl) == 0x2d)
			snprintf(w, room, ": reply buffer page 0x%x, %u bytes", rd32(pl + 8), rd32(pl + 4) * 8);
		break;
	case 0x37: // ExecIndirect: chunks of {u32 resource id, u32 length}
		if (plen >= 8)
			snprintf(w, room, ": %u chunk(s), first resource %u length %u", plen / 8, rd32(pl), rd32(pl + 4));
		break;
	default:
		break;
	}
}

bool streamHeader(const uint8_t *p, size_t avail, StreamCommand *out) {
	if (avail < kStreamHeaderBytes)
		return false;
	StreamCommand c { rd32(p), rd32(p + 4) };
	if (c.size < kStreamHeaderBytes || (c.size & 3) || c.size > avail)
		return false;
	if (out)
		*out = c;
	return true;
}

size_t walkCommands(const uint8_t *p, size_t n, bool operations, StreamSink sink, void *user) {
	size_t at = 0;
	StreamCommand c;
	while (at < n && streamHeader(p + at, n - at, &c)) {
		const char *name = operations ? operationName(c.id) : streamName(c.id);
		if (sink)
			sink(user, c.id, name, p + at + kStreamHeaderBytes, c.size - kStreamHeaderBytes, at);
		at += c.size;
	}
	return at;
}

const char *segmentTypeName(uint8_t type) {
	switch (type) {
	case 0: return "render";
	case 1: return "compute";
	case 2: return "blit";
	case 4: return "info";
	case 5: return "protection envelope";
	default: return "?";
	}
}

// ---- GetDeviceInfo reply ----

namespace {
// What we answer, field by field (docs/m0-pvgpu.md has the same table with the reasoning). Values that are properties of the card come from the card;
// the rest is the most conservative value the parser accepts (feature off, or the key left out so that the driver's own default applies).
struct ReplyValue {
	uint8_t key;
	uint32_t value;     // kGpuCores: the compute-unit count passed in
	const char *why;
};
constexpr uint32_t kGpuCores = 0xffffffffu;
const ReplyValue kReplyValues[] = {
	{ 0x01, 4, "max sample count (counts 1,2,4,8,16 are valid, supported iff n <= value): conservative, the card does 8" },
	{ 0x02, 0, "no Depth24Stencil8: conservative (D32S8 exists)" },
	{ 0x03, 1024, "max threads per threadgroup, x: card (workgroup limit 1024 per dimension)" },
	{ 0x04, 1024, "max threads per threadgroup, y: card" },
	{ 0x05, 1024, "max threads per threadgroup, z: card" },
	{ 0x06, 32768, "threadgroup memory length: conservative (the card's LDS is 64 KiB)" },
	{ 0x07, 0, "no framebuffer read: conservative" },
	{ 0x08, 0, "no RGB10A2 gamma: conservative" },
	{ 0x09, 1, "native FP16: card (RDNA4 packed FP16)" },
	// 0x0a DeserializerVersion: left out on purpose: the Metal bundle then uses the oldest serializer (no reflection, shared-texture, OpenGL, IOSurface-rotation
	// or corrected-base-vertex variants; thresholds 3/5/6/7/8). M1 raises it once the decoder covers the version-gated variants.
	{ 0x0b, 0x1f, "primitive types: bit per MTLPrimitiveType point..triangle strip [INFER: bit layout]" },
	{ 0x0c, 0, "no multiplane textures: conservative" },
	{ 0x0d, 256, "linear texture alignment in bytes: conservative power of two (AMD uses 256)" },
	{ 0x0e, 0, "no heap buffers: conservative" },
	// 0x0f HeapBufferAlignment: left out (heaps off)
	{ 0x10, 0, "no heap textures: conservative" },
	{ 0x11, 0, "no buffer-from-IOSurface: conservative" },
	// 0x12 MaxMetalShaderVersion: left out: the driver defaults it to 0x20002 (2.2); any value >= 0x20008 is clamped to 0x20007 anyway
	{ 0x13, 0, "no shared textures: conservative" },
	// 0x14 MaxVertexAmplificationCount: left out
	{ 0x15, 0, "no programmable sample positions: conservative" },
	// 0x16 RasterizationRateLayerCount: left out
	{ 0x17, 0, "no tile shaders: conservative" },
	{ 0x18, 0, "no image blocks: conservative" },
	{ 0x19, 0, "no raster order groups: conservative" },
	{ 0x1a, 0, "no memory-order atomics: conservative" },
	{ 0x1b, 0, "no large MRT: conservative" },
	{ 0x1c, 0, "SupportFlags2023 all clear: conservative" },
	{ 0x1d, 1024, "max total compute threads per threadgroup: card" },
	// 0x1e MaxComputeLocalMemorySizes: left out
	{ 0x1f, 32768, "max compute threadgroup memory: conservative (same as 0x06)" },
	{ 0x20, 16, "threadgroup memory alignment in bytes: conservative" },
	{ 0x21, 0, "SupportFlags2024 all clear (no argument buffers, no command-buffer jump...): conservative" },
	{ 0x22, kGpuCores, "GPU core count: card (compute units)" },
	{ 0x23, 2048, "max texture layers: Metal's 2D-array limit" },
	// 0x24 MaxPredicatedNestingDepth, 0x25 HostGPUFamilyClamped, 0x26 ArgumentBuffersTier, 0x27 ArgumentBuffersMaxSamplerCount, 0x29 write rounding modes: left out
	{ 0x28, 256, "minimum linear texture alignment: conservative (same as 0x0d)" },
	{ 0x2a, 0, "SupportFlags2025 all clear (no ICBs): conservative" },
	// 0x2c HostGPUFamilies: left out
};
} // namespace

const DeviceInfoKey *deviceInfoKeys(size_t *count) {
	if (count)
		*count = sizeof(kDeviceInfoKeys) / sizeof(kDeviceInfoKeys[0]);
	return kDeviceInfoKeys;
}

size_t buildDeviceInfoReply(uint8_t *buf, size_t bytes, uint32_t gpuCores) {
	memset(buf, 0, bytes);
	size_t n = 0;
	for (const ReplyValue &r : kReplyValues) {
		if ((n + 1) * 8 > bytes)
			break;
		const uint32_t key = r.key, value = r.value == kGpuCores ? gpuCores : r.value;
		memcpy(buf + n * 8, &key, 4);
		memcpy(buf + n * 8 + 4, &value, 4);
		n++;
	}
	return n;
}

void forEachReplyValue(ReplyValueSink fn, void *user, uint32_t gpuCores) {
	for (const ReplyValue &r : kReplyValues) {
		const char *name = "?";
		for (const DeviceInfoKey &k : kDeviceInfoKeys)
			if (k.key == r.key)
				name = k.name;
		fn(user, r.key, name, r.value == kGpuCores ? gpuCores : r.value, r.why);
	}
}

void parseDeviceInfoReference(const uint8_t *buf, uint32_t pairs, uint8_t out[kDeviceInfoBytes]) {
	for (uint32_t i = 0; i < pairs; i++) {
		const uint32_t key = rd32(buf + 8u * i), value = rd32(buf + 8u * i + 4);
		for (const DeviceInfoKey &k : kDeviceInfoKeys) {
			if (k.key == key) {
				memcpy(out + k.value, &value, 4);
				out[k.defined] = 1;
				break;
			}
		}
	}
	uint32_t version;
	if (out[0xbd]) {
		memcpy(&version, out + 0x44, 4);
		if (version >= 0x20008)
			version = 0x20007;
	} else {
		out[0xbd] = 1;
		version = 0x20002;
	}
	memcpy(out + 0x44, &version, 4);
	const uint32_t major = version >> 16, minor = version & 0xffff;
	memcpy(out + 0xd8, &major, 4);
	memcpy(out + 0xdc, &minor, 4);
}

} // namespace pvstream
