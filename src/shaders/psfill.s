// psfill.s — the pixel shader of the paravirtual host's clears (wave32): the colour in its four user SGPRs (SPI_SHADER_USER_DATA_PS_0..3,
// RSRC2_PS.USER_SGPR = 4) to MRT0 as 4 x fp32 (SPI_SHADER_COL_FORMAT.COL0 = 32_ABGR); the CB converts it to the target's format.

.text
.globl psfill
psfill:
	v_mov_b32 v0, s0           // R
	v_mov_b32 v1, s1           // G
	v_mov_b32 v2, s2           // B
	v_mov_b32 v3, s3           // A
	export    mrt0 v0, v1, v2, v3 done
	s_endpgm
