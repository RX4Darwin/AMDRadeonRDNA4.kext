# MetalFX on our device, backed by AMD FSR, plan

Status: plan, 2026-11-24. Nothing built. Asked for by Sunneva after the FSR 1 work (docs/fsr1-upscaling-plan.md).

## Why

FSR 1 in the driver (Super Resolution) upscales only the finished frame. Temporal upscaling (FSR 2/3, DLSS, MetalFX's temporal scaler)
is much better, but needs what only the game has: its depth, motion vectors and camera jitter. A driver cannot add it to a game; it can
serve a game that asks for it. On macOS games ask through **MetalFX** (`MTLFXSpatialScaler`, `MTLFXTemporalScaler`). Answering those on our
device with AMD's FSR gives every MetalFX game FSR-quality upscaling on the card. FSR 4 is not an option: AMD ships it only as signed
Windows DLLs, and the macOS port that exists extracts its model from that DLL (see the FSR 1 plan and fsr-and-submodules notes).

## What there is (checked on the rig, 2026-11-24)

- **Games:** two apps on the rig link MetalFX, both x86_64: Quake II's Metal port (`r_metalfx`, spatial, temporal, temporal denoised and
  frame interpolation; it logs what `supportsDevice` says) and MetalQuake (a "MetalFX Upscaling" preset: Spatial v1, Temporal v2).
- **MetalFX with our device today:** `+[MTLFXSpatialScalerDescriptor supportsDevice:]` and the temporal one both say YES. The spatial
  descriptor's `newSpatialScalerWithDevice:` returns nil. The temporal one returns Apple's `_MFXTemporalScalingEffectNRS`, whose command
  buffers complete with nothing run: our device refuses MetalFX's library ("no metallib at .../MetalFX.framework/Resources/default.metallib",
  though the file is there), its dispatches are left out ("a dispatch without a compute pipeline") and a render pipeline of it is refused.
  Its `default.metallib` is AIR (720 KB), so our translator could read it, but its temporal path is Apple's neural one (`.mtlpackage`
  networks for the Neural Engine, tile and imageblock functions): not something to make run on this card.
- **FSR 3.1's upscaler:** MIT, in the FidelityFX SDK. v1.1.4 is the last tag with both backends: `sdk/include/FidelityFX/gpu/fsr3upscaler/`
  (the passes, written for HLSL and GLSL through `ffx_core`) and `sdk/src/backends/vk/shaders/fsr3upscaler/*.glsl` (the nine compute passes:
  prepare inputs, luma pyramid (SPD), shading change pyramid, shading change, prepare reactivity, luma instability, accumulate, RCAS,
  debug view; and the optional autogen reactive). The 2.x tags are DirectX 12 only.

## Plan

0. **Submodules** (lib/, as the rule says): `lib/FidelityFX-SDK` at v1.1.4 (shallow; check its size first: the samples' media may be large)
   and `lib/SPIRV-Cross` (Khronos, Apache-2.0) for the shader conversion. glslang is already needed for Mesa.
1. **Shaders, at build time:** each GLSL pass compiled with glslang to SPIR-V (the defines the VK backend's CMake uses: FFX_GPU, FFX_GLSL,
   FFX_HALF on, the permutations MetalFX's options need: HDR or not, jittered motion vectors, depth inverted, low resolution motion
   vectors), converted to MSL by SPIRV-Cross (`--msl --msl-version 30000`), compiled by Apple's `metal` to AIR in one metallib in the bundle,
   translated by air2amdgpu at first use like any kernel. Check each pass translates on the Mac first (`air2amdgpu <metallib> <fn>`); the gaps
   are fixed in the translator as for MetalGraphicsPlayground. A metallib of the bundle (not an AIR copy of AMD's HLSL) keeps the build
   reproducible and the source AMD's.
2. **Spatial first, as a step on its own:** `newSpatialScalerWithDevice:` on our device answered by an object of ours conforming to
   `MTLFXSpatialScaler` (colorTexture, outputTexture, inputContentWidth/Height, colorProcessingMode, encodeToCommandBuffer:), backed by the
   FSR 1 kernels already in the bundle (`upscale:into:`, extended to RGBA16Float for HDR colour). Small, and testable with Quake II's and
   MetalQuake's spatial modes. Replaces the nil of today, so those modes work at all.
3. **Temporal:** `newTemporalScalerWithDevice:` answered by an object conforming to `MTLFXTemporalScaler`: its inputs map one to one on
   FSR 3.1's dispatch description (color, depth, motion with motionVectorScaleX/Y, jitterOffsetX/Y, exposure or auto exposure, preExposure,
   reactiveMask on macOS 14+, reset, depthReversed, inputContentWidth/Height); `encodeToCommandBuffer:` records the passes as compute
   dispatches on our command buffer, FSR's own resources (history, lock and luma textures, the SPD counter) kept by the scaler. Report
   `supportedInputContentMin/MaxScaleForDevice` as FSR's (1x to 3x).
4. **Hooks:** the two descriptor methods replaced once a process (as Super Resolution's CAMetalLayer methods), only for our device; any
   other device gets Apple's. A switch in RDNA4Gadget's Tweaks tab ("MetalFX: AMD FSR"), on by default once it works (without it, MetalFX
   does nothing on our device today).
5. **Card tests:** Quake II `r_metalfx` spatial and temporal, MetalQuake's two presets: picture, motion (ghosting, disocclusion), the
   cost per frame of each pass at 4K output, reset on camera cuts, a window resize (a new scaler), display sleep.

## Open questions

- Which of Quake II's and MetalQuake's MetalFX settings ask for what (formats, auto exposure, reactive mask). Read their source first.
- `frame interpolation` (MTLFXFrameInterpolator, macOS 26) and the denoised temporal scaler: out of scope; FSR 3's frame generation could
  answer the first later.
- The translator has to cope with SPIRV-Cross's MSL (its own idioms: spvUnsafeArray, texture2d<float, access::read_write>, packed types).
- Effort: several days of translator and bundle work; step 2 alone is small.
