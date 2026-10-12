// nggcol.s — the NGG "vertex shader" (hardware GS stage, wave32) of the G4 triangle: ngg.s plus one per-vertex
// attribute, colour = vertex 0 red, 1 green, 2 blue, stored to the ATTRIBUTE RING the way RADV/ACO does it on gfx12
// (docs/g4-colour.md; ground truth: docs/hw-logs/2026-09-30-linux-radv-colour/radv-shaders-and-ibs.txt:321-373).
//
// Same ABI and EXEC rules as ngg.s: s2 = gs_tg_info, s3 = merged_wave_info ([7:0] vertices, [15:8] primitives, [27:24]
// wave index in the subgroup), v0 = packed primitive, v3 = VertexID; EXEC starts as lane 0 only and is SET, never ANDed.
// New here: s5 = gs_attr_offset ([14:0], in 512 B units: where the hardware put this subgroup in the attribute ring;
// ac_nir_lower_intrinsics_to_args.c:310-312, radv_shader_args.c:797-798).
//
// The store: buffer_store_b128 v[8:11], v12 (index = thread id in the subgroup), a buffer descriptor for the attribute ring,
// soffset = gs_attr_offset << 9, idxen, scope device (ACCESS_COHERENT), base offset param 0 * 16. The descriptor is
// ac_build_attr_ring_descriptor (swizzle 16 B, index stride 32, structured, stride = 16 * number of params = 16 set by
// radv_nir_lower_abi.c:109-118): RADV loads it from its ring table (s[0:1]); this shader builds it in SGPRs from three
// literal dwords the builder patches (userspace/gfx12tricol.h, marker-literal convention of nggstore.s):
//   0xc01a0001 = ring VA low, 0xc01a0002 = dword1 = VA[47:32] | 0xc0000000 (swizzle_enable 3) | 16 << 16 (stride), 0xc01a0003 = size.
// The lanes that store are [0, (vertices + 7) & ~7): full vec4s in groups of 8 lanes (ac_nir_prerast_utils.c:485-491);
// the extra lanes store zero colours. The primitive and position exports come first, as ngg.s (ACO order).
// Needs 13 VGPRs: the stream sets SPI_SHADER_PGM_RSRC1_GS.VGPRS = 1.

.text
.globl nggcol
nggcol:
	// position export: lanes [0, vertices)
	s_and_b32          s8, s3, 0xff
	s_bfm_b64          s[10:11], s8, 0
	s_mov_b32          exec_lo, s10
	s_cbranch_execz    skip_pos
	v_cmp_eq_u32_e64   s20, 0, v3               // vertex 0
	v_cmp_eq_u32_e64   s21, 1, v3               // vertex 1
	v_cmp_eq_u32_e64   s22, 2, v3               // vertex 2
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
	s_cbranch_execz    skip_prim
	export             prim v0, off, off, off done
skip_prim:

	// attribute store: lanes [0, (vertices + 7) & ~7)
	s_wait_expcnt      0x0
	s_and_b32          s8, s3, 0xff
	s_add_co_u32       s8, s8, 7
	s_and_b32          s8, s8, -8
	s_bfm_b64          s[10:11], s8, 0
	s_mov_b32          exec_lo, s10
	s_cbranch_execz    done_vs
	v_cndmask_b32_e64  v8, 0, 1.0, s20          // colour.r = vertex 0   (the masks of the position section: 0 for the padding lanes)
	v_cndmask_b32_e64  v9, 0, 1.0, s21          // colour.g = vertex 1
	v_cndmask_b32_e64  v10, 0, 1.0, s22         // colour.b = vertex 2
	v_mov_b32          v11, 0                   // unused component
	s_bfe_u32          s9, s3, 0x40018          // wave index in the subgroup = merged_wave_info[27:24]
	s_lshl_b32         s9, s9, 5                // wave32: 32 threads per wave
	v_mbcnt_lo_u32_b32 v12, -1, s9              // thread id in the subgroup = the vertex index of the ring
	s_and_b32          s5, s5, 0x7fff           // gs_attr_offset [14:0]
	s_lshl_b32         s5, s5, 9                // 512 B units
	s_mov_b32          s12, 0xc01a0001          // descriptor dword0: ring VA low (patched)
	s_mov_b32          s13, 0xc01a0002          // dword1: VA[47:32] | swizzle 3 | stride 16 (patched)
	s_mov_b32          s14, 0xc01a0003          // dword2: size in bytes (patched)
	s_mov_b32          s15, 0x0043ffac          // dword3: R32G32B32A32_FLOAT, structured OOB, index stride 32, no resource level (ac_build_attr_ring_descriptor run against Mesa 26.2.2 for gfx12: docs/hw-logs/2026-09-30-linux-radv-colour/attr-ring-descriptor.txt)
	buffer_store_b128  v[8:11], v12, s[12:15], s5 idxen scope:SCOPE_DEV
done_vs:
	s_endpgm
