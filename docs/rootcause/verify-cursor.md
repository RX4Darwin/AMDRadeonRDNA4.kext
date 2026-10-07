# Adversarial verification: why the DCN 4.0.1 hardware cursor stays invisible (round 5)

This was a read-only review: no source was edited, and no VM, QEMU or card run was made.
Tags: **[F]** is a fact with its source, **[I]** an inference from facts, **[G]** a guess.
Path prefixes:
- `dc:` = `~/src/linux-amd/drivers/gpu/drm/amd/display/dc/`
- `dm:` = `.../display/amdgpu_dm/`
- `hdr:` = `.../include/asic_reg/dcn/dcn_4_1_0_{offset,sh_mask}.h`
- `k:` = `RDNA4FB-int/src/`
- `log:` = `premetal/hw-logs/rdna4fb-diag-20260929-`

The Linux tree is at d266640 (7.3-rc5).

## 0. Verdict

| Item | Verdict | Confidence |
|---|---|---|
| Per-update cursor registers equal amdgpu's (HUBP, CM_CUR0, FP scale/bias, lock) | **CONFIRMED** | 95% |
| Round-5 diagnostics (OTG CRC A/B, DSCL/pipe dumps) are correct and worth running | **CONFIRMED** | 85% |
| "Round-5 fixes will make the square visible" (DSCL case A, MALL, CROB, conditional DEBUG_DB, MPC lock) | **DOUBTFUL**, about 20% | 70% |
| "#1 = DSCL bypass" (rootcause-cursor.md) as the top theory | **DOUBTFUL**, demoted to #3 | 65% |
| DMUB / IPS / cursor offload owns the cursor | **REFUTED** | 95% |
| FP scale/bias (SDR white level) wrong | **REFUTED** | 98% |

**New #1 candidate, never checked by anyone: the GOP left the DPP colour-management block in bypass.**
- `CM0_CM_CONTROL.CM_BYPASS = 1`. [F] `log:082237.txt:245`: `latch domains: ... cm ctl=0x00000001`.
- amdgpu **always** writes `CM_BYPASS = 0` when it enables a plane (section 2).
- DCN 4 moved the cursor unit **into the CM block**: `CNVC_CUR0` became `CM_CUR0`, and composition moved after the scaler.
- The kext never writes `0x0d67` [F] (`k:cursor.cpp:442,1068` only read it).
- **Probability: about 40%. It is not in round 5.**

## 1. Every logged value, bit by bit, against amdgpu (all [F])

Sources: `hdr sh_mask` lines as cited; values from `log:082237.txt:245` (boot 9) and `log:082836.txt` (boot 17, identical).

