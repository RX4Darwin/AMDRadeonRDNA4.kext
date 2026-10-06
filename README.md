# RDNA4FB.kext

A **display driver** for the AMD Radeon **RX 9070 XT** (Navi 48 / RDNA 4,
PCI `0x1002:0x7550`) on x86_64 Hackintosh, built as a **Lilu plugin** against
MacKernelSDK and injected by OpenCore. Cross-compiles on Apple Silicon and on
Linux/WSL. No 3D/Metal acceleration (see Scope below); rendering is software.

**Status.** The Lilu plugin is verified end to end in a macOS Tahoe 26 VM
(QEMU/OSX-KVM): it loads from OpenCore, takes over the display requests of
Apple's generic framebuffer driver, and System Settings → Displays shows the
monitor by name with its EDID resolutions; switching between them resizes the
VM display. On real hardware, the hardware side of this code (carried over
unchanged from the earlier standalone build) was verified on Big Sur 11.7.10:
4K desktop with correct colors, EDID over DP AUX and HDMI DDC, display sleep,
BAR5 register MMIO. The plugin build runs on the card under Big Sur 11.6.6
(2026-10-03/04): desktop on a DP or an HDMI boot display, a resolution change
on either, and **two displays at once** (2026-10-04): a 4K DisplayPort
boot display plus a second desktop on a 1080p HDMI monitor whose pipe the
kext lights itself (`docs/second-pipe.md`). Since 2026-10-06 that needs no
boot-arg: a second display on either HDMI connector, mode switching on
both displays and hot-plug of the second are on by default, each with a
boot-arg to turn it off (below).

> **Why a Lilu plugin?** The earlier build was a standalone `IOFramebuffer`
> subclass. That links against `com.apple.iokit.IOGraphicsFamily`, whose code
> on macOS 11+ exists only inside the *System* kernel collection — OpenCore
> injects into the *Boot* collection, so the kext was silently dropped. The
> fallback, `/Library/Extensions` and the auxiliary collection, is closed to
> ad-hoc-signed kexts on Tahoe (`syspolicyd` blocks them with no approval
> button). Instead, RDNA4FB now extends the driver macOS already runs on an
> unsupported GPU — Apple's `IONDRVFramebuffer` with its boot NDRV — at
> runtime through Lilu, the way WhateverGreen and NootRX do. It is injected by
> OpenCore after Lilu, like any other plugin.

