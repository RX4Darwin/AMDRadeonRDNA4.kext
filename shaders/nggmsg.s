// nggmsg.s — the NGG "vertex shader" variant that sends GS_ALLOC_REQ, for
// VGT_SHADER_STAGES_EN.PRIMGEN_PASSTHRU_NO_MSG=0 (0x00400000). premetal/
// gfx12-draw-notes.md 3.4 / 3.7 (Mesa: ac_nir_lower_ngg.c:1604-1617). Same triangle
// as ngg.s. Diagnostic variant (rdna4-gfxdiag, bit 2): does the passthrough
// shader work where this one does not.

.text
.globl nggmsg
nggmsg:
	s_bfe_u32          s16, s3, 0x40018         // wave index in subgroup = merged_wave_info[27:24]
	s_cmp_lg_u32       s16, 0
	s_cbranch_scc1     skip_alloc
	s_bfe_u32          s17, s2, 0x9000c         // subgroup vertex count = gs_tg_info[20:12]
	s_bfe_u32          s18, s2, 0x90016         // subgroup prim count   = gs_tg_info[30:22]
	s_lshl_b32         s18, s18, 12
	s_or_b32           m0, s18, s17             // m0 = num_prim << 12 | num_vtx
	s_sendmsg          sendmsg(MSG_GS_ALLOC_REQ)
skip_alloc:
	s_and_b32          s8, s3, 0xff
	s_bfm_b64          s[10:11], s8, 0
	s_mov_b32          exec_lo, s10
	s_cbranch_execz    skip_pos
	v_cmp_eq_u32_e64   s20, 0, v3
	v_cmp_eq_u32_e64   s21, 1, v3
	v_cmp_eq_u32_e64   s22, 2, v3
	v_cndmask_b32_e64  v4, 0, 0.5, s21
	v_cndmask_b32_e64  v4, v4, -0.5, s20
	v_cndmask_b32_e64  v5, -0.5, 0.5, s22
	v_mov_b32          v6, 0
	v_mov_b32          v7, 1.0
	export             pos0 v4, v5, v6, v7 done
skip_pos:

	// primitive export after the positions, as ACO does (either order works: docs/linux-replay.md)
	s_wait_expcnt      0x0
	s_bfe_u32          s9, s3, 0x80008
	s_bfm_b64          s[12:13], s9, 0
	s_mov_b32          exec_lo, s12
	s_cbranch_execz    done_vs
	export             prim v0, off, off, off done
done_vs:
	s_endpgm
