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

**Menu bars, speed, logouts (2026-11-20, later):** Sunneva saw slow window drags, parts of windows flashing, the 4K menu bar without
app menus and falls back to the login window. A stats line in WindowServer's log every 5 s (`RDNA4MTL stats:`: command buffers, draws
and their costs, copies, barriers, host memory, live textures) found where the time and memory went. Fixed:

- Command buffers run in the order they were enqueued, not committed (Core Animation enqueues one, then another, and may commit the
  second first): run in commit order, layers landed under the ones meant below them (flashing parts, stale blocks).
- Menu bars: WindowServer draws the titles into an IOSurface whose rows are not 128-byte multiples (the 4K one's strip, 3752x60,
  15040 bytes a row), so into its CPU-refreshed copy; the next refresh put the surface's old (empty) pixels over them. A copy the GPU
  drew into is now left alone until the command buffer is done and then written back into the surface. Both menu bars show the app's
  menus.
- Logouts: a retain cycle (mip-level views cached on their texture, each view holding the texture) kept every blur texture: WindowServer
  passed 7000 textures and 1-3 GB, its allocations failed, Core Animation aborted it, loginwindow ended the session. Views are cached
  weakly now (steady at about 800 textures and 200-230 MB with 11 apps). Also: a shader the translator refuses gives a pipeline whose
  draws are left out (Core Animation aborts WindowServer when one fails to build); imports (other processes' memory) no longer count
  against the 1 GiB host quota; the runtime holds 16384 buffers.
- Speed: the draw IB and state pages in host memory written in place (two kext calls a draw were 55 us of its 75); a draw that samples
  its own target reads a copy the card makes (idle barrier, then DMA_DATA into VRAM), of the rows written since the last copy only, not
  a 33 MB CPU copy after a drain (3.8 s of every 5 while a window moved); a texture drawn by a draw in flight is waited for on the card;
  shadows refreshed once a command buffer. WindowServer went from about 34 to about 120 command buffers a second across both displays;
  a window moved by a test app (`wmove`, 60 moves a second asked) 46 -> 56-59 on the 4K display, 39 on the 1080p one with 11 apps.
- The kext: its paravirtual host opened the runtime from `runDraw`, 10 KB down the tick's command walk, and IOLog's path then ran the
  timer thread's 16 KB stack out (double fault panic); opened from the tick now.

Open: a draw reading its own target still copies all rows its scissor covers (Core Animation's are full-target quads of 6 vertices:
about 9 MB a copy, 1500 copies a second), the card is the limit now; textures live in host memory (Core Animation's private surfaces
would belong in VRAM); colour matching of DeviceRGB images (`nativeca`); MSAA, stencil not yet.

**Tearing and the remaining copies (2026-11-20, evening):** Sunneva: menu bars and the Dock fixed, windows better but still glitchy,
drags smoother but a bit laggy. A test window with a toolbar and a translucent sidebar (`wmove 12 0 glass`) showed frames torn
between the window's old and new places. Fixed:

- Tearing: a command buffer reported itself scheduled when the work queue started it; WindowServer hands the frame to the display then
  (on Apple's GPUs the display waits for the GPU), so the display read frames half drawn. Scheduled now comes after its draws are done:
  no torn frame in the grabs since (all were before). WindowServer makes about 63 command buffers a second while a window moves (it
  made about 100, frames no one saw); the window still moves 56-59 times a second.
- The translator writes two records after an RGFX blob's values (the PV host stops reading before them): `TXRD`, the textures a shader
  uses (a texture left bound from an earlier draw, Core Animation's own target, had cost a copy of it for every draw), and `POSM` when
  the vertex position is a buffer's float4x4 times an attribute (Core Animation's `Vfx*` vertex shaders: `mvp_matrix * position`). For
  those the device transforms the draw's vertices (up to 256) on the CPU: the rectangle it covers is what it marks written and what a
  draw reading its own target has copied (with 16 rows around). The menu-bar blends (`TcimBdrkXc_Isrc`, `TmuaBpldXc_Isrc_Isrc`, three
  a frame per display) are full-target quads with full scissors but cover the bar's 60-182 rows: 32 MB a copy each before.
- A planar IOSurface's shadow (the wallpaper's 4K 10-bit video frame, 25 MB) is refreshed again only when its seed moves: it was
  copied for every command buffer (6.4 GB per 5 s of CPU memcpy while a window moved, now under 0.1 GB).

Card, moving the glass window on the 4K display: copies for draws reading their target 50 -> 11 GB per 5 s, CPU shadow refreshes
6.4 -> 0.07 GB per 5 s. Open: `VfxXgb` (vertex pulling, three layouts) is not bounded, its full-target `Xgc` draws still copy all
written rows; each draw is still a kext call (submit about 9 us) and each self-read an idle barrier on the card; textures still in host
memory.

**Frames shown at half rate (2026-11-20, afternoon):** the kext's frame timing (`frames:` in its log) showed the 4K display taking a
new frame every second vblank while a window moved (326 of 370 frames two VBLs after the one before: 30 fps), the frames reaching it
about 10 ms after the vblank. New stats lines: commit to done, made to committed (and of that, the draw calls), allocations and imports.
A command buffer ran in about 4.5 ms; Core Animation took 3.5 ms from making one to committing it, only 0.2 ms of it in the draw calls:
the rest was the device's allocations, mainly imports. WindowServer makes textures of the same IOSurfaces (the displays', the windows')
every frame, and each import and its free were kext calls of about 1 ms (700 imports, 7 GB, 2 s of kext calls every 5 s). Now:

- An IOSurface's import is shared by the textures of it and kept 2 s after the last was made (by the surface's ID; the import holds the
  surface).
