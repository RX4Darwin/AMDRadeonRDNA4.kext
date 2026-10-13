// psred.s — the pixel shader of the first gfx12 triangle (wave32): one
// constant colour to MRT0 as 4 x fp32 (SPI_SHADER_COL_FORMAT.COL0 = 32_ABGR);
// the CB converts it to R8G8B8A8_UNORM, so a covered pixel reads 0xFF0000FF.
// SPI_PS_INPUT_ENA enables PERSP_CENTER (v0, v1 = i, j), which the hardware
// requires and this shader ignores. premetal/gfx12-draw-notes.md 3.8.

.text
.globl psred
psred:
	v_mov_b32 v0, 1.0          // R
	v_mov_b32 v1, 0            // G
	v_mov_b32 v2, 0            // B
	v_mov_b32 v3, 1.0          // A
	export    mrt0 v0, v1, v2, v3 done
	s_endpgm