> **Scope, honestly.** macOS has *no* driver for RDNA 3 or RDNA 4 — Apple's AMD
> support ends at RDNA 2 (Navi 2x), and spoofing is impossible (the RDNA 2
> driver would emit command streams Navi 48 can't decode). This project
> **started** as pure framebuffer adoption — take the scanout OpenCore's GOP
> already programmed and hand it to WindowServer, Linux `efifb`/`simpledrm`
> style. It has outgrown that description: the driver now actively operates
> the display controller — AUX and DDC-I2C engines for EDID, DPCD sink power
> and stream blanking for display sleep, discovery-derived register
> addressing — all ported piecewise from `amdgpu`'s display core (DC), which
> is this project's reference and upstream in spirit. Think of it as an
> early, hand-rolled KMS driver growing toward native mode setting.
> **What it still is not:** a GPU accelerator. 3D/Metal needs the GFX12
> command processor, ring buffers, memory management and a Metal userland —
> a much larger project, acknowledged as the long-term goal rather than
> disclaimed. Much of the groundwork here (IP-discovery-driven registers,
> AtomBIOS/EDID/timing parsers with host-side tests) is deliberately
> ASIC-portable and would carry over to other RDNA 4 cards with little more
> than wider PCI matching.

## How it works

On a GPU without a macOS driver, macOS drives the GOP framebuffer with Apple's
generic **`IONDRVFramebuffer`** (`com.apple.iokit.IONDRVSupport`) and its
built-in boot NDRV (`IOBootNDRV`), which knows one fixed mode and nothing about
the monitor. Every request that driver makes of its NDRV — Initialize, Open and
each `csc` Control/Status call — goes through `IONDRVFramebuffer::doDriverIO`.

1. **Hook** (`src/plugin.cpp`): Lilu routes `doDriverIO`. Framebuffers whose
   PCI device is the Navi 48 family (`0x7550` RX 9070/9070 XT, `0x7551`
   R9700) are ours; everything else goes straight to Apple's code.
2. **Device** (`src/device.cpp`): when Apple's driver opens our framebuffer,
   RDNA4FB reads the console geometry, maps BAR5, loads the VBIOS and IP
   discovery, finds the pipe the GOP lit, reads the sink's EDID and builds the
   mode table. **Hard-won detail:** `v_baseAddr` carries flag bits in its low
   bits (this machine reads `0x840000001`); they must be masked off. Passing
   the raw value shifts WindowServer's writes one byte from the true scanout
   base, which recolours every pixel (R'=G, G'=B, B'=previous pixel's alpha) —
   it presents as "inverted colors with a blue cast" and cost a week of DCN
   register archaeology to trace.
3. **Answers** (`src/ndrv.cpp`): the mode list, per-mode video parameters and
   timings, the current mode, the connection flags, the EDID blocks and DPMS
   (`cscSetSync`) are answered from RDNA4FB's state, record for record the
   way `IOBootNDRV` answers the same requests. The boot mode keeps
   `IOBootNDRV`'s ID 100; the other modes get 100 + their mode-table ID.
   Everything else (gamma, CLUT, power states, cursor) stays with
   `IOBootNDRV`, and Apple's code keeps doing the IOFramebuffer side: mode
   caching, pixel formats, aperture mapping, console, power management.
4. **Mode switches** go to a backend. Every mode keeps the boot surface's
   memory and pitch and is no larger than it, so a switch only moves the
   timing and viewport. On the card an HDMI (TMDS) boot display is switched
   by the mode-set engine (`src/modeset.cpp`: pixel clock and transmitter
   through DMUB, then OTG timing and viewport), verified 2026-10-04. A DP
   boot display is retimed on its trained link (pixel-rate DTO, timing,
   MSA; no link training, so nothing above the boot pixel clock), verified
   2026-10-04. In `VMTEST` builds QEMU's display is resized through its Bochs VBE
   interface.

The kext also publishes what each bring-up layer found as registry properties
on the framebuffer (`AtomBIOS,*`, `Discovery,*`, `Console,*`, `Pipe,*`,
`Modes,Count`, `VRAM,TotalMB`, `MMIO,Verified`), visible in `ioreg` without
kernel logs.

## Boot arguments

All parsed without a leading dash (`name=1`, not `-name=1`):

| Boot-arg | Effect |
|----------|--------|
| `rdna4-off=1` | Kill switch: the plugin does not hook anything and macOS runs its stock fallback framebuffer. Lilu's `-liluoff` disables all plugins. |
| `rdna4-trace=1` | Log every NDRV request for our framebuffer and who answered it (`rdna4` or `boot`), up to 400 lines. On by default in `VMTEST` builds. Mode switches are always logged. |
| `rdna4-modeset=0` | **Mode switching is on by default; `0` turns it off** (the boot mode alone is offered). On, the kext offers the sink's EDID modes (DTDs, CTA DTDs/VICs, standard and established timings; ≤ boot framebuffer size, TMDS ≤ 340 MHz or, for a sink whose EDID announces SCDC and a higher rate, ≤ 600 MHz, and ≤ 1.25 × boot pixel clock) instead of the boot mode alone. On an HDMI/DVI boot display a switch runs the mode-set engine (verified on the card 2026-10-04: 1920x1080 to 1600x900 on a 1080p HDMI sink, picture confirmed; the log line `modes: now ..., measured ... Hz` gives the refresh the OTG really runs at). On a DP boot display a switch retimes the stream and leaves the link as the firmware trained it, so only modes at or below the boot pixel clock are offered (verified on the card 2026-10-04: 3840x2160 to 2560x1440 on a 4K DP monitor over four HBR2 lanes, picture confirmed; the log shows `modes: DisplayPort, link untouched: DTO ...` and the measured refresh). Until 2026-10-06 a switch left the plane expecting the blank to end where it does in the boot mode, which cut the top of the picture off in modes whose blank ends earlier (the menu bar at 2560x1440, everything at 1280x720); the switch now sets that for the new mode, and 2560x1440, 1920x1080 and 1280x720 were confirmed whole on the 4K monitor that day. From 340 MHz up the link is HDMI 2.0 scrambled: the encoder's scrambler is switched with the mode and the monitor is told over SCDC, in both directions, so a monitor the GOP lit at 4K60 (533 MHz, scrambled) can be switched down as well. The scrambled path ran on the card 2026-10-05: a monitor the GOP lit at 3840x2160 (533 MHz) switched to 1920x1080@60, to 1920x1080@144 (333 MHz, measured 143.996 Hz) and back to 3840x2160 (`scdc: ... TMDS_CONFIG = 3 written`, measured 59.996 Hz), and showed the desktop again at 3840x2160. |
| `rdna4-head2=0` | **A second display is on by default (level 4, below); `0` turns it off.** `1` is the experiment it grew from: a phantom second head. The plugin creates an `IONDRVDevice` nub just before the framebuffer starts on the GPU, makes the two heads dependents of one controller, stands in for the boot NDRV the second one cannot have, and serves the second sink's EDID with one mode on a spare VRAM surface behind the console. No display register is written and nothing appears on the monitor (its pipe stays dark): it shows whether macOS accepts a second display. Next to `rdna4-compute` since 2026-10-06 (its surface is kept below the compute pool; ran together on the card that day). Verified on the card under Big Sur 11.6.6 (2026-10-04): WindowServer opens both framebuffers and macOS lists the second display. See `docs/second-head-ndrv.md`. **Higher values also light the pipe (`docs/second-pipe.md`; all verified on the card 2026-10-04):** `2` writes nothing and publishes what every step of the plan would do to the dormant pipe's registers (`RDNA4FB,Pipe2`, shown by `tools/diagnostic-log.sh`); `3` lights the stream alone, so the second monitor shows a solid teal-blue; `4` also lights the plane, so it shows the second desktop. There is a plan per connector of the card, all at 1920x1080@60 next to a boot display on another connector: HDMI on HPD3 / link 2 (the one verified), HDMI on HPD4 / link 3 (verified 2026-10-06), and DisplayPort on HPD1 / link 0 or HPD2 / link 1, whose link the kext trains itself (those two host-tested, not yet run on the card, and only used with `rdna4-head2dp=1`). Not in `VMTEST` builds. |
| `rdna4-head2dp=1` | Also serve a second display on a DisplayPort connector (lighting it, sleep, hot-plug and mode switching as on HDMI). Off by default because it has not run on the card yet: `docs/todo-dptest.md` has the tests. |
| `rdna4-hotplug=0` | **Hot-plug of the second display is on by default (level 2); `0` turns it off, `1` only logs what the pin does.** On: The connector's HPD pin is polled; unplugging takes the head offline in macOS, plugging reads the display, lights or wakes the pipe and brings the head back, and the head exists (offline) when no monitor was there at boot. `1` only logs what the pin does. Verified on the card 2026-10-05 (`docs/second-pipe.md`), and on 2026-10-06 on the other HDMI connector and through a 10-minute display sleep. A display unplugged from one HDMI connector and plugged into the other gets the pipe moved to it (verified on the card 2026-10-06, in both directions, at 60 and at 144 Hz). |
| `rdna4-dptrain=1` | Diagnostic for a DisplayPort boot display: log what the monitor can do and which link the firmware trained, watch its HPD pin, and log the link's state when the monitor comes back. The kext does not retrain that link, because the firmware keeps it itself (card, 2026-10-06: it retrains within two seconds of the pin returning). `2` also runs the kext's own training on that link once, 15 s after the desktop is up (`src/dptrain.cpp`, as amdgpu trains): on the card that day, 4 lanes at HBR2 trained and the video was back 0.57 s after the link was taken down. |
| `rdna4-cursor=1` | Hardware cursor on the boot display through the NDRV cursor calls (`src/cursor.cpp`, `docs/cursor-audit.md`, `docs/HANDOFF-linux.md`); used with `rdna4-vbl=1`. Off by default. Ran on the test rig 2026-10-06 on the 4K DisplayPort boot display next to the second display, through mode switches, without a fault in the log; it made no noticeable difference to how responsive the unaccelerated desktop feels. |
| `rdna4-nosleep=1` | Make display sleep a no-op (the screen stays on). Escape hatch if blank/unblank misbehaves. |
| `rdna4-noedid=1` | Skip the EDID probe over AUX/DDC. Use if a sink misbehaves on DDC. |
| `rdna4-lutbypass=1` | Force the MPC MCM stages (shaper/3D LUT/1D LUT) to bypass on all pipes. |
| `rdna4-8bpc=1` | Experiment (DP only): switch the active DP stream 10 bpc → 8 bpc and update the MSA to match (first proven live register write). |
| `rdna4-modedump=1` | Read-only survey of the mode-setting registers: all OTG timings/enables, DIG front/back-ends, HUBP surface addresses, DCCG clock muxes, DMUB status. The lit pipe is the template for further pipe bring-up. |
| `rdna4-dmubping=1` | First contact with the DMUB display firmware: resolve the inbox1 ring through the DMCUB region windows, submit one QUERY_FEATURE_CAPS command, verify RPTR advances. Proves the mailbox route for mode setting. |
| `rdna4-dmubhist=1` | Read-only decode of the GOP's recorded DMUB command ring — the exact VBIOS-family command sequence (encoder/transmitter/pixel-clock) the firmware accepted to light the display, as the template for mode setting. |
| `rdna4-dmubver=1` | Read-only fingerprint of the GOP-loaded DMUB firmware: the full `DMCUB_SCRATCH` bank + boot/enable state (`SCRATCH00` decodes as `dmub_fw_boot_status`), then a scan of VRAM below the region4/mailbox anchor for the fw-meta magic (`0x444D5542` "DMUB") to read the embedded `fw_version`. Compares it against Debian's `dmesg \| grep -i dmub` version (`0x00010300`): a match means the GOP runs the same blob and the mainline VBIOS subtypes 0/1/2 are safe to replay; otherwise the GOP has its own dialect (its DP bring-up used 6/10/12/16) and the amdgpu capture is reference-only. `rdna4-dmubver=2` also asks the firmware for its version over the GPINT register channel (`DMUB_GPINT__GET_FW_VERSION`: one write to `DMCUB_GPINT_DATAIN1`, the reply in `DMCUB_SCRATCH7`); level 1 stays read-only. Not run on the card yet. |
| `rdna4-smuping=1` | Read-only SMU (power-management firmware) handshake: TestMessage + PMFW/interface version queries over the MP1 mailbox. No DPM changes. Publishes `SMU,FirmwareVersion` / `SMU,Verified`. Prerequisite check for future clock control. |
| `rdna4-ihdump=1` | Read-only interrupt-delivery survey: OSSSYS IH ring state, per-OTG vertical-interrupt line config, PCI MSI/MSI-X capability words. Groundwork for real VBL interrupts. |
| `rdna4-ih=1` | Enable the RDNA 4 IH v7 ring and PCI MSI completion path after the runtime's DMA setup. Runtime fences remain authoritative; a missing interrupt source falls back to bounded polling. Default off. |
| `rdna4-pspdump=1` | Read-only PSP (security processor) survey: bootloader/sOS/GPCOM-ring status from the MPASP scratch registers. Publishes `PSP,Alive` / `PSP,SOSVersion`. Decides whether loading fresh firmware (e.g. a current DMUB) is viable. |
| `rdna4-compute=<stage>` | Compute bring-up, written from scratch (`src/compute.cpp`), run after the display is answered for and never with a GPU reset. `1` = read-only survey: GFX (IMU/RLC/PFP/ME/MEC/MES), both SDMA engines, GC and MM hub apertures, PSP, HDP flush remap, and the VRAM window chosen for compute; published as the `Compute,Survey` dictionary. `2` = PSP bring-up on a background thread 5 s after the desktop: the secure-OS components through the PSP bootloader, the GPCOM command ring, `LOAD_TOC`, then the SMU firmware via `LOAD_IP_FW`, proven by the SMU answering its mailbox; published as `Compute,PSP` and `Compute,Stage`. `3` = the 19 GC firmware images through the PSP and the RLC autoload (`Compute,GFX`). `4` = GC hub for VMID0, SDMA0 queue, a WRITE and a 1 MiB fill checked by the CPU (`Compute,SDMA`). `5` = a MEC compute queue programmed directly (no MES), fed through its doorbell, running PM4: the scratch-register ring test, WRITE_DATA and a RELEASE_MEM fence (`Compute,MEC`). `6` = a real gfx1201 kernel (`shaders/probe.s`) dispatched on the compute units, all 256 results checked (`Compute,Dispatch`). `7` = a clang-built kernel (`shaders/vadd.cl`) launched from its AMDGPU code object: the loader (`src/codeobj.cpp`) finds the kernel and its descriptor, the dispatch uses the compiled RSRC1/2/3 and a kernarg buffer, and 4096 results of `c = a + 3b` are checked (`Compute,Kernel`). After stage 6 or 7 the runtime is published for user space (see *User-space compute*). Each step leaves a breadcrumb in NVRAM (`4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trail`) so a hang names the step. Needs the linux-firmware blobs in `firmware/amdgpu/` at build time (`tools/fetch-firmware.sh`). |
| `rdna4-fakeedid=0` | `VMTEST=1` builds only: they serve the Lenovo fixture EDID and enable `rdna4-modeset` by default (so the mode list can be checked in a VM whose OpenCore pins boot-args); `=0` turns either off. |
| `-rdna4dbg` | Lilu debug logging for the plugin (Lilu DEBUG builds). |

The standalone build's `rdna4-hwcursor`, `rdna4-curmode`, `rdna4-curtest`,
`rdna4-dmubcursor` and `rdna4-cmap` belonged to its IOFramebuffer glue and are
gone; the hardware cursor now goes through the NDRV cursor calls
(`rdna4-cursor=1`, above).

### The lit pipe

At start the driver identifies the pipe the GOP lit instead of assuming
OTG0/DP0 (`src/pipe.cpp`): the running OTG, the DIG front-end sourcing it
(`DIG_SOURCE_SELECT`), its back-end/PHY (`STREAM_MAPPER_CONTROL`) and HPD, the
OPP/HUBP feeding it, and the signal type from `DIG_BE_MODE` (DP or HDMI
TMDS). The boot timing is read back from the OTG images; its pixel clock —
which for TMDS lives in the PHY PLL, not in any register — is measured from
the OTG frame counter and snapped to the matching EDID timing. Published as
`Pipe,*` (`Pipe,Signal`, `Pipe,BootMode`, `Pipe,MeasuredPixelClockKHz`, …).
Display power and the diagnostics use this pipe. On an HDMI boot display,
display sleep blanks through the OPP's pattern generator (solid black) instead
of toggling a DP stream; with mode switching on (the default) it takes the link down so the
monitor really sleeps, and wakes it with a mode set to the running timing
(verified on the card 2026-10-06: a monitor the firmware lit at 3840x2160,
533 MHz scrambled, went to standby and came back sharp).

## Files

| File | Purpose |
|------|---------|
| `src/plugin.cpp` | Lilu plugin entry: routes `IONDRVFramebuffer::doDriverIO`, recognises our framebuffers, creates the device state, hands NDRV requests to the translator. Checks the local NDRV record mirrors against the SDK at compile time. |
| `src/device.{hpp,cpp}` | `RDNA4Device`: scanout adoption, BAR5 MMIO, VBIOS/IP discovery, lit pipe, AUX/DC_I2C engines and EDID, DMUB ring, SMU/PSP/IH diagnostics, display power, mode table. |
| `src/compute.{hpp,cpp}` | `RDNA4Compute`: staged compute bring-up next to the display path (`rdna4-compute=<stage>`): survey, PSP and SMU firmware, GC firmware and RLC autoload, GC hub and SDMA, a MEC queue, kernel dispatch. |
| `src/gfxregs.hpp` | Register map of the compute side (GC 12.0.1, SDMA 7, GC/MM hubs, NBIF HDP remap, PSP and SMU mailboxes) as segment + dword offset pairs. |
| `src/amdfw.{hpp,cpp}` | Freestanding parser for AMD firmware containers: common header, LOAD_IP_FW payloads, the PSP secure-OS package. Host-tested against the real blobs. |
| `src/psp.{hpp,cpp}` | Freestanding PSP driver: bootloader component loading, GPCOM ring, command submission with fences, LOAD_TOC, LOAD_IP_FW. Host-tested against a simulated PSP. |
| `src/fwblobs.S` | Embeds the PSP sOS, SMU, SDMA and GC 12.0.1 firmware from `firmware/amdgpu/` (linux-firmware, AMD redistributable license) into the kext. |
| `src/sdma.{hpp,cpp}` | SDMA 7 packet builders (WRITE, FENCE, CONST_FILL, COPY) and ring writer. Host-tested. |
| `src/ih.{hpp,cpp}`, `src/ihdecode.cpp` | IH v7 ring setup, PCI MSI filter/action, bounded fence waits, vector decode and source accounting. Host-tested decode and ring arithmetic. |
| `src/pm4.{hpp,cpp}` | PM4 type-3 packet builders (SET_UCONFIG_REG, SET_SH_REG, WRITE_DATA, ACQUIRE/RELEASE_MEM, DISPATCH_DIRECT) and compute queue writer. Host-tested. |
| `src/codeobj.{hpp,cpp}` | Freestanding AMDGPU code-object reader: a kernel's descriptor (RSRC1/2/3, kernarg size, SGPR requests) and the loadable image (PT_LOAD segments at their virtual addresses). Host-tested against clang's output. |
| `src/runtime.cpp`, `src/gpuheap.{hpp,cpp}` | The user-space compute runtime: a 4 KiB-granule VRAM heap, buffers and programs per connection, synchronous dispatches on the MEC queue, hung-queue detection. The heap is host-tested. |
| `src/userclient.{hpp,cpp}` | `RDNA4ComputeService` (published once stage 6/7 finished) and `RDNA4ComputeClient`, the IOUserClient that checks each call and hands it to the runtime. Root only. |
| `include/rdna4compute.h` | The user-space ABI: selectors, scalar/struct shapes, the dispatch struct. Shared by the kext and user space. |
| `shaders/bench.cl`, `src/bench_codeobj.h` | `lds_reverse` (LDS + barrier test), `copy` (VRAM bandwidth), a tiled `sgemm` (64x64 tiles through LDS), the matrix units (`wmma16`, `hgemm`, `bf16gemm`), and the affine/scaled `mandelbrot` and `mandelbrot_zoom` kernels, one clang-built code object used by `rdna4-run`. |
| `userspace/librdna4.{h,c}`, `userspace/rdna4-run.c` | C library over the user client, including synchronous and queued Present/Restore display ownership, and `rdna4-run` (`info`, `selftest`, `bench`, `show`, `anim`, `load`). Built with the kext (`build/rdna4-run`). |
| `shaders/probe.s`, `src/probe_kernel.h` | The stage-6 test kernel (gfx1201 assembly) and its machine code, generated by `tools/build-shaders.sh` with upstream LLVM. |
| `src/ndrv.{hpp,cpp}` | Freestanding NDRV `csc` translator: mode list, video parameters, timings, current mode, connection, EDID blocks, DPMS, mode switch. Host-tested. |
| `src/bochsvbe.{hpp,cpp}` | `VMTEST` only: mode switches on QEMU's `vmware-svga` through the Bochs VBE interface (the one OVMF's GOP uses on that card). |
| `src/cursor.cpp` | The DCN hardware cursor behind the NDRV cursor calls (`rdna4-cursor=1`). |
| `src/atombios.{hpp,cpp}` | Freestanding, bounds-checked AtomBIOS parser: data tables (connectors, GPIO LUT, firmwareinfo) + command-function directory. |
| `src/ipdiscovery.{hpp,cpp}` | Parser for AMD's IP discovery binary — per-card IP versions and register segment bases (what amdgpu uses instead of hardcoded offsets; the key to ASIC portability). |
| `src/edid.{hpp,cpp}` | EDID parsers: base block (all descriptors, range limits, established/standard timings via a DMT table, physical size) and CTA-861 extension (DTDs, VICs via a CEA-861 table, HDMI VSDB). |
| `src/modes.{hpp,cpp}` | EDID → deduplicated, filtered, deterministically ordered display-mode table with stable IDs. |
| `src/pipe.{hpp,cpp}` | Lit-pipe discovery (OTG/DIG/link/OPP/HUBP, DP vs HDMI) and OTG-image → timing inversion. |
| `src/modeset.{hpp,cpp}` | The mode-switch plan for the lit pipe: HDMI (PLL and transmitter through DMUB, scrambling above 340 MHz) and DisplayPort (stream retimed on the trained link). |
| `src/pipe2.{hpp,cpp}`, `src/pipe2_linux*.inc` | The plan that lights a second pipe for the second head; the register tables are generated, one per connector: two HDMI, two DisplayPort (`docs/second-pipe.md`). |
| `src/dmub.hpp` | DMUB ring command ABI and builders for the VBIOS-family commands (transmitter control v1.7, set pixel clock v1.7, DIG encoder stream setup v1.5). |
| `src/otgtiming.{hpp,cpp}` | EDID timing → DCN OTG register images, per amdgpu's `optc1_program_timing` (mode-set groundwork). |
| `src/kmod_info.c` | kmod glue pointing at Lilu's plugin start/stop. |
| `Lilu/` | Lilu submodule (v1.7.3), for the plugin headers and `plugin_start.cpp`. |
| `tools/atomdump.cpp` | Host test harness: parsers against the real ROM and captured EDID fixtures, pipe discovery, DMUB payloads, the NDRV translator (`make test`). |
| `tools/vm-opencore.sh`, `tools/vm-ocplist.py` | Build a development OpenCore disk for an OSX-KVM VM with RDNA4FB injected after Lilu. |
| `tools/logs-ssh.sh` | Pull the RDNA4FB kernel log and registry properties from a macOS machine over SSH. |
| `src/dptrain.{hpp,cpp}` | DisplayPort link training (clock recovery, channel equalisation, retries), as amdgpu does it; everything the hardware and the monitor do comes through callbacks. Host-tested against `tools/dp_train_linux.inc`. |
| `src/dpphy.{hpp,cpp}` | What training asks of DCN 4.01: the link encoder registers behind each training pattern and the DMUB transmitter commands (enable, lane drive, disable). Host-tested against the same reference. |
| `src/n48n.{hpp,cpp}`, `src/vmtree.{hpp,cpp}` | The kernel half of the Vulkan driver's interface: one client's buffers, mappings, contexts, submitted work (what a command buffer may be, sequences, waits, when work counts as lost) and the display it may show pictures on, and its four-level page table. Freestanding. Host-tested, and run under the real driver with `tools/n48n-host/`. |
| `src/n48nkext.cpp`, `include/rdna4vulkan.h` | That engine on the card: a user client of type `'N48N'` on the compute service (root only, one at a time), buffers from the runtime's VRAM heaps and from system memory, the client's page table in VRAM, its address space (number 8), its work on the kernel's graphics ring, its pictures on the boot display through the runtime's flip, and a copy test that has the GPU's copy engine work in that address space. Needs `rdna4-compute=7`, and `rdna4-gfx=2` to submit work; not with `rdna4-vm=1`. **Verified on the card** (2026-10-06): the memory half and the copy test (`vulkan/n48nprobe.c`: all ok), and the driver's work through submit and wait: a fill by the command processor, one by a compute shader, and a triangle rendered into an image and read back (`vulkan/vkprobe.c`: done, all ok). and a picture on the boot display: a moving triangle for five seconds at 60 frames a second, the desktop back afterwards (`vkprobe ... show`) (`docs/vulkan-port.md` sections 8 to 10, `docs/todo-vulkantest.md`). |
| `tools/n48n-host/` | Runs the real RADV driver, and the card test's program, on that engine on a Mac, with no kext: a library that answers their IOKit calls with it (`run.sh`). |
| `vulkan/` | The RADV (Mesa Vulkan) port to macOS for this card, from Navi48-MacOS: five patches and the kernel interface they expect, two small patches of this repository's, a build script, and `n48nprobe.c`, the card test of the interface's memory half (`make n48nprobe`). On the card (2026-10-06) `n48nprobe` passes and the driver creates its device, runs two fills and renders a triangle through the kext, read back right (`vkprobe`: done, all ok). `vkprobe ... show` puts a picture on the boot display, 60 frames a second, and gives the desktop back (`docs/vulkan-port.md`). |
| `tools/cgmode.py` | On the test machine: list a display's modes and switch to any of them for the login session (Displays preferences hides most modes of a monitor it takes for a television). |
| `tools/pipegen/` | Runs Linux's own DCN 4.01 display code on the host against a recorder: generates `src/pipe2_linux*.inc` and the DisplayPort references `tools/dp_retime_linux.inc` (a mode switch) and `tools/dp_train_linux.inc` (link training against simulated monitors) (`run.sh <linux tree>`). |
| `tools/linux-capture.sh` | Ground-truth capture of amdgpu's display programming on Linux, for the mode-set engine. |
| `Info.plist` | Lilu plugin personality (`IOResources`), OSBundleLibraries (Lilu, IOPCIFamily, KPIs). |
| `Makefile` | Cross-compiles x86_64 on any host, assembles the `.kext`. |

