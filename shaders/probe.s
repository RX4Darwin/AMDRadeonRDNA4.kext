// probe.s — the stage-6 test kernel for gfx1201 (RDNA 4), wave32.
//
// Every work-item i writes 0x5EED0000 + 3*i to buffer[i]. Launch state:
//   s[0:1]  buffer address         (COMPUTE_USER_DATA_0/1, 2 user SGPRs)
//   ttmp9   work-group id x        (TGID_X_EN; GFX12 delivers work-group ids in
//                                  ttmp9/ttmp7, not in the SGPRs after the user
//                                  ones: architected SGPRs, as clang assumes)
//   v0      work-item id x         (TIDIG_COMP_CNT = 0; y = z = 0)
// with 64 work-items per group. Assembled by tools/build-shaders.sh
// (llvm-mc) into src/probe_kernel.h.

	.amdgcn_target "amdgcn-amd-amdhsa--gfx1201"
	.text
	.globl	rdna4_probe
rdna4_probe:
	s_lshl_b32	s3, ttmp9, 6			// group base = group id * 64
	v_add_nc_u32	v1, s3, v0			// global id
	v_lshlrev_b32	v2, 2, v1			// byte offset
	v_mul_u32_u24	v3, 3, v1			// 3 * id
	v_add_nc_u32	v3, 0x5eed0000, v3		// + magic
	global_store_b32	v2, v3, s[0:1]		// buffer[id]
	s_endpgm
