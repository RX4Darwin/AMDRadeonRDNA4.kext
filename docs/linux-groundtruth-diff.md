# W42: Linux ground truth (amdgpu 7.2.2, this RX 9070 XT) against what the kext programs

Source: `E:\linux\rdna4-groundtruth-idle-20260929-231658\` and `...-vkcube-20260929-232026\` (`gc-regs.txt`, `vm-walk.txt`, `system.txt`; idle and vkcube agree except the per-process page-table bases and
the vkcube draw state). Read-only. The kext now logs the same registers next to Linux's values on every `rdna4-gfx=2` boot, right before the draw:
`gfx: before draw: linux diff: DIFF <name> ours 0x.. linux 0x.. (why)` and a summary line. The table is `kLinuxRef` in `gfxring.cpp`.

## What the capture settles

- **VGT_PRIMITIVE_TYPE reads `0x0` under Linux too**, idle and with vkcube running. Our 0 MMIO readback is not evidence of a missing primitive type: that suspect is dropped. Reference draw state: `GE_CNTL=0xa0010080`,
  `PA_SC_MODE_CNTL_0=0x22` (both equal what our stream writes).
- **Leaf PTEs carry IS_PTE** (`0x80000003f5e003f1`: valid|exe|read|write|frag 7, mtype 0, not snooped; PDE-as-PTE `0x80000003d38005f1`, frag 11): W36's encoding is confirmed.
- **amdgpu runs the gfx pipe through an HQD/MQD**: `CP_GFX_HQD_ACTIVE=1`, `CP_GFX_MQD_BASE=0x80_00108000` (in VRAM), `CP_RB0_CNTL=0x00f0088a` (= `CP_GFX_HQD_CNTL`), `CP_RB0_BASE=0x28c0`, `RPTR_ADDR=0x1080`,
  `WPTR_POLL=0x10a0`, `CP_RB_DOORBELL_CONTROL=0x40000458`. Ours is a bare RB0: `CP_GFX_HQD_ACTIVE=0`, no MQD (rootcause-draw.md #2). Not changed here: mapping a gfx queue needs MES/KIQ or a full MQD/HQD programming
  sequence, which is the "full fix if #2 is confirmed", not a safe bring-up tweak.
- **RLC**: `RLC_SRM_CNTL=3` (we now write 3), `RLC_CNTL=1`, `RLC_CSIB_LENGTH=0x4b` (75 dwords = ours: 1 + 6 x 2 + 62), `CP_ME_CNTL=0xa000`, `CP_GFX_RS64_DC_BASE0=0x3_f9600000`, `BASE1=0x3_f9700000`.

## Diffs, and what was done

| Register | Ours | Linux | Action |
|---|---|---|---|
| `GCVM_CONTEXT1..15_CNTL` (e.g. CONTEXT8) | `0x00fffc07` (runtime wrote fault-enable bits 10..23) | `0x03fffc07` (bits 10..25) | **applied** (`vmContextInit`, and the W29 re-init path): bits 24-25 are reset defaults Linux keeps through its read-modify-write; the header names only bits 10..23. Same as the reset value of CONTEXT0 (`0x03fffc01`) and of the MM hub. |
| `GCVM_L2_CNTL` | `0x00080601` | `0x00080e01` | **not applied**: bit 11 `ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY` sends faulting accesses to a *system* dummy page. Ours is a VRAM page (`GcSysDefault*`, `GcL2FaultDefault*`); the bit would interpret it as a system address (a stray access could reach host memory). Needs a DMA-allocated dummy page first. Logged as `DIFF` with the reason. |
| `GCVM_L2_CNTL2/3/4/5`, `GCMC_VM_MX_L1_TLB_CNTL` | `3`, `0x80130009`, `1`, `0x3fe0`, `0x1859` (derived from the source) | `3`, `0x80130009`, `1`, `0x3fe0`, `0x1859` | identical, nothing to do; the log line confirms them on the card |
| `GCVM_CONTEXTS_DISABLE` | not written | `0` | logged; reset default assumed `0` |
| `GCMC_VM_SYSTEM_APERTURE_HIGH` | FB top `>> 18` = `0x20feff` | `0x20febf` | none: Linux ends the aperture 16 MiB below FB top; ours is a superset |
| `GCVM_CONTEXT0_*` | flat table, one page, START = END = 0 | 3-level GART, range `0..0x1ffff` pages | none (by design); IS_PTE is what W36 fixed |
| `CP_RB0_CNTL` | `0x0000090b` (16 KiB ring, BUFSZ 11, BLKSZ 9) | `0x00f0088a` (8 KiB ring) | ring size differs by design. Bits 20-23 (`MIN_AVAILSZ`=3, `MIN_IB_AVAILSZ`=3) come from the MQD's reset-default CNTL: **opt-in** `rdna4-gfxrbmin=1` sets them. Off by default: the proven ring/IB/wrap tests ran without them. |
| `CP_ME_CNTL` | `0x0100a000` (CE_HALT set) | `0x0000a000` | amdgpu never writes CE_HALT (no CE on gfx12) yet reads it 0, so something in the Linux load path clears it. **opt-in** `rdna4-gfxce=1` clears it after the unhalt. Off by default. |
| `CP_GFX_HQD_*`, `CP_GFX_MQD_BASE_ADDR*` | 0 (bare RB0) | active, MQD in VRAM | not changed (see above); logged |
| `CP_GFX_RS64_DC_BASE0/1` | read at bring-up (`RS64 DC_BASE0 ...`) | `0x3_f9600000` / `0x3_f9700000` | if ours read 0, the PSP loader did not set them; not programmed from the kext without the firmware image's address (W31 review) |
| `PA_SC_MODE_CNTL_1` | stream `0x060201b5` | `0x060201bc` (vkcube) | noted only: the stream is Mesa's value for a linear destination; vkcube's differs in bits 0-2/3 (its own state) |

## Kept

The pipeline-statistics verdict and the NGG marker stay the first lines after every draw (W41), together with the CP-side reads and the clear-state evidence.

## Caveats (W43)

- **No control run for the applied `GCVM_CONTEXT1..15_CNTL = 0x03fffc07`.** The W36 `ispte=0` control (a leaf without IS_PTE faults, with IS_PTE it works) was characterised on the card with bits 24-25 **clear** (`0x00fffc07`). The value Linux uses
  (and the reset default) has never been run with the kext's VM path; if a VM boot behaves differently from the W36 characterisation, suspect these two bits first (they are the fault-enable defaults Linux keeps).
- **The diff logger is bounded** (W43, review BLOCKER): `gfxLinuxDiff` used to append entries into a `char[300]` with unchecked `snprintf` lengths, which could overflow the stack buffer with long `why` strings. It now logs one line per register through
  `LinuxRefTable::format` (`src/linuxref.hpp`), which never writes past its capacity; `tools/atomdump.cpp` (`make test`, `testLinuxRefFormat`) feeds it a 4095-character `why`, capacities 1..600 with canary bytes, capacity 0 and a null buffer.
- **`linuxref.hpp` include (W44):** the header first did `#include <stdio.h>`. The kext builds with `-nostdinc -IMacKernelSDK/Headers`, which has no top-level `stdio.h` (only `sys/stdio.h`); the include resolves to libc++'s wrapper,
  which does not declare `snprintf`. It compiled inside `gfxring.cpp` only because another header had already pulled `libkern/libkern.h` in first, and failed as a standalone TU with the real kext flags
  (`use of undeclared identifier 'snprintf'`). It now uses `<libkern/libkern.h>` under `KERNEL` (the kext build's `-DKERNEL`) and `<stdio.h>` otherwise (the host test).
