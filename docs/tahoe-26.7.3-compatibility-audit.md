# macOS Tahoe 26.7.3 compatibility audit

**Target machine:** AMD Radeon RX 9070 XT (Navi 48), x86_64 Hackintosh  
**Reported OS:** macOS Tahoe 26.7.3, build 25G342  
**Bootloader:** OpenCore  
**Project baseline:** upstream `main`, AMDRadeonRDNA4 0.3.1  
**Audit status:** preliminary; no 26.7.3 hardware test has been performed

## Findings

1. The fork's `main` branch is an exact fork snapshot of upstream `main` at commit
   `a1f7661f188851e0dfe578538ad29ef4f5a7a943` (release 0.3.1).
2. The upstream README reports the Lilu plugin's end-to-end injection and display-request
   path tested in a Tahoe 26 virtual machine. It separately reports physical RX 9070 XT
   display-path tests on Big Sur 11.6.6/11.7.10.
3. The README and changelog specifically report a physical-card `PVGPU=1` Metal test
   on Tahoe 26.6.2 on 2026-11-18. They do **not** identify build 25G342 / Tahoe 26.7.3
   as tested.
4. Release 0.3.1 includes substantial changes after 0.3.0, especially the experimental
   paravirtual Metal/Vulkan path. A version upgrade alone is not evidence of compatibility.
5. The user's earlier boot test is a useful A/B observation: macOS booted with the
   plugin disabled and rebooted during startup when the experimental 0.3.0 plugin was
   enabled. The captured logs did not establish a definitive panic or failing function.

## Compatibility conclusion

There is currently no evidence-backed source patch that can be described as a
26.7.3 fix. The correct next step is a controlled compatibility test of the unmodified
0.3.1 source on the exact OS build, with the plugin disabled by default and a known-good
EFI backup retained.

Do not change the OpenCore configuration, disable other graphics kexts, or install a
new binary as part of this source audit. In particular, do not infer that a successful
VM test proves physical GPU compatibility.

## Required test sequence

1. Record the exact OpenCore version, Lilu version, WhateverGreen version, and the
   currently installed kext's build string.
2. Build the upstream 0.3.1 baseline without `PVGPU=1` first. Preserve the existing
   working EFI and keep the new kext entry disabled.
3. Run the repository's host-side tests (`make test`) and retain the complete output.
   Build the kext separately and inspect its architecture, bundle metadata, and linked
   symbols before any boot test.
4. If host tests and static checks pass, prepare a separate test EFI copy and a
   reversible boot entry. Do not overwrite the known-good EFI.
5. Perform one controlled boot test with verbose logging and the project's documented
   kill switch available. Capture the complete panic/reboot evidence if it fails.
6. Only patch source when the failure is tied to a specific incompatibility in the
   26.7.3 kernel environment. Add a regression test where the affected code can be
   exercised on the host.
7. Record physical-card results separately from VM results. Do not label the build
   26.7.3-compatible until it boots and the intended display path is verified on the
   RX 9070 XT.

## Known limitations

- The GitHub source inspection does not provide access to the user's local macOS SDK,
  kernel symbols, OpenCore EFI, or RX 9070 XT for compilation and hardware testing.
- The upstream Makefile targets the macOS 11 ABI for cross-compilation. This does not
  mean it is automatically compatible with every later kernel release.
- The experimental paravirtual GPU build (`PVGPU=1`) is a separate feature path and
  must not be enabled merely to test basic framebuffer/display support.

## Source comparison: 0.3.0 to 0.3.1

The release merge from 0.3.0 to 0.3.1 contains eight commits. The changed source files
are:

- `src/sku.hpp`: adds RX 9070 GRE identification and compute-unit reporting.
- `include/rdna4mtlc.h`, `src/pvdraw.hpp`, `src/pvgpu.cpp`, and
  `tools/metal/m3/air2amdgpu.cpp`: extend the experimental paravirtual Metal path,
  including additional Metal/Vulkan shader and draw support.
- `src/compute.cpp` and `src/ih.cpp`: small changes in the compute and interrupt
  paths; these require particular care because they interact with kernel/IOKit APIs.
- `test/pvdraw-test.cpp`: adds a corresponding host-side test.
- `userspace/moltenvk/install.sh`: adds a userspace configuration helper.

This is a meaningful update from 0.3.0, but the release notes attribute most of the
new work to GPU/Metal functionality, not to a Tahoe 26.7.3 kernel compatibility fix.
The diff alone does not establish that the earlier boot failure is fixed.

## Automated validation

A GitHub Actions workflow has been added at
`.github/workflows/tahoe-compatibility.yml`. It checks out submodules, records the
runner OS and SDK, runs `make test`, builds the x86_64 kext, verifies bundle metadata,
and uploads the bundle as an artifact.

The workflow targets GitHub's `macos-26-intel` runner. The latest runner image
information available during this audit reports macOS 26.7.2 (build 25G332), not the
user's 26.7.3 (25G342). Therefore even a green run on that runner is useful build/test
evidence but is **not** an exact-build 26.7.3 certification. The workflow has been
committed but no run result is available yet.

## Decision

Do not add speculative kernel patches or bump the kext's version to imply support.
The release 0.3.1 baseline is the sensible candidate for further testing; the 0.4.0
`next` branch changes the version/changelog but does not add a source-code fix for
26.7.3. The next evidence needed is a successful build/test run, followed by a
controlled test on the actual 26.7.3 machine.

## Startup-path review

The plugin's `pluginStart()` reads `rdna4-off=1` before registering its NDRV hooks.
When that argument is present, it logs that it is disabled and returns. This is the
project's documented kill switch and is suitable for a first, inert injection check.
It does **not** test display takeover or GPU operation.

Without the kill switch, `requestedStage()` defaults to `StageGfx` unless overridden
by the project's trial-option mechanism. The device then proceeds into compute
bring-up after the framebuffer path is reached. This makes the user's earlier
0.3.0 reboot especially important to investigate: the plugin's startup is not merely
a passive version check.

For any eventual controlled boot test, retain a separate known-good EFI and use the
documented `rdna4-off=1` kill switch for the first injection-only check. Remove the
kill switch only after the inert boot is stable and the test configuration has been
reviewed. Do not combine the first active test with `PVGPU=1`, extra experimental
boot arguments, or changes to WhateverGreen.

## Experimental code change: conservative compute default

The compatibility branch now differs from upstream source in one runtime behavior:
`RDNA4Compute::requestedStage()` defaults to `StageSurvey` (stage 1) rather than
`StageGfx` (stage 3). Stage 1 is documented as read-only and runs inline; it
does not load the GPU firmware or start the later bring-up thread. An explicit
`rdna4-compute=<stage>` boot argument still overrides the default, so
`rdna4-compute=3` requests the previous GFX/SMU firmware stage.

This is a **diagnostic experiment**, not a proven Tahoe fix. It is intended to
separate the NDRV/display takeover path from the later GPU firmware bring-up
that may be involved in the user's earlier reboot. It may leave the display
path without the display clocks needed for higher resolutions or full
functionality. Do not treat a successful stage-1 boot as proof that the GPU is
accelerated, and do not enable stages 3+ until the stage-1 result and logs are
reviewed. The user's known-good EFI remains separate; this branch build is not
to be installed over it.
