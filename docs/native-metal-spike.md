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
- Seen by eye (Sunneva, 2026-11-19): one arrow per display, right, and the box around the pointer in Safari is gone.

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

## Gate 6: apps, not only root

The device drives the card through the kext's runtime client, which was root only (a client's kernels ran in VMID 0 then and reached
all of VRAM). Following `docs/w13-client-access.md`:

- `rdna4-client-policy=local` (default: root only) also lets in the user logged in at the console
  (`kIOClientPrivilegeLocalUser`), **only in a GPU address space of its own**: a client that is not root and gets none (no
  `rdna4-vm=1`) is refused at its start (`RDNA4Compute::rtClientIsolated`).
- Such a client has the selectors a graphics client needs (its buffers, copies into and out of them, code objects, dispatches, compute
  and gfx IBs and their fences: `kLocalSelectors` in `src/userclient.cpp`); the display (present, restore, vblank), the sensors, the
  sleep test and the quiesce answer `kIOReturnNotPrivileged`.

**Gate 6 PASSED on the rig, 2026-11-19**: as the logged-in user (not root) `mtlprobe native` passes compute, the render pass and the
ray queries; `rdna4-run selftest` passes in VMID 9; `rdna4-run sensors` is refused (privilege violation).

## Gate 7: libraries from source

Metal's own compiler makes libraries for this device: `_MTLDevice` (which the device does not override there) answers
`newLibraryWithSource:` and the other library calls with an `_MTLLibrary` whose functions (`_MTLFunctionInternal`) give their AIR as
`-bitcodeData`. The pipelines take such a function: its bitcode is written to a file and translated as a metallib is.

**Gate 7 PASSED on the rig, 2026-11-19**: `mtlprobe nativesrc` compiles a kernel from Metal source for the device, makes its pipeline and
runs it: 0 of 256 wrong, as a user that is not root.

## Gate 9: on the screen

- The kext maps a page-aligned range of the client's **own** memory into its GPU address space (`kRDNA4MethodImportHost`,
  `rdna4_import_host`, `RDNA4Compute::rtImportHost`: wired with `IOMemoryDescriptor::withAddressRange` + `prepare`, unwired when the
  buffer is freed; the host-buffer limits apply), sharing the mapping code with `rtAllocHost` (`mapHostLocked`).
- The device: `newBufferWithBytesNoCopy:length:options:deallocator:` and `newTextureWithDescriptor:iosurface:plane:` (one plane of
  RGBA8 / BGRA8, rows a multiple of 256 bytes so far) over such imported memory; `CAMetalLayer`'s drawables are such textures. A command
  buffer presents its drawables (`presentDrawable:`) once its work has run.
- `NativeWin.app` (`mtlprobe nativewin` in a bundle whose `LSEnvironment` sets `RDNA4_NATIVE_MTL=1`, started with `open`): a window
  whose `CAMetalLayer` uses `RDNA4MTLDevice`, its shaders compiled from source, the colour triangle drawn each frame.

**Gate 9 PASSED on the rig, 2026-11-19**: 1207 frames in 20 s (60 a second), none failed, as the logged-in user; the frame grab shows
the window with the triangle on the desktop, composited by WindowServer (still on the paravirtual GPU) beside everything else.

## Gate 10: 3D

- `Depth32Float` textures in VRAM, laid out 64KB_2D in whole 256x256-pixel blocks as the PV host's depth buffers; a clear load action
  writes the value everywhere (one value does not see the tiling). The depth test and write (`newDepthStencilStateWithDescriptor:`,
  Metal's compare functions are the hardware's ZFUNC) with the PV host's depth-block registers and Metal's 0..1 depth range.
- Textures (linear RGBA8 / BGRA8) and samplers in vertex and fragment functions: their T#s and S#s in the stages' tables
  (`RDNA4_MTLC_TABLE_TEXTURES`, `_SAMPLERS`), the T# as the PV host's `linearImage`, the S# as its sampler conversion.
- Indexed draws (16- and 32-bit indices: `VGT_INDEX_TYPE`, `DRAW_INDEX_2`), with instances.

