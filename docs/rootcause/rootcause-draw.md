# Root cause: the first gfx12 draw writes 0 pixels (RX 9070 XT, round 4)

Read-only review. Nothing edited, no VM run. Tags: **[F]** fact with a citation, **[I]** inference, **[G]** guess.
Sources: kext `RDNA4FB-int/src`. amdgpu `~/src/linux-amd/drivers/gpu/drm/amd` (d266640), paths relative to `amdgpu/` unless given.
Mesa `~/src/mesa/src` (eda9ace). Logs `premetal/hw-logs/rdna4fb-diag-20260929-081934.txt` (**L4a**) and `-083603.txt` (**L4b**).

## 0. Verdict

- **#1 (about 35%).** The gfx context state is never initialized on this path.
  - On silicon, the context registers sit in per-context SRAM that powers up with garbage. The clear state that zeroes the registers Mesa never writes (`clearstate_gfx12.h`) is handed to the RLC, but nothing makes it land: the kext never enables SRM, and there is no MES queue map.
  - The QEMU model zero-fills unwritten registers (notes §6.2), so it draws.
  - Fix: replay the CSB as `SET_CONTEXT_REG` before the draw, zero-fill the rest, and enable `RLC_SRM_CNTL` as amdgpu does.
- **#2 (about 25%).** The legacy RB0 queue is one that amdgpu never runs on gfx12. It has no MQD/HQD, and VA 0 is unmapped in VMID0, so the CPG reads and writes VA 0.
- **#3 (about 15%).** VMID0 uses MTYPE_UC, but the GE position/primitive rings carry GL2 "stay-dirty / no-fill" hints. Mesa's rings are MTYPE_NC.
- **#4 (about 10%).** The NGG wave never launches, or gets different inputs than assumed. Nothing yet tells launch from cull, so a VS marker is the first thing to add.
- **Ruled out:** the stream itself. It matches Mesa for every register it writes (§2).

## 1. What the evidence pins down

1. **No pixel wave ran [F].**
   - The PS marker is `NOT written` (L4b:367).
   - The target stays empty after 2 ms and after a full GL2 write-back (L4a:353).
   - The CB state asks for a PS: `CB_TARGET_MASK=0xf`, `CB_SHADER_MASK=0xf`, `CB_COLOR0_INFO` FORMAT=0xa (8_8_8_8) (L4b:362-363; field layout from `amd/registers/gfx12.json`).
   - [I] So either the NGG wave never ran, or its primitive was dropped before the SC. The CB/GL2/readback path is downstream of the first failure.
2. **The draw completes without hanging [F].** The fence is 1, `CP_STAT 0`, `SPI_BUSY 0`, GRBM idle (L4a:339,341). [I] Any wave that did launch also ended.
3. **Only the first fault is visible [F].** `GCVM_L2_PROTECTION_FAULT_STATUS` is sticky: `MORE_FAULTS=1` is already set before the draw (L4a:335; decoded in w31-review §1).
   - [I] So a GE1/GE2/PA/SQC fault *during* the draw would be invisible in round 4. Round 5 clears the status per step.
4. **The IV pairs are not proven to recur per submission [F].**
   - `ih.cpp:817-819` logs at most 4 VM-fault IVs (`faultIvLogs < 4`).
   - Pair 1 comes at the draw kick (L4a:337,340). Pair 2 comes at the flush submission, which is `ACQUIRE_MEM`+`RELEASE_MEM` with no draw (L4a:351-352; `gfxring.cpp:880-882`).
   - [I] So the read+write at VA 0 is tied to cache-control/EOP packets, not to the NGG rings (those are at 0x8010xxxxxx).
   - [F] The first CPG read fault was latched during gfx bring-up, which has no `ACQUIRE_MEM` (L4a:270 no fault → L4a:335).
5. **Context registers are backed by full-width SRAM [I].**
   - **CORRECTION (verify-draw.md, W41): this readback is AFTER draw 1, not before it** (the draw fence is 1 at L4a:341, before the readback at L4a:345), and the registers it shows are ones the stream WRITES. None of the 62 CSB registers was read in round 4, so the CSB-specific half of this hypothesis has no direct evidence. Original text: the readback shows values with bits set where gfx12 defines no field. Example: `VGT_SHADER_STAGES_EN=0xfd1ffe88` (L4a:345), when the register only has bits 2,5,19,21,22,24,26 (`gfx12.json`).
   - The values differ between boots (L4b:343).
   - Unwritten SH registers are garbage too: `USERDATA_PS0/1` (L4a:344).
   - [I] So SRAM powers up with garbage, and an unwritten register is *not* 0 on silicon.
