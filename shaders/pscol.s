// pscol.s — the pixel shader of the G4 triangle (wave32): reads the interpolated attribute 0 (the vertex colour the NGG stage
// stored to the attribute ring) and exports it as 4 x fp32 to MRT0 (SPI_SHADER_COL_FORMAT.COL0 = 32_ABGR, like psred.s; RADV exports
// fp16 pairs). Ground truth: RADV's PS for the same colour triangle, docs/hw-logs/2026-09-30-linux-radv-colour/radv-shaders-and-ibs.txt:63-76,
// docs/g4-colour.md.
//
// gfx12 attribute interpolation: SPI moves the ring parameters into LDS; the PS sets m0 = PrimMask (the first system SGPR after the
// user SGPRs: s2, because the stream sets RSRC2_PS.USER_SGPR = 2 like RADV), loads each channel with ds_param_load and interpolates
// with v_interp_p10_f32 / v_interp_p2_f32 (src0 = the loaded parameter register, src1 = i (v0) resp. j (v1), src2 = the parameter
// register resp. the p10 result). wait_exp:N on the p10 instructions waits until at most N of the 3 param loads are outstanding.
// i, j = v0, v1 = SPI_PS_INPUT_ENA.PERSP_CENTER_ENA (the stream enables exactly that). Quads must be complete: s_wqm first.
// Needs 9 VGPRs: the stream sets SPI_SHADER_PGM_RSRC1_PS.VGPRS = 1.

.text
.globl pscol
pscol:
	s_mov_b32          m0, s2                   // PrimMask: LDS parameter offset + new-primitive mask
	s_wqm_b32          exec_lo, exec_lo         // whole quads (helper lanes) for the interpolation
	ds_param_load      v2, attr0.x wait_va_vdst:15 wait_vm_vsrc:1
	ds_param_load      v3, attr0.y wait_va_vdst:15 wait_vm_vsrc:1
	ds_param_load      v4, attr0.z wait_va_vdst:15 wait_vm_vsrc:1
	v_interp_p10_f32   v5, v2, v0, v2 wait_exp:2   // r = P0 + P10 * i
	v_interp_p10_f32   v6, v3, v0, v3 wait_exp:1   // g
	v_interp_p10_f32   v7, v4, v0, v4 wait_exp:0   // b
	v_interp_p2_f32    v5, v2, v1, v5 wait_exp:7   // r += P20 * j
	v_interp_p2_f32    v6, v3, v1, v6 wait_exp:7
	v_interp_p2_f32    v7, v4, v1, v7 wait_exp:7
	v_mov_b32          v8, 1.0                  // A
	export             mrt0 v5, v6, v7, v8 done
	s_endpgm
