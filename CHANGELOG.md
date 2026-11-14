# Changelog

What each version adds, and what of it has run on the card (RX 9070 XT, macOS Big Sur 11.6.6 unless a line says
otherwise). The newest version is on top. How versions and branches work is in `CONTRIBUTING.md`.

## After 0.2.2 (on `next`, no number yet)

Nothing yet.

## 0.2.2 (2026-11-14)

**OpenGL (`make PVGPU=1`)**
- On the card by default (`rdna4-pvgl=0` turns it off): the renderer is named on the accelerator only when its file
  on disk has the bytes the kext patches (Tahoe 26.6.2's); another renderer keeps OpenGL in software instead of
  aborting every GL app. Card (Tahoe 26.6.2, 2026-11-14, no `rdna4-pvgl` in the boot-args): the renderer named and
  patched, kitty draws on the card.

**Displays (`make PVGPU=1`, `rdna4-pvgpu-disp=7`)**
- A DisplayPort boot display has its modes as a paravirtual display: macOS was offered only 3840x2160 for it (the
  heads' mode lists only knew heads, and only an HDMI boot display behind an adapter becomes one). Port 0 now offers the
  boot framebuffer's own modes and switches through its mode-set engine (the stream retimed, the link as trained).
  A monitor's mode that is half of a scaled size above it is kept: 2560x1440 had gone beside 5120x2880, and macOS's
  1280x720 HiDPI with it. Card (Tahoe 26.6.2, 2026-11-14, Samsung 4K on DP): 2560x1440 and 1280x720 HiDPI listed,
  switched to 2560x1440 (measured 59.955 Hz) and back to 3840x2160.
- At most 15 modes are offered: macOS reads 4 blocks of the EDID the guest makes from them (5 modes a block) and lists
  only those; 17 were sent on the card and the last two (720x480, 640x480) never listed.
- The first WindowServer of a boot no longer dies: it opened 2 of the 8 framebuffers (ours refuse) and CoreDisplay
  asserted on that, every boot, so the desktop came about 10 s later from its restart. Our framebuffers are left out of
  user-space matching while the heads are the paravirtual displays, and WindowServer's first open of the GPU wrangler
  waits (about 2.5 s) until the paravirtual GPU and its accelerator are published, so that CoreDisplay opens their
  framebuffers with their display pipes. Card (Tahoe 26.6.2, 2026-11-14, two boots): the first WindowServer lives, both
  displays accelerated, no crash report.

**Metal (`make PVGPU=1`)**
- A mip level whose rows are not 128 bytes apart is sampled from its copy on the card (the one the pass that drew it
  left) instead of the level before: on most sizes WindowServer's glass blurs read level 0, and with Liquid Glass
  tinted the sidebar's colour flickered as a window was dragged. Card (Tahoe 26.6.2, 2026-11-14): mipprobe at 200x200
  and 100x100 matches 256x256, Finder dragged with the tint on does not flicker.
- The ranges dispatches in the draw batch write are kept for every batch that may be in flight (512), and those of
  finished batches dropped: 32, emptied only by waiting for all draws, made kitty's scrolling wait for the card 3158
  times in 5 s (each of GL's draws writes a new index buffer). Card (Tahoe 26.6.2, 2026-11-14, `seq 1 3000000` in
  kitty): those waits gone, time waiting for the card 635 -> 218 ms in 5 s, 5.1 us of the host's a draw as before.
- A render pass's multisample resolve is carried out: at the end of its encoder the multisampled texture (its samples
  averaged, next entry) is copied into the resolve texture (store action resolve, or store and resolve; level 0). The Sequoia and
  Macintosh wallpapers draw into a multisampled texture and resolve into their drawable, which stayed black. The
  translator (air2amdgpu) also takes `air.[fast_]sincos` (Sequoia's vertex shader). Card (Tahoe 26.6.2, 2026-11-14):
  both wallpapers show.
- The translator takes `air.simdgroup.barrier` (a SIMD group is one wave: a fence at the wave's scope); CoreImage's
  4:2:0 kernels were not translated without it. Card (2026-11-14): a kernel reading its neighbour's threadgroup memory
  after the barrier, 4096 of 4096 right.
- sRGB colour targets (BGRA8 and RGBA8 sRGB) are written as sRGB: the hardware encodes the shader's linear colour, as
  Metal does; unencoded, the Sequoia wallpaper came out dark and dull. Card (2026-11-14): its colours as Apple's.
- Multisampled colour attachments (2x and 4x, 4 or 8 bytes a pixel) are drawn with all their samples: into a 64KB_2D
  surface of the samples on the card (cleared there, with depth and stencil of as many samples), which the host's own
  kernel (`rdna4_resolve`, translated like a guest's; `rdna4_resolve_srgb` averages an sRGB target's in linear light)
  averages into the texture at the end of the pass, before the resolve copy. The translator reads `texture2d_ms`
  (`read`, `get_num_samples`, `get_width`/`get_height`). Clears of sRGB targets are encoded. Drawn as one sample, the
  Sequoia wallpaper's edges were rough; with the samples' spread left at 0 (`PA_SC_SAMPLE_PROPERTIES`) the rasterizer
  missed edge samples and lines of dots crossed it. Card (Tahoe 26.6.2, 2026-11-14): Sequoia's edges smooth, on the 4K
  DisplayPort display and the 1080p HDMI one.

## 0.2.1 (2026-11-14)

**OpenGL (`make PVGPU=1`, `rdna4-pvgl=1`)**
- macOS's OpenGL 4.1 on Metal (`AppleMetalOpenGLRenderer.bundle`) runs on the paravirtual GPU, so unmodified OpenGL
  programs draw on the card. `rdna4-pvgl=1` names the renderer on the accelerator (`IOGLBundleName`) and patches it
  where its pages come in (`cs_validate_page`): its GPU vendor test and the default raster sample count it sets on
  every pass, which the paravirtual device does not have. Tahoe 26.6.2's renderer only; off by default. Card
  (2026-11-12 to 2026-11-14): kitty 0.50.0 (GL 3.3 core) shows its text at once and scrolls smoothly, and Mixxx
  2.5.6 (GL 2.1) shows its waveforms.
- What GL needed of the host: `setViewports`/`setScissorRects` and viewports of negative height, the depth test with
  Depth32Float attachments, draws into a plain view's texture, RGBA16Unorm targets and textures, 2D array textures
  (and in the translator their samples and reads, and constant tables), every chunk of a split command buffer (only
  the last one ran), and a pipeline's function made from a metallib Apple's host reads at an address (looked for
  again once its pages are mapped: kitty's first window was dark).

**Metal (`make PVGPU=1`)**
- Faster with many small draws (kitty scrolling: the host's loop busy 0.75 s in 5 s at 78000 draws): compute
  dispatches go into the draw batch on the graphics ring, their buffers mapped in place, and buffers up to 512 KiB
  mapped by their 2 MiB window. Vertical blanks due inside a long command buffer are raised there (they were up to
  11 ms late).
- Mip levels: a render pass draws into the level its attachment names (each level's own memory, size and rows), clears
  it there, and blits copy any level; a shader that samples at an explicit LOD reads the two levels around it, each
  a level-0 image of the guest's memory, and blends them (`RDNA4_MTLC_LEVEL_FIRST`: the guest's levels are largest
  first, the hardware's linear ones smallest first, so one T# cannot hold them). Tahoe's glass blurs its backdrop
  that way; Finder's sidebar was black, now it is glass. Level 0 of a mipmapped RGBA8 texture was drawn as BGRA8
  (its format is past where it was looked for). A level whose rows are not 128 bytes apart is read as the level
  before it. Card (Tahoe 26.6.2, 2026-11-14): Finder's sidebar; `mipprobe` (four levels drawn and sampled at LODs
  0 to 3, shared and private) gives what an Apple GPU gives.
- RG32Uint and RG32Sint colour targets, as WindowServer's `brim_init_lpf` and `brim_jump_lpf` use: the translator
  exports a shader's integers as they are, the host draws them with an integer format, and clears them to a whole
  value. Card (2026-11-14): `intprobe`, shared, managed and private, drawn and cleared. WindowServer's own brim
  draws were not seen on the card again after this; a refused colour target now logs its format, size, rows and
  storage the first time each kind is refused.

## 0.2.0 (2026-11-12)

**Cards**
- The RX 9070 (device 0x7550, PCI revision 0xC3) is told apart from the RX 9070 XT (0xC0): System Report and
  ioreg name it "AMD Radeon RX 9070", and the fake paravirtual GPU reports its 56 compute units instead of 64
  (`src/sku.hpp`). The display paths of a Sapphire RX 9070 (1DA2:E489, VBIOS `2E489 Navi48 XT`) were read from its
  ROM and are the XT board's four (DP on HPD1/2, HDMI on HPD3/4), so the second-display tables apply unchanged; it
  uses the same firmware as the XT (the kicker variants are for revision 0xC8). Runs on a Sapphire RX 9070 under
  macOS Sequoia 15.8.2.

**Displays**
- A monitor on a DisplayPort connector through a passive DP++ (DP-to-HDMI) adapter: the boot display's EDID is
  read over DDC with the connector's pads switched to I2C mode (`DC_GPIO_DDCx_MASK.AUX_PADx_MODE`,
  `DC_GPIO_AUX_CTRL_5.DDC_PADx_I2CMODE`, `PHY_AUX_CNTL.AUXx_PAD_RXSEL`, as amdgpu's `hw_ddc.c` does), so it gets
  its name and mode list. Chosen when the lit pipe carries TMDS on a DP connector; another DP connector whose
  sink does not answer on AUX gets the same second try. Run on the card 2026-11-03 (Sapphire RX 9070, Dell
  P2723QE on both DP connectors through DP-to-HDMI cables, macOS 15.8.2): both EDIDs read, name and mode list.
- A second display behind such an adapter: the DP connectors have an HDMI table each as well
  (`pipe2_linux_dp{1,2}_tmds.inc`, pipegen's new `dpdual` scenario: a DisplayPort connector with an HDMI stream
  and sink, as amdgpu has a link with a DP-HDMI dongle). `Pipe2::use(hpd, tmds)` picks the one for the sink the
  connector has, and a hot-plug reads the EDID on AUX or DDC, whichever answers (a DP connector's sink that has
  not been read yet is offered with its HDMI table unless `rdna4-head2dp=1`). Not yet run on the card.
- What a passive DP++ adapter carries is read from its ID block (I2C 0x40, as amdgpu does): a type 2 adapter's
  maximum TMDS clock, otherwise 165 MHz. It caps the modes offered on that connector, for the boot display (never
  below its boot mode) and the second one; the boot log says `dp++: HPDn: ...`. The test card's cables are type 2
  (CS5216) at 300 MHz, which carries 3840x2160@30 but not 4K60 in RGB. Not yet run on the card.

- Boots where the kext never took over the display (only the firmware's picture, on the DisplayPort display) are
  explained: another kext (SimpleHDA, which restores AppleHDA on Tahoe) can load `IONDRVSupport` before Lilu notices
  kext loads, so the kext's hook never ran. From 30 s on the kext now checks whether it did, and if not routes
  `IONDRVSupport` itself and registers the GPU again. On two such boots (2026-10-22) this brought the boot display
  under the kext at 31-34 s. Not yet: the second display stays dark on these boots (it is made where the framebuffer
  starts, which happened before), and compute stopped at its PSP step on both (cause not known).

**Compute runtime**
- Address spaces lent per job (`rdna4-vmshared=2`), which lifts the 1 GiB limit of a client's address space, ran
  on the card for the first time on 2026-10-12: the diagnostic script's table all PASS, and the full benchmark in a
  client address space passes (SGEMM n=4096 at 12.1 TFLOPS, n=8192 at 12.8; the matrix units at 53 TFLOPS in FP16;
  host to GPU 4.1 GB/s). A client beyond 1 GiB works: `rdna4-run bigva 2048` fills 2 GiB of its address space and
  computes correctly on buffers past that. The default stays `rdna4-vmshared=1`, and the Vulkan interface does not
  open in mode 2.
- The copy engine's compression setting (`SDMA0_DCC_CNTL`, 0x0000aabe on this card) does not alter uniform blocks in
  any direction this kext copies in; the boot log says so at every start (`runtime: dma: SDMA0_DCC_CNTL ...`).

**Vulkan**
- Swapchains: the driver offers `VK_KHR_surface`, `VK_EXT_headless_surface` and `VK_KHR_swapchain`, and shows a
  headless swapchain on the boot display (Mesa patch 0008). Verified on the card 2026-10-13: `swapprobe`, 300
  frames in 5 s.
- A Vulkan application that is not ours: `make vkcube` builds Khronos's loader and `vkcube` beside the driver.
  Verified on the card 2026-10-13: the cube on the boot display for ten seconds, through the loader. Limits: the
  program has to be given the display's size, three images at most, B8G8R8A8 only, and every present waits for the
  GPU and copies the picture.

**Metal** (macOS Tahoe and Sequoia; built in only with `make PVGPU=1`, experimental; README *Metal*, `docs/m0-pvgpu.md`,
`docs/m1-stream.md` section 5b). Most of it by Jan Claußen (PR 6), reviewed and card-tested on Tahoe 26.6.2.
- A paravirtual GPU on the card: Apple's own `AppleParavirtGPU` driver runs on it, Metal finds an "Apple Paravirtual
  device", and the kext carries out its command stream on the RX 9070 XT.
- The displays: with `rdna4-pvgpu-disp=7` the heads are the paravirtual GPU's displays, their frames scanned out by the
  card's display engine, and WindowServer composites with Metal on the card. `rdna4-iop` (a LaunchDaemon) switches
  CoreDisplay's IOPresentment to what the boot needs. Verified on the card on Sequoia (2026-11-05, Jan Claußen) and on
  Tahoe 26.6.2 (2026-11-10): the login window, the desktop and apps' windows.
- Shaders are translated from Apple's AIR to gfx1201 at run time by a root helper (`tools/metal/m3/air2amdgpu.cpp`,
  `make metal-helper`). Compute: buffers, constant data, math, threadgroup memory and threadgroup buffers, barriers,
  atomics, SIMD-group functions, conversions (to bool as Metal does). Rendering: vertex and fragment functions, vertex
  descriptors, up to 16 values between the stages, `front_facing`, discard, several colour attachments.
- Drawing: every draw command form (64-bit counts and offsets, instances, base vertex and base instance), primitive
  restart for indexed strips, blending per attachment, the stencil test, viewports and scissors, clears to any colour
  in more formats; textures, samplers, texture views (also of a later mip level), textures over buffers; IOSurface
  targets (BGRA8, BGRA8 sRGB, RGBA16Float).
- Tahoe 26.6.2: the Dock with every icon (RenderBox's drawing; Metal's failures are ignored there because a binary
  archive asserts on the paravirtual device, `METAL_ERROR_MODE` set by `rdna4-iop`), Liquid Glass's brightness kernels,
  Finder's glass (2026-11-12). The device is published as soon as the card is up after `rdna4-pvgpu-delay` (about 15 s
  after boot with `rdna4-pvgpu-delay=5`; it waits up to 2 minutes for the card).
- Speed: targets, textures and buffers are mapped in place and kept between draws, textures drawn into are held on the
  card, copies run on the card, draws go to the card in batches.
- Checked by `tools/metal/m1/mtlprobe.m` (`mtlprobe all`: blits, compute, draws) on every run.
- Limits: it is Apple's paravirtual Metal: no Metal heaps, argument buffers or indirect command buffers, no texture
  swizzles (the Intel build of the guest driver drops them); OpenGL stays in software. The card ignores separate alpha
  blending here, so such draws run twice. Viewports in whole pixels. The IOSurface lookup depends on Tahoe 25G83's
  IOSurface internals. No device is published when the compute runtime does not come up (a build without
  `firmware/amdgpu/` never does).

**macOS Tahoe** (the rig runs 26.6.2, 25G83, since 2026-10-13)
- Restart and Shut Down from the Apple menu froze the whole machine whenever the paravirtual GPU's code was in the
  kext, even with `rdna4-off=1`. They were kernel panics the screen never showed: the kext linker had filled the
  device class's inherited vtable slots by redirecting the base-class symbols kext-wide, so every IOService of the
  kext ran `IOPCIDevice` methods (`IORegistryEntry::setProperty`, `getProperty`, ...). The device class now overrides
  them (found 2026-10-15, in `e27811a`), and `tools/metal/m0/check-vtable.py` finds such slots. The paravirtual GPU stays built
  in only with `make PVGPU=1`.

**Tools**
- `tools/rig/spike-bar.sh` and `tools/metal/air/sample-operands.py`, the two spikes of the roadmap (issue #3).
- `tools/rig/boot-history.sh` (how the last boots ended, from the log on disk) and `tools/rig/freeze-watch.sh` (a
  launch daemon that keeps each boot's kernel log on disk while it runs, so that a freeze leaves its last lines);
  both are in `bin/`. `rdna4-steptrace=<ms>`: a log line and a pause after each step of a compute client's open,
  info call and close and of the shutdown quiesce, for finding where a freeze happens.

**Layout and build**
- The repository is sorted: the submodules in `lib/`, the GPU test kernels' sources in `src/shaders/`, the programs
  that talk to the kext in `userspace/rdna4-run/` and `userspace/vulkan/`, the host tests in `test/`, and the
  development scripts by purpose in `tools/{build,gen,rig,vm,linux,metal}/` (`tools/README.md`). Paths written in
  older documents and logs were updated; a path in an old kernel log or test record names the old place.
- Everything that is built goes to `Build/`. The kext and the programs for the test machine land in
  `Build/AMDRadeonRDNA4-<version>-<CONFIG>/` (`AMDRadeonRDNA4.kext`, `bin/`, with `diagnostic-log.sh` in it), the VM
  variant under `Build/vmtest/`. `CONFIG` is `RELEASE` (the default: what ran on the card, same code as before the
  move) or `DEBUG` (`-O0`, `DEBUG` defined; not run on the card). `make product-dir` prints the directory.
- The `Makefile` works with GNU make (3.81 and later) and BSD make. `make clean` no longer removes the Mesa tree of
  `make mesa`; `make distclean` does. A firmware fetched after a build is now noticed (it was not before: the kext
  stayed without it until the objects were deleted).
- `AMDRadeonRDNA4.xcodeproj`: targets that run `make`, for building and browsing in Xcode.
- `make test` also runs three host tests that had to be started by hand (`test/test-gfx12tri.c`,
  `test-gfx12tricol.c`, `test-w12k-pm4.cpp`).
- The card's ROM dump is no longer in the repository (`firmware/README.md` says where it is to be had). Without it
  `make test` leaves out the AtomBIOS and IP discovery checks, says so, and runs the rest.

## 0.1.0 (2026-10-12)

**The name.** The driver is AMDRadeonRDNA4 (before: RDNA4FB). The bundle is `AMDRadeonRDNA4.kext`, its identifier
`com.hackintosh.AMDRadeonRDNA4`, its kernel-log lines begin `AMDRadeonRDNA4:`, its registry properties
`AMDRadeonRDNA4,`, and the diagnostic script writes `amdradeonrdna4-diag-<date>-<time>.txt`. The OpenCore entry has to
name the new bundle and executable. Boot-args (`rdna4-...`) are unchanged. Logs written by older kexts keep the old
names. The renamed build loaded on the card on 2026-10-12 (commit `5bdc020b`): both displays, the diagnostic
script's table all PASS.

**Builds say what they are.** The kernel log's first line and the registry property `AMDRadeonRDNA4,Build` give the
version and the commit, for example `0.1.0 (a634d7d9-dirty)`; the diagnostic script prints it.

**Displays** (verified on the card 2026-10-04 to 2026-10-06)
- A second display on the second pipe, on either HDMI connector, with its own modes; mode switching on both.
- Display sleep and wake; an HDMI boot display that really sleeps.
- Hot-plug of the second display, also through a long sleep and from one HDMI port to the other.
- DisplayPort link training by the kext (4 lanes HBR2).
- The top of the picture no longer cut off after a switch to a smaller mode.
- `rdna4-head2=4`, `rdna4-hotplug=2` and `rdna4-modeset=1` are the defaults.
- Not run on the card: HDMI 2.0 modes above 340 MHz, a second display on DisplayPort.

**Compute runtime** (verified on the card 2026-10-07)
- With `rdna4-vm=1` every client has its own GPU address space; the whole self-test passes, the graphics tests of
  `rdna4-run` included, and the GPU idles at 3 %.
- A job whose shader faults is ended and reported within milliseconds, and its queue runs the next job.

**Vulkan** (verified on the card 2026-10-06 and 2026-10-07)
- A kernel interface for the RADV driver (`userspace/vulkan/`): buffers, a private address space, submit and wait, and pictures
  on the boot display. `vkprobe` runs fills, a compute shader, a rendered triangle and 60 frames a second.
- It works in the same boot as the compute runtime's clients, and at the same time.
- A program whose compute shader hangs loses its own device after ten seconds; the graphics ring stays in service.
- Not solved: a draw that hangs still costs the ring until the reboot. No window-system layer; no Metal.

## 0.0.1 (`main`, 2026-08-21)

The display driver as a Lilu plugin under the name RDNA4FB: the boot display with its EDID modes through Apple's
generic framebuffer driver. The README of that commit says what had been verified.
