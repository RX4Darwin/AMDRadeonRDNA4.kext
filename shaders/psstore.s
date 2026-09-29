// psstore.s — psred.s plus one memory store, the discriminator of the draw
// ladder (rdna4-gfxdiag, bit 8). The PS writes 0xC0DE0001 to the address in its two
// user SGPRs (SPI_SHADER_USER_DATA_PS_0/1, s[0:1]) and then exports the same red.
// If the marker lands but the target stays empty, everything up to and including
// the pixel shader works and only the colour-buffer write is lost; if the marker
// is missing too, no pixel wave ran.

.text
.globl psstore
psstore:
	v_mov_b32 v4, 0
	v_mov_b32 v5, 0xc0de0001
	global_store_b32 v4, v5, s[0:1]
	v_mov_b32 v0, 1.0          // R
	v_mov_b32 v1, 0            // G
	v_mov_b32 v2, 0            // B
	v_mov_b32 v3, 1.0          // A
	export    mrt0 v0, v1, v2, v3 done
	s_endpgm
