# v0.4.0 real-card validation plan

Status: test plan only. A successful CI build is not evidence that the RX 9070 XT boots or renders correctly on a particular Hackintosh. Run these checks only after the machine has a known-good EFI recovery path and v0.4.0 has booted reliably. Do not use a machine that is needed for school or daily work as the first test target.

## 1. Record the test environment

Before changing anything, record:

- Exact GPU model and PCI revision (the primary development card is an RX 9070 XT, Navi 48).
- macOS version and build, OpenCore version, Lilu version, motherboard and BIOS.
- Display connections, resolution and refresh rate.
- The exact kext build and matching user-space package version.
- Complete OpenCore boot arguments and whether WhateverGreen or another graphics kext is enabled.

Keep a copy of the working EFI outside the machine. Have a way to select that EFI without relying on the new driver. Do not combine a driver update with unrelated BIOS, SMBIOS, OpenCore or macOS changes.

## 2. Confirm the matching components

The kext is injected by OpenCore; the user-space package installs the Metal bundle, helper daemons and optional RADV Vulkan components. They must come from the same build.

Capture:

```sh
rdna4-run info
```

Check that the kext is loaded, the reported build and compute ABI match the installed bundle, and the GPU is identified as the expected Navi 48 device. Save the output with the test log. Stop if the versions disagree or the runtime cannot open the kext service.

## 3. Metal functional tests

Use the `mtlprobe` built by the CI build job. Start with:

```sh
mtlprobe info
mtlprobe all
```

Keep the full output. A pass requires every probe to report its expected result and every child process to exit successfully. Do not treat a window appearing as proof that its rendered pixels are correct.

Pay particular attention to runtime shader translation and newer feature coverage:

- `mix`: runtime translation of a kernel not already in the kext's table.
- `nativesrc`: Metal source compilation and execution on the native device.
- `coverage2`, `ctex`, `fbfetch` and `varyings`: Core Animation-style shader features, texture operations, framebuffer fetch and interpolated varyings.
- `nativespin` and `nativestencil`: depth, textures, indexed/instanced drawing, stencil and multisampling.

Use the exact probe names supported by the checked-out `mtlprobe); do not copy binaries from a different commit.

## 4. OpenGL application checks

The project uses Apple's OpenGL-on-Metal renderer rather than a separate RDNA4 OpenGL bundle. Test a small known OpenGL application first, then a second application with a different rendering workload.

Record the app/version, whether it selected the accelerated renderer, visual correctness, window resizing and whether the desktop remains responsive. A successful launch alone is not a pass. The README lists kitty, Mixxx and Sublime Text as known OpenGL clients; choose applications already available on the test machine.

## 5. Vulkan checks: keep the two paths separate

There are two distinct Vulkan routes. Record which one each test uses.

- **RADV:** Mesa's `libvulkan_radeon.dylib` and ICD manifest from the package. Start with the project's `vkprobe` and `swapprobe`, then test a Vulkan application configured for RADV.
- **MoltenVK:** Vulkan translated to Metal. This exercises the native Metal driver, not RADV. Use a known Vulkan application configured for MoltenVK as a separate comparison.

For each path, record device name, resolution, frame rate, visual output, stability and the application/driver versions. Do not compare frame rates unless the resolution, scene, settings and run duration are identical.

## 6. Game checks

Use a short, repeatable scene before attempting long sessions:

- **MetalQuake:** the project README records a successful native Metal run of the demo and e1m1 at 3840x1878, HDR, 60 FPS on Tahoe 26.6.2. Treat that as a reference result, not a promise for another machine.
- **vkQuake:** test RADV and MoltenVK separately. The Vulkan README documents both routes and prior measurements, but performance varies with resolution and build.

Record the first launch, a repeat launch, a short gameplay session, a window resize and a return to the desktop. Stop after any display corruption, GPU timeout, system freeze or unexpected reboot; preserve logs rather than repeatedly retrying.

## 7. Evidence and acceptance

For every test session, keep:

- Full `mtlprobe` output and `rdna4-run info`.
- The driver's boot log from `/var/log/rdna4` and relevant helper logs.
- macOS version/build, GPU revision, boot arguments and component versions.
- Application names/versions, settings, resolution, refresh rate and measured frame rate.
- Screenshots or short recordings of incorrect rendering, with the corresponding log timestamp.

A feature is **validated on this machine** only when its functional checks pass on the real card and the result is repeatable after a reboot. CI success, emulator success, Linux results and another contributor's card results are useful evidence, but they are not substitutes for that test.

## 8. Current known limitations

Consult the repository README and changelog before testing. In particular, DSC, variable refresh, DisplayPort MST, HDMI/DP audio, hardware video encode/decode and fan control are not implemented. Navi 44 support remains untested under macOS. Tahoe releases other than the specifically tested build also need validation.

This plan does not authorize changing SIP/AMFI settings, replacing the daily EFI, or installing a package. Those are separate decisions and should be reviewed with an experienced adult before changing system security settings.