## User-space compute (`rdna4-run`, `librdna4`)

With `rdna4-compute=6` or `7`, once the bring-up finishes the kext publishes
`RDNA4ComputeService` and any root process can run gfx1201 kernels on the
card. The model is HSA's, kept synchronous: allocate VRAM buffers (their GPU
addresses go into kernel arguments), copy data in and out, load a code
object as clang builds it (`clang -target amdgcn-amd-amdhsa -mcpu=gfx1201`
then `ld.lld -shared`), dispatch a kernel over a 3-D grid and wait for it.
Buffers and programs belong to the connection and are freed when it closes.

```sh
sudo build/rdna4-run info                     # stage, heap size and free space
sudo build/rdna4-run selftest [items]         # vadd and an LDS/barrier kernel, every result checked
sudo build/rdna4-run bench [small]            # host<->GPU MB/s, VRAM GB/s, SGEMM GFLOPS, each vs the
                                              # CPU (memcpy; Accelerate's cblas_sgemm, weak-linked),
                                              # then FP16/BF16 GEMM on the matrix units (WMMA)
sudo build/rdna4-run show [seconds]           # render a Mandelbrot into a device buffer, present it,
                                              # CPU-check 64 spread pixels, then restore the desktop;
                                              # VM runs use scale 4 and real hardware uses scale 1
sudo build/rdna4-run anim [seconds]          # double-buffered zoom with async presents and FPS stats
sudo build/rdna4-run load k.hsaco my_kernel   # load a code object, describe a kernel
```

