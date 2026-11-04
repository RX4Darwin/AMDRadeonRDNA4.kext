/*
 * pvrun.cpp
 * AMDRadeonRDNA4
 *
 * See pvrun.hpp.
 */

#include "pvrun.hpp"
#include "pvexec.hpp"
#include "pvjob.hpp"
#include "pvsurface.hpp"
#include "klog.hpp"

#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <sys/sysctl.h>

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

/* A compute encoder's state between commands: the pipeline and the buffers bound by slot. */
struct Compute {
	uint32_t pipeline;
	bool havePipeline;
	uint32_t bound;     /* bit per slot */
	uint32_t ref[RDNA4_PVJOB_MAX_BUFFERS];
	uint64_t offset[RDNA4_PVJOB_MAX_BUFFERS];
};

/* How long a dispatch may take the service, compiling included. */
constexpr uint32_t kDispatchTimeoutMs = 20000;

uint32_t gRuns;

/*
 * Command buffers with commands not run yet or failed that are still logged
 * whole, and whether with the functions' code: from boot, and again when
 * root writes debug.rdna4_pvdump (and _code) right before a test.
 */
int gDumps = 8;
int gDumpCode;
constexpr uint32_t kDumpBytes = 2048;

SYSCTL_INT(_debug, OID_AUTO, rdna4_pvdump, CTLFLAG_RW | CTLFLAG_LOCKED, &gDumps, 0,
	   "AMDRadeonRDNA4: paravirtual command buffers still to log");
SYSCTL_INT(_debug, OID_AUTO, rdna4_pvdump_code, CTLFLAG_RW | CTLFLAG_LOCKED, &gDumpCode, 0,
	   "AMDRadeonRDNA4: log the functions' code with them");

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

/* setBuffers: {u32 first slot, u32 count, count x {u32 ref, u64 offset}} */
bool setBuffers(Compute &c, const uint8_t *p, uint32_t size)
{
	const uint32_t first = size >= 8 ? rd32(p) : 0;
	const uint32_t count = size >= 8 ? rd32(p + 4) : 0;

	if (size < 8 || first + count > RDNA4_PVJOB_MAX_BUFFERS || size < 8 + 12 * count)
		return false;

	for (uint32_t i = 0; i < count; i++) {
		c.ref[first + i] = rd32(p + 8 + 12 * i);
		c.offset[first + i] = rd64(p + 12 + 12 * i);
		c.bound |= 1u << (first + i);
	}

	return true;
}

/* setBufferOffset:atIndex: {u32 slot, u64 offset} */
bool setBufferOffset(Compute &c, const uint8_t *p, uint32_t size)
{
	const uint32_t slot = size >= 12 ? rd32(p) : RDNA4_PVJOB_MAX_BUFFERS;

	if (slot >= RDNA4_PVJOB_MAX_BUFFERS || !(c.bound & (1u << slot)))
		return false;

	c.offset[slot] = rd64(p + 4);

	return true;
}

/* The bound buffers into the job, by slot, each from its offset to its end. */
bool bindBuffers(uint32_t task, const Compute &c, RDNA4PvJob &job)
{
	for (uint32_t slot = 0; slot < RDNA4_PVJOB_MAX_BUFFERS; slot++) {
		RDNA4PvBuffer &b = job.buffers[job.bufferCount];
		uint64_t va, len;

		if (!(c.bound & (1u << slot)))
			continue;

		if (!PvExec::resolveBuffer(task, c.ref[slot], va, len) || c.offset[slot] > len)
			return false;

		b.slot = slot;
		b.va = va + c.offset[slot];
		b.length = len - c.offset[slot];
		job.bufferCount++;
	}

	return true;
}

/*
 * dispatchThreads / dispatchThreadgroups: {u64 size[3], u64 threadsPerThreadgroup[3]},
 * the first the grid in threads or in threadgroups. Run by the compiler
 * service; false when it is not there or the dispatch failed.
 */
bool dispatch(uint32_t task, const Compute &c, const uint8_t *p, uint32_t size, bool groups)
{
	RDNA4PvJob job {};

	if (size < 48 || !c.havePipeline)
		return false;

	if (!PvExec::resolveComputePipeline(task, c.pipeline, job.codeVa, job.codeBytes) ||
	    job.codeBytes > RDNA4_PVJOB_MAX_CODE)
		return false;

	job.task = task;

	for (int d = 0; d < 3; d++) {
		const uint64_t n = rd64(p + 8 * d);
		const uint64_t tg = rd64(p + 24 + 8 * d);
		const uint64_t threads = groups ? n * tg : n;

		if (!tg || tg > 1024 || threads > 0xffffffffull)
			return false;

		job.threads[d] = static_cast<uint32_t>(threads);
		job.groupSize[d] = static_cast<uint32_t>(tg);
	}

	if (!bindBuffers(task, c, job))
		return false;

	return PvJob::run(job, kDispatchTimeoutMs);
}

