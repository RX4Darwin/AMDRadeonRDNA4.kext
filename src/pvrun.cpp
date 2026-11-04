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
	uint32_t firstFailed;
};

/* Buffers bound by slot. */
struct Bindings {
	uint32_t bound;     /* bit per slot */
	uint32_t ref[RDNA4_PVJOB_MAX_BUFFERS];
	uint64_t offset[RDNA4_PVJOB_MAX_BUFFERS];
};

/* A compute encoder's state between commands: the pipeline and the buffers. */
struct Compute {
	uint32_t pipeline;
	bool havePipeline;
	Bindings buffers;
};

/* Objects bound by slot: textures or samplers. */
struct Refs {
	uint32_t bound;
	uint32_t ref[RDNA4_PVJOB_MAX_TEXTURES];
};

/* A render encoder's: the pipeline, both stages' buffers, the fragment textures and samplers, colour attachment 0. */
struct Render {
	uint32_t pipeline;
	bool havePipeline;
	uint32_t target;    /* texture ref, 0 = none */
	Bindings vertex, fragment;
	Refs textures, samplers;
	bool haveViewport, haveScissor;
	uint32_t viewport[4];   /* float bits */
	uint32_t scissor[4];
};

/* A segment's encoder, whichever kind it is. */
struct Encoder {
	Compute compute;
	Render render;
};

/* How long a dispatch may take the service, compiling included. */
constexpr uint32_t kDispatchTimeoutMs = 20000;

/* Draws whose refusal is logged with its reason, and resources logged for it. */
constexpr uint32_t kRefusalsLogged = 64;
uint32_t gResourceLogs;

uint32_t gRuns;

/*
 * Command buffers with commands not run yet or failed that are still logged
 * whole, and whether with the functions' code: from boot, and again when
 * root writes debug.rdna4_pvdump (and _code) right before a test; only a
 * task's while debug.rdna4_pvdump_task names one.
 */
int gDumps = 8;
int gDumpCode;
int gDumpTask;
constexpr uint32_t kDumpBytes = 2048;

SYSCTL_INT(_debug, OID_AUTO, rdna4_pvdump, CTLFLAG_RW | CTLFLAG_LOCKED, &gDumps, 0,
	   "AMDRadeonRDNA4: paravirtual command buffers still to log");
SYSCTL_INT(_debug, OID_AUTO, rdna4_pvdump_code, CTLFLAG_RW | CTLFLAG_LOCKED, &gDumpCode, 0,
	   "AMDRadeonRDNA4: log the functions' code with them");
SYSCTL_INT(_debug, OID_AUTO, rdna4_pvdump_task, CTLFLAG_RW | CTLFLAG_LOCKED, &gDumpTask, 0,
	   "AMDRadeonRDNA4: log only this task's command buffers (0: all)");

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

/* setBuffers (any stage): {u32 first slot, u32 count, count x {u32 ref, u64 offset}} */
bool setBuffers(Bindings &c, const uint8_t *p, uint32_t size)
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
bool setBufferOffset(Bindings &c, const uint8_t *p, uint32_t size)
{
	const uint32_t slot = size >= 12 ? rd32(p) : RDNA4_PVJOB_MAX_BUFFERS;

	if (slot >= RDNA4_PVJOB_MAX_BUFFERS || !(c.bound & (1u << slot)))
		return false;

	c.offset[slot] = rd64(p + 4);

	return true;
}

/* setTextures / setSamplerStates: {u32 first slot, u32 count, count x u32 ref} */
bool setRefs(Refs &r, const uint8_t *p, uint32_t size)
{
	const uint32_t first = size >= 8 ? rd32(p) : 0;
	const uint32_t count = size >= 8 ? rd32(p + 4) : 0;

	if (size < 8 || first + count > RDNA4_PVJOB_MAX_TEXTURES || size < 8 + 4 * count)
		return false;

	for (uint32_t i = 0; i < count; i++) {
		r.ref[first + i] = rd32(p + 8 + 4 * i);
		r.bound |= 1u << (first + i);
	}

	return true;
}