`rdna4_display_query`, `rdna4_present`, `rdna4_present_async`,
`rdna4_wait_present` and `rdna4_restore` expose the display surface through
appended runtime selectors without changing ABI 4. Present accepts a
256-byte-aligned offset into a device-heap ARGB8888 buffer. One connection owns
the presentation at a time; two asynchronous presents may be pending, and
`WaitPresent` reports the OTG frame that latched each one. An explicit Restore,
client close, buffer free, or the 30-second idle timeout returns the recorded
desktop surface. The flip hardware operations are serialized with the runtime
lock and do not write boot trails after bring-up.

From C, `userspace/librdna4.h`:

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

Limits for now:
- Kernels get the kernarg pointer and up to 64 KiB of LDS per work-group
  (the kernel's own plus `dynamicLdsBytes` in the dispatch, ABI 2). Kernels
  that need dispatch/queue pointers or scratch are refused at load, with the
  reason in the kernel log.
- Hidden (implicit) arguments are zero unless the caller writes them.
- Transfers: once its self-test passes, the runtime copies with the GPU's
  own copy engine (SDMA) through a pinned 16 MiB bounce buffer in host
  memory, which the GC hub's AGP aperture maps (amdgpu's layout; the
  aperture ends at the buffer). That needs PCI bus mastering, enabled for the
  compute path only; the self-test has the GPU read host memory first, so a
  wrong mapping costs a mismatch, never a stray write. Buffers then come from
  VRAM past the BAR (half of VRAM, up to 8 GiB). Without DMA, the CPU copies
  through the BAR and buffers share the 96 MiB CPU-visible heap.