**Gate 10 PASSED on the rig, 2026-11-19**: `mtlprobe nativespin` (the paravirtual GPU's `spin` on the native device): nine lit,
textured cubes with a depth buffer, drawn with one indexed draw of nine instances, at 1920x1200 and 60 frames a second (1202 frames in
20 s), as the logged-in user; the frame grab shows them right.

## Gate 8: a queue

- `commit` returns at once. One serial work queue per device runs the committed command buffers of all its queues in commit order: it
  calls the scheduled handlers, runs the recorded steps, presents the drawables and calls the completed handlers. `waitUntilScheduled`
  and `waitUntilCompleted` wait for those points.
- Draws are submitted behind each other without waiting. Each one takes the next of 16 slots (an IB page and a state page; a slot's
  earlier draw is waited for before reuse). Every other step (a dispatch, a CPU clear, blit or acceleration-structure build) first waits
  for the draws before it, as does the end of a command buffer: the gfx fences are ordered, so the last one is enough, and the last
  stream's own fence word is checked.
- A blit encoder: buffer copies and fills, buffer-to-texture, texture-to-buffer and texture-to-texture copies, by the CPU (every buffer
  and colour texture is shared memory; depth textures are not blitted).

**Gate 8 PASSED on the rig, 2026-11-19**: `mtlprobe native`'s queue check commits four command buffers (a blit copy, a dispatch each,
a blit fill) without waiting: they complete in order with the right results, the four commits return in 1 ms. Its draws check: 1000
draws in one command buffer, read back by a blit, right, 64 us a draw (two `rdna4_write` calls and a submit each: one IB per command
buffer when that matters). `nativespin` still presents at 60 frames a second from the work queue.

## The native desktop: Core Animation, then WindowServer

WindowServer composites with Core Animation's Metal renderer (`CA::OGL::MetalContext`). The same renderer runs in any process through
`CARenderer`, so `mtlprobe nativeca` renders one layer tree (solid colours, rounded corners and borders, a gradient, text, an image, a
half-transparent layer) on the paravirtual device and on ours and compares them pixel by pixel. What it needed, all ported from the PV
host's `runDraw` (the bundle is Objective-C++ now and includes `src/pvdraw.hpp`, which got the PV host's `textureFormat` table):

- Metal's own libraries (`_MTLDevice`'s `newLibraryWithURL:` / `Data:`): Core Animation makes its shaders with function constants
  (`newFunctionWithDescriptor:`), which Metal's `_MTLLibrary` specialises; its functions give their AIR as `-bitcodeData` (gate 7's path).
- Textures of every format of `textureFormat` (RGBA16Float, R8, RG16F, ...), IOSurface planes, textures over buffers
  (`newLinearTextureWithDescriptor:`), clears of any of them.
