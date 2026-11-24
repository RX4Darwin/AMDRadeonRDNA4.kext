# Driver-level FSR 1 upscaling ("Super Resolution"), plan

Status, 2026-11-24: step 0 done (below: the display-mode route covers few games); step 1 done (the kernels, host-tested, compiled
for gfx1201, not yet run on the card). AMD's source is the `lib/FidelityFX-FSR` submodule (FSR v1.0.2).

## Why FSR 1 and not FSR 4

FSR 4 ships only as AMD's signed Windows DLLs (`amd_fidelityfx_upscaler_dx12.dll`, `..._vk.dll`) under a licence that allows
redistribution in binary form and forbids reverse engineering. It is temporal: the game must hand it motion vectors, depth and its
camera jitter, so a driver cannot add it to a game that does not ship it. FSR 1 (EASU upscaling plus RCAS sharpening) is spatial: it
needs only the finished frame, which is what the driver has. Its source, `Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h`
in the FidelityFX SDK, is MIT licensed. Radeon Super Resolution in AMD's Windows driver is the same idea.

## What it does

With **Super Resolution** on in RDNA4 Gadget's new **Tweaks** pane, a display mode smaller than the monitor's largest one no longer
changes the monitor's timing. The head stays on its native mode, macOS draws frames of the smaller size, and each presented frame is
upscaled on the card with EASU and sharpened with RCAS into the scanout. The picture keeps its aspect ratio, centred, with black bars
where the aspects differ. Off (the default), modes work as today: the monitor scales.

Covered: anything that switches the display to a lower mode, which is what games' exclusive full screen does (SDL 2's
`SDL_WINDOW_FULLSCREEN` on macOS sets the mode). Also the desktop, if someone picks a low non-HiDPI mode.

Not covered: HiDPI modes, including "looks like" (their frames are already at or above the head's size). Games that keep the display mode
and render a smaller drawable in a full-screen window; CoreAnimation scales those inside WindowServer, out of our reach. Those could be
done later in the Metal bundle (EASU on the drawable at present), a separate project.

## Where it goes (what exists)

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

## Steps

0. **Gate: which games change the mode (no code).** On the rig, start vkQuake, OpenMW, Daggerfall Unity, Quake II and MetalQuake in full
   screen at 1920x1080 on the Samsung (4K) and see whether the kext logs a mode switch (`ndrv` switch lines, `cgmode` list). If none of
   them does, stop here and rethink (the bundle route above).
1. **Done: `src/shaders/fsr1.cl`** (`test/fsr1-test.c` in `make test`; both kernels inline everything: no stack, EASU 64 VGPRs): EASU and RCAS ported by hand from the float path of `ffx_fsr1.h` (`ffxFsrEasuFloat`, `fsrEasuTapFloat`,
   `fsrEasuSetFloat`, the RCAS float path) to OpenCL C. Buffers in, not textures: EASU's four gathers become twelve loads. 8-bit and
   10-bit (`'l10r'`) pixels as `scale.cl` reads them. The constants (`ffxFsrPopulateEasuConstants`, `FsrRcasCon`) are computed in the
   kernel from integer sizes and an integer sharpness, so the kext does no floating point. AMD's MIT notice at the top of the file and in
   `docs/licenses.md`. Host check: the same file compiled as C on the Mac (a few macros), with assert checks that a flat image stays
   flat, 1:1 is close to identity, and an edge stays sharp.
2. **Present path**: a frame smaller than the head both ways with the kernel loaded: EASU into a per-port buffer of the output size
   (VRAM from the runtime, made at first use, freed when the port goes back to direct), a compute barrier, RCAS from it into the
   scanout buffer. The bars cleared in the same pass. The debug grab (`debug.rdna4_pvframe`) takes the output, as for scaled frames.
3. **Mode switch**: with upscaling on, a real mode smaller than the head's largest switches the head to the largest mode of the same
   refresh, or the one nearest it, and marks the frame as upscaled. `scaledFrame` returns the frame for those too, so cursor position and
   attribute 0x10 follow. No host memory is needed: the frame fits in the head's VRAM slot, unlike "looks like". Without the kernel the
   switch falls back to a real mode set.
4. **Control**: a property any user may set (no password), on the runtime's service: `AMDRadeonRDNA4,Upscale` = `{Enabled, Sharpness}`,
   checked in `setProperties`. Sharpness is read for each frame. Enabled applies at the next mode switch. The app keeps the choice
   and sets it again at launch (it opens at login), so it lasts across boots without NVRAM.
5. **Tweaks pane** in RDNA4 Gadget: a "Super Resolution (FSR 1)" switch and a sharpness slider (RCAS stops, 0 = sharpest; AMD's
   samples default to 0.2). One line: "For games that switch to a lower resolution; applies at the next resolution change."
6. **Card tests**: `cgmode` 1920x1080 and 1280x720 on the Samsung, with grabs compared on and off; the cost per frame from the
   present timing; vkQuake at 1080p full screen by eye (picture, pointer position, menus); a 5:4 mode (bars); display sleep and wake
   while upscaled; back to native goes direct again; the Lenovo (1080p) at 1280x720.

## Open questions and limits

- The monitor stays at its native refresh rate. A game asking 1080p at 120 Hz on a 4K 60 Hz monitor gets 60 Hz, as with RSR.
- The pointer image is not scaled, as in "looks like" modes: at 1080p on a 4K head it looks half its size. Most games hide it.
- The cost is one extra pass per presented frame at the output size, to be measured (step 6).
- Effort: steps 1 to 3 need several kext builds and reboots of the test machine.
