/*
 * pvexec.hpp
 * AMDRadeonRDNA4
 *
 * The host's view of a paravirt guest's memory (docs/m1-stream.md s.5):
 * its tasks, each with a GPU page table the guest builds in its own RAM
 * and a resource heap, and what an ExecIndirect asks to run. For now it
 * reads and logs; executing the work comes on top of it.
 *
 * The page table (AppleParavirtPageTable, measured in Sequoia 24H232):
 * levels of 4 KiB nodes holding 1024 u32 entries each, the GPU address's
 * page number split 10 bits per level, top level first. An interior entry
 * is the page number of the next node; a leaf entry is the page number of
 * the data page, bit 31 a flag the guest sets per mapping. The root's page
 * number comes with DefineHostTask. A task of 16 GiB has four levels on
 * the card (walked, Sequoia), one more than the space needs and than
 * AppleParavirtTask::init passes to PageTable::init (3).
 */

#ifndef PvExec_hpp
#define PvExec_hpp

#include <stddef.h>
#include <stdint.h>

#include "rdna4pvjob.h"

namespace PvExec
{

constexpr uint32_t kMaxTasks = 64;
constexpr uint32_t kPageTableLevels = 4;
constexpr uint32_t kEntryPageMask = 0x7fffffff;

/* Resource heap entry kinds (the low byte of an entry's first word). */
constexpr uint32_t kResourceBuffer = 1;
constexpr uint32_t kResourceTexture = 2;    /* 116 bytes, see Texture */
constexpr uint32_t kResourceTexture3 = 3;   /* the same descriptor (WindowServer's layer textures) */
constexpr uint32_t kResourceSurface = 5;    /* an IOSurface's texture, see SurfaceTexture */
constexpr uint32_t kResourceFunction = 6;    /* {u64 code GPU address, u32 length, ...} */
constexpr uint32_t kResourceObject = 7;      /* createObject: {u32 type, u32 size, ...} */
constexpr uint32_t kObjectComputePipeline = 0xb;
constexpr uint32_t kObjectRenderPipeline = 0xe;
constexpr uint32_t kObjectSampler = 0x3;

/* Index into a node at `level` (0 = leaf) for a GPU address. */
inline uint32_t indexAt(uint64_t va, uint32_t level)
{
	return static_cast<uint32_t>((va >> (12 + 10 * level)) & 0x3ff);
}

#ifdef KERNEL
/* FIFO commands the host passes on: payload bytes after the header. */
void defineTask(const uint8_t *payload, uint32_t len);
void freeTask(const uint8_t *payload, uint32_t len);
void setHeap(const uint8_t *payload, uint32_t len);
void execIndirect(const uint8_t *payload, uint32_t len, uint32_t channel);
void createComputePipeline(const uint8_t *payload, uint32_t len);

/* Guest memory, by physical address or by a task's GPU address. */
bool accessPhys(uint64_t pa, void *buf, size_t len, bool write);
bool accessGpu(uint32_t task, uint64_t va, void *buf, size_t len, bool write);
bool readPhys(uint64_t pa, void *out, size_t len);
bool readGpu(uint32_t task, uint64_t va, void *out, size_t len);
bool writeGpu(uint32_t task, uint64_t va, const void *in, size_t len);

/* A buffer's GPU address and length by the ref the stream uses. */
bool resolveBuffer(uint32_t task, uint32_t ref, uint64_t &va, uint64_t &len);

/* Log every used entry of the task's resource heap and, with `code`, each function's code. */
void logHeap(uint32_t task, bool code);

/*
 * A texture by its ref (kind 2): the guest lays it out linearly, rows
 * `bytesPerRow` apart, in its own memory (measured, card: a private
 * 64x64 BGRA8 texture, {u64 bytes, u32 page, ..., u64 bytes per row at
 * 0x34, u32 width, height, depth at 0x3c, u16 MTLPixelFormat at 0x56}).
 */
struct Texture {
	uint64_t va;
	uint64_t bytes;
	uint64_t bytesPerRow;
	uint32_t width, height, depth;
	uint32_t layers;     /* the array length (1 without) */
	uint32_t format;
};

bool resolveTexture(uint32_t task, uint32_t ref, Texture &out);

/* Heap entry `ref` of `task` into the log: its kind, size and first bytes (why a resource was not understood). */
void logRef(uint32_t task, uint32_t ref, const char *what);

/*
 * A texture on an IOSurface by its ref (kind 5, 56 bytes): a CAMetalLayer's
 * drawable. {u64 IOSurface ID, ..., u16 MTLPixelFormat at 0x16, u32 width,
 * height at 0x18} (measured, card: a 960x640 BGRA8 layer).
 */
struct SurfaceTexture {
	uint32_t surface;
	uint32_t width, height;
	uint32_t format;
};

bool resolveSurfaceTexture(uint32_t task, uint32_t ref, SurfaceTexture &out);

/* A sampler's packed state by its ref (kind 7, type 3: the word at 0xc). */
bool resolveSampler(uint32_t task, uint32_t ref, uint32_t &word);

/* A compute pipeline's function by the pipeline's ref: its metallib. */
bool resolveComputePipeline(uint32_t task, uint32_t ref, uint64_t &codeVa, uint32_t &codeBytes);

/* A render pipeline's vertex and fragment functions by the pipeline's ref: their metallibs. */
struct RenderPipeline {
	uint64_t vertexVa, fragmentVa;
	uint32_t vertexBytes, fragmentBytes;
	uint32_t blend;     /* colour attachment 0, rdna4raster.c's blend word */
	uint32_t attributeMask;
	RDNA4PvAttribute attributes[RDNA4_PVJOB_MAX_ATTRIBUTES];
	RDNA4PvLayout layouts[RDNA4_PVJOB_MAX_ATTRIBUTES];
};

bool resolveRenderPipeline(uint32_t task, uint32_t ref, RenderPipeline &out);
#endif

} /* namespace PvExec */

#endif /* PvExec_hpp */
