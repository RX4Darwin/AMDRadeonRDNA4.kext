# Driver-level FSR 1 upscaling ("Super Resolution"), plan

Status, 2026-11-24: step 0 done (the display-mode route covers few games); the kernels done; the Metal bundle route chosen and built,
card-tested with a test window (below); the games tested present full-size frames. AMD's source is the `lib/FidelityFX-FSR` submodule (FSR v1.0.2).

## Why FSR 1 and not FSR 4

FSR 4 ships only as AMD's signed Windows DLLs (`amd_fidelityfx_upscaler_dx12.dll`, `..._vk.dll`) under a licence that allows
redistribution in binary form and forbids reverse engineering. It is temporal: the game must hand it motion vectors, depth and its
camera jitter, so a driver cannot add it to a game that does not ship it. FSR 1 (EASU upscaling plus RCAS sharpening) is spatial: it
needs only the finished frame, which is what the driver has. Its source, `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h`
in the FidelityFX SDK, is MIT licensed. Radeon Super Resolution in AMD's Windows driver is the same idea.

## The display-mode route (described first, not taken)

With **Super Resolution** on in RDNA4 Gadget's new **Tweaks** pane, a display mode smaller than the monitor's largest one no longer
changes the monitor's timing. The head stays on its native mode, macOS draws frames of the smaller size, and each presented frame is
upscaled on the card with EASU and sharpened with RCAS into the scanout. The picture keeps its aspect ratio, centred, with black bars
where the aspects differ. Off (the default), modes work as today: the monitor scales.

Covered: anything that switches the display to a lower mode, which is what games' exclusive full screen does (SDL 2's
`SDL_WINDOW_FULLSCREEN` on macOS sets the mode). Also the desktop, if someone picks a low non-HiDPI mode.

