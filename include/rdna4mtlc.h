/*
 *  rdna4mtlc.h — the Metal kernel translator's door into the kext (M3, docs/m1-stream.md s.5b)
 *
 *  The fake paravirtual GPU's host (src/pvgpu.cpp, make PVGPU=1) runs Metal compute functions it has no code for by asking a root helper
 *  (tools/metal/m3/air2amdgpu.cpp --serve) to translate them. The helper opens a user client of type RDNA4_MTLC_TYPE on RDNA4ComputeService
 *  (one helper at a time) and loops:
 *
 *    kMtlcNext     scalars in {buffer address, buffer bytes}; out {request id, metallib bytes}; structure out: char name[64].
 *                  Blocks until the host has a function to translate; the guest's whole metallib is copied into the buffer.
 *    kMtlcDeliver  scalars in {request id, code object address, code object bytes (0 = could not translate), buffers, written mask
 *                  [, textures written mask]}: the linked gfx1201 code object and its interface as air2amdgpu describes it (8-byte buffer
 *                  addresses for indices 0 .. buffers - 1, then u32 threads per threadgroup[3], u32 threads in the grid[3]); bit i of the
 *                  written mask: buffer index i is device memory the function may write (a vertex or fragment shader's too, which the
 *                  host then maps only where the guest lets the GPU write); bit i of the textures mask: texture index i is written (an older
 *                  helper sends five scalars: none known).
 */
#pragma once

#define RDNA4_MTLC_TYPE 0x4d544c43u   /* 'MTLC' */
#define RDNA4_MTLC_MAX_LIB (1u << 20)
#define RDNA4_MTLC_MAX_ELF (1u << 20)

enum { kMtlcNext = 0, kMtlcDeliver = 1 };

/* A vertex or fragment shader's table (the host fills it; the shader finds its address in two user SGPRs): the buffers' 64-bit addresses at
 * +0 (indices 0-7), texture n's T# (8 dwords) at RDNA4_MTLC_TABLE_TEXTURES + 32 n, sampler n's S# (4 dwords) at RDNA4_MTLC_TABLE_SAMPLERS
 * + 16 n (n 0-15). */
#define RDNA4_MTLC_TABLE_TEXTURES 64u
#define RDNA4_MTLC_TABLE_SAMPLERS 576u
/* The fragment stage's colour attachment 0 as the draw found it, for shaders that read it (Metal's [[color(0)]] input): a T# of the host's
 * staged RGBA8 copy of the target. */
#define RDNA4_MTLC_TABLE_FRAMEBUFFER 832u
/* Buffers 8-27's addresses (indices 0-7 are at +0). */
#define RDNA4_MTLC_TABLE_MORE_BUFFERS 864u
#define RDNA4_MTLC_MAX_BUFFERS 28u
/* The vertex stage's vertex descriptor: attribute n (0-15) at RDNA4_MTLC_TABLE_ATTRIBUTES + 16 n, {u32 offset in the element, u32 stride,
 * u32 buffer index | kind << 8 | components << 12 | BGRA << 15 | step function << 16 (MTLVertexStepFunction), u32 step rate}; kind:
 * 0 float, 1 half, 2 unorm8, 3 snorm8, 4 uint8, 5 sint8, 6 unorm16, 7 snorm16, 8 uint16, 9 sint16, 10 int, 11 uint (the host's
 * vertexFormat). Metal keeps offsets and strides multiples of 4. */
#define RDNA4_MTLC_TABLE_ATTRIBUTES 0x800u
/* The fragment stage's u32 colour attachment the draw goes into, for a shader that writes several (its RGFX header's bits 20-27): the value
 * of that attachment is the one exported. */
#define RDNA4_MTLC_TABLE_ATTACHMENT 0x500u
#define RDNA4_MTLC_MAX_ATTRIBUTES 16u
/* A shader that samples at an explicit LOD (its RGFX header's bit 16): level L (1-7) of texture n's T# at RDNA4_MTLC_LEVEL_FIRST
 * + RDNA4_MTLC_LEVEL_STRIDE (L - 1) past its level 0 T#, in both stages (the host's state page: the vertex stage's table at +0x400, the
 * fragment stage's at +0x800, level L's T#s from +0x1000 + RDNA4_MTLC_LEVEL_STRIDE (L - 1), the vertex stage's then the fragment stage's
 * 0x400 on). Each a level-0 image of that level's memory: the guest's levels are largest first, the hardware's linear ones smallest
 * first, so one T# cannot hold them. Past a texture's last level, its last. */
#define RDNA4_MTLC_LEVEL_FIRST 0xbc0u
#define RDNA4_MTLC_LEVEL_STRIDE 0x800u
#define RDNA4_MTLC_LEVELS 8u
