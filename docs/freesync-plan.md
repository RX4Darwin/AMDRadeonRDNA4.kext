# Variable refresh (FreeSync / DisplayPort Adaptive-Sync), plan

Status, 2026-11-26: step 0 (what macOS asks for) run once on the card; how macOS decides to offer "Variable" refresh is not
known yet.

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

## Plan

1. Find what makes macOS offer Variable: look for 'vrr?', verticalBlankingExtension and Adaptive-Sync strings in IOGraphics
   (IOFramebuffer), SkyLight/CoreDisplay (the shared cache) and an AMD framebuffer kext of Tahoe's, and what each reads.
2. Present that for a DP sink with Adaptive-Sync (EDID range, DPCD 0x007 bit 6).
3. Hardware, from Linux (pipegen, as the rest): OTG DRR (V_TOTAL_MIN/MAX, the flip as the event that ends the extended blank),
   MSA_TIMING_PAR_IGNORE_EN in DPCD 0x107, for the timing macOS picks.
4. Card: a game or a moving window at a rate under the maximum, the monitor's own refresh read-out; display sleep/wake.