- Every kernel runs in VMID0: it can reach all of VRAM, hence root only.
- A dispatch that misses its timeout marks the runtime wedged until reboot,
  because there is no queue reset yet.

## Building

Clone with the submodule (`git clone --recursive`, or
`git submodule update --init` in an existing checkout).

```sh
make            # -> build/RDNA4FB.kext  (x86_64, min macOS 11)
make test       # host-side tests: parsers against the real ROM and EDID
                # fixtures, pipe discovery, DMUB payloads, NDRV translator
make clean
```

Verify the output:

```sh
file build/RDNA4FB.kext/Contents/MacOS/RDNA4FB   # Mach-O 64-bit kext bundle x86_64
```

### Building on Linux / WSL (osxcross)

LLVM's `ld64.lld` cannot emit kext bundles, but Apple's ld64 can: build an
[osxcross](https://github.com/tpoechtrager/osxcross) toolchain from a macOS SDK
(the "Command Line Tools for Xcode 26.x" `.dmg` carries one, including
`libkmodc++.a`), then:

```sh
tools/build-osxcross.sh            # -> build/RDNA4FB.kext
tools/build-osxcross.sh VMTEST=1   # -> build-vmtest/RDNA4FB.kext
make test                          # host tests need only clang++
```

On Windows checkouts without symlink support, Lilu's header symlinks become
text files; the Makefile copies the real headers into `build*/liluhdr` first.

## Installing (OpenCore)

1. Use **Lilu 1.7.1 or newer** (older Lilu disables itself on macOS 26).
2. Copy `build/RDNA4FB.kext` to `EFI/OC/Kexts/` and add a `Kernel → Add`
   entry **after Lilu**:

   | Key | Value |
   |-----|-------|
   | `BundlePath` | `RDNA4FB.kext` |
   | `ExecutablePath` | `Contents/MacOS/RDNA4FB` |
   | `PlistPath` | `Contents/Info.plist` |
   | `Arch` | `x86_64` |
   | `Enabled` | `true` |

   No code signing, SIP change or kext approval is involved: OpenCore
   injects it into the boot collection like Lilu itself.
3. Recommended while bringing this up: `-v keepsyms=1 rdna4-trace=1`
   boot-args, and disable other GPU-related kexts (WhateverGreen) so nothing
   fights over the device.
4. *(Optional)* Inject the **full 2 MiB flash dump** (the `.rom` in
   `firmware/`) as `ATY,bin_image` under the GPU's PciRoot path in OpenCore
   `DeviceProperties`. Without it the driver reads the PSP's IP-discovery copy
   from the top-of-VRAM TMR via MM_INDEX/MM_DATA (`Discovery,Source` in ioreg
   shows which path won) and the VBIOS data tables from the PCI expansion ROM.

Remove any `/Library/Extensions/RDNA4FB.kext` (or `RX9070XT.kext`) left over
from the standalone build: it cannot load on Tahoe and only produces
`kernelmanagerd` rejections.

**Recovery:** keep a known-good copy of your EFI on a USB stick while testing.
`rdna4-off=1` turns the plugin off, `-liluoff` turns off all Lilu plugins;
both return macOS to its stock fallback framebuffer.

## Testing in a VM

`VMTEST=1` builds additionally treat QEMU's `vmware-svga` (`15ad:0405`) as
ours: all register work is skipped (it has no BAR5), the Lenovo fixture EDID
stands in for a monitor, and mode switches resize the VM display through the
Bochs VBE interface. With an [OSX-KVM](https://github.com/kholia/OSX-KVM)
guest:

```sh
tools/build-osxcross.sh VMTEST=1
tools/vm-opencore.sh --kext build-vmtest/RDNA4FB.kext --lilu ~/kexts/Lilu.kext
```

`vm-opencore.sh` starts from the stock `OpenCore/OpenCore.qcow2` and writes
`OpenCore-dev.qcow2`: verbose boot with the kernel log on the serial port,
a picker that waits, WhateverGreen disabled, the given Lilu, RDNA4FB after
Lilu, and a 1920x1080 console (`--resolution`; the fixture is a 1080p
monitor, and its modes are only published when the console matches its
preferred timing). Boot QEMU with that image; QEMU keeps an image open until
it restarts. With Remote Login on and a key authorised,
`REMOTE_USER=<account> tools/logs-ssh.sh` pulls the RDNA4FB log and registry
properties back.

### Emulated RX 9070 XT (no card needed, no VMTEST shortcuts)

`emu/` holds a QEMU model of the card, so the **bare-metal build** runs its
real code paths in the same OSX-KVM VM:

- `emu/qemu/rdna4.c` — in-tree QEMU 10.0 PCI device `rdna4`: the card's PCI
  identity (1002:7550, subsystem 1DA2:E489) and BAR layout, a 256 MiB VRAM
  aperture plus MM_INDEX/MM_DATA access to the top of VRAM (where the IP
  discovery copy sits), a BAR5 register file loaded from
  `emu/qemu/gop-state.txt` (the state the GOP leaves; same format as
  `tools/linux-capture.sh` register dumps — the current file is synthesized,
  a real capture drops in), and live engines for what the kext polls: OTG
  frame counter, DC_I2C with an EDID on HDMI line 2, DP AUX with nothing
  attached, RCC_CONFIG_MEMSIZE. The QEMU console scans out whatever the
  HUBP/OTG registers describe; the OPP pattern generator blanks it.
- `emu/efi/RdnaGopDxe` — small EFI GOP driver for the device's option ROM,
  which is the card's real legacy AtomBIOS image followed by this driver
  (OVMF skips the legacy image, the kext reads it as on hardware).

```sh
tools/emu-build.sh        # QEMU 10.0.13 + device, edk2 BaseTools + GOP driver, build-emu/rdna4.rom
tools/vm-opencore.sh --kext build/RDNA4FB.kext --lilu ~/kexts/Lilu.kext \
    --out OpenCore-emu.qcow2 --args "-v keepsyms=1 debug=0x100 serial=3 rdna4-trace=1"
tools/emu-boot.sh         # the VM on the emulated card (VNC :0, serial ~/tahoe-serial.log)
```

On Linux, `tools/emu-linux.sh <boot 0-7> [--diag]` dry-runs a real-card boot (macOS Recovery, headless,
kernel log and the diagnostic table saved per run): see `docs/emu-linux.md`.

`tools/emu-boot.sh trace=on` logs every BAR5 access to the QEMU log. The
`ih-dead=on` device option leaves IH ring writebacks enabled while suppressing
MSI delivery, which exercises the runtime's polling fallback. The same option
can be passed to `tools/vm-test.sh` alongside `rdna4-ih=1`. The
device sits on the root bus: behind a `pcie-root-port`, macOS's PCI
configurator closed the port's windows at boot. The QEMU device is
GPL-2.0-or-later (QEMU's license) and the GOP driver BSD-2-Clause-Patent
(edk2's); the rest of the repo stays BSD-3-Clause.

## Roadmap — from "framebuffer" to "real driver"

Rough order of increasing difficulty. Each step needs iteration on the actual
hardware; the `.rom` (NAVI48.bin AtomBIOS) in `firmware/` and the Linux
`amdgpu` sources (`drivers/gpu/drm/amd/`) are the references.

1. ~~**Confirm scanout adoption**~~ — **done.** Desktop verified on hardware
   with correct colors (after masking the `v_baseAddr` flag bits).
2. ~~**EDID over DP AUX and HDMI DDC**~~ — **done.** The AUX software engine
   (`auxTransaction`, following amdgpu's `dce_aux.c`) reads each DP sink's
   EDID over I2C-over-AUX; verified on hardware 2026-07-11 (Samsung 4K sink
   on AUX0). HDMI/DVI sinks are read via the DC_I2C hardware engine (per
   amdgpu's `dce_i2c_hw.c`: offset write + block read queued as two
   transactions in a single GO) — verified on hardware 2026-07-12 against a
   Lenovo 1080p sink. Served to macOS through `cscGetDDCBlock`.
3. ~~**Load on Tahoe**~~ — **done in a VM** (2026-09-27): Lilu plugin
   extending `IONDRVFramebuffer`, injected by OpenCore; mode list, EDID
   identity and (VM) mode switching verified in System Settings → Displays.
   Hardware verification of the plugin build is next.
4. **Native mode setting (DCN 4.1.0)** — program the pixel clock and
   transmitter through DMUB, then OTG timing and HUBP viewport, to change
   resolution and light additional connectors (`RDNA4Device::applyMode`).
   **Key constraint (verified against this ROM):** the VBIOS carries *no*
   display command tables — only `asic_init` survives;
   `setpixelclock`/`dig1transmittercontrol` are absent because DCN 3.1+ moved
   that work to DMUB firmware mailbox commands. The DMUB payloads are built
   and host-tested (`src/dmub.hpp`); `tools/linux-capture.sh` records what
   amdgpu programs on the same card as ground truth. **First run on the
   card 2026-10-04** (HDMI boot display, 1920x1080 to 1600x900): the GOP's
   DMUB firmware (version 0x0000e840, read over GPINT) takes the mainline
   transmitter, pixel-clock and encoder commands although the GOP itself
   never sends them. HDMI 2.0 scrambling above 340 MHz (encoder scrambler
   plus the sink's SCDC register) ran on the card 2026-10-05, off and on
   again (533 MHz). DP mode switching retimes the stream on the trained link
   (verified 2026-10-04). A pipe the GOP did not set up is lit from a
   register sequence generated from Linux's own code (verified 2026-10-04
   for one configuration, `docs/second-pipe.md`). DP link training is written and
   host-tested against Linux, not yet run on the card; modes above the boot pixel
   clock on a DP boot display still need it wired into the mode switch.
5. **Display power management** — display sleep was verified on hardware
   with the standalone build (DP: video stream off + sink D3 over native-AUX
   DPCD `SET_POWER`; HDMI: OPP pattern generator blank). It now runs from
   `cscSetSync` (DPMS), verified on hardware 2026-10-05 for the DP boot
   display and for the HDMI second pipe, which sleeps the way amdgpu does
   DPMS off (stream encoder and transmitter down, `docs/second-pipe.md`).
   An HDMI boot display does the same unless `rdna4-modeset=0`
   (`ModeSet::buildSleep`, then a mode set to the running timing to wake;
   verified on hardware 2026-10-06 at 3840x2160, 533 MHz) and is blanked
   without it.
   System sleep stays vetoed (as `IOBootNDRV` does): after GPU power loss the
   display pipe cannot be reprogrammed until native mode setting exists.
6. **Power / clocks** — SMU firmware handshake so the card is stable, not
   stuck at boot clocks.
7. **Acceleration (huge)** — a real accelerator: GFX12 command processor, ring
   buffers, memory controller, and a Metal driver. This is effectively
   reimplementing Apple's `AMDRadeonX6000` family for a new architecture and is
   out of scope for this repo's near term.

## Status

- [x] Cross-compiles on Apple Silicon and Linux/WSL → x86_64 kext bundle
- [x] Lilu plugin injected by OpenCore; extends `IONDRVFramebuffer` at
      runtime (verified in a macOS Tahoe 26 VM)
- [x] Mode list, EDID identity and mode switching answered through the NDRV
      interface (verified in the VM: System Settings → Displays)
- [x] **Boots to a 4K desktop on real hardware with correct colors**
      (standalone build, Big Sur)
- [x] AtomBIOS parser (rom header, master data table, firmwareinfo,
      display paths) verified against the real ROM via `make test`
- [x] Runtime VBIOS acquisition (`ATY,bin_image` property / expansion ROM)
- [x] Per-connector DDC/AUX line + HPD pin mapping (path records +
      gpio_pin_lut), published as `AtomBIOS,Connectors`
- [x] IP discovery parser: GC v12.0.1 / DCN v4.1.0 / NBIF v6.3.1 register
      segment bases extracted from the ROM (`make test` gates on them)
- [x] BAR5 register MMIO confirmed on hardware, read (VRAM size, DCN dumps)
      and write (DP stream registers, MCM bypass)
- [x] Kill-switch boot-arg (`rdna4-off=1`) for safe iteration
- [x] DP AUX software engine + EDID read over I2C-over-AUX (verified on
      hardware: Samsung 4K sink on AUX0)
- [x] HDMI/DVI EDID over the DC_I2C hardware engine (verified on hardware:
      Lenovo 1080p sink on ddc2; the engine must be woken — soft-reset
      deasserted, RAM out of light sleep, DDC clock enabled — before
      arbitration is requested)
- [x] Display sleep verified on hardware with the standalone build
- [x] Plugin build verified on hardware (RX 9070 XT, Tahoe 26.6 recovery, 2026-09-27)
- [x] **Compute written from scratch, running on the real card** (2026-09-28,
      `docs/hw-logs/2026-09-28-recovery-stage7-runtime-pass.txt`): PSP secure OS
      and SMU firmware, GC firmware + RLC autoload, GC hub, SDMA (doorbell) fill,
      a MEC compute queue running PM4, a hand-written and a clang-built gfx1201
      kernel dispatched and checked — with the display untouched
- [x] User-space compute runtime on the real card: `rdna4-run selftest`
      PASS (65536-item vadd in 103 us), LDS and barriers included
- [x] `rdna4-run bench` on the real card (2026-09-28,
      `docs/hw-logs/2026-09-28-recovery-bench-accelerate.txt`): SGEMM n=2048
      at 10.7 TFLOPS, 23x the Ryzen 7 5700G with Accelerate (463 GFLOPS),
      every result exact; VRAM copy 208 GB/s vs 34 GB/s CPU memcpy.
      Host->GPU 333 MB/s but GPU->host only 5 MB/s (CPU reads through the
      BAR) — the next bottleneck
- [x] DMA transfers (SDMA + AGP aperture + bounce buffer) and buffers in all
      of VRAM, on the real card (2026-09-28,
      `docs/hw-logs/2026-09-28-recovery-dma-pass.txt`): raw SDMA 12.6/13.2
      GB/s, end-to-end host->GPU 4.8 GB/s and GPU->host 3.8 GB/s (from
      0.33 and 0.005), 7896 MiB of buffers; SGEMM n=4096 11.2 TFLOPS (27x
      Accelerate), n=8192 10.4 TFLOPS in 105 ms (22x), all exact
- [x] A second display on the macOS side (`rdna4-head2=1`): a second
      `IONDRVFramebuffer` on a plugin-made nub, accepted by WindowServer and
      listed as a display (verified on hardware, Big Sur 11.6.6). Its pipe is
      not lit yet, so the monitor stays dark
- [x] HDMI mode switching on the lit pipe through DMUB (verified on
      hardware 2026-10-04, Big Sur 11.6.6: 1920x1080 to 1600x900, picture
      confirmed; `rdna4-modeset=1`)
- [x] A visible second display (`rdna4-head2=4`; verified on hardware
      2026-10-04, Big Sur 11.6.6): the kext lights a second pipe for an
      HDMI monitor next to the DisplayPort boot display and macOS extends
      the desktop onto it. The register sequence is generated from Linux's
      own DCN 4.01 code by `tools/pipegen` (`src/pipe2.cpp`). One
      configuration: HDMI on HPD3 / link 2 at 1920x1080@60. Display
      sleep and wake reach it too (verified on hardware 2026-10-05).
- [x] Mode switching on the second display (`rdna4-modeset=1` with
      `rdna4-head2=4`; verified on hardware 2026-10-05, Big Sur 11.6.6:
      1920x1080 to 1600x900 to 1280x720 and back, display sleep and wake
      at 1280x720, picture confirmed): the mode-set engine pointed at the
      second pipe, for modes up to the lit one's size and within 25 % of
      its pixel clock.
- [x] High refresh rates on the second display (verified on hardware
      2026-10-05, Big Sur 11.6.6: 1920x1080 at 120 and 144 Hz next to
      the 4K boot display, picture confirmed, measured 119.971 and
      144.012 Hz): each switch also programs what Linux's DML computes
      for the mode (request timing, DET size, global sync), generated
      per mode by `tools/pipegen` (`tools/pipegen/modes.txt`). Display
      sleep and wake ran at 144 Hz too (picture confirmed 2026-10-06,
      after a 12-minute sleep).
- [x] Hot-plug of the second display (`rdna4-hotplug=2` with
      `rdna4-head2=4`; verified on hardware 2026-10-05, Big Sur 11.6.6:
      unplug, replug, and plug after a boot without the monitor): the
      connector's HPD pin is polled; an unplugged display takes its
      head offline in macOS, a plugged one is read, lit and found.
      `rdna4-hotplug=1` only logs the pin. A 10-minute display sleep
      with hot-plug active ran on 2026-10-06: both displays woke and
      no unplug was mistaken.
- [x] The second display on the card's other HDMI connector (HPD4 /
      link 3; verified on hardware 2026-10-06, Big Sur 11.6.6: lit at
      boot, switched to 144 Hz, a 12-minute display sleep and wake at
      144 Hz, picture confirmed): a second generated table, picked by
      the connector the display answers on. Hot-plug on that
      connector ran the same day (unplug and replug twice, at 144 Hz).
- [x] The second display follows its cable between the two HDMI
      connectors (verified on hardware 2026-10-06, Big Sur 11.6.6:
      moved from one port to the other at 60 Hz, back at 144 Hz, then
      display sleep and a same-port replug, picture confirmed each
      time). Not between HDMI and DisplayPort.
- [x] The verified display features are the default (2026-10-06;
      booted on the card that day without `rdna4-head2` and
      `rdna4-hotplug`: both displays up, hot-plug acting): a
      second display on either HDMI connector, mode switching on both
      displays, hot-plug of the second. `rdna4-head2=0`,
      `rdna4-modeset=0` and `rdna4-hotplug=0` turn them off. A second
      display on DisplayPort stays behind `rdna4-head2dp=1` until it
      has run on the card.
- [ ] DisplayPort link training, for a second DisplayPort display and
      for a boot display that was unplugged. First part done on the
      host: `src/dptrain.cpp` trains a link through callbacks and
      `make test` holds its DPCD transfers, patterns, lane drive and
      delays against Linux's own code run on ten simulated monitors
      (`tools/pipegen`'s `dplink` scenario), and `src/dpphy.cpp`'s
      registers and transmitter commands against what Linux writes on
      the way. For the boot display it turned out not to be needed:
      the firmware retrains that link itself (seen on the card
      2026-10-06), so `rdna4-dptrain` only watches, and `=2` runs the
      kext's training once as a test. That training completed on the
      card 2026-10-06 (4 lanes at HBR2, all lanes locked), and the
      video was back 0.57 s after the link was taken down. The second
      DisplayPort display itself needs a second cable to try:
      `docs/todo-dptest.md` has the tests still to run. Third part written, not yet run on the card:
      a second DisplayPort display (`rdna4-head2=4`), lit from tables
      generated like the HDMI ones (`src/pipe2_linux_dp1.inc`,
      `_dp2.inc`) whose training step runs `src/dptrain.cpp` on the
      smallest link of the monitor's that carries the mode. With
      `rdna4-modeset=1` it switches modes like the HDMI one: the
      stream is retimed, after the link was trained larger if the
      mode needs that (`make test` holds 15 modes against Linux).
- [x] DP mode switching on the trained link (verified on hardware
      2026-10-04, Big Sur 11.6.6: 3840x2160 to 2560x1440, picture
      confirmed; `rdna4-modeset=1`). The plan's register writes are held
      against what Linux's own functions write for the same retime
      (`tools/dp_retime_linux.inc`).
- [x] Hardware cursor through the NDRV cursor path (`rdna4-cursor=1`,
      off by default; `docs/HANDOFF-linux.md`)
- [ ] Acceleration / Metal
