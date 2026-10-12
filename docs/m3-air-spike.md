# M3 track (b) spike: Apple AIR -> gfx1201 through upstream LLVM

hub-task-433 (Lathe), branch `premetal/m3-air` (base `premetal/metal-spike` ce583c4). Everything here runs on Linux; no macOS, no reboot, no download.
Status words: **card** = measured on the RX 9070 XT; **host** = measured on this machine without the card (LLVM 22.1.8 tools, the extracted Apple files);
**code** = written, never run; **INFER** = reasoning, not measured; **UNKNOWN**. Apple's binaries and their disassembly stay in `~/work/tools/macos-full/25G83/metallib/`
(outside the repo); nothing derived beyond aggregate counts, entry-point names and the hand-written sample kernel is in git. The tools were written from the bytes of the
files and the public write-ups ([worthdoingbadly](https://worthdoingbadly.com/metalbitcode/), [YuAo/MetalLibraryArchive](https://github.com/YuAo/MetalLibraryArchive), the `file(1)` magic
file that ships with macOS); no code was copied from LGPL or TSNPL projects.

## 0. The answer in one page

1. **The AIR is obtainable on Linux, and it is readable by stock LLVM 22.** The installer payload holds 212 `.metallib` files (858 MB), including
   **`SkyLightShaders.air64.metallib`** (31 shaders) and **QuartzCore's `default.metallib`** (180 shaders; a fat archive whose slice 0 is the AIR, the other 18 slices are Apple-GPU
   native code). They are not in the unreachable cryptex. [host]
2. **28,683 of 28,683 extracted AIR modules disassemble with `llvm-dis` 22.1.8**, no unknown record, attribute or bitcode version (section 3). The AIR is LLVM-IR with opaque
   pointers (Apple clang 32023.886, AIR 2.8, Metal 4.0), plus `air.*` metadata and `air.*` intrinsic calls. [host] `llvm-dis` parses; it does not verify, and
   `opt` refuses the triple `air64_v28-apple-macosx26.6.0` (the lowering tool re-triples before verifying).
3. **What the two shader sets need is small** (section 4): SkyLight uses **22** `air.*` intrinsic families, all `float` (no `half`), address spaces 1 and 2 only, no
   threadgroup memory, no atomics; QuartzCore uses 66 families, adds `half`, imageblocks (16 of 180 modules) and `air.fwidth`/`discard`.
4. **A real Apple compute kernel runs on the card after a ~600-line lowering** (`tools/air/air2amdgcn.cpp`, section 5): Apple's own `group_uniform_add_float`
   (CorePhotogrammetry) and `ReduceSumKernel_uint` (CoreRE3DGSFoundation; 1024-thread groups, 4 KiB LDS, barrier, `simd_sum`) were lowered to gfx1201 wave32 code
   objects and **both produced exactly the CPU reference on the card** (section 6; amdgpu compute ring, reset state 0, no kernel log lines). [card]
5. **Coverage today** (section 7): of 10,720 unique compute kernels in the corpus, **2,281 (21.3 %)** lower and compile for gfx1201 with this prototype
   (host; not run, fixed 64-thread groups); the rest stop on textures (the dominant blocker), function constants, indirect/argument buffers, ray tracing, matrix ops. SkyLight: **7 of 7
   vertex and 6 of 24 fragment shaders** lower and are accepted by the AMDGPU back end as plain functions; the other 18 fragments stop on `air.sample_texture_*` (and
   function constants); QuartzCore: 33 of 37 vertex, 18 of 119 fragment. [host]
6. **The graphics path is not a missing LLVM feature, it is missing front-end work** (section 8): LLVM 22 accepts `amdgpu_gs`/`amdgpu_ps` for gfx1201 but emits no NGG
   prologue (EXEC SET from `merged_wave_info`, primitive export, attribute-ring stores, `s5` = gs_attr_offset); that is our G4 recipe (`docs/g4-colour.md`) expressed as IR the
   lowering must generate, and texture/sampler operations need a descriptor ABI that the runtime and the lowering agree on.
7. **kext requirement (card, measured; section 6.2):** LLVM-compiled gfx1201 kernels read the work-group ID from `ttmp9` (x) and `ttmp7` (y | z << 16). The enable is
   `COMPUTE_PGM_RSRC2.TGID_X_EN/TGID_Y_EN/TGID_Z_EN` (bits 7/8/9) taken from the code object's kernel descriptor, nothing else in the IB. The kext **already** meets this for X:
   the repo's clang-built `vadd.cl` (the kernel the kext runs on the card) reads `ttmp9` with RSRC2 = 0x84.

## 1. Samples: extraction (step 1)

