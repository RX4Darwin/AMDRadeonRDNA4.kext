# Tahoe 26.7.3 native Metal compatibility audit

Target machine reported by the maintainer: macOS Tahoe 26.7.3, build 25G342, x86_64, Ryzen 5 5600X, RX 9070 XT (Navi 48).

This is an audit record, not a claim that 26.7.3 is supported. The upstream v0.4.0 README lists Tahoe 26.6.2 as tested and 26.7 / later as needing testing.

## Confirmed source facts

1. The v0.4.0 path uses the driver's own native Metal accelerator by default. The paravirtual path is an explicit alternative. See `README.md`, `CHANGELOG.md`, `src/trial.cpp` and `src/rdna4accel.cpp`.
2. The native accelerator derives from private `IOAcceleratorFamily2` classes. The project cannot link that System Kernel Collection framework directly, so it declares class layouts from a KDK and resolves base methods at runtime through generated thunks.
3. `src/ioaccel_thunks.S` currently identifies its generator source as `KDK_26.5.2_25F84.kdk`.
4. `tools/gen/accelgen.py` generates `src/ioaccel.hpp`, `src/ioaccel_thunks.S`, `src/ioaccel_symbols.inc` and `src/ioaccel_slots.inc` from a supplied KDK. Its comments say the generated slots must match the running macOS.
5. `src/rdna4accel.cpp::familyLoaded` checks symbol resolution, expected vtable slots and metaclass layout before marking the accelerator ready. `accelTo` then checks the runtime base-class sizes before constructing the derived accelerator. A mismatch should make the native accelerator unavailable rather than silently using an unverified layout.
6. The plugin also hooks `IONDRVFramebuffer::doDriverIO` in `src/plugin.cpp`. This is a separate compatibility surface from the native accelerator's private vtables.
7. The existing project documentation describes 26.6.2 as the hardware-tested Tahoe version. No 26.7.3 hardware validation is established by this audit.

## Working hypothesis

The highest-value compatibility check for the native Metal path is whether the private `IOAcceleratorFamily2` symbols, vtable slots and class sizes in build 25G342 still match the 26.5.2 KDK-generated declarations. The existing runtime checks are useful evidence: collect their complete log output before changing any layouts.

This does **not** yet explain the reported early boot stall. If the machine stops before `IOAcceleratorFamily2` is loaded, the native accelerator's `familyLoaded` checks have not run and cannot be assumed to be the cause. The `rdna4-off=1` option disables the driver's hooks, so a boot comparison with that option is useful, but it does not by itself isolate the private accelerator ABI.

## Next engineering steps

1. Obtain the exact matching Kernel Debug Kit: `Kernel_Debug_Kit_26.7.3_build_25G342.dmg` (build `25G342`). The archived KDK manifest lists SHA-256 `219f27c7e039b6ab3426496e0d5852d55a31d00b3d3984d855cba04377c5dba0`; verify the checksum after downloading. Apple Developer Downloads is the preferred source; the Dortania KdkSupportPkg archive also indexes this exact build. Do not substitute a different KDK merely because its marketing version also says 26.7.3.
2. Run `tools/gen/accelgen.py --kdk <matching-KDK>` on a clean checkout of this branch and inspect the generated diff. Do not overwrite the existing generated files until the diff is reviewed.
3. Compare the generated vtable slot tables, class sizes and symbol names with the 26.5.2-generated files. Preserve a clear record of every changed slot and class size.
4. Build the regenerated branch and run the existing host tests/CI. A successful compile is not hardware validation.
5. On the target machine, first use the separate test EFI. Capture the full `AMDRadeonRDNA4: accel:` sequence and verify that all symbols resolve, all slots match, all class sizes match, and the accelerator reaches its ready state before enabling the native desktop.
6. Keep the `rdna4-off=1` recovery path available. Do not install an unvalidated build into the only working EFI.

## Acceptance criteria

- The generated ABI files identify the exact target KDK.
- No missing symbols, wrong vtable slots or class-size mismatches are reported.
- Existing host tests and CI pass.
- The RX 9070 XT boots to the desktop on build 25G342 using a test EFI.
- Native Metal device enumeration and the project's Metal probe pass on that hardware.

Until the last two criteria pass, this branch is experimental and should not be described as Tahoe 26.7.3-compatible.


## Generator safety improvement

The compatibility branch now requires an explicit `--kdk` path and supports `--output-dir`. It no longer silently selects the newest installed KDK. Generate into a temporary directory, review all ABI diffs, and only then copy reviewed files into `src/`.