6. **The baseline readback holds `PA_SU_SC_MODE_CNTL=0x00010ecf`, which is CULL_FRONT|CULL_BACK (L4a:347) [F].**
   - [I] If the first draw ran with that context, it culled everything.
   - The later draws read the stream's values (L4b:362-366) and still drew nothing (L4b:409).
   - [I] So either a register *outside* the probed set is wrong in every context (context rolls copy it forward), or the cause is not state.

## 2. The stream is Mesa's: checked register by register [F]

Each value is decoded against `gfx12.json` fields and compared with the Mesa source that emits it.

| Register (stream value) | Decode | Mesa |
|---|---|---|
| VGT_SHADER_STAGES_EN 0x04400000 | GS_W32_EN, PRIMGEN_PASSTHRU_NO_MSG | `si_state_shaders.cpp` gfx12 branch (`S_028A98_*`) |
| GE_CNTL 0xa0010080 | 128/128 per subgroup, PRIM_GRP_SIZE 256, DIS_PG_SIZE_ADJUST | same file, `ge_cntl` gfx11+ |
| RSRC4_GS 0x007f0bff | WAVE_LIMIT 0x3ff, GLG_FORCE_DISABLE, LATE_ALLOC 127 | `S_00B220_*` gfx12 branch |
| SPI_SHADER_GS_OUT_CONFIG_PS 0x400 | NO_PC_EXPORT=1, NUM_INTERP=0 | `S_00B0C4_*` (`si_state_shaders.cpp:1220-1222,1769`) |
| GE_POS/PRIM_RING_* (readback L4a:348) | MEM_SIZE = per-SE size >> 5, scope and temporal bits | `ac_cmdbuf_cp.c:311-327` |
| DB_RENDER_OVERRIDE 0x1000, OVERRIDE2 0x08000000 | FORCE_STENCIL_READ; CENTROID_COMPUTATION_MODE=1 | `si_state.c:1847-1858` |
| DB_SHADER_CONTROL 0x10, SPI_PS_IN_CONTROL 0x8000 | Z_ORDER early-then-late; PS_W32_EN | `si_state_shaders.cpp` `si_shader_ps` |
| CB_COLOR0_INFO 0x2800a, ATTRIB3 0x01000000 | 8_8_8_8 UNORM, BLEND_CLAMP, SIMPLE_FLOAT; SW_MODE linear, 2D | gfx12 CB emit |
| PA_SU_SC_MODE_CNTL 0x80240, PA_CL_CLIP_CNTL 0x01000000 | no cull; DX_LINEAR_ATTR_CLIP_ENA | `si_state.c` rasterizer |
| RSRC3_PS/GS/HS 0xffff / 0xfffffdfd / ~0 | plain `SET_SH_REG` | gfx12 uses `ac_pm4_set_reg`, not idx3 (`ac_cmdbuf.c:658-684`) |

- **Packets [F].**
  - The classic `SET_CONTEXT/SH/UCONFIG_REG` packets are valid on gfx12 (`amd/packets/cp_pm4_table_data_gfx12.json` pfp entries).
  - radeonsi writes `VGT_PRIMITIVE_TYPE` with a plain `SET_UCONFIG_REG` on gfx10+ (`si_state_draw.cpp:1277-1278`).
  - The gfx12 PFP table marks `SET_UCONFIG_REG_INDEX` index 1 as `reserved1`, so the CP no longer tracks the primitive type.
- **NGG ABI [F].** On gfx12, VertexID is in v3 after 3 GS VGPRs, and merged_wave_info is in s3. radeonsi (`gfx/si_shader_args.c:303-371`) and RADV (`radv_shader_args.c:855-859`) agree.
- [I] The per-register content is not the bug. The difference is in what surrounds the stream.

## 3. Silicon + kext versus Mesa on amdgpu (what the model cannot see)

