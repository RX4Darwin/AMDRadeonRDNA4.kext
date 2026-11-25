# The power features held back under lit displays, plan

Status, 2026-11-25: step 1 done and on by default (memory free unless an extra display runs a 4K-class mode); GFXOFF built, opt-in (rdna4-gfxoff=1). Asked for by Sunneva after the OpenMW speed work: display DPM,
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

## GFXOFF: what it needs, and measuring the gain first (started 2026-11-25)

What exists (docs/gfx-pm-audit.md sections 9-10): `rdna4-gfxoff=1` sends AllowGfxOff once at the end of bring-up; every GC register or
doorbell access goes through a guard that wakes GFX first (DisallowGfxOff) and stays awake for the rest of the boot; after the wake the
boot compute HQD and the clock-gating registers are restored. It is refused while the gfx ring (rdna4-gfx) or any runtime client is up,
because those are programmed by MMIO and lost at the power-down (no MES and no MQD-based mapping here). On the default desktop (our
accelerator, WindowServer a client, the gfx ring always up) it therefore never happens.

What real GFXOFF needs: (a) re-arming: allow again after ~100 ms with no GC work, as amdgpu does (amdgpu_gfx.c gfx_off_delay_work);
(b) the gfx ring (CP_RB0 base/size/rptr/wptr, its doorbell, the goldens) and every client's HQD, SH_MEM_BASES/CONFIG and VMID state
restored after each wake, before the first packet; (c) every path that touches GC still through the guard (user-space doorbells would
bypass it: check none exist). The payoff is unknown: at idle GFX already sits at ~40 MHz in deep sleep with clock gating on.

Step 0, measure: a boot with `rdna4-gfxoff=1 rdna4-gfx=0 rdna4-accel=0` (nothing reaches GC after bring-up, so the existing Allow holds)
against the same boot without `rdna4-gfxoff=1`: idle socket power from the SMU metrics (sensors read the SMU only, no GC access). The
difference is what (a)-(c) can win at a still desktop. Worth the work only if it is several watts.

Step 0 result (2026-11-25, rig, accelerator and gfx ring off so nothing reaches GC after bring-up; one `rdna4-run sensors` reading
after 150 s idle, since that call opens a runtime client and wakes GFX): GFXOFF allowed: GFX 0 MHz, **14 W**; the same boot without it:
GFX 40 MHz (deep sleep), **17 W**. So GFXOFF is worth ~3 W at a still desktop (the default accelerated desktop idles at 19-21 W now).
Also seen: after the wake that `rdna4-run` caused, GFX ran at 3.3 GHz and 67 W with no load until the next boot: the existing wake path
leaves the block busy or unmanaged (GRBM 0x0800382c after the wake); to fix before any GFXOFF goes further.

## GFXOFF built (2026-11-25, opt-in `rdna4-gfxoff=1`)

- **Why GFX ran at 3.3 GHz after a wake:** CP_ME_CNTL lost its PFP/ME halt bits at the power-up (0x1500a000 -> 0x0000a000 with
  the gfx ring off; the fetcher then stuck on POST_WPTR_GFX_BUSY, 100 % busy). CP_ME_CNTL is now in the saved registers.
- **Queues:** every active compute HQD (ME1, 2 pipes x 4 queues), SH_MEM_CONFIG/BASES of 16 VMIDs and RLC_CP_SCHEDULERS are
  saved inside the Allow (idle, no accessor) and written back after the wake in hqdInitFor's order; all 3 came back each time,
  the self-test passes after a wake. The gfx ring is set up anew (goldens, CSB, SRM, ring 0, a scratch-register ring test).
- **Re-arming:** a 100 ms thread_call; the Allow needs rtLock (every submission holds it), no GC accessor, GFX idle (GRBM GUI_ACTIVE,
  CP_STAT, CPC), the gfx ring read up to what was written and no client gfx work outstanding. Every packet writer
  (submitIbLocked, gfxClientEmit, the Vulkan submit, sdmaRun, launch) wakes GFX before it writes. The NVRAM flag is written once a
  boot and the trail not at all during a wake (flash wear).
- **The early Disallow:** the SMU answers DisallowGfxOff neither at attach nor at the bring-up start (warm restart); holding GC then
  skipped the driver for the whole boot. Now it goes on; stage 3 sends DisallowGfxOff on every boot. At attach the flag write also
  hung (NVRAM not published): left alone there.
- **Card (rig, accelerated desktop):** ~1 cycle a second with the desktop up (WindowServer's gfx jobs and the kext's copies;
  allowed ~45 % of the time, power unchanged at 19-22 W); with the displays asleep 8 wakes in 40 s, allowed 90 %, GFX 0 MHz,
  15 W (18 W without). OpenMW's 12 cell loads, Quake II, vkcube (MoltenVK), the self-test: no hang, no failed restore. Not run:
  system sleep, a long run, a RADV Vulkan app, the paravirtual mode. `debug.rdna4_gfxoff` (counters by wake source) and
  `debug.rdna4_pmsurvey` (the GC power survey into the log) are read-only sysctls.

## Risks

- A display that underflows at every UCLK switch looks like flicker under load changes; worst case a blank display until the mode is
  set again. Recovery: reboot without `rdna4-uclk=0`. No firmware or NVRAM change in step 1.