Source: the 18.4 GB universal installer asset already on disk (`~/work/tools/macos-full/25G83/asset/*.zip`, `payloadv2/payload.000-052`, section 9 of
`docs/metal-spike.md`). `zpay` (Forge's `tools/e2/zpay-main.go.txt`, already built) listed `metallib` over all 53 payload chunks (97 s) and extracted every match
(80 s) to `~/work/tools/macos-full/25G83/metallib/tree/`. 213 matches: 212 regular files (858 MB; each size equals the listing) and one symlink
(`CoreImage.framework/CoreImage.metallib`). One of the 212 is not a metallib (`usr/share/file/magic/metallib`, the `file(1)` magic: it documents the header). **Nothing was downloaded.**

| container (what `tools/air/metallib.py` calls it) | files | functions | what it is |
|---|---:|---:|---|
| `MTLB` | 180 | 25,354 | the metallib proper; AIR bitcode per function |
| `FAT` with an MTLB slice | 1 | 180 | **QuartzCore `default.metallib`**: a big-endian fat archive (`0xcafebabe`, 19 slices): slice 0 = MTLB (AIR, 1.6 MB), slices 1-18 = Mach-O images of **precompiled Apple-GPU code** (cputype `0x01000013`, one per AGX generation, ~9 MB each) |
| `BUILTIN` | 21 | 3,149 | AGX's internal builtin libraries (`AGXMetal*/Resources/*_rt.metallib`, `tensor.metallib`, `tex_atomic_emu`...): `u32 count`, `count x (NUL name, u32 offset)`, each offset a `u32 size` + wrapped bitcode. Apple-GPU internals (they contain `llvm.agx2.*` / `llvm.agx3.*` intrinsics); irrelevant to RDNA4 |
| `FAT` without an MTLB slice | 8 | - | native Apple-GPU archives only, no AIR: CoreImage `ubershader_archive_bin` / `ci_uberwrapper_bin` / `coreui_archive_bin`, RenderBox `archive.metallib`, Vista, MXI, IconRendering, CompositorServices `default.binary.metallib` |
| (one MTLB with zero functions) | 1 | 0 | |

Owners of the AIR (functions): MetalPerformanceShaders 7,823 (7 libraries), CoreImage 3,322, ShaderGraph 2,040, Espresso 1,988, AvatarKit 1,510, VFX 1,170, CoreRE 885, SceneKit 678,
GPUToolsReplay 599, CorePhotogrammetry 559, ... 126 owners in all (list: `tools/air/disall.py` results). The ones WindowServer needs: **SkyLight** (`SkyLightShaders.air64.metallib`: 7 vertex + 24 fragment,
557 KB) and **QuartzCore** (180: 37 vertex, 119 fragment, 24 kernel). CoreImage ships 15 files (3 are native-only archives; `ci_*_stitchable*.metallib` are "visible" functions linked at run time).
A `visible` entry (8,985 functions) is a stitchable function, not a dispatchable one.

## 2. The container, as parsed (step 2): `tools/air/metallib.py`

MTLB, little endian (the meaning of the first fields follows the `file(1)` magic that ships in the same payload):

| offset | field |
|---|---|
| 0x00 | `"MTLB"` |
| 0x04 | `u16 0x8001` (0x8000 = macOS flag | container version 1), `u16 2`, `u16 9` (container 1.2.9), `u8 file type` (0 executable, 1 core image library, 2 dynlib, 3 companion), `u8 0x81` (platform macOS), `u16 26`, `u16 6` (OS 26.6) |
| 0x10 | `u64` file size (equals the real size: used as a sanity check) |
| 0x18, 0x28, 0x38, 0x48 | `u64 offset, u64 size` of: function list, public metadata, private metadata, bitcode |
| function list | `u32 count`, then `count` entries: `u32 entry_size` (including itself), tags `4-char name, u16 length, data`, closing `ENDT` (no length) |
| tags | `NAME` (NUL string), `TYPE` (u8: 0 vertex, 1 fragment, 2 kernel, 3 unqualified, 4 visible, 5 extern, 6 intersection, 7 mesh), `HASH` (sha256), `OFFT` (3 x u64: public md, private md, bitcode offset, relative), `VERS` (air 2.8, language 4.0 as u16 x4), `MDSZ` (u64 module size), `RFLT` |
| module | at `bitcode_offset + OFFT[2]`, `MDSZ` bytes: a bitcode wrapper (`0x0b17c0de`, version, offset, size, cputype: 5 x u32) around raw `BC C0DE` |

`metallib.py list FILE [--json]` prints the function list; `extract FILE OUTDIR [--name REGEX] [--wrapped]` writes each module as `NNNN_<name>.bc` (raw bitcode, `llvm-dis`-ready).
Public/private metadata (reflection) were not decoded: nothing below needed them, the `air.*` metadata inside each module carries the argument ABI. [host]

## 3. llvm-dis over everything (step 3): `tools/air/disall.py`

`llvm-dis` 22.1.8, 16 threads, 60 s: **28,683 modules (25,354 MTLB + 180 QuartzCore + 3,149 AGX builtin) -> 28,683 disassembled, 0 failures** (26,543 distinct by SHA-256).
No bitcode-version, unknown-attribute or unknown-record error occurred. The modules carry:
`target triple = "air64_v28-apple-macosx26.6.0"`, `!air.version = {2, 8, 0}`, `!air.language_version = {"Metal", 4|3, 0, 0}`, `!llvm.ident = "Apple metal version 32023.886 (metalfe-32023.886.1)"`,
opaque pointers, current-LLVM attributes (`captures(none)`, `memory(argmem: read)`), module flags `air.max_device_buffers 31`, `air.max_constant_buffers 31`, `air.max_threadgroup_buffers 31`,
`air.max_textures 128`, `air.max_read_write_textures 8`, `air.max_samplers 16`, and compile options `air.compile.denorms_disable`, `air.compile.fast_math_enable|disable`,
`air.compile.framebuffer_fetch_enable|disable`. [host] So the Apple front end is a recent LLVM, and LLVM 22 is new enough to read it: **no bitcode reader work is needed.**
Not established: that the IR *verifies* (the LLVM verifier rejects the triple only because `opt` does not know `air64`; the lowering tool verifies after re-triple, and passed for every module it lowered).

## 4. Census (step 4): `tools/air/census.py`, full tables in `docs/m3-air-census.md`

24,247 unique non-builtin modules; instruction counts: median 78, p90 813. [host] Entry types (unique modules): kernel 10,720, visible 8,950, fragment 2,872, vertex 1,677, intersection 15, extern 8, mesh 2.

**What an AIR entry looks like** (from `SimpleTextureFragment` and `group_uniform_add_float`, verbatim shapes):

    define <4 x float> @SimpleTextureFragment(<4 x float> %pos, <2 x float> %tex, ptr addrspace(1) %tex2D, ptr addrspace(2) %samp)
    !air.fragment = !{!15}                     ; !{ptr @fn, <outputs>, <inputs>}
    !17 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}                        ; output
    !19 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", ...}                         ; input 0
    !20 = !{i32 1, !"air.fragment_input", !"generated(3texDv2_f)", !"air.center", !"air.perspective"...}
    !21 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", ...}             ; texture slot 0
    !22 = !{i32 3, !"air.sampler", !"air.location_index", i32 0, i32 1, ...}                            ; sampler slot 0
    %5 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %2, ptr addrspace(2) %3, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)

    define void @group_uniform_add_float(ptr addrspace(1) %out, ptr addrspace(2) %group_sum, ptr addrspace(2) %num_input, i32 %tid, i32 %gid)
    !air.kernel = !{!15}
    !18 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, ...}
    !21 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", ...}

