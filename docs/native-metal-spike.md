# Native Metal: spike

Started 2026-11-19 (Sunneva: "start working on native acceleration" if it also gives ray tracing and a real cursor). The question
for the spike: can macOS load **our own** Metal driver for the card, beside the paravirtual GPU that runs the desktop today?

Why native at all (measured, 2026-11-18/19):

- **Ray tracing cannot come through the paravirtual route.** Apple's x86 `AppleParavirtGPUMetal.bundle` has encoders for blit, compute,
  render, parallel render and "info", and its serializer only *binds* acceleration structures (`setAccelerationStructure:atBufferIndex:`
  on the compute and render encoders); there is no acceleration-structure encoder to build one.
- **A hardware cursor cannot come through it either.** Under IOPresentment (`rdna4-iop`) WindowServer composites the pointer into the frames;
  the paravirtual display transactions (FIFO 0x06/0x07) carry one surface and a gamma table, no cursor plane. The legacy framebuffer cursor
  (FIFO 0x04 glyph, page `+0xe00` position, decoded and driven onto the DCN cursor plane, stash "experiment: PV hardware cursor") only ever
  shows the kernel's busy cursor in that mode.

This is route B of `docs/metal-spike.md` (our own accelerator and Metal bundle). The spike found the loader much friendlier than that
document assumed.

## How Metal loads a vendor driver (Tahoe 26.6.2, x86_64, read from the shared cache)

1. `MTLCopyAllDevices` / the device notifications match `IOAccelerator` services (`_mtlIOAccelDeviceMatched`).
2. `getMetalPluginClassForService(service)` (Metal) reads the service's **`MetalPluginName`** (CF string, at most 255 bytes) and calls
   **`gpu_bundle_find_trusted(name, path, 1024)`** (`libsystem_sandbox`), which looks for `<dir>/<name>.bundle` in
   `GPU_STANDARD_BUNDLE_PATHS`: **`/Library/GPUBundles`**, then `/System/Library/Extensions`. A bundle is trusted if
   `gpu_bundle_is_path_trusted` accepts it: `rootless_check_trusted_class(path, "KernelExtensionManagement")`, i.e. the file carries the
   rootless storage class `com.apple.rootless = KernelExtensionManagement` (what kext management gives staged third-party kexts; on the
   rig `/Library/StagedExtensions` has it), or it is on the authenticated root volume.
3. `[NSBundle bundleWithPath:]`, then the class named by **`MetalPluginClassName`** (`classNamed:`), else the bundle's `principalClass`;
   it must satisfy **`isSubclassOfClass:[_MTLDevice class]`**: Metal's generic device base class, **not** `MTLIOAccelDevice`.
4. `-[MTLIOAccelService initWithAcceleratorPort:deviceClass:]` (it also reads `SafeEjectRequested`) makes the device:
   **`[[class alloc] initWithAcceleratorPort:(io_service_t)service]`**.

Consequences:

- The bundle can live in `/Library/GPUBundles` (outside the sealed system volume). With SIP's filesystem protection off
  (`csr-active-config` 03000000, as on the rig) `gpu_bundle_is_path_trusted` returns 0 (trusted) for every path, `/tmp` included
  (checked on the rig with a small caller, 2026-11-19), so no storage class is needed; setting `com.apple.rootless` is refused (EPERM)
  even then. On a stock-SIP machine the bundle would have to come through kext management.
