# M1 prep: the command stream of Apple's paravirtual GPU (hub-task-440)

Static map of what the guest writes for the host, from Apple's own binaries: `AppleParavirtGPU.kext` (kernel driver; the full 26.6.2 install's `SystemKernelExtensions.kc`) and `AppleParavirtGPUMetal.bundle` (the x86_64 slice of the user-space Metal device; `PGSerializer*` encoders). Branch `premetal/m0`.
Status words: **measured** = read from the binaries (instruction level) or from a run; **INFER** = reasoned, not shown; **code** = written and compiled/tested on the host, not yet run against Apple's driver.
Clean-room: everything below was derived from Apple's binaries with the tools in `tools/metal/m0` and `tools/metal/m1`; the only other source consulted was the register map already in `docs/metal-spike.md` s.6.1 (which cites reims-vgpu's public documentation, LGPL, as the pointer to look at the control block). No reims code, table or structure was used. No Apple bytes are in git: tables contain ids, sizes and names we derived.

## 0. The answer in one page

There are **three command layers**, not one:

| layer | written by | carried in | framing | what it is |
|---|---|---|---|---|
| **FIFO** | the kernel driver (`AppleParavirtCommandAllocator`) | the root FIFO (guest RAM page, announced through the control block) and the child rings ("Exec", "Immediate", "Uploads", "Downloads") | `{u16 id, u16 nBarriers, u32 length, u32 signal, barriers{u32,u32}[], payload}` | tasks, page tables, channels, resource backing, exec submission, device info, display: **what the polling host sees first** |
| **operations** | the Metal bundle's `PGSerializer` (object creation/deletion) | a 4 KiB buffer handed to the kernel with `IOConnectCallMethod(selector 0x100)` | `{u32 id, u32 size}` + payload | create/delete textures, samplers, functions, fences, ICBs...: the object table of the device |
| **stream** | the Metal bundle's encoders (`PGSerializer{Render,Compute,Blit,Info}CommandEncoder`) | command-buffer chunks in guest *resources* (pooled buffers), listed to the kernel as `ExecIndirect` chunks `{resource id, length}` | segments `{u32 length, u8 type, u8 continuation, u8 flags, u8 0}`, inside them `{u32 id, u32 size}` + payload | draws, dispatches, blits, state, bindings: **the Metal work** |

The host sees FIFO commands directly in guest RAM; to see operations and the stream it must follow the kernel's `ExecIndirect`/object commands to **guest resources** and read them through the guest-built GPU page tables (`DefineHostTask` + `CommitIntoGPUPageTable`), which is the core of M1. In this commit the polling host decodes the FIFO layer (log-and-consume), `src/pvstream` carries the tables and walkers for all three layers, and `docs/m1-opcodes.md` and the tables in `src/pvopcodes.inc` hold **172 stream commands (148 with a constant-length writer, 24 more reached through helpers), 31 operations and 24 FIFO commands**.

## 1. The FIFO layer (kernel driver -> host) [measured, `tools/metal/m0/pvdis.py`]

**Framing.** `AppleParavirtCommandAllocator` builds each command in a 0x400-byte buffer (more via `IOMallocData` for big ones): `init(id, size)` writes `{u16 id at +0, u16 nBarriers at +2 (0), u32 length at +4 (= 0xc), u32 signal at +8 (0)}`; `addBarrier(a, b)` appends 8 bytes and bumps `nBarriers`; `addSignal(_, v)` writes `v` at +8; `getCommandBytes(n)` appends `n` payload bytes and grows `length`; `submitOnChannel(ch, desc)` hands `{pointer, length/4}` to the channel. The first record of a payload begins at `12 + 8 * nBarriers` (`getPayloadOffset`). A command is therefore **self-sized**: `length` (bytes, multiple of 4) covers header, barriers and payload.

