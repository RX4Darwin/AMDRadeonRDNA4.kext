# G4: the first triangle with a per-vertex attribute (colour)

2026-09-30. Same triangle as G3 (256x256 linear RGBA8 target, VertexID positions (-0.5,-0.5) (0.5,-0.5) (0,0.5)), but
vertex 0 red, vertex 1 green, vertex 2 blue, smoothly interpolated by the PS. On gfx12 the NGG stage writes the attribute to the
ATTRIBUTE RING in memory and the PS reads it back through LDS with `ds_param_load` + `v_interp_*`: a path the kext has never run.

**Status.** Phase 1 (RADV ground truth) is measured on the card. Phase 2 (the kext's shaders, stream, builder, replay mode) was written and
checked statically and on the host, and then **run on the card once (section 6): the colour triangle is exactly right**, through amdgpu's gfx queue. The CPU
readback of the attribute ring itself stayed at its pre-fill pattern (section 6). The kext does not use G4 yet.

Citations: `RADV-G4` = `docs/hw-logs/2026-09-30-linux-radv-colour/radv-shaders-and-ibs.txt` (RADV_DEBUG=shaders,dumpibs of the colour
triangle, system Mesa 26.2.2, same box as the G3 capture), `RADV-G3` = `docs/hw-logs/2026-09-30-linux-radv-triangle/radv-shaders-and-ibs.txt`.
Mesa source lines are named only where they were read (Mesa 26.2.2 tree).

## 1. Phase 1: RADV draws it (tools/radv-triangle, TRI_COLOR=1)

`TRI_COLOR=1 tools/radv-triangle/run.sh <outdir>` compiles `tri_col.vert` / `tri_col.frag` (the G3 positions; `col = vec3(vid==0, vid==1,
vid==2)`; the PS writes `vec4(col, 1)`; G3's `tri.vert`/`tri.frag` and the default are unchanged) and checks the read-back target against the
exact barycentric colour of every covered pixel. On the RX 9070 XT (RADV GFX1201), both the default and the `nonggc` runs:

    pixels: 8192 covered, bounds x 64..191 y 64..190       stats: ia_verts 3 ia_prims 1 vs_inv 3 c_inv 1 c_prim 4 ps_inv 8192
    colour: 8192 covered pixels (want 8192), 0 with alpha != 0xFF
    colour: max |channel - exact barycentric| 0.50, max |R+G+B - 255| 1, dominant channel = nearest vertex's in 8192/8192 pixels
    colour: near v0 (66,65) 0xff0303f9  near v1 (189,65) 0xff03f903  near v2 (128,188) 0xfff80402  centroid (128,106) 0xff555654
    colour: THE COLOUR TRIANGLE IS RIGHT

Tolerances measured: every channel within **0.5** of `255 * barycentric` (pure rounding of UNORM), R+G+B within **1** of 255. The builder's
check (`userspace/gfx12tricol.h`, `kColChannelTolerance`) allows 3. The pixel centre (128.5,106.5) of the centroid is (0x54,0x56,0x55) = ~85 each.
The same triangle covers exactly the G3 pixels (8192, rows 64..190).

## 2. What RADV does (the ground truth)

### 2.1 The NGG VS (RADV-G4:321-373, wave64; ACO stage HW_STAGE NEXT_GEN_GEOMETRY_SHADER)

Inputs (radv_shader_args.c:784-800, merged GS): `s[0:1]` = the pointer to RADV's ring table (`rw_buffers`; written through
`R_00B210_SPI_SHADER_PGM_LO_GS` on gfx12, radv_queue.c:548-566), `s2` = gs_tg_info, `s3` = merged_wave_info, `s4` = tess_offchip_offset,
**`s5` = gs_attr_offset** (gfx11+), `s6`,`s7` unused, `s8..` = user SGPRs (base vertex), `v0` = packed primitive, `v3` = VertexID.

| ISA (RADV-G4 line) | What it does |
|---|---|
| `s_setprio 3` (321) | wave priority hint (not replicated) |
| `s_pack_ll_b32_b16 s2,0,s3` / `s_bfe_u64 exec,-1,s2` (322-324) | EXEC = vertices, SET from merged_wave_info (ACO's idiom) |
| `v_add_nc_u32 v3,s8,v3` ... `v_cndmask` x4 ... `export pos0 v2,v3,v1,v4 done` (326-338) | the position, as ngg.s; RADV adds the base vertex from user SGPR s8 |
| `s_lshl_b32 s2,s3,8` / `s_wait_expcnt 0` / `s_bfe_u64 exec,-1,s2` / `export prim v0 done` (340-346) | primitive export after the positions |
| `s_bitset0_b32 s1,14` / `s_load_b128 s[12:15], s[0:1], 0xa0` (348-349) | load the **attribute ring descriptor** (entry 10 of the ring table, 10 x 16 B = 0xa0; radv_queue.c:256-350) |
| `s_add_co_u32 s0,s3,7` / `s_and_b32 s0,s0,-8` / `s_pack_ll_b32_b16` / `s_bfe_u64 exec` (350-356) | EXEC = (vertices + 7) & ~7 lanes: full vec4 stores in groups of 8 lanes (ac_nir_prerast_utils.c:485-491) |
| `s_bfe_u32 s3,s3,0x40018` / `s_lshl_b32 s3,s3,6` / `v_mbcnt_lo/hi` (359,363-368) | thread id in the subgroup = wave index [27:24] * 64 + lane (wave32 would shift by 5) = the store's `vindex` |
| `v_cndmask_b32_e64 v4..v6, 0, 1.0, <vid==0/1/2 masks of the position section>` (360-362) | the colour: attribute 0 = (r,g,b), w = v7 (garbage, never read) |
| `s_and_b32 s5,s5,0x7fff` / `s_lshl_b32 s5,s5,9` (364,366) | `soffset` = gs_attr_offset[14:0] * 512 B (ac_nir_lower_intrinsics_to_args.c:310-312) |
| `s_wait_kmcnt 0` / `s_bitset1_b32 s13,20` (369-370) | wait for the descriptor, then OR `stride = 16 * nparams` into dword1 (16 << 16 = bit 20; radv_nir_lower_abi.c:109-118) |
| `buffer_store_b128 v[4:7], v0, s[12:15], s5 idxen scope:SCOPE_DEV` (371) | the attribute store: index `v0`, offset `param 0 * 16`, coherent |
| `s_endpgm` (373) | no wait for the store (`has_attr_ring_wait_bug` is gfx11 only, ac_gpu_info.c:408) |

**The attribute ring descriptor** (`ac_build_attr_ring_descriptor(gfx12, has_desc_resource_level=false, va, size, stride 0)`, radv_queue.c:336-339;
`has_desc_resource_level` is false on gfx1201, ac_gpu_info.c:434-435), computed by running Mesa's own function
(`docs/hw-logs/2026-09-30-linux-radv-colour/attr-ring-descriptor.{c,txt}`):

    dword0 = VA[31:0]            dword1 = VA[47:32] | 0xc0000000 (swizzle_enable 3 = 16 B) | stride << 16 (the VS ORs 16 << 16)
    dword2 = 0x00a80000 (size: total attribute+pos+prim ring bytes; the kext's kRingBytes is the same number)
    dword3 = 0x0043ffac (R32G32B32A32_FLOAT, xyzw, index stride 2 = 32 elements, structured-with-offset OOB)

Layout (swizzled, index stride 32): a subgroup's region starts at `gs_attr_offset * 512`; within it, parameter p of vertex i (i < 32) is at
`p * 16 * 32 + i * 16`. With one parameter a group of 32 vertices is 512 B, which is why gs_attr_offset counts 512 B units.

### 2.2 The PS (RADV-G4:63-76; ACO pseudo code 37-56)

| ISA | What it does |
|---|---|
| `s_wqm_b64 exec, exec` | whole quads: helper lanes must run for the interpolation |
| `s_mov_b32 m0, s2` | `m0` = PrimMask, the first system SGPR after the 2 user SGPRs (RSRC2_PS.USER_SGPR = 2): LDS parameter base + new-primitive mask |
| `ds_param_load v2, attr0.y` / `v3, attr0.x` / `v4, attr0.z` `wait_va_vdst:15 wait_vm_vsrc:1` | SPI has put the ring parameters into LDS; each load fetches one channel of attribute 0 (P0/P10/P20 of the triangle, accessed through the operand slot of the interp instruction) |
| `v_interp_p10_f32 v5, v2, v0, v2 wait_exp:2` (and `v7,v3,...:1`, `v0,v4,...:0`) | first half: `P0 + P10 * i`; `v0` = i (barycentric, PERSP_CENTER); `wait_exp:N` waits until at most N of the 3 param loads are outstanding |
| `v_interp_p2_f32 v4, v4, v1, v0 wait_exp:7` | second half: `+ P20 * j`; `v1` = j |
| `v_fma_mix{hi,lo}_f16 ... dpp` / `v_cvt_pk_rtz_f16_f32` | RADV's fp16 packing of the result for its FP16 MRT export (the kext exports fp32 instead) |
| `export mrt0 v6, v4, off, off done` | compressed (two fp16 pairs) export |

The register encodings of these instructions were checked against the dump: `llvm-mc -mcpu=gfx1201` on `ds_param_load v2, attr0.y wait_va_vdst:15
wait_vm_vsrc:1` gives `ce8f0102`, `v_interp_p10_f32 v5, v2, v0, v2 wait_exp:2` gives `cd000205 040a0102`, `v_interp_p2_f32 v4, v4, v1, v0 wait_exp:7`
gives `cd010704 04020304`, exactly the dump's words.

### 2.3 Registers: RADV's G3 against RADV's G4 (triangle draw, `tools/linux-replay/radv-state-diff.py`; `register-diff-g3-vs-g4.txt`)

Only eight registers differ, and five of them are shader-size or VRS detail:

| Register | RADV-G3 | RADV-G4 | Meaning |
|---|---|---|---|
| `SPI_SHADER_GS_OUT_CONFIG_PS` (s:0x031) | 0x400 (G3:1933) | **0x800** (G4:2152-2157) | `NO_PC_EXPORT` 1 -> 0 (the NGG stage exports a parameter), `NUM_INTERP` 0 -> 1; `VS_EXPORT_COUNT` stays 0 (= params - 1) |
| `SPI_PS_INPUT_CNTL_0` (c:0x199) | not written | **0** (G4:1879-1892) | PS input 0 = attribute slot 0 (OFFSET 0), smooth (FLAT_SHADE 0), no default value, no primitive attribute |
| `SPI_PS_INPUT_ENA` / `_ADDR` (c:0x197/0x198) | 1 = PERSP_SAMPLE (G3:1458,861) | **2 = PERSP_CENTER** (G4:2056,1860) | the barycentrics the PS consumes (i, j in v0, v1); the kext's G3 stream already has PERSP_CENTER |
| `SPI_SHADER_PGM_RSRC1_PS` (s:0x00a) | 0x000c0002 | 0x000cc002 | FLOAT_MODE 0xc0 -> 0xcc (fp16 pkrtz path); VGPRS 2 (wave64, 12 VGPRs) |
| `SPI_SHADER_PGM_RSRC4_PS` / `_GS` (s:0x007/0x088) | INST_PREF_SIZE 2 / 3 | 3 / 5 | shader size prefetch hints; the kext keeps 0 (no prefetch, code padded with s_code_end) |
| `PA_SC_VRS_OVERRIDE_CNTL` (c:0x0f4) | 0x51 | 0 | VRS combiner; the kext already has 0 |

Identical in RADV's G3 and G4 (so not delta for G4): `SPI_SHADER_PGM_RSRC2_PS` = 4 (USER_SGPR 2) and `RSRC2_GS` = 4 at the triangle draw
(G3:1915,1875; G4:2134,2094), `SPI_BARYC_CNTL` = 0 (G3:1820, G4:2011), `SPI_PS_IN_CONTROL` = 0 (wave64 PS; G3:879), `PA_CL_VS_OUT_CNTL` = 0 at the draw
(G3:1647), `SPI_SHADER_POS_FORMAT`, the rings (`SPI_ATTRIBUTE_RING_BASE/SIZE`, `GE_POS/PRIM_RING_*`: G4:460-471), the user SGPRs
(`SPI_SHADER_USER_DATA_GS_0..2` = 0, `_PS_2..5` = 0; `_PS_0/1` = the ring table pointer, G4:486-487) and `VGT_SHADER_STAGES_EN`. RADV never writes
`SPI_INTERP_CONTROL_0` in either dump (see uncertainty 3).

## 3. The kext's version

| File | What |
|---|---|
| `shaders/nggcol.s` -> `src/nggcol_kernel.h` | wave32 NGG VS: ngg.s (EXEC SET from merged_wave_info, never ANDed; pos0 then `s_wait_expcnt 0` then prim) plus the attribute store above: EXEC = (vertices + 7) & ~7, colour `v8..v10` from the vid masks, `v12` = wave index * 32 + lane, `s5 = (s5 & 0x7fff) << 9`, the descriptor built in `s12..s15` from three patched literals and the constant dword3, `buffer_store_b128 v[8:11], v12, s[12:15], s5 idxen scope:SCOPE_DEV`. 13 VGPRs |
| `shaders/pscol.s` -> `src/pscol_kernel.h` | wave32 PS: RADV's sequence for three channels, `s_mov_b32 m0, s2`, `s_wqm_b32`, `ds_param_load` x3, `v_interp_p10/p2`, alpha 1.0, `export mrt0 v5..v8` as 4 x fp32 (COL_FORMAT 32_ABGR like psred.s). 9 VGPRs |
| `tools/gen-gfx12-draw.py header-col` -> `src/gfx12_draw_col.h` | the G3 stream (byte-identical regeneration of `gfx12_draw.h` checked) plus the deltas below; Col-prefixed names, namespace `Gfx12DrawCol`, C and C++ |
| `userspace/gfx12tricol.h` | `rdna4_tricol_place_shaders(code, attrRingVa)` (patches the three descriptor literals), `rdna4_tricol_record` (same alignment/range/relocation logic as gfx12tri.h, plus the end-of-buffer checks), `rdna4_tricol_check` (phase 1's colour check) |
| `userspace/test-gfx12tricol.c` | host test, no GPU (C11 gcc and clang): literals patched, stream records and walks, registers present, checker accepts the exact image and rejects flat red / a missing pixel / a wrong colour / a wrong alpha |
| `tools/linux-replay` `REPLAY_DRAW=col` | see section 5 |

**Register deltas of the kext's G4 stream against its G3 stream** (each is also in the generator with its citation; `python3 tools/gen-gfx12-draw.py table-col`):

| Register | G3 | G4 | Why / citation |
|---|---|---|---|
| `SPI_SHADER_GS_OUT_CONFIG_PS` | 0x400 | 0x800 | RADV-G4:2152-2157 (above) |
| `SPI_PS_INPUT_CNTL_0` | unset | 0 | RADV-G4:1879-1892; the kext must write it (an unwritten context register holds power-up garbage) |
| `SPI_SHADER_PGM_RSRC1_GS` | 0x000c0000 | 0x000c0001 | nggcol.s needs 13 VGPRs: VGPRS = granules of 8 - 1 (the same patch the marker shaders get, gfxring.cpp variant 32) |
| `SPI_SHADER_PGM_RSRC1_PS` | 0x000c0000 | 0x000c0001 | pscol.s needs 9 VGPRs |
| `SPI_SHADER_PGM_RSRC2_PS` | 0 | 4 (USER_SGPR = 2) | RADV-G4:2134-2140: PrimMask is the first system SGPR after the user SGPRs, so `s2` in RADV's PS; the kext matches RADV instead of relying on `s0` |
| `SPI_SHADER_USER_DATA_PS_0/1` | unset | 0 | the two user SGPRs the PS now has (RADV-G4:486-487 writes the ring table pointer; pscol.s does not read them) |

No delta (already equal to RADV's G4): `SPI_PS_INPUT_ENA/ADDR` (PERSP_CENTER), `PA_SC_VRS_OVERRIDE_CNTL` (0), `SPI_SHADER_COL_FORMAT` (32_ABGR, psred.s's export),
`SPI_SHADER_PGM_RSRC4_*` (0, prefetch off), the attribute/position/primitive ring registers (G3 already programs them), `VGT_SHADER_STAGES_EN` (wave32 passthrough),
`SPI_PS_IN_CONTROL` (wave32 PS). The stream is 492 dwords (G3: 489): the three new registers merge into existing SET_*_REG packets.

The attribute ring's VA must be the address the ring base registers point at (`SPI_ATTRIBUTE_RING_BASE = VA >> 16`): `rdna4_tricol_place_shaders` is given
the same `rings.va` the stream's relocation uses.

## 4. What was uncertain, and what the first run resolved

The list below was written before the first run (section 6). Status after it:

1. **The descriptor's `size`/`stride` and the swizzle layout are copied from RADV, not measured for our ring.** *Resolved functionally:* the PS read back per-vertex colours
   that are exact to 0.50, so what nggcol.s stored and what SPI read agree on the layout. Not observed directly: *where* in the ring the data went (section 6).
2. **`s5` really is gs_attr_offset under our configuration.** Mesa says so (radv_shader_args.c:797-798) for merged NGG on gfx11+, and RADV's dump works on this card
   with USER_SGPR 2. Probed on the card with the plain G3 stream (`shaders/nggsgpr.s`, `REPLAY_VS=file:<hex> REPLAY_DUMP_MARKER=1`, 10 runs, 8192 px each): the NGG wave's
   initial `s0 s1 s2 s3 s4 s5` = `0 0 0x00403000 0x10000103 <x> 0`, with `s5 = 0` in 10/10 runs and `s4` (tess_offchip_offset, unused) 0 in 6, 0x8 in 1 and 0x88888888 in 3 (so
   the unused system SGPRs are not reliably zeroed, which makes a constant `s5 = 0` more credible than stale). `s2`/`s3` reproduce the earlier measurement and `s0/s1 = 0` (nothing
   programs the GS pointer registers in the G3 stream). `(s5 & 0x7fff) << 9 = 0`: the first subgroup sits at the start of the attribute ring, well inside 0x580000. **Caveat:** G3
   exports no parameter (`NO_PC_EXPORT = 1`), so the hardware may have no reason to allocate ring space; in G4 (`NUM_INTERP = 1`) `s5` can differ.
   *Still open for a G4 draw:* the ring listing of the first run was empty, so it did **not** confirm `s5`'s value for G4; `s5 = 0` was measured only in G3. The correct image is indirect
   evidence that the store offset and SPI's read offset agree (a wrong offset would make the PS read other data), not a measurement of the value.
3. **`SPI_INTERP_CONTROL_0`** (kext 0x869, RADV unwritten). *Resolved:* the image is smoothly interpolated (every pixel within 0.50 of the barycentric colour), so 0x869 with
   `SPI_PS_INPUT_CNTL_0.FLAT_SHADE = 0` does not flat-shade. RADV's effective value stays unknown, but it no longer matters for this stream.
4. **PrimMask in `s2`** via RSRC2_PS.USER_SGPR = 2 with written zeros. *Resolved:* the LDS parameter loads returned the right colours.
5. **wave32 NGG + wave32 PS** (RADV: wave64 both). *Resolved for this configuration:* it draws correctly, including the wave32 thread-id shift.
6. **`s_setprio 3`** and the `s_delay_alu` hints are not replicated. No visible effect (performance hints).
7. The emulator cannot run G4 (no `ds_param_load`, `v_interp`, buffer stores). *Open, not in scope so far.*
8. **New, open: the attribute ring memory did not change as the CPU sees it** (section 6): where the data lives, and whether the same stream works on the kext's macOS path
   (bare RB0, VMID0, UC memory), are untested.

## 5. The first G4 replay run (the plan, as written before it was run; result in section 6)

`REPLAY_DRAW=col REPLAY_COL_OK=1 tools/linux-replay/build/replay` (after `tools/linux-replay/run.sh` has built it; no other `REPLAY_*` variable is accepted with it):

1. opens the amdgpu render node, creates a context, and allocates: code BO (VRAM, CPU-visible), target (256 KiB VRAM), the **ring block (0xA80000 B, 2 MiB aligned, VRAM,
   CPU-visible, filled with 0xee)**, a GTT page for the fence/statistics, the IB BO;
2. places `nggcol.s` with the ring VA in its descriptor literals and `pscol.s`; records the 492-dword stream through `rdna4_tricol_record`; IB = PIPELINESTAT_START,
   SAMPLE_PIPELINESTAT, the stream, SAMPLE_PIPELINESTAT;
3. submits one CS on the gfx ring and waits up to 5 s for the fence; it then prints the colour check (`THE COLOUR TRIANGLE IS RIGHT` or the measured errors), the fence, the
   dwords of the attribute ring that changed from 0xee (first 48 listed) and the pipeline statistics. Exit 0 only if the image is right and the submission completed.

What can go wrong: a wrong descriptor or `s5` makes the store fault or land elsewhere (the ring listing shows where; a VM fault would hang the gfx queue, which amdgpu resets as
it did once today for an over-budget-VGPR probe), a wrong PrimMask / LDS read gives garbage colours but should not hang, an unwritten context register could do anything the G3 stream's
register-by-register discipline was meant to prevent. Expected if right: 8192 px, colours as in section 1, ring dwords changed = the three vertices' vec4 at `gs_attr_offset * 512`
(plus the padding lanes up to 8 vertices) and nothing else.

## 6. First run on the card

2026-09-30 16:21:55, run by the lead with the user's OK: tree `a1b5a1f`, `REPLAY_DRAW=col REPLAY_COL_OK=1 bash tools/linux-replay/run.sh`
(`docs/hw-logs/2026-09-30-linux-g4-first-run.txt`; transcribed from the lead's report, the raw output was not saved).

| | |
|---|---|
| colour check | **THE COLOUR TRIANGLE IS RIGHT**: 8192 covered px, 0 bad alpha, 0 not dominated by the nearest vertex's colour |
| errors | max `|channel - exact barycentric|` 0.50, max `|R+G+B - 255|` 1 (the same as RADV's) |
| spot pixels | near v0 `0xff0303f9`, near v1 `0xff03f903`, near v2 `0xfff80402`, centroid (128,106) `0xff555654`: **identical to RADV's colour triangle** |
| fence / reset | fence 1, context reset state 0, exit 0 |
| statistics | PS 8192, C_PRIM 4, C_INV 1, VS 3, IA 1/3 (as G3 and RADV) |
| attribute ring (CPU) | **0 dwords changed from the 0xee pre-fill** |
| kernel journal | no amdgpu message since the run: no VM fault, no ring timeout, no reset |

**What this establishes.** The attribute path works end to end on this card with the kext's own shaders and stream: nggcol.s's `buffer_store_b128` with a descriptor built in SGPRs
reached a place SPI could read, SPI put the parameters into LDS, and pscol.s got the per-vertex colours through `ds_param_load` + `v_interp_p10/p2` (PrimMask in `s2` from RSRC2_PS.USER_SGPR = 2) and
exported them: the PS received vertex 0 red, 1 green, 2 blue, interpolated exactly. The register deltas of section 3 are sufficient, and the wave32 NGG/PS configuration is fine for attributes.

**What it does not establish.** The replay's listing of the attribute ring, read by the CPU after the fence, shows the 0xee pattern untouched, although the colours demonstrably went through some
ring-like path. So the run did not show *where* the store landed, and it **did not confirm `s5`'s value for a G4 draw**: `s5 = 0` (first subgroup at the ring base) was measured only in G3 (uncertainty 2).
No fault in the journal means the store did not go out of range; it says nothing more about the address.

**Leading hypothesis (UNVERIFIED, not a fact).** The attribute data stayed in the GPU's L2 (GL2) and was never written back to VRAM during the run, so the CPU, which reads VRAM through the BAR,
still sees the pre-fill. The ring registers carry GL2 hints (`SPI_ATTRIBUTE_RING_SIZE.L1_POLICY = 1`; `GE_PRIM_RING_SIZE` with its `SCOPE`/`TEMPORAL`/`NOFILL` bits; the store is `scope:SCOPE_DEV`), and the
attribute ring is transient by design (written by the NGG stage, consumed by SPI/PS of the same draw; nothing forces a write-back). Alternatives not excluded: the store went to a different address than the
one the listing scans (the listing covers the first `kColAttrRingBytes` = 0x580000 of the ring block only), or the data travels through a path that never touches this BO. **Against it:** the stream's own end-of-pipe RELEASE_MEM already has `GL2_WB = 1` (gen-gfx12-draw.py, the fence packet), which should have written dirty GL2 lines back before the fence, so if the hypothesis
is right the ring data must be in a state that write-back does not cover. How to tell: an explicit GL2 write-back + invalidate (`ACQUIRE_MEM`) after the draw, then the CPU read, a listing of the whole 0xA80000 block, or a run of RADV's colour triangle with the same kind of read of its ring.
Until one of these is done this stays a hypothesis.
