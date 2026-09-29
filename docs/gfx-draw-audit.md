# W23: why the round-3 draw wrote nothing (rdna4-gfx=2, real card)

Round 3 (`hw-logs/rdna4fb-diag-20260929-014202.txt`, boot 4): the gfx ring works on silicon (ring test, WRITE_DATA,
RELEASE_MEM fence, IB test, 1025 wrapped IBs, 35 us each), `gc_info 1.3` gives 4 shader engines, and the draw
then reports

```
draw: 489-dword stream at MC 0x8009e10000, VS 0x8009e20000 PS 0x8009e20400, target 0x8009e40000, rings 0x8010000000 (10 MiB)
draw: wrong image in 88 us: 0 pixels 0xff0000ff (want 8192), 0 others; bounds x 256..0 y 256..0 ...
```

The in-stream draw fence reached 1 (the code only gets to the read-back when `ringDone && drawFence == 1`), so the CP
executed the whole stream including the `RELEASE_MEM` that follows `DRAW_INDEX_AUTO`. The target is untouched:
0 covered and 0 "other" pixels. Nothing wrote to it. That happens if no wave ran (no primitive reached the rasterizer
or no pixel wave started) or if the colour-buffer write never reached memory.

Sources: the earlier spec `premetal/gfx12-draw-notes.md` (Mesa `eda9aceb39d5`, amdgpu `d266640c6c76`); amdgpu paths are
under `drivers/gpu/drm/amd/`.

## Suspects, most likely first

### 1. The gfx12 golden registers were never written (FIXED, to be confirmed on the card)

`gfx_v12_0_init_golden_registers` (`amdgpu/gfx_v12_0.c:3671-3690`) programs, for GC 12.0.0 and 12.0.1, before the CP
runs (`gfx_v12_0.c:253-261`):

| Register | Mask / value | When |
|---|---|---|
| `DB_MEM_CONFIG` (GC seg 0, `0x13d2`) | mask `0x8000` = `0x8000` | always |
| `DB_MEM_CONFIG` | mask `0xf` = `0xf` | `rev_id == 0` |
| `CB_HW_CONTROL_1` (seg 0, `0x1425`) | mask `0x03000000` = `0x03000000` | `rev_id == 0` |
| `GL2C_CTRL5` (seg 1, `0x2e19`) | mask `0x70` = `0x20` | `rev_id == 0` |

The notes' section 4.2 listed them as "should be in place before the first draw"; the kext defined `Gl2cCtrl5` in
`gfxregs.hpp` but nothing ever wrote any of the four (grep of `src/`). `CB_HW_CONTROL_1` and `DB_MEM_CONFIG` belong to the
colour and depth blocks that a first draw exercises; the compute stages never touch them, which is why compute works
without them. amdgpu's `rev_id` is `STRAP_ATI_REV_ID` of the NBIF strap (`nbif_v6_3_1.c` `get_rev_id`,
`RCC_STRAP0_RCC_DEV0_EPF0_STRAP0` [27:24]), not the PCI revision byte.

Fix: `RDNA4Compute::gfxGoldenInit` (`gfxring.cpp`) applies them at gfx-ring bring-up (while PFP/ME are still halted) and logs
strap, rev_id, PCI revision, and each register before/after/readback (`gfx: golden: ...`). The A/B control on the card is
`rdna4-gfxgolden=0` (skips them).

Not proven to be the cause: the evidence is that the kernel always programs them and the kext did not.

### 2. Registers Mesa relies on the reset value of

Open question 2 of the notes: the tree has no `gc_12_0_0_default.h` and the clear state is nearly empty, so any register the
stream leaves at its hardware reset value is unverified on this silicon. Evidence added: after every draw the SH and
context registers of the pipeline are read back over MMIO and logged (`draw: SH:` and `draw: CTX:` lines, see below);
a value different from `gfx12_draw.h` is a write that did not take, a register that is 0 and should not be is a missing one.

### 3. NGG stage details the emulator cannot distinguish (variants 1, 2, 4)

