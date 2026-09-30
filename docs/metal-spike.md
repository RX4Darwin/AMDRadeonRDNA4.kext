# Metal spike: what Apple's AMD stack is, which routes lead to a Metal driver for gfx1201, and what "ready to start" means

Written 2026-09-30 for hub-task-352. **Read-only research: no GPU was used, no kext code was written, the stick was not touched.**
Builds on `docs/spike-hardware-acceleration.md` (2026-09-27, compute/Vulkan/Metal verdicts) and `docs/metal-readiness.md` (branch `premetal/metal-ready`).

Evidence tags, used strictly:

- **[MEASURED]** I ran it in this session (7z/python/llvm-mc/strings on the files named). Commands are in appendix A.
- **[SOURCED]** read at the cited URL or path by me or by one of three research sub-agents (their claims are marked as theirs where I did not re-read the source).
  Sub-agent findings I could not re-check are **[SOURCED, agent]**.
- **[INFER]** judgement, with the reason.
- **[UNKNOWN]** looked for, not found.

## 0. Answer in one page

| Route | What it is | Verdict for gfx1201 | Effort (all [INFER]) |
|---|---|---|---|
| **A** reuse Apple's RDNA2 Metal bundle (`AMDRadeonX6000MTLDriver`), translate its PM4 + ISA to gfx12 under our kext | impostor for `AMDRadeonX6000`'s kernel side + PM4/ISA/descriptor translation | **Compute subset: plausible. Graphics: not realistic as a translator.** ISA is not binary compatible (section 3.1), descriptors and tiling differ, and the kernel-side user-client protocol of `AMDRadeonX6000` is untraced | compute: 2-4 months after the protocol is recovered; graphics: rewrite-class (6+ months) |
| **B** our own Metal device bundle (`MTLIOAccelDevice` subclass) + `IOAccelerator` kernel classes + our compiler | the contract is Apple's SPI (hundreds of properties) but we pick the kernel protocol | **The real route.** The one prerequisite nobody has: an AIR -> gfx12 compiler (LLVM's AMDGPU backend supports gfx1201, AIR is LLVM bitcode, so the work is lowering Apple's intrinsics and resource model, not a back end) | compute-only device: 6-12 person-months; a device WindowServer can composite on: 1-2+ years |
| **B'** fake Apple's paravirtual GPU (`AppleParavirtGPU`) so Apple's own stock "paravirt" Metal device works, and run its command stream on RDNA4 | the route VM guests use; AIR is translated on the "host" side (here: us) | **Worth one cheap probe, then decide.** Not in Recovery's kernel collection [MEASURED]; presence in the full x86 install is [UNKNOWN] | 1-2 person-years (agent estimate, project reims-vgpu is alpha) |
| **C** Vulkan (RADV with an IOKit winsys) and/or Metal-on-Vulkan | apps that speak Vulkan; Metal only per-process | Vulkan: tractable (our runtime already has BO/VA/submit/fence); **does not give the desktop** (WindowServer composites with Metal) | Vulkan compute+gfx: 3-6 months; Metal layer on top: years |

**Recommendation.** Plan on **B**, keep **A** only as a compute accelerator idea, and spend the first three experiments (section 5) on the questions that decide between B and B' without touching the card: (1) an impostor `IOAccelerator` census in the emulator, (2) a static survey of the full Tahoe x86 install, (3) a gfx1030 -> gfx1201 compute translation proof on the card (needs the lead's GPU OK).
"Ready to start the Metal phase" is defined in section 6.

## 1. What Apple's AMD stack consists of

### 1.1 What is in the stick's Recovery image [MEASURED]

