# M1 prep: the command stream of Apple's paravirtual GPU (hub-task-440)

Static map of what the guest writes for the host, from Apple's own binaries: `AppleParavirtGPU.kext` (kernel driver; the full 26.6.2 install's `SystemKernelExtensions.kc`) and `AppleParavirtGPUMetal.bundle` (the x86_64 slice of the user-space Metal device; `PGSerializer*` encoders). Branch `premetal/m0`.
Status words: **measured** = read from the binaries (instruction level) or from a run; **INFER** = reasoned, not shown; **code** = written and compiled/tested on the host, not yet run against Apple's driver.
Clean-room: everything below was derived from Apple's binaries with the tools in `tools/m0` and `tools/m1`; the only other source consulted was the register map already in `docs/metal-spike.md` s.6.1 (which cites reims-vgpu's public documentation, LGPL, as the pointer to look at the control block). No reims code, table or structure was used. No Apple bytes are in git: tables contain ids, sizes and names we derived.

## 0. The answer in one page

There are **three command layers**, not one:

| layer | written by | carried in | framing | what it is |
|---|---|---|---|---|
| **FIFO** | the kernel driver (`AppleParavirtCommandAllocator`) | the root FIFO (guest RAM page, announced through the control block) and the child rings ("Exec", "Immediate", "Uploads", "Downloads") | `{u16 id, u16 nBarriers, u32 length, u32 signal, barriers{u32,u32}[], payload}` | tasks, page tables, channels, resource backing, exec submission, device info, display: **what the polling host sees first** |
| **operations** | the Metal bundle's `PGSerializer` (object creation/deletion) | a 4 KiB buffer handed to the kernel with `IOConnectCallMethod(selector 0x100)` | `{u32 id, u32 size}` + payload | create/delete textures, samplers, functions, fences, ICBs...: the object table of the device |
| **stream** | the Metal bundle's encoders (`PGSerializer{Render,Compute,Blit,Info}CommandEncoder`) | command-buffer chunks in guest *resources* (pooled buffers), listed to the kernel as `ExecIndirect` chunks `{resource id, length}` | segments `{u32 length, u8 type, u8 continuation, u8 flags, u8 0}`, inside them `{u32 id, u32 size}` + payload | draws, dispatches, blits, state, bindings: **the Metal work** |

The host sees FIFO commands directly in guest RAM; to see operations and the stream it must follow the kernel's `ExecIndirect`/object commands to **guest resources** and read them through the guest-built GPU page tables (`DefineHostTask` + `CommitIntoGPUPageTable`), which is the core of M1. In this commit the polling host decodes the FIFO layer (log-and-consume), `src/pvstream` carries the tables and walkers for all three layers, and `docs/m1-opcodes.md` and the tables in `src/pvopcodes.inc` hold **172 stream commands (148 with a constant-length writer, 24 more reached through helpers), 31 operations and 24 FIFO commands**.

## 1. The FIFO layer (kernel driver -> host) [measured, `tools/m0/pvdis.py`]

**Framing.** `AppleParavirtCommandAllocator` builds each command in a 0x400-byte buffer (more via `IOMallocData` for big ones): `init(id, size)` writes `{u16 id at +0, u16 nBarriers at +2 (0), u32 length at +4 (= 0xc), u32 signal at +8 (0)}`; `addBarrier(a, b)` appends 8 bytes and bumps `nBarriers`; `addSignal(_, v)` writes `v` at +8; `getCommandBytes(n)` appends `n` payload bytes and grows `length`; `submitOnChannel(ch, desc)` hands `{pointer, length/4}` to the channel. The first record of a payload begins at `12 + 8 * nBarriers` (`getPayloadOffset`). A command is therefore **self-sized**: `length` (bytes, multiple of 4) covers header, barriers and payload.

