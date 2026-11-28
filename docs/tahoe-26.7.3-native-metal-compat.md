# Tahoe 26.7.3 native Metal compatibility audit

Target machine reported by the maintainer: macOS Tahoe 26.7.3, build 25G342, x86_64, Ryzen 5 5600X, RX 9070 XT (Navi 48).

This is an audit record, not a claim that 26.7.3 is supported. Upstream v0.4.0 documents Tahoe 26.6.2 as hardware-tested and 26.7 / later as needing testing.

## Confirmed source facts

1. The v0.4.0 path uses the driver's own native Metal accelerator by default. The paravirtual path is an explicit alternative.
2. The native accelerator derives from private `IOAcceleratorFamily2` classes. The project declares class layouts from a KDK and resolves base methods at runtime through generated thunks.
3. Before this audit, the generated declarations identified `KDK_26.5.2_25F84.kdk` as their source.
4. `tools/gen/accelgen.py` generates six files: `ioaccel.hpp`, `ioaccel_thunks.S`, `ioaccel_symbols.inc`, `ioaccel_slots.inc`, `ioaccel_kexts.inc` and `ioaccel_classes.inc`.
5. `src/rdna4accel.cpp::familyLoaded` checks symbol resolution, expected vtable slots and metaclass layout before marking the accelerator ready. `accelTo` checks runtime base-class sizes before constructing the derived accelerator.
6. The plugin also hooks `IONDRVFramebuffer::doDriverIO`. This is a separate compatibility surface from the native accelerator's private vtables.
7. The audit does not establish 26.7.3 hardware compatibility or explain the reported early boot stall.

## 26.7.3 KDK ABI audit: completed

GitHub Actions run [46148522866](https://github.com/lucasmcgroarty29-tech/AMDRadeonRDNA4.kext/actions/runs/46148522866) completed successfully on a macOS 26 runner.

- Downloaded `Kernel_Debug_Kit_26.7.3_build_25G342.dmg`.
- Verified SHA-256: `219f27c7e039b6ab3426496e0d5852d55a31d00b3d3984d855cba04377c5dba0`.
- Installed `KDK_26.7.3_25G342.kdk`.
- Ran the generator successfully: 16 classes and 531 thunks.
- Compared all six generated files against the committed v0.4.0 declarations.
- The only differences were the generated KDK-version comments. Class layouts, symbols, vtable slots, thunk declarations, KDK associations and class declarations were otherwise identical.
- Uploaded artifact: `tahoe-26.7.3-25G342-generated-abi` (artifact ID `15264732989`).

**Conclusion:** no ABI declaration changes are justified by this comparison. Regenerating and committing the six files would only create comment churn. The existing runtime checks remain important because the target machine's loaded runtime still needs to confirm the expected layout.

## Boot-stall investigation

The ABI audit does not explain the reported early boot stall. If the machine stops before `IOAcceleratorFamily2` loads, `familyLoaded` has not run and its checks cannot be the cause. The plugin's `rdna4-off=1` option disables the plugin's hooks as well as its accelerator path, so it is a broad isolation switch, not a targeted accelerator-only test.

The separate `IONDRVFramebuffer::doDriverIO` hook and the PCI/ACPI startup path remain independent areas to investigate. Do not infer that an unchanged accelerator ABI proves the whole driver is compatible.

## Next engineering steps

1. Run the full branch build and host tests on GitHub's macOS runner. A successful build is necessary but is not hardware validation.
2. If CI passes, investigate the PCI/ACPI boot stall separately from the accelerator ABI. Review the exact startup ordering and the existing boot log evidence before changing patches.
3. Keep experimental builds and any EFI changes separate from the only recovery EFI. Do not install an unvalidated build as the sole boot option.
4. Hardware acceptance still requires a successful boot to the desktop on build 25G342, runtime symbol/slot/class-size checks, native Metal device enumeration and the project's Metal probes.

Until those hardware criteria pass, this branch is experimental and must not be described as Tahoe 26.7.3-compatible.

## Generator safety improvement

The compatibility branch requires an explicit `--kdk` path and supports `--output-dir`. It no longer silently selects the newest installed KDK. Generated files are staged outside the source tree for review before any source replacement.
