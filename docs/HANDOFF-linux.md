# Handoff Windows -> Linux (2026-09-30)

Read this first, then the files in `memory/` (they are the long-term notes:
goal, architecture, hard-won rules). Install them with `install-memory.sh`.

## Goal
Native macOS Tahoe 26 on the user's Hackintosh (Gigabyte B550M K, Ryzen 7 5700G,
ASUS RX 9070 XT = Navi 48 / gfx1201, one HDMI 1080p60 monitor). Project:
RDNA4FB, a Lilu plugin (display) plus from-scratch compute/gfx bring-up inside
the kext. Metal is a long-term goal; we are building the hardware foundation.
Repo: https://github.com/RX4Darwin/RDNA4FB (PRIVATE). Remote `private` = that
repo. `origin` = public upstream somestupidgirl/RDNA4FB: NEVER push there.
Working branch: `premetal/int`. Dev setup (osxcross, QEMU emulator of the card,
Tahoe VM) lives in WSL Debian on the Windows box, not on this Linux.

## State on real card (rounds 5-6, 2026-09-30)
- DONE: display, IH interrupts, vblank, page flips, queue recovery, clock gating (~20 W idle).
- DONE (round 6): hardware cursor. Root cause: CM0_CM_CONTROL CM_BYPASS=1 left by the GOP; kext now clears it (default in rdna4-cursor=1). Real pointer visible.
- DONE (round 6): per-app GPU VM (`rdna4-vm=1` PASS). Leaf PTEs need IS_PTE bit 63 (W36) AND the EXECUTABLE bit (the CP fetches the EOP page with READ|EXE).
- OPEN: first triangle (`rdna4-gfx=2`). Draw returns 0 px. Card stats: IA_PRIM 1, VS_INV 3, C_INVOCATIONS 1, C_PRIMITIVES 0, PS 0; the NGG wave runs (marker 0xc0de0002 stored). NOT the clipper (CLIP_DISABLE variant still C_PRIM 0) and NOT setup prim filters (variant still 0). We are guessing what zeroes C_PRIMITIVES after the NGG export (NaN/degenerate positions, prim export dword layout on GFX12, gs_tg_info/merged_wave_info, VertexID ABI v3 vs lane index, GE/NGG state).
- After the gfx probe ladder the GPU is left busy (SMU 100 % / 81 W), later runtime clients get dequeue timeouts: keep the gfx probe boot LAST.

## What we need from Linux (the point of this handoff)
Ground truth of a WORKING triangle on this card, from amdgpu + Mesa (RADV):
1. The real command stream: `RADV_DEBUG=hang` IB dumps, or `umr --ring` on `amdgpu_ring_gfx_0.0.0`; compare packet by packet with ours (context regs, SPI/PA/VGT, NGG shader regs, the draw packet).
2. NGG wave initial state via umr (`-O halt_waves -wa`): SGPRs (gs_tg_info s2, merged_wave_info s3) and VGPRs (is VertexID in v3?).
3. Shader ISA + config: `RADV_DEBUG=shaders` (SPI_SHADER_PGM_RSRC1/2/3, GE_NGG_SUBGRP_CNTL, VGT_SHADER_STAGES_EN, prim export layout).
4. A STABLE render: the earlier vkcube crashed after a moment; note the exact error (`journalctl -k | tail -50`), try `vkcube --c 300` or glxgears, capture ~2 s in.
Existing scripts: `linux/linux-groundtruth.sh idle|vkcube` and `linux/linux-capture.sh --snap-only` (read-only debugfs register/page-table dumps; results already in the stick's linux/ folder: rdna4-groundtruth-*). These only see the LAST context via MMIO, so they are weak for the triangle: the command stream and wave state are what is missing.
Put results on the stick (`linux/`), then plug it into Windows and tell Claude there, or push a branch.

## Reference sources
- amdgpu: kernel tree (drivers/gpu/drm/amd/{amdgpu,include,pm}); Mesa: src/amd/{common,vulkan}, radeonsi si_state.c, ac_nir_lower_ngg. The Windows WSL has ~/src/linux-amd (sparse) and ~/src/mesa.
- Rule learned the hard way: when comparing to amdgpu, trace a value from its FIRST assignment to the register write (IS_PTE was composed in amdgpu_ttm_tt_pte_flags, not in the final gmc hook).

## Real-card routine (unchanged)
Stick OPENCORE (E: on Windows): boot Recovery, `bash /Volumes/OPENCORE/diagnostic-log.sh` IMMEDIATELY (dmesg window rotates), then `bash /Volumes/OPENCORE/set-boot.sh <n>`. Boots: 1 base+cursor (daily, safe), 2 = +VM, 3 = +triangle (last, can wedge the GPU), 5 GFXOFF (optional, then set-boot 1 and power off), 6/7 isolation, 0 = known-good. Logs: rdna4fb-diag-*.txt on the stick (round 6: 20260930-06*).

## Agents / tooling (Windows only)
A Relay canvas on the Windows box has Claude agents (pm, gfx, vmfix, review) and this lead session. From Linux you cannot `relay ask` them. Work in flight at handoff: gfx agent on `hub-task-288` (persist gfx verdicts to NVRAM, rethink what zeroes C_PRIMITIVES, probe idling); its result lands in a local Windows worktree branch. Nothing after commit bacb27b on premetal/int was pushed until the user says so; local head has the merges cursor7, vmfix6, gfx7 and set-boot.sh changes (42a1e96).
User rules: reply in the user's language (Portuguese), recruited agents are Claude Code Sonnet 5.5 at /effort medium, ask before pushing, never push to origin.

## Resume the exact conversation
`claude --teleport session_017xMjWWp2b9mNrXJio1s7oz`