**Where it goes.** `AppleParavirtRootChannel::submitBuffer` is `writeFifo(length, data)`: the root FIFO is the plain concatenation of commands (the ring of 60 KiB behind the 4 KiB root header, `position = counter % ringLen`, see `docs/m0-pvgpu.md`). A **virtual channel** (`AppleParavirtVirtualChannel`: Exec = 1, Immediate = 2, Uploads = 3, Downloads = 4, plus display pipes) has its own ring and is defined with FIFO command `DefineChannel` (below):
- the channel's ring is an `IOBufferMemoryDescriptor` of `ringBytes + 0x1000` (options 0x802, ring size a multiple of 4 KiB; default 0x10000): the **first page is a page list** (one u32 physical page number per 4 KiB of ring), the ring data starts at +0x1000;
- its **state record lives in the root page** at `+0x400 + (channelId - 1) * 20`: `{u32 write, u32 read, u32 0, u16 kind, u16 0, u32 pageListPFN}` (the driver zeroes `write`/`read` and fills `kind` from its channel number and `pageListPFN` from the descriptor's first page) [measured; field names INFER];
- `submitBuffer` copies the command into the ring at `write % ringBytes` (split at the wrap), advances `write`, and **only if the ring was empty** (`read == old write`) rings the doorbell by storing the channel id at `ctrl+0x20` (`CHILD_DOORBELL`). The host consumes by advancing `read` (a spin on a full ring otherwise, as for the root FIFO). A RAM-backed BAR0 cannot trap the doorbell: **the host polls the rings**.

**Command ids** (names are the calling function's, not Apple's enumerators; found at the call sites of `AppleParavirtCommandAllocator::init`):

| id | name | caller | payload read in the driver |
|---|---|---|---|
| 0x01 | DisplaySetupSharedState | `AppleParavirtDisplayPipe::setupSharedState` | |
| 0x02 | DisplayProcessOnline | `DisplayPipe::process_online` | |
| 0x04 / 0x05 | DisplayUpdateCursorGlyph / State | `updateCursorGlyph` / `updateCursorState` | |
| 0x06 / 0x07 | DisplaySubmitTransaction(2) | `DisplayPipe::submitTransaction` | |
| 0x1e | DisplayFlushChannelEvent | `flushChannelEvent` | |
| 0x20 | FreeTask | `AppleParavirtTask::free` | |
| 0x22 | ReleaseFromGPUPageTable | `AppleParavirtMemoryMap::releaseFromGPUPageTable` | |
| 0x25 | DeleteHostResourceID | `AppleParavirtResource::deleteHostResourceID` | |
| 0x28 | DeleteObject | `AppleParavirtShared::deleteObject` | |
| **0x30** | **DefineChannel** | `VirtualChannel::init` | `{u32 channel id}` |
| 0x31 | FreeChannel | `VirtualChannel::free` | |
| 0x33 | SetResourceHeap | `AppleParavirtTask::setResourceHeap` | |
| 0x34 | PageBacking | `AppleParavirtResource::pageBacking` | |
| 0x35 | SynchronizeForUnwire | `Resource::synchronizeForUnwire` | |
| 0x36 | DeleteHostIOSurfaceBacking | `Resource::deleteHostIOSurfaceBacking` | |
| **0x37** | **ExecIndirect** | `AppleParavirtCommandQueue::processExecIndirect` | chunks `{u32 resource id, u32 length}` (copied from the kernel command, below) |
| **0x38** | **DefineHostTask** | `AppleParavirtTask::defineHostTask` | `{u32 task << 1, u64 address-space size, u32 page-table root PFN}` (16 bytes) |
| **0x39** | **CommitIntoGPUPageTable** | `AppleParavirtMemoryMap::commitIntoGPUPageTable` | `{u32 task, u64 gpu address, u64 length}` (20 bytes) [field meanings INFER from the sources: task id, resource GPU address and length] |
| **0x3a** | **GetDeviceInfo** | `AppleParavirtAccelerator::setupDeviceInfo` | record `0x2d`: `{u32 0x2d, u32 bytes/8, u32 page}` (12 bytes): *the 4 KiB buffer the host fills with the device-info blob*, then the command's signal/barrier completes it |
| 0x3b | CreateComputePipeline | `AppleParavirtShared::createComputePipeline` | |
| 0x3c | ReplacePhysical | `AppleParavirtResource::replacePhysical` | |
| 0x41 | DeleteHostSharedTextureBacking | `Resource::deleteHostSharedTextureBacking` | |

The first of these a driver sends is `GetDeviceInfo` (`setupDeviceInfo` is the last step of `Accelerator::start`); it **waits for the host's reply** (the blob that `parseDeviceInfo` parses into `APVDeviceInfoStruct`: MSAA samples, max threadgroup sizes, feature flags, deserializer version, FP configs, ... the ivars of `AppleParavirtDevice._deviceInfo`, `docs` class dump), so it is the first host answer M1 must produce. `defineHostTask`'s address-space size and root page are what lets the host walk the guest's GPU page table (`AppleParavirtPageTable` interior/leaf nodes); `ExecIndirect` is the bridge to the stream layer.
Per-command log lines for all of these come from `pvstream::describeFifo` (`DefineChannel`, `DefineHostTask`, `CommitIntoGPUPageTable`, `GetDeviceInfo`, `ExecIndirect` are decoded, the rest by name and size).

## 2. The kernel commands between bundle and kernel [measured]

**`IOAccelKernelCommand`**: `{u32 type, u32 size, ...}`. The bundle puts one `ExecIndirect` kernel command (type **0x10000**) in the command buffer's kernel-command stream: `{u32 0x10000, u32 size, u32 chunkCount, {u32 resource id, u32 length}[chunkCount]}` (`insertCommandExec`: created with size 12 and count 0, each `endCurrentChunk`/new chunk grows size by 8 and count by 1; the resource id is `-[AppleParavirtPooledBuffer resourceID]`, the length is the chunk's final `currentOffset`). The kernel (`processExecIndirect`) checks each chunk against its resource (length, wired) and emits FIFO command 0x37.