So: the **function signature follows the metadata list** (one IR argument per metadata node, in order); a vertex function takes `air.vertex_input` (attribute fetch by location) and returns a struct of
position + `air.vertex_output`s; a fragment takes `air.position`, interpolated `air.fragment_input`s (centre/centroid/sample, perspective/no_perspective) and returns render targets; buffers are
pointers in AIR address space 1 (device) or 2 (constant); **textures are opaque `ptr addrspace(1)` handles and samplers opaque `ptr addrspace(2)` handles**, only meaningful as arguments of `air.*` calls;
constant samplers are baked into the module as 16-byte constants (`@__air_sampler_state = internal addrspace(2) constant [2 x i64] [i64 34901797601018002, i64 0]`, listed in `!air.sampler_states`).

### 4.1 All non-builtin modules (24,247 unique)

| air.* intrinsic family | modules | uses | | metadata key | modules |
|---|---:|---:|---|---|---:|
| `air.convert` | 12,385 | 146,738 | | `air.buffer` | 14,039 |
| `air.sample_texture_2d` | 6,822 | 56,602 | | `air.texture` | 9,618 |
| `air.write_texture_2d` | 5,896 | 53,345 | | `air.thread_position_in_grid` | 8,213 |
| `air.dot` | 5,628 | 102,221 | | `air.visible_input` / `air.stitching_*` | 8,824 / 7,762 |
| `air.fast_fmax` | 4,511 | 28,500 | | `air.read_write` / `air.write` | 7,295 / 6,653 |
| `air.wg.barrier` | 2,773 | 31,622 | | `air.thread_position_in_threadgroup` | 4,039 |
| `air.fast_rsqrt/fabs/sqrt/fmin/clamp` | 2,300-2,600 each | | | `air.position` / `air.render_target` | 4,246 / 2,849 |
| `air.mix`, `air.fast_floor`, `air.fma`, `air.min/max` | 1,100-2,200 | | | `air.sampler` / `air.sampler_state` | 3,241 / 4,658 |
| `air.read_texture_2d`, `air.get_read_sampler`, `air.get_width/height_texture_2d` | 1,100-1,500 | | | `air.function_constant` | 1,543 |
| `air.atomic.*`, `air.simd_*`, `air.is_uniform`, `air.fast_atan2/atan/tanh` | hundreds | | | `air.indirect_buffer`, `air.imageblock`, `air.patch*`, `air.vertex_id` | 1,074 / 16+ / 675 / 642 |

Address spaces (modules): **2 constant 20,279**, **1 device 15,052**, **3 threadgroup 3,090**, 4 (140; the type returned by `air.imageblock_data`, i.e. imageblock/tile memory [INFER]), 5/7 (a handful).
Vectors: `<4 x float>`, `<2 x float>`, `<4 x i32>`, `<3 x i32>` (uint3 builtins), `<4 x half>`; scalars: `i32`, `i64`, `float`, `half` (8,548 modules), `bfloat` (540). Named metadata: `air.kernel` 10,720,
`air.visible` 8,950, `air.function_constants` 5,568, `air.sampler_states` 4,658, `air.fragment` 2,872, `air.vertex` 1,677. `air.version`/`air.language_version`/`air.compile_options` in every module.

### 4.2 SkyLight (31 shaders: what WindowServer composites with)

Entry points (instruction counts in `docs/m3-air-census.md`): vertex `SimpleVertex`, `SimpleColorVertex`, `SimpleVertexShadow`, `SimpleMeshVertex`, `BlurCompositeVertex`, `UberCompositeVertex`, `SimpleTextureLightingVertex`
(15-25 instructions each: a matrix multiply and a pass-through); fragment `SimpleTextureFragment` (**3 instructions**: one `air.sample_texture_2d`), `SimpleColorFragment` (1), `AlphaTextureFragment`, `SimpleGrayscale`, `InPlaceSover`, `InPlaceColorOrClampEDR`,
`SimpleTextureTint/EDR/ScaleToSDR/Lighting`, `GroupFadeTextureFragment`, `RippleFragment`, `ColorFillYCbCr(_ChromaOnly)`, `ShadowCompositeFragment`, `UberCompositeFragment` (349), `BlurComposite` (350),
`UberResampleLanczosFragmentBGRA/YCbCr` (397/410), `Shadow{Horizontal,Vertical}Blur{,RGBA}Fragment` (11,722-12,226: fully unrolled Gaussian blurs).

