# Spike: GPU hardware acceleration for the RX 9070 XT on macOS

*2026-09-27. Question: what would it take to get hardware acceleration on the
RX 9070 XT (Navi 48, gfx1201, GC 12.0.1) under x86_64 macOS Tahoe 26, and can
anything from AsahiLinux help? Built on a deep-research run (20 primary
sources, 25 claims adversarially verified: 19 confirmed, 6 refuted). Effort
figures are judgement, not measurement.*

## Answer

| Kind of acceleration | Verdict | Effort (one developer) |
|---|---|---|
| **Compute** (ML, OpenCL-style kernels) | **Realistic.** Already proven on this very die from macOS, just not on an x86 Hackintosh yet | months |
| **Vulkan** (apps written for Vulkan) | Possible: a port of Mesa RADV onto a macOS kernel interface | 6–12+ months, uncertain |
| **Metal** (the macOS desktop, Safari, most apps) | Multi-year research, no public groundwork, likely past Tahoe's support window | years |

- **AsahiLinux:** no reusable *code*. Asahi drives Apple's AGX GPU under Linux,
  the opposite direction, and AGX is run almost entirely through a firmware
  coprocessor, unlike AMD. Its *methods* do carry over (see below).
- **The surprise:** RDNA4 compute on macOS without any Apple or AMD driver
  already works.
  - [lemonade-sdk/mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu)
    runs Qwen3 27B (~17.5 tok/s) on a Radeon AI PRO R9700, which is the same
    Navi 48 die as the RX 9070 XT, via a PCIDriverKit dext.
  - [tinygrad](https://docs.tinygrad.org/tinygpu/)'s userspace "AM" driver
    (the TinyGPU app) runs RDNA3/RDNA4 cards from macOS over USB4.
  - Both are on Apple Silicon with Thunderbolt eGPUs. Nobody has done it on an
    internal x86 PCIe card.
- **PSP-authenticated firmware loading is not a blocker.** Both projects hand
  AMD's stock, signed linux-firmware blobs to the card's security processor.
  The PSP does the authenticating, so nothing has to be signed by us.

## What AsahiLinux offers

**Transferable (methods):**
- **Host-driven, prototype-first workflow.** Asahi's first kernel driver was a
  Python prototype on a separate machine, driving the GPU over USB with
  hardcoded structures, until it drew a first triangle. The Rust driver came
  after that. tinygrad's AM driver follows the same pattern for AMD.
  ([Tales of the M1 GPU](https://asahilinux.org/2022/11/tales-of-the-m1-gpu/))
- **IOKit user-client tracing.**
  [`wrap.dylib`](https://github.com/AsahiLinux/gpu) is injected with
  `DYLD_INSERT_LIBRARIES` and uses `DYLD_INTERPOSE` to log every
  `IOConnectCall*Method`, `IONotificationPort*` and `IODataQueue*` call. That
  is how Asahi mapped Apple's undocumented GPU kernel interface: memory
  allocation, command buffer creation and submission.
  ([Rosenzweig, part 1](https://alyssarosenzweig.ca/blog/asahi-gpu-part-1.html))
  - The same hook could capture how Metal talks to Apple's AMD driver, but only
    on a machine where that driver runs, i.e. an RDNA2 or older card.
  - That card isn't available here (a second GPU is ruled out).
  - It needs SIP/AMFI relaxed.

**Not transferable:**
- The AGX Rust driver and the firmware (RTKit/ASC) knowledge.
- The AGX command decoder.
- The Honeykrisp/asahi Mesa drivers.
- Refuted in verification: "m1n1 tracing is the most transferable technique",
  and "the AGX selector table is a general IOAccelerator template".

## Prior art for RDNA4 outside amdgpu

**Firmware loading (PSP)**, from mac-amdgpu `psp_v14_0.cpp` and tinygrad `am/ip.py`:
1. Stage the sOS components through the bootloader mailbox (`C2PMSG_35/36`,
   MPASP prefix on MP0 14.x). Skip this if sOS is already alive (`C2PMSG_81`).
2. Create a KM ring, then send `GFX_CMD_ID_LOAD_IP_FW` once per firmware.
3. The gfx1201 set: `psp_14_0_3_sos`, `smu_14_0_3`, `sdma_7_0_1`,
   `gc_12_0_1_{rlc,imu,pfp,me,mec,uni_mes}`.
4. mac-amdgpu's blob hashes match upstream linux-firmware byte for byte.
   R9700 rev 0xC8 boards need the `*_kicker` variants.

**Bring-up order** (tinygrad AM, tested on 4× RX 9070):
1. The order is SOC → GMC → IH → PSP → SMU → GFX → SDMA.
2. If a previous session left the card up, only GFX and SDMA are redone.
3. RDNA4 has its own paths, not RDNA3's:
   - separate PFP/ME RS64 ucode alongside MEC;
   - IMU firmware;
   - nbif instead of nbio;
   - GC12-specific doorbells, PDE/PTE formats and MEC pipe reset.

**Compute queues without MES:** tinygrad writes a `v12_compute_mqd` straight
into the `CP_HQD_*` registers and sets `CP_HQD_ACTIVE`; its gfx1201 firmware
list has no MES blob at all. mac-amdgpu uses MES instead, and both work.

**The macOS side needs only a thin driver.**
- TinyGPU's dext is about 220 lines. It enables memory and bus mastering, maps
  the BARs to userspace and returns DMA addresses. It has no AMD code and no
  interrupts; all GPU work runs in userspace.
- Catches:
  - Apple grants the DriverKit PCI entitlement, and only for AMD/NVIDIA vendor
    IDs. The alternative route needs SIP off.
  - There is no BAR resize.
  - It has only been tested on eGPUs.

**Shader compilers:**
- Upstream LLVM's AMDGPU backend supports gfx1201 (the docs name the
  RX 9070 XT). Its OS ABIs are amdhsa, amdpal and mesa3d, none of them macOS,
  so a macOS driver brings its own loader.
- tinygrad already compiles and loads `amdgcn-amd-amdhsa` code objects from
  macOS. Apple's Xcode clang lacks the AMDGPU target, so upstream LLVM has to
  be built separately.
- Compiling Apple AIR (Metal shaders) to gfx1201 is unsolved.

## What Metal would need

Apple's RDNA2 accelerator kext `AMDRadeonX6000` (still in macOS 26.5/26.6)
subclasses `IOAcceleratorFamily2`. It publishes `MetalPluginName`,
`MetalPluginClassName`, `IOGLBundleName`, `IOOCDBundleName`,
`MetalStatisticsName` and `GPURawCounterBundleName`
([symbols](https://github.com/blacktop/symbolicator/tree/main/kernel/25.5/kexts)).
An RDNA4 Metal stack would need all of the following:

1. an `IOAcceleratorFamily2` subclass publishing those keys;
2. the user-client ABI behind them, which is undocumented, with no public
   reverse engineering found;
3. a userland Metal driver bundle (the role `AMDMTLBronzeDriver` plays for
   RDNA2);
4. an AIR → gfx1201 shader compiler, for graphics as well as compute;
5. WindowServer / IOSurface integration.

Translation layers don't help: MoltenVK and KosmicKrisp run Vulkan *on top of*
Metal, so they need a working Metal driver underneath.

## Vulkan via RADV

RADV is userspace and all its kernel interaction goes through one
Linux-amdgpu winsys ([docs](https://docs.mesa3d.org/drivers/radv.html)). A
port needs:
- a kext user client with amdgpu-like calls (BO alloc, VA map, CS submit,
  syncobj) or a new winsys behind `ac_drm_*`;
- changes to `ac_linux_drm.c`, `vk_drm_syncobj` and the meson platform gating;
- a Darwin WSI that presents to an IOSurface or the RDNA4FB scanout;
- a GFX ring, not just compute queues.

The payoff is limited to Vulkan apps; the macOS desktop stays unaccelerated.

## Blockers, ranked

1. **Metal's private interfaces** (the whole list above), with no public
   groundwork and no way to trace Apple's AMD driver on this machine.
2. **Coexisting with our display driver.**
   - tinygrad AM's full boot does an **SMU mode1 reset** when PSP and SMU are
     alive, which is exactly the state after the VBIOS POSTs the card.
   - It also reprograms **GMC and IH**, which the DCN scanout depends on.
   - Either would likely blank the screen RDNA4FB drives, unless a no-reset
     partial init works, or RDNA4FB re-lights the display afterwards (DMUB
     included). This is inferred, not observed.
3. **Platform plumbing.**
   - The DriverKit entitlement: SIP off, or fold a BAR/DMA user client into
     our Lilu plugin instead.
   - No interrupt path in the reference drivers.
   - No BAR resize: the 256 MiB aperture versus 16 GiB of VRAM.

PSP firmware loading, the blocker originally feared most, is solved.

## Recommended path

Aim for **compute first**, as a separate component next to RDNA4FB, and keep
the display driver the one thing that must never break.

| Phase | What | Estimate |
|---|---|---|
| 0 | On the Linux dual boot: unbind amdgpu and run tinygrad AM on the RX 9070 XT. Record the IP versions and whether the POSTed card leaves sOS alive (decides whether the mode1 reset can be skipped). | 1–3 days |
| 1 | On Tahoe: read-only probe. A BAR-map/DMA user client inside the Lilu plugin (or TinyGPU's SIP-off dext on x86); confirm the display survives. | 2–6 weeks |
| 2 | Compute bring-up **without reset**: GFX + SDMA firmware through PSP `LOAD_IP_FW`, one compute queue programmed directly (no MES), an SDMA copy, an amdhsa kernel from upstream LLVM. If the user client mimics TinyGPU's, tinygrad may run unmodified. | 1–3 months |
| 3 | Usable compute: interrupts or polling, per-process VM, a small runtime API. Still invisible to the macOS graphics stack. | 2–4 months |
| 4 | Vulkan via a RADV port, if a real workload needs it. | 6–12+ months |
| 5 | Metal. | multi-year |

**Where the emulator fits.** The QEMU model (`emu/`) can grow the PSP/SMU
mailboxes and MMIO protocol so Phase 1–2 plumbing is exercised without
reboots. It cannot execute GPU work, so dispatches need the real card.

## Open questions

1. Can GFX/SDMA firmware be loaded through the already-alive PSP on a POSTed
   RX 9070 XT, without the mode1 reset and GMC/IH re-init? (Phase 0 answers
   this.)
2. Can a DriverKit PCI dext bind on x86 Tahoe to an internal GPU that
   IONDRVFramebuffer already drives? Or is a user client in the Lilu plugin
   simpler?
3. Is tracing Apple's AMD Metal stack worth borrowing an RDNA2 machine for?
   Could Apple's GFX10 Metal bundle ever sit on a kext that emulates the
   AMDRadeonX6000 ABI? Unlikely, given the GFX10 → GFX12 changes in PM4 and
   ISA.
4. Would any workload that matters here actually use Vulkan rather than Metal?

## Caveats

- All macOS RDNA4 compute evidence is Apple Silicon + Thunderbolt/USB4.
- mac-amdgpu is small (10 stars) and its results are self-reported, though its
  code, logs and firmware hashes agree.
- Refuted in verification (don't cite):
  - m1n1 tracing as the top technique;
  - DYLD tracing "carrying over directly" to Apple's AMD drivers;
  - AGX selectors as a general template;
  - Honeykrisp being derived from NVK;
  - specific RLC-autoload polling details;
  - "no gfx11/12 classes in Apple's 26.5 AMD kexts" (unconfirmed, 1–2).
- Unresearched: AMDMTLBronzeDriver internals, the MTLCompiler plugin model,
  Mesa ACO GFX12 status.
- This area is moving fast: mac-amdgpu was pushed 2026-09-26, and tinygrad's
  RDNA4 mode1-reset commit landed 2026-09-24.

## Sources

- AsahiLinux: [gpu (wrap.dylib)](https://github.com/AsahiLinux/gpu),
  [Tales of the M1 GPU](https://asahilinux.org/2022/11/tales-of-the-m1-gpu/),
  [AGX docs](https://asahilinux.org/docs/hw/soc/agx/),
  [Asahi GPU part 1](https://alyssarosenzweig.ca/blog/asahi-gpu-part-1.html)
- RDNA4 without amdgpu: [lemonade-sdk/mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu),
  [tinygrad am/ip.py](https://github.com/tinygrad/tinygrad/blob/master/tinygrad/runtime/support/am/ip.py),
  [am/amdev.py](https://github.com/tinygrad/tinygrad/blob/master/tinygrad/runtime/support/am/amdev.py),
  [TinyGPU docs](https://docs.tinygrad.org/tinygpu/)
- macOS: [kext symbols 25.5](https://github.com/blacktop/symbolicator/tree/main/kernel/25.5/kexts),
  [OCLP patch notes](https://github.com/dortania/OpenCore-Legacy-Patcher/blob/main/docs/PATCHEXPLAIN.md)
- Compilers / APIs: [LLVM AMDGPUUsage](https://llvm.org/docs/AMDGPUUsage.html),
  [RADV](https://docs.mesa3d.org/drivers/radv.html),
  [KosmicKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html),
  [MoltenVK](https://github.com/KhronosGroup/MoltenVK)