- Freed host memory is kept 2 s for an allocation of the same size (at most 128 MB), handed out zeroed.

Card, the glass window moving on the 4K display: made to committed 3.5 -> 0.6 ms, imports 700 -> 40 and allocation calls 350 -> 80 ms
per 5 s; frames shown at the next vblank 24 of 370 -> 633 of 680 (about 59 a second).

Render-pass clears on the card: each was a step of its own (a drain of every draw before it, then the CPU filling up to 33 MB), about 9
a frame. A 2D target whose pixel is a 32-bit pattern repeated (4-byte formats, and 8-byte ones whose halves match) is now filled by
DMA_DATA with SRC_SEL DATA behind the idle barrier, in order with the draws; the CPU fills the rest as before. Card: non-draw steps
2800 -> about 0 per 5 s while a window moves, waiting for draws 350 -> 130-200 ms per 5 s; Mission Control and window drags render right.

**The bus is the limit (2026-11-20, afternoon):** with more apps open (Safari, Mail, Maps, Photos, ...) the glass window's drag on the
4K display fell back to about 40 frames a second (409 of 556 at the next vblank), the card about 94 % busy. Measured: of what draws
cover, about 94 % goes into IOSurfaces (WindowServer's displays' frames, made as render targets), about 6 % into the device's own
textures, so private textures in VRAM would barely help. A blended full-screen 4K quad took 0.92 ms on the card into host memory and
0.17 ms into VRAM (`mtlprobe nativebench`, in the stash below). Tried: the displays' surfaces held in VRAM (drawn and read there, the
rows drawn written back into the surface after each command buffer, filled again when the CPU wrote it): Core Animation redraws most of
the frame, the write-backs came to about 3 GB/s, and frames got worse (387 of 538 at the next vblank). Kept as the stash "experiment:
held display surfaces in VRAM (rdna4-nativeheld=1) + mtlprobe nativebench". What would pay: the kext presenting from the VRAM copy
(no write-back; other readers, screenshots, given the surface on demand), and bounds for `VfxXgb` (vertex pulling, three layouts) so its
full-frame self-read copies shrink.

