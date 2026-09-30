# W37: the first draw after `premetal/rootcause-draw.md` — what the kext now does and what round 5 shows

Round 4 drew 0 pixels on the card, the PS marker was not written, and the emulator drew. `premetal/rootcause-draw.md` ranks four causes. This branch
(`premetal/gfx5`) acts on all of them; boot 3 (`rdna4-gfx=2 rdna4-gfxprobe=1 rdna4-gfxdiag=11`) carries every new piece.

## 1. #1: the clear state never lands (about 35%)

Facts: gfx12 has no `CLEAR_STATE` packet; with PSP-loaded firmware amdgpu's `gfx_v12_0_rlc_resume` runs `gfx_v12_0_init_csb` **and** `gfx_v12_0_rlc_enable_srm`
(`RLC_SRM_CNTL |= AUTO_INCR_ADDR | SRM_ENABLE`, `gfx_v12_0.c:2005-2008`, called at `:2106-2112`). The kext handed the RLC the CSB and never touched `RLC_SRM_CNTL`.
The round-4 readback `VGT_SHADER_STAGES_EN=0xfd1ffe88` has bits where gfx12 defines no field and differs per boot: context registers power up as SRAM garbage.

- **(a) SRM** (`gfxSrmEnable`, right after the CSB init): `RLC_SRM_CNTL` (GC seg 1 dword `0x4c80`) `|= 3`, logged `SRM: RLC_SRM_CNTL 0x.. -> 0x..`. `rdna4-gfxsrm=0` skips it.
- **(b) CSB replay** (`gfxEmitCsbReplay`): before every draw the six CSB extents (62 registers, 74 ring dwords) are emitted as `SET_CONTEXT_REG` with the CSB's own values (all zero), so the draw
  does not depend on SRM (`CSB replay: 6 extents, 62 registers, 74 ring dwords`). The stream's later writes override the overlaps. `rdna4-gfxcsb=0` skips it.
- **Evidence:** the CP-side probe (`draw probe mid/post`) now also reads 7 clear-state registers the stream never writes (`PA_RATE_CNTL`, `CONTEXT_RESERVED_REG0/1`, `PA_SC_CLIPRECT_0_EXT`,
  `PA_SC_BINNER_OUTPUT_TIMEOUT_CNTL`, `..._DYNAMIC_BATCH_LIMIT`, `PA_SC_VPORT_0_TL`) plus `PA_SC_RASTER_CONFIG`, and prints `N of 7 clear-state registers ... are non-zero`. After each draw
  `draw: clear-state registers: ...` (MMIO) and `N context registers the stream never writes read non-zero (M of them in the clear-state extents)` list the garbage.
- **Decision (doc section 6):** non-zero with the control (`rdna4-gfxsrm=0 rdna4-gfxcsb=0`) and a drawing baseline = #1 confirmed.

## 2. #2: the bare RB0 queue and VA 0

- `gfxQueueEvidence` (bring-up, before and after the draw): `CP_GFX_MQD_BASE`, `CP_MQD_BASE`, `CP_GFX_MQD_CONTROL`, `HPD_OSPRE_FENCE`, the `CP_GFX_HQD_*` block, RS64 `LOCAL_BASE0`. Registers amdgpu programs from an MQD
  (`gfx_v12_0_gfx_mqd_init`) that the kext never touches; the ones reading 0 are the candidates.
- `gfxMapVa0` (`rdna4-gfxprobe=1`, after the single-packet probe has looked at the faults): VMID0 VA 0 (context 0 page 0) becomes a private zeroed VRAM page (PTE `VALID|READABLE|WRITEABLE`) and the GC TLB is flushed
  (`VA 0: VMID0 page 0 mapped ...`). `gfxDumpVa0` prints the non-zero dwords after the baseline draw and after the ladder: what the CPG writes at VA 0 fingerprints the structure (rptr, fence, MQD-like).
  Mapping the page makes the fault disappear, which is why it comes after the single-packet probe.

## 3. #4: where the primitive is lost

