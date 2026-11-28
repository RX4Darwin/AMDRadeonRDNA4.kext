# Variable refresh (FreeSync / DisplayPort Adaptive-Sync), plan

Status, 2026-11-26: with `rdna4-vrr=1` variable refresh works on both rig monitors, card-verified. The Lenovo (second
pipe) read 48 Hz idle and 70-144 Hz while dragging on its OSD. The Samsung (boot display, 2560x1440@144) counted 48-51
frames a second idle and 86-144 while dragging (debug.rdna4_otghz). macOS gets every EDID block with the option on: the
Samsung's 144 Hz timings are in a DisplayID block that our translator used to hide (it served two blocks). At 4K the
Samsung has only 60 Hz until DSC 4K144 works, and CoreDisplay makes variable copies only near the EDID's top rate.
Measuring: `sudo sysctl debug.rdna4_otghz=<pipe>; sysctl debug.rdna4_otghz` = that OTG's frames in the next second.

## What the monitors and macOS have

- Both rig monitors announce 48-144 Hz in their EDID range limits; the Samsung (Odyssey G70D) also has AMD's FreeSync data
  block (CTA vendor block, OUI 0x00001A). DisplayPort Adaptive-Sync also needs DPCD 0x007 bit 6 (MSA_TIMING_PAR_IGNORED):
  not read yet.
- macOS does variable refresh on x86 through IOFramebuffer: Apple lists the 2019 Mac Pro with Radeon Pro W5500X/W5700X/W6800X
  (AMD's own framebuffer driver) as Adaptive-Sync capable (support.apple.com/102144; Displays > Refresh Rate > Variable).
- IOGraphics has two hooks for it: the framebuffer attribute `kIOFBVariableRefreshRate` ('vrr?') and
  `IODetailedTimingInformationV2::verticalBlankingExtension` ("AdaptiveSync: 0 for non-AdaptiveSync support"). NDRV's
  `VDDetailedTimingRec` has no such field, so IONDRVFramebuffer always reports 0.

## Step 0: what WindowServer asks (rdna4-vrrprobe=1, kept as a diagnostic)

The boot display answered 'vrr?' = 1 and its modes' timing carried the extension down to 48 Hz (509 lines for 4K60, 3053 for
1440p144); every attribute WindowServer got or set on it was logged. Result: WindowServer read the timing info but never asked
'vrr?' or set anything VRR-like, and no variable mode appeared. The Lenovo (head 2, not probed) was black in that boot and
normal again without the probe: keep the probe off unless asked. So the decision is made elsewhere (a property AMD's driver
publishes, a mode flag, or CoreDisplay's own checks), to be found in Apple's binaries before any hardware work.

## Step 1: how CoreDisplay decides (Tahoe 26.6, x86_64, from its code and its debug log)

- A display is Adaptive-Sync capable (IODisplayInstallTimings) when CoreDisplay's preference `enableAdaptiveSync` is on
  (default true), the EDID's base block says digital DisplayPort (byte 20 & 0x8f == 0x85) and continuous frequency (byte 24
  bit 0), and the framebuffer's IOFBTimingRange has kIORangeSupportsVRR; the rates are the EDID range descriptor's V min/max.
- Only timings CoreDisplay installs itself from the EDID get a variable copy (IOFBInstallMode): those within 1 Hz of the
  maximum rate get `verticalBlankingExtension` lines down to the minimum, installed beside the fixed one. The driver's own
  modes never do. On IONDRVFramebuffer an installed timing goes to the NDRV as cscSetDetailedTiming in a programmable slot
  (cscGetNextResolution from kDisplayModeIDFindFirstProgrammable, cscGetDetailedTiming, then cscSwitchMode to the
  0x8000xxxx alias); the extension arrives in VDDetailedTimingRec's csReserved2.