- `SPI_SHADER_PGM_RSRC2_GS.USER_SGPR = 0` (Mesa always has user SGPRs, `si_shader.h:143-186`; notes open question 4).
- `INST_PREF_SIZE = 0` in `RSRC4_GS/PS` (Mesa always sets it from the code size, `ac_binary.c:146-165`; open question 5).
- `PRIMGEN_PASSTHRU_NO_MSG = 1` (RADV uses it whenever culling is off, but radeonsi usually sends `GS_ALLOC_REQ`, notes 3.4).

Each is one register in the IB copy (the `rdna4-gfxdiag` ladder patches it; the shader variant `shaders/nggmsg.s` is notes 3.7).

### 4. The colour write does not reach memory although the pipeline runs (variant 8)

Notes open questions 8 and 9: the fence writes back GL2 only (as amdgpu's does); the CB writes through GL2. The ladder's
pixel-shader variant (`shaders/psstore.s`) stores a marker (`0xC0DE0001`) to memory from the PS before exporting: marker present
and target empty means everything up to the pixel shader works and only the colour-buffer write is lost; marker missing means
no pixel wave ran. Independently, when the target is empty the code looks again after 2 ms, then issues a full
`ACQUIRE_MEM` GL2 write-back (`gfx_v12_0_emit_mem_sync` bits) plus a fresh end-of-pipe fence and looks a third time
(`draw: target still empty: ...`): a late or cache-stuck write shows up there.

### 5. Lower

- **GE rings at VA `0x8010000000`** (device heap offset 256 MiB, above the 4 GiB mark). The registers hold VA[47:16] (fits 32
  bits); Mesa asserts the attribute ring's high half equals the 32-bit shader window (`ac_cmdbuf_cp.c:299`) but only shaders that
  write attributes use it, and ours do not. Unverified either way.
- **CP-side addressing (VMID0 MC addresses).** Everything the CP fetches is VMID 0: the IB (`indirectBufferGfx(..., vmid 0)`), the
  shaders, the rings, the target and the fences are all pool/heap MC addresses in the GC hub's flat VMID0 view, the same view the
  compute path and the ring fences already use successfully. No inconsistency found.
- **Read-back path.** HDP has no read-cache invalidate on this ASIC (`hdp_v7_0_funcs`, see `compute.cpp` sensors comment) and
  the target is read through the same uncached BAR mapping that returns the ring fences correctly, so a stale read is unlikely.

## What round 4 sees (evidence added, `gfxring.cpp`)

For the baseline draw and every ladder variant, in this order:

1. `gfx: golden: ...` lines at bring-up.
2. `draw: before draw:` engine status (`GRBM`, `GRBM2`, `GRBM_STATUS_SE0-3`, `SPI_DEBUG_BUSY`, `CP_STAT`, RB0 rptr/wptr, fences, GC fault status).
3. After the fence: the same status again, then `SH:` and `CTX:` lines with the pipeline registers read back (SPI shader
   addresses and RSRC words, `VGT_SHADER_STAGES_EN`, `CB_TARGET_MASK`, `CB_SHADER_MASK`, `CB_COLOR0_BASE/INFO/ATTRIB*`,
   scissors, `GE_POS/PRIM_RING_*`, ...).
4. `draw: wrong image ...` with covered/other counts, first non-zero pixel and bounds; for an empty target the 2 ms and
   flushed recounts.
5. With `rdna4-gfxdiag=<mask>`: `diag 1|2|4|8:` variant lines (bit 1 USER_SGPR, 2 INST_PREF_SIZE, 4 GS_ALLOC_REQ shader,
   8 PS marker store; bit 16 runs them even when the baseline passed) and a summary `diag ladder: baseline N px; variants ...`, also
   in the registry property `Compute,GFXDiag`.

A variant runs only after the baseline drew and failed (or with bit 16); a variant whose fence does not signal ends the ladder
(`gfx:` trail `gfx: draw variant`).