**Where it goes.** `AppleParavirtRootChannel::submitBuffer` is `writeFifo(length, data)`: the root FIFO is the plain concatenation of commands (the ring of 60 KiB behind the 4 KiB root header, `position = counter % ringLen`, see `docs/m0-pvgpu.md`). A **virtual channel** (`AppleParavirtVirtualChannel`: Exec = 1, Immediate = 2, Uploads = 3, Downloads = 4, plus display pipes) has its own ring and is defined with FIFO command `DefineChannel` (below):
- the channel's ring is an `IOBufferMemoryDescriptor` of `ringBytes + 0x1000` (options 0x802, ring size a multiple of 4 KiB; default 0x10000): the **first page is a page list** (one u32 physical page number per 4 KiB of ring), the ring data starts at +0x1000;
- its **state record lives in the root page** at `+0x400 + (channelId - 1) * 20`: `{u32 write, u32 read, u32 0, u16 kind, u16 0, u32 pageListPFN}` (the driver zeroes `write`/`read` and fills `kind` from its channel number and `pageListPFN` from the descriptor's first page) [measured; field names INFER];
- `submitBuffer` copies the command into the ring at `write % ringBytes` (split at the wrap), advances `write`, and **only if the ring was empty** (`read == old write`) rings the doorbell by storing the channel id at `ctrl+0x20` (`CHILD_DOORBELL`). The host consumes by advancing `read` (a spin on a full ring otherwise, as for the root FIFO). A RAM-backed BAR0 cannot trap the doorbell: **the host polls the rings**.

**Command ids** (names are the calling function's, not Apple's enumerators; found at the call sites of `AppleParavirtCommandAllocator::init`):

| id | name | caller | payload read in the driver |
|---|---|---|---|
| 0x01 | DisplaySetupSharedState | `AppleParavirtDisplayPipe::setupSharedState` | `{u32 port, u32 shared-state page}` (8 bytes); the driver **waits** for the host (s.1b) |
| 0x02 | DisplayProcessOnline | `DisplayPipe::process_online` | `{u32 port, u32 shared+0x200}` (8): the guest's acknowledgement of an online event, no wait |
| 0x04 | DisplayUpdateCursorGlyph | `updateCursorGlyph` | 44 bytes: `{u32 port, u32 task, u64 mapping, u64 ?, u64 pitch, u16 x4 (processCursorImage results), u32 sum}` (s.1b) |
| 0x05 | DisplayUpdateCursorState | `updateCursorState` | `{u32 port, u8 visible}` (8 with padding); **only sent when the cursor is not in the shared state** (s.1b) |
| 0x06 / 0x07 | DisplaySubmitTransaction(2) | `DisplayPipe::submitTransaction` | 0x06: `{u32 port, u32 surface id, u32 task}` (12); 0x07: 36 bytes with the gamma table (s.1b); which one is chosen by the host (shared+0x1c) |
| 0x1e | DisplayFlushChannelEvent | `flushChannelEvent` | **no payload**; sent only on the error paths of `submitTransaction` |
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
| **0x37** | **ExecIndirect** | `AppleParavirtCommandQueue::processExecIndirect` | `{u32 task, u32 n, n resource entries, u64 command-buffer gpu address, u64 length}` [measured on the rig, s.5b]; the resource entries are 6-7 dwords, the first the resource id |
| **0x38** | **DefineHostTask** | `AppleParavirtTask::defineHostTask` | `{u32 task << 1, u64 address-space size, u32 page-table root PFN}` (16 bytes) |
| **0x39** | **CommitIntoGPUPageTable** | `AppleParavirtMemoryMap::commitIntoGPUPageTable` | `{u32 task, u64 gpu address, u64 length}` (20 bytes) [measured on the rig, s.5b] |
| **0x3a** | **GetDeviceInfo** | `AppleParavirtAccelerator::setupDeviceInfo` | record `0x2d`: `{u32 0x2d, u32 bytes/8, u32 page}` (12 bytes): *the 4 KiB buffer the host fills with the device-info blob*, then the command's signal/barrier completes it |
| 0x3b | CreateComputePipeline | `AppleParavirtShared::createComputePipeline` | |
| 0x3c | ReplacePhysical | `AppleParavirtResource::replacePhysical` | |
| 0x41 | DeleteHostSharedTextureBacking | `Resource::deleteHostSharedTextureBacking` | |

The first of these a driver sends is `GetDeviceInfo` (`setupDeviceInfo` is the last step of `Accelerator::start`); it **waits for the host's reply** (the blob that `parseDeviceInfo` parses into `APVDeviceInfoStruct`: MSAA samples, max threadgroup sizes, feature flags, deserializer version, FP configs, ... the ivars of `AppleParavirtDevice._deviceInfo`, `docs` class dump), so it is the first host answer M1 must produce. `defineHostTask`'s address-space size and root page are what lets the host walk the guest's GPU page table (`AppleParavirtPageTable` interior/leaf nodes); `ExecIndirect` is the bridge to the stream layer.
Per-command log lines for all of these come from `pvstream::describeFifo` (`DefineChannel`, `DefineHostTask`, `CommitIntoGPUPageTable`, `GetDeviceInfo`, `ExecIndirect` are decoded, the rest by name and size).

## 1b. The display pipe: shared state, events, transactions, cursor [measured unless marked; hub-task-451]

Read with `tools/metal/m0/pvdis.py dis AppleParavirtDisplayPipe::...` (26.6.2). Classes: `AppleParavirtDisplayMachine` (one per accelerator; `signalDisplays`), `AppleParavirtDisplayPipe` (one per display, **port < 8**, name `Display<port>`; `init` refuses port >= 8), `AppleParavirtFramebuffer` (the IOFramebuffer; `connectionChange`). The number of displays is `ctrl+0x22c` (`kRegNumDisplays`).

**The pipe's channel.** `DisplayPipe::init` creates its own `AppleParavirtVirtualChannel`: `init(accel, port + 5, 8, 0x90, 0x480, "Display<port>", ring 0x1000, port + 5)`, i.e. **channel number `port + 5`** (5..12) with a **one-page ring**; the root channel is `ROOT` with `(0, 0x80, 0x90, 0x4800)`. The record's `kind` (the stamp index) is the channel object's u16 at +0x20, which the base class sets from the first int [INFER: = the channel id, 0 for ROOT, 1..4 for the standard four, port + 5 for a display; the host takes it from the record anyway]. Everything the pipe sends (0x01 setup, 0x02, 0x04-0x07, 0x1e) goes **on that channel**, not on the root FIFO.

**The shared-state page** (`setupSharedState`: a 4 KiB `IOBufferMemoryDescriptor`, options 0x800, zeroed; sends 0x01 `{port, page}` and **waits**; then asserts `u16 shared+0x12 == port` and reads `u32 shared+0x1c` into `pipe+0x384`). The guest writes the cursor and enable fields, the host everything else:

| offset | size | who | meaning |
|---|---|---|---|
| +0x00 | u32 | host | first argument of `connectionChange` (also of the offline path) [INFER: a display id / EDID product code] |
| +0x04 | char[14] | host | display name, NUL-terminated: passed as the `const char*` of `connectionChange`, ends up in the synthesised EDID (`edidInit`) |
| +0x12 | u16 | host | **the port** (a mismatch panics the guest: `fSharedState->port == fPort`) |
| +0x14 / +0x16 | u16 / u16 | host | width / height of the display |
| +0x18 / +0x1a | u16 / u16 | host | cursor glyph width / height; `w * h * 4 == 0` makes `updateCursorGlyph` fail with 0xe00002bc |
| +0x1c | u32 | host | copied to `pipe+0x384` after the wait: **0 = transactions use FIFO 0x06, non-zero = 0x07** (the one with the gamma table) |
| +0x20 | u32 | host | read by `process_online` into `pipe+0x3ac` (low byte used); bit 0 = tell the host when the cursor's visibility changes, bit 1 = when its position changes (`ctrl+0x220` kick, below) |
| +0x2c .. +0x48 | f32 x 8 | guest default, host may override | colour chromaticities passed to `edidInit` (`edidColorCorrection_t`); `setupSharedState` pre-fills 0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.329 (Rec.709 primaries, D65) |
| +0x4c / +0x4e | u16 / u16 | host (guest default 0xffff) | `originX` / `originY` of the display in the global space (0xffff = not set); go into the registry property `ParavirtDisplayPrefs` |
| +0x50 | u8 | host | `scaleFactor` (same property) |
| **+0x100** | u32 | host sets, guest clears | **pending event bits**: bit 0 VBL, bit 2 *online*, bit 3 *offline* |
| **+0x104** | u32 | guest | **enabled mask**: `enable()` stores 0xc, `disable()` stores 0, `enableVBLInterrupt` ORs 1, `disableVBLInterrupt` clears bit 0 |
| +0x200 | u32 | host | echoed in the 0x02 acknowledgement `{port, +0x200}` [INFER: a cookie/serial of the online event] |
| +0x208 | u16 | host | number of display modes |
| +0x210 + 16 i | 16 bytes | host | mode i: `{u16 width, u16 height, u32 x, ...}` (the callback block reads +0x00, +0x02, +0x04; `x` is the refresh rate of the mode in **16.16 fixed-point Hz** [INFER: it is the `refresh` argument of `edidGenerateModeTiming`, which divides the product by 65536; 60 Hz = 0x3c0000]) |
| +0xe00 | u32 | guest | cursor position `x | y << 16` (hotspot added), initial 0xffffffff |
| +0xe04 | u8 | guest | cursor visible |

**Events and `INTR_STATUS_DISP` (`ctrl+0x14`).** The interrupt handler (`AppleParavirtAccelerator::setupInterrupts` block) reads `ctrl+0x18` (stamps, `signalStamps`), `ctrl+0x14` (`DisplayMachine::signalDisplays(mask)`), `ctrl+0x2c` (faults), **and writes none of them**: the register is read-to-clear in the real device, in our RAM the host drops the bits itself a tick later. **Bit `i` of `ctrl+0x14` = display pipe with port `i`.** `signalDisplays` calls `DisplayPipe::signalDisplay` for each set bit; `signalDisplay` takes `old = shared+0x100` with an atomic compare-exchange that clears the bits **that are in the enabled mask** (`+0x104`), and acts on `old & enabled`: bit 0 -> a call into the `IOAccelDisplayPipe` base class (`signalVBL`-like: [INFER] the VBL notification of the accelerator's clients), bit 2 -> event source `process_online`, else bit 3 -> event source `process_offline` (**if both are pending in one call only the online one runs**: both bits were cleared). A pending bit that is not enabled stays pending. `enable()` calls `signalDisplay` itself right after storing 0xc, so **an online bit set before `enable()` is delivered then**. This corrects the first reading in hub-task-451 ("bits 2/3 = transaction events"): **bits 2/3 are display connect / disconnect, not transaction completion.**
- `process_online`: reads the host's display info (+0x00, +0x04, +0x14/+0x16, +0x20 -> `pipe+0x3ac`, +0x2c.., +0x4c.., +0x208, the modes at +0x210), calls `AppleParavirtFramebuffer::connectionChange(...)` (builds the mode list, **synthesises an EDID** from name/size/chromaticities/modes with `edidInit`, sets `ParavirtDisplayPrefs`, notifies IOFramebuffer clients) and sends FIFO 0x02 `{port, +0x200}`.
- `process_offline`: the same `connectionChange` call with **zero modes and no mode callback** (the display goes away); no FIFO command.
- **VBL.** Two separate things: (1) the *framebuffer's own* `'vbl '` interrupt (what WindowServer waits on) is a **software timer in the guest** (`AppleParavirtFramebuffer::setVBLTimer`: period from the current mode's timing, `mach_absolute_time`, an `IOTimerEventSource`), it needs no host signal; (2) `DisplayPipe::enableVBLInterrupt` (sets +0x104 bit 0, called by `IOAccelDisplayPipe` for accelerator clients, e.g. Metal's display-link/present paths [INFER]) wants **the host to set +0x100 bit 0 and raise `INTR_STATUS_DISP` bit `port` once per frame**. Without it those clients never see a VBL.

**Transactions (flip/present).** `submitTransaction(txn)`: takes the surface id of the transaction's framebuffer surface (`IOSurface::getSurfaceID`, or the id at +0x180 of an `IOAccelResource` of type 6), `prepareResourceHeap(task)`, then builds on the pipe's channel
- **0x06** (when shared+0x1c was 0): `{u32 port, u32 surface id, u32 task}` (12 bytes; `task` = the u32 at +0x268 of the accelerator's root task object [INFER: root task id; the same value is at +4 of the cursor glyph command]);
- **0x07** (shared+0x1c != 0): 36 bytes: `{u32 port, u32 task, u32 surface id, u64 a, u64 b, u32 n, u32 sum}`; `a`, `b` are zero unless the transaction carries a new gamma table (`prepareGammaTable` builds `3 n` u32 of R, G, B ramps in a kernel buffer, maps it into the task, stores `n` and the sum of all entries) [fields INFER: a = the mapping's id, b = its size/address].
The command is attached to an `AppleParavirtDisplayPipeFence` (an `IOAccelEvent`) whose completion is **the stamp of the pipe's channel reaching the command's `signal`** (`isTransactionComplete` is the event machine's test; `Fence::notifyClient` tells the pipe): **completion needs no display-specific host signal, only the generic stamp store + `INTR_STATUS_GPU` bit of the channel** (s.1, `completeStamp` in the host) [INFER that the command carries a signal: the allocator's `addSignal` is called by the event machine, not visible in `submitTransaction`; the host log shows each command's `signal`]. On an error path (resource heap, bad transaction, gamma table failure) the driver sends **0x1e FlushChannelEvent** with no payload and returns 0xe00002bd / 0xe00002c2 / 0xe00002be.

**Cursor.** The driver chooses by the negotiated version: `AppleParavirtAccelerator::setupVersion` writes **6 to `ctrl+0x34` and reads it back**; the byte flags it derives from the result (`accel+0xf4c..0xf6c`) are all zero if the host returns more than 6; flag `+0xf50` (1 for versions 4..6) becomes `pipe+0x3aa` = "hardware cursor". With version 6: **position and visibility live in the shared page** (+0xe00 / +0xe04) and the host is told by a kick: a store of the port number at **`ctrl+0x220`** (when the host's +0x20 flag bits ask for it; it can be the same value every time, so the host should poll the two shared fields rather than the register). Without hardware cursor, a visibility change sends FIFO **0x05** `{port, u8 visible}` instead. The glyph is always FIFO **0x04**: the driver copies the image into a kernel buffer, runs `processCursorImage` (crops/measures it: four u16 results), maps it into the task and sends `{u32 port, u32 task, u64 mapping id [INFER], u64 [INFER], u64 pitch = width * 4, u16 w, u16 h, u16 x, u16 y [INFER order], u32 pixel sum}` (44 bytes). `PGCursorAnatomy` / `PGCursorValidation` boot-args make the driver draw an outline / clamp the image for debugging.

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

**Commands.** `docs/m1-opcodes.md` (generated by `tools/metal/m1/gen-opcodes.py --md` from the writers' call sites) has every id, payload length and field list. Ranges:

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

## 5b. Measured on the rig (Tahoe 26.6.2, RX 9070 XT, 2026-10-21) [measured]

M0 runs on the real machine (`make PVGPU=1`, `rdna4-pvgpu=1 rdna4-pvgpu-class=0xff0000 rdna4-pvgpu-delay=120`): Apple's driver talks to our host and
Metal apps submit work. `tools/metal/m1/mtlprobe.m` issues known work (a blit, a compute dispatch, a render pass) and the host's log shows it:

- **A task's GPU page table.** `DefineHostTask` names a root page: a header whose first dword is the page number of the top table (then 3, ...; the
  dword at +0x18 is 4, the top table's entries for the 16 GiB space). Below it three tables of **dword** entries: top `[va >> 32]`, middle
  `[(va >> 22) & 1023]`, leaf `[(va >> 12) & 1023]`. Leaf entries are `0x80000000 | page number` (bit 31 = mapped), upper entries bare page numbers.
  The leaf counts match the commits exactly (a 1 MiB heap at VA 0x1000, a 128 KiB command-buffer region, one page per small buffer).
- **Resources.** Each resource id has a fixed GPU range: `DeleteHostResourceID {task, id}` comes paired with `ReleaseFromGPUPageTable {task, va,
  length}` (buffer 1 at 0x101000, 4 KiB). `SetResourceHeap` is `{task, 1, 0x100000}`; every task commits VA 0x1000, 1 MiB.
- **`ExecIndirect`** carries the task, its resource list and the command buffer's GPU address and length (table in s.1). The command buffer, read
  through the page table, is the stream of s.4 with the framing as written there: segment `{u32 length, u8 type, ...}`, commands `{u32 id, u32 size}`.
  The probe's work decodes with **zero unknown ids and zero undecoded bytes** against `src/pvopcodes.inc`: `Blit.fillBuffer {buffer 1, offset 0,
  length 0x1000, value 0x5a}` + `Blit.copyFromBuffer {1 -> 2, 0x1000}`; `Compute.setComputePipelineState {2}` + 3x `Compute.setBuffers {index, 1,
  buffer, u64 offset}` + `Compute.dispatchThreads {u64 threads[3], u64 per group[3]}` (64/64); `Render.writeDescriptor` (584 bytes, the clear colour
  as doubles) and `Blit.copyFromTexture`.
- **`CreateComputePipeline` (FIFO 0x3b)** is `{task, pipeline ref (2, the one setComputePipelineState names), 5, 6, u64 gpu address}`; the address
  lies in a 1 MiB region committed just before, and read as zeros at that moment (where the compiled function lives is still open).
- **The resource heap** (VA 0x1000, 1 MiB) starts with 12-byte records indexed by resource id: `{u32 (descriptor bytes << 8) | type, u32 descriptor
  address, u32 0}`. Types seen: 1 buffer (16-byte descriptor), 6 (32 bytes, probably the function), 7 compute pipeline (24 bytes; the stream's
  pipeline ref). A buffer's descriptor is `{u64 length, u64 page number of its start}` (mtlprobe's two 4 KiB buffers: `{0x1000, 0x101}`,
  `{0x1000, 0x102}`). The stream names buffers by these resource ids.
- **M2, first step: blits run on the card.** `Blit.fillBuffer` and `Blit.copyFromBuffer` are carried out with SDMA on the guest's pages, found through
  the resource heap and the page table, reached through the GC hub's AGP aperture (grown on demand to the highest bus address needed) by
  `RDNA4Compute::sysFill/sysCopy`. The engine does not snoop the CPU's caches: without `kIOMemoryIncoherentIOFlush` around each transfer the CPU
  reads its stale lines (a kernel-page self-test read 0/1024 written dwords; with the flushes 1024/1024). mtlprobe's blit now reads
  `0x5a 0x5a .. 0x5a` in its destination buffer.
- **The compiled function is a metallib.** A function resource (type 6) has a 32-byte descriptor whose first qword is the GPU address of a
  standard Metal library (magic `MTLB`, 0xe20 bytes for mtlprobe's vadd), with the function's `NAME`, `TYPE` and a 32-byte `HASH` tag (vadd:
  `552cf258b072b59d...`): the AIR reaches the host in the clear, no compiler plug-in is involved. `CreateComputePipeline`'s address is the end of
  that library. A compute pipeline is type 7 (24 bytes, not decoded), a texture type 2 (116 bytes; the first qword is its size, 0x4000 = 64x64x4).
- **M2, second step: a compute dispatch runs on the card** with a precompiled stand-in for the function (the plan's bootstrap before the AIR
  compiler): `src/shaders/mtlvadd.cl`, chosen by the function's name and its three bound buffers. The host follows `setComputePipelineState`,
  `setBuffers {u32 first index, u32 count, count x {u32 resource id, u64 offset}}` and `dispatchThreads`, copies the bound buffers into runtime
  buffers, dispatches through the runtime as one of its clients, and writes `c` back into the guest's pages: mtlprobe's compute reads
  `c[0] 1000 c[1] 1001 c[63] 1063` (64 threads, 30 us on the card).
- **M2, third step: a render pass that only clears.** `Render.writeDescriptor` (584 bytes) carries colour attachment 0 as `{u32 texture at
  +0x4c, u32 (store action << 16) | load action at +0x60, double clear colour[4] at +0x68}` (mtlprobe: texture 1, 0x00010002 = clear + store,
  0.25 0.5 0.75 1). A texture (type 2, 116-byte descriptor) is dwords `{u64 size, u32 start page, ..., [13] bytes per row, [15] width}`, linear
  (mtlprobe: 0x4000 bytes at page 0x101, 0x100 per row). The host fills the texture with the colour as a BGRA8 pixel (the format is assumed
  until found in the descriptor; the colour is converted from the doubles' bits with integers, no floating point in the kernel) and carries out
  `Blit.copyFromTexture {u32 texture, u32 buffer, u64 origin[3], u64 size[3], u64 destination offset, u64 bytes per row, u64 bytes per image,
  u32 slice, u32 level}` for a whole linear texture: mtlprobe's render reads `B 191 G 128 R 64 A 255`. **All three mtlprobe checks now pass on
  the card.** Other processes (system UI) also send render passes to the device now; theirs carry other values at +0x4c (no texture resources
  there) and are refused, and nothing they draw is executed.
- **M3, first step: kernels translated from AIR.** `tools/metal/m3/air2amdgpu.cpp` reads a kernel's AIR with upstream LLVM (23 reads
  `air64_v28` as it is: buffers are `ptr addrspace(1)` as on AMDGPU, the roles are `!air.kernel` metadata), wraps it in an `amdgpu_kernel`
  that computes the position arguments from the work-group and work-item ids and bounds-checks them against the grid (dispatchThreads need not
  fill whole groups), and emits a gfx1201 object. Its kernargs: a 64-bit address per buffer index, then u32 threads per threadgroup[3] and
  u32 threads in the grid[3]. `tools/metal/m3/build.sh` translates `tools/metal/m3/kernels.metal` (mtlprobe's `vadd` and the 2D `grid2d`)
  into the committed table `src/mtlkernels.h` (with each kernel's written buffers, from `air.read` / `air.read_write`); the host picks the
  entry by the function's name, copies every bound buffer from its offset to its end into the runtime, dispatches `ceil(threads / per group)`
  groups and copies back the written ones (`src/shaders/mtlvadd.cl` is gone). **On the card (2026-10-21): mtlprobe's `compute` reads
  `c[0] 1000 c[1] 1001 c[63] 1063` (vadd, 23 us) and the new `grid` (50 x 20 threads in groups of 16 x 4, partial groups at the edges)
  `0 of 1000 wrong` (grid2d, 28 us), both through the translated kernels; blit and render still pass.**
- **M3, second step: translated at run time.** A root helper (`air2amdgpu --serve`, installed as a LaunchDaemon by
  `tools/metal/m3/install-helper.sh`) opens a user client of type `'MTLC'` on `RDNA4ComputeService` (`include/rdna4mtlc.h`). A function the
  host has not seen (by its metallib `HASH`) is handed to it whole: the metallib's file size is the header u64 at 0x10; the helper finds
  the function's bitcode by its `NAME`, `OFFT` (third u64: offset in the bitcode section, whose offset is the header u64 at 0x48) and `MDSZ`
  (bytes), translates it, links it with lld in the same process and returns the code object with its buffer count and written mask. The host
  waits at most 3 s, loads it into the runtime and caches it by hash (32 entries). Without a helper it falls back to `src/mtlkernels.h`.
  The helper for the rig is x86_64 with LLVM/lld 23.1.1 built from source for x86_64, AMDGPU only, linked in statically (`build.sh
  helper`, 58 MB); its code objects match Homebrew LLVM's byte for byte except the linker's name in `.comment`; one translation takes about
  30 ms on the rig. A partial threadgroup's `threads_per_threadgroup` is its own size in Metal (measured on Apple's GPU), not the requested
  one. **On the card (2026-10-21): vadd, grid2d and mtlprobe's new `mix` (a kernel only the guest has: read and written buffers, threadgroup
  position, position in the threadgroup, a partial last group) were each translated at run time and are correct (`0 of 100 wrong`), 27 us
  each on the card.** Open: one boot before that run froze with a panic in `AppleGPUWrangler` (null pointer in its work loop, 2 s after the
  host publishes the fake GPU at `rdna4-pvgpu-delay`); the next boot with the same kext did not; that boot also lit only the DP display, at
  another resolution, before the host started. The helper parses data from Metal clients as root: sandbox it before it is on by default.
- **M3, third step: several pipelines per process.** A compute pipeline's resource (type 7) has a 24-byte descriptor `{u32 0xb, u32 24, u32
  its own id, u32 7, u32 (function id << 24) | 0x040001, u32 0}` (pipelines 2 and 4 of one process: 0x01040001 and 0x03040001, their
  functions are resources 1 and 3; ids above 255 not seen). A function's descriptor is `{u64 metallib address, u32 metallib bytes, u32 0xf,
  u32 0x14, u32 its own id, ...}`, and each function has a metallib of its own even when the source library has several kernels (add1 at
  0x101000, 0xd70 bytes, mul3 right after it at 0x101d70). `CreateComputePipeline {u32 task, u32 pipeline id, u32 5, u32 6, u64 address}`
  does not name the function (5 and 6 for every pipeline). **On the card (2026-10-21): mtlprobe's `two` (two kernels of one library) and
  `twolib` (two libraries) each run two pipelines back to back in one command buffer, all four kernels translated at run time: both
  `0 of 64 wrong`.**
- **M3, fourth step: the common parts of the language.** Before code generation the translator inlines the AIR function into the
  kernel, drops the module's other kernels (one unsupported kernel no longer sinks the rest) and legalises the body (`legalize()` in
  `tools/metal/m3/air2amdgpu.cpp`): AIR's constant address space (2) is AMDGPU's GDS, so constant buffers become global memory again; local
  arrays move to address space 5 (they must stay out of scratch, which the runtime does not give kernels: LLVM keeps small ones in
  registers); `air.*` calls (Metal compiles with fast math, so `air.fast_sqrt.f32` and the like) become LLVM intrinsics, `pow` becomes
  `exp2(y log2 x)`, `rsqrt` `1 / sqrt`; `air.wg.barrier(flags, scope)` becomes fence, `s_barrier`, fence; `air.atomic.{global,local}.<op>`
  becomes `atomicrmw` (relaxed, device or workgroup scope). Threadgroup memory is address space 3 on both sides. Refused by name for now:
  SIMD-group functions (`air.simd_sum`...), constant tables in AIR's constant address space, anything else unmapped. **On the card
  (2026-10-21): mtlprobe's `feat`, nine kernels (constant reference, struct and array, math, a threadgroup reduction with barriers, device
  atomics, a local array indexed by data, integer functions, half), all translated at run time and equal to the CPU's results.** Not
  decoded yet: `setBytes` (inline constants), a common way to pass small constants.
- **M3, fifth step: the rest of the usual compute encoder.** `setBytes` needs nothing of its own: the driver copies the bytes into a buffer
  of its own and binds it with `setBuffers` (resource 4, offsets 0 and 0x20 in mtlprobe's `bytes`). `Compute.dispatchThreadgroups` (0x0c8)
  is `{u64 groups[3], u64 threads per group[3]}` and was skipped until now; `Compute.setBufferOffset` (0x0cf) is `{u32 index, u64 offset}`.
  **On the card (2026-10-21): mtlprobe's `bytes` (setBytes, dispatchThreadgroups, setBufferOffset) `0 of 64 wrong`; all other probes pass.**
- **M3, sixth step: SIMD-group functions and conversions.** A Metal SIMD group is a wave of 32 (Metal's width; the kernels are wave32):
  `thread_index_in_simdgroup`, `simdgroup_index_in_threadgroup` and `simdgroups_per_threadgroup` come from the thread's linear index in its
  group (x fastest, the order the hardware packs waves); reductions are `llvm.amdgcn.wave.reduce.*` (active lanes only, as on Apple's GPU),
  shuffles and broadcast `llvm.amdgcn.wave.shuffle`, ballot/any/all `llvm.amdgcn.ballot`, prefix sums five upward shuffles (right when the
  inactive lanes are the top ones, as at the grid's edge). Measured on Apple's GPU first: a shuffle up or down past the group's edge keeps
  the lane's own value. `air.convert.<to kind>.<to type>.<from kind>.<from type>` (kinds f, s, u) become LLVM casts, float to integer
  saturating; every kernel mixing integers and floats needs them. **On the card (2026-10-21): mtlprobe's `simd` (40 threads: a full SIMD
  group and one of 8 lanes; reductions, prefix sums, shuffles, broadcast, ballot, any/all, the indices) 0 of 432 results wrong.**
- **M4, first look at draws (2026-10-21, measured with mtlprobe's `tri` and `drawargs`).** A render pipeline has no FIFO command of its
  own: it is a resource (type 7, like a compute pipeline) whose descriptor starts with the same 16-byte header `{u32 tag (0xe render, 0xb
  compute), u32 bytes, u32 its own id, u32 ?}` and continues with a list `{u8 count, count x {u8 key, u8 size, value}}`: render key 1 =
  the vertex function's resource id, key 2 = the fragment function's, key 8 = 0x14 (not known); compute key 0 = the function (so the
  compute rule "top byte of the fifth dword" is this list for small ids). After the list come the attachment states: 0x50 = 80 =
  MTLPixelFormatBGRA8Unorm for colour attachment 0. `Render.setRenderPipelineState {u32 id}`. The draws are one opcode per encoding,
  odd = compact 16-bit fields: 0x001 `{u32 primitive type, u16 vertex start, u16 count}`; 0x003 `{u16 start, u16 count, u16 instances, u16
  type}`; 0x005 `{u16 start, u16 count, u16 instances, u16 base instance, u32 type}`; 0x007 drawIndexed `{u32 type, u32 index buffer, u16
  count, u16 offset}` (uint16 indices; the even opcodes are the wide forms, not seen yet). Executing a draw needs the vertex function as an
  NGG shader and the fragment function as a pixel shader: LLVM compiles an `amdgpu_gs` function with the ABI of `src/shaders/nggcol.s`
  (`llvm.amdgcn.init.exec`, position and primitive exports) to almost the same code as the hand-written shader, and its PAL metadata gives
  RSRC1 and the VGPR count; the draw itself can be the template stream of `src/gfx12_draw.h` with its target, viewport and draw patched.
- **M4, second step: a Metal triangle drawn on the card (2026-10-21).** The helper translates vertex and fragment functions too
  (`translateShader()` in `tools/metal/m3/air2amdgpu.cpp`): a vertex function becomes an `amdgpu_gs` NGG shader with the ABI of
  `src/shaders/nggcol.s` (s3 = merged_wave_info, v0 = the packed primitive, v3 = VertexID; `llvm.amdgcn.init.exec`, pos0 and prim
  exports; no GS_ALLOC_REQ: the stream sets PRIMGEN_PASSTHRU_NO_MSG), a fragment function an `amdgpu_ps` exporting MRT0 as 4 x fp32; both
  are compiled for `amdgcn-amd-amdpal`, whose metadata note gives RSRC1/RSRC2 and SPI_PS_INPUT_ENA/ADDR, and handed back as an `RGFX` blob
  `{"RGFX", u32 stage, u32 code bytes, u32 RSRC1, u32 RSRC2, u32 PS_INPUT_ENA, u32 PS_INPUT_ADDR, u32 0}` + code. The host
  (`runDraw()` in `src/pvgpu.cpp`) takes the pipeline's functions from its descriptor (keys 1 and 2), the target from the render pass's
  colour attachment, and draws with the stream of `src/gfx12_draw.h` (built by `userspace/rdna4-run/gfx12tri.h`, the triangle every gfx
  test draws) with the translated shaders, their registers, the target's size (CB_COLOR0_ATTRIB2 = pitch - 1 << 16 | height - 1, the
  window and viewport scissors), the viewport (Y scale -h/2: Metal's y points up) and the vertex count patched; the BGRA8 texture is
  staged through a linear RGBA8 target (pitch a multiple of 32 pixels) and swapped on the way in and out. **On the card: mtlprobe's
  `tri` (vertices from vertex_id, a constant colour, 64x64 BGRA8) equals Apple's GPU: centre 64 128 255 255, upright, 512 pixels lit;
  16 us for the draw.** So far: triangle lists from vertex 0, one instance, no vertex buffers, no vertex outputs but the position, no
  fragment inputs; anything else is refused by name. The rig once failed the compute bring-up at stage 4 (SDMA's test write did not land)
  after a warm restart; a cold boot cleared it.
- **The freezes at the fake GPU's publish (solved, 2026-10-21).** Three boots froze: `AppleGPUWrangler` (null pointer in its work loop)
  1.5-2 s after the delayed publish. In the two with a readable log the kext's `IONDRVSupport` hook had never run, so the card was never
  brought up (only the firmware's picture, on the DP display) and the delayed publish fell back to the platform expert, the emulator's
  case: Apple's driver came up on a GPU with nothing behind it. On real hardware the host no longer publishes without the card
  (`rdna4-pvgpu` builds without `VMTEST`); the next such boot logs `not publishing after the delay`. Open: why the hook sometimes does not
  run (three boots of about a dozen that day: two were the first boot of a newly installed kext, one the restart after such a freeze).

## 6. What is implemented now (code)

- `src/pvstream.{hpp,cpp}` + generated `src/pvopcodes.inc`: names/lengths of the three layers (binary-searched), `fifoHeader`/`describeFifo` (FIFO framing validation and per-command description), `streamHeader`/`walkCommands` (stream and operation framing), `segmentTypeName`. Host test `test/pvstream-test.cpp` (in `make test`): framing across a ring wrap, barriers, malformed headers, unknown ids, truncation, table spot checks against the values read above.
- `src/pvgpu.cpp` polling host: every new FIFO span is walked as commands and logged as `host: fifo @0x...: cmd 0x3a GetDeviceInfo, 24 bytes (12 payload), 0 barrier(s), signal 0x7: reply buffer page 0x1234, 4096 bytes`; a span that does not start with a command header is hex-dumped (12 times) and abandoned; everything is acknowledged (`FIFO_READ = FIFO_WRITTEN`). The self-test sends two valid commands, one across the ring wrap. Still log-and-consume, default off.
- Child channels (`DefineChannel` -> record in the root page -> page-list ring, polled every tick, commands decoded and answered, per-channel stamp and interrupt bit): implemented in the M0 host and self-tested (`docs/m0-pvgpu.md`, hub-task-447). The page list has no length field: the host takes the entries up to the first zero (the driver zeroes the page first).
- Display pipe (hub-task-451, `src/pvgpu.cpp` + `pvstream::disp`): the shared-state page stays mapped; optional *online* event with the display info (`rdna4-pvgpu-disp` bit 0), ~60 Hz VBL events for pipes whose guest enabled VBL (bit 1, default), level-triggered `INTR_STATUS_DISP`, the 0x02/0x04-0x07/0x1e commands decoded and counted (their completion is the generic stamp), cursor fields and the `ctrl+0x220` kick logged; self-tested (`docs/m0-pvgpu.md` Test A).
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
- `tools/metal/m1/apvdis.py classes|methods|dis|strings|addr` (the bundle: ObjC classes with ivars, method addresses/sizes, annotated disassembly with selector/CFString/cstring resolution; `llvm-objdump` for the instruction listing),
- `tools/metal/m1/apvcmds.py table|layout|json` (the command scanner), `tools/metal/m1/gen-opcodes.py [--md]` (generates `src/pvopcodes.inc` and `docs/m1-opcodes.md`),
- `tools/metal/m0/pvdis.py list|dis|xref|vt` (the kernel driver in the collection, imports resolved through the chained fixups).
