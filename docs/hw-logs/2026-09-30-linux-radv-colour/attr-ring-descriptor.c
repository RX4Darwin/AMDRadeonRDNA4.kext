/* The gfx12 attribute ring buffer descriptor RADV builds (radv_queue.c:336-339 radv_fill_shader_rings -> ac_build_attr_ring_descriptor), linked
 * against Mesa 26.2.2's libamd_common.a. has_desc_resource_level = false on gfx1201 (ac_gpu_info.c:434-435). The VS ORs S_008F04_STRIDE(16 * params)
 * into dword1 (radv_nir_lower_abi.c:109-118). util_format stubs: the descriptor path for gfx12 does not use them.
 * Build: see docs/g4-colour.md. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "amd/common/ac_descriptors.h"
#include "amd/common/amd_family.h"
#include "amd/common/sid.h"
int main(void) {
	uint32_t d[4];
	ac_build_attr_ring_descriptor(GFX12, false, 0x0000800100200000ull, 0xA80000, 0, d);
	printf("attr ring descriptor, va 0x800100200000 size 0xA80000 stride 0: %08x %08x %08x %08x\n", d[0], d[1], d[2], d[3]);
	printf("after the VS's stride OR (16 << 16): dword1 %08x\n", d[1] | S_008F04_STRIDE(16));
	ac_build_attr_ring_descriptor(GFX12, false, 0, 0, 0, d);
	printf("constant parts (va 0, size 0): dword1 %08x dword3 %08x\n", d[1], d[3]);
	return 0;
}
const void *util_format_description(int f) { (void)f; return 0; }
int util_format_is_intensity(int f) { (void)f; return 0; }
int util_format_is_pure_integer(int f) { (void)f; return 0; }