**Presented from VRAM (2026-11-20, evening; `rdna4-nativeheld=1`, off by default):** WindowServer's render-target IOSurfaces (its
displays' frames, the menu bars: made as render targets, at least 1 MB, BGRA8, one plane) are held in VRAM by the native device and
the kext presents them from there. Kext: `kRDNA4MethodPresentSource` (IOSurface ID, a VRAM buffer of the caller's; WindowServer only,
by process name: another process could put its own pixels on the screen), a table of present sources in the paravirtual host, mapped
into its GPU address space when first presented (`rtMapClientVram`: the client buffer's MC range, a mapping of the host's that frees
only the mapping), dropped when the device unregisters, the client closes or the runtime goes (sleep). Device (`RDNA4Held`): one VRAM
copy a surface, the surface's layout; filled from the surface when first used or when its seed moved, registered once current,
unregistered when the CPU touches the surface; the drawn rows (up to 8 ranges) written back when WindowServer goes idle (100 ms), at
least each second, and before a CPU step, not after every command buffer.

Card, the glass window moving on the 4K display with 8 apps open: frames at the next vblank 74 % -> 98 % (705 of 721), a command buffer
commit to done 4.5 -> 2.2 ms (at most 6), write-backs about 80 MB a second; the 1080p display 695 of 702; Mission Control right.
Not tested: system sleep with held surfaces (VRAM is lost in sleep; the device does not refill on wake yet), a CPU screenshot
(`screencapture` cannot capture over ssh), display mode changes.

**Cursor and frame pacing (2026-11-20, evening):** Sunneva: dragging windows and moving the mouse still a bit laggy and choppy. The
paravirtual host moves the hardware cursor and raises the VBL events WindowServer paces on from its tick; its VBL events came up to
8-16 ms late (15 in 5 s by 2 ms or more) while a window moved. Two causes: `rtWaitGfxFence` busy-polled its first 2 ms holding the
runtime's lock, so every wait (WindowServer's slot waits and drains, the host's ~400 a second) kept the other clients from submitting
and the host from presenting; and the host's own waits for the card (behind WindowServer's frames) left the cursor and the VBLs standing.
Now the runtime polls the first 2 ms without the lock (the caller's own fence page stays while its call runs), and the host waits in
short turns with `pollDisplay` (cursor, VBL events, the interrupt) between them. Card: VBL events 240-400 -> 140 us late on average
while a window moves, 1 in 5 s by 2 ms or more (the latest 4.4 ms, was 8-16); frames at the next vblank 97 %.

**Where a frame's time goes (2026-11-20, evening):** Sunneva: dragging smoother, still slightly laggy. The one-second pointer freezes
were the USB controller XH00 resetting (fixed by a power cycle and unplugging the UR22). New timing: `debug.rdna4_pvvblat` (when display
0's last VBL event was raised, the same clock as the device's), the native device timing each command buffer from it, and per port a
histogram of VBL to present. While the glass window moves on the 4K display: a command buffer is made 5.5 ms after the VBL on average,
spread over the whole frame (WindowServer's own work: 9 % under 1 ms, 26 % after 8 ms), committed 0.4 ms later, done 1.7 ms after that;
VBL to present 28 % under 4 ms, 30 % 4-8, 27 % 8-12, 13 % 12-16, 1 % later. `debug.rdna4_pvvbldelay` (us after the vblank to raise
the VBL event, 0 by default) moves WindowServer's frame later in the period, so the frame waits less for the vblank: tried at 3 ms, about
a fifth of the frames then miss the earlier vblank (a frame later), worse than 0. The device's own share is about 2 ms of a frame.

**WindowServer's main thread waited for the card (spindump, 2026-11-20):** with `sudo -n /usr/local/libexec/rdna4-spindump.sh WindowServer 10`
(installed by `~/rdna4/add-spindump-sudoers.sh`) during a window drag: the main thread spent 23-25 % of the time in
`commitAndWaitUntilSubmitted` -> `waitUntilScheduled`, because a command buffer counted as scheduled only once its draws were done (the
tearing fix). Scheduled now means every draw is on the gfx ring (the kext's present copies behind them on the same ring, after an idle
barrier: no torn frame in the grabs); 64 draw slots instead of 16 (the submit loop waited for the card to free slots: slot waits 340 ->
45 ms per 5 s), the runtime's gfx submission limits 64 -> 128 (per client and across clients, 2816 of the ring's 4096 dwords), a submit
retried while the ring is full, the work queue at user-interactive QoS (it ran at 31). Main thread during the drag: composing 401 -> 277
samples of 1000, waiting for the device 230 -> 152, idle 590 -> 708. What is left of the wait is the device's own submit loop (about
1.4 ms a command buffer: a draw built, 9 us, and submitted with a kext call, 8 us).

