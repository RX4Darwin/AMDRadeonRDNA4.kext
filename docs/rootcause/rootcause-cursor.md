# Root cause: the DCN 4.0.1 hardware cursor is invisible (RX 9070 XT)

This was a read-only analysis: no source was edited and nothing was run on the VM or the card.
Tags: **[F]** means a fact, with its source cited. **[I]** means an inference from facts. **[G]** means a guess.
Path prefixes:
- `k:` = `RDNA4FB-int/src/`
- `dc:` = `~/src/linux-amd/drivers/gpu/drm/amd/display/dc/`
- `dmub:` = `.../display/dmub/inc/`
- `hdr:` = `.../include/asic_reg/dcn/dcn_4_1_0_{offset,sh_mask}.h`
- `log:` = `premetal/hw-logs/rdna4fb-diag-2026092x-`

## 0. Bottom line

The best-supported explanation is this: the kext programs every per-update cursor register amdgpu programs, and they latch. The missing piece is pipe-level state. Linux resets that state by power-gating the pipe and then reprogramming it. The GOP-lit pipe never had that done, and the kext has never read it.

- **#1 suspect: the DPP scaler (DSCL) stage.**
  - DCN4 composes the cursor *after the scaler, in recout space* [F] `dc:hwss/dcn401/dcn401_hwseq.c:1076-1099`.
  - amdgpu on DCN401 never uses full `DSCL_BYPASS` for a 1:1 RGB plane. It uses `SCALING_444_BYPASS` (0) with the line buffer, RECOUT and MPC_SIZE programmed [F] (section 2.1).
  - Nobody has ever read `SCL_MODE`, `RECOUT_*`, `MPC_SIZE` or `LB_*` on the card [F]: no DSCL read exists in `k:cursor.cpp`. The only DSCL writes are the mode-set plan's RECOUT/MPC writes, `k:modeset.cpp:297-299`.
  - If the GOP runs the plane in `DSCL_MODE=6`, or with RECOUT unset, the post-scaler cursor has nothing to be composed into [G].
  - **Confidence: about 30% for #1 alone, about 75% that the cause is one of #1-#3.**
- **Fix for #1:** write amdgpu's `SCALING_444_BYPASS` DSCL set under the OTG update lock (section 4, step B).
- **Cheapest test:** a read-only dump of about 10 DSCL registers, plus an OTG-CRC A/B check that proves or disproves "cursor pixels reach the output" without anyone having to look at the screen (section 3).

## 1. What the evidence establishes, with two corrections

1. **[F] The cursor registers are programmed as amdgpu programs them, and they read back.**
   - The self-test readback: `hubp ctl=0x03000205 addr=0x007e9000/0080 size=0x00400040 pos=0x00320064 dst=0x21 set=0x300`, `cm ctl=0xa5`, `fp=0x3c00/0x3c00`, `mtx=0` (`log:082237.txt:245`).
   - Field layout and values check out against `hdr sh_mask:7920-7935` and `:11072-11105`, `dc:hubp/dcn32/dcn32_hubp.c:108-182`, `dc:hubp/dcn401/dcn401_hubp.c:806-897` and `dc:dpp/dcn401/dcn401_dpp_cm.c:88-165`.
2. **[F] AMD's own minimal DCN401 cursor register set, which the DMUB cursor-offload payload replays on every update, is `dmub:dmub_cmd.h:880-911`.**
   - It holds: address, size, position, hot spot, DST_X_OFFSET, CURSOR_CONTROL (ENABLE/MODE/2X/PITCH/LPC), CM_CUR0 (ENABLE/MODE/EXPANSION/ROM_EN), COLOR0/1, FP scale/bias, CURSOR_SETTINGS **and `DCHUBP_MALL_CONFIG.USE_MALL_FOR_CURSOR`**.
   - The kext writes all of these except USE_MALL_FOR_CURSOR (`k:cursor.cpp:572-600`). COLOR0/1 are mono-only.
   - [I] Every per-update item is therefore covered, and whatever is missing sits at pipe level.
