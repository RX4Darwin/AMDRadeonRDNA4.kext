# W32: cursor request scheduling (TTU) — evidence and the DML-style test

Source: `premetal/cursor-invisible-analysis.md` (rank 1, sections 5.1 and 5.3). Round 4 showed the cursor plane enabled and latched
(`LATCHED`, no lock held, all cursor registers as amdgpu writes them) yet neither the pointer (`rdna4-cursor=1`) nor the magenta square
(`rdna4-cursor=2`) ever appeared. The one block nobody programmed is the cursor's **request scheduling**: `HUBPREQ0_DCN_CUR0_TTU_CNTL0/1`.

## 1. What amdgpu writes and where DML gets the numbers

| Register (dcn_4_1_0, base idx 2, HUBP0) | dword | amdgpu | Value source |
|---|---|---|---|
| `DCN_CUR0_TTU_CNTL0` | 0x0627 | `hubp401_program_deadline`, `dcn401_hubp.c:406-409` | `REFCYC_PER_REQ_DELIVERY` [22:0] = `refcyc_per_req_delivery_cur0`, `QoS_LEVEL_FIXED` [27:24] = 8, `QoS_RAMP_DISABLE` [28] = 0 |
| `DCN_CUR0_TTU_CNTL1` | 0x0628 | `hubp401_setup_interdependent`, `:473-474` | `REFCYC_PER_REQ_DELIVERY_PRE` [22:0] = `refcyc_per_req_delivery_pre_cur0` |
| `HUBPREQ_DEBUG_DB` | 0x05fc | `hubp401_program_deadline` `:329` writes `1 << 8` ("put DLG in mission mode") | logged only, not written (the task limits writes to the CUR0 pair) |

DML (`dc/dml2_0`):

- `display_mode_core.c:3441-3448` (VRatio <= 1): `CursorRequestDeliveryTime[us] = CursorWidth / HRatio / PixelClock[MHz] / cursor_req_per_width`,
  `cursor_req_per_width = ceil(CursorWidth * CursorBPP / 256 / 8)` (= 1 for the 64 px, 32 bpp slot); the prefetch variant is the same while `VRatioPrefetchY <= 1`.
- `dml_display_rq_dlg_calc.c:403-404`: `refcyc_per_req_delivery_cur0 = CursorRequestDeliveryTime * refclk_freq_in_mhz` (and `_pre_`); `:486-487`: the register value is that times 2^10;
  `:497,:500`: `qos_level_fixed_cur0 = 8`, `qos_ramp_disable_cur0 = 0`.

For the boot timing (1920x1080@60, 148.511 MHz) and the 50 MHz DCHUB reference clock the kext already uses for `CURSOR_DST_X_OFFSET`:
`64 / 148.511 = 0.4310 us`, `* 50 = 21.55` ref cycles, `* 1024 = 22064` (0x5630); `CNTL0 = 0x08005630`, `CNTL1 = 0x00005630`.

## 2. Why it is rank 1

`CURSOR_REQ_MODE = 1` (mandatory on DCN4x) starts the cursor fetch at the beginning of display prefetch, paced by exactly these registers. The GOP set up one
surface and no cursor, so the CUR0 pair very likely holds a reset or zero rate. A requestor with a zero delivery rate can fetch nothing for **any** image
(the pointer, the square, a sprite at the scanout base), which is what the three rounds show, while every cursor register still reads back "right".
Not a fact: nothing in the logs has read these registers yet.

## 3. What the kext does now

- **Read-only evidence** (`cursorRegProbe`, `rdna4-cursor` = 1 or 2, into the `RDNA4FB,Cursor` trail and the kernel log). Two lines per probe:
  - `ttu: cur0 <CNTL0>/<CNTL1> surf0 <CNTL0>/<CNTL1> surf1 <CNTL0> global <GLOBAL_TTU_CNTL> qos_wm <QOS_WM> debug_db <HUBPREQ_DEBUG_DB>`
  - `gate: hubp clk <HUBP_CLK_CNTL> cursor mem pwr <CTRL>/<STATUS> stereo <STEREO_CONTROL> | mpcc<N> top/bot/opp <MPCC_TOP_SEL/BOT_SEL/OPP_ID> | sprite px first/mid/last <VRAM readback>`
- Probed at: `armed`; `selftest` (after the programming below); after every boot flip (`flip`, `flip back`, `restore original`; the compute thread calls it through
  `Env::cursorProbe`, and the trail is now taken under a lock); `first cscSetHardwareCursor`; `first cscDrawHardwareCursor` (the state seen when macOS first talks to the plane, analysis rank 3).
  Capped at 10 probes; the trail buffer is 12 KiB (was 8).
- **Decisive test (rdna4-cursor=2 only):** `cursorProgramTtu()` writes `DCN_CUR0_TTU_CNTL0/1` DML-style inside the same MPC cursor lock bracket as the position and attributes,
  and logs the value, its inputs and the readback (`CUR0 TTU programmed: ...`). Without a pixel clock it copies `SURF0_TTU_CNTL0`'s delivery value. `rdna4-cursor=1` never writes them.
- **Control:** `rdna4-cursorttu=0` (set-boot boot 18): the same dump, the CUR0 pair NOT written.

## 4. Reading the next round (boot 9 = programmed, boot 18 = control)

| Log | Meaning |
|---|---|
| `armed: ttu: cur0 0x00000000/0x00000000` next to a non-zero `surf0` | rank 1 confirmed as far as the register goes: the plane had no delivery rate |
| `cur0` already non-zero at arming | the GOP/reset value was something; compare with the DML value in `CUR0 TTU programmed` |
| the square appears in boot 9, not in boot 18 | request scheduling was the missing piece; the fix is the DML values for the timing (the kext computes them; then `rdna4-cursor=1` gets the same write) |
| square in neither | `gate:` line next: `mpcc top/bot/opp` must select DPP0/OPP0; `hubp clk` clock-on bits; `cursor mem pwr status` non-zero (shutdown/light sleep); `sprite px ... (SELF-TEST DATA GONE)` = the data is not there (rank 4) |
| `cur0`/`gate:` differ between `armed` and `flip`/`first cscDrawHardwareCursor` | the state is clobbered after arming (rank 3): a re-program is needed from the flip/first-draw path |

## 5. Emulator

`cursor-ttu-strict=on` (emulator option) rejects the plane while `CUR0_TTU_CNTL0.REFCYC_PER_REQ_DELIVERY` is 0. It encodes the rank-1 **hypothesis**, not known hardware behaviour, like
`gfx-golden-strict`: a pass says only that the kext writes the register (boot 9 shape: square; `rdna4-cursorttu=0`: no square).
