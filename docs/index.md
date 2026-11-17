# Where things are

What each directory and file of the repository is. The project's overview, boot arguments, building and the roadmap
are in the [README](../README.md).

## Directories

| Directory | What |
|---|---|
| `src/` | The kext. `src/shaders/` has the sources of the GPU test kernels whose machine code is committed beside them; `src/kext.mk` builds the objects. |
| `include/` | The kext's interface to user space, shared by both sides. |
| `userspace/` | Programs that use that interface: `rdna4-run/` (the compute runtime's client and its library) and `vulkan/` (the RADV port and its test programs). |
| `test/` | Host tests, run by `make test`; `test/ref/` holds the tables taken from Linux that they compare against. |
| `tools/` | Development scripts by purpose (`tools/README.md`). |
| `emu/` | The emulated card: a QEMU device and the EFI driver of its option ROM. |
| `firmware/` | Where fetched firmware goes; nothing of it is in the repository (`firmware/README.md`). |
| `lib/` | The submodules: Lilu and MacKernelSDK. |
| `docs/` | Notes, plans and the records of the card tests. |
| `Build/` | Everything that is built (not in the repository). |

## Files

| File | Purpose |
|------|---------|
| `src/plugin.cpp` | Lilu plugin entry: routes `IONDRVFramebuffer::doDriverIO`, recognises our framebuffers, creates the device state, hands NDRV requests to the translator. Checks the local NDRV record mirrors against the SDK at compile time. |
| `src/device.{hpp,cpp}` | `RDNA4Device`: scanout adoption, BAR5 MMIO, VBIOS/IP discovery, lit pipe, AUX/DC_I2C engines and EDID (all extension blocks, E-DDC segments), DMUB ring, SMU/PSP/IH diagnostics, display power, mode table and mode switches (DisplayPort up to what the link and DISPCLK carry, with Linux's request timing), the cold start (`coldStart`) and the mode-1 reset of a real cold boot (`coldReset`, `rdna4-coldboot`). |
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
| `src/pvgpu.{hpp,cpp}`, `src/pvstream.*`, `src/pvdraw.hpp`, `src/pvopcodes.inc`, `src/pvdevinfo.inc` | `make PVGPU=1` only: the paravirtual GPU, a PCI device on the card for Apple's `AppleParavirtGPU`, and its host, which carries out the Metal command stream on the card (blits, compute, draws, textures, IOSurfaces). |
| `src/pvscanout.{hpp,cpp}`, `src/pvhooks.{hpp,cpp}` | `make PVGPU=1` only: the heads as the paravirtual GPU's displays (`rdna4-pvgpu-disp` bit 2), their frames scanned out by the card; the hooks that keep WindowServer off the real framebuffers then. |
| `src/userclient.{hpp,cpp}` | `RDNA4ComputeService` (published once stage 6/7 finished) and `RDNA4ComputeClient`, the IOUserClient that checks each call and hands it to the runtime. Root only. |
| `include/rdna4compute.h` | The user-space ABI: selectors, scalar/struct shapes, the dispatch struct. Shared by the kext and user space. |
| `src/shaders/bench.cl`, `src/bench_codeobj.h` | `lds_reverse` (LDS + barrier test), `copy` (VRAM bandwidth), a tiled `sgemm` (64x64 tiles through LDS), the matrix units (`wmma16`, `hgemm`, `bf16gemm`), and the affine/scaled `mandelbrot` and `mandelbrot_zoom` kernels, one clang-built code object used by `rdna4-run`. |
| `userspace/rdna4-run/librdna4.{h,c}`, `userspace/rdna4-run/rdna4-run.c` | C library over the user client, including synchronous and queued Present/Restore display ownership, and `rdna4-run` (`info`, `selftest`, `bench`, `show`, `anim`, `load`). Built with the kext (`Build/AMDRadeonRDNA4-<version>-RELEASE/bin/rdna4-run`). |
| `src/shaders/probe.s`, `src/probe_kernel.h` | The stage-6 test kernel (gfx1201 assembly) and its machine code, generated by `tools/gen/build-shaders.sh` with upstream LLVM. |
| `src/ndrv.{hpp,cpp}` | Freestanding NDRV `csc` translator: mode list, video parameters, timings, current mode, connection, EDID blocks, DPMS, mode switch. Host-tested. |
| `src/bochsvbe.{hpp,cpp}` | `VMTEST` only: mode switches on QEMU's `vmware-svga` through the Bochs VBE interface (the one OVMF's GOP uses on that card). |
| `src/cursor.cpp` | The DCN hardware cursor behind the NDRV cursor calls (`rdna4-cursor=1`). |
| `src/atombios.{hpp,cpp}` | Freestanding, bounds-checked AtomBIOS parser: data tables (connectors, GPIO LUT, firmwareinfo) + command-function directory. |
| `src/ipdiscovery.{hpp,cpp}` | Parser for AMD's IP discovery binary — per-card IP versions and register segment bases (what amdgpu uses instead of hardcoded offsets; the key to ASIC portability). |
| `src/edid.{hpp,cpp}` | EDID parsers: base block (all descriptors, range limits, established/standard timings via a DMT table, physical size), CTA-861 extension (DTDs, VICs via a CEA-861 table, HDMI VSDB) and DisplayID detailed timings (Type I and VII). |
| `src/modes.{hpp,cpp}` | EDID (with its DisplayID blocks) → deduplicated, filtered, deterministically ordered display-mode table with stable IDs. |
| `src/pipe.{hpp,cpp}` | Lit-pipe discovery (OTG/DIG/link/OPP/HUBP, DP vs HDMI) and OTG-image → timing inversion. |
| `src/modeset.{hpp,cpp}` | The mode-switch plan for the lit pipe: HDMI (PLL and transmitter through DMUB, scrambling above 340 MHz) and DisplayPort (stream retimed on the trained link). |
| `src/pipe2.{hpp,cpp}`, `src/pipe2_linux*.inc` | The plans that light a pipe beside the boot display, one generated table per pipe, connector and stream (HDMI, DisplayPort, DP++ adapters, 4K, YCbCr 4:2:0), each with Linux's request timing for the modes it switches to; `pipe2_linux_p0_dp1_uhd.inc` is pipe 0 itself, for the cold start and the boot display's mode switches (`docs/second-pipe.md`). |
| `src/dmub.hpp` | DMUB ring command ABI and builders for the VBIOS-family commands (transmitter control v1.7, set pixel clock v1.7, DIG encoder stream setup v1.5). |
| `src/otgtiming.{hpp,cpp}` | EDID timing → DCN OTG register images, per amdgpu's `optc1_program_timing` (mode-set groundwork). |
| `src/klog.{hpp,cpp}` | The kext's own log: every line to IOLog and into a ring that `sysctl debug.rdna4_log` reads (and `rdna4-logd` follows into `/var/log/rdna4`). |
| `src/trial.{hpp,cpp}` | Options for one test boot, armed in NVRAM (`rdna4-trial`) and marked used before they act. Needs NVRAM that persists. |
| `src/sku.hpp` | Which Navi 48 card this is (RX 9070 XT, RX 9070, Radeon AI PRO R9700). |
| `src/clk.cpp` | The display clocks through the power-management firmware's display mailbox (DALSMC), as amdgpu's DCN 4.01 clock manager: DPM levels, the hard minimums a 4K pipe needs. |
| `src/dmubload.{hpp,cpp}` | The display microcontroller on the kext's own firmware (`dcn_4_0_1_dmcub.bin` through the PSP, `rdna4-dmubload`), as amdgpu starts it. Host-tested against Linux's own sequence. |
| `src/dcninit.{hpp,cpp}`, `src/dcn_init_linux.inc` | Linux's `dcn401_init_hw` on display hardware with nothing running (168 recorded steps), each timing generator's stop, and the DCHUB global timer: the display half of a cold start. |
| `src/dpdual.hpp` | What a passive DP++ (DP-to-HDMI) adapter carries, from its ID block. |
| `src/regsnap.{hpp,cpp}` | Register snapshots of what a plan wrote, compared after display sleep and wake (`rdna4-wakediff`). |
| `src/vrambench.{hpp,cpp}` | CPU write and read speed of VRAM through BAR0, per cache mode (`rdna4-vrambench`). |
| `src/cursor_linux_ref.inc` | The cursor registers as Linux leaves them on this card, for the cursor's comparison log. |
| `src/gfxring.cpp`, `src/amdgpu/clearstate_*.h` | The graphics ring (PFP/ME) brought up as amdgpu's gfx_v12_0 does, with Linux's clear-state data. |
| `src/gpuvm.{hpp,cpp}`, `src/gpuvmtable.{hpp,cpp}`, `src/ptpages.{hpp,cpp}`, `src/vmid.{hpp,cpp}` | The GPU's page tables for the runtime's clients: entry encoders, the table writer, demand-allocated table pages, the VMID pool. Freestanding and host-tested. |
| `src/vmshared.cpp`, `src/vmtest.cpp` | Clients' jobs on kernel-owned compute queues (`rdna4-vmshared`), and the address-space self-test (`rdna4-vmid-test`). |
| `src/pmidle.cpp` | Idle accounting and the GFX power items of `docs/power-gfx.md`. |
| `src/flip.{hpp,cpp}` | The display page-flip smoke test after the compute bring-up. |
| `src/accelcensus.{hpp,cpp}` | An IOAccelerator-shaped service that logs what Metal asks of it (`rdna4-accelcensus`, `docs/metal-spike.md`). |
| `include/rdna4mtlc.h` | The Metal kernel translator's interface to the kext (the root helper `air2amdgpu --serve`). |
| `src/linuxref.hpp`, `src/smu_metrics.h`, `src/isa.hpp` | Linux's GC register values on this card (for the draw's comparison log), SMU 14.0 metrics offsets, and the few shader instruction words the kext writes by hand (`make check-isa`). |
| `src/*_kernel.h`, `src/*_codeobj.h`, `src/gfx12_draw*.h`, `src/mtlkernels.h` | Generated GPU code: test kernels and shaders from `src/shaders/` (`tools/gen/build-shaders.sh`), the draw's register state from Mesa's gfx12 data, the Metal kernels translated ahead of time (`tools/metal/m3`). Do not edit. |
| `src/kmod_info.c` | kmod glue pointing at Lilu's plugin start/stop. |
| `lib/Lilu/` | Lilu submodule (v1.7.3), for the plugin headers and `plugin_start.cpp`. |
| `test/atomdump.cpp` | Host test harness: parsers against the real ROM and captured EDID fixtures, pipe discovery, DMUB payloads, the NDRV translator (`make test`). |
| `tools/vm/vm-opencore.sh`, `tools/vm/vm-ocplist.py` | Build a development OpenCore disk for an OSX-KVM VM with AMDRadeonRDNA4 injected after Lilu. |
| `tools/vm/logs-ssh.sh` | Pull the AMDRadeonRDNA4 kernel log and registry properties from a macOS machine over SSH. |
| `src/dptrain.{hpp,cpp}` | DisplayPort link training (clock recovery, channel equalisation, retries), as amdgpu does it; everything the hardware and the monitor do comes through callbacks. Host-tested against `test/ref/dp_train_linux.inc`. |
| `src/dpphy.{hpp,cpp}` | What training asks of DCN 4.01: the link encoder registers behind each training pattern and the DMUB transmitter commands (enable, lane drive, disable). Host-tested against the same reference. |
| `src/n48n.{hpp,cpp}`, `src/vmtree.{hpp,cpp}` | The kernel half of the Vulkan driver's interface: one client's buffers, mappings, contexts, submitted work (what a command buffer may be, sequences, waits, when work counts as lost) and the display it may show pictures on, and its four-level page table. Freestanding. Host-tested, and run under the real driver with `test/n48n-host/`. |
| `src/n48nkext.cpp`, `include/rdna4vulkan.h` | That engine on the card: a user client of type `'N48N'` on the compute service (root only, one at a time), buffers from the runtime's VRAM heaps and from system memory, the client's page table in VRAM, its address space (number 8), its work on the kernel's graphics ring, its pictures on the boot display through the runtime's flip, and a copy test that has the GPU's copy engine work in that address space. Needs `rdna4-compute=7`, and `rdna4-gfx=2` to submit work; with `rdna4-vm=1` it takes an address space the runtime's clients do not hold (**verified on the card** 2026-10-07: the probes, and a Vulkan program presenting for two minutes beside the runtime's whole self-test, all rows PASS). A program whose shader faults loses its own device after ten seconds and the ring stays in service (`vkprobe <lib> fault`, verified 2026-10-07). **Verified on the card** (2026-10-06): the memory half and the copy test (`userspace/vulkan/n48nprobe.c`: all ok), and the driver's work through submit and wait: two fills by the command processor (the second was taken for a compute shader's until 2026-10-07, when a real one, a 252 KiB fill, first ran) and a triangle rendered into an image and read back (`userspace/vulkan/vkprobe.c`: done, all ok). and a picture on the boot display: a moving triangle for five seconds at 60 frames a second, the desktop back afterwards (`vkprobe ... show`) (`docs/vulkan-port.md` sections 8 to 10, `docs/todo-vulkantest.md`). |
| `test/n48n-host/` | Runs the real RADV driver, and the card test's program, on that engine on a Mac, with no kext: a library that answers their IOKit calls with it (`run.sh`). |
| `userspace/vulkan/` | The RADV (Mesa Vulkan) port to macOS for this card, from Navi48-MacOS: five patches and the kernel interface they expect, two small patches of this repository's, a build script, and `n48nprobe.c`, the card test of the interface's memory half (`make n48nprobe`). On the card (2026-10-06) `n48nprobe` passes and the driver creates its device, runs two fills and renders a triangle through the kext, read back right (`vkprobe`: done, all ok). `vkprobe ... show` puts a picture on the boot display, 60 frames a second, and gives the desktop back (`docs/vulkan-port.md`). Since 2026-10-13 an eighth patch gives the driver swapchains on a headless surface, shown on that display, and `make vkcube` builds Khronos's loader and `vkcube` to run on it; **both verified on the card** 2026-10-13: a swapchain program at 60 frames a second and vkcube's cube on the boot display (`docs/vulkan-port.md` section 12). |
| `tools/rig/cgmode.py` | On the test machine: list a display's modes and switch to any of them for the login session (Displays preferences hides most modes of a monitor it takes for a television). |
| `tools/gen/pipegen/` | Runs Linux's own DCN 4.01 display code on the host against a recorder: generates `src/pipe2_linux*.inc` (modes from `modes.txt`, and `modes-boot-dp.txt` for pipe 0's table), `src/dcn_init_linux.inc` (`run.sh <linux> init`), the DMUB start-up reference (`dmubgen.sh`) and the DisplayPort references `test/ref/dp_retime_linux.inc` (a mode switch) and `test/ref/dp_train_linux.inc` (link training against simulated monitors). |
| `tools/linux/linux-capture.sh` | Ground-truth capture of amdgpu's display programming on Linux, for the mode-set engine. |
| `Info.plist` | Lilu plugin personality (`IOResources`), OSBundleLibraries (Lilu, IOPCIFamily, KPIs). |
| `Makefile` | Cross-compiles x86_64 on any host, assembles the `.kext`. |
