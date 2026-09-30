# W45: the clip / cull / viewport / scissor state of the first draw

Round 5, boot 3 on the card (`E:\rdna4fb-diag-20260930-053620.txt`, `-053909.txt`): **every draw and every ladder variant** reads

    PS_INVOCATIONS 0, C_PRIMITIVES 0, C_INVOCATIONS 1, VS_INVOCATIONS 3, IA_PRIMITIVES 1, IA_VERTICES 3
    "pipeline statistics say: C_INVOCATIONS > 0 but C_PRIMITIVES = 0: clipped or culled"
    variant 32: NGG marker 0xc0de0002, s2 0x00403000, s3 0x10000103  (the NGG wave RAN)

So the GE issues the draw, the NGG wave runs and exports, the primitive reaches the clipper (C_INVOCATIONS 1) and is dropped there (C_PRIMITIVES 0); no pixel wave. Everything after the clipper is
downstream of the failure. The clear state is fine (`clear state BEFORE any CSB replay: 0 of 7 non-zero`), `VGT_PRIMITIVE_TYPE` reads 0 on Linux too (dropped).

## 1. What the draw stream writes (checked register by register)

Decoded with `gfx12.json` (script: parse `kStream` in `gfx12_draw.h` against the register database; 151 of 564 context registers are written) and compared with what radeonsi writes
(`si_state.c:908-913`, `:4863-4916`, `ac_cmdbuf.c:551,751`) and with the Linux capture:

| Register | Stream | Mesa / Linux | Reading |
|---|---|---|---|
| `PA_CL_CLIP_CNTL` | `0x01000000` = DX_LINEAR_ATTR_CLIP_ENA | radeonsi: `DX_CLIP_SPACE_DEF(clip_halfz)`, ZCLIP_NEAR/FAR_DISABLE(!depth_clip), DX_RASTERIZATION_KILL(discard), DX_LINEAR_ATTR_CLIP_ENA(1) | no kill, clip enabled, GL clip space (z in [-w, w]): the shader's z = 0, w = 1 is inside |
| `PA_CL_VTE_CNTL` | `0x0000043f` = all six scale/offset enables + VTX_W0_FMT | Mesa always | positions are clip-space, the VTE divides by W and applies the viewport |
| `PA_CL_VPORT_X/Y/Z SCALE/OFFSET` | 128 / 128 / 128 / 128 / 0.5 / 0.5 | | 256x256 target, z mapped to 0..1 |
| `PA_SC_VPORT_ZMIN_0/ZMAX_0` | 0 / 1.0 | | |
| `PA_CL_GB_*_ADJ` | all 1.0 | Mesa (`si_state_viewport.c:270-295`) | guard band = viewport |
| `PA_CL_VS_OUT_CNTL` | `0x60000000` (BYPASS_VTX/PRIM_RATE_COMBINER) | Mesa gfx12 | no misc export flags (kill flag, viewport index, ...) |
| `PA_SU_SC_MODE_CNTL` | `0x00080240` (PROVOKING_VTX_LAST, POLYMODE PTYPE 2) | | CULL_FRONT/BACK = 0: no face culling |
| `PA_SU_PRIM_FILTER_CNTL` / `PA_SU_SMALL_PRIM_FILTER_CNTL` | `0` / `0x41` | radeonsi gfx12 writes only the small-prim one (`SMALL_PRIM_FILTER_ENABLE(1)`) | |
| `PA_CL_NANINF_CNTL`, `PA_SU_VTX_CNTL` | `0`, `0x2d` | | |
| `PA_SC_MODE_CNTL_0` | `0x22` (VPORT_SCISSOR_ENABLE, ALTERNATE_RBS_PER_TILE) | Linux `0x22` | implicit viewport scissor OFF (Mesa: `ac_cmdbuf.c:718-725`) |
| `PA_SC_MODE_CNTL_1` | `0x060201b5` | Linux/vkcube `0x060201bc` | only walk size / fence bits (vkcube's tiled destination); not a clip switch |
| `GE_CNTL` | `0xa0010080` | Linux `0xa0010080` | equal |
| scissors (screen, generic, window, viewport 0) | full / 255 | | |
| `PA_SC_CLIPRECT_RULE` | `0xffff` | | every pixel passes whatever the rectangles hold |

Nothing here is a kill or cull. The stream matches Mesa for every register it writes.

## 2. What it leaves undefined

335 of the 564 context registers are not written by the stream. The clear-state extents (62 registers) are zeroed by the SRM/replay; the rest powers up as SRAM garbage on the card
(`draw: context registers the stream never writes, non-zero: 0x001=0x20211bd2 ...`). The ones in the geometry front end:

- the **implicit viewport scissor** rectangles `PA_SC_VPORT_0..15_TL/BR` (`0x040..0x05f`; zeroed by the CSB, i.e. an empty rectangle), gated by `PA_SC_MODE_CNTL_0.IMPLICIT_VPORT_SCISSOR_ENABLE` (bit 7, 0 here);
- the four **cliprects** and their EXT registers (gated by `CLIPRECT_RULE 0xffff`), the **scissors of viewports 1..15**;
- the six **user clip planes** `PA_CL_UCP_0..5` (gated by `CLIP_CNTL.UCP_ENA`, 0) and `PA_CL_PROG_NEAR_CLIP_Z` (`ZCLIP_PROG_NEAR_ENA`, 0);
- polygon offset (`0x2de..0x2e3`), stereo (`PA_STATE_STEREO_X`), line stipple, `VGT_TF_PARAM`, `VGT_LS_HS_CONFIG`, the streamout opaque registers.

Each is gated off by the stream's own state (verify-draw.md), but "gated off" is an argument. The card said the primitive dies in the clipper, so these are now written explicitly.

## 3. What W45 adds

- **Sane clip state block** (`gfxEmitSaneClip`): 13 `SET_CONTEXT_REG` packets, 112 registers, 138 ring dwords, emitted before the draw IB (after the CSB replay): the rectangles full screen (TL 0, BR `0xffffffff`),
  everything else in the list above 0. The stream's own writes (Mesa's values) come later and win for any overlap. `rdna4-gfxsane=0` leaves it out (the control; the emulator run shows the implicit rectangles then read 0).