| | Mesa on amdgpu gfx12 | kext |
|---|---|---|
| Queue | Kernel gfx ring mapped with an MQD by MES/KIQ. Defaults: `amdgpu_async_gfx_ring=1` (`amdgpu_drv.c:223`), `gfx_v12_0.c:3568-3575`; MES always on for 12.0.1 (`amdgpu_discovery.c:2993-2998`); map at `amdgpu_gfx.c:812-840` | Bare `CP_RB0_*` (`gfxring.cpp:99-157`); no MQD/HQD registers |
| Clear state | CSIB to the RLC, **then `RLC_SRM_CNTL \|= SRM_ENABLE\|AUTO_INCR_ADDR`** (`gfx_v12_0.c:2104-2112`, `2000-2009`) | CSIB only (`gfxring.cpp:71-97`); SRM never written (grep `src/`: no `Srm`) |
| VMID of the draw | ≥1, PTE MTYPE_NC by default (`gmc_v12_0.c:518-528`) | 0, system aperture MTYPE_UC (`compute.cpp:2267-2271`) |
| VA 0 in VMID0 | GART, placed low (`gmc_v12_0.c:700`, `amdgpu_gmc.c:325-327`): a stray access is silent | page 0 of a 1-page table with PTE 0 (`compute.cpp:2235-2246`), which faults to the shared scratch page (`:2254-2265`) |
| Per-job preamble | `CONTEXT_CONTROL` with the load bits on a context switch (`gfx_v12_0.c:4655-4673`) | only the stream's 0x80000000/0x80000000 |

## 4. Ranked candidates

### #1 The clear state is never applied, so the context SRAM keeps garbage (about 35%)

- **Why it fits.**
  - [F] gfx12 has no `CLEAR_STATE` packet, and amdgpu does not replay the CSB through the ring (`gfx_v12_0.c:2699-2710`).
  - [F] Mesa sets `has_clear_state = false` for gfx11+ (`ac_gpu_info.c:1122`) and writes only what it tracks.
  - [F] The CSB (`clearstate_gfx12.h`, 6 extents, 62 regs) is exactly a list of registers **Mesa never writes** and that must be zero:
    - `PA_RATE_CNTL`, `CONTEXT_RESERVED_REG0/1`, `PA_SC_BINNER_OUTPUT_TIMEOUT_CNTL`, `PA_SC_BINNER_DYNAMIC_BATCH_LIMIT`;
    - `PA_SC_VPORT_0..15`, `PA_CL_PROG_NEAR_CLIP_Z`, `PA_SC_CLIPRECT_n_EXT`, the HiZ/HiS base/size registers.
    - Mesa's own shadowing table flags `PA_RATE_CNTL` as special for RS64 (`amd/common/ac_shadowed_regs.c:652`).
  - [I] So on real hardware the RLC/CP firmware applies that CSB to the contexts.
  - [G] The trigger is part of the path the kext skips: SRM enable and/or the MES queue map.
  - [F] Section 1 items 5-6: SRAM garbage is real, unwritten SH/context registers are garbage, and rolls copy it to every later draw.
  - [F] The model treats unwritten registers as 0 (`premetal/gfx12-draw-notes.md` §6.2). That is exactly the leniency.
- **Why it could be wrong.**
  - [G] Mesa also leaves non-CSB registers unwritten (for example `SPI_TMPRING_SIZE`: `si_gfx_cs.c:563-564` only writes it with a scratch buffer). So those are either hardware-reset or harmless.
  - The CSB registers' effect on raster is undocumented.
- **Exact fix** (ring packets before the draw IB, so no reloc shifts, next to the `SET_SH_REG` at `gfxring.cpp:817-821`):
  1. `SET_CONTEXT_REG` per CSB extent, all values 0 (build them from `gfx12_cs_data` as `gfxCsbInit` does):
     - `C0226900 0000003E` + 34×0
     - `C0026900 000000CC 0 0`
     - `C0016900 000000D8 0`
     - `C0066900 000000DB` + 6×0
     - `C00B6900 000002E5` + 11×0
     - `C0086900 000003C0` + 8×0
     - That is 74 dwords. The stream's later writes (SC_MEM_*, HIZ/HIS_INFO, HISZ_CONTROL=2, CB_MEMx_INFO) override the rest.
  2. Mirror `gfx_v12_0_rlc_enable_srm`: after `gfxCsbInit`, `RLC_SRM_CNTL` (GC seg 1, 0x4c80) |= 0x3 (`gc_12_0_0_sh_mask.h:20337-20338`).
  3. Wider variant: zero every other gfx12 context register the stream does not write (413 in `gfx12.json` 0x28000-0x28FFF).
     - Exclude the triggers and DMA registers: `VGT_DRAW_INITIATOR` 0x287f0, `VGT_EVENT_INITIATOR` 0x28a90, `VGT_EVENT_ADDRESS_REG` 0x287f8, `GFX_COPY_STATE` 0x287d0, `CP_CP_PIPEID/VMID` 0x28364/8, `COHER_DEST_BASE*`, `VGT_DMA_*`.
     - Keep `PA_SC_RASTER_CONFIG(_1)` at its reset value.
     - [F] Of the 355 of these with a known gfx11 default (`include/asic_reg/gc/gc_11_0_0_default.h`), only `PA_SC_RASTER_CONFIG` (0x2a00126a) is non-zero. So zeroing the rest equals a reset.
