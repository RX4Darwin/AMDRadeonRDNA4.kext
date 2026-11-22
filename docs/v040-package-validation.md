# v0.4.0 package validation checklist

Status: release-engineering checklist. This document does not authorize installing the package on a daily-use machine.

## 1. Build identity

The kext and user-space package must be produced from the same commit and Makefile version. Record:

- Git commit SHA and branch.
- `VERSION` from the Makefile and package filename.
- `CONFIG=RELEASE`, `PVGPU=1`, and the firmware set embedded in the kext.
- macOS SDK and Xcode/Metal toolchain versions.
- SHA-256 for the package, matching kext executable, README and final archive.

Never pair a kext from one CI run with a user-space package from another run, even when both are labelled 0.4.0.

## 2. Expected package contents

The package is intentionally split into three installer choices:

| Component | Expected contents | Purpose |
| --- | --- | --- |
| Core (required) | `/Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle` | Native Metal driver and embedded runtime |
| Core helpers | `air2amdgpu`, `rdna4-run`, `rdna4-iop` | Shader translation, runtime client and display setup |
| Boot logs (optional) | `org.rdna4.logd` LaunchDaemon and helper | Captures driver boot logs |
| Vulkan (optional) | RADV library and `radeon_icd.json` | Mesa Vulkan ICD |

The OpenCore-injected `AMDRadeonRDNA4.kext` is deliberately outside the installer package. The release archive must carry the matching kext beside the package so that users can verify and install the pair through their OpenCore workflow.

The package build must fail if any helper required by a selected component is absent. In particular, check `rdna4-iop` and `rdna4-logd`, not only `air2amdgpu` and `rdna4-run`.

## 3. Static checks before publication

On the macOS build runner, verify all of the following before uploading an artifact:

- Kext executable, Metal driver executable, and all bundled helper executables exist and are executable.
- Every executable and the RADV dylib contains the expected `x86_64` architecture.
- Kext, bundle and embedded framework `CFBundleVersion` and `CFBundleShortVersionString` match the Makefile release version. The package components are built with that same version argument.
- All property lists and the Vulkan ICD manifest parse successfully.
- The package archive contains its Distribution document and all three component packages.
- The release archive contains the package, matching kext, README and checksum manifest.
- `shasum -c SHA256SUMS.txt` succeeds from inside the extracted release directory.
- The final ZIP can be listed and its contents match the expected release directory.

A package that merely exists is not a successful package build. Missing optional-looking files can still make a selected installer choice fail at runtime.

## 4. Installer-script review

Review the generated component scripts as well as the package payload:

- Core preinstall stops the old helper daemons and removes only the legacy paths documented by the package script.
- Core postinstall bootstraps the two core daemons.
- Logd preinstall and postinstall stop and start only the log collection daemon.
- The Vulkan choice installs only the RADV ICD manifest and does not silently alter unrelated Vulkan installations.
- Generated preinstall and postinstall scripts for core and logd pass `sh -n`.
- LaunchDaemon plists point to helpers inside the installed bundle, use the intended labels, and pass `plutil -lint`.
- The core choice remains required; log collection and Vulkan remain independently selectable.

Do not run these scripts on the development machine as a substitute for static inspection. They are system-level installer actions.

## 5. Evidence boundary

A successful package workflow proves that the installer was assembled and passed static checks on the CI runner. It does not prove that:

- the installer can safely update every prior 0.3 installation;
- the OpenCore-injected kext and user-space bundle work together on a specific Hackintosh;
- the display stack survives login, sleep/wake, shutdown or reboot;
- Metal, OpenGL or Vulkan applications render correctly on an RX 9070 XT.

Those claims require a separately controlled real-card validation session with a known-good EFI recovery route. Keep build validation, package validation and hardware validation as distinct results.