Not covered: HiDPI modes, including "looks like" (their frames are already at or above the head's size). Games that keep the display mode
and render a smaller drawable in a full-screen window; CoreAnimation scales those inside WindowServer, out of our reach. Those could be
done later in the Metal bundle (EASU on the drawable at present), a separate project.

## Where the display-mode route would have gone (not taken)

- Native present path: `RDNA4PvHost` present code (`src/pvgpu.cpp`, about line 8500) copies or scales each frame into the head's
  scanout with a compute dispatch in the frame's IB. `scale.cl` already runs there for "looks like" frames larger than the head
  (`scaleKernel`, `scaleFrame`). FSR goes beside it: frames smaller than the head both ways.
- Mode switch: `deviceSwitchTo` and `head2SwitchTo` (`src/plugin.cpp`) already keep the head on its largest mode for a
  `Modes::SourceScaled` mode. `Translator::scaledFrame` (`src/ndrv.cpp`) tells the cursor (`PvScanout::cursorMove`) and the pipe's mode
  attribute 0x10 (`src/rdna4accel.cpp`) the frame's size. Upscaled modes reuse all of that.
- Kernels: OpenCL C compiled by clang for gfx1201 with `-nogpulib` (`tools/gen/build-shaders.sh` -> `src/*_codeobj.h`), loaded with
  `rtLoad`.

## Step 0 result (2026-11-24, on the test machine: Samsung 4K over DP, Tahoe 26.6.2)

Each game full screen at 1920x1080, the display's mode read with `cgmode` while it ran:

| Game | What the display did | A driver upscaler would see |
|---|---|---|
| vkQuake (MoltenVK) | switched to a real 1920x1080 @ 60 Hz mode | low-resolution frames: yes |
| OpenMW (SDL 2) | switched to the HiDPI "looks like 1920x1080" mode: 3840x2160 pixels | native-size frames: no |
| Quake II, Metal port (SDL 2) | the same as OpenMW: 3840x2160 pixels | no |
| Daggerfall Unity (Unity) | no mode change: a full-screen window, macOS scales the frame | no |
| MetalQuake | not run (it stops at its launcher and ignores the command line) | unknown |

So the plan as written serves one of the four games run. SDL 2 picks the HiDPI mode of the asked size, and Unity does not change
modes at all. The route that covers the other three is the one listed under "Not covered": upscaling a drawable that is smaller
than its layer, in the Metal bundle at present time, before Core Animation's own scaling. That changes the plan's step 1 (the same
EASU/RCAS kernels, run from the bundle on the drawable) and replaces steps 2 and 3. Decision pending.

## Route chosen: the Metal bundle (2026-11-24, Sunneva)

Built (`tools/metal/native/RDNA4MTLDevice.mm`, "Super Resolution" at its end; off unless `RDNA4_FSR=1` or RDNA4Gadget's
`SuperResolution` preference is on, `defaults write com.rx4darwin.RDNA4Gadget SuperResolution -bool true`; sharpness `Sharpness` or
`RDNA4_FSR_SHARPNESS`, thousandths of a stop, default 200):

- CAMetalLayer's `setDrawableSize:`, `drawableSize` and `nextDrawable` are replaced once a process (not WindowServer). A layer of our
  device (or none yet: SDL sets the size first) asked for a drawable smaller than the pixels it covers on the screen (its bounds at its
  window's backing scale) in both directions, in an 8-bit or 10-bit format, keeps its own size; the app is told the size it asked for.
- `nextDrawable` gives the app an `RDNA4UpscaledDrawable`: its texture a private one of the asked size (one per real drawable texture).
- Presenting it (a command buffer's `presentDrawable:`, or its own `present`) records a step after the app's draws: EASU into an
  intermediate VRAM image of the layer's size, RCAS into the real drawable's texture (`src/shaders/fsr1.cl`, `src/fsr1_codeobj.h`,
  `rdna4_dispatch`), then the real drawable is presented.

Card-tested (2026-11-24, rig): a test window (1280x720 points at 2x, drawables of 640x360) shows FSR's output, 2560x1440, sharp edges
where Core Animation's own scaling is blocky; no errors.

**Finding: the games tested do not present small drawables.** Quake II's Metal port (SDL 2, HiDPI) asks for 3200x1800 on a 1600x900-point
window; Daggerfall Unity at 1920x1080 asks for the full 5120x2880 and scales its frame up itself (step 0's "macOS scales the frame" was
wrong). So nothing of theirs reaches the upscaler. It applies to apps that hand Core Animation a smaller frame: SDL games without HiDPI
support, apps with a render scale that shrinks the drawable. **Open decision:** a "render scale" (Quality 77 %, Balanced 67 %,
Performance 50 %) that makes apps asking for a full-size drawable draw a smaller one, upscaled by FSR: the real frame-rate gain, but an app
that sizes its viewport from the layer's bounds rather than its drawable would draw clipped; to be tried per game.

## Render scale and the Tweaks pane (2026-11-24)

Render scale (`RenderScale` percent, `RDNA4_FSR_SCALE`; FSR 1's Ultra Quality 77, Quality 67, Balanced 59, Performance 50): an app asking
for a full-size drawable gets one that much smaller, upscaled back. Card-tested at 67 %: Quake II draws 2144x1206 shown at 3200x1800,
Daggerfall Unity 3430x1930 shown at 5120x2880, both whole (no clipping). RDNA4Gadget's **Tweaks** tab sets the switch, the render scale and
the sharpness (0-100 %, 100 % = 0 stops); they apply to games started after a change.

Not done yet: frame-rate numbers at each scale; the cost of the upscale at 4K; display sleep and a window moved between displays of
different scales.

## Open questions and limits

- The monitor stays at its native refresh rate. A game asking 1080p at 120 Hz on a 4K 60 Hz monitor gets 60 Hz, as with RSR.
- The pointer image is not scaled, as in "looks like" modes: at 1080p on a 4K head it looks half its size. Most games hide it.
- The cost is one extra pass per presented frame at the output size, to be measured (step 6).
- Effort: steps 1 to 3 need several kext builds and reboots of the test machine.
