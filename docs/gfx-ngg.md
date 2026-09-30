# W46: C_INVOCATIONS 1, C_PRIMITIVES 0 with the clipper and the filters off — what else can zero it

Round 6, boot 3 on the card (`E:\rdna4fb-diag-20260930-062351.txt`, NVRAM/registry `Compute,GFXDiag` only; the `gfx:` kernel lines had scrolled out of the diagnostic script's dmesg window):

    32:0/cprim 0/ngg ran   128:0/cprim 0   256:0/cprim 0   512:0/cprim 0   64:0/primtype-idx   8:0/marker no   2:0   1:0

Reading of the W45 decision table with this result: **128 (`PA_CL_CLIP_CNTL.CLIP_DISABLE`) still cprim 0 = not the clipper's clip/discard logic; 256 (primitive filters off) still cprim 0 = not a setup filter.**
So the primitive is lost before the clipper can act on it, or it arrives unusable. Variant 512's verdict (`vid`/`pos`/`v0`) was lost with the log; it is now persistent (below).

## 1. Checked against Mesa (radeonsi / ACO / ac_nir) and the Linux capture

| Item | ngg.s / the stream | Mesa / Linux | Result |
|---|---|---|---|
| `gs_tg_info` (s2) | `0x00403000` on the card | `workgroup_num_input_vertices = gs_tg_info[20:12]`, `..._primitives = gs_tg_info[30:22]` (`ac_nir_lower_intrinsics_to_args.c`) | 3 vertices, 1 primitive: exactly what the draw declared |
| `merged_wave_info` (s3) | `0x10000103` | `[7:0]` vertices in the wave, `[15:8]` primitives (`ac_nir_lower_ngg.c`) | 3 / 1 |
| system SGPRs | s0/s1 PGM_LO/HI_GS, s2 gs_tg_info, s3 merged_wave_info, s4 tess_offchip_offset, s5 gs_attr_offset (`si_shader_args.c:303-320`) | `RSRC2_GS.USER_SGPR = 0` | the shader reads only s2/s3 |
| VGPRs on gfx12 | v0 primitive, v3 VertexID | `gs_vtx_offset[0]` (v0), `gs_prim_id` (v1), `gs_vtx_offset[1]` (v2), then the VS inputs, VertexID first (`si_shader_args.c:349-358`) | as assumed |
| the primitive value | ngg.s exports v0 unchanged | "NGG passthrough mode: the HW already packs the primitive export value to a single register" (`ac_nir_lower_intrinsics_to_args.c`); vertices are `ubfe(v0, 9 * v, 8)` (`ac_nir_lower_ngg.c:126`); packing `idx << 9*i`, edge flags from `v0` (`ac_nir_pack_ngg_prim_exp_arg`) | as assumed; **the emulator had it wrong** (byte-packed v0 converted at the export): fixed, v0 = `0x00080200` exported as is |
| export flags | prim export `done`, last position export `done` | prim `done` from NIR (`AC_EXP_FLAG_DONE`), position `done` on the last position export by ACO (`aco_assembler.cpp fix_exports`) | same |
| `VGT_SHADER_STAGES_EN` | `0x04400000` (GS_W32_EN, PRIMGEN_PASSTHRU_NO_MSG) | gfx12: `PRIMGEN_PASSTHRU_NO_MSG(passthrough)`, no `GS_ALLOC_REQ` for passthrough (`has_ngg_passthru_no_msg`, asserted on gfx12; `ac_nir_lower_ngg.c:1604`) | same |
| `GS_VGPR_COMP_CNT` / `ES_VGPR_COMP_CNT` | 0 / 0 | gfx12: `gs_vgpr_comp_cnt = 0` "VGPR0 contains offsets 0-2, edgeflags"; ES 0 for a VS with only VertexID (`si_state_shaders.cpp:1100-1143`) | same |
| clip/cull/filter state | W45 | radeonsi | all equal (docs/gfx-clip.md) |
| NGG state regs | `GE_CNTL 0xa0010080`, `PA_CL_NGG_CNTL 0x78`, `GE_NGG_SUBGRP_CNTL 1`, `PA_SC_NGG_MODE_CNTL 0x40` | Mesa; Linux `GE_CNTL` equal | same |

Nothing in the shader ABI or the registers differs from Mesa. What remains:

1. **the shader's inputs really being what the ABI says on this hardware/firmware** (a VertexID that is not the lane index, or a `v0` that is not the packed primitive, gives three identical vertices or a malformed primitive: exactly C_INV 1 / C_PRIM 0);
2. **the GE's position/primitive rings**: the NGG wave's exports go to the position/primitive path; the rings are in VMID0 system-aperture memory (UC) with Mesa's GL2 hints (`GE_PRIM_RING_SIZE`: SCOPE, PAF/PAB_TEMPORAL, FORCE_SE_SCOPE, PAB_NOFILL: "stay dirty / no fill" assumes a cacheable BO; Mesa's rings are NC BOs). A read of zeros returns all-zero positions (`w = 0`), which is dropped as degenerate;
3. **the gfx queue itself** (bare RB0 without MQD/HQD; rootcause-draw.md #2), which no register experiment in the ladder can change.

## 2. What W46 adds

- **Persistence**: the registry property `Compute,GFXVerdict` (like `Compute,GFXDiag`), one entry per draw, so a late script run sees it:
  `<label>: ia <prims>/<verts> vs <n> ci <C_INVOCATIONS> cp <C_PRIMITIVES> ps <n> px <pixels> ring ok|HUNG clip <equal>/<compared> sw <mask>[ mk ran|NO | vid lane|NOT pos ok|NOT v0 <hex>]`
  (`sw` mask: 1 CLIP_DISABLE, 2 VTX_KILL_OR, 4 DX_RASTERIZATION_KILL, 8 CULL_FRONT, 16 CULL_BACK, 32 triangle prim filter disabled), plus `park: ...`. Bounded (3 KiB, ends `...(full)`).
- **Ladder variants** (probe boots, order 32, 1024, 2048, 512, 128, 256, 64, 8, 2, 1, 4):
  - **1024** (`shaders/nggconst.s`): nothing taken from the VGPR inputs: a constant primitive (`0x00080200`) and the lane id as the vertex index. cprim > 0 = the wave's inputs were wrong (see 512 for which).
  - **2048**: `GE_PRIM_RING_SIZE` = MEM_SIZE only (no GL2 hints). cprim > 0 = the ring attributes were the problem (move the rings to NC memory).
  - 512 (W45) is now also recorded in the property; 128/256 stay as the negative results they gave.
- **Park**: after a probe boot's draws PFP and ME are halted (`gfxPark`, `rdna4-gfxpark=0` leaves them running) with the SMU's GFX activity / power logged before and after
  (`park: SMU GFX activity a% -> b%`). CP_STAT and GRBM read idle after every draw in the round-5/6 logs, so nothing was wedged; what stays busy is the pair of unhalted microengines polling a
  ring nobody uses (GFXOFF is disallowed while `gfxMode` is set). If the SMU still says 100 % afterwards the load is elsewhere (clock/gating state, not the CP).

## 3. Reading boot 3 next (registry: `ioreg -l | grep GFXVerdict`)

| Result | Meaning |
|---|---|
| `1024: cp 1` or more | the wave's VGPR inputs were wrong: read `512: vid NOT lane` (VertexID) or `v0 <hex>` (a `v0` that is not `0x00080200` + edge flags) and fix the shader |
| `2048: cp 1` | the GE ring hints/attributes: rings need cacheable (NC) memory or no hints |
| every variant `cp 0`, `512: vid lane pos ok v0 00080200`, `1024: cp 0`, `2048: cp 0` | not the shader, not the clip state, not the ring hints: the gfx queue (MQD/HQD, VMID > 0) or memory/SE state the register experiments cannot reach; next step: real queue mapping |
| `park: ... gfx a% -> b%` with b small | the busy pipe was the polling microengines |
