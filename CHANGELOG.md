# Changelog

What each version adds, and what of it has run on the card (RX 9070 XT, macOS Big Sur 11.6.6 unless a line says
otherwise). The newest version is on top. How versions and branches work is in `CONTRIBUTING.md`.

## After 0.1.0 (on `next`, no number yet)

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

**Tools**
- `tools/rig/spike-bar.sh` and `tools/metal/air/sample-operands.py`, the two spikes of the roadmap (issue #3).

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