**Shared user client**, selectors 0x100..0x106 (`AppleParavirtSharedUserClient`, method table at `AppleParavirtSharedMethods`, selector - 0x100 indexes it):

| selector | method | struct in | out |
|---|---|---|---|
| 0x100 | createObject | variable (the **operation buffer**) | scalar 1 |
| 0x101 | deleteObject | variable | |
| 0x102 | createFunction | variable | scalar 1 |
| 0x103 | addChildResource | variable | |
| 0x104 | removeChildResource | 8 bytes | |
| 0x105 | getDeviceInfo | | struct 284 bytes |
| 0x106 | createComputePipeline | variable | scalar 1 |

`createObject` reads the first dword of the operation buffer as the **object type** and creates the kernel-side object through a table lookup (`createObjectLocked`); how the host learns of the object (its own FIFO command or the bytes forwarded on the Immediate channel) is **not read yet** [unknown].

## 3. The operation layer [measured ids and sizes, field lists partial]

`PGSerializer` creates and deletes device objects by writing into an `AppleParavirtAllocator` (4 KiB, `allocateOperationBytes:`); the caller writes the 8-byte header itself: `{u32 id, u32 total size}`. Object **refs** are u32 indexes handed out by `AppleParavirtObjectRefAllocator` (`newBufferRef`, `newTextureRef`, ..., `releaseXRef` returns the index; `deleteXRef:allocator:` emits the deletion op). Ids (full table with fields in `docs/m1-opcodes.md`):