- **Pipeline statistics** (`rdna4-gfxprobe=1`): `PIPELINESTAT_START` and a `SAMPLE_PIPELINESTAT` before and after the draw (`si_query.c:854-857`; PS_INVOCATIONS, C_PRIMITIVES, C_INVOCATIONS, VS, GS,
  IA_PRIMITIVES, IA_VERTICES). `draw: pipeline statistics say: ...` applies the rule: IA = 0 the GE dropped the draw (queue/VA 0); IA > 0 and C_INV = 0 the NGG/export path;
  C_INV > 0 and C_PRIM = 0 clip/cull; C_PRIM > 0 and PS = 0 SC/raster state (#1); PS > 0 the pixel shaders ran. Sentinel-filled buffers: `wrote nothing` means the events are not available on this path.
- **NGG marker variant** (ladder bit 32, added automatically to a probe boot's ladder, order 32, 8, 2, 1, 4): `shaders/nggstore.s` is `ngg.s` preceded by three stores (`0xC0DE0002`, `s2`, `s3`) to the marker address
  (patched into two `s_mov_b32` literals; `RSRC1_GS.VGPRS=1`). `draw: NGG marker 0x..` says whether the NGG wave launched at all.

## 4. Emulator

The emulator drew because it read every unwritten register as 0 and loaded the CSB by itself when the ring started. `ctx-garbage=on` (OPT-IN since W39, default off; a run with it off is the neutral baseline):

- the 1024 context registers power up as a fixed non-zero pattern (`0x9e3779b1 * (off + 1) | 0x00010001`), so an unwritten register reads garbage;
- the CSB reaches them only through the RLC: a write of `RLC_SRM_CNTL.SRM_ENABLE` applies the buffer in `RLC_CSIB_*`; the CP no longer loads it on its own;
- a draw is refused while a clear-state register still holds the pattern (`draw lost: N clear-state registers still hold power-up garbage`); the pipeline statistics then read C_PRIM 1, PS 0.

This encodes the round-4 hypothesis (like `gfx-golden-strict`): a pass says the fixed kext clears the state, not that the card behaves so. The old kext (`rdna4-gfxsrm=0 rdna4-gfxcsb=0`) fails there (negative control).
`SAMPLE_PIPELINESTAT` writes the emulator's counters; `s_mov_b32` with a literal is decoded for the NGG marker.

## 5. Boot-args

| arg | default | effect |
|---|---|---|
| `rdna4-gfxsrm=0` | on | do not enable the SRM (control) |
| `rdna4-gfxcsb=0` | on | no CSB replay before the draw (control) |
| `rdna4-gfxprobe=1` | off | CP-side probe, pipeline statistics, NGG marker in the ladder, VA-0 page, context dump |

## 6. Reading boot 3

| Log | Meaning |
|---|---|
| `CP view: N of 7 clear-state registers ... non-zero` before the draw with SRM on | the clear state did not land through SRM (or the replay is the only thing clearing it) |
| triangle drawn (8192 px) with SRM + replay | fixed; run the control (`rdna4-gfxsrm=0 rdna4-gfxcsb=0`) on the next boot to attribute it |
| still 0 px; `pipeline statistics say: IA = 0` | the GE never got the draw: #2 (queue, VA 0); read the VA-0 dump and the queue lines |
| `IA > 0` and `C_INVOCATIONS = 0`, marker `NOT written` | the NGG wave did not run or its primitive never reached the clipper: #4/#3 |
| `C_INVOCATIONS > 0`, `C_PRIMITIVES = 0` | clipped or culled: context state, look at the non-zero context registers |
| `C_PRIMITIVES > 0`, `PS_INVOCATIONS = 0` | between the clipper and the pixel shader: SC/raster state, garbage registers |
| `VA 0 page: N dwords non-zero` | the CPG wrote there; the dwords fingerprint the structure |

## 7. W39 (W37 review): the two round-5 boots and which lines to read

- **Boot 3** = base + `rdna4-gfx=2 rdna4-gfxprobe=1 rdna4-gfxdiag=11`: SRM **and** replay (most likely to draw).
- **Boot 6** = the same + `rdna4-gfxcsb=0`: SRM only, amdgpu-exact (no replay), for attribution.
- Order: the `rdna4-gfxsrm=0` control (both off) first or after a cold power cycle. RLC state survives a warm restart like CG: read
  `SRM: RLC_SRM_CNTL found at start 0x...` on every gfx boot; if `SRM_ENABLE` is already set the line says so and a `rdna4-gfxsrm=0` control is void. Power-cycle before a GFXOFF boot.

| Line | Read it as |
|---|---|
| `SRM: RLC_SRM_CNTL found at start 0x..` then `-> 0x..` | whether SRM persisted from an earlier boot, and what we set |
| `clear state BEFORE any CSB replay: CP view N of 7 non-zero` (+ the MMIO line, + `says: ...`) | **the attribution line.** Read once per boot before the first replay is emitted. All zero = the clear state is already applied (SRM landed it or the registers reset to 0); non-zero (power-up garbage) = the CSB handoff / SRM enable alone did not land it |
| boot 3 draws (8192 px) and boot 6 draws | SRM alone is enough; the replay is belt and braces |
| boot 3 draws, boot 6 does not (and its pre-replay line shows garbage) | #1 confirmed: SRM does not land the clear state, the replay is the fix |
| neither draws | #1 is not (the whole) cause: `pipeline statistics say` and the NGG marker (bit 32) pick #2/#3/#4; `VA 0 page` and `queue:` lines for #2 |
| `VA 0: ... (PTE ..., IS_PTE set); GC TLB flush NOT acked` | the flush is never acked on this card and the ack is not required; the mapping needs IS_PTE (bit 63, W36) or it is a directory entry and does nothing; a silent VA-0 fault afterwards means the mapping took effect; the mapping stays for the boot, so a probe boot is not a clean fault-status baseline |

The `draw probe mid` clear-state line is read AFTER the replay: with the replay on it reads 0 whatever SRM did, so only boot 6 (no replay) or the pre-replay line attribute #1. The 1024-register context dump runs for the baseline and the first variant only.

## 8. W41 (premetal/verify-draw.md): the clean boot 3

An adversarial verification rates the SRM/CSB fix **DOUBTFUL (~12%)**: of the 49 CSB registers the stream never writes, 32 are gfx12's implicit viewport scissor (off in the stream), most of the
rest are gated off by the stream's own state (HiZ off, ZCLIP_PROG_NEAR off, CLIPRECT_RULE 0xffff, binning disabled); only `PA_RATE_CNTL` and `CONTEXT_RESERVED_REG0/1` could matter. SRM is amdgpu's
CGPG/gfxoff save/restore engine (its enable is inherited from gfx10/11, where ring `CLEAR_STATE` initialised the context), and gfx12 has no CP clear-state mechanism at all. Also,
`rootcause-draw.md` 1.5 was wrong: the garbage readback was taken AFTER draw 1 (corrected there). So round 5's value for the triangle is **diagnosis**, and boot 3 is made as clean as possible:

- **`rdna4-gfxva0=1`** (own opt-in, not in boots 3/6): `gfxMapVa0`/`gfxDumpVa0` no longer ride on `rdna4-gfxprobe`, so the baseline draw is not confounded by a VA-0 mapping.
- **First lines after every draw** (baseline and every ladder variant): `pipeline statistics say: ...` then the NGG marker line (the marker only exists in variant 32; the clean baseline says so and variant 32 is the
  first ladder draw), then the evidence. Pipeline statistics run in every draw of a probe boot.
- **`VGT_PRIMITIVE_TYPE`**: it reads 0 over MMIO on the card while the stream writes 4 (TRILIST). Each draw now prints `VGT_PRIMITIVE_TYPE: CP view mid/post, the stream wrote, MMIO` (COPY_DATA of uconfig `0x242`).
  radeonsi writes it with a plain `SET_UCONFIG_REG` (`si_state_draw.cpp:1277-1278`); RADV with `SET_UCONFIG_REG_INDEX` (opcode `0x7a`), **index 1** in bits `[31:28]` of the offset dword
  (`radv_cmd_buffer.c:4685`, `ac_cmdbuf.h ac_cmdbuf_set_ucfg_reg_idx`). New ladder variant **64** patches the stream's packet the same way (`header 0xc0017900 -> 0xc0017a00`, offset `0x242 -> 0x10000242`); a probe boot's ladder
  is now 32, 64, 8, 2, 1, 4. `DI_PT_NONE` would give exactly "fence, no pixel wave": if the CP view of the primitive type is 0 while the stream wrote 4, the plain packet did not latch on gfx12.
- Reading boot 3 first: `pipeline statistics say` (IA = 0: GE/queue A1/B; C_INV > 0 and C_PRIM = 0: clip/cull; C_PRIM > 0 and PS = 0: SC/raster state), then `VGT_PRIMITIVE_TYPE`, then the NGG marker
  (variant 32), then `clear state BEFORE any CSB replay` (the first silicon read of a CSB register).
