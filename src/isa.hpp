//
//  isa.hpp
//  RDNA4FB
//
//  The few shader instruction words the kext writes by hand (everything else comes from shaders/*.s through tools/build-shaders.sh). gfx12 (RDNA4) shares
//  the SOPP encodings of gfx11, NOT gfx6-10's: s_endpgm is SOPP opcode 48, and the old 0xbf810000 is s_setkill on this ISA (hub-task-347: vmtest.cpp's T5c
//  kernel used it). tools/check-isa.sh (make check-isa, part of make test) assembles and disassembles these with llvm-mc -mcpu=gfx1201 and fails if a
//  constant here does not mean what its name says, and if any other raw 0xbf.. instruction literal shows up in src/*.cpp.
//

#ifndef RDNA4Isa_hpp
#define RDNA4Isa_hpp

#include <stdint.h>

namespace Isa {

constexpr uint32_t kSEndpgm  = 0xbfb00000u;   // s_endpgm
constexpr uint32_t kSCodeEnd = 0xbf9f0000u;   // s_code_end: padding after a kernel so the SQ's instruction prefetch never runs into unmapped memory

} // namespace Isa

#endif /* RDNA4Isa_hpp */
