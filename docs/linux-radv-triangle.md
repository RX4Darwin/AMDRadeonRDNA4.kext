# The first triangle drawn by RADV on this card: what the kext does differently

> **Update 2026-09-30 04:20: the export-order hypothesis below is DISPROVEN on the card.** RADV patched to
> export the primitive first (the kext's old order; `tools/radv-triangle/mesa-26.2.2-force-early-prim.patch`,
> `RADV_FORCE_EARLY_PRIM=1`) still draws 8192 px (C_PRIMITIVES > 0) on this RX 9070 XT. Mesa's late
> primitive export is a choice, not a hardware rule. The reordered kext shaders are harmless (Mesa's order)
> but do not fix the triangle. The "second tier" table is still open, and the next test is replaying the
> kext's own stream through amdgpu on Linux.
>
> **Root cause found: `docs/linux-replay.md`** (NGG waves start with EXEC = lane 0 only; the kext ANDed it).

Linux side of the handoff (`docs/HANDOFF-linux.md`, "what we need from Linux"), 2026-09-30, on the same PC
(RX 9070 XT, Arch Linux 7.2.2, Mesa 26.2.2, linux-firmware 20260810). No root needed.

`tools/radv-triangle/` draws **the kext's G3 triangle** with RADV: a 256x256 linear R8G8B8A8 target cleared
to 0, one triangle from VertexID only (0 -> (-0.5,-0.5), 1 -> (0.5,-0.5), 2 -> (0,0.5), z 0, w 1), a constant red
PS, then reads the target back and the pipeline statistics. `run.sh` dumps the ISA/config (`RADV_DEBUG=shaders`)
and the command stream (`RADV_DEBUG=dumpibs`). Capture: `hw-logs/2026-09-30-linux-radv-triangle/`.

## Result on the card

    pixels: 8192 red (0xFF0000FF, kext wants 8192), 0 others; bounds x 64..191 y 64..190
    stats: ia_verts 3 ia_prims 1 vs_inv 3 c_inv 1 c_prim 4 ps_inv 2112

Exactly the pixels `gfxring.cpp` expects, so the draw itself is right. The kext on the same card gets
`ia 1/3 vs 3 ci 1 cp 0 ps 0`. (C_PRIMITIVES reads 4 for one good triangle; why is unexplained. It is also not a reliable
signature: one failing draw on the card (wave64, VGPRS 0, 0 px) read 0, 0, 4, 0, 0, 8 in six runs, so the pixels and
PS_INVOCATIONS are the signal and C_PRIMITIVES only supporting evidence.)

RADV compiles the VS as NGG passthrough (no culling; the `nonggc` run is byte-identical), which is the
kext's configuration: `VGT_SHADER_STAGES_EN.PRIMGEN_PASSTHRU_NO_MSG=1`, `GE_CNTL 0xa0010080`,
`GE_NGG_SUBGRP_CNTL 1`, `GE_MAX_OUTPUT_PER_SUBGROUP 0x80`, `SPI_SHADER_IDX_FORMAT 1`,
`SPI_SHADER_POS_FORMAT 4`, `VGT_PRIMITIVE_TYPE 4`, `VGT_GS_OUT_PRIM_TYPE 2`, the same as `gfx12_draw.h`.

## The difference: the primitive export comes LAST on gfx11+ (disproven as the cause, see top)

RADV's NGG shader for this triangle (ACO, gfx1201):

    s_setprio 3
    s_pack_ll_b32_b16 s0, 0, s3            ; vertex count = merged_wave_info[7:0]
    s_bfe_u64 exec, -1, s0
    s_cbranch_execz BB6
    v_add_nc_u32 v3, s8, v3                ; VertexID = v3 + base vertex (user SGPR)
    ... x, y from VertexID, z 0, w 1.0 ...
    export pos0 v2, v3, v1, v4 done        ; f80008cf
    BB6:
    s_lshl_b32 s3, s3, 8                   ; primitive count = merged_wave_info[15:8]
    s_wait_expcnt 0x0                      ; bfc40000
    s_bfe_u64 exec, -1, s3
    s_cbranch_execz BB12
    export prim v0, off, off, off done     ; f8000941
    BB12:
    s_endpgm

`shaders/ngg.s` and every variant of the W41-W46 ladder (nggmsg, nggstore, nggvgpr, nggconst) did the
opposite: **prim export first, position export after**. Mesa never emits that order on this generation:

    radv_shader_info.c:  has_ngg_early_prim_export = gfx_level < GFX11 && exec_list_is_singular(...)
    ac_nir_lower_ngg.c:  if (!state.early_prim_export) { ... late primitive export at the end of the shader }

It seemed to fit the card's numbers: the primitive reaches the clipper (C_INVOCATIONS 1) and is dropped with no
position data behind it (C_PRIMITIVES 0). It is also the one thing every ladder variant shared, which
explains why no register experiment (CLIP_DISABLE, prim filters, ring hints) could move it.

The rest of the ABI the kext assumed is confirmed by RADV's code: v0 = the packed primitive, exported as is;
v3 = VertexID (RADV adds the base vertex from s8 because it has user SGPRs, the kext has none); s3 =
merged_wave_info with vertices in [7:0] and primitives in [15:8].

**Fix (this commit):** all five NGG shaders now export pos0 (done), then `s_wait_expcnt 0`, then prim (done),
the ACO sequence. The emulator is order-independent (it assembles the triangle after the wave) and already
accepts `s_wait_expcnt` (SOPP 68). Headers regenerated with LLVM 22. The unchanged kernels came out
byte-identical, so the only binary change is the reordering.

## Other differences (second tier, if the order fix alone is not enough)

| Register | RADV (works) | kext | Note |
|---|---|---|---|
| wave size | NGG **wave64** (`GS_W32_EN 0`, `VGT_SHADER_STAGES_EN 0x04000000`), PS wave64 | wave32 (`0x04400000`, `SPI_PS_IN_CONTROL.PS_W32_EN`) | both are legal on gfx12; RADV's default |
| `PA_CL_GB_{VERT,HORZ}_CLIP_ADJ` | 256.0 | 1.0 | guard band; only affects clipping, and CLIP_DISABLE did not help |
| `PA_SU_HARDWARE_SCREEN_OFFSET` | 0x00080008 | 0 | |
| `PA_CL_VS_OUT_CNTL` | 0 | 0x60000000 (BYPASS_{VTX,PRIM}_RATE_COMBINER) | |
| `PA_CL_CLIP_CNTL` | 0x01080000 (DX_CLIP_SPACE_DEF) | 0x01000000 | z = 0 is inside either |
| `SPI_SHADER_PGM_RSRC4_GS` | 0x01ff0bff (INST_PREF_SIZE 3) | 0x007f0bff (INST_PREF_SIZE 0 unless patched) | |
| `PA_SC_BINNER_CNTL_0` | 0x19fc01b0 (BINNING_ALLOWED) | 0x19fc0123 | after the clipper, cannot zero C_PRIMITIVES |
| `PA_SU_SC_MODE_CNTL` | 0x240 | 0x80240 (PROVOKING_VTX_LAST) | GL vs Vulkan convention |
| rings | `SPI_ATTRIBUTE_RING_SIZE 0x00020015`, `GE_POS_RING_SIZE 0x2000`, `GE_PRIM_RING_SIZE 0x0c6e07fe`, attr/pos/prim 0x580000/0x400000 apart | identical values | RADV's rings live in its own VMID (VRAM, VA 0xffff8000_00200000); the kext's in VMID0 UC memory. The GL2 hints are the same on a working draw, so W46's variant 2048 is a weak lead |

Not visible in the dump: RADV's queue preamble IB (the chained IB at the start: the gfx12 init state, 328
dwords) is not printed by `dumpibs`. The kext's PHASE 1 already follows `ac_cmdbuf.c` for it; umr with root
is the way to read it if needed.

## Next

1. Boot 3 with this build: `GFXVerdict` should show `cp > 0` and pixels on the baseline draw.
2. If it still reads `cp 0`, the next variant is RADV's exact shader and state: wave64
   (`GS_W32_EN 0`, RSRC1 VGPR granules for wave64), guard band 256, screen offset 8,8.
