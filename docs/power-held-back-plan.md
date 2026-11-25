# The power features held back under lit displays, plan

Status, 2026-11-25: step 1 done and on by default (memory free unless an extra display runs a 4K-class mode); GFXOFF open. Asked for by Sunneva after the OpenMW speed work: display DPM,
memory and fabric deep sleep, GFXOFF.

## What holds them back today (rig: RX 9070 XT, two displays lit, desktop idle)

- **Memory clock: pinned by us.** `raiseDisplayClocks` (`src/clk.cpp`, `kRaised`) sends `SetHardMinByFreq` UCLK = the top DPM level at
  start, with DISPCLK/DPPCLK 650 MHz and DCFCLK 600 MHz for a 4K pipe. Idle now: memory 2505 MHz, GFX 90 MHz, 48 W at the socket, fan 0.
  `rdna4-uclk=0` leaves UCLK alone (the option exists, not tried on the card).
- **The firmware's own features are on.** The board's `FeaturesToRun` (0x048cf19e3afffcfb, logged during the fan work) has DPM_UCLK,
  DPM_FCLK, DPM_DCN, DS_FCLK, DS_DCFCLK, DS_UCLK, DF_CSTATE and GFXOFF set; under SCPM the PMFW ignores the "display-safe" mask the cold-start
  path asks for (docs/gfx-pm-audit.md section 3). So memory/fabric deep sleep and display DPM are not refused: the hard minimum above keeps
  the memory clock, and with it the fabric, from ever dropping.
- **What makes a memory clock switch safe:** the display controller must allow a p-state change (DCHUBBUB watermarks set C/D for the DRAM
  switch latency, or FAMS2: DMUB-managed switching in the vertical blank). Our mode-set engine keeps the watermarks the GOP left
  (`src/modeset.hpp`, "Not programmed"), and FAMS2 is not used. With the hard minimum gone, the PMFW may switch UCLK under scanout: the
  display may then underflow (flicker, a torn or black line) at each switch. Screenshots do not show that; it needs eyes on the displays.
- **GFXOFF:** the kext sends `DisallowGfxOff` for the whole boot. An opt-in exists (`rdna4-gfxoff=1`, the W27 guard: every GC access wakes
  GFX first, an NVRAM flag covers the next boot), but it is refused while the gfx ring or any runtime client is open (section 10 of the
  audit): with WindowServer on our accelerator, that is always. GFXOFF needs the HQD/ring state saved and restored across power-down first.

## Step 1 result (2026-11-25, rig: Samsung 4K on DP as the boot display, Lenovo 1080p on DP lit by the kext)

`rdna4-uclk=0` in config.plist for one boot, Sunneva watching both displays: memory 230-860 MHz at the idle desktop (was 2505), socket
21-33 W (was 47), up to 2495 MHz / 142 W under Quake II and back, 58 MHz / 18 W with the displays asleep, both back after the wake; no
flicker, line or blank seen, the kext's underflow checks 0. Made the default: `memoryClockFollowDisplays` (`src/clk.cpp`) holds UCLK at
its top level only while an extra display runs a 4K-class mode (its 3840x2160 table or 500 MHz of pixel clock and more: the two-4K case
that underflowed on 2026-11-04, commit 6c0e4d9), the lowest level otherwise; re-checked when an extra display is lit, taken down or
switches mode. Default boot after: `UCLK (free)`, memory 96-400 MHz, 19-24 W. Not run: two 4K displays (no second 4K panel here); the hold
itself ran (a build that held it for any second display: 2505 MHz, 46 W), its 4K-class condition did not.

## Plan

1. **UCLK free (one boot, eyes on both displays):** `rdna4-uclk=0`. Watch: UCLK in `rdna4-run sensors` at idle and under a load; the
   socket power at idle; both displays for flicker, a line or a blank when the clock moves (a window drag, a video, display sleep/wake).
   Expected if the GOP's watermarks allow it: memory down to ~100-500 MHz at idle, 15-25 W less. If the screens glitch: step 2.
2. **Watermarks for the switch:** program DCHUBBUB watermark sets for the DRAM change latency from DML (as amdgpu's dcn401 does for the
   modes we set), or ask DMUB for FAMS2 per stream. Host-test against the Linux generator (tools/gen/pipegen) first.
3. **Then the deep sleeps follow on their own** (DS_UCLK/DS_FCLK need the memory clock free to drop); check FCLK and the socket power.
4. **GFXOFF last:** save and restore the gfx ring's and the runtime's HQD state across a power-down, then allow GFXOFF after 100 ms idle as
   amdgpu does; card tests: the self-test, the desktop, sleep/wake, the 24-hour run.

## Risks

- A display that underflows at every UCLK switch looks like flicker under load changes; worst case a blank display until the mode is
  set again. Recovery: reboot without `rdna4-uclk=0`. No firmware or NVRAM change in step 1.
