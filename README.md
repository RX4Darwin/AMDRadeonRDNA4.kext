# AMDRadeonRDNA4

A macOS driver for AMD Radeon **RDNA 4** graphics cards (Navi 48: RX 9070 XT, RX 9070, RX 9070 GRE; Navi 44: RX 9060 XT,
not yet run on one under macOS) on x86_64 Hackintosh. It is a **Lilu plugin**, injected by OpenCore, built against
MacKernelSDK; it cross-compiles on Apple Silicon and on Linux/WSL.

macOS has no driver for RDNA 3 or RDNA 4: Apple's AMD support ends at RDNA 2, and an RDNA 2 driver cannot be spoofed
onto Navi 48 (its command streams are another architecture's). AMDRadeonRDNA4 drives the card itself:

- **Displays**: up to four, on DisplayPort and HDMI, with every mode the monitor offers, mode switching, hot-plug,
  display sleep and system sleep. The display controller (DCN 4.01) is programmed by the kext, after `amdgpu`'s
  display core.
- **Metal, OpenGL and Vulkan** for the desktop and apps: the driver carries its own Metal driver for the card, on its
  own accelerator, so WindowServer composites on the card and Metal, OpenGL and Vulkan apps (through MoltenVK) draw on
  it — by default, with no virtual GPU (build with `make PVGPU=1`). macOS's paravirtual GPU driver is kept as an
  alternative (`rdna4-accel=0 rdna4-pvgpu=1`).
- **Compute**: the GFX12 command processor, its rings, memory management and a compute runtime for gfx1201 kernels,
  written from scratch.

Versions and what each one changed are in [CHANGELOG.md](CHANGELOG.md); branches, version numbers and releases in
[CONTRIBUTING.md](CONTRIBUTING.md). Until October 2026 the driver was called RDNA4FB, and older log files carry that
name.

## Supported cards

| Card | Chip | PCI ID (revision) | Status |
|---|---|---|---|
| Radeon RX 9070 XT | Navi 48 (gfx1201) | `1002:7550` (`0xC0`) | **Supported.** The card everything is developed and tested on. |
| Radeon RX 9070 | Navi 48 (gfx1201) | `1002:7550` (`0xC3`) | **Supported.** Ran the paravirtual GPU under Sequoia 15.8.2 (the measurements of PR 6). |
| Radeon RX 9070 GRE | Navi 48 (gfx1201) | `1002:7550` (`0xC2`) | Recognised and named, its 48 compute units reported; **needs testing** (not yet run on one). |
| Radeon AI PRO R9700 | Navi 48 (gfx1201) | `1002:7551` | Matched and named; **needs testing** (not yet run on one). |
| Radeon RX 9060 XT | Navi 44 (gfx1200) | `1002:7590` (`0xC0`) | **Experimental: not yet run under macOS** (help wanted). The kext matches the card, names it, and carries its own firmware set (PSP 14.0.2, SMU 14.0.2, SDMA 7.0.0, GC 12.0.0), chosen from the GC version the card reports. Measured on an RX 9060 XT 16 GB under Linux: the same register bases and `GB_ADDR_CONFIG` as the RX 9070 XT, 2 shader engines and 32 compute units, its VBIOS's display paths (two DisplayPort, one HDMI) parsed by the host tests, and the kext's triangle and colour-triangle streams drawing right; the kext also runs through every bring-up stage in the emulator with that card's VBIOS and firmware (PR 17). Its displays, power management and the SMU 14.0.2 table layout have not been driven by the kext. |
| Radeon RX 9060 | Navi 44 (gfx1200) | `1002:7590` | As the RX 9060 XT, untested; it is named and counted as an RX 9060 XT (32 compute units) for now. |

Everything that differs between the cards (VRAM size, shader engines, display paths, the compute pool's place) is read
from the card, not assumed (`src/sku.hpp` holds only the names and compute-unit counts). One kext carries the firmware of
both chips when both sets are in `firmware/amdgpu/` (`tools/build/fetch-firmware.sh` fetches both); a card whose set the
build lacks stops at the firmware stage with a message that says so.

On an RX 9060 XT under Linux, reading the display engine's registers one after another (`tools/linux/linux-capture.sh`)
froze the machine at `ODM_MEM_PWR_CTRL2`; the kext never reads it, and `docs/emu-linux.md` has the details.

## macOS versions

| macOS | Status |
|---|---|
| **Tahoe 26.6.2** | **Tested**: the main test system. Everything in *Features* runs here, Metal, OpenGL and Vulkan included. |
| Tahoe 26.0 - 26.6.1, 26.7 and later | **Needs testing.** The displays and compute should run as on 26.6.2. OpenGL on the card needs Tahoe 26.6.2's renderer (the kext patches it as it loads; any other version stays in software), and the driver's IOSurface lookup follows 26.6.2's IOSurface internals. |
| **Sequoia 15.8.2** | **Tested** by a contributor (Jan Claußen, PR 6): the Metal desktop through the paravirtual GPU. OpenGL stays in software. |
| Sequoia 15.0 - 15.7 | **Needs testing.** |
| Sonoma 14, Ventura 13, Monterey 12 | **Needs testing.** The kext builds for macOS 11 and later; the paravirtual GPU path has not been tried on them. |
| **Big Sur 11.6.6 / 11.7.10** | **Tested** for the displays (two displays, mode switching, hot-plug, display sleep) and compute, until October 2026. Metal through the paravirtual GPU not tried. |

Lilu **1.7.1 or newer** is needed on macOS 26 (older Lilu turns itself off there).

## Features

### Displays

- The display the firmware (GOP) lit is taken over without a flash or a reset: its EDID read over DisplayPort AUX or
  HDMI DDC, its modes offered to macOS, the monitor named in System Settings.
- **Mode switching** on DisplayPort and HDMI, from the monitor's EDID modes: HDMI up to 600 MHz (HDMI 2.0, scrambled,
  for a monitor that announces SCDC), DisplayPort with the link retrained by the kext for modes above what the firmware
  trained (2560x1440 at 144 Hz, 3840x2160 at 60 Hz). High refresh rates (1920x1080 at 120 and 144 Hz).
- **Several displays**: up to three beside the boot display (`rdna4-heads`), two tested at once. The kext lights the
  extra pipes itself, with register sequences generated from Linux's own DCN 4.01 code: HDMI on either HDMI connector,
  DisplayPort with the kext's own link training (`rdna4-head2dp=1`), HDMI monitors behind passive DP++ adapters.
- **Hot-plug** of the second display: unplugging takes it offline in macOS, plugging it in lights it again, also when
  it moves to the other HDMI connector.
- **HiDPI and scaled modes**, larger than the monitor ("looks like 2560x1440" on a 4K monitor), scaled on the card.
- **Hardware cursor** on every display, HiDPI included (WindowServer then redraws nothing as the pointer moves).
- **Display sleep and system sleep**: the displays sleep and wake; the system sleeps and wakes with the desktop back on
  every display a couple of seconds after a key press (the card is brought up again from nothing after its power
  loss).
- **Cold start**: the display engine can be started with the kext's own display firmware and lit from nothing, even
  after a reset of the card (`rdna4-dmubload`, `rdna4-dcninit`, `rdna4-coldboot`).

### Metal

The driver has its own Metal driver for the card: a Metal plug-in (`AMDRadeonRDNA4MTLDriver.bundle`) that Apple's
Metal.framework loads, running on the driver's own accelerator and command submission — Metal natively, no virtual GPU.
WindowServer and apps use it by default (`src/trial.cpp`; `rdna4-accel`, `rdna4-nativews`). Shaders arrive as Apple's
AIR and are translated to gfx1201 by a helper (`tools/metal/m3/air2amdgpu.cpp`, LLVM).

- The desktop: WindowServer composites on the card, scanned out by the card's display engine. The login window, the
  Dock, Liquid Glass with its blurred glass, apps' windows, the animated wallpapers.
- Compute and blits; draws of every form (indexed, instanced, indirect, primitive restart), vertex descriptors, up to
  eight colour attachments, blending (dual-source blending too), the depth and stencil tests, 2x and 4x multisampling,
  sRGB, textures with mip levels, texture views and arrays, 3D textures, IOSurfaces.
- **Ray tracing**: acceleration structures (primitive and instance) and ray queries in shaders — MetalQuake's
  ray-traced shadows.
- 3D games: MetalQuake at 3840x1878 in HDR at 60 frames a second.
- WebGL in Safari and Chrome (Safari's sandboxed GPU process is allowed to load the bundle by `rdna4-webkitgpu`, on by
  default).
- `tools/metal/m1/mtlprobe.m` checks it (`mtlprobe all`).

**Apple's paravirtual GPU** (the default before 0.4.0; now `rdna4-accel=0 rdna4-pvgpu=1`) is kept as an alternative:
macOS ships a Metal driver for the paravirtual GPU of its virtual machines (`AppleParavirtGPU`), and with `make PVGPU=1`
the kext plays its host on the card (`src/pvgpu.cpp`), running Apple's command stream on the RX 9070. It has no ray
tracing, Metal heaps, argument buffers or indirect command buffers, and gives shaders at most 16 textures a stage, so
some apps (Chrome's WebGL via Dawn) do not run on it.

### OpenGL

macOS's OpenGL 4.1 runs on Metal (`AppleMetalOpenGLRenderer`), and so on the card through the driver's Metal device
(`rdna4-accelgl`, on by default), so OpenGL programs draw on the card unchanged: kitty, Mixxx, Sublime Text. Tahoe
26.6.2's renderer only (the kext patches it as it loads).

### Vulkan

- **Through MoltenVK** on Metal: Vulkan apps and games run as on any Mac (vkQuake at 3840x2160, about 44 frames a
  second). The driver's own Metal device has Metal argument buffers, so MoltenVK runs on it unchanged; on Apple's
  paravirtual driver (which lacks them) `userspace/moltenvk/install.sh` (once per user) turns them off.
- **RADV**, Mesa's Vulkan driver, built for macOS (`make mesa`, `userspace/vulkan/`): Vulkan on the card without
  Metal, in windows (`VK_EXT_metal_surface`), up to three programs at once. Programs that use Khronos's loader find it
  through the manifest in the driver bundle (below); `userspace/vulkan/radv-app.sh` makes a copy of an app built
  against MoltenVK that runs on RADV (vkQuake at 3840x2160, about 84 frames a second).

### Compute

`rdna4-run` and `librdna4` run gfx1201 kernels (as clang builds them) on the card from programs run as root: buffers
in VRAM, DMA transfers, a 3-D dispatch, each client in an address space of its own. SGEMM at 10-12 TFLOPS, the matrix
units (WMMA) at 53 TFLOPS in FP16. See *Compute runtime* below.

### Power

- The power-management firmware runs its features: the graphics clock follows the load (3.2 GHz under load, about
  250 MHz idle), the display clocks are held at what the displays need.
- Sensors: temperature, power, clocks and activity (`rdna4-run sensors`), also in the accelerator's `PerformanceStatistics` under
  the keys Apple's AMD drivers use, so Activity Monitor's GPU history, AMD Power Gadget and iStat read them.
- **RDNA4Gadget** (`RDNA4Gadget.app` in /Applications from the installer; `make gadget`, `userspace/gadget`): a menu bar app with the card's temperature and power, its readings in a
  popover, the installed driver against the latest release (with a notification when one is out), install, update and removal
  (the kext into the EFI OpenCore booted from, through `efi.sh` and its backup). A VirtualSMC GPU plug-in's SMC keys (`TGDD`,
  `TG0P`, ...) are shown where there is one.
- Settings that last: `sudo sysctl debug.rdna4_settings="on rdna4-gfxcap=2000"` (the NVRAM variable `rdna4-settings`), taken at
  every boot below a boot-arg of the same name, for `rdna4-pvcursor`, `rdna4-accelgl`, `rdna4-head2dp`, `rdna4-pvfast`,
  `rdna4-hotplug` and `rdna4-gfxcap`. A boot that does not last two minutes turns them off.

### Not implemented yet

- Apps started before the driver's accelerator is ready (a few seconds after boot) draw on the CPU until restarted.
- A display plugged in after boot gets no accelerated display for it (they are counted at boot).
- In the paravirtual mode (`rdna4-pvgpu=1`): no ray tracing, Metal heaps, argument buffers or indirect command buffers,
  and at most 16 textures a stage (so Chrome's WebGL via Dawn does not run). The native driver (the default) has these.
- Display Stream Compression (DSC): 3840x2160 at 120 and 144 Hz.
- Variable refresh (FreeSync), DisplayPort MST, HDR output to the monitor.
- Audio over HDMI and DisplayPort.
- Hardware video decode and encode (VCN).
- Fan control; the power features held back so that they do not retune clocks under the lit displays (display DPM,
  memory and fabric deep sleep, GFXOFF).
- The Navi 44 cards (RX 9060 XT, RX 9060) under macOS: matched and given their firmware, measured under Linux and in the
  emulator, not yet run on one (see *Supported cards*).

## Installing

1. Use **Lilu 1.7.1 or newer**.
2. Copy `AMDRadeonRDNA4.kext` (from a release, or `Build/AMDRadeonRDNA4-<version>-RELEASE/`) to `EFI/OC/Kexts/` and add
   a `Kernel → Add` entry **after Lilu**:

   | Key | Value |
   |-----|-------|
   | `BundlePath` | `AMDRadeonRDNA4.kext` |
   | `ExecutablePath` | `Contents/MacOS/AMDRadeonRDNA4` |
   | `PlistPath` | `Contents/Info.plist` |
   | `Arch` | `x86_64` |
   | `Enabled` | `true` |

   No code signing, SIP change or kext approval is involved: OpenCore injects it into the boot collection like Lilu
   itself.
3. Turn off other GPU kexts (WhateverGreen) so that nothing else claims the card.
4. *(Optional)* The card's full 2 MiB flash dump as `ATY,bin_image` under the GPU's PciRoot path in `DeviceProperties`.
   Without it the driver reads IP discovery from VRAM and the VBIOS from the PCI expansion ROM (`Discovery,Source` in
   ioreg says which).
5. **Relax SIP and AMFI** so macOS loads the driver's user-space bundle. The kext and the bundle are signed ad hoc, not
   by Apple, and the bundle lives in `/Library/GPUBundles` (the only place Metal loads a third-party driver from), so
   two concessions are needed — the same ones every comparable driver (including nullmoth's NVIDIA driver) needs:
   - **Filesystem Protections off** in `csr-active-config` (bit `0x2`). An Apple platform binary such as WindowServer
     will not map a non-Apple library while they are on, and this is what makes `amfi_get_out_of_my_way` take effect at
     all; it cannot be worked around from the kext (tested exhaustively on Tahoe 26.6.2). A Hackintosh already runs with
     kext signing off (bit `0x1`), so `csr-active-config = 03000000` is the usual minimum (nullmoth's driver uses the
     looser `0A430000`).
   - **AMFI relaxed** so the ad-hoc signatures are accepted, one of:
     - `amfi_get_out_of_my_way=0x1` in `boot-args` (simplest, what the main test system uses), or
     - [`AMFIPass.kext`](https://github.com/dortania/OpenCore-Legacy-Patcher/blob/main/payloads/Kexts/Acidanthera/AMFIPass-v1.4.1-RELEASE.zip)
       in `Kernel → Add` after Lilu — it leaves AMFI's library validation on for the rest of the system. On a macOS
       newer than the kext build knows (Tahoe needs this with v1.4.1), add `-amfipassbeta` to `boot-args`.
6. Install the user-space driver (Metal, OpenGL, Vulkan) from `AMDRadeonRDNA4-<version>.pkg` of the **same release** as
   the kext, then reboot:

   ```bash
   sudo installer -pkg AMDRadeonRDNA4-0.4.0.pkg -target /
   ```

   Or double-click it; the package is not signed, so the first time Finder refuses it: Control-click it and choose
   **Open**. What it installs: [Metal, OpenGL and Vulkan](#metal-opengl-and-vulkan).

Keep a known-good copy of your EFI on a USB stick while testing. `rdna4-off=1` turns the plugin off and `-liluoff` all
Lilu plugins; macOS then runs its stock framebuffer.

### OpenCore settings

**Install macOS first**, with the kext disabled or with `rdna4-off=1`. The macOS installer and Recovery do not have the Metal
driver from the package. Then enable the kext and install the package.

| Setting | Value | Why |
|---|---|---|
| Lilu | 1.7.1 or newer, in `Kernel → Add` before this kext | Older Lilu turns itself off on macOS 26 |
| Other GPU kexts | WhateverGreen, NootRX and older copies of this driver disabled, their boot-args removed | Only one driver can claim the card |
| `Misc → Security → SecureBootModel` | `Disabled` | Apple Secure Boot refuses kexts Apple did not sign |
| `csr-active-config` | `03000000` or looser | Bit `0x2` lets WindowServer load the Metal driver |
| `boot-args` | `amfi_get_out_of_my_way=0x1` (or AMFIPass), no old `rdna4-*` options | The defaults need no driver options |
| `NVRAM → Delete → 7C436110-AB2A-4BBB-A880-FE41995C9F82` | list `boot-args` and `csr-active-config` | Otherwise OpenCore keeps the stored value and logs `ignored, exists` |
| BIOS | Above 4G Decoding on, CSM off | The card's apertures need it |
| Resizable BAR | On or off in the BIOS. With it on, `Booter → Quirks → ResizeAppleGpuBars` = `10` (1 GiB; `0` gives 256 MiB) | macOS gives a VRAM BAR above 1 GiB no address: with `-1` and Resizable BAR on there is no desktop. 256 MiB and 1 GiB are tested |
| USB | A port map (on Tahoe, USBToolBox 1.2.0 or newer) | Without one the keyboard and mouse may not work at all |
| System sleep | If USB does not wake the machine: USBWakeFixup and an `SSDT-USBW` for your board's GPE | Some boards wake USB through a GPE nothing claims (GPE 8 on the main test system) |

`tools/package/efi.sh` (also in the driver bundle's `Contents/Resources`) applies the kext, boot-args, `NVRAM → Delete` and
`ResizeAppleGpuBars` parts of this table to a mounted EFI, and keeps a backup. It reports SIP and Secure Boot without changing them. It has been tested against sample
configs, not yet on a real EFI, so keep your USB copy. Try it with `--dry-run` first:

```bash
sh tools/package/efi.sh install --efi /Volumes/EFI --kext AMDRadeonRDNA4.kext --dry-run
```

Check a config without changing it:

```bash
sh tools/package/efi.sh check --efi /Volumes/EFI
```

Go back to the `config.plist` and kext from before the last install:

```bash
sh tools/package/efi.sh remove --efi /Volumes/EFI
```

### Updating from 0.3.3

0.4.0 needs no boot-args: what 0.3 asked for is built in, and its desktop is now the driver's own accelerator
(`rdna4-accel=4`), not Apple's paravirtual GPU. **Remove the driver's boot-args from 0.3** from `boot-args` in `config.plist`
(`NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82`), and keep `boot-args` listed under `NVRAM → Delete` for that GUID (or
reset NVRAM) so the new value is used. In particular:

- `rdna4-pvgpu=1`: left in, it brings Apple's paravirtual GPU up beside the native desktop.
- `rdna4-compute=7`, `rdna4-gfx=2`, `rdna4-vm=1`, `rdna4-vmshared=…`, `rdna4-pvgpu-disp=…`, `rdna4-pvgpu-delay=…`, `rdna4-pvfast=1`,
  `rdna4-pvgl=…`, `rdna4-accel=4`, `rdna4-nativews=…`, `rdna4-dmubload=1`, `rdna4-dcninit=1`, `rdna4-head2dp=1`,
  `rdna4-syssleep=1`: now the defaults.

Keep only a boot-arg that changes a default on purpose ([Boot arguments](#boot-arguments)), and the ones that are not the
driver's (`amfi_get_out_of_my_way=0x1`, Lilu's). Then install the kext and the package of the same release (step 6 above
replaces 0.3's per-component install scripts) and reboot.

### Metal, OpenGL and Vulkan

`AMDRadeonRDNA4-<version>.pkg` (step 5 above) comes with each release; to build it: `make PVGPU=1 all metal-helper mesa
package` (the shader translator builds LLVM for x86_64 in `Build/llvm` the first time, which takes a while). In the
installer's **Customize** pane the boot logs and Vulkan can be left out. It installs:

| Choice | What |
|--------|------|
| Metal driver (required) | the driver bundle below; the LaunchDaemons `org.rdna4.air2amdgpu` (the shader translator for WindowServer, log `/var/log/rdna4-air2amdgpu.log`) and `org.rdna4.iop` (sets CoreDisplay up for each boot, log `/var/log/rdna4-iop.log`); `/usr/local/bin/rdna4-run` |
| Boot logs | the LaunchDaemon `org.rdna4.logd`: each boot's kernel log in `/var/log/rdna4` |
| Vulkan (RADV) | RADV's manifest in `/usr/local/share/vulkan/icd.d`, where Khronos's Vulkan loader looks |

It replaces what 0.3's per-component scripts installed. To remove it:
`sudo sh /Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle/Contents/Resources/uninstall.sh` (the kext stays on the EFI).

The user-space driver is one bundle, `AMDRadeonRDNA4MTLDriver.bundle`, installed in `/Library/GPUBundles`, the only place
Metal loads a third-party driver from:

| In `Contents/` | What |
|----------------|------|
| `MacOS/AMDRadeonRDNA4MTLDriver` | the Metal driver; the kext names it to Metal (`MetalPluginName`) |
| `Frameworks/AMDRadeonRDNA4.framework` | `librdna4`, the kext's runtime client, with its headers and the kext ABI it was built for |
| `Frameworks/libvulkan_radeon.dylib` | RADV; `Resources/radeon_icd.json` is its manifest for Khronos's Vulkan loader |
| `Helpers/` | the shader translator `air2amdgpu`, `rdna4-run`, `rdna4-iop`, `rdna4-logd` |

Everything stays inside the bundle: WindowServer's sandbox does not let it map libraries from `/Library/Frameworks`. Metal
loads a bundle from `/Library/GPUBundles` only with SIP's filesystem protection off (`csr-active-config` bit 1). The kext
and the bundle must come from the same release (0.4.0 renamed the bundle from `AMDRadeonRDNA4Metal.bundle`).

For development, `sudo tools/metal/native/install.sh <bundle>` replaces just the driver bundle with one from a build.

No boot-args are needed: the driver's defaults (`src/trial.cpp`) are the accelerated desktop, WindowServer and Metal on the driver's
own accelerator. For Apple's paravirtual GPU on the card instead (the desktop before 0.4.0), set `rdna4-accel=0 rdna4-pvgpu=1`.

## Boot arguments

All without a leading dash (`name=1`). The features above are on by default; these turn them off or change them (`=0` turns off any
default).

| Boot-arg | Effect |
|----------|--------|
| `rdna4-off=1` | The plugin hooks nothing; macOS runs its stock framebuffer. |
| `rdna4-modeset=0` | Only the boot mode; no mode switching. |
| `rdna4-head2=0` | No displays beside the boot display. |
| `rdna4-heads=<n>` | How many displays beside the boot display (1-3; default one per connector). |
| `rdna4-head2dp=0` | No displays on DisplayPort beside the boot display (one plugged in after boot gets no accelerated display). |
| `rdna4-hotplug=0` | No hot-plug of the second display (`1`: only log its pin). |
| `rdna4-syssleep=0` | macOS refuses system sleep (as for any unsupported card). |
| `rdna4-nosleep=1` | Display sleep does nothing (the screen stays on). |
| `rdna4-noedid=1` | No EDID read (for a monitor that misbehaves on DDC). |
| `rdna4-boot4k=0` | An HDMI boot display behind a passive DP++ adapter stays at what the firmware lit. |
| `rdna4-dpdual-khz=<kHz>` | The TMDS clock a passive DP++ adapter is taken to carry. |
| `rdna4-accel=0` | WindowServer not on the driver's own accelerator (default 4; after a boot where it did not survive, the next boot leaves it out). |
| `rdna4-accelgl=0` | OpenGL in software on the driver's own accelerator (default 1: Apple's GL-on-Metal renderer on the driver's Metal device; `sysctl debug.rdna4_accelgl` switches it for apps started afterwards). |
| `rdna4-webkitgpu=0` | Leave Safari's sandboxed GPU process to the system policy (default on: it is allowed to load the driver bundle, so WebGL runs on the card). |
| `rdna4-compute=<0-7>` | The compute bring-up stage (default 7, with the runtime; 3: the power-management firmware and the display clocks only; `0` off). |
| `rdna4-gfx=0`, `rdna4-vm=0` | No graphics ring; no address space per client (both needed for Metal). |
| `rdna4-pvgpu=1` | The paravirtual GPU on the card (`make PVGPU=1` builds): Metal, OpenGL, Vulkan through MoltenVK (with `rdna4-accel=0`). |
| `rdna4-pvgpu-disp=<0-7>` | Which of the displays are the paravirtual GPU's (default 7: all; WindowServer composites on the card). |
| `rdna4-pvgpu-delay=<s>` | Publish the paravirtual GPU this many seconds after the plugin starts (default 5). |
| `rdna4-pvcursor=0` | No hardware cursor (WindowServer draws the pointer into each frame). |
| `rdna4-pvgl=1` | OpenGL on the paravirtual GPU's device (default off: on the native desktop OpenGL runs on the driver's own accelerator, `rdna4-accelgl`). |
| `rdna4-pvmac2=1` | The paravirtual device reports Mac GPU family 2: WebGL in Safari (Chrome's does not run yet). |
| `rdna4-pvscaled=0` | No scaled modes larger than the monitor. |
| `rdna4-pvfast=0` | Each size at the refresh nearest 60 Hz, not its fastest. |
| `rdna4-dmubload=0`, `rdna4-dcninit=0`, `rdna4-coldboot=1` | The firmware's display firmware, not the kext's own; the display engine as the firmware left it, not started from nothing; a real cold start from a reset card. |
| `rdna4-dispclk=<MHz>`, `rdna4-dcfclk=<MHz>`, `rdna4-uclk=<MHz>`, `rdna4-fclk=<MHz>` | The clock minimums asked of the power-management firmware (`rdna4-dispclk=0`: clocks left alone, no 4K). |
| `rdna4-vmshared=<0-2>`, `rdna4-hang=0` | Where compute jobs run (2, the default: address spaces from a pool, one per app; 1: two shared kernel queues and eight address spaces); recovery from a hung job off. |
| `rdna4-client-policy=root` | The compute runtime's client for root only. By default also for the logged-in user and WindowServer (the native Metal device's apps): only in a GPU address space of its own (`rdna4-vm=1`) and without the display, sensor and power calls. |
| `rdna4-nativews=0`, `rdna4-nativeheld=0` | With the paravirtual GPU: Apple's Metal plug-in for WindowServer (default 3: the driver's own); WindowServer's targets not held in VRAM. |
| `rdna4-cursor=1` | A hardware cursor without the paravirtual GPU (through Apple's framebuffer driver). |
| `rdna4-trace=1` | Log every request Apple's framebuffer driver makes of ours. |
| `-rdna4dbg` | Lilu's debug logging for the plugin (Lilu DEBUG builds). |

Diagnostics and experiments (`rdna4-modedump`, `rdna4-dmubping`, `rdna4-dmubhist`, `rdna4-dmubver`, `rdna4-smuping`,
`rdna4-ihdump`, `rdna4-ih`, `rdna4-pspdump`, `rdna4-dptrain`, `rdna4-wakediff`, `rdna4-clocks`, `rdna4-coldprobe`,
`rdna4-lutbypass`, `rdna4-8bpc`, `rdna4-pm`, `rdna4-sleeptest`, `rdna4-gfx*`, `rdna4-vm-*`, `rdna4-cursor*`,
`rdna4-pvgpu-class`, `rdna4-pvgpu-match`, `rdna4-accelcensus`, ...) are described where they are used in `src/` and
in `docs/` ([docs/index.md](docs/index.md)).

## How it works

On a card without a macOS driver, macOS drives the firmware's framebuffer with Apple's generic `IONDRVFramebuffer` and
its boot NDRV, which knows one mode and nothing of the monitor. Every request that driver makes of its NDRV goes
through `IONDRVFramebuffer::doDriverIO`.

1. **Hook** (`src/plugin.cpp`): Lilu routes `doDriverIO`; framebuffers on a Navi 48 card are answered by the kext,
   everything else by Apple's code. (A standalone `IOFramebuffer` kext cannot load: its framework lives in the system
   kernel collection, OpenCore injects into the boot one, and Tahoe closes `/Library/Extensions` to ad-hoc signed
   kexts.)
2. **Device** (`src/device.cpp`): BAR5 registers, the VBIOS, IP discovery, the pipe the firmware lit, the monitors'
   EDIDs, the mode table.
3. **Answers** (`src/ndrv.cpp`): modes, timings, connection state, EDID and display power are answered from the kext's
   state; Apple's code keeps the IOFramebuffer side (mode caching, console, power management).
4. **Displays**: mode switches, extra pipes, link training and sleep go to the display engine, mostly through the
   display microcontroller's (DMUB) commands, as `amdgpu` does them (`src/modeset.cpp`, `src/pipe2.cpp`,
   `src/dptrain.cpp`).
5. **Compute** (`src/compute.cpp`, `src/runtime.cpp`): the PSP loads the firmware, then the SMU, the graphics and
   compute engines, the copy engines and the GPU's page tables come up.
6. **Metal** (`make PVGPU=1`): by default the kext publishes its own accelerator (`src/rdna4accel.cpp`,
   `src/ioaccel_thunks.cpp`) and names its Metal driver bundle to Metal, and WindowServer and apps run on it, with the
   command stream submitted through the kext's runtime (`src/n48nkext.cpp`) and shaders translated from AIR by
   `air2amdgpu`. With `rdna4-accel=0 rdna4-pvgpu=1` the kext instead plays host to Apple's `AppleParavirtGPU`
   (`src/pvgpu.cpp`), running its command stream on the card. Either way the displays are scanned out by the card's
   display engine (`src/pvscanout.hpp`).

What each layer found is published in the registry (`AtomBIOS,*`, `Discovery,*`, `Pipe,*`, `Modes,Count`,
`VRAM,TotalMB`, ...), visible with `ioreg`.

## Compute runtime

With `rdna4-compute=7` the kext publishes `RDNA4ComputeService`, and programs run as root can run gfx1201 kernels as
clang builds them (`clang -target amdgcn-amd-amdhsa -mcpu=gfx1201`, then `ld.lld -shared`):

```sh
sudo rdna4-run info                   # stage, heap, the client's address space
sudo rdna4-run selftest               # kernels with LDS and barriers, every result checked
sudo rdna4-run bench                  # transfers, VRAM bandwidth, SGEMM and WMMA against the CPU
sudo rdna4-run sensors                # temperature, power, clocks
sudo rdna4-run load k.hsaco my_kernel # load a code object, describe a kernel
```

From C, `userspace/rdna4-run/librdna4.h`:

```c
rdna4_t gpu;              rdna4_open(&gpu);
rdna4_buffer_t a;         rdna4_alloc(&gpu, n * 4, &a);          // a.gpu -> kernargs
rdna4_write(&gpu, &a, 0, host, n * 4);
rdna4_program_t k;        rdna4_load(&gpu, elf, elfBytes, "vadd", &k);
uint64_t args[3] = { a.gpu, b.gpu, c.gpu };
uint32_t groups[3] = { n / 64, 1, 1 }, size[3] = { 64, 1, 1 };
rdna4_dispatch(&gpu, &k, groups, size, args, sizeof args, 1000, NULL);
rdna4_read(&gpu, &c, 0, host, n * 4);
rdna4_close(&gpu);
```

Kernels get their kernargs and up to 64 KiB of LDS a work-group; kernels that need scratch memory or dispatch and queue
pointers are refused at load, with the reason in the kernel log.

## Building

Clone with the submodules (`git clone --recursive`, or `git submodule update --init`; they are in `lib/`).

```sh
tools/build/fetch-firmware.sh   # once: AMD's firmware for the compute bring-up (both chips' sets), into firmware/amdgpu/
make                            # the kext and the programs (x86_64, macOS 11 and later)
make PVGPU=1                    # the Metal path: the driver's own Metal driver by default (OpenGL, Vulkan via MoltenVK), paravirtual with rdna4-pvgpu=1
make PVGPU=1 metal-helper       # the shader translator for it
make mesa && make vkcube        # RADV and Khronos's vkcube
make metal-driver               # AMDRadeonRDNA4MTLDriver.bundle: the Metal driver with librdna4, RADV and the helpers
make package                    # AMDRadeonRDNA4-<version>.pkg: the bundle and its daemons in one installer
make CONFIG=DEBUG               # unoptimised, with DEBUG defined
make test                       # host-side tests: parsers, EDID fixtures, the display engines' plans against Linux's
make clean                      # what make built (make distclean: all of Build/)
```

The results go to `Build/AMDRadeonRDNA4-<version>-<RELEASE|DEBUG>/` (the kext, the driver bundle, and `bin/` with the programs);
`make product-dir` prints that directory. The `Makefile` works with the GNU make 3.81 macOS ships and with BSD make;
`AMDRadeonRDNA4.xcodeproj` is a front for it.

On **Linux / WSL**, with an [osxcross](https://github.com/tpoechtrager/osxcross) toolchain made from a macOS SDK
(Apple's ld64 is needed for kext bundles): `tools/build/build-osxcross.sh`. `make test` needs only clang.

## Testing in a VM

`VMTEST=1` builds also take QEMU's `vmware-svga` as theirs (no register work, a fixture EDID, mode switches resize the
VM display), for an [OSX-KVM](https://github.com/kholia/OSX-KVM) guest:

```sh
tools/build/build-osxcross.sh VMTEST=1
tools/vm/vm-opencore.sh --kext "$(make -s product-dir VMTEST=1)"/AMDRadeonRDNA4.kext --lilu ~/kexts/Lilu.kext
```

`emu/` holds a QEMU model of the RX 9070 XT (its PCI identity, BARs, the register state the GOP leaves, live engines
for what the kext polls, a GOP driver in its option ROM), so the build for the real card runs its own code paths in
the VM: `tools/vm/emu-build.sh`, then `tools/vm/emu-boot.sh`. The QEMU device is GPL-2.0-or-later and the GOP driver
BSD-2-Clause-Patent; the rest of the repository is BSD-3-Clause.

## Documentation

What each directory and source file is: [docs/index.md](docs/index.md). The design notes and measurements of the
paravirtual GPU are in `docs/m0-pvgpu.md` and `docs/m1-stream.md`, the native Metal driver's in
`docs/native-metal-spike.md`. The licences of what a release ships: [docs/licenses.md](docs/licenses.md).