- **Cheapest test.**
  - Read-only, zero risk: after the **second** draw (when MMIO shows the stream's context, L4b:362), read these seg-1 registers:
    - `PA_RATE_CNTL` 0x00cd, `CONTEXT_RESERVED_REG0/1` 0x00db/0x00dc, `PA_SC_CLIPRECT_0_EXT` 0x00dd;
    - `PA_SC_BINNER_OUTPUT_TIMEOUT_CNTL` 0x02ed, `..._DYNAMIC_BATCH_LIMIT` 0x02ee, `PA_SC_VPORT_0_TL` 0x0040, `PA_SC_RASTER_CONFIG` 0x00d4;
    - plus a dump of 0x0000-0x03ff.
  - Non-zero there means the CSB never landed. Then run fix 1+2 as a new `rdna4-gfxdiag` bit.

### #2 A bare RB0 queue that gfx12 firmware is not used to; VA 0 unmapped in VMID0 (about 25%)

- [F] amdgpu never runs gfx12 on RB0 (table §3). Its kernel queue's MQD carries shadow/CSA/fence = 0 but a real `cp_mqd_base_addr` (`amdgpu_ring.c:823-854`, `gfx_v12_0.c:3002-3097`).
- [F] The CPG reads VA 0 during bring-up; reads and writes at VA 0 go with cache-control/EOP submissions (§1 item 4).
- [F] The gfx pipe has several address registers the kext never programs:
  - `CP_GFX_MQD_BASE_ADDR` (seg 0 0x1e7e/0x1e7f), `CP_MQD_BASE_ADDR` (0x1fa9), `CP_GFX_HPD_OSPRE_FENCE_ADDR_LO/HI` (0x1e74/0x1e75), and the `CP_GFX_HQD_*` block (`gc_12_0_0_offset.h:3664-3730`).
- [I] In amdgpu VA 0 is a mapped GART page, so the same stray access there is silent.
- [G] If draw bookkeeping in the ME firmware (context rolls, preemption state) lives in a structure at base 0, then with the 4 KiB default page shared and aliased (w30-review:38) it could drop draws while ring tests pass.
- Against: MEC compute works with the same RS64 core and never logged a CPC fault.
- **Test.** Replace the zero PTE at `compute.cpp:2235-2246` with a valid PTE to a private zeroed 4 KiB VRAM page (`VALID|READABLE|WRITEABLE`, `amdgpu_vm.h:57,67,68`).
  - This mirrors amdgpu, and the fault disappears.
  - After bring-up and the draw, dump that page. The dwords the CPG wrote fingerprint the structure (rptr, fence, MQD-like).
  - Log the MQD, HPD and RS64 `DC_BASE`/`LOCAL_BASE0` registers above. The ones reading 0 are the candidates.
- **Fix, if confirmed.** Program the gfx HQD like `gfx_v12_0_gfx_mqd_init` (`gfx_v12_0.c:3002-3097`) with a real MQD page. The full fix is MES/KIQ `MAP_QUEUES`.

### #3 GE position/primitive rings in UC memory with GL2 hints (about 15%)

- [F] Mesa programs `GE_PRIM_RING_SIZE` with `PAF_TEMPORAL=high_temporal_stay_dirty`, `PAB_TEMPORAL=last_use_discard`, `FORCE_SE_SCOPE`, `PAB_NOFILL` (`ac_cmdbuf_cp.c:319-326`). That design keeps the data resident in GL2.
- [F] Mesa's ring is NC (`gmc_v12_0.c:518-528`). The kext's is VMID0 UC (`compute.cpp:2267-2271`).
- [G] If a NOFILL read misses GL2 because UC bypassed it, the PA reads zeros: null or degenerate primitives, culled with no hang and no PS. The model has no GL2.
- **Test (one dword).** Variant `GE_PRIM_RING_SIZE = 0x000007fe` (MEM_SIZE only), patched through `findStreamReg(kStream, n, 0x79, 0x26b)`. The stronger test is the draw IB in a VMID ≥ 1 with NC PTEs.

### #4 NGG launch or its inputs on silicon (about 10%, but instrument this first)

- [F] Nothing in round 4 tells "no NGG wave" apart from "wave ran, primitive culled".
- **Test.** A VS variant that stores before its exports:
  - `s_mov_b32 s24/s25` with the marker address as literals, `v_mbcnt_lo_u32_b32 v8,-1,0`, `v_lshlrev_b32 v8,4,v8`;
  - `v_mov` s2, s3, v0, v3 into v9..v12, then `global_store_b128 v8, v[9:12], s[24:25]`.
  - Needs `RSRC1_GS.VGPRS=1`: the wave has only 8 VGPRs at 0.
  - Expect s2=0x00403000, s3=0x10000103, v0=0x00080200|edges, v3 = 0/1/2 (notes §3.2).
- **Pipeline statistics, same boot.**
  - `EVENT_WRITE PIPELINESTAT_START` = `C0004600 00000019`.
  - Before and after the draw: `EVENT_WRITE SAMPLE_PIPELINESTAT` = `C0024600 0000021E lo hi` (events 25/30: `gfx12.json:693,698`; packet: `si_query.c:854-857`).
  - Result: 14 × u64 (`si_query.c:613-633`): PS_INVOCATIONS @0, C_PRIMITIVES @8, C_INVOCATIONS @16, IA_PRIMITIVES @48, IA_VERTICES @56.
  - [I] Reading it: IA=0 means the GE dropped the draw; IA=1 with C_INV=0 means NGG/export; C_INV=1 with C_PRIM=0 means clip/cull; C_PRIM=1 with PS=0 means SC/scissor/raster state (#1).

### Lower

- **RS64 `DC_BASE` = 0** (w31 §3.1). [I] Unlikely: a 256 KiB data/stack image aliased onto one 4 KiB default page would break more than draws, and MEC with the same loader works. Round 5 reads it anyway.
- **`VGT_PRIMITIVE_TYPE` reads 0 while the other UCONFIG ring registers read back (L4a:345,348) [F].** [I] Probably not readable by MMIO. Round 5's COPY_DATA probe (`gfxring.cpp:461`) settles it. If the CP also reads 0 there, it jumps to #1 priority.
- **Goldens (rev_id 1), `INST_PREF_SIZE`, `USER_SGPR` [F].** Tested: 0 px (L4b:409).

## 5. The brief's list, one line each

1. VA-0 read+write: #2. It comes from cache/EOP packets, not the draw (§1 item 4). Async/MQD: amdgpu's kernel MQD has shadow/CSA = 0 as well, so the difference is the MQD/HQD itself, not those pointers.
2. Pipe init:
   - `cp_gfx_start` is only `CP_MAX_CONTEXT` and `CP_DEVICE_ID` [F `gfx_v12_0.c:2699-2710`].
   - `GRBM_CNTL` (`compute.cpp:2972`) and VMID0 `SH_MEM_CONFIG` (`compute.cpp:3100`) are done.
   - `setup_rb` only computes masks [F `:1749-1782`]. There is no raster config on gfx12.
   - Missing: **SRM enable** (#1).
3. Preamble: equal to `ac_cmdbuf.c:629-845` and `si_state.c:5042-5107`. Neither `_INDEX` nor PAIRS is required (§2).
4. CB/DCC/MTYPE: no PS ran, so the CB is not first. DCC is off (no PTE DCC bit in VMID0). MTYPE matters for #3.
5. PS export config: correct (§2 table). The barycentric requirement is met (`SPI_PS_INPUT_ENA=2`).
6. Readbacks: the garbage-context signature supports #1 (§1 items 5-6).

## 6. Cheapest real-card plan (one boot, then one fix boot)

- **Boot A: read-only plus instrumentation.**
  - The #1 register reads and the full seg-1 dump after the second draw.
  - The #2 register reads.
  - The #4 VS-marker variant and the pipeline statistics.
  - The round-5 probes as planned.
- **Boot B:**
  - #1 fix (CSB replay + SRM) as the baseline.
  - Then variants: #1 wide zero-fill, #3 `GE_PRIM_RING_SIZE=0x7fe`, #2 VA-0 page mapped (bring-up change, so its own boot if the lead wants isolation).
- **Decision rule.**
  - A shows garbage in the CSB registers and B draws: #1 confirmed.
  - The pipeline statistics show C_INV=0: go to #3/#4.
  - IA=0: #2 or queue mode.
