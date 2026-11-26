# FSR 2 and FSR 4: are they worth adding? (2026-11-26)

Asked for in the work queue: evaluate FSR 2 and FSR 4 as `lib/` submodules. Short answer: neither is worth adding. FSR 2
adds nothing the FSR 3.1 upscaler we already run does not do, and none of the FSR 4 sources can be shipped. Only the
repos' GitHub pages were read (nothing downloaded or cloned).

## What we have

- FSR 1 (spatial, `lib/FidelityFX-FSR`): the driver's render scale and MetalFX's spatial scaler.
- FSR 3.1's upscaler (temporal, `lib/FidelityFX-SDK` at AMD FidelityFX SDK 1.1.4, MIT): MetalFX's temporal scaler on our
  device (docs/fsr3-metalfx-plan.md). Games reach it through MetalFX, so they need no AMD code of their own.

## FSR 2 (GPUOpen-Effects/FidelityFX-FSR2)

- AMD's own repo, FSR 2.2.1 (May 2023), MIT-style licence from AMD with no extra conditions.
- C/C++ runtime with HLSL compute shaders and DX12 and Vulkan backends; no Metal backend, no ML weights.
- FSR 3.1's upscaler is FSR 2's successor (same temporal design, later fixes) and is already in `lib/FidelityFX-SDK`
  with the same licence. FSR 2 would be a second, older copy of the same pass.
- A game that ships FSR 2 compiles it into itself (through Vulkan/MoltenVK on macOS); the driver has nothing to provide
  for it.

Verdict: do not add. If a lighter temporal pass is ever wanted, FSR 3.1's upscaler has the same quality presets.

## FSR 4

AMD ships FSR 4 (the ML upscaler, made for RDNA 4's WMMA instructions) as signed prebuilt Windows DLLs. Neither repo
changes that:

- **UzenUPoZiTiV4ik/fsr4-native-macos** (individual): runs "FSR 4.1.2 INT8 Lite" on Apple silicon through CrossOver's
  D3DMetal. Its importer takes the shaders and the model's initializers from AMD's Windows driver DLL
  (`amdvkffx64.dll`, supplied by the user) and translates them to Metal; its optional lossy profiles use weights it
  describes as GPL-trained. No licence file (the README calls the code MIT). Not usable here: it is built on AMD's
  proprietary DLL contents, targets Apple GPUs (not RDNA 4 under macOS), and hooks D3DMetal rather than a driver.
- **Uklosk/FidelityFX-SDK-2.0.0** (individual, one commit): a copy of AMD's FidelityFX SDK 2.0.0 tree with an MIT
  licence in someone else's name (2025). The FSR 4 files in it (ML runtime and `initializers.bin` weights) carry
  "Copyright AMD. All rights reserved.", so the repo's MIT licence does not cover them. DX12 only. Not usable: the FSR 4
  part is not licensed for redistribution.

Verdict: FSR 4 stays out until AMD publishes it under a licence that allows it. Our temporal path (MetalFX → FSR 3.1)
remains the legitimate one.
