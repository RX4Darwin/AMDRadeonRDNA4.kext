/*
 * pvrun.cpp
 * AMDRadeonRDNA4
 *
 * See pvrun.hpp.
 */

#include "pvrun.hpp"
#include "pvexec.hpp"
#include "klog.hpp"

#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

#define RLOG(fmt, ...) \
	rdna4Log("AMDRadeonRDNA4: pvgpu: run: " fmt "\n", ##__VA_ARGS__)

namespace
{

constexpr size_t kMaxCommandBuffer = 64 * 1024;
constexpr size_t kChunk = 4096;
constexpr uint32_t kSegmentHeader = 8;
constexpr uint32_t kCommandHeader = 8;

/* What a command buffer did: logged for the first ones. */
struct Stats {
	uint32_t done;
	uint32_t failed;
	uint32_t skipped;
	uint32_t firstSkipped;
};

uint32_t gRuns;

uint32_t rd32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));

	return v;
}

uint64_t rd64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));

	return v;
}

/* Whether [off, off + len) lies in a buffer of `size` bytes. */
bool inside(uint64_t off, uint64_t len, uint64_t size)
{
	return off <= size && len <= size - off;
}

/* Fill `len` bytes at `va` with the 4-byte `pattern`, repeated. */
bool fill(uint32_t task, uint64_t va, uint64_t len, uint32_t pattern)
{
	uint32_t *chunk = static_cast<uint32_t *>(IOMalloc(kChunk));
	bool ok = chunk != nullptr;

	for (size_t i = 0; ok && i < kChunk / 4; i++)
		chunk[i] = pattern;

	while (ok && len) {
		const size_t n = len < kChunk ? len : kChunk;

		ok = PvExec::writeGpu(task, va, chunk, n);
		va += n;
		len -= n;
	}

	if (chunk)
		IOFree(chunk, kChunk);

	return ok;
}

bool copy(uint32_t task, uint64_t from, uint64_t to, uint64_t len)
{
	uint8_t *chunk = static_cast<uint8_t *>(IOMalloc(kChunk));
	bool ok = chunk != nullptr;

	while (ok && len) {
		const size_t n = len < kChunk ? len : kChunk;

		ok = PvExec::readGpu(task, from, chunk, n) && PvExec::writeGpu(task, to, chunk, n);
		from += n;
		to += n;
		len -= n;
	}

	if (chunk)
		IOFree(chunk, kChunk);

	return ok;
}

/* fillBuffer:range:value: and fillBuffer:range:pattern4: */
bool fillBuffer(uint32_t task, const uint8_t *p, uint32_t size, bool pattern4)
{
	uint64_t va, bufLen, off, len;
	uint32_t value;

	if (size < 21 || !PvExec::resolveBuffer(task, rd32(p), va, bufLen))
		return false;

	off = rd64(p + 4);
	len = rd64(p + 12);
	value = pattern4 ? rd32(p + 20) : p[20] * 0x01010101u;

	if (!inside(off, len, bufLen))
		return false;

	return fill(task, va + off, len, value);
}

/* copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size: */
bool copyBuffer(uint32_t task, const uint8_t *p, uint32_t size)
{
	uint64_t src, srcLen, dst, dstLen;
	const uint64_t srcOff = size >= 32 ? rd64(p + 8) : 0;
	const uint64_t dstOff = size >= 32 ? rd64(p + 16) : 0;
	const uint64_t len = size >= 32 ? rd64(p + 24) : 0;

	if (size < 32 || !PvExec::resolveBuffer(task, rd32(p), src, srcLen) ||
	    !PvExec::resolveBuffer(task, rd32(p + 4), dst, dstLen))
		return false;

	if (!inside(srcOff, len, srcLen) || !inside(dstOff, len, dstLen))
		return false;

	return copy(task, src + srcOff, dst + dstOff, len);
}

/* One blit command: true when it was run. */
bool runBlit(uint32_t task, uint32_t id, const uint8_t *p, uint32_t size, Stats &s)
{
	bool ok;

	switch (id) {
	case PvRun::kFillBuffer:
		ok = fillBuffer(task, p, size, false);
		break;
	case PvRun::kFillBufferPattern:
		ok = fillBuffer(task, p, size, true);
		break;
	case PvRun::kCopyBufferToBuffer:
		ok = copyBuffer(task, p, size);
		break;
	default:
		return false;
	}

	if (ok)
		s.done++;
	else
		s.failed++;

	return true;
}

/* The commands of one segment. */
void runSegment(uint32_t task, uint8_t type, const uint8_t *p, uint32_t len, Stats &s)
{
	uint32_t at = 0;

	while (at + kCommandHeader <= len) {
		const uint32_t id = rd32(p + at);
		const uint32_t size = rd32(p + at + 4);

		if (size < kCommandHeader || size > len - at)
			break;

		if (type != PvRun::kBlit ||
		    !runBlit(task, id, p + at + kCommandHeader, size - kCommandHeader, s)) {
			if (!s.skipped++)
				s.firstSkipped = id;
		}

		at += size;
	}
}

void runBuffer(uint32_t task, const uint8_t *buf, uint32_t len, Stats &s)
{
	uint32_t at = 0;

	while (at + kSegmentHeader <= len) {
		const uint32_t segLen = rd32(buf + at);
		const uint8_t type = buf[at + 4];

		if (segLen < kSegmentHeader || segLen > len - at)
			break;

		runSegment(task, type, buf + at + kSegmentHeader, segLen - kSegmentHeader, s);
		at += segLen;
	}
}

} /* namespace */

void PvRun::run(uint32_t task, uint64_t va, uint64_t len)
{
	uint8_t *buf;
	Stats s {};

	if (!len || len > kMaxCommandBuffer)
		return;

	buf = static_cast<uint8_t *>(IOMalloc(len));
	if (!buf)
		return;

	if (PvExec::readGpu(task, va, buf, len))
		runBuffer(task, buf, static_cast<uint32_t>(len), s);

	IOFree(buf, len);

	if (gRuns++ < 32 || s.failed)
		RLOG("task %u, %llu bytes: %u run, %u failed, %u not run yet (first id 0x%x)", task,
		     len, s.done, s.failed, s.skipped, s.firstSkipped);
}
