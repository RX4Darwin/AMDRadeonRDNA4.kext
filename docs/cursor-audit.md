# W21: hardware cursor audit (rdna4-cursor=1) against amdgpu DCN 4.01

Real card, round 2, boot 6: the pointer was invisible but clicks and hit-testing worked (macOS thinks a
hardware cursor is showing). `hw-logs/rdna4fb-diag-20260928-205336.txt` has no cursor evidence at all: the
kernel message buffer wrapped, so the "HW cursor" and "compute bring-up" sections came out empty.

amdgpu paths below are under `drivers/gpu/drm/amd/display/dc/` (`hubp/...`, `dpp/dcn401/...`) and
`include/asic_reg/dcn/dcn_4_1_0_sh_mask.h`. Kext lines are `src/cursor.cpp` on `premetal/cursor`.

## History that bounds the search

The standalone build fought the same invisible cursor for eight hardware rounds (commits `d0a884a` .. `d5750c1`):
every cursor register matched a working Linux/amdgpu register dump (`tools/cursor-test.py`), the sprite data
read back from VRAM, `CUR0_UPDATE_PENDING` cleared (writes latched), `DCN_VM_FB_LOCATION` covered all of
VRAM, and even a sprite fetched from the scanout base itself (`rdna4-curtest`) was invisible. That
eliminated fetch-address and latch theories and left the composition stage (and the DMUB firmware, which owns
CM_CUR0 bits 2 and 7, `3c713b7`). The W14 port kept those register values; nobody has seen it work.

## Differences found

| # | Where (kext) | amdgpu | Difference | Status |
|---|---|---|---|---|
| 1 | SIZE/pitch/LINES_PER_CHUNK, old `cursor.cpp:555,560` (at 610746e^) | `hubp/dcn32/dcn32_hubp.c:113-118,149-157`; `hubp/dcn20/dcn20_hubp.c:571-589` | SIZE was the image size (e.g. 32x32) while pitch was 64 px and LINES_PER_CHUNK a constant 8 lines. The hardware derives the fetch chunk from width x lines (2 KiB): width 32 needs 16 lines. amdgpu programs the whole cursor buffer (width 64, pitch 64, 8 lines). | FIXED: SIZE = the 64x64 slot, image top-left over transparent pixels (`cursor.cpp:660`) |
| 2 | Hot spot, old `cursor.cpp:556` | `hubp/dcn401/dcn401_hubp.c:811-812,866-878` | The image hot spot was written to CURSOR_HOT_SPOT while IOFramebuffer already passes the hot-spot-adjusted top-left corner (as the parked `cursor.cpp:237` said): subtracted twice, the pointer sits up/left of where it acts. | FIXED: hot spot 0 (`cursor.cpp:665`), used only for corners above/left of the screen (`:699`) |
| 3 | Position, old `drawHardwareCursor` (`:584`) | `dcn401_hubp.c:838-841`, DC clamps at >= 0 with the excess in the hot spot | Negative x/y masked with 0x7fff wrapped to ~32K: off screen at the top/left edge. | FIXED (`cursor.cpp:694-699`) |
| 4 | CURSOR_DST_X_OFFSET, old `:587` wrote raw x | `dcn401_hubp.c:838-856` | amdgpu scales x from pixel clock to DCHUB refclk (`x * ref_clk / pixel_clk`); a wrong value shifts the fetch deadline. | FIXED (`cursor.cpp:702-704`, 100 MHz refclk assumed, pixel clock from the boot timing) |
| 5 | Control rewritten every move | `dcn401_hubp.c:858-864`, `dpp/dcn401/dcn401_dpp_cm.c:138-141` | amdgpu writes CURSOR_ENABLE / CUR0_ENABLE only when the enable changes; every move re-armed `CUR0_UPDATE_PENDING`. | FIXED (`cursor.cpp:709`) |
| 6 | CM_CUR0_CURSOR0_CONTROL bits 2 and 7 (`kCursorCmWorkingBits`) | `dpp/dcn401/dcn401_dpp_cm.c:104-107` sets only MODE, EXPANSION_MODE, ROM_EN | Bits 2 (PIX_INV_MODE) and 7 (PIXEL_ALPHA_MOD_EN) come from the working Linux capture (`565377e`) and are written by the DMUB firmware, not by the driver code. Kept; unverified whether a host write of them is right. | OPEN |
| 7 | DMUB path | every write in `dcn32_hubp.c:143`, `dcn401_hubp.c:861,866`, `dcn401_dpp_cm.c:103,112,138` is skipped when `cursor_offload` is set: the firmware then programs the plane | If Linux on this card uses cursor offload, the firmware owns the cursor and host writes may be discarded. The DMUB experiment (`3c713b7`, VERSION_2) never resolved this. | OPEN, biggest suspect |
| 8 | Address routing | n/a | The kext never logged `DCN_VM_FB_LOCATION`, the AGP window or the HUBP system aperture since W14. `cursor: vm:` now does. | evidence added |
| 9 | premultiply | `dc_cursor_color_format` 2 = premultiplied | We premultiply the prepared image; if VSLPrepareCursor already returns premultiplied data the colours are darkened, not hidden. The `image` line logs max alpha and a VRAM readback. | evidence added |

