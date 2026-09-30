# Linux ground truth for round 5 (VM, triangle, cursor, power)

Written 2026-09-29. Three root-cause passes (`docs/rootcause/`) found concrete
differences between RDNA4FB and amdgpu. The cheapest way to confirm them is to
read what Linux amdgpu actually programs on **this** card, on the same machine,
instead of inferring it from source. Everything below is **read-only**: it reads
registers through debugfs and page tables through debugfs `amdgpu_vram`; it
never writes to the GPU.

## What you need

- The dual-boot Linux on this PC, a kernel with RX 9070 XT support (6.14 or
  newer is safe) and the matching firmware (`linux-firmware` recent enough that
  the desktop runs on the card; `dmesg | grep amdgpu` shows no firmware errors).
- `python3` (present on every mainstream distro).
- Secure Boot off for the session if the kernel is in lockdown mode (debugfs
  `amdgpu_regs` / `amdgpu_vram` are refused under `lockdown=confidentiality`).
- Optional: `vkcube` (package `vulkan-tools`) for the 3D capture.
- This repository on the machine (clone the `private` remote, branch
  `premetal/int`), or just copy the two scripts in `tools/`.

## The captures (about 5 minutes in total)

Run each from a terminal in the repository, with the desktop on the monitor
that RDNA4FB drives (the HDMI output of the RX 9070 XT):

1. **Desktop idle, mouse pointer visible on screen** (move it into the middle of
   the screen first, then leave the mouse alone):

       sudo bash tools/linux-groundtruth.sh idle
       sudo bash tools/linux-capture.sh --snap-only

2. **A 3D app on screen**: start `vkcube` (keep its window visible), then in a
   second terminal:

       sudo bash tools/linux-groundtruth.sh vkcube

3. Optional, only if ROCm or an OpenCL runtime is installed: start any compute
   job (e.g. `clinfo` in a loop, or a ROCm sample) and run

       sudo bash tools/linux-groundtruth.sh compute

Each run leaves a `rdna4-groundtruth-<label>-<time>.tar.gz` (and
`linux-capture.sh` an `rdna4-capture-<time>.tar.gz`). Copy all `.tar.gz` files
to the OPENCORE stick and tell Claude.

## What each capture answers

| Question | Where | What confirms it |
|---|---|---|
| **VM**: does amdgpu set bit 63 (IS_PTE) on leaf PTEs on this card? (W36, `rootcause/rootcause-vm.md`) | `vm-walk.txt` in `idle` and `vkcube` | leaf lines `PTE ... bit63 1`, and the other bits (exe/read/write/snooped/mtype) of real amdgpu mappings, to compare with W36's encoding |
| VM: how amdgpu programs VMID contexts 1-15 and the GC hub | `gc-regs.txt`: `GCVM_CONTEXT*`, `GCVM_L2_CNTL*`, `GCMC_VM_*`, `GCVM_CONTEXTS_DISABLE` | the values to compare with our `vm: diag` lines (CONTEXT8 0x00fffc07, L2_CNTL 0x00080601, ...) |
| **Triangle**: is the RLC save/restore machine enabled? (W37, `rootcause/rootcause-draw.md` #1) | `gc-regs.txt` `RLC_SRM_CNTL` | bits 0-1 set (`SRM_ENABLE`, `AUTO_INCR_ADDR`) under amdgpu; RDNA4FB never sets them |
| Triangle: clear-state buffer, gfx queue setup, RS64 data bases | `RLC_CSIB_*`, `CP_RB0_*`, `CP_GFX_HQD_*`, `CP_GFX_MQD_BASE_ADDR*`, `CP_GFX_RS64_DC_BASE*` | whether amdgpu runs the gfx pipe through an HQD/MQD (non-zero `CP_GFX_HQD_ACTIVE`/`MQD_BASE`) and what the RS64 data bases hold (#2 in the draw doc) |
| Triangle: context registers the draw stream never writes | `PA_RATE_CNTL`, `CONTEXT_RESERVED_REG0/1`, `VGT_SHADER_STAGES_EN`, ... in `vkcube` | clean values next to the garbage RDNA4FB reads (MMIO shows the last context, so read them as a hint only) |
| **Cursor**: the whole DCN pipe-0 state with a visible hardware cursor (W38, `rootcause/rootcause-cursor.md`) | `dcn-regs.txt` (`CURSOR0_0_*`, `CM_CUR0_*`, `CNVC_*`, `DSCL0_*`, `HUBP0_*`, `HUBPREQ0_*`) and `linux-capture.sh --snap-only` | the exact register values amdgpu uses for a cursor that is on screen: diff them against our `RDNA4FB,Cursor` trail (DSCL mode, RECOUT, CURSOR_REQ_MODE, TTU/DLG, mission mode `HUBPREQ_DEBUG_DB`, MALL/CRQ bits, cursor memory power) |
| **Power**: idle watts and gating under Linux | `pm.txt` (`gpu_metrics`, `amdgpu_pm_info` clock-gating flags, `pp_dpm_sclk`) | Linux's idle power, clock and CG/PG flags, to compare with our 20 W after W29 |

## After the capture

Claude diffs these against the kext's own logs (`hw-logs/`) and the round-5
code; any register where amdgpu differs from RDNA4FB on the same card is a
candidate fix with no guessing. The scripts only read; if a read fails (lockdown,
missing debugfs file) the output says so and the rest still runs.
