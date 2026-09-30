// nggconst.s — ngg.s with NOTHING taken from the wave's VGPR inputs, the ladder variant that separates "the wave gets the wrong inputs"
// from "the pipeline drops the primitive" (rdna4-gfxdiag, bit 1024; W46). Round 6 on the card: C_INVOCATIONS 1, C_PRIMITIVES 0 even with
// CLIP_DISABLE and the primitive filters off, and the NGG marker proves the wave ran. ngg.s exports v0 (the packed primitive the HW puts in
// VGPR0 on gfx12: "the HW already packs the primitive export value", ac_nir_lower_intrinsics_to_args.c) and picks each vertex position from
// v3 (VertexID). If either input is not what those assumptions say, the three vertices coincide or the primitive is malformed and the clipper
// drops it. Here:
//   * the primitive is built from a constant: vertices 0,1,2 at a 9-bit stride (0x00080200), no edge flags;
//   * the vertex index is the LANE id (v_mbcnt_lo_u32_b32), not v3.
// Everything else (exports, positions) is ngg.s. It needs 11 VGPRs: the kext patches SPI_SHADER_PGM_RSRC1_GS.VGPRS to 1.

.text
.globl nggconst
nggconst:
	v_mbcnt_lo_u32_b32 v10, -1, 0               // lane id: the vertex index
	v_mov_b32          v9, 0x00080200           // primitive export: vertex indices 0, 1, 2 (9-bit stride), edge flags 0

	// primitive export: lanes [0, primitives)
	s_bfe_u32          s9, s3, 0x80008          // s9 = merged_wave_info[15:8]
	s_bfm_b64          s[12:13], s9, 0          // lane mask of the primitives
	s_and_saveexec_b32 s14, s12
	s_cbranch_execz    skip_prim
	export             prim v9, off, off, off done
skip_prim:
	s_mov_b32          exec_lo, s14

	// position export: lanes [0, vertices)
	s_and_b32          s8, s3, 0xff
	s_bfm_b64          s[10:11], s8, 0
	s_and_saveexec_b32 s15, s10
	s_cbranch_execz    done_vs
	v_cmp_eq_u32_e64   s20, 0, v10
	v_cmp_eq_u32_e64   s21, 1, v10
	v_cmp_eq_u32_e64   s22, 2, v10
	v_cndmask_b32_e64  v4, 0, 0.5, s21          // x = vid == 1 ? 0.5 : 0
	v_cndmask_b32_e64  v4, v4, -0.5, s20        // x = vid == 0 ? -0.5 : x
	v_cndmask_b32_e64  v5, -0.5, 0.5, s22       // y = vid == 2 ? 0.5 : -0.5
	v_mov_b32          v6, 0                    // z
	v_mov_b32          v7, 1.0                  // w
	export             pos0 v4, v5, v6, v7 done
done_vs:
	s_endpgm