| | SkyLight |
|---|---|
| `air.*` families (22) | `sample_texture_2d` (18 modules, 4,103 call sites), `fast_fmax`, `dot`, `normalize_function_constant_predicate`, `convert`, `fast_sqrt`, `fast_pow`, `fast_saturate`, `sample_texture_1d_array`, `fast_fabs`, `mix`, `fast_clamp`, `fast_fmin`, `dfdx`, `dfdy`, `fast_rsqrt`, `all`, `any`, `discard_fragment`, `fast_fract`, `sample_texture_1d`, `sign` |
| types | **float only** (`float`, `<2/3/4 x float>`, `i32`, `i64`, `i1`, `i8`); no `half`, no `bfloat`, no atomics, no threadgroup memory |
| address spaces | 2 (constant buffers, 29 modules) and 1 (textures, 18); nothing else |
| argument kinds | `air.position`, `air.fragment_input`, `air.vertex_input`/`air.vertex_output`, `air.buffer` (constant, read-only; structs up to a `float4x4`), `air.texture` (`texture2d<float, sample>`, `texture1d_array`, `texture1d`), `air.sampler` (6 modules) + **constant sampler states** (16 modules), `air.render_target` (1 colour output) |
| specialisation | `air.function_constants` in 9 modules (`ShadowCompositeFragment` has 4, `UberCompositeFragment` 14): feature switches resolved at pipeline creation; calls to `air.normalize_function_constant_predicate`; arguments tagged `!"air.function_constant"` are present or absent per pipeline |
| compile options | `fast_math_enable`, `denorms_disable`, `framebuffer_fetch_enable` in all 31 |

### 4.3 QuartzCore (180 shaders in the AIR slice)

