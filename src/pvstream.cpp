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

} // namespace pvstream
