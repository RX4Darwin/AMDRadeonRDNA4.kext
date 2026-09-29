# W32/W34: cursor request scheduling and DLG mission mode — evidence and tests

Source: `premetal/cursor-invisible-analysis.md` (rank 1) and `premetal/w32-review.md`. Round 4 showed the cursor plane enabled and latched (`LATCHED`, no lock held, all cursor
registers as amdgpu writes them) yet neither the pointer (`rdna4-cursor=1`) nor the magenta square (`rdna4-cursor=2`) ever appeared.

## 1. The correction (W32 review S1): amdgpu does not program a cursor rate on DCN 4.01

DCN401 uses DML 2.1 (`using_dml21 = true`, `dcn401_resource.c:784`). Its pipe-register calculation (`dml2_core_dcn4_calcs.c:12796-12806`) fills the surface pairs only; there is no
`cur0` value anywhere in `dml21`, and the per-pipe register set is zeroed first. `hubp401_program_deadline` therefore writes `DCN_CUR0_TTU_CNTL0/1 = 0`
(`dcn401_hubp.c:406-409`, `:473-474`) and the Linux cursor works on this ASIC. So "the CUR0 pair is zero" (rank 1) cannot by itself be why the plane shows nothing.
The formula W32 computed (`display_mode_core.c:3441-3448`, `dml_display_rq_dlg_calc.c:403-404,486-487,497,500`) belongs to the *legacy* DML2 (dml2_0 core, DCN3.x-style): an untested
combination on this ASIC, so it is now a **secondary opt-in** (`rdna4-cursorttu=1`, default off).

## 2. What amdgpu writes and the kext did not: HUBPREQ_DEBUG_DB = 1 << 8

`hubp401_program_deadline` starts with `REG_WRITE(HUBPREQ_DEBUG_DB, 1 << 8)` ("put DLG in mission mode", `dcn401_hubp.c:329`); `HUBPREQ0_HUBPREQ_DEBUG_DB` is dword `0x05fc`
(base idx 2, `+ hubpOff()`). The GOP-lit pipe never went through that function. Under `rdna4-cursor=2` the kext now (`cursorProgramMissionMode`, before the cursor lock bracket):

- reads the register; if bit 8 is already set it says so and writes nothing;
- **W35 (W34 review S1): bit 8 is not cursor-only.** amdgpu's own comments call it "disable dlg test mode" / "hack mode disable" (`dcn20_hubp.c:176`, `dcn10_hubp.c:129`): it takes the whole
  HUBP request generator, the primary surface included, from the DLG test mode to the mode that follows the programmed DLG/TTU registers, and DC writes it in the same call that programs
  them. So the kext writes it only when the GOP already programmed the DLG (`DCN_SURF0_TTU_CNTL0` delivery non-zero) and bit 8 is clear; otherwise it logs
  `DLG registers not programmed by the GOP (SURF0_TTU_CNTL0 delivery 0, ...); mission mode NOT written` and skips. A black or corrupt primary after `rdna4-cursor=2` without `rdna4-cursordlg=0` would be caused by this write;
- when it writes: the whole register (`0x100`, as DC's `REG_WRITE` does), logs `HUBPREQ_DEBUG_DB was 0x... (SURF0 delivery ...), wrote 0x100 ...; reads back ...`;
- `rdna4-cursordlg=0` skips it (the control: same dumps).

`rdna4-cursor=1` never writes it.

## 3. Read-only evidence (unchanged, `rdna4-cursor` = 1 or 2)

Two lines per probe in the `RDNA4FB,Cursor` trail and the kernel log, at `armed`, `selftest`, after every boot flip (compute-thread hook `Env::cursorProbe`), and at the first
`cscSetHardwareCursor` / `cscDrawHardwareCursor` (max 10 probes):

- `ttu: cur0 <CNTL0>/<CNTL1> surf0 <CNTL0>/<CNTL1> surf1 <CNTL0> global <GLOBAL_TTU_CNTL> qos_wm <QOS_WM> debug_db <HUBPREQ_DEBUG_DB>`
- `gate: hubp clk <HUBP_CLK_CNTL> cursor mem pwr <CTRL>/<STATUS> stereo <STEREO_CONTROL> | mpcc<N> top/bot/opp <MPCC_TOP_SEL/BOT_SEL/OPP_ID> | sprite px first/mid/last <VRAM readback>`

## 4. Reading the next round

| Log | Meaning |
|---|---|
| `armed: ttu: ... debug_db 0x00000000` (bit 8 clear) and the square appears after `wrote 0x00000100` | DLG mission mode was the missing piece; make it part of the cursor bring-up (also for `rdna4-cursor=1`) |
| `debug_db` already `0x00000100` at arming | mission mode was not it; look at `gate:` and the other DLG registers below |
| the square does not appear either way | `gate:` next: `mpcc top/bot/opp` must select DPP0/OPP0, `hubp clk` clock-on bits, `cursor mem pwr status` (shutdown/light sleep), `sprite px ... (SELF-TEST DATA GONE)` (data), and `global`/`qos_wm`/`surf0` against a Linux dump of the same HUBP |
| `cur0`/`gate:` differ between `armed` and `flip` / `first cscDrawHardwareCursor` | state clobbered after arming: re-program from the flip/first-draw path |
| square only with `rdna4-cursorttu=1` | a legacy-DML value is sufficient; amdgpu proves 0 is sufficient in the presence of whatever else it programs (mission mode, DLG registers), so look at what else differs |

The best single piece of evidence would be a Linux boot on the same card reading `DCN_CUR0_TTU_CNTL0/1`, `HUBPREQ_DEBUG_DB`, `DCN_GLOBAL_TTU_CNTL` and `DCN_TTU_QOS_WM` (umr / amdgpu_regs).

## 5. The opt-in TTU write (`rdna4-cursorttu=1`, `rdna4-cursor=2` only)

`cursorProgramTtu` writes `DCN_CUR0_TTU_CNTL0 = delivery | 8 << 24`, `CNTL1 = delivery` inside the MPC cursor lock bracket, with
`delivery = 64 * refclk_kHz * 1024 / (pixel_clock_kHz * cursor_req_per_width)` (1080p60 + 50 MHz DCHUB ref: 22064). Not written when the stream clock is above 600 MHz (ODM combine would
double the value) or no usable value exists. Whether the pair latches inside the cursor lock or under the OTG master update lock is not established; compare the write's readback
with the next `ttu:` line.

## 6. Emulator

`cursor-ttu-hypothesis=on` (emulator option, formerly `cursor-ttu-strict`) rejects the plane while `CUR0_TTU_CNTL0` delivery is 0. Given section 1 it models something amdgpu
contradicts; a VM pass with it says only that the kext writes the register (`rdna4-cursorttu=1`), never anything about the card. Default off.