- **The device class only has to be a `_MTLDevice`.** It does not have to speak Apple's private IOAccelerator user-client protocol
  (`MTLIOAccelDevice`'s): our device can drive the card through our own kext's interfaces (the runtime client `rdna4-run` and the Vulkan
  driver already use). That was the largest unknown in route B's estimate.
- `_MTLDevice` is exported by Metal (`_OBJC_CLASS_$__MTLDevice`, also in the SDK's `Metal.tbd`): a bundle links against it normally.
- The kernel side only needs a service that matches as `IOAccelerator` and carries the two properties. The E1 census service
  (`src/accelcensus.cpp`) already is one (an `IOService` answering `getMetaClass()` with IOAccelerator's metaclass, since the boot
  kernel collection cannot link IOGraphicsFamily).

## Gate 1: our device exists

- Kernel: `src/accelcensus.cpp` level 3, published by `sudo sysctl debug.rdna4_nativemtl=1` after WindowServer chose its device (never at
  boot): service `RDNA4NativeMetal`, class `IOAccelerator`, `MetalPluginName` = `AMDRadeonRDNA4Metal`, `MetalPluginClassName` =
  `RDNA4MTLDevice`; its user client refuses every call.
- Bundle: `tools/metal/native/` (`build.sh` -> `Build/native/AMDRadeonRDNA4Metal.bundle`; `install.sh`, as root, puts it into
  `/Library/GPUBundles`). `RDNA4MTLDevice : _MTLDevice` has `initWithAcceleratorPort:`, a name and the service's
  registry ID, nothing else.
- Check: `mtlprobe devices` lists every device with its class and registry ID. Passed when `RDNA4MTLDevice` is listed beside the
  paravirtual device.

**Gate 1 PASSED on the rig, 2026-11-19** (kext f338606b): `RDNA4_NATIVE_MTL=1 mtlprobe devices` lists "AMD Radeon RDNA4 (native Metal,
spike)" (class `RDNA4MTLDevice`, registry ID of the `RDNA4NativeMetal` service) beside "Apple Paravirtual device". What it took after
the loader found the bundle:

- `MTLAddDevice` asserts `conformsToProtocol:` **`MTLDevice`** and **`MTLDeviceSPI`** (private: added at `+load` with
  `objc_getProtocol` + `class_addProtocol`).
- `-[MTLIOAccelService initWithAcceleratorPort:deviceClass:]` sends **`_setAcceleratorService:`** (the `MTLIOAccelService`) right after
  the init; `_safeEjectRequested` sends **`_removeRequested`**.
- Then Metal asks `featureProfile` and `maxBufferLength` (answered zero by the bundle's catch-all for now).
- An init that returns nil must call `[super init]` first: `_MTLDevice`'s dealloc crashes otherwise. Metal skips a nil device.
- **The device must not appear in every process.** Published without a guard, every process that enumerates devices made one and
  crashed on the first missing message (ControlCenter, TextInputMenuAgent, AMD Power Gadget). The bundle now makes the device only
  where `RDNA4_NATIVE_MTL` is set; that stays until the device answers everything an ordinary app asks.

## Gate 2: buffers and compute

The bundle drives the card through the kext's runtime client (`librdna4`, `RDNA4ComputeService`, root only):

- `newBuffer*` -> `rdna4_alloc_host`: snooped system memory mapped into the task and the client's GPU address space (shared storage,
  `contents` and `gpuAddress` straight from it; at most 64 MiB a buffer).
- `newLibraryWithURL:` / `newLibraryWithFile:` / `newLibraryWithData:` keep the metallib; `newFunctionWithName:` is a name in it.
- `newComputePipelineStateWithFunction:` runs the translator in file mode (`/usr/local/libexec/rdna4-air2amdgpu <metallib> <name>
  <out.hsaco>`, it prints the buffer count) and loads the code object with `rdna4_load`.
- The compute encoder records each dispatch with the translator's kernargs (buffer addresses, threads per group, threads in the grid,
  texture table 0); `commit` runs them in order with `rdna4_dispatch` and then the completion handlers.
- Every class answers a message it lacks with zeros of the protocol method's own return type (from the protocol's type encoding) and
  logs it once: `RDNA4MTL: <class> does not implement <selector>`.

Check: `sudo sh ~/rdna4/native/probe.sh` on the rig (`tools/metal/native/probe.sh`: publishes the service, runs `mtlprobe native
probe.metallib`; unattended: `sudo -n /usr/local/libexec/rdna4-native-run.sh native ~/rdna4/native/probe.metallib`): c = a + b over 1000 floats in groups of 64 (dispatchThreads, a partial last group), then c *= 3 with `setBytes`.

**Gate 2 PASSED on the rig, 2026-11-19**: `mtlprobe: native: PASS: 0 of 1000 wrong (status 4); c[0] 3, c[999] 4498.5`. The only message
Metal sent that the device lacks is `featureProfile`.

Open for later gates: the root-only client (an app cannot be root: the native device needs a user client of its own on the
`RDNA4NativeMetal` service, a GPU address space per process); `newLibraryWithSource:` (needs Metal's compiler service for our device,
or AIR from elsewhere); a real queue (IBs and fences instead of a synchronous commit); VRAM buffers.

