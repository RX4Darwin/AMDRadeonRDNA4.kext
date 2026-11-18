# AMDRadeonRDNA4

A **display driver** for the AMD Radeon **RX 9070 XT** (Navi 48 / RDNA 4,
PCI `0x1002:0x7550`) on x86_64 Hackintosh, built as a **Lilu plugin** against
MacKernelSDK and injected by OpenCore. Cross-compiles on Apple Silicon and on
Linux/WSL. Metal on macOS Tahoe and Sequoia runs through Apple's paravirtual GPU, served on the card by the
kext (experimental, built only with `make PVGPU=1`: [Metal](#metal-on-macos-tahoe-and-sequoia-experimental)); without
it rendering is software.

Until October 2026 the driver was called RDNA4FB; log files written before then carry that name. Versions are in
[CHANGELOG.md](CHANGELOG.md); branches, version numbers and how a build names its commit are in
[CONTRIBUTING.md](CONTRIBUTING.md).

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
boot-arg to turn it off (below); since 2026-11-15 system sleep is too, the
desktop back on both displays a couple of seconds after a key press. Since 2026-11-10 a `make PVGPU=1` build gives
macOS Tahoe 26.6.2 a Metal desktop on the card: WindowServer composites through
the card, and the Dock and apps draw with Metal; Liquid Glass's kernels run
since 2026-11-11 (Sequoia 15 tested by Jan Claußen), and since 2026-11-14 its blurred
glass (Finder's sidebar) too. OpenGL apps draw on the card as well
(Apple's GL-on-Metal renderer; kitty and Mixxx run). Since 2026-11-17 Metal 3D runs on it too: MetalQuake at
3840x1878 in HDR at 60 frames a second, and WebGL in Safari (with `rdna4-pvmac2=1`). See
[Metal](#metal-on-macos-tahoe-and-sequoia-experimental).

> **Why a Lilu plugin?** The earlier build was a standalone `IOFramebuffer`
> subclass. That links against `com.apple.iokit.IOGraphicsFamily`, whose code
> on macOS 11+ exists only inside the *System* kernel collection — OpenCore
> injects into the *Boot* collection, so the kext was silently dropped. The
> fallback, `/Library/Extensions` and the auxiliary collection, is closed to
> ad-hoc-signed kexts on Tahoe (`syspolicyd` blocks them with no approval
> button). Instead, AMDRadeonRDNA4 now extends the driver macOS already runs on an
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
> **What it is not yet:** a native GPU accelerator. The GFX12 command
> processor, rings, memory management and a compute runtime are here, and
> Metal works through Apple's paravirtual GPU (the kext plays its host), but
> macOS's Metal is still Apple's paravirtual one, with its limits. Supporting
> every feature of the card, native Metal and 3D acceleration included, is the
> goal. Much of the groundwork here (IP-discovery-driven registers,
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
   PCI device is the Navi 48 family (`0x7550` RX 9070 XT/9070/9070 GRE, `0x7551`
   R9700) are ours; everything else goes straight to Apple's code.
2. **Device** (`src/device.cpp`): when Apple's driver opens our framebuffer,
   AMDRadeonRDNA4 reads the console geometry, maps BAR5, loads the VBIOS and IP
   discovery, finds the pipe the GOP lit, reads the sink's EDID and builds the
   mode table. **Hard-won detail:** `v_baseAddr` carries flag bits in its low
   bits (this machine reads `0x840000001`); they must be masked off. Passing
   the raw value shifts WindowServer's writes one byte from the true scanout
   base, which recolours every pixel (R'=G, G'=B, B'=previous pixel's alpha) —
   it presents as "inverted colors with a blue cast" and cost a week of DCN
   register archaeology to trace.
3. **Answers** (`src/ndrv.cpp`): the mode list, per-mode video parameters and
   timings, the current mode, the connection flags, the EDID blocks and DPMS
   (`cscSetSync`) are answered from AMDRadeonRDNA4's state, record for record the
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
| `rdna4-head2=0` | **A second display is on by default (level 4, below); `0` turns it off.** `1` is the experiment it grew from: a phantom second head. The plugin creates an `IONDRVDevice` nub just before the framebuffer starts on the GPU, makes the two heads dependents of one controller, stands in for the boot NDRV the second one cannot have, and serves the second sink's EDID with one mode on a spare VRAM surface behind the console. No display register is written and nothing appears on the monitor (its pipe stays dark): it shows whether macOS accepts a second display. Next to `rdna4-compute` since 2026-10-06 (its surface is kept below the compute pool; ran together on the card that day). Verified on the card under Big Sur 11.6.6 (2026-10-04): WindowServer opens both framebuffers and macOS lists the second display. See `docs/second-head-ndrv.md`. **Higher values also light the pipe (`docs/second-pipe.md`; all verified on the card 2026-10-04):** `2` writes nothing and publishes what every step of the plan would do to the dormant pipe's registers (`AMDRadeonRDNA4,Pipe2`, shown by `tools/rig/diagnostic-log.sh`); `3` lights the stream alone, so the second monitor shows a solid teal-blue; `4` also lights the plane, so it shows the second desktop. There is a plan per connector of the card, all at 1920x1080@60 next to a boot display on another connector: HDMI on HPD3 / link 2 (the one verified), HDMI on HPD4 / link 3 (verified 2026-10-06), and DisplayPort on HPD1 / link 0 or HPD2 / link 1, whose link the kext trains itself (those two host-tested, not yet run on the card, and only used with `rdna4-head2dp=1`). Not in `VMTEST` builds. |
| `rdna4-head2dp=1` | Also serve a second display on a DisplayPort connector (lighting it, sleep, hot-plug and mode switching as on HDMI). Card-verified 2026-11-15/16 with the second DisplayPort display connected at boot: link trained by the kext at the first attempt, desktop on both displays, its sleep and wake (`debug.rdna4_head2sleep`). Still off by default: with the paravirtual displays one plugged in after boot is lit but gets no paravirtual display (noise). `docs/todo-dptest.md` has the tests. |
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
| `rdna4-compute=<stage>` | Compute bring-up, written from scratch (`src/compute.cpp`), run after the display is answered for and never with a GPU reset. **Stage 3 by default since 2026-11-03**: it starts the power-management firmware, which raises the display clocks a 3840x2160 display needs (`src/clk.cpp`); `0` turns it off. `1` = read-only survey: GFX (IMU/RLC/PFP/ME/MEC/MES), both SDMA engines, GC and MM hub apertures, PSP, HDP flush remap, and the VRAM window chosen for compute; published as the `Compute,Survey` dictionary. `2` = PSP bring-up on a background thread 5 s after the desktop: the secure-OS components through the PSP bootloader, the GPCOM command ring, `LOAD_TOC`, then the SMU firmware via `LOAD_IP_FW`, proven by the SMU answering its mailbox; published as `Compute,PSP` and `Compute,Stage`. `3` = the 19 GC firmware images through the PSP and the RLC autoload (`Compute,GFX`). `4` = GC hub for VMID0, SDMA0 queue, a WRITE and a 1 MiB fill checked by the CPU (`Compute,SDMA`). `5` = a MEC compute queue programmed directly (no MES), fed through its doorbell, running PM4: the scratch-register ring test, WRITE_DATA and a RELEASE_MEM fence (`Compute,MEC`). `6` = a real gfx1201 kernel (`src/shaders/probe.s`) dispatched on the compute units, all 256 results checked (`Compute,Dispatch`). `7` = a clang-built kernel (`src/shaders/vadd.cl`) launched from its AMDGPU code object: the loader (`src/codeobj.cpp`) finds the kernel and its descriptor, the dispatch uses the compiled RSRC1/2/3 and a kernarg buffer, and 4096 results of `c = a + 3b` are checked (`Compute,Kernel`). After stage 6 or 7 the runtime is published for user space (see *User-space compute*). Each step leaves a breadcrumb in NVRAM (`4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trail`) so a hang names the step. Needs the linux-firmware blobs in `firmware/amdgpu/` at build time (`tools/build/fetch-firmware.sh`). |
| `rdna4-fakeedid=0` | `VMTEST=1` builds only: they serve the Lenovo fixture EDID and enable `rdna4-modeset` by default (so the mode list can be checked in a VM whose OpenCore pins boot-args); `=0` turns either off. |
| `rdna4-heads=<n>` | How many displays beside the boot one the kext serves (1-3; default: one for every connector, a head without a monitor waits offline). |
| `rdna4-boot4k=0` | Under the paravirtual displays an HDMI boot display behind a passive DP++ adapter (which the firmware lights at what the adapter carries) is taken over by a head and lit at 3840x2160 once the display clocks are up; `0` turns that off. |
| `rdna4-dpdual-khz=<kHz>` | The TMDS clock a passive DP++ adapter is taken to carry, in place of what its ID block says. |
| `rdna4-wakediff=1` | After a display wake, compare the registers with what lighting the pipe left in them (`src/regsnap.cpp`): a wake that leaves the display dark differs somewhere. |
| `rdna4-dmubload=1` | The display microcontroller on the kext's own firmware (`dcn_4_0_1_dmcub.bin`, loaded through the PSP after the compute bring-up's PSP stage) instead of the GOP's. |
| `rdna4-dcninit=1` | The cold-start test on a warm card: once the SMU runs, the extra displays and the DisplayPort boot display (pipe 0 at 3840x2160) are taken down, Linux's `dcn401_init_hw` runs from nothing, the kext starts the DCHUB timer, and the pipes are lit again from Linux-generated tables. A DisplayPort extra pipe is taken down without its transmitter disable (`src/pipe2.cpp`: on link 1 it stopped the card); after a power loss such a pipe is only lit again. |
| `rdna4-coldboot=1` | A real cold start: once the kext has read what the GOP left (pipes, EDIDs, boot link, scanout address, DCHUB timer), the card is reset (mode 1, as Linux resets it on this SMU) and the displays are lit from nothing once the SMU runs. Wants `rdna4-compute` at stage 3 or more and `rdna4-dmubload=1`. |
| `rdna4-coldprobe=1` | Log what a mode-1 reset depends on (the PSP and SMU mailboxes, the registers `asic_init` touches, the PCI config space and Resizable BAR), and add `sudo sysctl debug.rdna4_coldreset=1`, the reset alone at run time (the card comes back; the GPU work running then hangs the machine later). |
| `rdna4-syssleep=0` | **System sleep is on by default (since 2026-11-15); `0` turns it off**, and macOS then refuses system sleep, as IONDRVFramebuffer does for any framebuffer on IOBootNDRV. Off in VM test builds. On: the runtime and the paravirtual GPU's host give back what the card holds at sleep (writing back what lives only in VRAM), and the wake brings the runtime up again with both displays lit from nothing. With it IOGraphics' power work runs right for our framebuffers and for the paravirtual one (`src/plugin.cpp`: framebuffer slots, controller state, the dark wake, the paravirtual display's power states). Card-verified 2026-11-15, also with two DisplayPort displays: about 30 s from Sleep to off (macOS's own display-off step), the desktop back about 2 s after a key press. A board that wakes from USB through a GPE macOS does not take for a user wake needs USBWakeFixup with an `SSDT-USBW` on that GPE in its OpenCore config (here GPE 8, reported as GPP2). |
| `rdna4-pm=1` | The compute service joins the power tree and suspends and resumes the runtime itself. Stalls a real system sleep: not with `rdna4-syssleep=1`. |
| `rdna4-sleeptest=1`, `rdna4-quiesce-test=1`, `rdna4-resume-tests=1`, `rdna4-sleepabort=0` | Power tests and their controls: the runtime's own sleep/wake cycle without a system sleep (`rdna4-run sleeptest`), the shutdown quiesce at boot, the bring-up tests again after a wake; `rdna4-sleepabort=0` lets client waits run on through a sleep request. |
| `rdna4-clocks=1` | Log the DPM levels of the display and memory clocks the power-management firmware offers. |
| `rdna4-dispclk=<MHz>`, `rdna4-dcfclk=<MHz>`, `rdna4-uclk=<MHz>`, `rdna4-fclk=<MHz>` | The hard minimums the kext asks the SMU for, in place of DISPCLK/DPPCLK 650, DCFCLK 600, UCLK at its top level, FCLK left alone. `rdna4-dispclk=0` leaves the clocks alone (and offers no 4K). |
| `rdna4-gfx=2` | The graphics ring (PFP/ME through its doorbell), after the compute stages; `rdna4-gfxclient=1` adds a client's gfx IB through it, `rdna4-gfxcol=1` a colour test. |
| `rdna4-gfxcap`, `rdna4-gfxce`, `rdna4-gfxcg`, `rdna4-gfxcsb`, `rdna4-gfxdiag`, `rdna4-gfxgolden`, `rdna4-gfxidle`, `rdna4-gfxoff`, `rdna4-gfxpark`, `rdna4-gfxpm`, `rdna4-gfxprobe`, `rdna4-gfxrbmin`, `rdna4-gfxsane`, `rdna4-gfxsrm`, `rdna4-gfxva0` | Experiments and A/B controls of the GFX bring-up and its power features (clock gating, GFXOFF, idle accounting, golden registers, clear state, ring and clip settings); what each does is at its use in `src/gfxring.cpp`, `src/compute.cpp` and `src/pmidle.cpp` and in `docs/power-gfx.md`. |
| `rdna4-vm=1` | The runtime's clients each in an address space of their own (a VMID). |
| `rdna4-vmshared=<0-2>` | Where the clients' compute jobs run: 1 (the default) on two kernel-owned shared queues, 2 the same with pooled VMIDs, 0 the old path, a queue of the client's own. |
| `rdna4-hang=0` | The shared queues' recovery from a hung job is on by default; `0` turns it off. |
| `rdna4-vm-diag`, `rdna4-vm-exec`, `rdna4-vm-force-fail`, `rdna4-vm-ispte`, `rdna4-vmid-test`, `rdna4-faultpage` | Diagnostics and negative controls of the GPU page tables and VMIDs (`docs/w13-vmid.md`, `src/vmtest.cpp`). |
| `rdna4-flip=1`, `rdna4-vrambench=1`, `rdna4-accelcensus=1` | The page-flip smoke test after the compute bring-up; VRAM speed through BAR0 per cache mode; an IOAccelerator-shaped service that logs what Metal asks of it (`docs/metal-spike.md`). |
| `rdna4-pvgpu=1` | `make PVGPU=1` builds: the paravirtual GPU Apple's `AppleParavirtGPU` drives, its host on the card (Metal, *Metal* below); `rdna4-pvgpu-disp` (bit 2: the heads are its displays), `rdna4-pvgpu-delay=<s>` (publish it later), `rdna4-pvgl=1` (OpenGL on it), `rdna4-pvscaled=0` (no scaled modes above 3840x2160), `rdna4-pvfast=1` (the boot display's modes at each size's fastest refresh, not the one nearest 60 Hz), `rdna4-pvedid=0` (the guest's EDID as it makes it); `rdna4-pvgpu-class`, `rdna4-pvgpu-match`, `rdna4-pvgpu-width`, `rdna4-pvgpu-height` and `rdna4-steptrace` are bring-up diagnostics. |
| `rdna4-cursorcm`, `rdna4-cursorcrq`, `rdna4-cursordlg`, `rdna4-cursordscl`, `rdna4-cursorlock`, `rdna4-cursormpcc`, `rdna4-cursormpcsel`, `rdna4-cursorpipe`, `rdna4-cursorttu` | A/B controls of the hardware cursor's programming (`rdna4-cursor=1`; `docs/cursor-audit.md`, `docs/cursor-ttu.md`). |
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

## User-space compute (`rdna4-run`, `librdna4`)

With `rdna4-compute=6` or `7`, once the bring-up finishes the kext publishes
`RDNA4ComputeService` and any root process can run gfx1201 kernels on the
card. The model is HSA's, kept synchronous: allocate VRAM buffers (their GPU
addresses go into kernel arguments), copy data in and out, load a code
object as clang builds it (`clang -target amdgcn-amd-amdhsa -mcpu=gfx1201`
then `ld.lld -shared`), dispatch a kernel over a 3-D grid and wait for it.
Buffers and programs belong to the connection and are freed when it closes.

```sh
sudo ./rdna4-run       info                     # stage, heap size and free space
sudo ./rdna4-run       selftest [items]         # vadd and an LDS/barrier kernel, every result checked
sudo ./rdna4-run       bench [small]            # host<->GPU MB/s, VRAM GB/s, SGEMM GFLOPS, each vs the
                                              # CPU (memcpy; Accelerate's cblas_sgemm, weak-linked),
                                              # then FP16/BF16 GEMM on the matrix units (WMMA)
sudo ./rdna4-run       show [seconds]           # render a Mandelbrot into a device buffer, present it,
                                              # CPU-check 64 spread pixels, then restore the desktop;
                                              # VM runs use scale 4 and real hardware uses scale 1
sudo ./rdna4-run       anim [seconds]          # double-buffered zoom with async presents and FPS stats
sudo ./rdna4-run       load k.hsaco my_kernel   # load a code object, describe a kernel
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

## Metal on macOS Tahoe and Sequoia (experimental)

macOS has a Metal driver for one GPU it does not make itself: the paravirtual GPU of Apple's virtual machines
(`AppleParavirtGPU`, `AppleParavirtGPUMetal.bundle`). With `make PVGPU=1` the kext publishes such a device on the
card and plays its host: Apple's driver and Metal run unchanged, and the kext carries out their command stream on
the RX 9070 XT (`src/pvgpu.cpp`). Shaders arrive as Apple's AIR and are translated to gfx1201 at run time by a root
helper (`tools/metal/m3/air2amdgpu.cpp`, LLVM). With `rdna4-pvgpu-disp=7` the displays are the paravirtual GPU's
displays, so WindowServer composites with Metal and its frames are scanned out by the card's own display engine
(`src/pvscanout.hpp`). Written by Jan Claußen (PR 6), reviewed and card-tested on Tahoe.

What runs (card, macOS Tahoe 26.6.2, 2026-11-10 to 2026-11-12; Sequoia 15 by Jan Claußen): the login window and the
desktop composited on the card, the Dock with every icon, apps' windows (Finder, iTerm2, ...), Liquid Glass with its
blurred glass (Finder's sidebar), Metal compute, blits and draws of every form with vertex descriptors, textures, mip
levels (drawn into, sampled at explicit LODs) and mip-level views, 2D array textures, blending, stencil, the depth
test (Depth32Float), RG32 integer targets, sRGB targets, 2x and 4x multisampling, up to eight colour attachments,
and the animated wallpapers (Tahoe's "Macintosh") at about 42 frames a second. `tools/metal/m1/mtlprobe.m` (`mtlprobe all`)
checks blits, compute and drawing; `mtlprobe cube` checks a depth-tested cube in perspective against the CPU, `mtlprobe spin`
shows nine lit, textured cubes in a window. `sysctl debug.rdna4_pvnotrun` lists the draws the host could not run since
the boot, by reason.

**3D** (card, 2026-11-17): MetalQuake (`tools/metal/metalquake/`: a patch to the game and how to build it for Intel)
plays its demo and its levels at 3840x1878 in HDR at 60 frames a second. WebGL runs in Safari with `rdna4-pvmac2=1`:
Tahoe's Intel build of the paravirtual Metal driver reports a device without Mac GPU family 2, which WebGL (ANGLE)
requires, and the boot-arg patches that one value as the driver loads. It is off by default: Chrome then takes its
Dawn path, whose shaders do not translate yet, and WindowServer hung while it ran. `tools/metal/webgl/` has the cube
check and the spinning cubes as a WebGL page.

**OpenGL** (on unless `rdna4-pvgl=0`, card 2026-11-12 to 2026-11-14): macOS's OpenGL 4.1 on Metal
(`AppleMetalOpenGLRenderer.bundle`) runs on the paravirtual GPU, so unmodified OpenGL programs draw on the card:
kitty 0.50.0 (GL 3.3 core) and Mixxx 2.5.6 with its waveforms (GL 2.1). The kext names the renderer on the
accelerator (`IOGLBundleName`) and patches two places in the renderer as its pages are loaded (its GPU vendor test
and a render-pass setting the paravirtual device does not have). Those patches are for Tahoe 26.6.2's renderer: the kext
names it only when the renderer's file has the bytes they replace, so after an update of it (or on Sequoia) OpenGL
stays in software.

**Setting it up.** On the build machine, from the top of the repository: the kext with the paravirtual GPU, the
user-space programs, and the shader translator (which builds LLVM for x86_64 in `Build/llvm` the first time, which
takes a while):

```bash
make PVGPU=1 all userspace metal-helper
```

Install the kext as usual (*Installing*). Then, on the Mac that has the card, from a copy of the repository with
that build in it, install the shader translator (a LaunchDaemon, log in `/var/log/rdna4-air2amdgpu.log`):

```bash
sudo tools/metal/m3/install-helper.sh Build/m3/x86_64/air2amdgpu
```

Install the switch that sets CoreDisplay's IOPresentment to what each boot needs, and on Tahoe Metal's error mode
(log in `/var/log/rdna4-iop.log`):

```bash
sudo userspace/rdna4-iop/install.sh
```

Optionally, keep each boot's kernel log in `/var/log/rdna4`:

```bash
sudo userspace/rdna4-logd/install.sh
```

Then boot with these boot-args (in OpenCore's `boot-args`, next to the others). `rdna4-pvgpu-delay=5` publishes the
device early; it waits for the card, up to 2 minutes after the delay:

```
rdna4-compute=7 rdna4-gfx=2 rdna4-vm=1 rdna4-pvgpu=1 rdna4-pvgpu-disp=7 rdna4-pvgpu-delay=5
```

| Boot-arg | Effect |
|----------|--------|
| `rdna4-pvgpu=1` | Publish the paravirtual GPU on the card (only in `make PVGPU=1` builds). Needs the compute runtime up (`rdna4-compute=7`, `rdna4-gfx=2`, `rdna4-vm=1`); without it no device is published. |
| `rdna4-pvgpu-disp=<bits>` | The display side. `7`: the heads are the paravirtual GPU's displays (bit 2), reported online (bit 0) with ~60 Hz vertical blanks (bit 1). |
| `rdna4-pvgpu-delay=<s>` | Publish this many seconds after the plugin starts (default 25), then as soon as the card and its runtime are up. |
| `rdna4-pvgpu-class=<24 bits>` | The PCI class code the device reports (default `0x030000`). |
| `rdna4-pvgpu-match=0` | The device is published but no driver may take it (diagnostics). |
| `rdna4-pvscaled=0` | No scaled modes above a head's 3840x2160. |
| `rdna4-pvgl=0` | OpenGL in software. By default OpenGL is on the card: `AppleMetalOpenGLRenderer` named on the accelerator and patched as it loads, when it is Tahoe 26.6.2's. |
| `rdna4-pvmac2=1` | The paravirtual device reports Mac GPU family 2 and Common3 (`AppleParavirtGPUMetal`'s feature profile patched as it loads, Tahoe 26.6.2): WebGL in Safari. Off by default: Chrome's Dawn path does not run yet, and WindowServer hung while Chrome ran with it. |

**Limits.** It is Apple's paravirtual Metal, with that device's limits and the features the host does not offer
yet: no Metal heaps, no argument buffers, no indirect command buffers, no texture swizzles (the Intel build of the
guest driver drops them); OpenGL only with Tahoe 26.6.2's renderer. The shader translator takes 16 textures and
samplers a stage, buffer slots 0-27 and no scratch memory (a local array the compiler cannot keep in registers):
Chrome's Dawn shaders and MetalQuake's bindless world shader need more. Known open issues: flickering lines and squares
over Safari's active tab on the display (not in screenshots), and WindowServer's `brim_init_lpf` draws into a format
the host does not take yet. Apps started before the device is published draw on the CPU until
they are restarted. The IOSurface lookup depends on Tahoe 25G83's IOSurface internals. Without the card's firmware in the
build (`firmware/amdgpu/`) the runtime does not come up and no device is published. The design notes and every
measurement are in `docs/m0-pvgpu.md` and `docs/m1-stream.md`.

## Where things are

What each directory and source file is: [docs/index.md](docs/index.md).

## Building

Clone with the submodules (`git clone --recursive`, or
`git submodule update --init` in an existing checkout; they are in `lib/`).

```sh
tools/build/fetch-firmware.sh   # once: AMD's firmware for the compute bring-up, into firmware/amdgpu/
make                    # the kext and the programs for the test machine (x86_64, min macOS 11)
make CONFIG=DEBUG       # the same, unoptimised and with DEBUG defined
make test               # host-side tests: parsers, EDID fixtures, pipe discovery, DMUB payloads,
                        # NDRV translator, the engines; with a ROM dump (firmware/README.md) also
                        # the AtomBIOS and IP discovery checks
make clean              # what make built; the Mesa tree of `make mesa` stays (make distclean: all of Build/)
```

Everything that is built goes to `Build/`, and what is meant to be used goes to one directory per
version and configuration:

```
Build/AMDRadeonRDNA4-<version>-RELEASE/AMDRadeonRDNA4.kext
Build/AMDRadeonRDNA4-<version>-RELEASE/bin/    rdna4-run, n48nprobe, diagnostic-log.sh; with Mesa built
                                              (make mesa) also libvulkan_radeon.dylib, vkprobe and swapprobe;
                                              make vkcube adds Khronos's vkcube, their loader and vulkan/icd.d/
Build/AMDRadeonRDNA4-<version>-DEBUG/...       make CONFIG=DEBUG
Build/vmtest/AMDRadeonRDNA4-<version>-.../     make VMTEST=1
```

`make product-dir` prints that directory for the configuration it is given. `RELEASE` (`-Os`) is the
default and what every card test up to 0.1.0 ran; `DEBUG` (`-O0`, `DEBUG` defined) has not run on the card.

The `Makefile` (and `src/kext.mk`, which it runs for the kext's objects) keep to what GNU make, from the
3.81 that macOS ships, and BSD make (`bmake`) both understand. `AMDRadeonRDNA4.xcodeproj` is a front for
it: its targets run `make` (Debug and Release are `CONFIG=DEBUG` and `CONFIG=RELEASE`), and it shows the
directories as they are on disk, so there is no second list of sources or flags to keep in step.

Verify the output:

```sh
file "$(make -s product-dir)"/AMDRadeonRDNA4.kext/Contents/MacOS/AMDRadeonRDNA4   # Mach-O 64-bit kext bundle x86_64
```

### Building on Linux / WSL (osxcross)

LLVM's `ld64.lld` cannot emit kext bundles, but Apple's ld64 can: build an
[osxcross](https://github.com/tpoechtrager/osxcross) toolchain from a macOS SDK
(the "Command Line Tools for Xcode 26.x" `.dmg` carries one, including
`libkmodc++.a`), then:

```sh
tools/build/build-osxcross.sh            # -> Build/AMDRadeonRDNA4-<version>-RELEASE/
tools/build/build-osxcross.sh VMTEST=1   # -> Build/vmtest/AMDRadeonRDNA4-<version>-RELEASE/
make test                                # host tests need only clang and clang++
```

On Windows checkouts without symlink support, Lilu's header symlinks become
text files; the Makefile copies the real headers into `Build/liluhdr` first.

## Installing (OpenCore)

1. Use **Lilu 1.7.1 or newer** (older Lilu disables itself on macOS 26).
2. Copy `Build/AMDRadeonRDNA4-<version>-RELEASE/AMDRadeonRDNA4.kext` to `EFI/OC/Kexts/` and add a `Kernel → Add`
   entry **after Lilu**:

   | Key | Value |
   |-----|-------|
   | `BundlePath` | `AMDRadeonRDNA4.kext` |
   | `ExecutablePath` | `Contents/MacOS/AMDRadeonRDNA4` |
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
tools/build/build-osxcross.sh VMTEST=1
tools/vm/vm-opencore.sh --kext "$(make -s product-dir VMTEST=1)"/AMDRadeonRDNA4.kext --lilu ~/kexts/Lilu.kext
```

`vm-opencore.sh` starts from the stock `OpenCore/OpenCore.qcow2` and writes
`OpenCore-dev.qcow2`: verbose boot with the kernel log on the serial port,
a picker that waits, WhateverGreen disabled, the given Lilu, AMDRadeonRDNA4 after
Lilu, and a 1920x1080 console (`--resolution`; the fixture is a 1080p
monitor, and its modes are only published when the console matches its
preferred timing). Boot QEMU with that image; QEMU keeps an image open until
it restarts. With Remote Login on and a key authorised,
`REMOTE_USER=<account> tools/vm/logs-ssh.sh` pulls the AMDRadeonRDNA4 log and registry
properties back.

### Emulated RX 9070 XT (no card needed, no VMTEST shortcuts)

`emu/` holds a QEMU model of the card, so the **bare-metal build** runs its
real code paths in the same OSX-KVM VM:

- `emu/qemu/rdna4.c` — in-tree QEMU 10.0 PCI device `rdna4`: the card's PCI
  identity (1002:7550, subsystem 1DA2:E489) and BAR layout, a 256 MiB VRAM
  aperture plus MM_INDEX/MM_DATA access to the top of VRAM (where the IP
  discovery copy sits), a BAR5 register file loaded from
  `emu/qemu/gop-state.txt` (the state the GOP leaves; same format as
  `tools/linux/linux-capture.sh` register dumps — the current file is synthesized,
  a real capture drops in), and live engines for what the kext polls: OTG
  frame counter, DC_I2C with an EDID on HDMI line 2, DP AUX with nothing
  attached, RCC_CONFIG_MEMSIZE. The QEMU console scans out whatever the
  HUBP/OTG registers describe; the OPP pattern generator blanks it.
- `emu/efi/RdnaGopDxe` — small EFI GOP driver for the device's option ROM,
  which is the card's real legacy AtomBIOS image followed by this driver
  (OVMF skips the legacy image, the kext reads it as on hardware).

```sh
tools/vm/emu-build.sh        # QEMU 10.0.13 + device, edk2 BaseTools + GOP driver, Build/emu/rdna4.rom
tools/vm/vm-opencore.sh --kext "$(make -s product-dir)"/AMDRadeonRDNA4.kext --lilu ~/kexts/Lilu.kext \
    --out OpenCore-emu.qcow2 --args "-v keepsyms=1 debug=0x100 serial=3 rdna4-trace=1"
tools/vm/emu-boot.sh         # the VM on the emulated card (VNC :0, serial ~/tahoe-serial.log)
```

On Linux, `tools/vm/emu-linux.sh <boot 0-7> [--diag]` dry-runs a real-card boot (macOS Recovery, headless,
kernel log and the diagnostic table saved per run): see `docs/emu-linux.md`.

`tools/vm/emu-boot.sh trace=on` logs every BAR5 access to the QEMU log. The
`ih-dead=on` device option leaves IH ring writebacks enabled while suppressing
MSI delivery, which exercises the runtime's polling fallback. The same option
can be passed to `tools/vm/vm-test.sh` alongside `rdna4-ih=1`. The
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
4. **Native mode setting (DCN 4.1.0)** — done on the card, but for the
   items below the struck ones. The VBIOS carries *no* display command
   tables (only `asic_init`): pixel clock, transmitter and encoder go to the
   DMUB firmware as mailbox commands (`src/dmub.hpp`).
   - ~~HDMI mode switches (2026-10-04) and HDMI 2.0 scrambling at 533 MHz
     (2026-10-05)~~
   - ~~DisplayPort mode switches retimed on the trained link (2026-10-04)~~
   - ~~A second pipe lit from a register sequence generated from Linux's own
     code (2026-10-04, `docs/second-pipe.md`), on either HDMI connector and
     behind passive DP++ adapters, with its sleep, mode switches and
     hot-plug~~
   - ~~The kext's own DisplayPort link training (2026-10-06, 4 lanes at
     HBR2)~~
   - ~~Lighting the displays when nothing set them up, on a warm card
     (2026-11-14/15): the DMUB firmware loaded through the PSP and started as
     amdgpu does (`rdna4-dmubload=1`), Linux's `dcn401_init_hw` from nothing
     (168 recorded steps), the boot display relit from a Linux-generated
     table with the kext's own training, its scanout address computed and
     the DCHUB global timer started by the kext (`rdna4-dcninit=1`), in a
     cold start's order~~
   - ~~A real cold start from a reset card (`rdna4-coldboot=1`, 2026-11-15):
     a mode-1 reset at boot as Linux does it, then both displays and the
     desktop with nothing of the GOP's left in the hardware~~
   - ~~DisplayPort modes above the boot pixel clock, with Linux's request
     timing for each (2026-11-15: 2560x1440@144, 604 MHz, above the 533 MHz
     boot mode)~~
   - ~~A second DisplayPort display (2026-11-15, `rdna4-head2dp=1`): connected
     at boot, its link trained by the kext at the first attempt, the desktop
     on both DisplayPort displays; plugged in later, lit as well~~
   - ~~The cold start after a real power loss (2026-11-15): after system
     sleep both displays are lit from nothing (step 5)~~
   - ~~A DisplayPort extra pipe's own sleep (2026-11-16): its transmitter
     disable (DMUB) stopped the card on link 1, so it is left out there; the
     rest of the sleep and the wake run on the card, the cold start takes such
     a pipe down again~~
   - Open: with the paravirtual displays, a display plugged in after boot gets no
     paravirtual display (they are counted at boot); DisplayPort modes that
     need a faster link than the GOP trained, or DSC (3840x2160 at 120 and
     144 Hz).
5. **Display power management** — done on the card.
   - ~~Display sleep (DPMS through `cscSetSync`, 2026-10-05/06): DP video
     stream off and sink D3 over native-AUX `SET_POWER`; the HDMI second pipe
     as amdgpu does DPMS off (`docs/second-pipe.md`); an HDMI boot display
     through `ModeSet::buildSleep` and a mode set to wake~~
   - ~~System sleep (2026-11-15, on by default, `rdna4-syssleep=0` turns it
     off): macOS no longer refuses it; the runtime quiesced while the card has power, the paravirtual GPU's
     host writing back what lives only in VRAM; after the wake the card brought
     up from nothing (PSP, SMU, DMUB, clocks, cold start) with both displays
     lit and the desktop back~~
   - ~~IOGraphics' own power work (2026-11-15, `src/plugin.cpp`): every
     framebuffer in its controller's table (the expander's wrappers on this
     board left gaps: 45 s per framebuffer at boot, sleep and wake), the
     controller's state recomputed for framebuffers without a WindowServer
     port (a 90 s wait before every sleep), no deadlock in a dark wake, and
     power states for the paravirtual display (a 30 s WindowServer wait)~~
   - ~~Two DisplayPort displays through sleep and wake (2026-11-15): both
     back about 2 s after a key press~~
   - ~~A key press wakes to the desktop (2026-11-15): on this board it wakes
     the system through GPE 8 (reported as GPP2, shared with the GPU's port),
     which macOS took for a dark wake; an `SSDT-USBW` on that GPE with
     USBWakeFixup (in the rig's OpenCore config, not this repository) makes it
     a user wake~~
   - ~~System sleep on by default (2026-11-15)~~
6. **Power / clocks** — done on the card, but for the items below the
   struck ones.
   - ~~SMU firmware handshake: the power-management firmware loaded through
     the PSP, its mailbox answering, its features turned on (GFXCLK and UCLK
     DPM among them), so the card is not stuck at boot clocks: the graphics
     clock goes from 3195 MHz under load to 249 MHz idle within a third of a
     second (2026-11-15)~~
   - ~~The display clocks held at what a 4K pipe needs (DISPCLK and DPPCLK
     650 MHz, DCFCLK 600 MHz, UCLK at its top level, through the display
     mailbox as amdgpu's DCN 4.01 clock manager asks for them)~~
   - Open: the features held back so that they do not retune clocks under the
     lit displays (DCN DPM, the memory, fabric and DCEF deep sleeps, GFXOFF;
     the display then takes part in memory clock switches, with watermarks for
     the SMU); fan and thermal behaviour.
7. **Acceleration** — the GFX12 command processor, rings, memory management
   and a compute runtime run on the card (*User-space compute*), and Vulkan
   through RADV. Metal runs through Apple's paravirtual GPU, served on the card
   by the kext (*Metal*, experimental, since 2026-11-10 on Tahoe). The goal is
   every feature of the card, native Metal and 3D acceleration included: a
   driver of our own in the place of Apple's `AMDRadeonX6000` family, for a new
   architecture.

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
- [x] System sleep and wake with the desktop back (on by default,
      `rdna4-syssleep=0` turns it off; verified on hardware 2026-11-15,
      Tahoe 26.6.2, with one and with two displays)
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
      own DCN 4.01 code by `tools/gen/pipegen` (`src/pipe2.cpp`). One
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
      per mode by `tools/gen/pipegen` (`tools/gen/pipegen/modes.txt`). Display
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
      display on DisplayPort stays behind `rdna4-head2dp=1` (it ran on
      the card 2026-11-15, its sleep and wake 2026-11-16).
- [x] DisplayPort link training, for a second DisplayPort display and
      for a boot display that was unplugged (the second display's trained
      at the first attempt on the card, 2026-11-15). First part done on the
      host: `src/dptrain.cpp` trains a link through callbacks and
      `make test` holds its DPCD transfers, patterns, lane drive and
      delays against Linux's own code run on ten simulated monitors
      (`tools/gen/pipegen`'s `dplink` scenario), and `src/dpphy.cpp`'s
      registers and transmitter commands against what Linux writes on
      the way. For the boot display it turned out not to be needed:
      the firmware retrains that link itself (seen on the card
      2026-10-06), so `rdna4-dptrain` only watches, and `=2` runs the
      kext's training once as a test. That training completed on the
      card 2026-10-06 (4 lanes at HBR2, all lanes locked), and the
      video was back 0.57 s after the link was taken down. Third part, run
      on the card 2026-11-15: a second DisplayPort display (`rdna4-head2=4`
      with `rdna4-head2dp=1`), lit from tables
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
      (`test/ref/dp_retime_linux.inc`).
- [x] Hardware cursor through the NDRV cursor path (`rdna4-cursor=1`,
      off by default; `docs/HANDOFF-linux.md`)
- [x] Metal through Apple's paravirtual GPU, served on the card (`make PVGPU=1`;
      verified on hardware 2026-11-10 to 2026-11-12, Tahoe 26.6.2): a Metal
      desktop, the Dock, Liquid Glass and apps' windows; Sequoia 15 tested by
      Jan Claußen. Experimental (*Metal* above)
- [x] Metal 3D through it (verified on hardware 2026-11-17, Tahoe 26.6.2):
      MetalQuake at 3840x1878 in HDR at 60 frames a second, WebGL in Safari
      (`rdna4-pvmac2=1`), `mtlprobe cube` with Apple's GPU's pixels
- [ ] Native Metal and 3D acceleration (the goal: every feature of the card)
