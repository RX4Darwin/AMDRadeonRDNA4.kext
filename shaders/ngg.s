// ngg.s — the NGG "vertex shader" (hardware GS stage, wave32) of the first
// gfx12 triangle, in passthrough mode (VGT_SHADER_STAGES_EN.
// PRIMGEN_PASSTHRU_NO_MSG=1: no GS_ALLOC_REQ). From premetal/gfx12-draw-notes.md
// 3.6, where the gfx12 NGG contract is cited to Mesa.
//
// Inputs (gfx12 merged ES/GS layout):
//   s2 = gs_tg_info, s3 = merged_wave_info ([7:0] vertices, [15:8] primitives
//   in this wave); v0 = the packed primitive (9 bits per vertex index), which
//   passthrough exports unchanged; v3 = VertexID.
// VertexID 0 -> (-0.5,-0.5), 1 -> (0.5,-0.5), 2 -> (0,0.5), z = 0, w = 1.

//
// EXEC: an NGG (merged ES/GS) wave does NOT start with its vertex lanes enabled. Measured on the card
// under Linux (tools/linux-replay, docs/linux-replay.md): it starts with EXEC = 0x00000001, lane 0 only.
// The shader must SET exec from merged_wave_info, as ACO does (s_bfe_u64 exec, -1, ...), never AND it
// with the initial value: s_and_saveexec left only vertex 0 exporting a position, so the primitive
// reached the clipper without two of its vertices (C_INVOCATIONS 1, C_PRIMITIVES 0, rounds 4-6).

.text
.globl ngg
ngg:
	// position export: lanes [0, vertices)
	s_and_b32          s8, s3, 0xff
	s_bfm_b64          s[10:11], s8, 0
	s_mov_b32          exec_lo, s10
	s_cbranch_execz    skip_pos
	v_cmp_eq_u32_e64   s20, 0, v3
	v_cmp_eq_u32_e64   s21, 1, v3
	v_cmp_eq_u32_e64   s22, 2, v3
	v_cndmask_b32_e64  v4, 0, 0.5, s21          // x = vid == 1 ? 0.5 : 0
	v_cndmask_b32_e64  v4, v4, -0.5, s20        // x = vid == 0 ? -0.5 : x
	v_cndmask_b32_e64  v5, -0.5, 0.5, s22       // y = vid == 2 ? 0.5 : -0.5
	v_mov_b32          v6, 0                    // z
	v_mov_b32          v7, 1.0                  // w
	export             pos0 v4, v5, v6, v7 done
skip_pos:

	// primitive export: lanes [0, primitives), after the positions like ACO (pos0 done, s_wait_expcnt 0,
	// prim done). Either order draws on the card (docs/linux-replay.md); this one is Mesa's.
	s_wait_expcnt      0x0
	s_bfe_u32          s9, s3, 0x80008          // s9 = merged_wave_info[15:8]
	s_bfm_b64          s[12:13], s9, 0          // lane mask of the primitives
	s_mov_b32          exec_lo, s12
	s_cbranch_execz    done_vs
	export             prim v0, off, off, off done
done_vs:
	s_endpgm
