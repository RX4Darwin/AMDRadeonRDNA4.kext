// nggvgpr.s — ngg.s plus stores of what the NGG wave really gets and computes, the input-side discriminator of the
// draw ladder (rdna4-gfxdiag, bit 512; W45). Round 5 on the card: C_INVOCATIONS = 1, C_PRIMITIVES = 0 in every draw and the
// NGG marker (nggstore.s) proves the wave ran, so the primitive reaches the clipper and is dropped there. The clipper drops what is
// degenerate, non-finite or outside its guard band: this shows whether the VGPR inputs (v0 = packed primitive, v3 = VertexID) and the
// positions the shader exports are what ngg.s assumes. One dword per lane, four arrays 0x80 bytes apart (32 lanes):
//   [0x000 + 4*lane] v0 (the packed primitive; only lanes < primitives are meaningful)
//   [0x080 + 4*lane] v3 (VertexID)
//   [0x100 + 4*lane] v4 (x position, as exported; vertex lanes only)
//   [0x180 + 4*lane] v5 (y position, as exported; vertex lanes only)
// The four base addresses are eight literal dwords (0xDEAD0001..0xDEAD0008) the kext patches at run time (s_mov_b32 sN, literal).
// It needs 10 VGPRs: the kext patches SPI_SHADER_PGM_RSRC1_GS.VGPRS to 1.

.text
.globl nggvgpr
nggvgpr:
	s_mov_b32          exec_lo, -1              // the wave starts with EXEC = lane 0 only (see ngg.s)
	s_mov_b32          s24, 0xdead0001          // array 0 base low  (patched)
	s_mov_b32          s25, 0xdead0002          // array 0 base high
	s_mov_b32          s26, 0xdead0003          // array 1
	s_mov_b32          s27, 0xdead0004
	s_mov_b32          s28, 0xdead0005          // array 2
	s_mov_b32          s29, 0xdead0006
	s_mov_b32          s30, 0xdead0007          // array 3
	s_mov_b32          s31, 0xdead0008
	v_mbcnt_lo_u32_b32 v8, -1, 0                // lane id
	v_lshlrev_b32      v8, 2, v8                // lane * 4
	global_store_b32   v8, v0, s[24:25]
	global_store_b32   v8, v3, s[26:27]

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
	global_store_b32   v8, v4, s[28:29]
	global_store_b32   v8, v5, s[30:31]
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
