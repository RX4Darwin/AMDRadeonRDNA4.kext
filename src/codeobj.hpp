//
//  codeobj.hpp
//  RDNA4FB
//
//  Freestanding reader for AMDGPU code objects (the ELF64 files clang/lld
//  produce for -target amdgcn-amd-amdhsa -mcpu=gfx1201): finds a kernel's
//  symbol and its 64-byte kernel descriptor ("<name>.kd"), and exposes what
//  a dispatch needs — the code, the COMPUTE_PGM_RSRC1/2/3 values, the
//  kernarg size and which launch SGPRs the kernel expects.
//
//  Layouts per LLVM's AMDGPUUsage ("Kernel Descriptor") and the ELF64 spec.
//  No allocation: views into the caller's buffer, bounds-checked.
//

#ifndef CodeObj_hpp
#define CodeObj_hpp

#include <stddef.h>
#include <stdint.h>

namespace CodeObj {

struct Kernel {
	// Where the code lives, as an offset into the ELF image and a length
	// (to the end of its section): copy [codeOffset, codeOffset + codeSize)
	// to a 256-byte-aligned GPU address and point COMPUTE_PGM_LO/HI at it.
	uint32_t codeOffset;
	uint32_t codeSize;
	// The code's virtual address in the image (see Image): load the image
	// at a 256-byte-aligned base and the code is at base + entryVa.
	uint64_t entryVa;

	uint32_t groupSegmentSize;     // LDS bytes
	uint32_t privateSegmentSize;   // scratch bytes per work-item
	uint32_t kernargSize;
	uint32_t rsrc1, rsrc2, rsrc3;
	uint16_t properties;           // kernel_code_properties

	uint32_t userSgprCount() const { return (rsrc2 >> 1) & 0x1f; }
	bool wave32() const { return properties & (1u << 10); }
	bool wantsKernargPtr() const { return properties & (1u << 3); }
	bool wantsDispatchPtr() const { return properties & (1u << 1); }
};

// Look up `name` in `elf`. Returns false (and `why`) if the file is not a
// gfx12 AMDGPU code object or the kernel / its descriptor is missing.
bool findKernel(const uint8_t *elf, size_t size, const char *name, Kernel &out,
                const char **why);

// The loadable image: the PT_LOAD segments at their virtual addresses in
// one block of `size` bytes (ld.lld links code objects at 0), as ROCm's
// loader places them, so PC-relative references from a kernel to .rodata
// or to another function keep working. Bytes past a segment's file size
// are zero. Dynamic relocations are not applied: a file with any is
// refused.
struct Image {
	static constexpr uint32_t kMaxSegments = 8;
	static constexpr uint64_t kMaxSize = 16ull << 20;
	struct Segment { uint64_t fileOffset, fileSize, vaddr, memSize; };
	Segment  seg[kMaxSegments];
	uint32_t count;
	uint64_t size;
};
bool parseImage(const uint8_t *elf, size_t size, Image &out, const char **why);

} // namespace CodeObj

#endif /* CodeObj_hpp */