3. **[F] Correction A: latching never needed the MPC lock.**
   - Control boot 17 (`rdna4-cursorlock=0`, so `cursorMpcLock` returns at once, `k:cursor.cpp:791-793`) logs `attributes+enable: cursor update LATCHED after 1 ms` and `selftest: ... LATCHED after 0 ms` (`log:082836.txt:577`).
   - Round 3's `update pending 1` came from dumps taken right after the write (`k:cursor.cpp:1031-1033`), before the `cursorWaitLatched` poll existed.
   - [I] Three latches of 1 ms or less, against a 16.7 ms VUPDATE period, suggest the DPP cursor update applies almost at once. That fits MPCC0 not being mapped to any OPP lock set (`lock_sel=0xf`, see #4).
4. **[F] Correction B: the "scanout-base sprite was also invisible" result (standalone round 8) is weaker than the docs treat it.**
   - It assumed the framebuffer's alpha bytes were 0xFF, but the parked code never logged them (`k:cursor.cpp:124-128,233`).
   - With XRGB alpha = 0, a premultiplied sprite is fully transparent.
   - [I] It does not exclude much. Address routing is still covered independently: the sprite is inside `DCN_VM_FB_LOCATION` and at the same aperture as the working scanout (`log:082237.txt:245`, `vm:` line).
5. **[F] The GOP did not run amdgpu's hubp401 pipe-programming path.**
   - amdgpu sets `CURSOR_REQ_MODE=1` every time it programs the requestor (`dc:hubp/dcn401/dcn401_hubp.c:1092-1104`, called from `dc:hwss/dcn401/dcn401_hwseq.c:3767-3768`).
   - The GOP left `CURSOR_CONTROL=0x01000000`, which has REQ_MODE=0 (`log:082237.txt:245`, `armed:`).
   - [I] The GOP's DLG, requestor and DSCL state is therefore its own and not amdgpu's, and anything amdgpu programs on pipe enable may differ.
6. **[F] Linux's working cursor runs on a pipe that amdgpu power-gated and then reprogrammed.**
   - `init_pipes = dcn10_init_pipes` (`dc:hwss/dcn401/dcn401_init.c:126`) disables every pipe that is not in seamless boot (`dc:hwss/dcn10/dcn10_hwseq.c:1695-1738`).
   - That path runs `dcn401_plane_atomic_power_down`, which sets `DOMAINn_PG_CONFIG.DOMAIN_POWER_GATE=1` (`dc:hwss/dcn401/dcn401_hwseq.c:3094-3115`, `dc:hwss/dcn32/dcn32_hwseq.c:167-184`). Power gating is enabled by default for DCN401 (no `disable_*_power_gate` in `dc:resource/dcn401/dcn401_resource.c:750-806`).
   - [I] On Linux, every HUBP/DPP register amdgpu does not write is at its hardware reset value. On our card, those same registers hold whatever the GOP/VBIOS left.
   - The July Linux diff covered only about 30 registers (`RDNA4FB/tools/cursor-test.py`), and it did **not** compare DCHUBP_CNTL (commit `565377e` message).
7. **[F] Other refuted or neutral points.**
   - `dpp pending 0x2220` means DPP1-3 CONFIG_UPDATE_PENDING (bits 5/9/13, `hdr sh_mask:17514-17525`). DPP0 is clean.
   - DCN401 has no IPS capability in `dcn401_resource.c` (grep: none).
   - Cursor offload stays off unless `config.enable_cursor_offload` is set (`docs/cursor-latch.md` section 3).
   - The position (100,100) is on-screen under any of the three candidate field layouts.

## 2. Ranked candidates

### #1 The DSCL / post-scaler stage is not in amdgpu's state (SCL_MODE, RECOUT, MPC_SIZE, LB)

- **[F] Why this stage matters.**
  - "DCN4 moved cursor composition after Scaler, so in HW it is in recout space ... valid coordinates are only in range from 0,0 - recout width, recout height" (`dc:hwss/dcn401/dcn401_hwseq.c:1076-1099`).
  - The HW position is written recout-relative (`:1172-1207`).
- **[F] What amdgpu programs for a 1:1 RGB plane.** Both the legacy selector and the SPL selector return `SCALING_444_BYPASS` (0):
  - `dc:dpp/dcn401/dcn401_dpp_dscl.c:128-133`
  - `dc:sspl/dc_spl.c:784-791`
  - SPL is on for DCN401: `dcn401_resource.c:2154`.
- **[F] amdgpu never picks full bypass on this ASIC.** `DSCL_BYPASS` (6, enum at `dc:dpp/dcn401/dcn401_dpp.h:688-696`) is returned only for FP16 on a *fixed-format* DSCL (`dcn401_dpp_dscl.c:120-125`), and DCN401's caps are FLOAT (`dc:dpp/dcn401/dcn401_dpp.c:257`).
- **[F] The full list of amdgpu writes in `dpp401_dscl_set_scaler_manual_scale` (`dcn401_dpp_dscl.c:1067-1161`):**
  - AUTOCAL off
  - DSCL_CONTROL=0
  - RECOUT_START/SIZE (`:640-654`)
  - MPC_SIZE
  - `SCL_MODE.DSCL_MODE`
  - LB_DATA_FORMAT and LB_MEMORY_CTRL (`:177-211`); `use_max_lb` gives config 0 (`dcn401_resource.c:779`, `dcn401_dpp_dscl.c:480-484`)
  - EASF disable, because `prefer_easf` is set (`dcn401_resource.c:2155`, `:913-925`)
  - ISHARP off
- **[I] Why the GOP would skip this.** A minimal firmware path would pick full DSCL bypass (no line buffer, no recout) or leave RECOUT at reset. The plane displays either way. The GOP never tested the post-scaler cursor.
- **[G] How this would hide the cursor.** With `DSCL_MODE=6`, or RECOUT_SIZE=0, the recout-space cursor window is empty or bypassed. Every image, every address and any correct CM_CUR0 value would then produce zero pixels, which matches all rounds.
- **What decides it (read-only).** All registers are base 2, DPP0; add `dppOff()` (stride 0x16b: `hdr offset:3879` `DSCL1_SCL_MODE 0x0e73`).

  | Register | Offset | Field layout |
  |---|---|---|
  | `SCL_MODE` | `0x0d08` | `DSCL_MODE [2:0]`, sh_mask:11195 |
  | `RECOUT_START` | `0x0d1e` | |
  | `RECOUT_SIZE` | `0x0d1f` | W `[13:0]` H `[29:16]`, sh_mask:11309-11317 |
  | `MPC_SIZE` | `0x0d20` | |
  | `LB_DATA_FORMAT` | `0x0d21` | |
  | `LB_MEMORY_CTRL` | `0x0d22` | |
  | `DSCL_MEM_PWR_CTRL` / `_STATUS` | `0x0d24` / `0x0d25` | |
  | `OBUF_CONTROL` | `0x0d26` | `OBUF_BYPASS` bit 0; DC never writes it |
  | `DSCL_UPDATE` | `0x0d18` | |
  | `ISHARP_MODE` | `0x0d55` | |
  | `EASF_H_MODE` / `EASF_V_MODE` | `0x0d28` / `0x0d29` | |

  Offsets from `hdr offset:3370-3430`.
- **How to read the result.**
  - `DSCL_MODE==6`, or `RECOUT_SIZE` different from `0x04380780`, means #1 is live. Go to the write test (section 4B).
  - `DSCL_MODE==0` with RECOUT, MPC and LB all set means #1 is eliminated.
  - [G] Reading DPP1-3 (`+0x16b*n`) shows the reset defaults of unused pipes.

### #2 The HUBP DLG is not in mission mode, or its prefetch is unprogrammed, so the cursor prefetch never issues

- **[F] Why the DLG matters.** With `REQ_MODE=1` the cursor is fetched only inside the display prefetch. DML sizes it as `cursor_prefetch_bytes = max(chunk, 4*bytes_per_line)` over `LinesToRequestPrefetchPixelData` (`dc:dml2_0/dml21/src/dml2_core/dml2_core_dcn4_calcs.c:5834-5835`).
- **[F] What amdgpu writes.**
  - `HUBPREQ_DEBUG_DB = 1<<8`, then the whole DLG/TTU set (`dcn401_hubp.c:321-409`, `:433-480`).
  - DCN20 describes bit 8 as "disable dv mode" (`dc:hubp/dcn20/dcn20_hubp.c:176-178`).
  - REQ_MODE=0 at arming (section 1.5) shows the GOP skipped this function family.
- **[F] A gap in the pending W34/W35 experiment.**
  - It writes mission mode only if the GOP already programmed `SURF0_TTU_CNTL0` (`k:cursor.cpp:667-672`).
  - If the GOP left the DLG in test mode with zero values, which is exactly when this cause is live, the round logs "NOT written" and tests nothing.
- **Fix.** Replay amdgpu's DLG/TTU/RQ block for 1080p60 (HUBPREQ0 `0x0620-0x065f` plus `DEBUG_DB=0x100`) under the OTG lock. The values must come from DML21; the cheapest source is `RDNA4FB/tools/linux-capture.sh --snap-only`, whose list already includes HUBP0/HUBPREQ0 (`linux-capture.sh:414-451`).

### #3 Cursor-only state that Linux gets from reset defaults or from writes the kext lacks

- **[F] CURSOR_MEM_PWR_CTRL/STATUS (0x0681/0x0682).**
  - Fields: `CROB_MEM_PWR_FORCE [1:0]`, `DIS [2]`, `STATE [1:0]` (`hdr sh_mask:7967-7976`).
  - DC never writes them (grep `CROB`: none).
  - [G] A VBIOS force-off of the cursor return buffer would fill the sprite with zeros, which are transparent in premultiplied mode.
  - The probe already reads these registers (`k:cursor.cpp:632-637`), but there is no card data yet: `d16-r5` is the emulator.
  - Fix if STATE is not 0: write FORCE=0.
- **[F] `DCHUBP_MALL_CONFIG.USE_MALL_FOR_CURSOR` (0x05f7 bit 2, `hdr sh_mask:7265-7268`).**
  - amdgpu writes 0 for a 16 KiB cursor on every attribute set (`dc:hubp/dcn32/dcn32_hubp.c:133-165`), and DMUB includes the bit (`dmub_cmd.h:909`).
  - The kext has never read or written it. Fix: RMW clear bit 2 inside `cursorProgramPlane`.
- **[F] `DCN_EXPANSION_MODE.CRQ_EXPANSION_MODE` (0x0620 `[3:2]`, `sh_mask:7504-7512`).**
  - amdgpu writes 1 (`dml2_core_dcn4_calcs.c:12509`, `dcn401_hubp.c:293-305`).
  - The GOP value is unknown. Fix: RMW `[3:2]=1`.
- **[F] Also unread:**
  - `CURSOR_STEREO_CONTROL` (0x067f); the probe exists.
  - `UCLK_PSTATE_FORCE` cursor bits (0x0660 `[3:2]`, `sh_mask:7739-7742`).

### #4 MPCC0 not mapped to OPP0's lock set, or state clobbered after arming (low for "invisible")

- **[F] The lock-set mapping differs from amdgpu.** amdgpu maps the MPCC with `MPCC_UPDATE_LOCK_SEL = opp_id` (`dc:mpc/dcn10/dcn10_mpc.c:221-222`). The GOP left `0xf` (`log:082237.txt:245`).
  - [I] This explains the immediate latching (section 1.3), not the missing pixels.
  - Fix, for tear-free updates later: write `0` to base-3 `0x0005` (`hdr offset:5308`).
- **[F] The live state has not been verified since the self-test.** No dump after the self-test re-reads `CURSOR_CONTROL`/`CM_CUR0`: the flip probe logs only TTU and gate (`k:cursor.cpp:616-639`).
  - Add `hubp ctl`/`cm ctl` to `cursorRegProbe`.
  - Add `OTG_PIPE_UPDATE_STATUS` (`0x1b9e`+otg, `CURSOR_UPDATE_PENDING` bit 8, `hdr sh_mask:27615-27618`).
  - Add `HUBPREQ_STATUS_REG2` (`0x0663`; `*_CUR` bits 16-21 are live cursor-request status, `sh_mask:7772-7777`).

### Refuted or unlikely (all [F] from the logs)

| Item | Why it is ruled out |
|---|---|
| MPC cursor lock | the control boot latches without it |
| FP scale/bias | scale half is 0x3c00, bias 0 |
| REQ_MODE | it is 1 |
| Size, pitch, lines-per-chunk, hot spot, DST offset | match amdgpu, see `cursor-invisible-analysis.md` section 1 |
| CUR0 TTU = 0 | amdgpu also writes 0 on DCN401 (`docs/cursor-ttu.md` section 1) |
| DMUB lock / offload / IPS | not active on this configuration (section 1.7) |

## 3. Make the card report the result itself: OTG CRC A/B

[F] DCN401 uses `optc1_configure_crc` and `optc1_get_crc` (`dc:optc/dcn401/dcn401_optc.c:507-508`, `dc:optc/dcn10/dcn10_optc.c:1465-1576`).

**Registers** (base 2, +`otgOff()`, `hdr offset:8643-8655`, `sh_mask:27188-27247`):

| Register | Offset | Fields |
|---|---|---|
| `OTG_CRC_CNTL` | `0x1b65` | `EN` bit 0, `CONT_EN` bit 4, `CRC0_SELECT [22:20]` (0 = union of windows A and B) |
| `CRC0_WINDOWA_X` / `_Y` | `0x1b66` / `0x1b67` | START `[..:0]`, END `[..:16]` |
| `CRC0_WINDOWB_X` / `_Y` | `0x1b68` / `0x1b69` | same as window A |
| `CRC0_DATA_RG` | `0x1b6a` | R `[15:0]`, G `[31:16]` |
| `CRC0_DATA_B` | `0x1b6b` | B `[15:0]` |

**Procedure** (`rdna4-cursor=2`, at the first `cscDrawHardwareCursor`, when the desktop at (100,100) is static):
1. Set windows A and B to x and y from 100 to 164.
2. Set `CONT_EN=1`, `SELECT=0`, `EN=1`.
3. Read the CRC with the cursor on.
4. Clear `CURSOR_ENABLE` (0x0679 bit 0) and `CUR0_ENABLE` (0x0cf1 bit 0), wait 3 vblanks, and read.
5. Turn the cursor on, wait, read. Turn it off, wait, read.
6. Log all four CRCs to `RDNA4FB,Cursor` and to `RDNA4FB,Results`.

**How to read the result.**
- The two "on" values equal each other and differ from the "off" values: cursor pixels reach the OTG.
- All four are identical: the cursor is lost inside DCN, as believed.

Every later experiment (#1-#3) then reports its own PASS/FAIL, and nobody has to look at the screen. [G] The DPP CRC (`0x0cc7-0x0cc9`, `DPP_CRC_SRC_SEL [5:4]`, `sh_mask:12423-12432`) could narrow the loss down to DPP or downstream, but DC never uses it, so its source semantics are unknown.

## 4. Test plan, cheapest first (display-only, all gated on `rdna4-cursor=2`)

**Boot A: read only, no risk.** One more `cursorNote` line at arming, after the self-test, and at the first draw. It should include:
- the DSCL list from #1
- `DCHUBP_CNTL 0x05f4` (bits per `sh_mask:7192-7209`: VTG_SEL, VREADY_AT_OR_AFTER_VSYNC bit 8, UNBOUNDED bit 10, TTU_DISABLE bit 12, TTU_MODE bit 13, UNDERFLOW bit 28)
- `MALL_CONFIG 0x05f7` and `EXPANSION_MODE 0x0620`
- the DLG registers: `BLANK_OFFSET_0 0x063b`, `DST_AFTER_SCALER 0x063e`, `PREFETCH_SETTINGS 0x063f`, `REF_FREQ_TO_PIX_FREQ 0x0654`
- `HUBPREQ_STATUS_REG2 0x0663`
- `OTG_PIPE_UPDATE_STATUS 0x1b9e`
- the CRC A/B from section 3

The W32/W34 `ttu:`/`gate:` lines in the same boot cover the TTU, the debug register, CROB and the MPCC selects.

**Boot B: the #1 fix, only if Boot A shows `DSCL_MODE==6` or RECOUT/MPC not 1920x1080.** New arg `rdna4-cursordscl=1`. The sequence follows amdgpu `dcn401_dpp_dscl.c:1124-1161` inside a pipe lock (like `k:modeset.cpp:292-300`):
1. `OTG_MASTER_UPDATE_LOCK` (0x1b89) = 1, then wait for bit 8.
2. If `DSCL_MEM_PWR_STATUS` is not 0: clear the FORCE fields of `DSCL_MEM_PWR_CTRL` (`sh_mask:11343-11357`) and wait for STATUS=0 (`:149-163`).
3. `DSCL_AUTOCAL` 0x0d19 = 0.
4. `DSCL_CONTROL` 0x0d0a = 0.
5. `RECOUT_START` = 0.
6. `RECOUT_SIZE` = `MPC_SIZE` = `0x04380780`.
7. `LB_DATA_FORMAT` = 0.
8. `LB_MEMORY_CTRL` = `0x00003f00` (config 0, 63 partitions).
9. EASF `H_EN`/`V_EN` = 0.
10. `ISHARP_EN` = 0.
11. `SCL_MODE.DSCL_MODE` = 0.
12. Unlock, then check that `DSCL_UPDATE` bit 0 clears.
13. Run the CRC A/B.

Risk: a wrong LB setting can blank or garble the primary until reboot; this is display-only. Log the GOP's values first so they can be restored.

**Boot C: #3 writes, independent of A and B.** In `cursorProgramPlane`:
- clear `USE_MALL_FOR_CURSOR`
- set `CRQ_EXPANSION_MODE=1`
- clear `CROB_MEM_PWR_FORCE` if `STATUS` is not 0

These are cursor-only fields with near-zero risk.

**Boot D: #2 fix.** If Boot A shows the GOP's DLG is in test mode or zero (`debug_db` bit 8 = 0, `PREFETCH_SETTINGS` = 0), get one Linux `--snap-only` capture at 1080p60 and replay DLG/TTU/RQ + `DEBUG_DB` under the lock. The same capture also diffs #1 and #3 in one go. It is the single most informative action if Linux can be booted.

## 5. amdgpu cursor writes vs `k:cursor.cpp`

| amdgpu write (DCN401 path) | kext | Where |
|---|---|---|
| MPC cursor lock around updates | yes (optional) | `dc_stream.c:313,332,476,491`, `k:cursor.cpp:791-807` |
| Address hi/lo, SIZE, CONTROL (MODE/2X/PITCH/LPC), SETTINGS | yes | `dcn32_hubp.c:143-163`, `k:cursor.cpp:577-589` |
| `DCHUBP_MALL_CONFIG.USE_MALL_FOR_CURSOR` | **no** | `dcn32_hubp.c:165` |
| POSITION, HOT_SPOT, DST_X_OFFSET, ENABLE on change | yes | `dcn401_hubp.c:858-876`, `k:cursor.cpp:1062-1083` |
| CM_CUR0 MODE/EXPANSION/ROM_EN, ENABLE, FP scale/bias, matrix bypass | yes | `dcn401_dpp_cm.c:88-165,169-244`, `k:cursor.cpp:590-595` |
| Pipe setup: REQ_MODE + UNBOUNDED_REQ_MODE | REQ_MODE only | `dcn401_hubp.c:1092-1104` |
| Pipe setup: DEBUG_DB, DLG, TTU, RQ (incl. CRQ expansion) | mission mode opt-in only | `dcn401_hubp.c:293-490` |
| Pipe setup: DSCL mode, RECOUT, MPC_SIZE, LB | **no** | `dcn401_dpp_dscl.c:1067-1161` |
| Pipe setup: `MPCC_UPDATE_LOCK_SEL = opp` | **no** | `dcn10_mpc.c:222` |
| Pipe power-gate reset before enable | **no (GOP state)** | `dcn401_hwseq.c:3094-3115` |