- Before that, each timing has to pass CheckTimingWithRange against IOFBTimingRange. Silent refusals found: a timing above
  165 MHz counts as two links (DVI's rule), so csMaxNumLinks must be 2; the table's slowest mode needed a margin below the
  minimum pixel clock.
- CoreDisplay's debug log names every decision: `sudo log config --subsystem com.apple.CoreDisplay --mode
  "level:debug,persist:debug"` (the rig has a sudoers entry for it), then `log show --last boot --debug` after a reboot.
  tools: a probe that calls CheckTimingWithRange inside CoreDisplay (anchored on CDNotificationRegisterCallbackBlock)
  settled which field refused a timing.

The translator (src/ndrv.cpp, only with `rdna4-vrr=1`) answers cscGetTimingRanges and keeps 8 programmable slots; a
timing macOS installs is accepted only if it is a table mode (same raster, clock within 0.1 %), which it then stands for.

Left: the Samsung's 144 Hz timings are in its DisplayID blocks, and IODisplayEDID holds only the first 256 bytes;
the hardware (step 3); an early black-screen boot on 2026-11-26 with this kext (nothing logged, the next boot fine,
probably unrelated).

## Plan

1. (Done, see above.) Find what makes macOS offer Variable: look for 'vrr?', verticalBlankingExtension and Adaptive-Sync strings in IOGraphics
   (IOFramebuffer), SkyLight/CoreDisplay (the shared cache) and an AMD framebuffer kext of Tahoe's, and what each reads.
2. Present that for a DP sink with Adaptive-Sync (EDID range, DPCD 0x007 bit 6).
3. (Done for the second pipe.) Hardware, from Linux: pipegen's "drr" scenario runs dcn10_set_drr (on, off) and the
   manual trigger for each pipe into src/drr_linux.inc (src/drr.cpp). Switching to a variable timing turns it on with the
   mode's V total as the minimum and the extension added as the maximum, and sets DPCD 0x107 bit 7 (ignore MSA) when the
   sink has 0x007 bit 6; each flip (rtScanoutFlip) then writes OTG_TRIGA_MANUAL_TRIG, as Linux after a flip. Only flips end
   the blank (Linux's default): a cursor moving over a still desktop is shown at the slow rate.
4. Card: a game or a moving window at a rate under the maximum, the monitor's own refresh read-out; display sleep/wake.

## With HDR on the boot display (2026-11-28)

CoreDisplay installs no variable timings of its own on a display it counts as HDR-capable: with `rdna4-hdr=1` the Samsung
got no programmable timings at all in four boots (no cscSetDetailedTiming), whatever the timing range declared (all four HDR
fields, or only the dynamic ranges), while a boot whose range made it not HDR-capable installed 17 and the Lenovo always gets
its own; its range check (CheckTimingWithRange, probed) passes the timings either way. So the Samsung had no Variable even in
SDR, and HDR switched on from a variable mode could only land on the fixed HDR twin (CoreDisplay pairs twins by timing).

So for a display with HDR twins the translator lists variable modes of its own (Ndrv::Translator::kVrrIdBit): a copy of every
mode within 1 Hz of the EDID's top refresh, its timing carrying the blanking extension down to the EDID's lowest refresh, and
an HDR10 twin of each copy with the same extension. CoreGraphics lists the copies (0x10000066 is 2560x1440@144 variable). A
switch to a copy runs variable; HDR switched on from it lands on its twin (0x30000066), which runs on the base mode of its
refresh (3840x2160@144 over DSC on the Samsung) with the extension taken to that timing. Card, 2026-11-28: the copy at
48-144 Hz measured 48 frames/s on an idle desktop; with HDR on, V total 2314..6941 lines on the 4K144 base, 50 frames/s idle.
The copies are listed first: CoreDisplay restores a saved mode by what it looks like, and a copy looks like its fixed mode,
so with the fixed one first a saved variable mode came back fixed (and with HDR on as the fixed twin); listed first, the saved
variable HDR mode came back at login. Variable is then what a restore picks for the top refresh, as macOS does on a display
where it installs variable timings itself.
A Linux capture with HDR and GNOME's "+vrr" modes (test/ref/linux-vrrhdr, tools/linux/vrrhdr-snap.py) showed amdgpu not running
variable on the desktop either (V_TOTAL_MIN = MAX); it sends a FreeSync infopacket (GSP2) and the VSC SDP (GSP0) even in SDR,
and drives HDR at 10 bits a component.