Draw submits batched (2026-11-20, evening): each draw (and barrier) still writes its IB into its slot's 4 KB page, the rest of the page a
NOP (`0xc0001000 | (room - 2) << 16`, or the one-dword `0xffff1000`), so the pages of consecutive slots are one IB, submitted with one
kext call: at 32 slots (`kBatchSlots`), before the slots wrap (128 now), before every wait (`drainGfx`) and before a command buffer is
scheduled (its draws must be on the ring before WindowServer presents). Card: kext submit time 294 -> 53 ms and slot waits 340 -> 36 ms
per 5 s while a window moves; WindowServer's main thread waiting for the device 152 -> 89 samples of 1000 (230 before a0b36cc);
frames 328 of 329 at the next vblank, no torn frames, no hangs, Mission Control right.

**Apps on the native device, step 1: Safari (2026-11-20, evening).** `debug.rdna4_nativeapps` (and the boot-arg `rdna4-nativeapps`): process
names besides WindowServer that get the native device (with `rdna4-nativews=3`). What it took:

- **Sandboxes.** Safari's processes may open only an IOAccelerator's user clients (`(iokit-connection "IOAccelerator")` in WebKit's
  profiles; their message filter allows every method, "with report"): ours (`RDNA4ComputeService`) was refused ("not permitted").
  `IOAccelerator` lives in IOGraphicsFamily, which is in the system kernel collection and not the boot one OpenCore injects into, so our
  service cannot subclass it. Instead the kext routes `IOGraphicsAccelerator2::newUserClient`: type `RDN4` (`RDNA4_NATIVE_UC_TYPE`) on the
  paravirtual accelerator is the runtime's compute client, attached to the accelerator (`rdna4AcceleratorClient`). The device opens
  that first (`rdna4_open_accelerator`), our service second. WindowServer goes this way too now.
- **The plug-in per process.** With route 3 the accelerator's `MetalPluginName` was ours for every process, and WebKit's GPU process,
  whose sandbox maps code only from the system volume (`/System/Library/Extensions`, ...), failed to load it ("file system sandbox
  blocked mmap() of /Library/GPUBundles/..."): no Metal device there at all since the native desktop (54 failures in 6 hours), web pages
  drawn without the GPU. Now the accelerator keeps Apple's names, ours are `RDNA4MetalPluginName` / `ClassName`, and the kext routes
  `is_io_registry_entry_get_property_bin_buf`: a native process (`accelNativeCaller`: WindowServer and the list) reading the plug-in
  name reads ours. Every other process loads Apple's paravirtual bundle again.
- **What runs on it.** Safari's own process (its windows' Metal use); its pages are drawn in WebKit's GPU process, which cannot map our
  bundle from `/Library/GPUBundles`: that would need it in `/System/Library/Extensions` (SIP and the sealed system volume off; Sunneva's
  call). Card: Safari opens, loads Wikipedia, its window right.
- **Maps** (`Safari,Maps`): VectorKit aborted ("Metal returned null texture"): cube textures (type 5) are not supported yet, `newFence`
  is missing. Taken off the list.

Maps on the native device, round 2: 2D array and cube textures (one level; their images laid out as a 3D texture's slices, the T#
`linearArray2d`, typed CUBE for cubes; `replaceRegion` / `getBytes` by slice), `newFence` (an `RDNA4MTLFence`: nothing to wait for on
one work queue), `commandBufferWithDescriptor:`. Maps then runs, its interface right, the map black: its pipelines have a
Depth32Float_Stencil8 attachment, refused ("not yet"), and it makes 2D multisample textures. Stencil and MSAA (the PV host has both) are
what the native device needs next for Maps and 3D apps; Maps stays off the list until then.

