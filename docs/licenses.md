# Licences of what a release ships

A checklist for each binary release, from reading the build on 2026-11-23. It is an engineering inventory, not legal advice.

| Shipped in | Component | Licence | Text travels in |
|---|---|---|---|
| kext, bundle | AMDRadeonRDNA4 | BSD 3-Clause | `LICENSE` |
| kext | AMD firmware, embedded | AMD's firmware licence (binary redistribution with its notice) | `Contents/Resources/LICENSE.amdgpu` |
| kext | Lilu's `plugin_start.cpp`, compiled in | BSD 3-Clause | `Contents/Resources/LICENSE.Lilu` |
| kext | `libkmod`, `libkmodc++` from MacKernelSDK, linked in | Apple Public Source License 2.0 | `Contents/Resources/LICENSE.MacKernelSDK` |
| kext | Tables and register sequences from Linux's amdgpu | MIT | `Contents/Resources/LICENSE.amdgpu-code` |
| bundle, `bin/` | Mesa's RADV | MIT, one file BSL-1.0 | `Contents/Resources/Licenses/Mesa-*` |
| bundle, `bin/` | The Mesa patches and ABI header from Navi48-MacOS and Navi48-bringup | MIT, BSD 3-Clause | `Contents/Resources/Licenses/Navi48-*` |
| bundle | LLVM and lld, linked statically into `air2amdgpu` | Apache-2.0 with LLVM exception | `Contents/Resources/Licenses/LLVM` |

`tools/package/licenses.sh` copies these at build time and stops the build when a component is there without its licence.

## Checked

- **No GPL code in RADV.** None of the 694 source files in the build's `compile_commands.json` carries a GPL notice in its first 6 KB.
  Mesa's GPL files (`src/gallium/drivers/svga`, a freedreno header) are not built.
- **`emu/qemu/rdna4.c` is GPL-2.0-or-later** because it is a QEMU device. It is development source only and is in no release binary.
- **`tools/gen/pipegen/shim`** is the generator's stand-in for Linux headers, with its licence in its `NOTICE`. It is not shipped.

## Open

- **Releases up to 0.4.0 ship without the Lilu, MacKernelSDK, amdgpu-code, Mesa, Navi48 and LLVM texts.** Only the AMD firmware licence was
  in them. The next release fixes this. Whether to re-upload the older zips is the maintainer's decision.
- **APSL 2.0** asks that the source of the covered code is available. MacKernelSDK is public, and the repository pins its commit.
- **Mesa's MIT notices** name many copyright holders, in each source file. The bundle carries Mesa's licence and the MIT text, not a list of
  every holder.
- **AMD FidelityFX FSR 1 (MIT).** `src/shaders/fsr1.cl` is a port of `ffx_fsr1.h` (the `lib/FidelityFX-FSR` submodule) and carries
  AMD's notice. Nothing ships it yet; once the kext or bundle embeds its code object, its licence text goes with the others above.
- **LGPL references.** `docs/metal-spike.md` names reims-vgpu (LGPL-3.0-or-later) as read-only reference. Nothing here was compared against it.