- **Clip-state readback through the CP** (`gfxEmitClipProbe` / `gfxClipReport`, `rdna4-gfxprobe=1`): 51 registers COPY_DATA'd to memory right after the draw IB (the state the CP holds for the draw that was dropped):
  `draw: clip state (CP view after the draw): ...`, `clip state: N of M registers equal what the stream / the sane block wrote; differ: ...`, and `clip switches: CLIP_DISABLE .. VTX_KILL_OR .. DX_RASTERIZATION_KILL ..
  CULL_FRONT/BACK .. VTE enables .. prim filter ..` spelled out.
- **Ladder variants** (auto-added to a probe boot's ladder, order 32, 128, 256, 512, 64, 8, 2, 1, 4; each summary entry gets `/cprim N`, the C_PRIMITIVES delta of that draw):
  - **128**: `PA_CL_CLIP_CNTL.CLIP_DISABLE = 1`. If `cprim` becomes 1 the clipper was dropping the primitive.
  - **256**: the primitive filters off (`PA_SU_PRIM_FILTER_CNTL` disable bits, `SMALL_PRIM_FILTER_ENABLE = 0`). If `cprim` becomes 1 a setup filter was dropping it.
  - **512** (`shaders/nggvgpr.s`): `ngg.s` plus per-lane stores of `v0` (the packed primitive), `v3` (VertexID) and the exported `x`/`y` (`draw: NGG VGPRs per lane: ...`,
    `NGG inputs say: VertexID IS/is NOT the lane index, the exported positions are/are NOT what ngg.s intends`). The wave runs (marker), but the shader's inputs (`v3` as the VertexID, `v0`) are an ABI assumption
    (verified in the emulator only): a wrong VertexID makes all three vertices identical = a degenerate triangle, which the clipper drops (C_INVOCATIONS 1, C_PRIMITIVES 0).
- Emulator: `v_mbcnt_lo_u32_b32` and `v_lshlrev_b32` decoded for `nggvgpr.s`.

## 4. Reading boot 3 next

| Line | Reading |
|---|---|
| baseline `C_PRIMITIVES` now > 0 (or pixels appear) | the explicit block fixed it: one of the register groups above mattered; run with `rdna4-gfxsane=0` to confirm, then bisect the groups |
| `NGG inputs say: VertexID is NOT the lane index` | the NGG ABI assumption is wrong (v3 is not the VertexID on this hardware/firmware): fix the shader's inputs; the clip state is not the cause |
| `NGG inputs say: ... exported positions are NOT what ngg.s intends` while VertexID is right | a shader/VGPR-file problem (x/y values wrong): read the hex per lane |
| VertexID and positions right, `128:.../cprim 1` | the clipper drops it unless CLIP_DISABLE: guard band / frustum state; read `clip state (CP view)` for the values the CP holds |
| `256:.../cprim 1` | a setup filter drops it |
| `clip state: ... differ: X got .. want ..` | the CP did not take that write (the register named) |
| every variant `cprim 0`, inputs right, clip state equal | something not in this table: the GE/PA path below the clipper (ring memory attributes, #3) |