/* The bound buffers into the job, by slot, each from its offset to its end. */
bool bindBuffers(uint32_t task, const Bindings &c, uint32_t stage, RDNA4PvJob &job)
{
	for (uint32_t slot = 0; slot < RDNA4_PVJOB_MAX_BUFFERS; slot++) {
		RDNA4PvBuffer &b = job.buffers[job.bufferCount];
		uint64_t va, len;

		if (!(c.bound & (1u << slot)))
			continue;

		if (job.bufferCount == RDNA4_PVJOB_MAX_BUFFERS ||
		    !PvExec::resolveBuffer(task, c.ref[slot], va, len) || c.offset[slot] > len)
			return false;

		b.slot = slot;
		b.stage = stage;
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

	if (!bindBuffers(task, c.buffers, RDNA4_PVJOB_VERTEX, job))
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
		ok = setBuffers(c.buffers, p, size);
		break;
	case PvRun::kSetBufferOffset:
		ok = setBufferOffset(c.buffers, p, size);
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
	else if (!s.failed++)
		s.firstFailed = id;

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

/* A double's bits as a float's (rounded toward zero), without the FPU. */
uint32_t floatOfDouble(uint64_t bits)
{
	const uint32_t sign = static_cast<uint32_t>(bits >> 63) << 31;
	const int exp = static_cast<int>((bits >> 52) & 0x7ff) - 1023 + 127;

	if (!(bits << 1))
		return sign;

	if (exp <= 0)
		return sign;

	if (exp >= 255)
		return sign | 0x7f800000;

	return sign | static_cast<uint32_t>(exp) << 23 | static_cast<uint32_t>((bits >> 29) & 0x7fffff);
}

/* setViewport: {double x, y, width, height, znear, zfar} (measured, card) */
bool setViewport(Render &r, const uint8_t *p, uint32_t size)
{
	if (size < 32)
		return false;

	for (int i = 0; i < 4; i++)
		r.viewport[i] = floatOfDouble(rd64(p + 8 * i));

	r.haveViewport = true;

	return true;
}

/* setScissorRect: {u64 x, y, width, height} (measured, card) */
bool setScissor(Render &r, const uint8_t *p, uint32_t size)
{
	if (size < 32)
		return false;

	for (int i = 0; i < 4; i++)
		r.scissor[i] = static_cast<uint32_t>(rd64(p + 8 * i));

	r.haveScissor = true;

	return true;
}

/* The bits of a float of value `v` (a pixel count), built without the FPU. */
uint32_t floatBits(uint32_t v)
{
	uint32_t exp = 127 + 31;

	if (!v)
		return 0;

	while (!(v & 0x80000000u)) {
		v <<= 1;
		exp--;
	}

	return exp << 23 | ((v >> 8) & 0x7fffff);
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
bool renderPass(uint32_t task, Render &r, const uint8_t *p, uint32_t size)
{
	bool ok = true;

	if (size >= PvRun::kFirstAttachment + 4)
		r.target = rd32(p + PvRun::kFirstAttachment);

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
/* Bytes a pixel of the MTLPixelFormats a blit copies; 0 for the others. */
uint32_t bytesPerPixel(uint32_t format)
{
	switch (format) {
	case 10:            /* R8Unorm */
		return 1;
	case 25:            /* R16Float */
	case 30:            /* RG8Unorm */
		return 2;
	case 55:            /* R32Float */
	case 65:            /* RG16Float */
	case 70:            /* RGBA8Unorm */
	case 71:            /* RGBA8Unorm_sRGB */
	case 80:            /* BGRA8Unorm */
	case 81:            /* BGRA8Unorm_sRGB */
	case 90:            /* RGB10A2Unorm */
	case 94:            /* BGR10A2Unorm */
		return 4;
	case 115:           /* RGBA16Float */
		return 8;
	case 125:           /* RGBA32Float */
		return 16;
	default:
		return 0;
	}
}

/*
 * copyFromBuffer:...toTexture: {u32 buffer, u32 texture, u64 source offset,
 * u64 source bytes per row, u64 source bytes per image, u64 size[3], u64
 * destination origin[3], u64 destination slice} (measured, card: CoreDisplay
 * filling the slices of a 1D array, level 0). A slice of the texture is
 * bytesPerRow x height bytes.
 */
bool copyBufferToTexture(uint32_t task, const uint8_t *p, uint32_t size)
{
	PvExec::Texture t;
	uint64_t buf, bufLen;
	uint8_t *row;
	bool ok = true;

	if (size < 88 || !PvExec::resolveBuffer(task, rd32(p), buf, bufLen) ||
	    !PvExec::resolveTexture(task, rd32(p + 4), t))
		return false;

	const uint64_t off = rd64(p + 8), pitch = rd64(p + 16), image = rd64(p + 24);
	const uint64_t w = rd64(p + 32), h = rd64(p + 40), d = rd64(p + 48);
	const uint64_t x = rd64(p + 56), y = rd64(p + 64), z = rd64(p + 72), slice = rd64(p + 80);
	const uint64_t bpp = bytesPerPixel(t.format), rowBytes = bpp * w;
	const uint64_t sliceBytes = t.bytesPerRow * t.height;

	if (!bpp || !w || !h || !d || x + w > t.width || y + h > t.height || rowBytes > kChunk ||
	    !inside(off, (d - 1) * image + (h - 1) * pitch + rowBytes, bufLen) ||
	    !inside((slice + z + d - 1) * sliceBytes + (y + h - 1) * t.bytesPerRow + bpp * x, rowBytes, t.bytes))
		return false;

	row = static_cast<uint8_t *>(IOMalloc(kChunk));
	if (!row)
		return false;

	for (uint64_t k = 0; ok && k < d; k++) {
		for (uint64_t i = 0; ok && i < h; i++) {
			const uint64_t dst = (slice + z + k) * sliceBytes + (y + i) * t.bytesPerRow + bpp * x;

			ok = PvExec::readGpu(task, buf + off + k * image + i * pitch, row, rowBytes) &&
			     PvExec::writeGpu(task, t.va + dst, row, rowBytes);
		}
	}

	IOFree(row, kChunk);

	return ok;
}

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

/* A texture in the client's memory or an IOSurface, for the job. */
bool describeTexture(uint32_t task, uint32_t ref, RDNA4PvTexture &t)
{
	PvExec::Texture tex;
	PvExec::SurfaceTexture st;

	if (PvExec::resolveTexture(task, ref, tex)) {
		t.va = tex.va;
		t.pitch = static_cast<uint32_t>(tex.bytesPerRow);
		t.width = tex.width;
		t.height = tex.height;
		t.format = tex.format;

		/* a 1D array: one row a slice, which is how rdna4raster.c samples it */
		if (tex.height == 1 && tex.layers > 1 && tex.bytesPerRow * tex.layers <= tex.bytes)
			t.height = tex.layers;

		/* a 3D texture: its slices one below the other, the depth beside the format */
		if (tex.depth > 1 && tex.depth <= 0xffff && tex.bytesPerRow * tex.height * tex.depth <= tex.bytes) {
			t.height = tex.height * tex.depth;
			t.format |= tex.depth << 16;
		}

		return true;
	}

	if (!PvExec::resolveSurfaceTexture(task, ref, st))
		return false;

	t.surface = st.surface;
	t.pitch = PvSurface::bytesPerRow(st.surface);
	t.width = st.width;
	t.height = st.height;
	t.format = st.format;

	return t.pitch != 0;
}

/* The fragment textures and samplers bound by slot, into the job. */
bool bindTextures(uint32_t task, const Render &r, RDNA4PvJob &job)
{
	for (uint32_t slot = 0; slot < RDNA4_PVJOB_MAX_TEXTURES; slot++) {
		if (r.textures.bound & (1u << slot)) {
			RDNA4PvTexture &t = job.textures[job.textureCount++];

			t.slot = slot;
			if (!describeTexture(task, r.textures.ref[slot], t)) {
				if (gResourceLogs++ < kRefusalsLogged)
					PvExec::logRef(task, r.textures.ref[slot], "fragment texture not understood");
				return false;
			}
		}

		if (r.samplers.bound & (1u << slot)) {
			RDNA4PvSampler &sm = job.samplers[job.samplerCount++];

			sm.slot = slot;
			if (!PvExec::resolveSampler(task, r.samplers.ref[slot], sm.word)) {
				if (gResourceLogs++ < kRefusalsLogged)
					PvExec::logRef(task, r.samplers.ref[slot], "fragment sampler not understood");
				return false;
			}
		}
	}

	return true;
}

/* The draw's render target, colour attachment 0: a texture in the client's memory or an IOSurface. */
bool drawTarget(uint32_t task, uint32_t ref, RDNA4PvJob &job)
{
	RDNA4PvTexture t {};

	if (!describeTexture(task, ref, t))
		return false;

	job.targetVa = t.va;
	job.targetSurface = t.surface;
	job.targetPitch = t.pitch;
	job.targetWidth = t.width;
	job.targetHeight = t.height;
	job.targetFormat = t.format;

	return true;
}

/* The index buffer of an indexed draw: a buffer ref, MTLIndexType (0 uint16, 1 uint32), the offset of the first index. */
struct Indices {
	uint32_t ref;
	uint32_t type;
	uint64_t offset;
};

/* The draw's `count` indices as the job's index buffer. */
bool bindIndices(uint32_t task, const Indices &ix, uint64_t count, RDNA4PvJob &job)
{
	RDNA4PvBuffer &b = job.buffers[job.bufferCount];
	const uint64_t bytes = count * (ix.type ? 4 : 2);
	uint64_t va, len;

	if (ix.type > 1 || job.bufferCount == RDNA4_PVJOB_MAX_BUFFERS || !PvExec::resolveBuffer(task, ix.ref, va, len) ||
	    ix.offset > len || bytes > len - ix.offset)
		return false;

	b.slot = 0;
	b.stage = RDNA4_PVJOB_INDEX;
	b.va = va + ix.offset;
	b.length = bytes;
	job.bufferCount++;
	job.indexType = ix.type ? RDNA4_PVJOB_UINT32 : RDNA4_PVJOB_UINT16;

	return true;
}

/*
 * A draw of `count` vertices from `start` (or of the vertices `ix` names),
 * run by the compiler service with the encoder's pipeline, buffers,
 * viewport and scissor into colour attachment 0 (the whole target without a
 * viewport or scissor).
 */
/* Why a draw was not run, for the first few (the guest only sees a command buffer that completed). */
bool refuse(uint32_t task, const Render &r, const char *why)
{
	static uint32_t logged;

	if (logged++ < kRefusalsLogged)
		RLOG("task %u: draw with pipeline %u into %u not run: %s", task, r.pipeline, r.target, why);

	return false;
}

bool draw(uint32_t task, const Render &r, uint32_t topology, uint64_t start, uint64_t count, uint64_t instances,
	  const Indices *ix = nullptr)
{
	RDNA4PvJob job {};
	PvExec::RenderPipeline pl;

	if (!r.havePipeline || !r.target)
		return refuse(task, r, "no pipeline or no target");

	if (!count || !instances || start + count > 0xffffffffull)
		return refuse(task, r, "no vertices, or too many");

	if (!PvExec::resolveRenderPipeline(task, r.pipeline, pl))
		return refuse(task, r, "the pipeline object is not understood");

	if (pl.vertexBytes > RDNA4_PVJOB_MAX_CODE || pl.fragmentBytes > RDNA4_PVJOB_MAX_CODE)
		return refuse(task, r, "a function too large");

	if (!drawTarget(task, r.target, job))
		return refuse(task, r, "the target is no texture or IOSurface known");

	job.kind = RDNA4_PVJOB_DRAW;
	job.task = task;
	job.codeVa = pl.vertexVa;
	job.codeBytes = pl.vertexBytes;
	job.fragmentVa = pl.fragmentVa;
	job.fragmentBytes = pl.fragmentBytes;
	job.topology = topology;
	job.vertexStart = static_cast<uint32_t>(start);
	job.vertexCount = static_cast<uint32_t>(count);
	job.instanceCount = static_cast<uint32_t>(instances);
	job.blend = pl.blend;
	job.attributeMask = pl.attributeMask;
	memcpy(job.attributes, pl.attributes, sizeof(job.attributes));
	memcpy(job.layouts, pl.layouts, sizeof(job.layouts));

	if (r.haveViewport) {
		memcpy(job.viewport, r.viewport, sizeof(job.viewport));
	} else {
		job.viewport[2] = floatBits(job.targetWidth);
		job.viewport[3] = floatBits(job.targetHeight);
	}

	if (r.haveScissor) {
		memcpy(job.scissor, r.scissor, sizeof(job.scissor));
	} else {
		job.scissor[2] = job.targetWidth;
		job.scissor[3] = job.targetHeight;
	}

	if (!bindBuffers(task, r.vertex, RDNA4_PVJOB_VERTEX, job) || !bindBuffers(task, r.fragment, RDNA4_PVJOB_FRAGMENT, job))
		return refuse(task, r, "a bound buffer is not understood, or there are too many");

	if (!bindTextures(task, r, job))
		return refuse(task, r, "a texture or sampler is not understood");

	if (ix && !bindIndices(task, *ix, count, job))
		return refuse(task, r, "the indices are not understood");

	return PvJob::run(job, kDispatchTimeoutMs);
}

/*
 * drawIndexedPrimitives: {u16 type, u16 MTLIndexType, u32 index buffer ref}
 * and then 0x006 {u64 count, u64 offset}, 0x007 {u16 count, u16 offset},
 * 0x009 {u16 count, u16 offset, u32 instances} (measured, card).
 */
bool drawIndexed(uint32_t task, const Render &r, uint32_t id, const uint8_t *p, uint32_t size)
{
	auto u16 = [&](uint32_t off) { return static_cast<uint32_t>(p[off] | p[off + 1] << 8); };
	Indices ix { size >= 8 ? rd32(p + 4) : 0, size >= 8 ? u16(2) : 0, 0 };

	switch (id) {
	case PvRun::kDrawIndexed:
		ix.offset = size >= 24 ? rd64(p + 16) : 0;
		return size >= 24 && draw(task, r, u16(0), 0, rd64(p + 8), 1, &ix);
	case PvRun::kDrawIndexedShort:
		ix.offset = size >= 12 ? u16(10) : 0;
		return size >= 12 && draw(task, r, u16(0), 0, u16(8), 1, &ix);
	case PvRun::kDrawIndexedInstancedShort:
		ix.offset = size >= 16 ? u16(10) : 0;
		return size >= 16 && draw(task, r, u16(0), 0, u16(8), rd32(p + 12), &ix);
	default:
		return false;
	}
}

/*
 * drawPrimitives: 0x000 {u32 type, u64 start, u64 count}, 0x001 {u32 type, u16
 * start, u16 count}, 0x002 {u64 start, count, instances, u16 type}, 0x003
 * {u16 start, count, instances, type} (docs/m1-opcodes.md).
 */
bool drawPrimitives(uint32_t task, const Render &r, uint32_t id, const uint8_t *p, uint32_t size)
{
	auto u16 = [&](uint32_t off) { return static_cast<uint32_t>(p[off] | p[off + 1] << 8); };

	switch (id) {
	case PvRun::kDraw:
		return size >= 20 && draw(task, r, rd32(p), rd64(p + 4), rd64(p + 12), 1);
	case PvRun::kDrawShort:
		return size >= 8 && draw(task, r, rd32(p), u16(4), u16(6), 1);
	case PvRun::kDrawInstanced:
		return size >= 26 && draw(task, r, u16(24), rd64(p), rd64(p + 8), rd64(p + 16));
	case PvRun::kDrawInstancedShort:
		return size >= 8 && draw(task, r, u16(6), u16(0), u16(2), u16(4));
	default:
		return false;
	}
}

/* One render command: true when it was run. */
bool runRender(uint32_t task, Render &r, uint32_t id, const uint8_t *p, uint32_t size, Stats &s)
{
	bool ok;

	switch (id) {
	case PvRun::kRenderPass:
		ok = renderPass(task, r, p, size);
		break;
	case PvRun::kSetRenderPipeline:
		ok = size >= 4;
		r.pipeline = ok ? rd32(p) : 0;
		r.havePipeline = ok;
		break;
	case PvRun::kSetVertexBuffers:
		ok = setBuffers(r.vertex, p, size);
		break;
	case PvRun::kSetFragmentBuffers:
		ok = setBuffers(r.fragment, p, size);
		break;
	case PvRun::kSetFragmentTextures:
		ok = setRefs(r.textures, p, size);
		break;
	case PvRun::kSetViewport:
		ok = setViewport(r, p, size);
		break;
	case PvRun::kSetScissor:
		ok = setScissor(r, p, size);
		break;
	case PvRun::kSetFragmentSamplers:
		ok = setRefs(r.samplers, p, size);
		break;
	case PvRun::kSetVertexBufferOffset:
		ok = setBufferOffset(r.vertex, p, size);
		break;
	case PvRun::kSetFragmentBufferOffset:
		ok = setBufferOffset(r.fragment, p, size);
		break;
	case PvRun::kSetCullMode:
	case PvRun::kSetFrontFacing:
	case PvRun::kSetFillMode:
	case PvRun::kSetState17:
		ok = size >= 4;   /* the rasterizer draws both faces, filled */
		break;
	case PvRun::kDrawIndexed:
	case PvRun::kDrawIndexedShort:
	case PvRun::kDrawIndexedInstancedShort:
		ok = drawIndexed(task, r, id, p, size);
		break;
	case PvRun::kDraw:
	case PvRun::kDrawShort:
	case PvRun::kDrawInstanced:
	case PvRun::kDrawInstancedShort:
		ok = drawPrimitives(task, r, id, p, size);
		break;
	default:
		return false;
	}

	if (ok)
		s.done++;
	else if (!s.failed++)
		s.firstFailed = id;

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
	case PvRun::kCopyBufferToTexture:
		ok = copyBufferToTexture(task, p, size);
		break;
	default:
		return false;
	}

	if (ok)
		s.done++;
	else if (!s.failed++)
		s.firstFailed = id;

	return true;
}

/* One command of a segment: true when it was run (or failed running). */
bool runCommand(uint32_t task, uint8_t type, Encoder &e, uint32_t id, const uint8_t *p, uint32_t size, Stats &s)
{
	if (type == PvRun::kBlit)
		return runBlit(task, id, p, size, s);

	if (type == PvRun::kCompute)
		return runCompute(task, e.compute, id, p, size, s);

	if (type == PvRun::kRender)
		return runRender(task, e.render, id, p, size, s);

	return false;
}

/* The commands of one segment; an encoder starts with nothing bound. */
void runSegment(uint32_t task, uint8_t type, const uint8_t *p, uint32_t len, Stats &s)
{
	auto *e = static_cast<Encoder *>(IOMallocZero(sizeof(Encoder)));
	uint32_t at = 0;

	if (!e)
		return;


	while (at + kCommandHeader <= len) {
		const uint32_t id = rd32(p + at);
		const uint32_t size = rd32(p + at + 4);

		if (size < kCommandHeader || size > len - at)
			break;

		if (!runCommand(task, type, *e, id, p + at + kCommandHeader, size - kCommandHeader, s)) {
			if (!s.skipped++)
				s.firstSkipped = id;
		}

		at += size;
	}

	IOFree(e, sizeof(Encoder));
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
	sysctl_register_oid(&sysctl__debug_rdna4_pvdump_task);
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

	if ((s.skipped || s.failed) && gDumps > 0 && (!gDumpTask || task == static_cast<uint32_t>(gDumpTask))) {
		gDumps--;
		dumpBuffer(task, buf, static_cast<uint32_t>(len));
	}

	IOFree(buf, len);

	if (gRuns++ < 32 || s.failed)
		RLOG("task %u, %llu bytes: %u run, %u failed (first id 0x%x), %u not run yet (first id 0x%x)", task,
		     len, s.done, s.failed, s.firstFailed, s.skipped, s.firstSkipped);
}
