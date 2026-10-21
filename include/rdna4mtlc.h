/*
 *  rdna4mtlc.h — the Metal kernel translator's door into the kext (M3, docs/m1-stream.md s.5b)
 *
 *  The fake paravirtual GPU's host (src/pvgpu.cpp, make PVGPU=1) runs Metal compute functions it has no code for by asking a root helper
 *  (tools/metal/m3/air2amdgpu.cpp --serve) to translate them. The helper opens a user client of type RDNA4_MTLC_TYPE on RDNA4ComputeService
 *  (one helper at a time) and loops:
 *
 *    kMtlcNext     scalars in {buffer address, buffer bytes}; out {request id, metallib bytes}; structure out: char name[64].
 *                  Blocks until the host has a function to translate; the guest's whole metallib is copied into the buffer.
 *    kMtlcDeliver  scalars in {request id, code object address, code object bytes (0 = could not translate), buffers, written mask}:
 *                  the linked gfx1201 code object and its interface as air2amdgpu describes it (8-byte buffer addresses for indices
 *                  0 .. buffers - 1, then u32 threads per threadgroup[3], u32 threads in the grid[3]).
 */
#pragma once

#define RDNA4_MTLC_TYPE 0x4d544c43u   /* 'MTLC' */
#define RDNA4_MTLC_MAX_LIB (1u << 20)
#define RDNA4_MTLC_MAX_ELF (1u << 20)

enum { kMtlcNext = 0, kMtlcDeliver = 1 };