| id | operation | payload |
|---|---|---|
| 0x001 / 0x034 | newTextureWithDescriptor | `{u32 textureRef, ...serialised descriptor}` (44 / 52 bytes total) |
| 0x003 | newSamplerState | `{u32 ref, ...}` |
| 0x004 | newDepthStencilState | `{u32 ref, u32 ...}` |
| 0x007 / 0x008 / 0x01b | newTextureView (pixel format / type / levels / slices / swizzle) | `{u32 newRef, u32 baseTextureRef, u16 format, u16 type, u64 levels, u64 slices, ...}` |
| 0x009 / 0x037 | newTextureWithBuffer | `{u32 ref, u32 bufferRef, u64 offset, u64 bytesPerRow ...}` |
| 0x00d | newFence | `{u32 fenceRef}` |
| **0x00f** | **newFunctionWithIR** | `{u32 functionRef, ...}` (20 bytes total) |
| 0x015 / 0x038 | newTextureWithDescriptor (heap) | `{u32 ref, u32 heapRef, ... u64 offset}` |
| 0x02f / 0x039 | newIOSurfaceTexture (without / with a second plane field) | `{u32 ref, ... u16 plane, u16 rotation}` |
| 0x030 / 0x035 / 0x031 | newSharedTexture (descriptor / handle) | |
| 0x036 | newIndirectCommandBuffer | `{u32 commandTypes, u8 maxVertexBuffer, maxFragmentBuffer, maxKernelBuffer, ...}` (88 bytes total) |
| 0x3e8..0x3f7 | delete{Buffer,Texture,DepthStencilState,SamplerState,Function,ComputePipelineState,RenderPipelineState,Fence,Heap,RasterizationRateMap,IndirectCommandBuffer}Ref | `{u32 ref}` (12 bytes total) |

Not found as simple immediates (variable, built by helpers): render/compute **pipeline state** creation (`newRenderPipelineStateWithSerializedDescriptor:allocator:` and the compute one) and rasterization-rate-map creation.

## 4. The stream layer (the Metal work) [measured ids/lengths, partial layouts]

