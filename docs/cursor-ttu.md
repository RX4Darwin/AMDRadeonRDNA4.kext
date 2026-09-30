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

## 7. W38: DSCL / pipe state, per-update fields and the CRC self-check (`rdna4-cursor=2`)

Source: premetal/rootcause-cursor.md (candidates #1 DSCL stage, #3 cursor-only state, #4 MPCC lock-set mapping). Everything logs into `RDNA4FB,Cursor`, every wait is bounded (<= 50 ms),
only display registers are touched, the GPU is never reset.

Order inside the self-test: `dscl:`/`pipe:` dump (`selftest pre`) -> per-update fields -> `DSCL decision:` -> mission mode (W34) -> arm -> `dscl:`/`pipe:` dump (`selftest post`) -> CRC check.

| Boot-arg | Default | Effect |
|---|---|---|
| `rdna4-cursorpipe=0` | on | skips CRQ_EXPANSION_MODE=1 (`0x0620` [3:2], dcn401_hubp.c:293-305), CROB memory power un-force (`0x0681/0x0682`, only if STATUS says gated) and the per-update clear of USE_MALL_FOR_CURSOR (`0x05f7` bit 2, dcn32_hubp.c:133,165; 64x64 ARGB = 16384 B, not above the limit) |
| `rdna4-cursormpcsel=1` | off | `MPCC_UPDATE_LOCK_SEL` = OPP number (dcn10_mpc.c:221-222); the GOP left 0xf. Changes when the MPCC latches, hence its own arg |
| `rdna4-cursordscl=1` | off | only if the dump shows `DSCL_MODE == 6` (full bypass) or `RECOUT_SIZE`/`MPC_SIZE` != plane size: amdgpu's mode-0 set (section 4B of the root-cause doc; dcn401_dpp_dscl.c:1067-1161) under the OTG update lock (`0x1b89`, held-wait bit 8). GOP values are logged first |

Register offsets verified in dcn_4_1_0_offset.h (DSCL0_SCL_MODE 0x0d08, RECOUT_START/SIZE 0x0d1e/0x0d1f, MPC_SIZE 0x0d20, LB_DATA_FORMAT/MEMORY_CTRL 0x0d21/0x0d22, DSCL_MEM_PWR_CTRL/STATUS
0x0d24/0x0d25, OBUF_CONTROL 0x0d26, HUBP0_DCHUBP_MALL_CONFIG 0x05f7, HUBPREQ0_DCN_EXPANSION_MODE 0x0620, OTG0_OTG_CRC_CNTL 0x1b65 ... OTG_CRC0_DATA_B 0x1b6b).

### Reading the verdict

`DSCL decision: ... -> the #1 candidate is LIVE / eliminated` says whether the GOP's DSCL state matches what amdgpu programs. The CRC check is the one that answers the user's question:

    CRC A/B window (104,104)..(160,160) R.G/B: on 1f80.9e16/... off f303.f303/... on ... off ... torn 0: cursor pixels reach the output: YES
    cursor pixels reach the output: NO      (on == off: the OTG sees no square, so the loss is upstream of the OTG)
    cursor pixels reach the output: INCONCLUSIVE (...)   (the two "on" reads differ, or one "off" read equals "on" and the other does not)

The method is optc1_configure_crc / optc1_get_crc (dcn10_optc.c:1465-1576); the cursor is read on, off, on, off, back to back. The sprite is opaque, so an "on" CRC does not depend on the desktop; the verbose console scrolls under the window, so only the "off" reads may differ. **YES** = on0 == on2 and on0 differs from both off reads; **NO** = on0 equals both off reads. The window is the square shrunk by 4 px per side (104..160): END's inclusive/exclusive convention is not in the tree, and an edge text pixel must not enter an "on" read. Each sample is read twice and repeated until both agree (`torn` counts the samples that never did); `DATA_B` is masked to 16 bits. `CRC programmed:` prints `OTG_CRC_CNTL` and the four window registers as they read back, so a window that did not latch (`WINDOW_DB_EN`) is visible.
`NO` with the pipe fixes applied means the cursor never leaves the HUBP/DPP side: continue with candidate #2 (`rdna4-cursordlg`, DLG), then #4 (`rdna4-cursormpcsel=1`).

### Emulator

`cursor=on` composites the plane; the model returns `OTG_CRC0_DATA_RG/B` as a CRC-16 over the composited scanout in the window of `OTG_CRC0_WINDOWA_X/Y` while `OTG_CRC_CNTL.EN` is set. This is
NOT the silicon CRC algorithm or its values (unknown to the model), and it takes the window in active-area coordinates, which the silicon may not (blanking offset unverified). It
can only demonstrate that the kext's CRC program/compare logic works and that a plane the model composites is detected; it says nothing about the card's DSCL or MPC dropping the cursor.

Emulator options for the W38 review tests (`RDNA4_DEV=cursor=on,...`, boot-4 arguments plus `rdna4-cursordscl=1`; `tools/vm-test.sh`):

| Option | Effect |
|---|---|
| `dscl-mode=N` | the GOP left `DSCL0_SCL_MODE.DSCL_MODE = N`; 6 also leaves RECOUT/MPC_SIZE 0 (bypass: the model then does not check RECOUT), 1-5 leave a 1280x720 RECOUT (a scaling setup; the model has no scaler and shows "no signal", so the CRC reads 0 and the verdict is INCONCLUSIVE, but the decision line is what the test asserts) |
| `desktop-churn=on` | one desktop pixel inside the CRC window changes with the OTG frame number, like a scrolling console; only the "off" reads see it because the sprite is opaque |

Expected log lines: `dscl-mode=6` -> `case A` + `programming amdgpu's mode-0 set` + `DSCL mode-0 set written`; `dscl-mode=3` -> `case C` + `not written`; default (mode 0, RECOUT = plane) -> `case D` + `not written`;
with `desktop-churn=on` every run must still end in `cursor pixels reach the output: YES` (the previous strict rule reads INCONCLUSIVE there). The model cannot know whether a real DSCL in bypass drops the cursor plane: it composites it.

## 8. W40: CM bypass (`rdna4-cursorcm=1`, `rdna4-cursor=2` only)

Source: premetal/verify-cursor.md sections 2, 7.1 and 8. The card logs `latch domains: ... cm ctl=0x00000001` (hw-logs/rdna4fb-diag-20260929-082237.txt:245): the GOP left `CM0_CM_CONTROL.CM_BYPASS = 1`.
amdgpu clears it whenever a plane is enabled (`dcn401_dpp.c:223` `.dpp_program_gamcor_lut = dpp3_program_gamcor_lut` -> `dpp3_enable_cm_block`, `dcn30_dpp_cm.c:43-54, 219-227`, `REG_UPDATE(CM_CONTROL, CM_BYPASS, 0)`;
`debug.cm_in_bypass` is never set for DCN401), and on DCN4 the cursor unit (`CM_CUR0`) sits inside the CM block. Whether the bypass mux sits before or after the cursor blend is undocumented: the CRC A/B decides.

Read-only, always with `rdna4-cursor=2` (`selftest pre`): the `cm:` line (`CM0_CM_CONTROL 0x0d67`, `POST_CSC_CONTROL 0x0d68`, `BIAS_CR_R/Y_G_CB_B 0x0d75/0x0d76`, `GAMCOR_CONTROL 0x0d77`, `HDR_MULT_COEF 0x0dc1`,
`MEM_PWR_CTRL/STATUS 0x0dc2/0x0dc3`, `DEALPHA 0x0dc5`; offsets verified in dcn_4_1_0_offset.h, base idx 2, `+ dppOff()`) and three `dlg 0x063b/0x0644/0x064d:` lines (the DLG/TTU registers 0x063b-0x0655, `+ hubpOff()`, nine per line).

With `rdna4-cursorcm=1`, in the same boot: CRC A/B `[GOP CM state]` -> CM write -> CRC A/B `[after CM enable]` -> `CM bypass: before NO/YES, after NO/YES`. The write happens under the OTG update lock
(same bracket as modeset.cpp:292-300, held-wait <= 10 ms), each value as amdgpu leaves an SDR RGB plane without degamma:

| Register | Value | amdgpu |
|---|---|---|
| `CM_POST_CSC_CONTROL` | 0 (bypass) | `dcn30_dpp.c:118-120` |
| `CM_BIAS_CR_R`, `CM_BIAS_Y_G_CB_B` | 0 | `dcn30_dpp_cm.c:160-170` |
| `CM_DEALPHA` | 0 | `dcn30_dpp_cm.c:149-158` |
| `CM_GAMCOR_CONTROL` | 0 (whole-register REG_SET) | `dcn30_dpp_cm.c:229-230` |
| `CM_HDR_MULT_COEF` | RMW [18:0] = 0x1f000 (1.0, s6e12) | `dcn30_dpp_cm.c:308-314`, `dcn10_hwseq.c:3247` |
| `CM_CONTROL` | RMW bit 0 = 0, last | `dcn30_dpp_cm.c:43-54` |

Then `CM_UPDATE_PENDING` (`CM_CONTROL` bit 8) is polled clear for at most 50 ms. If the CM is already amdgpu's identity nothing is written and the second CRC is not run. The GOP values are in the `selftest pre: cm:` line to restore by hand.

| Log | Reading |
|---|---|
| `CM bypass: before NO, after YES` | CM_BYPASS was routing the pixel stream around the cursor blend: the fix |
| `before NO, after NO` | not the CM (or not enough): continue with the DLG lines (`dlg`) and mission mode, then DSCL |
| `before YES, after ...` | the cursor already reaches the OTG on this card; look downstream (the panel path) |
| `after INCONCLUSIVE` | the picture changed under the window (a mode change from the CM write would show as a colour shift) |

Emulator: it has no CM (`rdna4_get_cursor` does not read `CM_CONTROL`), and nothing in amdgpu justifies a model of "CM_BYPASS drops the cursor" (it is a guess, [G] in verify-cursor.md section 2.4), so the emulator cannot show the effect.
`cm-bypass=on` only leaves `CM0_CM_CONTROL = 1` in the register file, as the GOP does, to exercise the kext's decision/write/latch path: the two CRCs are equal there by construction.
