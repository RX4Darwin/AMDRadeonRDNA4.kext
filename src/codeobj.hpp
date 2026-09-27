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

} // namespace CodeObj

#endif /* CodeObj_hpp */