**Chunks and segments.** `AppleParavirtCommandBuffer` writes into *storage* resources (`_commandCurrentStorage`: `AppleParavirtPooledBuffer`, its `resourceID`, pointer, size, current offset); when one is full it is closed (`endCurrentChunk`: the chunk length goes to the **first dword of the chunk buffer** and to the exec chunk entry) and a new one opened. Inside a chunk the stream is a list of **segments**, one per encoder: an 8-byte header written by `-[PGSerializerCommandEncoder writeSegmentHeader:continuation:protectionOptions:]`: `{u32 length (0, patched at the end of the segment by Metal's generic `endCurrentSegment`, not read here [INFER]), u8 type, u8 continuation, u8 flags, u8 0}`. **Segment types** (the argument of each encoder's `beginSegment:`): **render 0, compute 1, blit 2, info 4**; with protection options enabled an extra envelope segment (type 5, followed by the raw u64 options) precedes it. `beginContinuation` sets the continuation byte of the previous header (a segment split by the buffer size: `split` closes the chunk and `beginSegment:` re-opens with the continuation flag).

**Command framing inside a segment.** `-[PGSerializerCommandEncoder getCommandBytes:forCommand:]` is the single writer: it asks the stream for `align4(payload + 8)` bytes, stores `{u32 id (command), u32 size (bytes including this header)}` and returns a pointer to the payload (which the caller fills; a NULL means the chunk is full: the encoder retries after a split).

**Commands.** `docs/m1-opcodes.md` (generated by `tools/m1/gen-opcodes.py --md` from the writers' call sites) has every id, payload length and field list. Ranges:

| group | ids | examples (payload bytes) |
|---|---|---|
| Render | 0x000-0x0a6 | draws 0x00-0x13 (each in a **narrow** form with 16-bit counts and a **wide** form: `drawPrimitives` is 0x001 `{u32 type, u16 start, u16 count}` (8) or 0x000 `{u32 type, u64 start, u64 count}` (20), likewise 0x002/3, 0x004/5, 0x006/7, ...), indirect draws 0x10-0x13, ICB execution 0x14/0x15, barriers/fences 0x16-0x19, `writeDescriptor` (render pass, 584 bytes, 0x1a), state 0x65-0x8f (viewport 0x82 (48), scissor 0x75 (32), blend colour 0x65 (16), cull 0x6b, winding 0x73, depth-stencil state ref 0x68 (4), render pipeline state ref 0x74 (4)...), tile/threadgroup 0x9b-0xa6; bindings 0x6e-0x72 (fragment), 0x7d-0x81 (vertex), 0x9d-0xa1 (tile), 0xa5 |
| Compute | 0x0c8-0x0e6 | `dispatchThreadgroups` 0xc8 (48: two `MTLSize`), `dispatchThreads` 0xca (48), indirect 0xc9/0xe6, `setComputePipelineState` 0xd0 `{u32 pipelineRef}` (4), `setThreadgroupMemoryLength` 0xd3, fences 0xd4/0xd5, barriers 0xd6/0xd7, GPU-side control flow (`encodeStartIf/Else/EndIf/While/DoWhile`) 0xdc-0xe2, ICB 0xe4/0xe5; bindings 0xcb (buffers), 0xcc/0xcd (samplers), 0xce (textures), 0xd9 |
| Blit | 0x12c-0x143 | `copyFromBuffer:toBuffer` 0x12d (32), buffer<->texture 0x12c/0x12e (88), texture<->texture 0x12f/0x130, `fillBuffer` 0x132 (24: `{u32 bufferRef, u64 offset, u64 length, u8 value}`), mipmaps 0x133, synchronize 0x13a/0x13b, fences 0x13c/0x13d, optimize 0x134-0x139, fillTexture 0x140/0x141 |
| Info | 0x1c2-0x1d5 | queries answered by the host (pipeline state info, heap texture size/align, host resource info, rasterization rate): **the host writes a reply** |

**Bindings** are variable-length. `setBuffers` (render 0x6e/0x7d/0x9d, compute 0xcb): payload `{u32 firstIndex, u32 count, {u32 bufferRef, u64 offset}[count]}` (8 + 12 × count; `offset` = the buffer's `parentResourceOffset` + the bound offset); `setTextures` (0x72/0x81/0xa1/0xce) and `setSamplerStates` (0x70/0x7f/0x9f/0xcc): `{u32 firstIndex, u32 count, u32 ref[count]}`. `setBytes` has **no inline data command**: the encoder copies the bytes into the command buffer's pooled *buffer storage* (`getBufferBytes:alignment:buffer:offset:`) and binds that as a buffer: constants travel as resources. Every resource a command names is also passed to `addResourceReference:isWrite:` (the kernel's `IOAccelResourceList`): that is how the kernel knows what to wire and what the host must map.

**An example, one compute dispatch** (INFER on the order, measured ids): segment header (type 1) / `0xd0 setComputePipelineState {pipelineRef}` / `0xcb setBuffers {0, n, [bufferRef, offset]...}` / `0xc8 dispatchThreadgroups {groups w,h,d; threads-per-group w,h,d}` / next segment or end; the chunk lands in a pooled resource, `ExecIndirect` (FIFO 0x37) names it.

## 5. How resources and shaders are referenced [measured unless marked]

- **Objects are named by u32 refs** from the serialiser's index allocator (buffer, texture, sampler, depth-stencil, function, compute/render pipeline, fence, heap, ICB, rasterization-rate map): every command carries refs, never pointers. A buffer's ref is created in `AppleParavirtBuffer initWithDevice:...` (`[serializer newBufferRef]`) and passed in the arguments of the kernel resource-creation call, so the kernel resource and the ref are tied together there (the host learns `ref <-> host resource id` from the resource's creation, `_hostResourceID`; the exact FIFO/object command is not read yet [unknown]). Sub-allocated buffers add `parentResourceOffset`.
- **Memory is guest RAM described by page tables**: the host never gets data in the stream; it reads resources through the guest GPU page table (`DefineHostTask` root + `CommitIntoGPUPageTable` ranges) and the resource list of each exec.
- **Shaders**: Metal asks `MTLCompiler` (plug-in path chosen by the bundle: `libAppleParavirtCompilerPlugin.dylib`, in the OS cryptex we could not obtain) for the "compiler output". `-[AppleParavirtFunctionVariant initWithCompilerOutput:device:functionType:]` **copies that blob** (`dispatch_data`) into a new *descriptor buffer* resource (`AppleParavirtDescriptorBuffer`, `newDescriptorBuffer:size`), then builds a 24-byte record `{u32 functionRef, u32 descriptor-buffer resource id, u64 resource address, u64 size}` and sends operation 0x0f (`newFunctionWithIR`). Pipeline states reference function refs; their descriptors are serialised into descriptor buffers likewise (`AppleParavirtRenderPipelineState` holds two). **The shader that reaches the host is a blob in guest memory named by a resource id; what the blob is (AIR bitcode wrapped by the plug-in?) is [INFER: AIR, as reims-vgpu's `metal2vulkan` host translates it; to be verified by capturing one with the stub bundle of `docs/metal-spike.md` s.12 or from a VM run].**
- **Textures**: `serializeTextureDescriptor:`/`...2:` write the descriptor into the creation op; texture data moves through blit commands (`copyFromBuffer:...toTexture`) and the Uploads/Downloads channels [INFER].

## 6. What is implemented now (code)

- `src/pvstream.{hpp,cpp}` + generated `src/pvopcodes.inc`: names/lengths of the three layers (binary-searched), `fifoHeader`/`describeFifo` (FIFO framing validation and per-command description), `streamHeader`/`walkCommands` (stream and operation framing), `segmentTypeName`. Host test `tools/pvstream-test.cpp` (in `make test`): framing across a ring wrap, barriers, malformed headers, unknown ids, truncation, table spot checks against the values read above.
- `src/pvgpu.cpp` polling host: every new FIFO span is walked as commands and logged as `host: fifo @0x...: cmd 0x3a GetDeviceInfo, 24 bytes (12 payload), 0 barrier(s), signal 0x7: reply buffer page 0x1234, 4096 bytes`; a span that does not start with a command header is hex-dumped (12 times) and abandoned; everything is acknowledged (`FIFO_READ = FIFO_WRITTEN`). The self-test sends two valid commands, one across the ring wrap. Still log-and-consume, default off.
- Child channels (`DefineChannel` -> record in the root page -> page-list ring, polled every tick, commands decoded and answered, per-channel stamp and interrupt bit): implemented in the M0 host and self-tested (`docs/m0-pvgpu.md`, hub-task-447). The page list has no length field: the host takes the entries up to the first zero (the driver zeroes the page first).
- Replies implemented: `GetDeviceInfo` (0x3a) and `DisplaySetupSharedState` (0x01). Not implemented: resource/page-table walking, stream decoding of real guest memory, every other reply.

## 7. Unknowns and the next measurements

1. **Segment length semantics and chunk layout details**: Metal's generic `endCurrentSegment` (`MTLIOAccelCommandBuffer`, in `Metal.framework` of the dyld cache on disk) patches the header; read it.
2. **How an object/resource becomes known to the host**: the FIFO/Immediate-channel commands behind `createObject`, `addChildResource`, resource creation (`AppleParavirtResource`, the `args` of `initWithDevice:pointer:length:...`). Candidates among FIFO ids 0x33-0x3c.
3. **`parseDeviceInfo` layout** (999 bytes of parsing code): the blob M1 must write for `GetDeviceInfo`.
4. **Pipeline-state and rasterization-rate-map creation records** (variable length).
5. **Compiler output format** (needs the cryptex plug-in, E2b dropped; or capture).
6. **The 600/64/24/16-byte replies of the generic `IOAccelDevice` init** (`docs/metal-spike.md` s.12): independent of the paravirt stream but needed by every route.
7. Oracle: run Apple's own `PGSerializer` on a real Mac (or in the full-install VM once its host answers `GetDeviceInfo`) and diff against these tables; capture a compute app's chunk to confirm the example of s.4.

## 8. Reproducing

Needs `~/work/tools/macos-full` (Apple's collections and the bundle, outside the repo; `lipo -thin x86_64` of the bundle's binary to `m1/apv-x86_64`).
- `tools/m1/apvdis.py classes|methods|dis|strings|addr` (the bundle: ObjC classes with ivars, method addresses/sizes, annotated disassembly with selector/CFString/cstring resolution; `llvm-objdump` for the instruction listing),
- `tools/m1/apvcmds.py table|layout|json` (the command scanner), `tools/m1/gen-opcodes.py [--md]` (generates `src/pvopcodes.inc` and `docs/m1-opcodes.md`),
- `tools/m0/pvdis.py list|dis|xref|vt` (the kernel driver in the collection, imports resolved through the chained fixups).
