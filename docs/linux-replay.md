# The first triangle: root cause found on the card under Linux (EXEC at NGG wave start)

2026-09-30, Linux side, RX 9070 XT, amdgpu (kernel 7.2.2), no root, no reboot.

## Result

**An NGG (merged ES/GS) wave on gfx12 starts with `EXEC = 0x00000001`: lane 0 only.** Every kext NGG
shader built its lane masks with `s_and_saveexec_b32`, which ANDs with that initial EXEC. Only vertex 0
exported a position; vertices 1 and 2 never did, so the primitive reached the clipper incomplete and was
dropped. That is the card's signature in rounds 4-6: `IA 1/3, VS 3, C_INVOCATIONS 1, C_PRIMITIVES 0, PS 0`.

The fix is to **set** EXEC from `merged_wave_info` (`s_mov_b32 exec_lo, <mask>`) instead of ANDing it, as
ACO does (`s_bfe_u64 exec, -1, s0` at the top of RADV's NGG shader). The diagnostic shaders that
compute or store per-lane values before that (`nggconst`, `nggstore`, `nggvgpr`) open all lanes first
(`s_mov_b32 exec_lo, -1`).

With the fix, **the kext's unchanged draw stream and registers draw the triangle on the card: 8192 px,
bounds x 64..191 y 64..190, C_PRIMITIVES 4, PS_INVOCATIONS 8192**. That holds for all five NGG shaders,
each with its ladder variant's register patch.

The emulator launched NGG waves with EXEC = all ones, which hid the bug. It now starts them with `0x1` like
the card, so the old shaders fail there too (negative control).

## How it was found (tools/linux-replay)

`tools/linux-replay/replay.cpp` submits the kext's `gfx12_draw.h` stream through amdgpu's CS ioctl
(libdrm_amdgpu), in a normal per-process VMID, with the kext's shaders placed and relocated the same way
`gfxDrawRun` does. It adds SAMPLE_PIPELINESTAT around the draw and counts the target. `REPLAY_SET` inserts
SET_*_REG writes before the draw, and `REPLAY_VS` picks the VS binary.

| Step | Result on the card |
|---|---|
| kext stream + kext `ngg.s`, as on macOS | **0 px, C_INV 1, C_PRIM 0**: the macOS symptom reproduced on amdgpu's own queue, so NOT the bare ring, VMID0 or UC memory |
| + every context/uconfig register RADV had at its draw (98 regs, `radv-state.py`) | 0 px |
| RADV's VS binary (wave64) + RADV's SH regs, rest = kext stream | **8192 px**: everything but the VS is right |
| RADV's VS hand-ported to wave32 in the kext's wave32 config | 8192 px: wave32 is fine |
| RADV's exec/export skeleton + kext position math | 8192 px |
| kext skeleton + RADV position math | 0 px: the skeleton |
| kext `ngg.s` with `s_and_saveexec` -> `s_mov_b32 exec_lo`, kext regs unchanged | **8192 px**, with either export order |
| probe: store `exec_lo`/`exec_hi`/`s3` at wave start | `0x00000001 / 0x00000000 / 0x10000103` |

Also measured by `nggvgpr` (now storing all lanes): VertexID in v3 is 0, 1, 2. The positions are
(-0.5,-0.5), (0.5,-0.5), (0,0.5). v0 is `0x040a0300`, which is indices 0, 1, 2 at a 9-bit stride plus the
three edge-flag bits (8, 17, 26). The ABI assumptions in `gfx-ngg.md` were right; the EXEC assumption was not.

Ruled out along the way (all on the card): the export order (`linux-radv-triangle.md`), the guard band,
`PA_SU_HARDWARE_SCREEN_OFFSET`, `PA_CL_VS_OUT_CNTL`, `PA_CL_CLIP_CNTL`, `PA_SU_SC_MODE_CNTL`,
`PA_SC_BINNER_CNTL_0`, the viewport Z, the VGPR count, user SGPRs and `INST_PREF_SIZE`.

## Reproduce

    tools/linux-replay/run.sh                                 # the committed ngg.s: 8192 px
    REPLAY_VS=old tools/linux-replay/run.sh                   # the round-6 ngg.s (edc5f81): 0 px
    REPLAY_SET="c:0x10b=0x43800000" tools/linux-replay/run.sh # any register override

## W12k groundwork: the app-side triangle IB

`userspace/gfx12tri.h` is the triangle as an application records it: `rdna4_tri_place_shaders`,
`rdna4_tri_record` (the stream with the app's code/target/ring/fence addresses) and `rdna4_tri_count`.
It is plain C (rdna4-run) and C++ (the replay), over `src/gfx12_draw.h`, which the generator now emits
for both languages. The regenerated stream and relocations are byte-identical and the kext's code is
unchanged. The replay records through it, so the exact bytes `rdna4-run tri` will submit are proven on
the card as an unprivileged IB in a per-process VMID: 8192 px, and the stream's own RELEASE_MEM
writes the fence the app waits on.

The emulator (W12e) agrees for this stream in a client VMID: every opcode is modelled (none takes the
OPCODE_ERROR fault) and all its SET_UCONFIG_REG writes are in the unprivileged range.

`tools/linux-replay/ring-capture.sh` (needs sudo, read-only) dumps amdgpu's gfx rings right after a
replay and decodes the packets around our INDIRECT_BUFFER. That shows what the kernel wraps a user gfx IB
with on this card, which is the reference for the kext side of W12k.

## The wave64 fallback (ladder variant 4096)

`shaders/ngg64.s` is `ngg.s` as a wave64 NGG shader (64-bit EXEC and compare masks; `tools/build-shaders.sh`
assembles `*64.s` with `+wavefrontsize64`). Variant 4096 places it, clears `VGT_SHADER_STAGES_EN.GS_W32_EN`
and sets `SPI_SHADER_PGM_RSRC1_GS.VGPRS = 2`. On the card (`REPLAY_VARIANT=4096`) it draws **8192 px**, with
the same counters as the wave32 baseline.

Proof that it runs as wave64: the same `ngg64` binary with the same `VGPRS = 0` (8 VGPRs in wave32, 4 in wave64)
and only `GS_W32_EN` differing. Wave32 mode draws 8192 px; wave64 mode draws 0 px (PS 0, C_INV 1; C_PRIM read 0, 4 or 8 over six runs, so the pixel count is the signal, not C_PRIM). With
`VGPRS = 1` (8 VGPRs in wave64) it draws again. Reproduce, after `run.sh` has built `tools/linux-replay/build/replay`:

    REPLAY_VARIANT=4096 REPLAY_SET="c:0x2a6=0x04400000;s:0x8a=0x000c0000" tools/linux-replay/build/replay   # wave32: 8192 px
    REPLAY_VARIANT=4096 REPLAY_SET="s:0x8a=0x000c0000" tools/linux-replay/build/replay                      # wave64: 0 px

It joins every probe boot's ladder right after the NGG marker. The ladder only runs when the baseline draw
is not right. If boot 3's wave32 baseline were still empty on macOS, `diag 4096` says whether the wave size
matters there. The emulator refuses wave64 draws ("requires GS_W32_EN"): the draw is skipped, 0 px, no
false pass.

## Next

Real-card round 7, boot 3 (`set-boot.sh 3`): the baseline draw should report `PASS ... THE TRIANGLE IS RIGHT,
draw 8192 px`. Run the emulator dry run on Windows first (both boot 3 and boot 1), because it now models the
initial EXEC.