- Colour attachments of every `colourInfo` format; per attachment blend states and the PV host's choice of one attachment per draw (header
  bits 20-30, `RDNA4_MTLC_TABLE_ATTACHMENT`); blending with the alpha-only second pass; blend colour; scissor rectangles; vertex
  descriptors (`RDNA4_MTLC_TABLE_ATTRIBUTES`); explicit-LOD sampling (level T#s; one level so far); primitive restart.
- `newBufferWithBytesNoCopy:` of read-only memory (a CGImage's pixels): the kext cannot wire it for the GPU, so it is copied.

Result on the rig (2026-11-19): the native render matches the paravirtual one by eye; 6156 of 262144 pixels differ, all in the image,
which the paravirtual path colour-matches (DeviceRGB to the target's space) and ours does not yet (open).

WindowServer takes its device from the accelerator that drives the displays, the paravirtual one. The kext can point that accelerator's
Metal plug-in at our bundle (`src/accelcensus.cpp` `routePv`; `debug.rdna4_nativemtl=2` / `3` at run time, `rdna4-nativews=2` / `3` from
the boot), class `RDNA4MTLRouter`: in the processes the accelerator's `RDNA4NativeProcesses` names (WindowServer with 3), or started with
`RDNA4_NATIVE_MTL=1`, it makes our device; everywhere else Apple's paravirtual device from Apple's bundle, as before. With
`rdna4-client-policy=local` the kext also lets WindowServer (`_windowserver`, uid 88) open its runtime client, with the local
restrictions. Route 2 (nobody native) checked on the rig: new processes get Apple's device through the router and its compute, render,
texture, blend, depth and dual-source checks complete.

### WindowServer on the native device (rdna4-nativews=3), 2026-11-20: where it stands

What it took to get WindowServer composing on `RDNA4MTLDevice` (each found on the rig, one boot at a time):

- WindowServer's sandbox: it may open only user clients whose class starts with `IOAccel` (`com.apple.WindowServer.sb`), so the kext's
  runtime client is `IOAccelRDNA4ComputeClient` (a subclass, same code); and it may not start programs, so the device translates through
  the kext (`kRDNA4MethodTranslate`, `rdna4_translate`): the kext queues the AIR to the translator helper in the paravirtual host's request
  slot (`pvTranslateForClient`; the host's own requests and these take turns, `waitForSlot`).
- `rdna4-client-policy=local` admits `_windowserver` (uid 88) as it does the console user. The device returns nil without its runtime
  client (the router then makes Apple's device), and waits up to 15 s for the gfx ring (WindowServer makes its device about a second
  before the kext's gfx ring is up).
- Metal and CoreDisplay: `MTLCopyDeviceForRegistryID` hands out `[[service weakDevice] _deviceWrapper]` (ours: the device itself);
  CoreDisplay loads its own `default.metallib` and needs a BGR10A2Unorm target and a 1D-array R32Float gamma LUT; SkyLight's
  `CompositorMetal::new_compositor` picks the device whose `acceleratorPort` is the display's accelerator (else "null compositor", a
  black desktop); WindowServer commits with `commitAndWaitUntilSubmitted` and its own completion queue (`setCompletionQueue:`);
  MPS (the desktop's blurs) keeps its state in the command buffer's `userDictionary` and runs kernels with textures (the kernel texture
  table, air2amdgpu.cpp's ABI).
- The paravirtual host keeps presented display frames on the card (`keepOnCard`) and scans that copy out, which only Apple's driver's
  draws update: with WindowServer native, `debug.rdna4_pvinplace` needs bit 32 (35) for the scanout to read the frames' memory.
- Textures: mipmapped 2D (levels as images of their own, real level T#s, `generateMipmaps` by the CPU), 3D (the hardware's pitches),
  IOSurfaces whose rows are not 128-byte multiples (a CPU-refreshed copy), the PV host's spare texel in unbound slots (an all-zero T# can
  stall the texture unit), a barrier before reading what a draw in flight draws, a copy of the target when a draw samples it.
- A safety net: the router counts WindowServer's starts on the native device (a file in its temporary directory); the third start within
  two minutes gets Apple's device, so a crash loop ends with the desktop back.

**A native desktop (2026-11-20, 1c4c73e):** with `rdna4-nativews=3`, WindowServer composes both displays on `RDNA4MTLDevice`: the
wallpaper (a 10-bit YCbCr image), desktop icons and labels, menu bars, notifications, the Dock, matching a boot on Apple's device by eye
but for a translucent notification banner (opaque here). What the last steps were:

- A fragment shader without an export (Core Animation's `TkfhBvcmA2Xh_IsrcM1`, `s_endpgm` alone, its header saying it has a colour)
  hung the gfx ring: drawn as one without a colour now (an export is a dword with bits 31-26 `111110`).
- Our device answers the feature queries and GPU families as Apple's paravirtual device does (`mtlprobe features` lists both):
  `_MTLDevice`'s defaults describe an Apple GPU, and Core Animation chose techniques for it that the paravirtual device never sees.
- Textures over buffers and IOSurfaces typed 2D (0 is `MTLTextureType1D`: their T#s read row 0 only; the wallpaper was flat).
- Tiled textures as linear ones; CPU-refreshed copies where offsets or rows do not suit the hardware; `set*Bytes` in 256 KiB chunks;
  the kext's runtime holds 4096 buffers (WindowServer ran it out at 1024).
- The kext keeps nothing of the paravirtual host's on the card while WindowServer is native (`debug.rdna4_pvinplace` bits 32 and 16
  from the boot): the display frames and the other processes' targets (they still draw through Apple's device) are read from memory.

Since then (2026-11-20, fef6ef4 .. this): compute pipelines from descriptors (Core Animation's average-luma kernel: WindowServer
aborted and logged the user out when Finder opened), the spare buffer and texture table for kernels (every dispatch faulted on address 0),
texture views, a render pass's attachment level drawn into that level (Liquid Glass: Core Animation's blur chain draws each mip level
from the one before; all into level 0, the glass was transparent), read-only imports (`RDNA4_HOST_READ_ONLY`: memory WindowServer may
only read, shared by other processes, mapped for the GPU without WRITEABLE; copied once, the menu bars never updated), heaps.
Finder, Safari, Mail, System Settings, Notes and Terminal render as on Apple's device by eye.

Open: the 4K (HiDPI) display's menu bar shows no app menus (the 1080p one does; not found: the titles' vibrancy draws run, no errors);
the Dock flashed at intervals once (Sunneva; not seen in frame grabs since); every draw is still its
own IB (about 64 us each); colour matching of DeviceRGB images (`nativeca`); MSAA, stencil, heaps not yet.
