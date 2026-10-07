// nggstore.s — ngg.s plus three memory stores at the very top, the NGG-launch discriminator of the draw
// ladder (rdna4-gfxdiag, bit 32; premetal/rootcause-draw.md #4). Nothing in round 4 told "the NGG wave never
// launched" from "it ran and its primitive was culled": this stores, before any export,
//   [0] 0xC0DE0002 (proof the wave started), [1] s2 (gs_tg_info), [2] s3 (merged_wave_info)
// to the marker address. The address is two literal dwords (0xDEAD0001 / 0xDEAD0002) the kext patches at run
// time (s_mov_b32 sN, literal; no user SGPRs, so the system SGPR layout stays that of ngg.s). It needs 10
// VGPRs, so the kext also patches SPI_SHADER_PGM_RSRC1_GS.VGPRS to 1 (8 VGPRs per granule) for this variant.

.text
.globl nggstore
nggstore:
	s_mov_b32          exec_lo, -1              // the wave starts with EXEC = lane 0 only (see ngg.s)
	s_mov_b32          s24, 0xdead0001          // marker address low  (patched)
	s_mov_b32          s25, 0xdead0002          // marker address high (patched)
	v_mov_b32          v8, 0
	v_mov_b32          v9, 0xc0de0002
	global_store_b32   v8, v9, s[24:25]
	v_mov_b32          v8, 4
	v_mov_b32          v9, s2
	global_store_b32   v8, v9, s[24:25]
	v_mov_b32          v8, 8
	v_mov_b32          v9, s3
	global_store_b32   v8, v9, s[24:25]

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