## Gate 3: a render pass

- `newTextureWithDescriptor:`: linear 2D RGBA8 / BGRA8 (unorm or sRGB), one level, one sample, in shared memory (`rdna4_alloc_host`), rows
  256-byte multiples; `replaceRegion:` / `getBytes:` copy rows.
- `newRenderPipelineStateWithDescriptor:`: the vertex and fragment functions translated into RGFX blobs (file mode again), the values
  between the stages matched by name (`SPI_PS_INPUT_CNTL_n`), the code placed as the PV host does (VS at +0, PS at
  `RDNA4_TRI_PS_OFFSET`, `s_code_end` around them, the attribute ring literals patched) in a code buffer of the pipeline's own.
- The render encoder records buffers per stage (`set{Vertex,Fragment}{Buffer,Bytes}`), the viewport and `drawPrimitives`; a clear load
  action is done by the CPU (shared memory). Each draw is the PV host's `runDraw` cut down: the proven triangle stream
  (`gfx12tri.h`, `gfx12tricol.h` with values between the stages), its registers patched as the PV host's table (shaders, target,
  viewport, primitive, one sample, no blending), the stages' tables in a state page, the draw spliced in for the template's
  `DRAW_INDEX_AUTO`, submitted as the client's own gfx IB (`rdna4_submit_gfx_ib`, `rdna4-gfx=2`) and waited for.

Check (the same command as gate 2): the kext's colour triangle (vertex 0 red, 1 green, 2 blue at (64,64), (192,64), (128,192)) into a
256x256 RGBA8 texture filled with 0x55 and cleared to 0, checked with `rdna4-run tricol`'s own check (`rdna4_tricol_check`).