/* One compute command: true when it was run. */
bool runCompute(uint32_t task, Compute &c, uint32_t id, const uint8_t *p, uint32_t size, Stats &s)
{
	bool ok;

	switch (id) {
	case PvRun::kSetComputePipeline:
		ok = size >= 4;
		c.pipeline = ok ? rd32(p) : 0;
		c.havePipeline = ok;
		break;
	case PvRun::kSetBuffers:
		ok = setBuffers(c, p, size);
		break;
	case PvRun::kSetBufferOffset:
		ok = setBufferOffset(c, p, size);
		break;
	case PvRun::kComputeBarrier:
		ok = true;   /* the service runs one dispatch after the other */
		break;
	case PvRun::kDispatchThreads:
	case PvRun::kDispatchThreadgroups:
		ok = dispatch(task, c, p, size, id == PvRun::kDispatchThreadgroups);
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

/*
 * A colour channel of a clear colour, a double in [0, 1], as an 8-bit unorm,
 * from its bits: the kernel does not use the FPU.
 */
uint32_t unorm8(uint64_t bits)
{
	const uint32_t exp = static_cast<uint32_t>(bits >> 52) & 0x7ff;
	uint64_t mant;
	uint32_t shift;

	/* below 2^-9 the value rounds to 0, and the shift below stays under 64 */
	if (bits >> 63 || exp < 1023 - 9)
		return 0;

	if (exp >= 1023)
		return 255;

	/* 255 * 1.m * 2^(exp - 1023), rounded */
	mant = (bits & ((1ull << 52) - 1)) | (1ull << 52);
	shift = 52 + (1023 - exp);

	return static_cast<uint32_t>((mant * 255 + (1ull << (shift - 1))) >> shift);
}

/* The bytes of one pixel of a clear colour, for the 8-bit RGBA and BGRA formats. */
bool clearPixel(uint32_t format, const uint8_t *rgba, uint32_t &pixel)
{
	const uint32_t r = unorm8(rd64(rgba)), g = unorm8(rd64(rgba + 8));
	const uint32_t b = unorm8(rd64(rgba + 16)), a = unorm8(rd64(rgba + 24));

	switch (format) {
	case PvRun::kRGBA8Unorm:
	case PvRun::kRGBA8UnormSrgb:
		pixel = r | g << 8 | b << 16 | a << 24;
		return true;
	case PvRun::kBGRA8Unorm:
	case PvRun::kBGRA8UnormSrgb:
		pixel = b | g << 8 | r << 16 | a << 24;
		return true;
	default:
		return false;
	}
}

/* Every row of a texture's first slice and level, filled with one pixel. */
bool fillTexture(uint32_t task, const PvExec::Texture &t, uint32_t pixel)
{
	bool ok = true;

	for (uint32_t y = 0; ok && y < t.height; y++)
		ok = fill(task, t.va + y * t.bytesPerRow, 4ull * t.width, pixel);

	return ok;
}

/* Clear the texture `ref` names, in the client's memory or on an IOSurface. */
bool clearAttachment(uint32_t task, uint32_t ref, const uint8_t *rgba)
{
	PvExec::Texture t;
	PvExec::SurfaceTexture st;
	uint32_t pixel;

	if (PvExec::resolveTexture(task, ref, t))
		return clearPixel(t.format, rgba, pixel) && fillTexture(task, t, pixel);

	if (PvExec::resolveSurfaceTexture(task, ref, st))
		return clearPixel(st.format, rgba, pixel) && PvSurface::fill(st.surface, st.width, st.height, pixel);

	return false;
}

/*
 * The render pass descriptor: colour attachments of 0x3c bytes from 0x4c,
 * {u32 texture ref, u32 resolve ref, u16 level, slice, depth plane,
 * resolve level, slice, depth plane, u16 load action, u16 store action,
 * u32 options, double clear colour[4]} (measured, card). The clears run
 * here; drawing is not done yet.
 */
bool renderPass(uint32_t task, const uint8_t *p, uint32_t size)
{
	bool ok = true;

	for (uint32_t i = 0; i < PvRun::kColourAttachments; i++) {
		const uint8_t *a = p + PvRun::kFirstAttachment + i * PvRun::kAttachmentBytes;

		if (PvRun::kFirstAttachment + (i + 1) * PvRun::kAttachmentBytes > size)
			return false;

		if (!rd32(a) || (a[0x14] | a[0x15] << 8) != PvRun::kLoadClear)
			continue;

		ok = clearAttachment(task, rd32(a), a + 0x1c) && ok;
	}

	return ok;
}

/*
 * copyFromTexture:...toBuffer: {u32 texture, u32 buffer, u64 origin[3],
 * u64 size[3], u64 destination offset, bytes per row, bytes per image,
 * u64 options} (measured, card), of the first slice and level.
 */
bool copyTextureToBuffer(uint32_t task, const uint8_t *p, uint32_t size)
{
	PvExec::Texture t;
	uint64_t buf, bufLen;
	uint8_t *row;
	bool ok = true;

	if (size < 88 || !PvExec::resolveTexture(task, rd32(p), t) ||
	    !PvExec::resolveBuffer(task, rd32(p + 4), buf, bufLen))
		return false;

	const uint64_t x = rd64(p + 8), y = rd64(p + 16);
	const uint64_t w = rd64(p + 32), h = rd64(p + 40);
	const uint64_t off = rd64(p + 56), pitch = rd64(p + 64);
	const uint64_t rowBytes = 4 * w;

	if (x + w > t.width || y + h > t.height || rowBytes > kChunk || !h ||
	    !inside(off, (h - 1) * pitch + rowBytes, bufLen))
		return false;

	row = static_cast<uint8_t *>(IOMalloc(kChunk));
	if (!row)
		return false;

	for (uint64_t i = 0; ok && i < h; i++) {
		ok = PvExec::readGpu(task, t.va + (y + i) * t.bytesPerRow + 4 * x, row, rowBytes) &&
		     PvExec::writeGpu(task, buf + off + i * pitch, row, rowBytes);
	}

	IOFree(row, kChunk);

	return ok;
}

/* One render command: true when it was run. */
bool runRender(uint32_t task, uint32_t id, const uint8_t *p, uint32_t size, Stats &s)
{
	if (id != PvRun::kRenderPass)
		return false;

	if (renderPass(task, p, size))
		s.done++;
	else
		s.failed++;

	return true;
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
	case PvRun::kCopyTextureToBuffer:
		ok = copyTextureToBuffer(task, p, size);
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

/* One command of a segment: true when it was run (or failed running). */
bool runCommand(uint32_t task, uint8_t type, Compute &c, uint32_t id, const uint8_t *p, uint32_t size, Stats &s)
{
	if (type == PvRun::kBlit)
		return runBlit(task, id, p, size, s);

	if (type == PvRun::kCompute)
		return runCompute(task, c, id, p, size, s);

	if (type == PvRun::kRender)
		return runRender(task, id, p, size, s);

	return false;
}

/* The commands of one segment; a compute segment starts with nothing bound. */
void runSegment(uint32_t task, uint8_t type, const uint8_t *p, uint32_t len, Stats &s)
{
	Compute c {};
	uint32_t at = 0;

	while (at + kCommandHeader <= len) {
		const uint32_t id = rd32(p + at);
		const uint32_t size = rd32(p + at + 4);

		if (size < kCommandHeader || size > len - at)
			break;

		if (!runCommand(task, type, c, id, p + at + kCommandHeader, size - kCommandHeader, s)) {
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

/* A command buffer with commands not run yet or failed, whole, and the heap they name. */
void dumpBuffer(uint32_t task, const uint8_t *buf, uint32_t len)
{
	char line[64 * 3];

	RLOG("buffer of task %u, %u bytes:", task, len);

	for (uint32_t off = 0; off < len && off < kDumpBytes; off += 32) {
		uint32_t at = 0;

		for (uint32_t i = off; i < len && i < off + 32 && at + 3 < sizeof(line); i++) {
			snprintf(line + at, sizeof(line) - at, "%02x", buf[i]);
			at += 2;
			if ((i & 3) == 3)
				line[at++] = ' ';
		}

		line[at] = 0;
		RLOG("  %04x %s", off, line);
	}

	PvExec::logHeap(task, gDumpCode != 0);
}

} /* namespace */

void PvRun::start()
{
	sysctl_register_oid(&sysctl__debug_rdna4_pvdump);
	sysctl_register_oid(&sysctl__debug_rdna4_pvdump_code);
}

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

	if ((s.skipped || s.failed) && gDumps > 0) {
		gDumps--;
		dumpBuffer(task, buf, static_cast<uint32_t>(len));
	}

	IOFree(buf, len);

	if (gRuns++ < 32 || s.failed)
		RLOG("task %u, %llu bytes: %u run, %u failed, %u not run yet (first id 0x%x)", task,
		     len, s.done, s.failed, s.skipped, s.firstSkipped);
}
