// nggsgpr.s — ngg.s plus a probe of the NGG wave's initial SGPRs, for G4 (docs/g4-colour.md uncertainty 2: is s5 really
// gs_attr_offset under the kext's configuration, RSRC2_GS.USER_SGPR = 0?). G3 stream and registers unchanged, no attribute store.
//
// At the top, lane 0 only (EXEC = 1, what the wave starts with, set explicitly), it stores six dwords with the nggstore.s marker
// convention (the address is the two literal dwords 0xdead0001 / 0xdead0002 the builder patches):
//   [0] s0  [1] s1  [2] s2 (gs_tg_info, measured 0x00403000)  [3] s3 (merged_wave_info, measured 0x10000103)  [4] s4  [5] s5
// It uses v1 / v2 (unused inputs) as store offset / data, so it needs no more than the 8 VGPRs of ngg.s (VGPRS = 0, no register patch).
// Everything after the stores is ngg.s unchanged (EXEC SET from merged_wave_info, positions then primitive).

.text
.globl nggsgpr
nggsgpr:
	s_mov_b32          exec_lo, 1               // lane 0 stores (the wave starts with EXEC = lane 0 only)
	s_mov_b32          s24, 0xdead0001          // marker address low  (patched)
	s_mov_b32          s25, 0xdead0002          // marker address high (patched)
	v_mov_b32          v1, 0
	v_mov_b32          v2, s0
	global_store_b32   v1, v2, s[24:25]
	v_mov_b32          v1, 4
	v_mov_b32          v2, s1
	global_store_b32   v1, v2, s[24:25]
	v_mov_b32          v1, 8
	v_mov_b32          v2, s2
	global_store_b32   v1, v2, s[24:25]
	v_mov_b32          v1, 12
	v_mov_b32          v2, s3
	global_store_b32   v1, v2, s[24:25]
	v_mov_b32          v1, 16
	v_mov_b32          v2, s4
	global_store_b32   v1, v2, s[24:25]
	v_mov_b32          v1, 20
	v_mov_b32          v2, s5
	global_store_b32   v1, v2, s[24:25]

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

	// primitive export: lanes [0, primitives), after the positions like ACO
	s_wait_expcnt      0x0
	s_bfe_u32          s9, s3, 0x80008          // s9 = merged_wave_info[15:8]
	s_bfm_b64          s[12:13], s9, 0          // lane mask of the primitives
	s_mov_b32          exec_lo, s12
	s_cbranch_execz    done_vs
	export             prim v0, off, off, off done
done_vs:
	s_endpgm