## Checked and equal to amdgpu

- Register offsets and field shifts: `CURSOR0_0_CURSOR_CONTROL` enable bit 0, REQ_MODE bit 2, MODE [9:8], PITCH [17:16],
  LINES_PER_CHUNK [27:24]; SIZE height [15:0] width [31:16]; POSITION y [14:0] x from bit 15; HOT_SPOT y [15:0] x from
  bit 16 (`dcn_4_1_0_sh_mask.h:7920-7954`); CURSOR_SETTINGS CHUNK_HDL_ADJUST at bit 8 (`:7686-7689`);
  CM_CUR0 control ENABLE/EXPANSION/PIX_INV/ROM/MODE [6:4]/ALPHA_MOD bit 7/UPDATE_PENDING bit 16 (`:11072-11078`).
- CURSOR_REQ_MODE = 1 (`dcn401_hubp.c:1097-1102`), FP scale/bias 1.0 and matrix bypass
  (`dcn401_dpp_cm.c:147-165`), CHUNK_HDL_ADJUST = 3 (`dcn32_hubp.c:159-163`), pitch code 0 = 64 px
  (`dcn10_hubp.c:1123-1144`), mode 2 = premultiplied alpha.
- Instance strides: HUBP 0xdc (`regHUBPREQ1_CURSOR_SETTINGS` 0x072f - 0x0653) and DPP 0x16b
  (`regDPP_TOP1_DPP_CONTROL` 0x0e30 - 0x0cc5).
- `USE_MALL_FOR_CURSOR` (`dcn32_hubp.c:139`): 64x64x4 = 16384 B is not > 16384, so 0.
- The OTG master update lock and VUPDATE pulse (`ensureUpdateLatch`, `device.cpp:1599`) are handled at arming, as in the
  standalone build.

## Other observations

- `device.cpp:1778` arms the cursor right after `discoverPipe()`, before `probeEDID`/`buildModeTable`; nothing after it
  re-checks the latch or the sprite address if a later modeset reprograms the pipe. HDMI modesets reprogram the OTG.
- `plugin.cpp:456` turns the VSL routes on for either `rdna4-vbl` or `rdna4-cursor`; `cscSupportsHardwareCursor`
  answers only when `hwCursorReady` (`ndrv.cpp:192-205`). No csc call is lost silently: `cursor: image` appears exactly when
  macOS calls `cscSetHardwareCursor`.
- The sprite sits at scanout + console length (page aligned), inside the CPU-visible BAR window.

## Evidence that survives (W21 part 1)

- Every `cursor:` line is also appended to the registry property `RDNA4FB,Cursor` (first ~2 KiB, `cursor.cpp:474-500`).
  `tools/diagnostic-log.sh` prints it (`ioreg -l -w0 | grep RDNA4FB,Cursor`) and falls back to `log show --last boot`
  when dmesg lost the early lines (also for `compute:`).
- Per-interrupt IH lines (`CP EOP interrupt`, `SDMA trap interrupt`) are limited to the first three unless
  `rdna4-trace=2` (`ih.cpp:731`). Vblank and page-flip lines were already thinned to 1, 60 and then once a minute.

## Next boot 6: what to read

`RDNA4FB,Cursor` in the diagnostic report:
1. `armed` / `vm:` lines: the scanout and sprite MC addresses, the FB window and system aperture. A sprite address
   outside them explains a zero fetch.
2. `image ...`: `max alpha` > 0 and `(match)`; `MISMATCH` means the CPU writes do not reach VRAM.
3. `set` and `move`/`shown` lines: `hubp ctl` enable bit set, `cm ctl` bit 0 set and `update pending 0`, position
   readback equal to the move, `otg lock` released, `sync` bit 8 set. All right and still invisible points at
   items 6 and 7 (the DMUB-owned bits and cursor offload), not at anything the host programs.

## rdna4-cursor=2 (boot 9): a yes/no answer that does not depend on macOS

`rdna4-cursor=2` programs an opaque magenta 64x64 square at (100,100) through the same `cursorProgramPlane()` macOS's
pointer uses, and then ignores macOS's `cscSetHardwareCursor`/`cscDrawHardwareCursor` (logged as ignored) so the
square stays. On the emulator (`RDNA4_DEV=cursor=on`, boot-6 arguments with `rdna4-cursor=2`) a screendump has exactly
4096 magenta pixels inside (100..163, 100..163) and none outside. On the card: a square means the plane works and the
problem is in what macOS gives it (image, position); no square with correct `selftest:` readbacks points at items 6-7
above (firmware-owned bits / cursor offload).