`~/work/tools/emu/images/BaseSystem.img` (converted from Kiln's `BaseSystem.dmg`), listed with `7z l` and parsed read-only. **macOS 26.6.2, build 25G83** (`System/Library/CoreServices/SystemVersion.plist`).

- The Recovery image has **no AMD kext or bundle as files**: `System/Library/Extensions` holds 216 top-level entries (211 `.kext`) and none is `AMD*` (a case-insensitive grep for `AMD`/`ATI`/`Radeon` hits only `BridgeAudioCommunication` and `SEPHibernation`; the only other "AMD" text in the whole listing is an unrelated sandbox profile name). Everything Recovery loads comes from two kernel collections, `System/Library/KernelCollections/BootKernelExtensions.kc` (67.6 MB) and `BaseSystemKernelExtensions.kc` (22.3 MB, bundle list in `.kc.bundles`).
- `BaseSystemKernelExtensions.kc` is a Mach-O fileset with 95 entries. The graphics-relevant ones, with the prelinked Info.plist data:

| kext (bundle id) | version | what it is |
|---|---|---|
| `com.apple.iokit.IOAcceleratorFamily2` | 487.4.3 | the accelerator family (section 1.3) |
| `com.apple.iokit.IOGraphicsFamily`, `IONDRVSupport` | - / 600 | framebuffer base; IONDRV is what RDNA4FB hooks |
| `com.apple.kext.AMDSupport` | 7.0.1 (src 7.1.6, build 24649) | shared helper; matches any AMD PCI vendor id (`IOPCIMatch 0x00001002&0x0000FFFF`); `GPUCompanionBundles`: `AMDShared.bundle`, `AMDShared800.bundle`, `AMDRadeonX5000Shared.bundle`, `AMDRadeonX5000Shared800.bundle` |
| `com.apple.kext.AMDRadeonX6000Framebuffer` | 7.0.1 (7.1.6) | **display only** for Navi 1x/2x: classes `AMDRadeonX6000_AmdRadeonControllerNavi10/12/14/21/23`, `AMDRadeonX6000_AmdGpuWrangler`; 24 PCI ids (0x7310-0x731F, 0x7340-0x734F, 0x7360/62, 0x73A0-0x73AF, 0x73BF, 0x73E0/E3, 0x73FF); companions `AMDShared.bundle`, `AMDRadeonX6000Shared.bundle` |
| `com.apple.kext.AMD9500Controller`, `AMD10000Controller`, `AMDFramebuffer` | 7.0.1 | Polaris (0x67xx) / Vega (0x68xx, 0x69Ax, 0x66Ax) / generic display |

- **Not present in Recovery:** the accelerator kext `AMDRadeonX6000`, `AMDRadeonX6000HWServices`, every `*HWLibs`, and every user-space bundle (`AMDRadeonX6000MTLDriver`, `AMDMTLBronzeDriver`, `AMDShared`, GL drivers, VA drivers). Recovery therefore has display but no accelerator; this is also why it is a good place for the emulator loop.
- **No match for Navi 48** (device id 0x7550 / 0x7551) in `AMDRadeonX6000Framebuffer`'s 24 ids. Stock macOS will not bind any AMD kext to the RX 9070 XT.
- The Metal stack itself is in the dyld shared cache (`System/Library/dyld/dyld_shared_cache_x86_64` 761 MB + `.01` 232 MB; its `.map` lists 719 images) and includes `Metal.framework`, `IOAccelerator.framework`, `IOSurface`, `GPURawCounter`, `GPUWrangler`, `SkyLight`, `QuartzCore`, `CoreImage`, `OpenGL`, `OpenCL`. There is **no `AppleParavirtGPU` kext** in the base collection and none in the file list (`AppleVirtualGraphics` is there, plus a `ParavirtualizedGraphicsGPUTask` sandbox profile).
- `MTLCompiler`, `GPUCompiler` and the other compiler frameworks are not in Recovery's cache image.

### 1.2 The full install (what Tahoe ships for RDNA2) [SOURCED]

Tahoe 26.x is the last macOS release for Intel Macs ([Dortania Tahoe page](https://dortania.github.io/OpenCore-Install-Guide/extras/tahoe.html); its supported list has the AMD models MacPro7,1 and iMac20,x), and Apple still ships the AMD stack up to RDNA2.
**There is no full macOS install on this machine** (disk survey: only the OpenCore stick, Linux and a Windows/BitLocker disk), so none of the following could be read locally; it comes from public listings.

- **Kexts** (25.5 signature data, [blacktop/symbolicator kernel/25.5/kexts](https://github.com/blacktop/symbolicator/tree/main/kernel/25.5/kexts), [SOURCED, agent]): `AMDRadeonX4000/X5000/X6000` (+ `HWServices`, `HWLibs` per ASIC: X6000 covers X6100/6200/6300/6700/6800/6810 HWLibs), `AMDRadeonX6000Framebuffer`, `AMDSupport`, `AMDRadeonServiceManager`, `AMD9500/10000Controller`. **No X7000 or later.**
- `AMDRadeonX6000` holds the accelerator classes (`AMDGFX10GraphicsAccelerator`, `AMDNavi21Hardware`, `AMDPM4HWEngine`, `AMDUserQueue`, `AMDHWVMContext`, `AMDAccelCommandQueue`, `AMDAccelSharedUserClient`, ...). Its personalities match `IOPCIMatch` + `IOPropertyMatch {LoadAccelerator=true}`, `IOMatchCategory IOAccelerator` ([OCLP PatcherSupportPkg, 12.5 copy](https://github.com/dortania/PatcherSupportPkg/tree/main/Universal-Binaries/12.5/System/Library/Extensions)) [SOURCED, agent].
- **User-space bundles**, all under `/System/Library/Extensions` ([ipsw-diffs macOS 26](https://github.com/ipsw-diffs/macos-26), [blacktop/ipsw-diffs](https://github.com/blacktop/ipsw-diffs)) [SOURCED, agent]:
  - Metal drivers: `AMDMTLBronzeDriver.bundle` (GCN/Polaris), `AMDRadeonX5000MTLDriver.bundle` (Vega), `AMDRadeonX6000MTLDriver.bundle` ("AMD GFX10 Metal Driver", classes `GFX10_MtlDevice` / `AMDMTLGFX10Device`, `GFX10_*` and `amdMtl_*` functions for PM4 draw/dispatch, blit, heaps, textures, acceleration structures).
    It imports only libSystem/libc++/libobjc and **consumes a `GFX10_PackedBinaryRec`** (ISA + relocations + symbols + strings) through `-[GFX10_MTLLibrary initWithDevice:packedBinary:]`: it does not compile.
  - GL: `AMDRadeonX4000GLDriver`, `X5000GLDriver`, `X6000GLDriver`. Video: `AMDRadeonVADriver`, `VADriver2`.
  - Compiler-side: `AMDShared.bundle`, `AMDRadeonX5000Shared.bundle`, `AMDRadeonX6000Shared.bundle` (the last is the GFX10 compiler plug-in; `AMDShared` has `PlugIns/libAMDNTPlugin.dylib`, `libSC.dylib`, `libAMDIL902.dylib`, `libGCNASM.dylib`).
- A third-party repository that appeared 2026-09-28 (**[Almosst-DEV/Navi48-MacOS](https://github.com/Almosst-DEV/Navi48-MacOS)**, one commit, no README, authors unknown to me) claims a macOS x86 bring-up for the same card and ships a tool that swizzles `-[GFX10_GfxMtlFunctionVariant initWithCompilerOutput:...]` in `AMDRadeonX6000MTLDriver` on **26.6.2 (25G83)**, the same build as our Recovery image, to dump Apple's `GFX10_PackedBinaryRec` and the derived HW shader record for SkyLight's `UberCompositeVertex` / `SimpleTextureFragment`, "userspace only, no GPU submission". Its comments cite design notes that are not in the repo.
  I read only the tool's header comments [SOURCED]. It is evidence that (a) someone has run Apple's GFX10 Metal device on a Navi2x PC on this exact OS build, (b) the GFX10 driver's internal record layouts are reachable from user space, (c) the window-server composite shaders are two small shaders. **Treat all of it as unverified third-party claims; do not copy code.**

### 1.3 IOAcceleratorFamily2: what a vendor accelerator plugs into [MEASURED: class and symbol names from the kernel collection; the roles are [INFER]]

`IOAcceleratorFamily2` (487.4.3, present in Recovery's collection, so our kext can link against it [INFER: not tried]) exports the generic classes; the vendor kext subclasses them. Method names below are from `strings | c++filt` on the collection (appendix A); no signatures or layouts were recovered.

| Class | Methods visible in the symbol table (selection) | Role |
|---|---|---|
| `IOGraphicsAccelerator2` (base of the vendor accelerator; superclass `IOAccelerator`, public header `IOKit/graphics/IOAccelerator.h`) | `newUserClient`, `newSharedUserClient`, `newDevice`, `newContext`, `newCommandQueue`, `newShared`, `newDrawable`, `newDisplayPipe`, `newStatistics`, `newSurfaceMTL`, `createResource`, `createVidMemory`, `createSysMemory`, `createEventMachine`, `createDMACommandPool`, `createMemoryDescriptorWithAddressRange`, `systemWillSleep/DidWake`, `reportGPURestart`, `calcMaxGPUPhysicalMemoryBytes`, `getMaxResourceSize`, ... | the factory: the vendor overrides these to hand out its own user-client/context/queue classes |
| `IOAccelSharedUserClient2` | `new_resource`, `delete_resource`, `get_resource_info`, `create_shmem`, `destroy_shmem`, `create_mtlevent`, `allocate_fence_memory`, `set_resource_purgeable`, `setup_dirty_ring`, `process_dirty_commands`, `externalMethod` (static table `sSharedMethods`) | per-process resources, shared memory, events |
| `IOAccelDevice2` | `get_device_info`, `get_config`, `get_name`, `get_event_machine`, `set_api_property`, `sDeviceMethods` | per-process device object |
| `IOAccelCommandQueue` | `submit_command_buffers`, `process_command_buffer`, `processKernelCommands`, `parseSegmentList`, `processAndSubmitCoalescedSegments`, `retireCommandBuffer`, `set_priority_and_background`, `set_quality_of_service`, `sCommandQueueMethods` | Metal command-queue submission: **the kernel parses a segment list and "kernel commands" out of shared memory** |
| `IOAccelContext2` (+ `IOAccelGLContext2`, `IOAccelCLContext2`) | token stream: `process_token_BindDataBuffer/End/Start/SetBlockFence/ResourceRelease`, `processSidebandBuffer`, `getDataBuffer`, `beginCommandStream`, `sContextMethods` | legacy GL/CL submit path |
| `IOAccelResource2` | `newResourceWithIOSurface`, `newResourceWithClientBuffer`, `allocBackingMemory`, `pageon/pageoff`, `setPurgeable`, `getPhysicalOffset`, `lockForCPUAccess`, ... | resources incl. IOSurface-backed ones |
| `IOAccelSubmitter2`, `IOAccelShared2`, `IOAccelMTLEvent2`, `IOAccelDisplayPipe*`, `IOAccelMemoryInfoUserClient` | ... | submitter base, per-process shared state, MTLEvent ring, display pipe, memory info |

User side, the private `IOAccelerator.framework` exports the C API that Metal's `MTLIOAccel*` classes call [MEASURED: `IOAccelerator.tbd` in the SDK]: `IOAccelDeviceCreate/GetConfig`, `IOAccelContextCreate`, `IOAccelCommandQueueCreate(WithQoS)` / `IOAccelCommandQueueSubmitCommandBuffers`, `IOAccelResourceCreate` / `GetGPUVirtualAddress` / `DirtyBufferRange` / `ResourceList*`, `IOAccelDisplayPipe*`, `IOAccelGLContext*`, `IOAccelCLContext*`.
`Metal.framework` itself contains the generic `MTLIOAccel*` implementation (`MTLIOAccelDevice`, `CommandQueue`, `CommandBuffer` with `...CommandBufferStorage{Create,BeginSegment,EndSegment,BeginKernelCommands,...}`, `Buffer`, `Texture`, `Heap`, `Fence`, all encoders, `DeviceShmem`) [MEASURED: `Metal.tbd`]. The vendor bundle subclasses `MTLIOAccelDevice` and the encoders; Apple's base builds the shared-memory command buffer and calls the vendor for the hardware-specific parts.

**No headers exist** for any of these except the old public `IOAccelerator.h`/`IOAccelTypes.h`/`IOAccelClientConnect.h` ([MEASURED]: the SDK and `MacKernelSDK` contain only those four). Class layouts, vtable order and the selector tables have to be recovered from the binaries; the vtable order is recoverable statically from the kernel collection.
Selector tables: known only for the Intel-era generic classes (user-client types 0 Surface, 1 GLContext, 2 2DContext, 4 DisplayPipe, 5 SharedUserClient, 6 Device, 7 MemoryInfo, 8 CLContext, 9 CommandQueue, 0x100 VideoContext: [CanSecWest 2016 "Apple Graphics Compromised"](https://papers.put.as/papers/macosx/2016/CanSecWest2016_Apple_Graphics_Compromised.pdf) pp. 8-10) [SOURCED, agent]; **current tables for CommandQueue/Event/Submitter/MemoryInfo and the AMD subclasses: [UNKNOWN]**.

### 1.4 How the OS picks the accelerator and its bundles [SOURCED, agent]

1. IOKit matching: the accelerator personality matches `IOPCIDevice` by `IOPCIMatch` (explicit device-id whitelist) and `IOPropertyMatch {LoadAccelerator=true}`; `HWServices` (`LoadHWServices`) and the framebuffer controller (`LoadController`) match the same ids.
2. The bundle names are **not** in the personality; the accelerator class sets them at `start` (`setDriverBundleNames()`), as registry properties on the `IOAccelerator` node. Values from a live Navi14 registry dump ([diag file](https://github.com/hoOJluGun/agent-mode/blob/main/preserve_store/manifests/driver_readonly_diag_20260605_001003.txt), OS version [UNKNOWN]): `IOClass AMDRadeonX6000_AMDNavi14GraphicsAccelerator`, `MetalPluginName AMDRadeonX6000MTLDriver`, `MetalPluginClassName AMDMTLGFX10Device`, `MetalStatisticsName GFX10Statistics`, `IOGLBundleName = IOOCDBundleName AMDRadeonX6000GLDriver`, `IODVDBundleName AMDRadeonVADriver2`, `GPURawCounterBundleName AMDRawCounterPlugin`, `GPURawCounterPluginClassName AMDRawCounterSourceGroup`, `IOAccelRevision 2`.
   Polaris: `MetalPluginName AMDMTLBronzeDriver`, `MetalPluginClassName BronzeMtlDevice` ([DarwinDumped](https://github.com/khronokernel/DarwinDumped/blob/master/iMac/iMac18,3/IORegistry/IOregViewer/Resources/dataFiles/IOService/data183.txt)).
3. Metal enumerates `IOAccelerator` services (`+[MTLIOAccelDevice registerDevices]`, `MTLIOAccelService`, registry-id map, log messages "MTLIOAccelDevice bad MetalPluginClassName property (null)" / "Zero Metal services found") ([header dump of 26.4](https://github.com/thatmarcel/macOS-26.4-headers/blob/main/headers/Metal/MTLIOAccelDevice.h), [Apple forum 689424](https://developer.apple.com/forums/thread/689424)), then loads the named bundle and instantiates the class. WhateverGreen sets/removes the same three properties to steer Metal ([kern_ngfx.cpp](https://github.com/acidanthera/WhateverGreen/blob/master/WhateverGreen/kern_ngfx.cpp)).
   **Where the loader looks for the bundle and whether a path outside the sealed system volume is honoured on Tahoe: [UNKNOWN]** (a stub project lists it as open too: [winkleplex10-bit/MetalGPUDrivers](https://github.com/winkleplex10-bit/MetalGPUDrivers/blob/HEAD/ihv/amd-rdna2-igpu/metal/README.md)).
4. A second, separate Metal device stack exists for Apple silicon (`IOGPUMetalDevice`, IOGPU.framework) ([header](https://github.com/thatmarcel/macOS-26.4-headers/blob/main/headers/IOGPU/IOGPUMetalDevice.h)); Intel/AMD/NVIDIA use `MTLIOAccelDevice` [INFER: the dump does not say so].
5. Metal 4 needs Apple silicon ([Apple 102894](https://support.apple.com/en-us/102894)); an x86 device stays on Metal 3.

## 2. How Metal compiles shaders

- Offline: MSL -> **AIR** (LLVM bitcode, magic `0x0b17c0de`, triple `air64-apple-macosx...`, `air.*` metadata/intrinsics) by Xcode's `metal`; `.metallib` is a custom container ("MTLB" header; function list, metadata, bitcode) ([worthdoingbadly](https://worthdoingbadly.com/metalbitcode/), [zhuowei/MetalShaderTools](https://github.com/zhuowei/MetalShaderTools), [YuAo/MetalLibraryArchive](https://github.com/YuAo/MetalLibraryArchive)). **AIR is not documented by Apple**; it is community-RE'd only.
- On device, at pipeline creation: Metal.framework -> XPC `MTLCompilerService` -> `MTLCompiler.framework/Versions/<build>/MTLCompiler` (Tahoe ships builds 32023 and 32024) -> `GPUCompiler.framework` (`libLLVM`, `libGPUCompiler`) -> **vendor plug-in** ([Khronokernel](https://khronokernel.com/macos/2022/11/01/LEGACY-METAL-PART-2.html), [ipsw-diffs](https://github.com/ipsw-diffs/macos-26)) [SOURCED, agent].
  `MTLCompiler.framework` exports `_MTLCodeGenServiceCreate / SetPluginPath / BuildRequest / Destroy` and `GPUCompiler.framework` exports the `_MTLGPUArchiver*` family with `SetTarget` [MEASURED: SDK `.tbd` files]: the plug-in path is a parameter, which is the most promising hook for a replacement compiler (whether it is honoured for a third-party path is [UNKNOWN]).
- **AMD's plug-in** [SOURCED, agent]: `libAMDNTPlugin.dylib` (exports `MTLCompilerCreate`, `MTLCompilerBuildRequest*`, `amd_getStageMetadata`, `amd_hasValidTargetTriple`; dispatches by family "Bronze"/"GFX9"/"GFX10") which loads `AMDRadeonX6000Shared.bundle` for GFX10: **AIR -> AMDIL -> AMD's proprietary SC (`libSC.dylib`) -> GCN/RDNA ISA**, serialised into the `GFX10_PackedBinaryRec` the Metal driver loads. Xcode 16 beta 5's copies of these name targets only up to gfx1032 and contain `SCGfx10/101/103Emitter` but **no gfx11/gfx12 emitter** (from strings of [keith/Xcode.app-strings](https://github.com/keith/Xcode.app-strings); Tahoe's own binaries were not inspected). **Metal will not compile anything for gfx1201 with Apple's pieces.** Intel uses `libMTLIGCCompilerPlugin` + Intel's open IGC; Apple silicon uses `libapplegpu-nt`.
- Open tools that matter for a replacement compiler [SOURCED, agent]: upstream LLVM AMDGPU supports gfx1201 (`llvm-mc` here is 22.x and assembles it: [MEASURED]); [metal2vulkan](https://github.com/steelbrain/metal2vulkan) (AIR -> SPIR-V, used by reims-vgpu) and Darling's Iridium (AIR -> SPIR-V) show that AIR can be consumed by third-party code.

## 3. The routes

### 3.1 Route A: Apple's RDNA2 Metal bundle, translated

What must be true: Apple's `AMDRadeonX6000MTLDriver` runs in user space against an impostor `IOAccelerator` that speaks `AMDRadeonX6000`'s kernel protocol, and everything it emits (PM4, ISA, descriptors, tiled surfaces) is rewritten for gfx12. Apple's `libSC` would still compile AIR to gfx10.3 ISA (it has no gfx12 emitter), so the ISA is gfx10.3 bytes that must be translated.

**ISA differences that break byte-copying** (sub-agent run with `llvm-mc` 22.1.8, plus my re-checks):

- The same bytes decode as different instructions. [MEASURED by me] gfx1030 `s_endpgm` `BF810000` is `s_setkill 0` on gfx1201 (this is the bug fixed in `vmtest.cpp` on `premetal/w13`); `s_barrier` `BF8A0000` is `s_wait_idle`; `BE800301` (`s_mov_b32`) is `s_cmov_b64`; `s_barrier` does not assemble for gfx1201 (`s_barrier_signal -1` + `s_barrier_wait -1`); `v_fma_f32` is `D54B0000` on gfx1030 and `D6130000` on gfx1201.
- [SOURCED/measured, agent] Opcodes renumbered: SOP1 62 of 63, SOPP 23 of 24, VOPC 142 of 162, VOP3 169 of 300 differ; VOP1 and DS do not. Removed: SDWA, `s_waitcnt_vscnt`, the `s_cmpk_*` family, VINTRP (replaced by `ds_param_load` + `v_interp_p10/p2`), `exp param*`, `glc/slc/dlc` (now SCOPE/TH), several hwregs. Counters split into `s_wait_loadcnt/storecnt/samplecnt/bvhcnt/kmcnt/dscnt`; legacy `s_waitcnt` still assembles but is "wait for idle". Memory encodings grow (global load 8 -> 12 bytes, MUBUF -> VBUFFER, MIMG -> VIMAGE/VSAMPLE), so **every branch offset changes**.
- [SOURCED/measured, agent] On five small gfx1030 OpenCL kernels 96 % (564/586) of wave32 instructions assembled unchanged for gfx1201 and code grew 3.2 %: the mnemonic layer is easy. The failures were `s_barrier`, SDWA and FLAT_SCR setreg. **The corpus has no graphics, image or scratch code: it says nothing about the hard layers.**
- Compute ABI: gfx10.3 passes workgroup ids in SGPRs after the user SGPRs, plus a tg_size SGPR; gfx12 passes them in `TTMP7/8/9` and packs local ids into `v0` ({Z,Y,X} 10 bits each) ([RDNA4 ISA pp. 43-44](https://gpuopen.com/download/rdna4-instruction-set-architecture.pdf)). A launch shim must unpack them into the registers the old kernel expects (needs free registers: register-allocation knowledge).
- Graphics: gfx12 has only PS/GS/HS waves (NGG always on); the legacy VS/ES/LS paths and their registers are gone. Our own measured facts apply (`docs/metal-readiness.md` s.2, `docs/linux-replay.md`, `docs/g4-colour.md`): **NGG EXEC starts at lane 0** (a shader must SET exec from `merged_wave_info`), VGPR granules 8/4, attribute-ring stores + `ds_param_load` interpolation, `s4` garbage. A gfx10.3 legacy-VS shader must become NGG code with attribute-ring stores: that is a compiler back-end, not a rewrite. Whether Apple's GFX10 driver uses NGG or legacy VS is [UNKNOWN] (the third-party tool says the bit is read from `HwInfoRec+0xcc` and calls it unsettled).
- State and descriptors [SOURCED/measured, agent, from Mesa 26.2 register JSON]: 60 % (279/463) of context registers present in both generations have different addresses, 177 are gone and 104 new (example: `CB_COLOR0_INFO` 0x28C70 -> 0x28EC0, `VGT_SHADER_STAGES_EN` 0x28B54 -> 0x28A98); `SPI_SHADER_PGM_LO_GS` moved onto what is now `RSRC4_GS`'s address, so a copied write corrupts state. Buffer/image/sampler descriptors have different field layouts; the image **format enum is renumbered** (74 of 104 common formats have different numbers); tiled memory uses `ADDR3_*` swizzles instead of `ADDR_SW_*` (re-tiling needed); DCC/CMASK/FMASK/HTILE are gone (HiZ/HiS, PTE-based compression). Descriptors live in **user memory** (argument buffers, heaps) that the translator cannot distinguish from data without hooking the bundle's writes.
- The `INDIRECT_BUFFER` packet is identical (VMID in [27:24]) and `RELEASE_MEM` keeps its 7-dword layout minus a few cache bits ([SOURCED, agent]); our own `SubmitIb` / W12k paths already use these.

**Kernel side.** The bundle talks to `AMDRadeonX6000`'s user clients (`AMDAccelSharedUserClient`, `AMDAccelCommandQueue`, ...). Their selector tables and argument structs are untraced [UNKNOWN]; they can be read statically from the `AMDRadeonX6000.kext` binary of the full install (not on this machine), or traced at runtime only on a Navi2x box (a second GPU on this machine is ruled out; the third-party repo above suggests someone has one).

**Judgement [INFER].** A byte-level submit-time rewriter is not feasible. Compute: disassemble -> rewrite text -> reassemble with `-mcpu=gfx1201` plus an ABI shim and descriptor rewriting at descriptor-write time is weeks to a few months (sub-agent estimate 2-4 person-weeks for the instruction layer alone, 1-2 months for the compute prolog, 2-3 months for descriptors). Graphics: sub-agent estimate 4-8 months for a working subset, "effectively writing a new compiler back-end"; I agree, and since the repo already has gfx12 NGG/PS shaders and PM4 that run on the card, **for graphics a new AIR -> gfx12 path (route B) is comparable or cheaper than a translator**. Also: Apple's GFX10 driver would still need an impostor for the whole `AMDRadeonX6000` kernel protocol, which is more untraced protocol than route B has to implement (B defines its own).
Precedent for patching Apple's AMD Metal bundle from a Lilu-style kext exists ([Polaris22Fixup](https://github.com/osy/Polaris22Fixup)) but only where ISA/PM4 already match.

### 3.2 Route B: our own Metal device bundle

Components (nothing exists yet; [INFER] unless marked):

1. **Kernel**: an `IOAccelerator`-family service. Subclass `IOGraphicsAccelerator2` (or its public parent `IOAccelerator`, `IOKit/graphics/IOAccelerator.h`) from our Lilu plugin, publish `MetalPluginName` / `MetalPluginClassName` / `MetalStatisticsName` / `IOGLBundleName`, hand out user clients that Metal's `MTLIOAccel*` base can drive (the generic `IOAccel*2` classes are already in `IOAcceleratorFamily2`, so the vendor-specific part is smaller than it looks: resources, queues, memory, events, submission). Needs header reconstruction from the collection (class names, virtuals and static method tables are visible: section 1.3). Every `IOGraphicsAccelerator2` virtual we do not override runs Apple's generic code, which assumes hardware behaviours [UNKNOWN].
2. **User bundle**: a `MTLIOAccelDevice` subclass with the `MTLDeviceSPI` contract (about 347 properties and many methods in the 26.4 dump: [header](https://github.com/thatmarcel/macOS-26.4-headers/blob/main/headers/Metal/MTLDeviceSPI-Protocol.h)), encoders (render/compute/blit), resources, heaps, fences, events. A non-nil `targetDeviceArchitecture` is required or `MTLLoader` aborts ([reims-vgpu issue 93](https://github.com/steelbrain/reims-vgpu/issues/93), found on the paravirt device). Nothing public documents these contracts; they change between major releases (OCLP keeps per-major payload suffixes like "12.5-24" for Sequoia: [sys_patch amd_navi.py](https://github.com/dortania/OpenCore-Legacy-Patcher/blob/main/opencore_legacy_patcher/sys_patch/patchsets/hardware/graphics/amd_navi.py)). Tahoe being the last Intel release makes the moving target finite: one OS line to track.
3. **Compiler plug-in**: AIR -> gfx12. Options: (i) lower AIR (LLVM bitcode with `air.*` intrinsics) into upstream LLVM's AMDGPU back end (gfx1201 supported; the job is the intrinsic and resource-binding mapping, plus NGG/attribute handling for graphics, which ACO does in Mesa and LLVM's `amdgpu_gs` convention may or may not handle well: [UNKNOWN]); (ii) AIR -> SPIR-V (`metal2vulkan`/Iridium exist) -> Mesa NIR/ACO (RADV's gfx12 path is the one our Linux ground truth uses); (iii) for early desktop bring-up only, a **whitelist plug-in** that returns prebuilt gfx12 binaries for the handful of shaders WindowServer/SkyLight need, keyed by AIR hash, refusing everything else. (iii) is my proposal, not from any source. Whether a third-party plug-in path is honoured is [UNKNOWN] (experiment 1 tests the loader; `MTLCodeGenServiceSetPluginPath` exists).
4. **Submission**: command buffers -> our PM4 using W12k (`SubmitGfxIb`/`WaitGfxFence`) and `SubmitIb` paths, per-client VM (W13).

Effort [INFER, agrees with the sub-agent's 6-12 person-months for a compute-only device; the desktop-capable device is larger because WindowServer renders with Metal]: compute-only device 6-12 person-months; a device SkyLight can composite on, plus IOSurface scanout through the flip path, 1-2+ years. There is **no oracle GPU** to trace Apple's own AMD driver against (second GPU ruled out), so every behaviour Apple's generic code expects from the vendor has to be discovered by experiment (experiment 1 is designed for that).

### 3.3 Route B': impersonate Apple's paravirtual GPU

`AppleParavirtGPUMetal.bundle` ships with macOS (x86_64 and arm64e slices) and provides a full Metal device for VM guests; `AppleParavirtDevice` needs an `AppleParavirtGPU` IOKit service that bare metal lacks. The open-source [reims-vgpu](https://github.com/steelbrain/reims-vgpu) emulates that device in QEMU so a stock macOS guest gets Metal, decodes Apple's wire format (364 selectors enumerated), and translates the AIR shaders on the host with `metal2vulkan` [SOURCED, agent, alpha quality, LGPL-3.0]. The shape is attractive: Apple's own Metal encoders and compiler-side serializer stay in use, and "the host" would be our kext + a user-space renderer that executes the paravirt command stream on RDNA4.
Blockers: (1) `AppleParavirtGPU.kext` is **not in Recovery's kernel collection** [MEASURED]; whether it exists in the full x86 Tahoe install and binds on bare metal is [UNKNOWN] (experiment 2); (2) the paravirt device is an MMIO/FIFO/page-table device, so the kext would fake a PCI device's semantics in a driver; (3) it needs an AIR -> gfx12 translation anyway (same compiler problem as B); (4) agent estimate 1-2 person-years. It would replace B's user-bundle work (SPI, encoders) by a decoder for someone else's wire format; that trade is worth a decision after experiment 2.

### 3.4 Route C: Vulkan, Metal-on-Vulkan, other

- **RADV with an IOKit winsys.** Mesa's meson sets `_vulkan_drivers = []` on Darwin and RADV needs `libdrm_amdgpu` + the amdgpu winsys ([meson.build](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/meson.build), [radv meson](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/amd/vulkan/meson.build)); no RADV-on-Darwin work was found [UNKNOWN: none]. Precedents for Mesa with a custom kext winsys: NVBringup (NVK compute on Intel macOS 14: [repo](https://github.com/kvarun-p/NVBringup)) and VMQemuVGA (virgl via `IOGLBundleName`, macOS 10.6: [repo](https://github.com/startergo/VMQemuVGA)). Our runtime (alloc, VA map, IB submit, fences, per-client VM) looks like the `radv_amdgpu_winsys` surface [INFER], and RADV's gfx12 path is exactly what produced our Linux ground truth (`tools/radv-triangle`). Effort [INFER]: 3-6 months for Vulkan compute+gfx with a headless or IOSurface-presenting WSI. **It produces Vulkan, not Metal, and WindowServer would still have no accelerated device.**
- **Metal over Vulkan.** Nothing registers a Vulkan-backed `MTLDevice` as a system device on macOS [UNKNOWN: none found]; Darling's Indium/Iridium is a Metal-like API for Linux Darling and not a drop-in; MoltenVK/KosmicKrisp go the other way. Per-process injection of a replacement `MTLCreateSystemDefaultDevice` is plausible for one app [INFER]; the system-wide case needs a real `IOAccelerator` service anyway, i.e. route B.
- **No accelerated MTLDevice at all.** WindowServer's compositor is Metal (`CompositorMetal::composite` in SkyLight: [Khronokernel](https://khronokernel.com/macos/2022/11/01/LEGACY-METAL-PART-2.html)); OCLP's non-Metal support relied on older frameworks ([OCLP](https://dortania.github.io/OpenCore-Legacy-Patcher/TROUBLESHOOT-NONMETAL.html)). What Tahoe does with zero `MTLDevice`s is [UNKNOWN] (forum snippets of ~3 fps for an unsupported RX 7900 XTX are unverified). This is today's state of the project: display works through IONDRV and scanout, nothing composites on the GPU.
- **GL/CL plug-in.** `IOGLBundleName`/`IOOCDBundleName` select GL/CL bundles; GL is not what WindowServer uses, so low value [INFER]; the VMQemuVGA GL contract was not portable across releases.
- **OCLP's approach** reinstalls Apple's own older bundles/kexts; it never writes a new plug-in, so it offers no template ([amd_polaris.py](https://github.com/dortania/OpenCore-Legacy-Patcher/blob/main/opencore_legacy_patcher/sys_patch/patchsets/hardware/graphics/amd_polaris.py)).

## 4. What the kext foundation provides, per route, and what is missing

Status words as in `docs/metal-readiness.md` (card / card (Linux) / emulator / code).

| Capability | Kext interface today | Route A | Route B | B' | C (Vulkan) |
|---|---|---|---|---|---|
| Display, flip, cursor, vblank | IONDRV hook, DCN401 modes, `Present*`, IH (card) | needed, have | needed, have; IOSurface-backed scanout must be added | have | n/a |
| GPU memory (VRAM, pinned host, DMA) | `Alloc/AllocHost/Read/Write`, SDMA bounce (card) | map onto `IOAccelResource`-style resources: missing glue | same | same | maps to BOs: missing glue |
| Per-process address spaces | `rdna4-vm` VMIDs 8-15 (**client jobs fail on the card, section 4 of metal-readiness**); W13 pool/tree for >8 clients (code + host tests; **S8 waits on guest runs**) | **hard blocker for every route**: applications must run in their own VM | same | same | same |
| Compute submit | `SubmitIb`/`WaitFence` (card without VM) | have the primitive; PM4 would come from the bundle, translated | have | have | have (maps to CS submit) |
| Graphics submit | W12k `SubmitGfxIb`/`WaitGfxFence` (code + emulator; RB0 + hand PM4 proven on the card under Linux) | needs translated gfx PM4 | have the ring; need a PM4 emitter for Metal's pipeline state | same | need a RADV-style emitter (RADV has it) |
| gfx12 shaders | hand-made NGG + PS kernels exact on the card (Linux) | ISA translation missing | **compiler missing** | compiler missing | ACO (in RADV) has it |
| Hang recovery, power | W6 compute recovery (card non-VM), W13 kill-by-VMID (code), CG (card) | same | same | same | same |
| Access policy | `RDNA4ComputeClient` root-only; proposal `docs/w13-client-access.md` | needed (WindowServer is not root [INFER]) | needed | needed | needed |
| **Apple-facing service** | none: the kext hooks the IONDRV framebuffer only | impostor of `AMDRadeonX6000` protocol: missing, untraced | `IOAccelerator` subclass: missing; headers must be reconstructed | fake paravirt device: missing | none needed |
| Shader compiler | LLVM `llvm-mc` pipeline for hand-written kernels | Apple's libSC emits gfx10.3 only | AIR -> gfx12 missing | missing | RADV ACO |

The foundation is strong on hardware (display, memory, DMA, IB submit, gfx12 draw facts) and absent on everything Apple-facing. Its one blocking gap for every route is the same: **applications' work in their own VM on the card**.

## 5. First three experiments

Each is ordered by value per cost and needs no new card risk except the third (needs the lead's OK for a GPU run).

**E1. Impostor-accelerator census in the emulator (no GPU, kext change, ~1-2 weeks).**
Teach the kext to publish one `IOAccelerator`-class service (subclass of the public `IOAccelerator`, `IOKit/graphics/IOAccelerator.h`) carrying `MetalPluginName`, `MetalPluginClassName`, `MetalStatisticsName`, `IOAccelRevision 2`, plus a `newUserClient` that logs every open (type) and every `externalMethod` (selector, scalar and struct sizes) and returns errors.
Run Recovery in the emulator (Kiln's `tools/emu-linux.sh` loop) with a tiny osxcross-built tool that calls `MTLCopyAllDevices()` and dumps `log show`. Measure: which registry properties Metal reads, whether it looks for the bundle and where (path, sandbox/SSV handling; the Recovery image has no bundle, so the error text names what it tried), which user-client types and selectors `MTLIOAccelDevice` opens first, and whether a deliberately wrong plug-in name fails softly or takes the process down (a stub-project author claims it breaks Metal.framework on Sequoia: unverified). Answers the B1/B3 unknowns of this document, at zero card risk, and tells us whether B and B' are possible at all. The census itself is the first "trace" of Apple's side of the protocol, because Metal rather than Apple's AMD driver makes the calls.

**E2. Static survey of the full Tahoe x86 install (no GPU, downloads, ~1 week).**
Obtain the x86 macOS 26.6.2 (25G83) system files (installer package or restore image, a download of the order of 15 GB: needs the user's decision) and read, never run: (1) is `AppleParavirtGPU.kext` / `AppleParavirtGPUMetal.bundle` there and x86_64; (2) slices and versions of `AMDRadeonX6000*`, `AMDRadeonX6000MTLDriver`, `AMDShared`, `libAMDNTPlugin`, `libSC`; do any contain gfx11/gfx12 targets (the agents only checked names); (3) class-dump `Metal.framework`/`IOAccelerator.framework` from the dyld cache to get the real `MTLIOAccelDevice` and `MTLDeviceSPI` interfaces for 26.6.2 instead of the 26.4 dump; (4) the `AMDRadeonX6000` kernel user-client selector tables (feeds route A, and is the reference for what B's kernel must accept); (5) recover the `IOGraphicsAccelerator2` / `IOAccelSharedUserClient2` vtable order and sizes (feeds B's kernel headers). Decides A vs B vs B' on facts rather than on the agents' inference, and cross-checks the third-party repo's claims.

**E3. gfx1030 -> gfx1201 compute translation proof on the card (needs lead's GPU OK; uses `tools/linux-replay` only).**
Compile 10-20 OpenCL/HIP compute kernels with upstream LLVM for gfx1030 (what Apple's RDNA2 libSC approximates for compute), run them through a disassemble -> rewrite -> `llvm-mc -mcpu=gfx1201` translator with the compute-ABI shim (TTMP/packed-`v0` unpack, waitcnt mapping), launch through the existing replay-compute path, and compare against a native gfx1201 build. Quantifies route A's compute viability (translation rate, what fails: scratch, LDS, images, waitcnt) and produces a reusable tool regardless of route: the same translator can post-process kernels from any source. The sub-agent's 96 % instruction figure is the prior to beat or refute.

## 6. "Ready to start the Metal phase": proposed definition

The Metal phase is ready to start when all of these hold (the first two are the lead's existing open blocker):

1. **Client VM works on the card**: two clients run compute and gfx jobs in their own VMIDs, including one mapping more than 1 GiB (W13 S8 guest runs + the real-card boot that resolves `docs/vm-client-rootcause.md`).
2. **Graphics submission from a client on the card**: W12k `SubmitGfxIb` draws the G4 colour triangle in a client VM (today: code + emulator only).
3. **The route decision is made on facts**: E1 and E2 done (the loader, the selector census and the full-install survey); E3 optional but informs A.
4. **Access policy chosen**: `docs/w13-client-access.md` default decided (WindowServer is not root [INFER: verify with `ps`]).
5. **A shader-compiler plan**: which of the three AIR -> gfx12 options in section 3.2 is pursued, and the whitelist plug-in for WindowServer's shaders as the first deliverable.

Before that, work on Metal is speculative; after it, the first build target is "MTLCopyAllDevices() returns our device" (E1 shows how far it gets), then "a compute MTLComputePipelineState runs a whitelisted kernel", then the two SkyLight composite shaders.

## 7. Could not find, or not checked

- Anything from a stock Tahoe x86 install: there is none on this machine; bundle slices, versions and the exact property values on Tahoe are [UNKNOWN] (the registry dump is of unknown OS).
- The bundle loader in Metal.framework (lookup path, sealed-volume policy); the current IOAccel selector tables; whether a third-party compiler plug-in path is honoured.
- Whether Apple's GFX10 bundle builds PM4 in user space (likely from the class names, not verified), whether it uses NGG or legacy VS, the packed-binary layout beyond the third-party tool's offsets.
- Whether gfx12 hardware accepts gfx10.3-format BVH nodes; SDMA 5.2 -> 7.0 and PTE differences (not covered).
- Any working third-party `MTLIOAccelDevice` plug-in on a modern macOS; any RADV-on-Darwin port; Tahoe's behaviour with zero Metal devices; Apple's open-source IOAcceleratorFamily2 (not found).
- The RDNA3 ISA document was downloaded but not read in depth; RDNA2 and RDNA4 ISA pages cited by the sub-agent were not re-read by me.
- The sub-agents' scratch files live outside the repo (session scratchpad) and will not persist; everything needed is cited by URL here.

## 8. Sources

Apple / macOS: [Dortania Tahoe](https://dortania.github.io/OpenCore-Install-Guide/extras/tahoe.html), [WWDC20 10615](https://developer.apple.com/videos/play/wwdc2020/10615/), [Metal 4 support](https://support.apple.com/en-us/102894), [metal-shaderconverter](https://developer.apple.com/metal/shader-converter/).
Symbols and diffs: [blacktop/symbolicator 25.5 kexts](https://github.com/blacktop/symbolicator/tree/main/kernel/25.5/kexts), [ipsw-diffs macOS 26](https://github.com/ipsw-diffs/macos-26), [blacktop ipsw-diffs](https://github.com/blacktop/ipsw-diffs), [keith/Xcode.app-strings](https://github.com/keith/Xcode.app-strings), [macOS 26.4 headers](https://github.com/thatmarcel/macOS-26.4-headers).
Registry dumps: [Navi14 dump](https://github.com/hoOJluGun/agent-mode/blob/main/preserve_store/manifests/driver_readonly_diag_20260605_001003.txt), [DarwinDumped iMac18,3](https://github.com/khronokernel/DarwinDumped/blob/master/iMac/iMac18,3/IORegistry/IOregViewer/Resources/dataFiles/IOService/data183.txt), [OCLP PatcherSupportPkg](https://github.com/dortania/PatcherSupportPkg/tree/main/Universal-Binaries/12.5/System/Library/Extensions).
Metal compile chain: [Khronokernel LEGACY-METAL part 1/2](https://khronokernel.com/macos/2022/11/01/LEGACY-METAL-PART-2.html), [worthdoingbadly metalbitcode](https://worthdoingbadly.com/metalbitcode/), [MetalShaderTools](https://github.com/zhuowei/MetalShaderTools), [MetalLibraryArchive](https://github.com/YuAo/MetalLibraryArchive).
Kernel interface RE: [CanSecWest 2016](https://papers.put.as/papers/macosx/2016/CanSecWest2016_Apple_Graphics_Compromised.pdf), [ret2 pwn2own 2021](https://blog.ret2.io/2022/06/29/pwn2own-2021-safari-sandbox-intel-graphics-exploit/), [Asahi wrap.c](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/asahi/lib/wrap.c) (AGX, structure only).
Third-party projects: [reims-vgpu](https://github.com/steelbrain/reims-vgpu), [metal2vulkan](https://github.com/steelbrain/metal2vulkan), [MetalGPUDrivers](https://github.com/winkleplex10-bit/MetalGPUDrivers), [Navi48-MacOS](https://github.com/Almosst-DEV/Navi48-MacOS), [Polaris22Fixup](https://github.com/osy/Polaris22Fixup), [NVBringup](https://github.com/kvarun-p/NVBringup), [VMQemuVGA](https://github.com/startergo/VMQemuVGA), [lemonade-sdk/mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu), [tinygrad AM](https://docs.tinygrad.org/developer/am/), [Darling Indium](https://github.com/darlinghq/indium), [OCLP](https://github.com/dortania/OpenCore-Legacy-Patcher).
ISA and kernel: [RDNA2](https://gpuopen.com/download/rdna2-shader-instruction-set-architecture.pdf) / [RDNA3](https://gpuopen.com/download/rdna3-shader-instruction-set-architecture-feb-2023.pdf) / [RDNA4](https://gpuopen.com/download/rdna4-instruction-set-architecture.pdf) ISA guides, [LLVM AMDGPUUsage](https://llvm.org/docs/AMDGPUUsage.html), Mesa `src/amd/registers/*.json`, Linux `gfx_v10_0.c` / `gfx_v12_0.c`.

## Appendix A. How the [MEASURED] items were obtained

```sh
# image survey (read-only; the image is Kiln's conversion, not the stick)
7z l ~/work/tools/emu/images/BaseSystem.img > list.txt            # 69,787 lines
grep "Library/Extensions/[^/]*$" list.txt                          # no AMD* entries
7z e BaseSystem.img "macOS Base System/System/Library/KernelCollections/BaseSystemKernelExtensions.kc" \
   "macOS Base System/System/Library/KernelCollections/BaseSystemKernelExtensions.kc.bundles" \
   "macOS Base System/System/Library/dyld/dyld_shared_cache_x86_64.map" \
   "macOS Base System/System/Library/CoreServices/SystemVersion.plist"
# fileset entries: walk LC_FILESET_ENTRY (0x80000035) in the Mach-O header; versions/personalities: the
# prelink-info plist strings in the .kc (CFBundleGetInfoString, IOPCIMatch, GPUCompanionBundles, IOClass)
strings -n 4 BaseSystemKernelExtensions.kc | grep -E '^__ZN[0-9]+IOAccel|^__ZN[0-9]+IOGraphicsAccelerator2' | sed 's/^_//' | c++filt
# user-space exports of the private frameworks: the SDK .tbd files
grep -o '_IOAccel[A-Za-z0-9_]*' $SDK/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator.tbd
grep -o 'MTLIOAccel[A-Za-z0-9_]*' $SDK/System/Library/Frameworks/Metal.framework/Metal.tbd
# ISA aliasing (llvm-mc 22.x)
echo "0x00,0x00,0x81,0xbf" | llvm-mc -triple=amdgcn-amd-amdhsa -mcpu=gfx1201 -disassemble     # s_setkill 0
echo "0x00,0x00,0x8a,0xbf" | llvm-mc -triple=amdgcn-amd-amdhsa -mcpu=gfx1201 -disassemble     # s_wait_idle
```
