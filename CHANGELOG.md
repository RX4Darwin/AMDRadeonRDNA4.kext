# Changelog

What each version adds, and what of it has run on the card (RX 9070 XT, macOS Big Sur 11.6.6 unless a line says
otherwise). The newest version is on top. How versions and branches work is in `CONTRIBUTING.md`.

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
- A kernel interface for the RADV driver (`vulkan/`): buffers, a private address space, submit and wait, and pictures
  on the boot display. `vkprobe` runs fills, a compute shader, a rendered triangle and 60 frames a second.
- It works in the same boot as the compute runtime's clients, and at the same time.
- A program whose compute shader hangs loses its own device after ten seconds; the graphics ring stays in service.
- Not solved: a draw that hangs still costs the ring until the reboot. No window-system layer; no Metal.

## 0.0.1 (`main`, 2026-08-21)

The display driver as a Lilu plugin under the name RDNA4FB: the boot display with its EDID modes through Apple's
generic framebuffer driver. The README of that commit says what had been verified.
