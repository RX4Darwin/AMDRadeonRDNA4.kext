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

namespace PvExec
{

constexpr uint32_t kMaxTasks = 64;
constexpr uint32_t kPageTableLevels = 4;
constexpr uint32_t kEntryPageMask = 0x7fffffff;

/* Resource heap entry kinds (the low byte of an entry's first word). */
constexpr uint32_t kResourceBuffer = 1;

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
#endif

} /* namespace PvExec */

#endif /* PvExec_hpp */
