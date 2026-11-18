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
