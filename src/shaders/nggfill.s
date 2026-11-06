// nggfill.s — the NGG "vertex shader" (hardware GS stage, wave32, passthrough) of the paravirtual host's clears: one triangle over the
// whole viewport, VertexID 0 -> (-1,-1), 1 -> (3,-1), 2 -> (-1,3), z = 0, w = 1. Inputs and exports as ngg.s.

.text
.globl nggfill
nggfill:
	// position export: lanes [0, vertices)
	s_and_b32          s8, s3, 0xff
	s_bfm_b64          s[10:11], s8, 0
	s_mov_b32          exec_lo, s10
	s_cbranch_execz    skip_pos
	v_cmp_eq_u32_e64   s20, 1, v3
	v_cmp_eq_u32_e64   s21, 2, v3
	v_cndmask_b32_e64  v4, -1.0, 0x40400000, s20   // x = vid == 1 ? 3 : -1
	v_cndmask_b32_e64  v5, -1.0, 0x40400000, s21   // y = vid == 2 ? 3 : -1
	v_mov_b32          v6, 0                       // z
	v_mov_b32          v7, 1.0                     // w
	export             pos0 v4, v5, v6, v7 done
skip_pos:

	// primitive export: lanes [0, primitives)
	s_wait_expcnt      0x0
	s_bfe_u32          s9, s3, 0x80008             // s9 = merged_wave_info[15:8]
	s_bfm_b64          s[12:13], s9, 0             // lane mask of the primitives
	s_mov_b32          exec_lo, s12
	s_cbranch_execz    done_vs
	export             prim v0, off, off, off done
done_vs:
	s_endpgm