**Gate 3 PASSED on the rig, 2026-11-19, first run**: `render PASS: 8192 px (want 8192), bounds x 64..191 y 64..190, max channel error
0.50, max |R+G+B-255| 1, centroid 0xff555654`, pixel (0,0) cleared to 0. The desktop's paravirtual drawing on the same gfx ring went on
(no fault or timeout in the kext's log).

Not yet: blending, depth and stencil, textures and samplers in shaders, MSAA, indexed draws, other formats, the GPU doing the clear.

## Gate 4: the hardware cursor (WindowServer and a display)

The gate as first planned, WindowServer showing a display of the native device, needs WindowServer to composite on that device: every
format, blend mode, IOSurface and shader the compositor uses (`docs/metal-spike.md` rates that device at 1-2+ years). So gate 4 first
asks the cheaper question: **does the hardware cursor need a native display at all?** (The box lines around the pointer in Safari come
from one draw re-rendering slightly differently each time; they show only because WindowServer redraws the pointer's rectangle: with
a hardware cursor it would not.)

How Tahoe's WindowServer decides (x86_64, read from CoreDisplay and SkyLight, 2026-11-19):

- `WS::Displays::CDDisplay::supports_hardware_cursor()` is `!WSDisplayGetAcceleratorForcingSWCursor()`, which returns CoreDisplay's
  global `gAcceleratorBackedModeForcingSWCursor`. `reconfigureDisplays` sets that global from the displays' "forcing a software
  cursor" flag (bit 19 of the display's flags at +0x64).
- That flag is set by `MPSelectMode` for HDR output (dynamic range traditional-gamma HDR or SDR always; HDR10 unless the CoreDisplay
  preference `preferHWCursor` is set) and by `setup_hardware` for a display whose mode is scaled (a non-identity transform between
  its framebuffer and the mode). SDR displays in unscaled modes do not force it.
- Under IOPresentment (the paravirtual displays need it: `userspace/rdna4-iop`), the cursor still goes through the framebuffer's
  shared cursor (`IOFBCreateSharedCursor`, CoreDisplay's `MappedCursor.cpp`, `CGXMP*Cursor`): the legacy IOFramebuffer cursor that the
  paravirtual framebuffer passes to the host as FIFO 0x04 (stash "experiment: PV hardware cursor").
- On the rig: `SLSHardwareCursorActive()` is 0; the Samsung runs 1920x1080 points at 3840x2160 pixels (HiDPI), the Lenovo 1920x1080 at
  1x, both SDR.

**Answer: no native display is needed.** Measured on the rig, 2026-11-19, with the stashed PV hardware cursor (`rdna4-pvcursor=1`):

| setup | `SLSHardwareCursorActive` | glyphs WindowServer sends | port 0 frames / draws per 5 s, pointer circling |
| --- | --- | --- | --- |
| cursor size 64 announced, Samsung HiDPI | 0 | only the kernel's busy cursor (16x16) | ~252 / ~2900 |
| same, Samsung switched to 1x (`kCGConfigureForAppOnly`) | 1 | its arrow, 32x64 | 13-15 / ~650 (idle) |
| cursor size 128 announced, each glyph cropped, Samsung HiDPI | 1 | 2x arrow 64x128 (covers 29x43) to the Samsung, 1x 32x64 (15x22) to the Lenovo | 15-18 / ~650-1200 |

- With a hardware cursor WindowServer no longer redraws the pointer's rectangle as it moves (the frame rate drops to the idle rate),
  which is what made the box lines.
- At HiDPI WindowServer needs a cursor larger than 64 (its 2x images are 64x128). DCN 4.01's cursor is 64x64 at most (Linux:
  `dcn401_resource.c` `max_cursor_size = 64`, cursor cache 64x64x8 bytes; 128x128 would need MALL). So the host announces 128 and
  crops each image to what it covers (alpha above 0, the hot spot included): the arrow, 2x, fits.
- The two displays get different images (2x, 1x): each pipe has its own sprite slot and hot spot (`cursor.cpp` `cursorSlot`).
- Not yet seen by eye (the cursor plane is in no screen capture): `rdna4-pvcursor=1` stays default off until it is.

## Gate 5: ray queries (acceleration structures)

What Metal compiles a ray query to (Xcode's toolchain, AIR 2.8): `intersector<triangle_data>::intersect(ray, accel)` is one call,
`air.intersect.triangle_data(float3 origin, float3 direction, float min, float max, ptr addrspace(1) accel, ptr function table,
ptr payload, i64, i32 x 9, i1 accept any)` returning `{i32 type (1 triangle), float distance, i32 primitive id, i32 geometry index, ptr,
<2 x float> barycentrics, i1}`, with `air.get_null_intersection_function_table()` for the table. The structure is a kernel argument
`air.primitive_acceleration_structure` at a buffer location: an opaque pointer, so **the device chooses its format**.

- The format (`tools/metal/native/RDNA4MTLDevice.m`, `tools/metal/m3/rtlib.c`): a header, a BVH of 32-byte nodes in depth-first
  order, each with the index after its subtree (its escape), and 48-byte triangles. The traversal is stackless (next node on a hit,
  escape on a miss): kernels have no scratch memory for a stack.
- The build: `buildAccelerationStructure:descriptor:...` records a step the CPU runs at commit, from shared float3 vertex (and 16- or
  32-bit index) buffers: leaves of at most 4 triangles, median splits along the widest axis of the centres.
- The translator (`air2amdgpu.cpp`): the acceleration structure argument is a buffer slot; `intersect.triangle_data` becomes a call of
  `rdna4_rt_intersect` (`rtlib.c`: slab test, Moller-Trumbore), linked in after retargeting from gfx1201 bitcode that `build.sh` makes
  with Homebrew's clang (`rtlib.h`, committed), and inlined. The native bundle carries its own translator (`Contents/Resources`), so
  the system helper the paravirtual GPU uses is not touched.
- The compute encoder binds a structure with `setAccelerationStructure:atBufferIndex:`; the device answers `supportsRaytracing`,
  `accelerationStructureSizesWithDescriptor:` and `newAccelerationStructureWith{Size,Descriptor}:`.

Check: `mtlprobe native` (as gates 2 and 3): 1024 rays from a grid, slightly tilted, against 300 random triangles (a fixed seed), each
hit compared with the CPU's nearest triangle (distance, barycentrics, primitive id).

**Gate 5 PASSED on the rig, 2026-11-19, first run**: `rays PASS: 0 of 1024 rays wrong, 556 hit (CPU); structure 215 nodes, 300
triangles, 33648 bytes; commit 0.3 ms`.

Not yet: instances (instance acceleration structures), bounding-box geometry and intersection functions, masks and cull modes,
refit and compaction, ray queries in vertex and fragment functions, RDNA4's hardware BVH instructions (`image_bvh*_intersect_ray`,
which want the hardware's node format), building on the GPU.


