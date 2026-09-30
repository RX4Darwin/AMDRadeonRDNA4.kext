// ngg64.s — ngg.s as a wave64 NGG shader, the way RADV runs its NGG (GS) stage for this triangle (GS_W32_EN = 0;
// docs/linux-radv-triangle.md). RADV also runs the PS as wave64; the draw ladder's variant 4096 keeps the PS wave32.
// The wave64 fallback of the draw ladder (rdna4-gfxdiag, bit 4096): if the wave32 baseline draws nothing on macOS, this
// tells whether the GS wave size matters there. Proven on the card under Linux first (tools/linux-replay, REPLAY_VARIANT=4096).
//
// Same ABI as ngg.s: s3 = merged_wave_info ([7:0] vertices, [15:8] primitives), v0 = packed primitive,
// v3 = VertexID; EXEC starts as lane 0 only and is SET from merged_wave_info. wave64: EXEC and the VALU
// compare masks are 64-bit SGPR pairs. Both exec masks are built the way ACO does, correct for up to 64 lanes:
// s_bfe_u64 exec, -1, <width at [22:16], offset 0 at [5:0]>; a plain s_bfm_b64 has a 6-bit width and would give 0 for 64.
// Assembled with -mattr=+wavefrontsize64 (tools/build-shaders.sh picks that from the "64.s" suffix). Needs 8 VGPRs: the
// kext sets SPI_SHADER_PGM_RSRC1_GS.VGPRS = 2 (wave64 allocates VGPRs in granules of 4; 12 like RADV; 1 = 8 would do too).

.text
.globl ngg64
ngg64:
	// position export: lanes [0, vertices)
	s_pack_ll_b32_b16  s8, 0, s3                // s8 = merged_wave_info[15:0] << 16: vertices in the width field [22:16]
	s_bfe_u64          exec, -1, s8             // exec = the low <vertices> lanes (up to 64)
	s_cbranch_execz    skip_pos
	v_cmp_eq_u32_e64   s[20:21], 0, v3
	v_cmp_eq_u32_e64   s[22:23], 1, v3
	v_cmp_eq_u32_e64   s[24:25], 2, v3
	v_cndmask_b32_e64  v4, 0, 0.5, s[22:23]     // x = vid == 1 ? 0.5 : 0
	v_cndmask_b32_e64  v4, v4, -0.5, s[20:21]   // x = vid == 0 ? -0.5 : x
	v_cndmask_b32_e64  v5, -0.5, 0.5, s[24:25]  // y = vid == 2 ? 0.5 : -0.5
	v_mov_b32          v6, 0                    // z
	v_mov_b32          v7, 1.0                  // w
	export             pos0 v4, v5, v6, v7 done
skip_pos:

	// primitive export: lanes [0, primitives), after the positions like ACO
	s_wait_expcnt      0x0
	s_lshl_b32         s9, s3, 8                // s9 = merged_wave_info << 8: primitives (bits 15:8) in the width field [22:16]
	s_bfe_u64          exec, -1, s9             // exec = the low <primitives> lanes (up to 64)
	s_cbranch_execz    done_vs
	export             prim v0, off, off, off done
done_vs:
	s_endpgm