119 fragment, 37 vertex, 24 kernel. 66 `air.*` families: SkyLight's, plus `half` arithmetic (`air.fma.f16`, `air.saturate.f16`, `air.dot.v3f16`; 104 modules use `half`), `air.fwidth` (17 modules), `air.discard_fragment` (11),
`air.read_texture_2d` / `air.write_texture_2d` (27 / 9), `air.get_read_sampler` (27), `air.sample_texture_3d`, `air.fast_atan2` (5), simdgroup/quad ops (`simd_shuffle_and_fill_down`, `quad_shuffle_xor`, 7 and 3 modules) and
**imageblocks**: `air.load/store.implicit_imageblock`, `air.imageblock_data`, `air.write_imageblock_slice_to_texture_2d` in 16 modules (`imageblock<ColorData, layout_implicit>`, `<VarBlurTextureData, layout_explicit>`; tile-memory
programming that exists only on Apple's tile-based GPUs). 7 modules use function constants; 8 use threadgroup memory; the 24 kernels are blur/downsample/glass helpers.
Arguments are mostly `float4`/`half4` structs of uniforms (`NarrowBlurUniforms`, `GlassBackgroundUniforms`, `CA::OGL::Metal::GammaLUTs`, ...).

## 5. The lowering (step 5): `tools/air/air2amdgcn.cpp`

A C++ tool against the system LLVM 22 (`tools/air/build-tools.sh`; `llvm-config --link-shared`), reading `.bc` or `.ll`. Two modes:
`kernel IN --tg X,Y,Z -o OUT.ll [--entry NAME]` produces an `amdgpu_kernel` for gfx1201 (then `llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx1201 -O2 -filetype=obj` and `ld.lld -shared`, `tools/air/build-kernel.sh`),
and `coverage [--codegen] IN...` lowers every `air.*` call of every function in place and reports what lowered, what has no rule, and whether the AMDGPU back end compiles it (one JSON line per module).

### 5.1 Compute ABI the tool produces (the contract with the runtime) [code, host-compiled; the first two kernels also card-run]

- Kernel arguments, in this order: **one 8-byte pointer per `air.buffer` argument** in AIR order (AIR address space 1 device -> AMDGPU 1 global; 2 constant -> AMDGPU **4** constant: AMDGPU's own 2 is GDS, so the module is
  printed, `addrspace(2)` rewritten and re-parsed: every GEP, load, global and declaration changes consistently); then **three hidden `u32`: the grid size in threads** (x, y, z; `dispatchThreads` semantic, `dispatchThreadgroups` = groups * tg);
  then **one hidden `u32` per threadgroup-memory argument** (AIR buffer in address space 3): its byte offset into the dynamic LDS, which the runtime computes from the `setThreadgroupMemoryLength` sizes (16-byte aligned)
  and adds to the dispatch (the kext's `dynamicLdsBytes`); the kernel reaches it through `external addrspace(3) global [0 x i8] @air.dynamic_lds`.
- The **threadgroup size is a compile-time constant** (`--tg`, `!reqd_work_group_size`, `amdgpu-flat-work-group-size`), as it is fixed per pipeline in Metal (INFER: the real compiler plug-in
  gets it from the pipeline descriptor). The launch must use `groups = ceil(grid / tg)` per dimension. Threads outside the grid return at once (Metal has no such threads: non-uniform last threadgroups);
  that is safe before barriers (the hardware counts waves, not lanes) and `simd_*` operations then see only real threads, like Metal. Edge groups report the full `threads_per_threadgroup` (differs from Metal for non-uniform dispatch; unmeasured).
- Builtins computed in a prologue (`air.thread_position_in_grid`, `threadgroup_position_in_grid`, `thread_position_in_threadgroup`, `thread_index_in_threadgroup`, `threads_per_threadgroup`,
  `threadgroups_per_grid`, `threads_per_grid`, `thread_index_in_simdgroup`, `simdgroup_index_in_threadgroup`, `threads_per_simdgroup`, `simdgroups_per_threadgroup`; scalar, `uint2`, `uint3`, `ushort*` forms) from `llvm.amdgcn.workgroup.id.{x,y,z}`,
  `llvm.amdgcn.workitem.id.{x,y,z}` and the hidden grid size. A simdgroup is one wave32 (INFER that this matches what Apple code assumes: 32 on Apple GPUs).
- Threadgroup memory = the module's `addrspace(3)` globals, unchanged (AIR 3 = AMDGPU 3). Their `zeroinitializer` initialisers become `poison` (AMDGPU accepts only undef; Metal threadgroup memory is uninitialised).
- Thread-local memory: AIR `alloca` is address space 0, AMDGPU wants space 5: each is re-created in space 5 and cast back to a flat pointer (clang's OpenCL scheme); `llvm.lifetime.*` on them are dropped.
- Attributes: `amdgpu-no-dispatch-ptr/queue-ptr/dispatch-id/implicitarg-ptr/heap-ptr/hostcall-ptr/...` are set because `llc` does not run AMDGPU's attributor: without them the kernel asks the CP for 8 user SGPRs instead of 2.
  `denormal-fp-math-f32 = preserve-sign` for `air.compile.denorms_disable`. The AIR `"air-buffer-no-alias"` parameter attribute is dropped; Apple's `!alias.scope`/`!noalias`/`!tbaa` metadata is kept (it is what makes the loads schedulable).
- Refused (with a message): AIR address spaces 4-7 (imageblock), texture/sampler/indirect-buffer/acceleration-structure/function-constant/stage_in arguments, and any `air.*` call without a rule. Nothing is silently dropped.

### 5.2 `air.*` -> LLVM rules implemented [code; compile-checked on all of section 7, run only for the cases listed in section 6]

| AIR | lowered to |
|---|---|
| `fmax/fmin/fast_*`, `fabs`, `floor/ceil/trunc/round/rint`, `fma`, `sqrt`, `pow/powr`, `exp/exp2/log/log2/log10`, `sin/cos` | `llvm.maxnum/minnum/fabs/floor/ceil/trunc/round/roundeven/fma/sqrt/pow/exp/exp2/log*/sin/cos` (`fast_*` carry fast-math flags) |
| `rsqrt` | `llvm.amdgcn.rsq` (scalarised for vectors) |
| `fract`, `mix`, `saturate`, `clamp`, `sign`, `dot`, `fmax3/fmin3`, `fmod`, `tanh`, `unpack.unorm4x8`, `popcount`, `abs`, `min/max/clamp` (int, `s`/`u`) | expanded (`x - floor(x)` capped below 1, `x + (y-x)*a`, min(max()), select chain, fma chain, `(e-1)/(e+1)` with clamp, ...) or `llvm.smin/umax/ctpop/abs` |
| `convert.<d>.<dt>.<s>.<st>` | `sitofp/uitofp/fpext/fptrunc/fptosi.sat/fptoui.sat` (Metal saturates), `icmp/fcmp` for `i1`, `sext/zext/trunc` by source sign |
| `all`, `any` | `llvm.vector.reduce.and/or` |
| `wg.barrier(flags, scope)` | `fence release` + `llvm.amdgcn.s.barrier` + `fence acquire`, `workgroup` scope (`agent` when the device flag is set); gfx12 emits `s_barrier_signal -1` / `s_barrier_wait`. Argument meanings (flags: device 1, threadgroup 2, texture 4; scope 1 threadgroup, 4 simdgroup) are INFER from the Metal enums and the two value pairs seen, `(2,1)` and `(2,4)` |
| `simdgroup.barrier` | `fence acq_rel syncscope("wavefront")` |
| `simd_sum`, `simd_max/min` (f32, i32) | `llvm.amdgcn.wave.reduce.{fadd,fmax,fmin,add,max,min,umax,umin}` (inactive lanes excluded, as Metal; f16 has no selectable form on gfx1201 in LLVM 22: left unsupported) |
| `simd_shuffle`, `_down`, `_up`, `_xor` (f32/i32/i16/f16) | `llvm.amdgcn.ds.bpermute` on the lane index (out-of-range deltas wrap: Metal leaves them undefined) |
| `simd_is_first`, `is_uniform` | `mbcnt(ballot(true)) == 0`; `ballot(x != readfirstlane(x)) == 0` |
| `get_simdgroup_size` | `32` |
| `atomic.{global,local}.{add,sub,and,or,xor,min,max,exchange}`, `load`, `store` | `atomicrmw` / atomic load / store, `monotonic`; scope: 1 -> `workgroup`, 2 -> `agent` (values seen: local atomics carry scope 1, global ones 2; INFER) |
| `discard_fragment` | `llvm.amdgcn.kill(false)` (pixel shaders only; compiled as plain functions this is not exercised) |

## 6. Run on the card (step 6) and the ttmp requirement

### 6.1 Results [card, 2026-10-01 19:14-19:15 local; REPLAY_COMPUTE_OK=1 with the lead's go-ahead hub-task-443, one run, no retry]

`tools/air/run-spike.sh run` builds everything, then `tools/air/test-kernels.py` launches each kernel with `tools/air/air-run.cpp` (a generalisation of `tools/linux-replay/replay-compute.cpp`: the same
`amdgpu_cs_submit` on the compute ring, same `pm4build.h` IB, same code-object reader; kernel name, grid, work-group size, kernargs and `COMPUTE_PGM_RSRC2.LDS_SIZE` are parameters) and compares with a CPU reference.

| kernel (Apple's, unmodified AIR) | launch | lowered object | result |
|---|---|---|---|
| `group_uniform_add_float` (CorePhotogrammetry): `out[tid] += group_sum[gid]` for `tid < num_input` | 16 groups x 64, grid 1,000 threads, `num_input` = 1,024 (so the hardware guard, not the kernel's own, must stop threads 1,000-1,023) | 3 VGPRs, 2 user SGPRs, RSRC2 0x384, 45 ISA lines | **PASS**: 1,024 floats bit-exact; 1,000 updated, the 24 past the grid untouched |
| `ReduceSumKernel_uint` (CoreRE3DGSFoundation): 1,024-thread group sums its inputs through threadgroup memory, a barrier and a `simd_sum`; thread 0 stores | 5 groups x 1,024 | LDS 4,096 B (RSRC2 0x40384), wave32, 73 ISA lines | **PASS**: 5 group sums of 1,024 random `uint`s (mod 2^32) |

Both: fence signalled, `amdgpu_cs_query_reset_state` 0, `journalctl -k` for the window empty. The two kernels are the *first lowered by this tool*; the tool was extended afterwards (alloca/address-space
fixes, atomics, more rules): re-lowering gives the same line counts (45 and 73 ISA lines), but the ISA was not byte-compared with the object that ran.

**Not run: `grid3d`.** `tools/air/samples/grid3d.ll` is hand-written, AIR-shaped (same metadata conventions; no Apple code): a `uint3` grid (30,10,5) with work-group (8,4,2), each thread writes its
`thread_position_in_grid`, `threadgroup_position_in_grid` and `thread_position_in_threadgroup`. It exercises exactly what the two runs above did not: Y/Z work-group IDs (`ttmp7`), the packed local IDs
(`v0`, `TIDIG_COMP_CNT` = 2, RSRC2 = 0x1384) and the grid guard in 3-D. It lowers and assembles on the host [host]. The lead approved one run (`REPLAY_COMPUTE_OK=1 python3 tools/air/test-kernels.py --only grid3d`), but the Claude Code permission
classifier refused the command (it treats the `REPLAY_COMPUTE_OK` gate as a "safety bypass flag") and I did not work around it. **Status: code, not run**; whoever has the go can run that one line.

### 6.2 ttmp9 / ttmp7: the work-group ID on gfx12, and what the kext must do

- LLVM's gfx12 code takes the work-group ID from **`ttmp9` (x) and `ttmp7` (y in bits 15:0, z in 31:16)**, not from system SGPRs after the user SGPRs as gfx10 does: `v_lshl_or_b32 v0, ttmp9, 6, v0`, `s_and_b32 s3, ttmp7, 0xffff`,
  `s_lshr_b32 s1, ttmp7, 16` (all three kernels, [host] disassembly in `build/air/k/*.dis`). [card] for X (all runs) and for "y = z = 0" (the guard compares them with the grid size and the results are right).
- **The enable is `COMPUTE_PGM_RSRC2.TGID_X_EN` [7], `TGID_Y_EN` [8], `TGID_Z_EN` [9]** (`gc_12_0_0_sh_mask.h`: `TG_SIZE_EN` [10], `TIDIG_COMP_CNT` [12:11], `LDS_SIZE` [23:15]); the code object's kernel descriptor
  already sets them (`.amdhsa_system_sgpr_workgroup_id_x/y/z`), and the IB adds **no other register**: `air-run` writes exactly `PGM_LO`, `PGM_RSRC1`, `PGM_RSRC2` (descriptor value, LDS field patched), `PGM_RSRC3`, resource limits, thread-per-SE masks, `START_*`, `NUM_THREAD_*`, `USER_DATA0..1`,
  `DISPATCH_DIRECT`, as the kext's own `rdna4-run` does. The Linux amdgpu `gfx_v12_0.c` has no TTMP set-up of its own (grep: none). Which hardware block writes the TTMPs when the bit is set is **INFER** (the architected-SGPR scheme of RDNA4;
  RDNA4 ISA pp. 43-44 per `docs/metal-spike.md` s.3.1, not re-read here).
- **Correction to the premise "our hand-written kernels use TGID system SGPRs":** the repo's clang-built `vadd.cl` (`shaders/vadd.cl` -> `src/vadd_codeobj.h`, the kernel the kext runs on the card) *also* reads `ttmp9` (`v_lshl_or_b32 v0, ttmp9, 6, v0` at +0xc) with RSRC2 = 0x84 (USER_SGPR = 2, TGID_X_EN). [host: disassembly of a fresh clang build; the embedded header
  is generated from the same source] So **the kext already provides `ttmp9` for LLVM-compiled kernels, by honouring the descriptor's RSRC2**; the only open items are Y/Z (`grid3d`), and that the kext does not mask `TGID_Y_EN/TGID_Z_EN` or `TIDIG_COMP_CNT` when it copies RSRC2
  (it patches only `LDS_SIZE`: `src/compute.cpp:3142`).
- Also required of any runtime: **`LDS_SIZE` is not in the code object's RSRC2** (it is 0): the dispatch must OR in `ldsSizeField(group_segment_fixed_size + dynamic)` (1 KiB granule, 512-byte units) or the kernel's LDS accesses fault (the kext does; `air-run` does).
  RSRC2 `TIDIG_COMP_CNT` = 2 means local IDs are packed in `v0` as {z:10, y:10, x:10} (`v_bfe_u32 v3, v0, 10, 10`); the SPI does that, no runtime work.

## 7. Coverage (the "coverage list of AIR features supported")

Host only; nothing here ran on the card. "Lowered + llc ok" = every `air.*` call has a rule above AND the AMDGPU back end (`gfx1201`, `-O2`) compiles the whole module as plain callable functions
(`tools/air/coverage-all.sh` + `coverage-report.py`; one process per module). "Refused/crash" = the tool refuses the module (imageblock address space) or LLVM aborts (`LLVM ERROR: Do not know how to promote this operator`, in a few modules).

| group | entry type | modules | lowered + llc ok | blocked by a missing air.* rule | codegen fail | refused / crash |
|---|---|---:|---:|---:|---:|---:|
| SkyLight | fragment | 24 | 6 | 18 | 0 | 0 |
| SkyLight | vertex | 7 | 7 | 0 | 0 | 0 |
| QuartzCore | fragment | 119 | 18 | 101 | 0 | 0 |
| QuartzCore | vertex | 37 | 33 | 4 | 0 | 0 |
| QuartzCore | kernel | 24 | 2 | 6 | 0 | 16 |
| ALL | kernel | 10753 | 2338 | 8227 | 0 | 188 |
| ALL | visible | 8985 | 7216 | 1697 | 0 | 72 |
| ALL | fragment | 2885 | 829 | 2056 | 0 | 0 |
| ALL | vertex | 1688 | 1441 | 247 | 0 | 0 |
| ALL | extern | 1095 | 27 | 1068 | 0 | 0 |
| ALL | unqualified | 111 | 0 | 75 | 0 | 36 |
| ALL | intersection | 15 | 9 | 0 | 0 | 6 |
| ALL | mesh | 2 | 0 | 0 | 0 | 2 |

Blockers by number of modules, SkyLight: `air.sample_texture_2d.v4f32` 18, `air.normalize_function_constant_predicate` 8, `air.sample_texture_1d_array` 4, `air.dfdx/dfdy` 2 each, `air.discard_fragment` 1, `air.sample_texture_1d` 1.
QuartzCore: `sample_texture_2d` (v4f16 50, v4f32 45), `get_read_sampler` 27, `fwidth` 17, `discard_fragment` 11, `read_texture_2d` 11+9+8+7, imageblock ops 16, function constants 7, `simd_shuffle_and_fill_down` 7, `fast_atan2` 5. Whole corpus (top): `sample_texture_2d` 8,600,
`write_texture_2d` 5,400, function constants 1,500+870, `get_read_sampler` 1,470, `get_width/height_texture_2d` 2,300, `fast_atan2` 1,070, `gather_texture_2d` 1,340, `fast_sincos`, `fast_atan`, `fast_fmod`.

**Compute kernels end to end** (`tools/air/kernels-all.py`: `air2amdgcn kernel` with a fixed 64-thread group + `llc`, every unique kernel): **2,281 of 10,720 unique kernels (21.3 %) reach a gfx1201 code object**, 8,438 stop in the lowering, 1 fails in `llc`. [host; fixed 64-thread groups, never run beyond the two kernels of section 6] Why the others stop (kernels): texture arguments 1,628 (`air.texture`), `air.sample_texture_2d` f16/f32 2,660, `get_height/array_size_texture` 894, `sample_texture_2d_array` 239, `get_read_sampler` 483, visible-function tables 300, function constants 509 (argument kind or `is_function_constant_defined`), `get_null_texture` 177, AIR address space 4 (imageblock) 162, indirect-buffer arguments 150, acceleration-structure arguments 132, simdgroup matrix multiply 72, `stage_in` 69, `gather` 83, `write_texture` 119; 60 modules do not verify after lowering (not investigated: the earlier class, lifetime markers on re-addressed allocas, is fixed) and 1 is an `llc` selection failure on a 512-bit store to a constant-space global. The 2,281 that do compile are the buffer-and-threadgroup-memory kernels: reductions, scans, histograms, sorts, clears, copies, small image-less ML/Photogrammetry/VFX helpers. The tool reports the first reason a kernel stops, so the reasons are exclusive: about 6,300 of the 8,438 stops are texture/sampler arguments or calls, i.e. texture support is what would move this number.

## 8. What is missing for vertex/fragment shaders, i.e. the path to the SkyLight composite shaders

Facts first: **7 of 7 SkyLight vertex shaders and 6 of 24 fragment shaders already lower and compile** as plain functions; their *bodies* are not the problem. What is missing is everything around the body.

1. **Texture and sampler operations (all 18 remaining SkyLight fragments, 95 of QuartzCore's 119).** AIR passes opaque handles and `air.sample_texture_2d.v4f32(tex, samp, coord, i1, offset, i1, f32, f32, i32)`. On gfx12 this is
   `llvm.amdgcn.image.sample.*` with a **<8 x i32> image descriptor** and a **<4 x i32> sampler descriptor**. Needed: (a) a runtime ABI for what a texture/sampler *handle* is (a pointer to a 32-byte gfx12 image
   descriptor and a 16-byte sampler descriptor in a heap the runtime fills at `setFragmentTexture`/argument-buffer encode time; the format enum differs from Apple's: `docs/metal-spike.md` s.3.1 "74 of 104 common formats differ"), (b) the meaning of the extra operands
   of `sample_*` (the `i1`/`float`/`i32` tail encodes bias/level/gradient/offset/min-lod; **UNKNOWN**, to be derived from the corpus variants, since 2-D, 1-D-array and 3-D forms are all present), (c) constant sampler states: decode the packed `[2 x i64]` constants
   (filter, address mode, anisotropy, compare...) into gfx12 sampler descriptors at pipeline creation, (d) `get_width/height/array_size`, `read_texture` (`image.load`), `write_texture` (`image.store`), `gather`, `get_null_texture`, `get_read_sampler`.
2. **Function constants** (`air.function_constants`, `normalize_function_constant_predicate`, 5,568 modules overall, 9 of the 31 SkyLight modules): specialisation values arrive with the pipeline; the compiler must substitute them before lowering (constant-fold the predicate, drop absent `air.function_constant` arguments). Nothing about this needs the card.
3. **Entry-point conventions** (the NGG recipe of `docs/g4-colour.md` s.2.1, `docs/metal-readiness.md` s.2): the lowering must generate, as IR, the part LLVM does not. **Measured on the host:** `llc -mcpu=gfx1201 -mtriple=amdgcn--amdpal` accepts an `amdgpu_gs` function (also `amdgpu_vs/hs/ps`) with one `llvm.amdgcn.exp`, emits `export pos0 ... done` and `s_endpgm`, and emits *nothing else* (checked on that one-export function): no `EXEC` set from `merged_wave_info`, no primitive export, no `s_sendmsg`/GS alloc request, no attribute-ring store.
   So for a vertex function the front end must (a) set `exec` from `merged_wave_info` (bits [7:0] vertex count: `s_bfe_u64 exec, -1, s2` after `s_pack_ll_b32_b16 s2, 0, s3`; **never AND**: EXEC starts at lane 0 here), (b) export position, then `s_wait_expcnt 0`, then the primitive,
   (c) store every `air.vertex_output` into the **attribute ring** with `buffer_store` using the ring descriptor from the ring table and `s5` = `gs_attr_offset` ((s5 & 0x7fff) << 9), `scope:SCOPE_DEV`, EXEC = (vertices + 7) & ~7, (d) wave32 `VGPR` granule 8 / 4 (`metal-readiness` s.2).
   For a fragment function it must read each `air.fragment_input` back through LDS (`ds_param_load` + `v_interp_p10/p2`, `llvm.amdgcn.lds.param.load`, `llvm.amdgcn.interp.inreg.p10/p2`) with the attribute index, centre/centroid/sample and perspective/no-perspective barycentrics (`air.center`, `air.perspective`, ...), produce
   `air.render_target` outputs as colour exports (an `amdgpu_ps` return value becomes `exp mrt0`, with the SPI_SHADER_COL_FORMAT state matching), and use `air.position` from the PS position inputs. All of it exists as hand-written ISA that ran on the card (G4) and as RADV's output; here it becomes IR the tool generates around the lowered body. [INFER: not attempted; effort below]
4. **Derivatives and quad ops**: `dfdx/dfdy/fwidth` (SkyLight 2 modules, QuartzCore 19) = DPP quad-permute differences in WQM (`llvm.amdgcn.update.dpp` on `llvm.amdgcn.wqm`'d values); `quad_shuffle_xor`, `simd_shuffle_and_fill_down`. No rule written.
5. **`air.discard_fragment`** -> `llvm.amdgcn.kill`, with the demote-to-helper semantics decision (derivatives after a discard).
6. **Imageblocks / framebuffer fetch (tile memory, QuartzCore 16 modules)**: no hardware equivalent; lowering = read the render target as a texture / input attachment with a barrier (`CB` -> `image.load` after a cache flush) or re-express as a separate pass. The tool refuses these (AIR address space 4).
7. **Not seen in the SkyLight/QuartzCore sets** (so deferred): mesh/object, tessellation (`air.patch*`, 675 modules in the corpus), ray tracing, simdgroup matrices (`air.simdgroup_matrix_8x8_multiply_accumulate`, 72 kernels; gfx12 WMMA is 16x16x16, the repo's `Matrix units` work covers the hardware side), visible-function tables/indirect command buffers.
8. **Half precision**: QuartzCore uses `half` in 104 modules; gfx12 has packed FP16 and the AMDGPU back end handles `half` arithmetic natively (18 of the 53 QuartzCore modules that compile use `half`); denormal behaviour of `fp16` vs Apple's `denorms_disable` not examined.
9. **PAL route tooling risk [host]:** compiling several functions of one module under `amdgcn--amdpal` crashed LLVM 22's AsmPrinter (`MCStreamer::finish`, msgpack DocNode insert) in most QuartzCore modules; not investigated. The HSA triple works for compute and callable functions but refuses graphics calling conventions (`unsupported non-compute shaders with HSA`), so a graphics pipeline needs either PAL metadata handled by us or our own object writer (we already own the shader-binary format: `src/*_kernel.h`).

Effort for "the two SkyLight composite shaders draw the right pixels through linux-replay" (M3's gate), [INFER]: sampler/texture ABI + `sample`/`load` lowering, function constants, the vertex NGG and fragment PS wrappers: **about 2-4 person-weeks** for SimpleVertex + SimpleTextureFragment-class shaders (3-instruction fragment, 15-instruction vertex),
plus 1-2 weeks for `UberComposite*` (function constants, 1-D-array LUT sampling, derivatives, `discard`); the whole SkyLight set then needs the blur and Lanczos fragments only for their texture loops. This does not include the Metal bundle, only the compiler.

## 9. Reproduce

    cd ~/work/rx4darwin/RDNA4FB-air
    ~/work/tools/macos-full/zpay ~/work/tools/macos-full/25G83/asset/*.zip metallib ~/work/tools/macos-full/25G83/metallib/tree     # step 1 (80 s)
    python3 tools/air/disall.py  ~/work/tools/macos-full/25G83/metallib/tree ~/work/tools/macos-full/25G83/metallib/air              # steps 2-3 (60 s, 4 GB of .bc/.ll)
    python3 tools/air/census.py  ~/work/tools/macos-full/25G83/metallib/air census.json && python3 tools/air/census.py x census.json --report > docs/m3-air-census.md   # step 4
    tools/air/run-spike.sh                           # host only: build tools, lower three kernels, show descriptors
    REPLAY_COMPUTE_OK=1 tools/air/run-spike.sh run   # on the card (after the reviewer's go)
    tools/air/coverage-all.sh ~/work/tools/macos-full/25G83/metallib/air -not -path '*AGXMetal*' > cov.jsonl && python3 tools/air/coverage-report.py ~/work/tools/macos-full/25G83/metallib/air/results.jsonl cov.jsonl
    python3 tools/air/kernels-all.py ~/work/tools/macos-full/25G83/metallib/air/results.jsonl

Files: `tools/air/{metallib.py, disall.py, census.py, air2amdgcn.cpp, build-tools.sh, build-kernel.sh, air-run.cpp, test-kernels.py, run-spike.sh, coverage-all.sh, coverage-report.py, kernels-all.py, samples/grid3d.ll}`, `docs/m3-air-census.md`.
