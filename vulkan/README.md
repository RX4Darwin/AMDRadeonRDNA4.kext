# Vulkan: the RADV Darwin port, taken from Navi48-MacOS

What is here was copied unchanged on 2026-10-06 from
https://github.com/Almosst-DEV/Navi48-MacOS at commit `69695975`:

| File | From | Licence |
|---|---|---|
| `mesa-patches/0001..0005-*.patch`, `mesa-patches/README.txt` | `mesa-patches/` | MIT, as Mesa (`LICENSE-Navi48-MacOS`) |
| `navi48_native_abi.h` | `src/navi48-bringup/src/Navi48NativeABI.h` | BSD 3-Clause (`LICENSE-Navi48-bringup`) |

The five patches (about 9,800 lines) add a macOS backend to RADV, Mesa's Vulkan driver for AMD cards. They
apply to Mesa commit `f5cb8ee032adabef599ae892ec4e42d796b84da9` (26.3.0-devel); `mesa-patches/README.txt` says how.
Instead of a Linux DRM node the driver talks to a kernel extension through an IOKit user client. The header is that
interface: a close copy of the part of Linux's amdgpu ioctls RADV uses (buffers, address-space mapping, contexts,
command submission, fences) plus calls to present to the screen.

**Status: taken, not built, not run.** No kext in this repository provides the interface yet, so the patched Mesa
would find no device. `docs/vulkan-port.md` has what the port needs from the kext, what this repository already
has, and the order of work.

Not taken from that project, on purpose: its Metal bundle, its helper accelerator kext and its hooks into Apple's
Radeon driver. They are built against the internals of one macOS build; the Vulkan driver and its kernel interface
are not.
