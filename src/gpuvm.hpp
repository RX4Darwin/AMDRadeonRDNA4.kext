//
//  gpuvm.hpp
//  RDNA4FB
//
//  The small, freestanding part of the gfx12 GPUVM implementation.  The
//  runtime owns the page-table storage and calls these routines while holding
//  its lock; keeping the encoders here also lets the host test exercise the
//  exact bit layout without a kext build.
//

#ifndef RDNA4GpuVm_hpp
#define RDNA4GpuVm_hpp

#include <stdint.h>

namespace GpuVm {

constexpr uint32_t kPageShift = 12;
constexpr uint64_t kPageBytes = 1ull << kPageShift;
constexpr uint32_t kIndexBits = 9;
constexpr uint32_t kEntries = 1u << kIndexBits;
constexpr uint32_t kLevels = 4;          // PDB2 -> PDB1 -> PDB0 -> PTB
constexpr uint32_t kDepth = 3;           // VM_CONTEXT PAGE_TABLE_DEPTH
constexpr uint32_t kBlockSize = 9;       // 512 entries per directory
constexpr uint32_t kFragment64K = 4;     // log2(64 KiB / 4 KiB)
constexpr uint64_t kVaStart = 0x0000000100000000ull;
constexpr uint64_t kVaBits = 48;
constexpr uint64_t kVaEnd = 1ull << kVaBits;
constexpr uint64_t kPhysicalMask = 0x0000FFFFFFFFF000ull;

/* AMDGPU_PTE_* and AMDGPU_PDE_* flags used by gfx_v12_0. */
constexpr uint64_t kValid      = 1ull << 0;
constexpr uint64_t kSystem     = 1ull << 1;
constexpr uint64_t kSnooped    = 1ull << 2;
constexpr uint64_t kExecutable = 1ull << 4;
constexpr uint64_t kReadable   = 1ull << 5;
constexpr uint64_t kWritable   = 1ull << 6;
constexpr uint64_t kFragMask   = 0x1full << 7;
constexpr uint64_t kMtypeMask  = 3ull << 54;  // AMDGPU_PTE_MTYPE_GFX12
constexpr uint64_t kPdeBfsMask = 0x1full << 58;
constexpr uint64_t kPdePte     = 1ull << 63;  // AMDGPU_PDE_PTE_GFX12

/* Flags are intentionally explicit at call sites: no hidden permission policy. */
uint64_t encodePte(uint64_t physical, uint64_t flags, bool fragment64K);
uint64_t encodePde(uint64_t physical, uint64_t flags, uint32_t level);

uint32_t index(uint64_t va, uint32_t level);
uint64_t entryPhysical(uint64_t entry);

using ReadEntry = bool (*)(void *context, uint64_t address, uint64_t &entry);

/* Walk a gfx12 page table.  `physical` is the page address plus VA offset. */
bool walk(uint64_t root, uint64_t va, ReadEntry read, void *context,
          uint64_t &physical, uint64_t &flags);

} // namespace GpuVm

#endif /* RDNA4GpuVm_hpp */
