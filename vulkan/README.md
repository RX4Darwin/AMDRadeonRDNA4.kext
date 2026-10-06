# Vulkan: the RADV Darwin port, taken from Navi48-MacOS

Copied unchanged on 2026-10-06 from https://github.com/Almosst-DEV/Navi48-MacOS at commit `69695975`:

| File | From | Licence |
|---|---|---|
| `mesa-patches/0001..0005-*.patch`, `mesa-patches/README.txt` | `mesa-patches/` | MIT, as Mesa (`LICENSE-Navi48-MacOS`) |
| `navi48_native_abi.h` | `src/navi48-bringup/src/Navi48NativeABI.h` | BSD 3-Clause (`LICENSE-Navi48-bringup`) |

This repository's own:

| File | What |
|---|---|
| `mesa-patches/0006-*.patch` | One line: the IOKit connection opened without a symbol that only exists from macOS 12 on (MIT, as Mesa) |
| `mesa-patches/0007-*.patch` | The driver looks for this kext's service, `RDNA4ComputeService`, before `Navi48Bringup` (MIT, as Mesa) |
| `n48nprobe.c` | The card test of the interface's memory half, without Mesa (`make n48nprobe`; `docs/vulkan-port.md` section 8) |
| `build-mesa.sh` | Fetches Mesa at the right commit, applies the patches, builds the driver |
| `vkprobe.c` | A small Vulkan program that drives the built driver without a Vulkan loader: two fills by the GPU, submitted, waited for and checked |

The first five patches (about 9,800 lines) add a macOS backend to RADV, Mesa's Vulkan driver for AMD cards. They
apply to Mesa commit `f5cb8ee032adabef599ae892ec4e42d796b84da9` (26.3.0-devel). Instead of a Linux DRM node the
driver talks to a kernel extension through an IOKit user client. The header is that interface: a close copy of
the part of Linux's amdgpu ioctls RADV uses (buffers, address-space mapping, contexts, command submission,
fences) plus calls to present to the screen.

**Status: builds, and runs on a Mac against its own stand-in for the kext and against this kext's engine
(`tools/n48n-host/`); never run against a kext or a card.** The kext has the interface up to submitting work and
waiting for it (`src/n48nkext.cpp`), none of it run on the card. `docs/vulkan-port.md` has what the port needs
from the kext, what this repository already has, the order of work, and what each step showed.

Not taken from that project, on purpose: its Metal bundle, its helper accelerator kext and its hooks into Apple's
Radeon driver. They are built against the internals of one macOS build; the Vulkan driver and its kernel interface
are not.
