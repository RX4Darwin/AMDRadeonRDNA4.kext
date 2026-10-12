// Host-only test (no GPU) of the ring packets the kernel wraps around a client's gfx IB (W12k, src/runtime.cpp gfxClientEmit):
//   c++ -std=c++17 -Wall -Wextra -I src userspace/test-w12k-pm4.cpp src/pm4.cpp -o /tmp/t && /tmp/t
// Checked against docs/w12k-gfx-submit.md (amdgpu's own wrapping, captured from the card) and what the emulator's W12e accepts
// (emu/qemu/rdna4.c: INDIRECT_BUFFER refuses VALID/CHAIN/PRE_ENB, takes the VMID from bits 27:24, PRIV from bit 31).
#include <stdio.h>

#include "pm4.hpp"

static int failures;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main() {
	uint32_t p[8];
	// amdgpu's user IB packet on the card, VMID 5, 0xbd0 dwords: control 0x05000bd0 (docs/w12k-gfx-submit.md #11): no VALID, no PRIV.
	EXPECT(Pm4::indirectBufferGfx(p, 0x100043000ull, 0xbd0, 5) == 4);
	EXPECT(p[0] == 0xc0023f00u && p[1] == 0x00043000u && p[2] == 1u && p[3] == 0x05000bd0u);
	// every client VMID (8..15), the largest IB (20-bit size): only the size and VMID fields are set
	for (uint32_t vmid = 8; vmid <= 15; vmid++) {
		Pm4::indirectBufferGfx(p, 0x100000000ull, 0xfffff, vmid);
		EXPECT(p[3] == (0xfffffu | (vmid << 24)));
		EXPECT(!(p[3] & ((1u << 20) | (1u << 21) | (1u << 23) | (1u << 31))));   // what the emulator refuses / treats as PRIV
	}
	// CONTEXT_CONTROL: the emulator's W12e accepts exactly count 1, first dword 0x80000000, second 0 or 0x80000000
	EXPECT(Pm4::contextControl(p, 0x80000000u, 0x80000000u) == 3);
	EXPECT(p[0] == 0xc0012800u && p[1] == 0x80000000u && p[2] == 0x80000000u);
	// the per-client fence: RELEASE_MEM to a dword-aligned address, 32-bit data, no interrupt
	EXPECT(Pm4::releaseMem(p, 0x100005040ull, 7) == 8);
	EXPECT(((p[0] >> 8) & 0xff) == 0x49 && p[3] == 0x00005040u && p[4] == 1u && p[5] == 7u);
	// a submission is 3 + 4 + 8 = 15 dwords
	EXPECT(3 + 4 + 8 == 15);
	printf(failures ? "%d FAILURES\n" : "all W12k ring packet host tests passed\n", failures);
	return failures != 0;
}