| Register = value | Decode | What amdgpu writes | Match |
|---|---|---|---|
| `CURSOR0_0_CURSOR_CONTROL = 0x03000205` (`sh_mask:7920-7935`) | ENABLE[0]=1, REQ_MODE[2]=1, 2X[4]=0, MODE[10:8]=2, TMZ[12]=0, PITCH[17:16]=0, XY_ROT_BYPASS[20]=0, LINES_PER_CHUNK[28:24]=3 | MODE=2 premultiplied (`dm:amdgpu_dm_plane.c:1699`); 2X=0 (flags=0, `:1700`); PITCH 0=64 px (`dc:inc/hw/hubp.h:50-54`, max cursor 64 `dc:resource/dcn401/dcn401_resource.c:2075`); LPC 3=8 lines for 33-64 px (`dc:hubp/dcn20/dcn20_hubp.c:584-585`, `hubp.h:56-61`); REQ_MODE=1 (`dc:hubp/dcn401/dcn401_hubp.c:1097-1103`); bit 20 never written, reset 0 (GOP `0x01000000`) | yes |
| `CURSOR_SIZE = 0x00400040` (`:7943-7946`) | W[24:16]=64, H[8:0]=64 | buffer size (`dc:hubp/dcn32/dcn32_hubp.c:145-147`) | yes |
| `CURSOR_POSITION = 0x00320064` (`:7948-7951`) | Y[14:0]=100, X[29:15]=100 | recout-relative x,y (`dcn401_hubp.c:866-869`) | yes, if RECOUT starts at 0 |
| `HOT_SPOT = 0`, `DST_OFFSET = 0x21` (`:7953-7966`) | DST_X = 33 = 100*50000/148511 | `dcn401_hubp.c:838-856` (x_viewport * ref / pix) | yes |
| `CURSOR_SETTINGS = 0x300` (`:7686-7687`) | DST_Y_OFFSET 0, CHUNK_HDL_ADJUST 3 | `dcn32_hubp.c:159-163` | yes |
| `ADDR = 0x0080 / 0x007e9000` (`:7937-7941`) | MC `0x80_007e9000`, in `DCN_VM_FB_LOCATION` 0x8000-0x83fb, same space as the primary `0x80_00000000` | MC address of the BO | yes |
| `CM_CUR0_CURSOR0_CONTROL = 0xa5` (`:11072-11085`) | ENABLE[0]=1, EXPANSION[1]=0, PIX_INV[2]=1, ROM_EN[3]=0, MODE[6:4]=2, ALPHA_MOD[7]=1, PENDING[16]=0 | MODE/EXPANSION=0/ROM_EN (`dc:dpp/dcn401/dcn401_dpp_cm.c:102-106`), ENABLE (`:138-141`); bits 2 and 7 = reset (GOP `0x84`) = July Linux capture `0xa5` (RDNA4FB `565377e`); ROM_EN=1 only with legacy gamma (`dm:amdgpu_dm_plane.c:1703-1707`, `amdgpu_dm_color.c:1290-1313`) | yes |
| `FP_SCALE_BIAS_G_Y / _RB_CRCB = 0x00003c00` (`:11093-11101`) | SCALE[15:0]=0x3c00 = 1.0 (s1e5m10), BIAS[31:16]=0 | `hw_scale=0x3c00, bias=0` (`dc:hwss/dcn10/dcn10_hwseq.c:3942-3966` -> `dcn401_dpp_cm.c:147-165`); DM never sets `sdr_white_level` (grep `amdgpu_dm/`: no hit) | yes (already 0x3c00 at arming) |
| `CUR0_MATRIX_MODE = 0` (`:11103-11108`) | bypass | forced bypass (`dcn401_dpp_cm.c:234-244`) | yes |
| `MPC_DPP_PENDING_STATUS = 0x2220` (`:17514-17525`) | bits 5, 9, 13 = `IN_DPP1/2/3_CONFIG_UPDATE_PENDING`; DPP0 bits 0-2 (incl. CURSOR bit 2) = 0; same before any write | n/a | harmless |
| `MPCC0_MPCC_CONTROL = 0xffff0461` (`:17085-17092`) | MODE 1 (top passthrough), BLND 2 (global), MULT 1, BG_BPC 4, global alpha/gain 0xff | normal | ok |
| `MPCC_UPDATE_LOCK_SEL = 0xf` | not in any lock set | amdgpu writes the OPP id on insert (`dc:mpc/dcn10/dcn10_mpc.c:222`), and 0xf only on *removal* (`:319,330,364`) | **differs** (explains the 0-1 ms latch, see #6) |
| `CNVC FORMAT_CONTROL = 0x24000000` (`:10909-10919`) | crossbar 0/1/2, ALPHA_EN 0, CNVC_BYPASS 0 | `dpp401_dpp_setup` (`dcn401_dpp.c:75-85`) | ok |
| `DPP_TOP0_DPP_CONTROL = 0x70000010` (`:12385-12393`) | DPP_CLOCK_ENABLE 1 | ok | ok |
| **`CM0_CM_CONTROL = 0x00000001`** (`:11809-11812`, `offset:3556`) | **CM_BYPASS[0]=1**, CM_UPDATE_PENDING[8]=0 | **always 0** (section 2) | **DIFFERS** |

[I] Every cursor register is amdgpu-exact. The only logged register that differs from what amdgpu produces on this ASIC is `CM_CONTROL` (plus the MPCC lock select).

## 2. The difference nobody checked: `CM_BYPASS`

1. **amdgpu clears CM_BYPASS on every plane enable.** [F]
   - `dcn401_program_pipe` calls `set_hdr_multiplier` and `set_input_transfer_func` when the pipe is enabled (`dc:hwss/dcn401/dcn401_hwseq.c:2299-2308`).
   - For DCN401 these are `dcn10_set_hdr_multiplier` and `dcn32_set_input_transfer_func` (`dc:hwss/dcn401/dcn401_init.c:131,157`).
   - `dcn32_set_input_transfer_func` calls `dpp_program_gamcor_lut` unconditionally (`dc:hwss/dcn32/dcn32_hwseq.c:559`).
   - For DCN401 that is `dpp3_program_gamcor_lut` (`dcn401_dpp.c:223`). Its first statement calls `dpp3_enable_cm_block` (`dc:dpp/dcn30/dcn30_dpp_cm.c:219-227`), which does `REG_UPDATE(CM_CONTROL, CM_BYPASS, 0)` unless `debug.cm_in_bypass` is set (`:43-54`).
   - `cm_in_bypass` (`dc:dc.h:1176`) is not set anywhere in `dcn401_resource.c` (grep: none).
   - **So on Linux every visible cursor sits on a DPP with CM_BYPASS = 0.**
2. **The GOP left CM_BYPASS = 1.** [F] `log:082237.txt:245`, `log:082836.txt` (`cm ctl=0x00000001`). This is the "full bypass" style amdgpu itself uses only in `dpp*_full_bypass` (`dc:dpp/dcn10/dcn10_dpp_cm.c:770-783`, `dc:dpp/dcn60/dcn60_dpp.c:274-294`), which DCN401 does not have (`dcn401_dpp.c:234`: `.dpp_full_bypass = NULL`).
3. **DCN 4 put the cursor unit inside the CM.**
   - [F] DCN 3.2 and 3.5 call the block `CNVC_CUR0` (pre-scaler format converter; `hdr dcn_3_2_0_offset.h:3281`, `dcn_3_5_0_offset.h:4505`). DCN 4.1 renames it `CM_CUR0` (`offset:3326`).
   - [F] amdgpu: "DCN4 moved cursor composition after Scaler" (`dcn401_hwseq.c:1076`) and "Program Cursor matrix block in DPP CM" (`dcn401_dpp_cm.c:168,234`).
   - [F] The CM (post-scaler) holds POST_CSC, BIAS, GAMCOR, HDR_MULT and DEALPHA (`offset:3556-3742`).
   - [I] The cursor has its own degamma ROM, FP scale and matrix: the exact counterparts of GAMCOR, HDR_MULT and POST_CSC. That points to a blend at or near the end of the CM chain.
4. **[G] What follows.** If `CM_BYPASS` routes the pixel stream around the whole CM, it routes it around the cursor blend too.
   - Every cursor register would then read back right and latch, and zero cursor pixels would come out, for any image, position and address.
   - That is exactly all three rounds. It is also why the July "all registers match Linux" diff found nothing: `CM_CONTROL` was never in it (`RDNA4FB/tools/cursor-test.py:10-42`).
   - The new Linux ground-truth list would miss it too: its CM entries are only `CM_CUR0_*` (`RDNA4FB-int/tools/linux-groundtruth.sh:464-481`).
5. **Why the earlier analyses passed over it.** `cursor-invisible-analysis.md:39` labelled `CM_BYPASS = 1` "normal for no color management", with no source cited. That inference is contradicted by the amdgpu chain in point 1.
6. **Against this theory.** [G] The bypass mux could sit *before* the cursor stage, in which case CM_BYPASS is irrelevant. No public document settles this. It is decided in one boot by the CRC A/B (section 7).
7. **The emulator cannot catch it.** [F] `rdna4_get_cursor` ignores `CM_CONTROL` (`RDNA4FB-int/emu/qemu/rdna4.c:6488-6560`), so every VM pass is silent on it.

## 3. DCN4-specific requirements (question 2)

- **Recout space.**
  - [F] HW position is recout-relative (`dcn401_hwseq.c:1076-1099`, `:1172-1207`), and amdgpu drops the cursor when `recout_x >= recout.width` (`:1195-1205`).
  - [I] (100,100) is valid only if the DSCL `RECOUT` is at least 164x164. This is unknown on the card: no card log has a `dscl:` line (grep of `hw-logs/`: none). Round 5 decides it; this is candidate #3.
- **DST_X_OFFSET** is a viewport-relative fetch deadline, not a clip (`dcn401_hubp.c:838-856`). The value 33 is right [F].
- **Enables.**
  - [F] Both halves are set: HUBP `CURSOR_ENABLE` and DPP `CUR0_ENABLE` (section 1).
  - [F] A grep of `sh_mask` for every cursor field outside HUBP and CM_CUR0 finds only status/lock fields: `MPC_DPP_PENDING_STATUS`, `CUR_VUPDATE_LOCK_SET`, `OTG_CURSOR_UPDATE_PENDING` (`:27617`), MPCC debug sideband IDs (`:55554,55612`), and `DCHUBBUB_SDPIF_PIPE_CURSOR0_SEC_LVL` (`offset:1766`, `sh_mask:6404-6409`).
  - **There is no MPCC or OPP cursor enable.**
- **HUBP_TTU_DISABLE.**
  - [F] It is `DCHUBP_CNTL` bit 12 (`sh_mask:7201`). Linux always writes 0 (`hubp2_set_blank_regs`, `dc:hubp/dcn20/dcn20_hubp.c:965-988`, used by DCN401 at `dcn401_hubp.c:1116`).
  - It is unknown on the card; round 5's `pipe:` line logs it.
- **SDPIF cursor security level (0x0481).** [F] DC never writes it and the kext never reads it. [G] It is low-probability.

## 4. DMUB, IPS and cursor offload (question 3): REFUTED

All points are [F]:
- `should_use_dmub_inbox1_lock` returns false for `>= DCN_VERSION_4_01` (`dc:dce/dmub_hw_lock_mgr.c:107-113`). The cursor lock is the MPC register.
- `config.enable_cursor_offload` is only *read* (`dc:dc_dmub_srv.c:1206`). A grep of all of `drivers/gpu/drm/amd` finds no assignment, so the HUBP/DPP writes are never skipped on Linux (`dcn32_hubp.c:143`, `dcn401_hubp.c:861,866`).
- `dcn401_resource.c` has no IPS (grep `ips`: none).
- The kext's values persist in every readback, so nothing overwrites them.
- `UPDATE_CURSOR_INFO` is PSR/Replay-only (`docs/cursor-latch.md` section 3).

## 5. SDR white level / FP multiplier (question 4): REFUTED

- [F] `0x3c00` is the **scale** half (bits 15:0) and equals amdgpu's default: `hw_scale = 0x3c00` ("1.0 default multiplier"), s1e5m10 format, **bias = 0** (`dcn10_hwseq.c:3946-3963`).
- [F] It was already `0x3c00/0x3c00` at arming, before any kext write (`log:082237.txt:245`, `armed: cm ctl=0x00000084 fp=...`).
- [I] The FP stage has only G_Y and RB_CRCB fields and no alpha field (`sh_mask:11093-11101`). A scale of 0 would therefore give an **opaque black** square, not an invisible one. The July reasoning "multiplied to invisibility" (`RDNA4FB` `ab1c117`) could not have been right either.

## 6. Ranked candidates

| # | Cause | P | In round 5? | Decided by |
|---|---|---|---|---|
| 1 | `CM0_CM_CONTROL.CM_BYPASS=1`: the cursor blend inside the CM is bypassed | **40%** | no (read only) | CM enable + CRC A/B (section 7.1) |
| 2 | DLG left in DV/test mode or unprogrammed, so the REQ_MODE=1 prefetch or the post-scaler cursor delivery (`DST_AFTER_SCALER`) never happens | 18% | bit 8 written only if the GOP programmed SURF0 TTU (`k:cursor.cpp:681-686`); no DLG values dumped | `debug_db`, `0x063b-0x0655`, Linux capture |
| 3 | DSCL full bypass (mode 6) or RECOUT unset, so recout space is empty | 12% | yes, case A only (`k:cursor.cpp:846-918`) | `dscl:` line |
| 4 | `HUBP_TTU_DISABLE=1` | 4% | logged, no write | `pipe: dchubp_cntl` bit 12 |
| 5 | CROB memory forced off / `USE_MALL_FOR_CURSOR` | 4% | yes | `pipe:` line |
| 6 | `MPCC_UPDATE_LOCK_SEL=0xf` (MPCC in no lock set) | 3% | opt-in `rdna4-cursormpcsel=1` | A/B |
| 7 | SDPIF cursor security level | 3% | no | read `0x047e`/`0x0481` |
| 8 | Unknown | 16% | | Linux ground truth |

Round-5 chance of a visible square is about 12% (#3) plus about 4% (#2, when the conditional write fires and suffices) plus about 4% (#5): **about 20%**. [I] The CRC will most likely say `NO`, and that is still useful.

## 7. Top-3 exact register changes

All registers are base idx 2 unless noted. DPP registers take `+ dppOff()` (stride 0x16b: `CM1_CM_CONTROL 0x0ed2 - 0x0d67`, `offset:4065`), HUBP registers `+ hubpOff()` (stride 0xdc).

**7.1 (#1) Enable the CM as amdgpu does for an RGB SDR plane without degamma.**
- **Where.** New opt-in `rdna4-cursorcm=1`, run inside the existing OTG update-lock bracket of `cursorDsclDecide` (`k:cursor.cpp:880-911`).
- **Before writing,** log `0x0d67 0x0d68 0x0d75 0x0d76 0x0d77 0x0dc1 0x0dc2 0x0dc3 0x0dc5 0x0dc6` so the GOP values can be restored.
- **Writes, in order:**

  | Register | Write | Meaning | amdgpu source |
  |---|---|---|---|
  | `CM_POST_CSC_CONTROL` `0x0d68` | 0 | POST_CSC_MODE bypass | `dc:dpp/dcn30/dcn30_dpp.c:118-120`; `select` defaults to BYPASS for RGB, `dcn401_dpp.c:67` |
  | `CM_BIAS_CR_R` `0x0d75` | 0 | no bias | `dcn30_dpp_cm.c:160-170` |
  | `CM_BIAS_Y_G_CB_B` `0x0d76` | 0 | no bias | `dcn30_dpp_cm.c:160-170` |
  | `CM_DEALPHA` `0x0dc5` | 0 | dealpha off | `dcn30_dpp_cm.c:149-158`; fields `sh_mask:12364-12367` |
  | `CM_GAMCOR_CONTROL` `0x0d77` | 0 | GAMCOR_MODE bypass (whole-register `REG_SET`) | `dcn30_dpp_cm.c:229-230` |
  | `CM_HDR_MULT_COEF` `0x0dc1` | RMW `[18:0] = 0x1f000` | 1.0 in s6e12 | `dcn10_hwseq.c:3247`, `dcn30_dpp_cm.c:308-314`; mask `sh_mask:12353-12354` |
  | `CM_CONTROL` `0x0d67` | RMW bit 0 = 0 (**last**) | CM_BYPASS off | `dcn30_dpp_cm.c:43-54` |

- **After the writes:** unlock, poll `CM_UPDATE_PENDING` (`0x0d67` bit 8) until it clears (at most 50 ms), then run `cursorCrcCheck()` again.
- **Make the boot its own A/B:** CRC before the write (expected `NO`), CRC after it. `NO` then `YES` proves #1.
- **Risk:** with identity sub-blocks the primary should look unchanged (it is the path Linux runs). The worst case is wrong colours or black until reboot; this is display-only.

**7.2 (#2) DLG mission mode with real DLG values.**
- **Registers:** `HUBPREQ_DEBUG_DB` `0x05fc` = `0x00000100` (`dcn401_hubp.c:329`), together with the DLG/TTU block amdgpu writes in the same call:
  - `0x063b-0x0655` and `0x065a-0x065f` (`dcn401_hubp.c:321-409`)
  - `0x0621-0x0628` (`dcn401_hubp.c:433-480`)
  - `HUBP_TTU_DISABLE=0` (`dcn20_hubp.c:986-988`)
- **Where the values come from:** DML21, so take them from the Linux capture (`linux-groundtruth.sh` already lists `0x05f4`, `0x05fc`, `0x063e`, `0x063f`: lines 302, 310, 359-360). Write them under the OTG update lock.
- **Round 5 first:** dump `0x063b-0x0655` next to `debug_db`, so the GOP's DLG state is known before anything is written.

**7.3 (#3) DSCL mode-0 set (already implemented).**
- The sequence is `dpp401_dscl_set_scaler_manual_scale` (`dc:dpp/dcn401/dcn401_dpp_dscl.c:1067-1161`) as the W38 code writes it: `RECOUT_SIZE=MPC_SIZE=0x04380780`, `LB_MEMORY_CTRL=0x3f00`, `DSCL_MODE=0`.
- It fires only in case A (`DSCL_MODE==6`).
- Keep it, but note that RECOUT matters for the cursor even in mode 0.

## 8. Cheap additions before or with round 5

1. **Read-only, zero risk.** Add the ten CM0 registers of 7.1 to `cursorDsclDump` (`k:cursor.cpp:756-776`). `CM_HDR_MULT_COEF` and `GAMCOR_CONTROL` also bound the risk of 7.1 before it is ever written.
2. **Linux capture.** Add `CM0_CM_CONTROL 2 0x0d67`, `0x0d68`, `0x0d75-0x0d77`, `0x0dc1-0x0dc6`, `DCHUBBUB_SDPIF_PIPE_SEC_LVL 0x047e`, `_CURSOR0_SEC_LVL 0x0481` and `MPCC0_*` (base 3, `0x0000-0x0005`) to the `dcn-list` in `tools/linux-groundtruth.sh:287+`. **[I] Expect `CM_CONTROL=0x00000000` on Linux**, which would confirm the premise of #1 without the card.
3. **Order in boot 4.** Keep the round-5 diagnostics. Add 7.1 as the last step of `cursorSelfTest`, after the first CRC, so that one boot yields: GOP DSCL/CM/DLG state, CRC with the GOP CM state, and CRC with the CM enabled.
