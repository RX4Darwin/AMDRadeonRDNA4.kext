# M0, kext side: the fake Apple paravirtual GPU (hub-task-427)

Branch `premetal/m0` (from `premetal/metal-spike`). Code: `src/pvgpu.{hpp,cpp}`, boot-arg `rdna4-pvgpu=1|2` (default off); display side (hub-task-451): `rdna4-pvgpu-disp=<bits>` (bit 0 = report the display *online*, bit 1 = ~60 Hz VBL events for pipes whose guest enabled VBL; default 2, level 2 forces 3) and `rdna4-pvgpu-width=` / `rdna4-pvgpu-height=` (the display's mode, default 1920x1080). Helpers: `tools/m0/pvdis.py` (read Apple's kext), `tools/m0/check-vtable.py` (is our `IOPCIDevice` the one Tahoe has).
Status words as in `docs/metal-readiness.md`: **measured** = read from Apple's binaries or a run; **inferred** = reasoned, not shown; **code** = written and compiled, not run.

## What it is, in one paragraph

Apple's `AppleParavirtGPU.kext` drives a virtual PCI function (vendor 0x106b, device 0xeeee) made of an MMIO window, a FIFO in guest RAM and a version handshake; on a full macOS install it matches by `IOPCIMatch 0xEEEE106B`. With `rdna4-pvgpu=1` our kext publishes an object that looks like that PCI function: a subclass of `IOPCIDevice` (`RDNA4PvNub`, registry name `PVGPU`) with a RAM-backed BAR0 (16 KiB, control block at +0x1000), a config space with an MSI capability, and an interrupt source our side can raise. A polling "host" (`RDNA4PvHost`) watches the control block, maps the FIFO the guest announces, **logs** what the guest writes and acknowledges it. It executes nothing: M0 asks only "does Apple's driver accept the device and how far does it get"; the host loop proper is M1.

## What Apple's driver needs from its provider [measured: disassembly of `com.apple.driver.AppleParavirtGPU` in 25G83's `SystemKernelExtensions.kc`, `tools/m0/pvdis.py`]

The sequence, in the order the code runs it (control-block offsets are from BAR0+0x1000):

| Step | Who | What it does with the provider / registers |
|---|---|---|
| match | IOKit | personality `AppleParavirtGPUControl`: `IOProviderClass IOPCIDevice`, `IOPCIMatch 0xEEEE106B`, `IOMatchCategory IOFramebuffer`, `IOProbeScore 1000000`, `FramebufferCount 0`, `AcceleratorProperties {MetalPluginName AppleParavirtGPUMetal, MetalPluginClassName AppleParavirtDevice, ...}` |
| `Control::start` | `AppleParavirtGPUControl` | boot-arg `-x` read (safe mode); `provider->setProperty("model", "Apple Paravirtualized Graphics Device")`; `setupMMIO()`; reads **`ctrl+0x22c` = number of display pipes** (0 is taken as 1, more than 8 is clamped to 8); then the superclass (`IOFramebuffer`) start |
| `Control::setupMMIO` | | `getProvider()` cast to `IOPCIDevice` (`safeMetaCast`); `mapDeviceMemoryWithRegister(0x10, 0)`; `getVirtualAddress() + 0x1000` is the control block |
| `Accelerator::configureDevice(IOPCIDevice*)` | `AppleParavirtAccelerator` (a virtual of `IOGraphicsAccelerator2`) | `retain`; `findPCICapability(5 /*MSI*/, &off)` must return non-zero; `setMemoryEnable(true)`; `setupMMIO` (same mapping); `setupVersion`; `setupRoot`; `setupFIFO` |
| `setupVersion` | | writes `6` to `ctrl+0x34` and **reads it back**; the read-back is the version the host accepts (plain RAM returns 6: accepted, features of version 6). [Inferred from the code] If the read-back is rejected (bit 31 set, or a version whose feature byte is zero) the driver lowers its proposal and retries, ending with a write of `0x80000006` |
| `setupRoot` | | 4 KiB `IOBufferMemoryDescriptor`, wired, mapped; its physical page goes to `ctrl+0x1c` |
| `setupFIFO` | | `IOBufferMemoryDescriptor::withOptions(0x890, 0x10000, 1)` (64 KiB, physically contiguous, mapper-none), wired; first 4 KiB zeroed (root header), ring = bytes 0x1000..0x10000 (60 KiB); writes `ctrl+0x30 = phys>>12` (FIFO_BASE_PAGE), `+0x04 = length`, `+0x10 = 0x1000` (FIFO_START), `+0x00 = 1` (enable) |
| `Accelerator::start` | | `IORangeAllocator`; `open(provider)`; `setupInterrupts` (**`IOInterruptEventSource::interruptEventSource(this, provider, 0, block)`** added to the accelerator's work loop and enabled: the provider must implement `getInterruptType/registerInterrupt/enableInterrupt`); `setupChannels` (root channel "ROOT" 0x4800 bytes, then child channels "Exec", "Immediate", "Uploads", "Downloads" of 0x10000 bytes); `setupDeviceInfo`; `registerService` |
| `writeFifo(len, data)` | every command | free space = `FIFO_READ + ringLen - written`; **spins until that is >= len** (so a host that never advances `FIFO_READ` hangs the guest after 60 KiB); position = `written % ringLen` (split at the wrap); then `ctrl+0x08 = written` (FIFO_WRITTEN) |
| `setupDeviceInfo` | | builds command `0x3a` ("get device info", `AppleParavirtCommandAllocator::init(0x3a, 0x400)`) with a sub-record `0x2d` carrying a 4 KiB reply buffer (size/8, physical page), barrier + signal, `submitOnChannel(root)`, waits, then `parseDeviceInfo(buffer, size, APVDeviceInfoStruct*)` (999 bytes of parsing). **This is where the host has to answer**: write the device-info blob and the completion. M0's host does not, so it is the expected stall point (see "What M0 should show") |

Other provider calls found in the kext (scan of every function for virtual calls beyond IOService's range): `IOPCIDevice::mapDeviceMemoryWithRegister` (Accelerator, Framebuffer, Control), `findPCICapability`, `setMemoryEnable` (also in `teardownDevice`), and `deviceMemoryRead` (slot +0x968, in `AppleParavirtCommandQueue::commandQueueStart/processExecIndirect`; [inferred] the provider's, not shown). Display-pipe/machine code calls virtuals at +0x850..0x870 (`configRead32/16/8` shapes) on its own object: not the provider [inferred from the receiver being `this`]. Register offsets used by the kext beyond the doc's table (s.6.1): `+0x22c` (display count). The register table of `docs/metal-spike.md` s.6.1 stands.

## Implementation notes

- **A subclass of `IOPCIDevice`, because the driver casts and calls it.** IOPCIFamily is in the boot collection OpenCore injects into (`com.apple.iokit.IOPCIFamily` is already a dependency), so it links normally, unlike IOGraphicsFamily (`docs/metal-spike.md` s.9.7, `src/accelcensus.hpp`). `nm -u` of the built kext has no undefined symbol that `BootKernelExtensions.kc` does not define (besides Lilu's), which includes the symbols of `IOPCIDevice`.
- **The nub never runs IOPCIDevice's own bridge-dependent code.** Every lifecycle/power/match virtual is overridden to the `IOService` behaviour; the config accessors read a 256-byte array; `free` is `IOService::free`. The nub is never released once published.
- **Layout and vtable check (measured, `tools/m0/check-vtable.py`).** MacKernelSDK's `IOPCIDevice.h` lays out the same vtable as Tahoe's `IOPCIFamily`: 313 slots compared, **11 differences: the two destructor slots (only the derived class's name differs) and nine that Tahoe turned from reserved into real virtuals: `configRead32/16/8(UInt8)` and `configWrite32/16/8(UInt8, v)` (slots 272-277), `configureInterrupts` (300), `deviceMemoryRead/Write` (301-302)**. The reserved symbols no longer exist in Tahoe's IOPCIFamily, so a derived vtable that inherits them would leave undefined symbols: the nub overrides the reserved virtuals (removing the references) and **copies its vtable at run time, pointing those nine slots at working implementations**. `publish()` also refuses if Tahoe's `IOPCIDevice` instance size (`OSMetaClass::getClassSize`) exceeds the header's (our data members would overlap its own).
- **Interrupts.** The nub is its own interrupt controller: `registerInterrupt` stores (target, handler, refCon), `enable/disableInterrupt` flip a flag, `raiseInterrupt(n)` calls the handler as `IOInterruptController::handleInterrupt` would. Nothing raises one in M0 except the self-test; the reims register set has `INTR_STATUS_DISP/GPU` (+0x14/+0x18) and `INTR_FAULT` (+0x2c) for the real thing.
- **The host loop (`RDNA4PvHost`).** One work loop and timer (5 ms idle, 2 ms during the self-test). Per tick: log changes of the control-block dwords (not FIFO_WRITTEN/READ); when `FIFO_BASE_PAGE`, `FIFO_LENGTH` and `CONTROL_FIFO` are set, map the physical page (`withPhysicalAddress` + `createMappingInTask`) and log the announcement; for new bytes (`written - read`, counters are monotonic modulo 2^32) log up to 64 bytes in hex per command and set `FIFO_READ = FIFO_WRITTEN`. The first 80 commands are logged, then silently consumed; total budget 400 `pvgpu:` lines.
- **Child channels (hub-task-447).** `DefineChannel n` from the root FIFO makes the host read channel `n`'s **state record** in the root page (`ROOT_PAGE` register, mapped when announced) at `+0x400 + (n - 1) * 20`: `{u32 write, u32 read, u32 0, u16 kind, u16 0, u32 page-list page}`, map the page-list page, take the page numbers up to the first zero (the driver zeroed the page, so the list is zero-terminated; at most 256 pages = 1 MiB per ring), and map those pages as one contiguous ring (`IOMemoryDescriptor::withAddressRanges` of physical ranges). Each tick every defined channel with `write != read` is decoded by the same walker as the root FIFO (`ch<n>` in the log), `read = write` is stored when done, and a command's `signal` completes **stamp `kind`** (the channel's stamp index; FIFO page `+4 * kind`) with `INTR_STATUS_GPU` bit `kind` and the interrupt. `ctrl+0x20` (the doorbell, the channel number, written only when the ring was empty) is not needed: all defined channels are polled. Guard rails: channels 1..16 only, a record read only inside the root page, ring pages and page-list entries taken as given but never written (the host writes only the record's `read` counter, stamp slots inside the FIFO page, `+0x12`/`+0x1c` of a mapped shared-state page, and the reply buffer inside its announced length), pending bytes beyond the ring size resynchronise instead of being read, a `DefineChannel` before the root page is announced is kept and retried, a channel is dropped when the root page register or the FIFO registers are cleared, unknown commands are logged by id and consumed.
- **Matching.** `matchPropertyTable` accepts **only** a personality that carries `IOPCIMatch` or `IOPCIPrimaryMatch` that matches 0xEEEE106B (with `IOPCISecondaryMatch`/`IOPCIClassMatch` honoured if present); every table offered is logged once (first 40) as `match offered: <bundle> <class> (IOPCIMatch ...) -> MATCH|no`.

## How Apple's kext gets matched (and why no personality injection is needed)

On a **full** install `AppleParavirtGPU.kext` is in `SystemKernelExtensions.kc`: its personality is in the IOKit catalogue from boot and the kext is loaded when a provider matches (the ordinary mechanism for any system kext). The OpenCore restriction of `docs/metal-spike.md` s.9.7 is about **linking** our kext against collections that are not the boot collection, not about matching: we link only against the boot collection (`IOPCIFamily`, `kernel`), and our nub is a plain `IOService::registerService()`. So **the route is: publish the nub, let the catalogue match.**
Fallbacks if the catalogue does not offer the personality (checked from the `match offered` log lines, none for `AppleParavirtGPUControl`): (a) `IOCatalogue::addDrivers` with a copy of Apple's personality (MetalCyan's technique, Lilu has the pattern; needed in Recovery, where the kext is not present at all, so there it cannot work); (b) none needed otherwise. [inferred until the VM run]

## Safety

- Default off, **emulator/VM only**. Never in a real-card boot: with Apple's driver matched, `AppleParavirtGPUControl` is an `IOFramebuffer` and would present a second display adapter with no modes next to the card's display (WindowServer would see it). The boot-arg is not in `tools/set-boot.sh`.
- No GPU access of any kind: the nub only touches its own RAM, the host reads the FIFO pages the guest announces. Reading a physical page announced by a guest driver is the one place where a wrong value could touch foreign memory: the length is limited to 1 MiB and the page must come with `CONTROL_FIFO = 1`.

## Tests

**A. Self-test, no Apple kext, in Kiln's emulated Recovery** (level 2: the kext plays the guest driver against the nub). **Measured so far:** the first run (`...-191810-boot1-m0`, tip 2118816) published the nub (the size check passed: `IOPCIDevice is 184 bytes here (header 184)`) and then **panicked with a kernel stack overflow**: `IOService::matchPropertyTable(table, score)` calls the virtual one-argument version, and the nub's overloads called each other; fixed in 1f9a905 (the PCI logic is in a helper that calls neither). **Second run** (`...-193513-boot1-m0b`, kext with the GetDeviceInfo and display answers, 91ed891): **no panic, `selftest: PASS`**: IOKit offered the nub 37 PCI personalities (Intel/AMD framebuffers, XHCI, AHCI, NVMe, ...; the log keeps at most 40) and all were refused, then every check above passed in the emulated Recovery (version handshake, FIFO announce, three decoded commands across the ring wrap, the 31-pair reply, stamp 2, port 2 at +0x12, interrupt delivered, status bit dropped, mapping released).
**Third run** (`...-195557-boot1-m0c`, kext with child-channel polling, f04afac): **no panic, `selftest: PASS`** with every new check ok: root page mapped, channel 3 defined from its state record (stamp index 3, ring of 2 physical pages mapped as one range set), the `DisplaySetupSharedState` read from the child ring across its wrap and answered (`port 5 written at +0x12`), `stamp[3] = 7` with `INTR_STATUS_GPU` bit 3, the record's read counter caught up, both interrupts seen, bits dropped, all mappings released when the registers were cleared.
`KEXT=~/work/rx4darwin/RDNA4FB-m0/build/RDNA4FB.kext tools/emu-linux.sh 1 --extra 'rdna4-pvgpu=2'`, then read `rdna4fb.log`. Expected lines (all prefixed `RDNA4FB: pvgpu:`):

```
fake Apple paravirtual GPU published under <GPU name>: PCI 106b:eeee, BAR0 16384 bytes of RAM (control block at +0x1000), level 2; IOPCIDevice is 184 bytes here (header 184)
host: display side: online event on, VBL events on, mode 1920x1080
match offered: <bundle> <class> (IOPCIMatch ...) -> no            (up to 40 lines: what IOKit offered the nub; Recovery has no Apple paravirt personality)
selftest: start
selftest: config space, capabilities, personality matching        ok
selftest: mapDeviceMemoryWithRegister(0x10)                        ok
selftest: driver's mapping and the host's pointer are the same RAM  ok
selftest: version handshake: 6 written, 6 read back                ok
selftest: FIFO buffer (0x10000 bytes, options 0x890) allocated and wired   ok
interrupt 0 registered
selftest: IOInterruptEventSource on the nub (index 0)              ok
selftest: GetDeviceInfo reply buffer (4 KiB) allocated and wired   ok
selftest: display shared state page (4 KiB) allocated and wired    ok
selftest: root page and a child channel (page list + 2 ring pages) allocated and wired   ok
selftest: second display shared state page (for the child channel) allocated and wired   ok
host: FIFO announced: page 0x..., length 0x10000, ring start 0x1000, root page 0x0, version 6
host: fifo @0xeff8: cmd 0x30 DefineChannel, 16 bytes (4 payload), 0 barrier(s), signal 0x0: channel 3          (written across the ring's wrap)
host: fifo @0x8: cmd 0x3a GetDeviceInfo, 24 bytes (12 payload), 0 barrier(s), signal 0x1: reply buffer page 0x..., 4096 bytes
host: GetDeviceInfo answered: reply buffer page 0x... (4096 bytes, 512 pair slots), 31 pairs written, GpuCoreCount 64
host: root page 0x... mapped (channel state records at +0x400)
host: channel 3 defined: stamp index 3, ring of 2 pages (8192 bytes) from page list 0x..., counters write 8204 read 8184
host: fifo @0x20: cmd 0x01 DisplaySetupSharedState, 20 bytes (8 payload), 0 barrier(s), signal 0x2: pipe port 2, shared state page 0x...
host: display shared state page 0x...: port 2 written at +0x12; display info filled (1920x1080, 1 mode(s), name "RDNA4FB PV", transactions via FIFO 0x06), online event pending (+0x100 bit 2)
host: stamp[0] = 2 written (FIFO page offset 0x0), INTR_STATUS_GPU bit 0 set, interrupt 0 raised
host: ch3 @0x1ff8: cmd 0x01 DisplaySetupSharedState, 20 bytes (8 payload), 0 barrier(s), signal 0x7: pipe port 5, shared state page 0x...   (across the child ring's wrap)
host: display shared state page 0x...: port 5 written at +0x12; display info filled (1920x1080, 1 mode(s), name "RDNA4FB PV", transactions via FIFO 0x06), online event pending (+0x100 bit 2)
host: stamp[3] = 7 written (FIFO page offset 0xc), INTR_STATUS_GPU bit 3 set, interrupt 0 raised
selftest: host consumed the FIFO (FIFO_READ caught up with FIFO_WRITTEN)   ok
selftest: host mapped the announced FIFO page                      ok
selftest: host decoded the four FIFO commands (three on the root FIFO, one on the child channel)   ok
selftest: host answered GetDeviceInfo once                         ok
selftest: GetDeviceInfo reply buffer holds the documented key/value pairs   ok
selftest: parseDeviceInfo (mirror): MSAASamples 4, GpuCoreCount 64, DeserializerVersion undefined, shader version defaulted to 2.2   ok
selftest: stamp[0] == 2 in the FIFO page (the last signal of the batch: GetDeviceInfo 1, display 2)   ok
selftest: display shared state page: port 2 written at +0x12 (what AppleParavirtDisplayPipe asserts)   ok
selftest: child channel 3 defined from the root page record (stamp index 3, ring of 2 pages)   ok
selftest: stamp[3] == 7 in the FIFO page (the child command's signal)   ok
selftest: child channel: DisplaySetupSharedState answered: port 5 written at +0x12   ok
selftest: child channel record: read counter caught up with write counter (0x200c)   ok
selftest: host raised interrupt 0 for both stamps (root 0, child 3) and the registered handler saw two   ok
selftest: INTR_STATUS_GPU bits 0 and 3 dropped again by the host   ok
selftest: raiseInterrupt(0): handler registered and enabled        ok
selftest: interrupt delivered once to the event source's action    ok
selftest: display info in the shared state page: port, 1920x1080 @ 60, cursor 64x64, FIFO 0x06, one mode   ok
selftest: online event (+0x100 bit 2) pending on both pipes        ok
selftest: INTR_STATUS_DISP quiet while no pipe is enabled          ok
host: display pipe 2: enabled mask 0x0 -> 0xc (enable())
selftest: INTR_STATUS_DISP bit 2 (pipe port 2) set once the pipe is enabled   ok
selftest: ... and only that bit (pipe 5 is not enabled)            ok
selftest: signalDisplay finds the online event and clears it       ok
selftest: INTR_STATUS_DISP bit 2 dropped once the event was taken  ok
selftest: the interrupt was raised for it                          ok
host: display pipe 5: enabled mask 0x0 -> 0x1
host: VBL event 1 on display pipe 5 (+0x100 bit 0)                 (events 1, 2, 3 are logged, then every 600th)
selftest: VBL: 5..20 events in 250 ms on the pipe that enabled VBL (60 Hz = 15)   ok
selftest: ... each raised INTR_STATUS_DISP bit 5                   ok
selftest: no VBL event on the pipe whose guest did not enable it   ok
selftest: <n> VBL events in 250 ms                                 (n about 15)
host: display pipe 5: enabled mask 0x1 -> 0x0
selftest: INTR_STATUS_DISP quiet again after VBL was disabled      ok
host: ch3 @0x...: cmd 0x02 DisplayProcessOnline, 20 bytes (8 payload), ...: pipe port 5, cookie 0x1
host: display pipe 5 acknowledged the online event (cookie 0x1, as filled)
host: ch3 @0x...: cmd 0x05 DisplayUpdateCursorState, ...: pipe port 5, cursor visible
host: ch3 @0x...: cmd 0x04 DisplayUpdateCursorGlyph, 56 bytes (44 payload), ..., signal 0x0: pipe port 5, task 3, mapping 0x7000, pitch 256, image 64x48 (3, 5), pixel sum 0x99
host: ch3 @0x...: cmd 0x06 DisplaySubmitTransaction, 24 bytes (12 payload), ..., signal 0x15: pipe port 5, surface 77, task 3
host: ch3 @0x...: cmd 0x07 DisplaySubmitTransaction2, 48 bytes (36 payload), ..., signal 0x16: pipe port 5, task 3, surface 77, gamma 0 entries (a 0x0, b 0x0, sum 0x0)
host: ch3 @0x...: cmd 0x1e DisplayFlushChannelEvent, 12 bytes (0 payload), ..., signal 0x17
host: stamp[3] = 23 written (FIFO page offset 0xc), INTR_STATUS_GPU bit 3 set, interrupt 0 raised
selftest: the pipe's commands were consumed from its channel ring   ok
selftest: stamp[3] == 23: the last signal of the batch (SubmitTransaction 21, SubmitTransaction2 22, FlushChannelEvent 23)   ok
selftest: host counted 2 transactions, 2 cursor commands, 1 flush and the online acknowledgement   ok
host: display pipe 5 cursor: position (10, 20), visible
selftest: host noticed the cursor position and visibility in the shared state   ok
interrupt 0 unregistered
selftest: host dropped its FIFO, root page, channel and display pipe mappings when the guest cleared the registers   ok
selftest: PASS
```
Between them `host: ctrl+0x... a -> b` lines show the register writes the host saw (0x034, 0x004, 0x010, 0x000, 0x030). Any `FAIL` line names the check. A guest panic is data: send the serial tail. [measured: PASS in runs 2 and 3; the display lines are **code**, not yet run: hub-task-451 commit d4d053b]

**B. Apple's kext, in a full-install VM** (`rdna4-pvgpu=1`; coordinate with Anvil, hub-task-427; `tools/m0/vm-check.sh` collects items 2-4 over ssh. Anvil's VM, measured status 2026-10-01: being installed, `tools/emu-full.sh oc --kext <kext> --args "... rdna4-pvgpu=1"`, serial log like Kiln's, **no emulated RDNA4 device yet**, so the nub is published by the fallback: `RDNA4PvNub::publishLater` publishes it under the platform expert 25 s after plugin start when the GPU path never ran): the checks, in order:
1. Our log: `pvgpu: fake Apple paravirtual GPU published ...`, and a `match offered: com.apple.driver.AppleParavirtGPU AppleParavirtGPUControl (IOPCIMatch 0xEEEE106B) -> MATCH` line. No such line = the catalogue never offered Apple's personality (fallback (a) above).
2. `ioreg -l -w0 -n PVGPU` shows the nub with `compatible pci106b,eeee`; `ioreg -c AppleParavirtGPUControl` shows Apple's class attached; `kextstat | grep Paravirt` (or `kmutil showloaded`) lists `com.apple.driver.AppleParavirtGPU`.
3. Our log: `host: ctrl+0x034 0x00000000 -> 0x00000006` (version handshake), `host: FIFO announced: ...` (setupFIFO), `host: fifo @0x...: cmd 0x.. <name>, N bytes ...` (the commands by name). Which of these appear is the M0 result.
4. `host: GetDeviceInfo answered` in our log and `ioreg -c AppleParavirtAccelerator` / `ioreg -c IOAccelerator` showing the accelerator with `MetalPluginName AppleParavirtGPUMetal`: `setupDeviceInfo` completed; what to expect after that is under "What the kext does next".

## The GetDeviceInfo reply (hub-task-446) [measured in the kext and the bundle unless marked]

**The wait.** `AppleParavirtAccelerator::setupDeviceInfo` (the last step of `Accelerator::start`) wires a 4 KiB `IOBufferMemoryDescriptor`, builds FIFO command `0x3a` with one record `{u32 0x2d, u32 bytes/8 (0x200), u32 physical page}`, submits it on the root channel through the generic `IOAccelChannel2` path, and then **`IOAccelEventMachineFast2::finishEvent`** waits for the event of that submission. After the wait it calls `parseDeviceInfo(buffer, 0x200, &accelerator->deviceInfo)` and returns true whatever the content (no validation: even an all-zero buffer passes). Then `Accelerator::start` releases its lock and `registerService()`.

**The completion protocol (stamps).** The event machine is `IOAccelEventMachineFast2`/`IOAccelEventMachine2` with the paravirt subclass. Each channel index `i` (root = 0, Exec 1, Immediate 2, Uploads 3, Downloads 4, display pipes after them) has a **stamp**: a u32 at `stampBase + 4 * i` (`getStampOffset(i) = 4 * i`), where `stampBase` is the **first page of the FIFO buffer** (`getStampBaseAddress()`: the 4 KiB the driver zeroed before the ring). On every submission the machine increments its issued counter for the channel and writes it into the command header's `signal` field (`writeStamp` -> `AppleParavirtCommandAllocator::addSignal`, dword at command +8). The event is complete when `(int32)(issued - *stampPtr[i]) <= 0`, i.e. **when the host stores the command's `signal` value into stamp slot `i`**. `waitForStamp` sleeps with a **1 s deadline**, re-reads the stamp after every wake-up, and after the second timeout logs `timed out waiting for stamp` and returns `kIOReturnTimeout`, whereupon `finishEvent` runs `handleFinishChannelRestart` and **retries without end**. The interrupt only shortens the wait: the handler (`setupInterrupts` block) reads `INTR_STATUS_GPU` (ctrl+0x18), calls `signalStamps(mask)` (one `signalStamp(i)` per set bit), then handles `INTR_STATUS_DISP` (ctrl+0x14) and `INTR_FAULT` (ctrl+0x2c). **Our host therefore does, per FIFO batch: write the reply, `stamp[0] = last signal` (monotonic compare), set `INTR_STATUS_GPU` bit 0, raise interrupt 0, drop the bit one tick later** (the register clears on read in the real device; RAM cannot).

**The reply layout.** The buffer is an array of **512 pairs `{u32 key, u32 value}`** (the parser always walks `bytes/8` pairs). `parseDeviceInfo` has a jump table of 45 entries: keys 1..0x2a and 0x2c each store `value` into a field of the 224-byte `APVDeviceInfoStruct` and set that field's "defined" byte; **keys 0 and 0x2b (and anything above 0x2c) are ignored**, so a zero-filled remainder is harmless and the order does not matter. After the loop the parser post-processes `MaxMetalShaderVersion` (undefined: set to `0x20002`; defined: values `>= 0x20008` are cut to `0x20007`) and splits it into major/minor at `+0xd8/+0xdc`. The struct is then copied to the accelerator (`+0xe68`) and returned **together with 60 bytes of version-derived feature flags** (`AppleParavirtShared::getDeviceInfo`, user-client selector 0x105, 284 bytes) to the Metal bundle, which keeps the first 224 bytes as `AppleParavirtDevice._deviceInfo` and the rest as `_features` (`-[AppleParavirtDevice setupDeviceInfo]`). The field names and offsets are the bundle's own type description of `_deviceInfo` (`tools/m1/objctype.py`), the key-to-field mapping is read from the kext's jump table (`tools/m0/gen-devinfo.py` -> `src/pvdevinfo.inc`).

**What we answer (`src/pvstream.cpp`, `kReplyValues`).** Values that are properties of the card come from the card; the rest is the most conservative value the parser accepts, and everything the driver can default is left out. `GpuCoreCount` is **64** (RX 9070 XT, Navi 48) from the product specification, not read from the card.

| key | field (struct offset) | value | why |
|---|---|---|---|
| 0x01 | MSAASamples (+0x00) | 4 | max sample count (counts 1,2,4,8,16 are valid, supported iff n <= value): conservative, the card does 8 |
| 0x02 | D24S8Supported (+0x04) | 0 | no Depth24Stencil8: conservative (D32S8 exists) |
| 0x03 | MaxThreadsPerThreadgroupW (+0x08) | 1024 | max threads per threadgroup, x: card (workgroup limit 1024 per dimension) |
| 0x04 | MaxThreadsPerThreadgroupH (+0x0c) | 1024 | max threads per threadgroup, y: card |
| 0x05 | MaxThreadsPerThreadgroupD (+0x10) | 1024 | max threads per threadgroup, z: card |
| 0x06 | MaxThreadgroupMemoryLength (+0x14) | 32768 | threadgroup memory length: conservative (the card's LDS is 64 KiB) |
| 0x07 | IsFramebufferReadSupported (+0x18) | 0 | no framebuffer read: conservative |
| 0x08 | IsRGB10A2GammaSupported (+0x1c) | 0 | no RGB10A2 gamma: conservative |
| 0x09 | SupportsNativeHardwareFP16 (+0x20) | 1 | native FP16: card (RDNA4 packed FP16) |
| 0x0a | DeserializerVersion (+0x24) | *left out* | left out on purpose: the Metal bundle then uses the oldest serializer (no reflection, shared-texture, OpenGL, IOSurface-rotation or corrected-base-vertex variants; they switch on at versions 3/5/6/7/8). M1 raises it when the decoder covers the version-gated variants |
| 0x0b | PrimtiveTypeSupport (+0x28) | 0x1f | primitive types: bit per MTLPrimitiveType point..triangle strip [INFER: bit layout] |
| 0x0c | SupportsMultiplaneTextures (+0x2c) | 0 | no multiplane textures: conservative |
| 0x0d | LinearTextureAlignment (+0x30) | 256 | linear texture alignment in bytes: conservative power of two (AMD uses 256) |
| 0x0e | HeapBuffers (+0x34) | 0 | no heap buffers: conservative |
| 0x0f | HeapBufferAlignment (+0x38) | *left out* | heaps are off |
| 0x10 | HeapTextures (+0x3c) | 0 | no heap textures: conservative |
| 0x11 | BufferFromIOSurface (+0x40) | 0 | no buffer-from-IOSurface: conservative |
| 0x12 | MaxMetalShaderVersion (+0x44) | *left out* | the driver defaults it to 0x20002 (Metal 2.2); a requested value >= 0x20008 is clamped to 0x20007 anyway |
| 0x13 | SupportsSharedTextures (+0x48) | 0 | no shared textures: conservative |
| 0x14 | MaxVertexAmplificationCount (+0x4c) | *left out* | no vertex amplification |
| 0x15 | SupportsProgrammableSamplePositions (+0x50) | 0 | no programmable sample positions: conservative |
| 0x16 | RasterizationRateLayerCount (+0x54) | *left out* | no rasterization-rate maps |
| 0x17 | TileShaders (+0x58) | 0 | no tile shaders: conservative |
| 0x18 | ImageBlocks (+0x5c) | 0 | no image blocks: conservative |
| 0x19 | RasterOrderGroups (+0x60) | 0 | no raster order groups: conservative |
| 0x1a | MemoryOrderAtomics (+0x64) | 0 | no memory-order atomics: conservative |
| 0x1b | LargeMRT (+0x68) | 0 | no large MRT: conservative |
| 0x1c | SupportFlags2023.value (+0x6c) | 0 | SupportFlags2023 all clear: conservative |
| 0x1d | MaxTotalComputeThreadsPerThreadgroup (+0x70) | 1024 | max total compute threads per threadgroup: card |
| 0x1e | MaxComputeLocalMemorySizes (+0x74) | *left out* | unknown consumer |
| 0x1f | MaxComputeThreadgroupMemory (+0x78) | 32768 | max compute threadgroup memory: conservative (same as 0x06) |
| 0x20 | MaxComputeThreadgroupMemoryAlignmentBytes (+0x7c) | 16 | threadgroup memory alignment in bytes: conservative |
| 0x21 | SupportFlags2024.value (+0x80) | 0 | SupportFlags2024 all clear (no argument buffers, no command-buffer jump...): conservative |
| 0x22 | GpuCoreCount (+0x84) | 64 | GPU core count: card (compute units) |
| 0x23 | MaxTextureLayers (+0x88) | 2048 | max texture layers: Metal's 2D-array limit |
| 0x24 | MaxPredicatedNestingDepth (+0x8c) | *left out* | unknown consumer |
| 0x25 | HostGPUFamilyClamped (+0x90) | *left out* | unknown consumer |
| 0x26 | ArgumentBuffersTier (+0x94) | *left out* | argument buffers off (SupportFlags2024 clear) |
| 0x27 | ArgumentBuffersMaxSamplerCount (+0x98) | *left out* | argument buffers off |
| 0x28 | MinimumLinearTextureAlignment (+0x9c) | 256 | minimum linear texture alignment: conservative (same as 0x0d) |
| 0x29 | SupportedTextureWriteRoundingModes (+0xa0) | *left out* | unknown consumer |
| 0x2a | SupportFlags2025.value (+0xa4) | 0 | SupportFlags2025 all clear (no ICBs): conservative |
| 0x2c | HostGPUFamilies.value (+0xa8) | *left out* | no host GPU family claimed |

How the consumers use some of them [measured in the bundle]: `supportsSampleCount:(n)` is true for n in {1,2,4,8,16} with `n <= MSAASamples`; `isDepth24Stencil8PixelFormatSupported` is `D24S8Supported != 0`; `maxThreadsPerThreadgroup` is `{W,H,D}`; `maxThreadgroupMemoryLength` is the +0x14 dword; `initWithAcceleratorPort:` builds the serializer with `initWithDevice:objectRefAllocator:deserializerVersion:` only when `DeserializerVersionDefined`, otherwise the version-less initialiser. **[INFER]** for the bit layout of `PrimtiveTypeSupport` and the meaning of `HostGPUFamilyClamped`/`HostGPUFamilies`; the Metal frameworks read many of these later (M1/M2), M0 only needs the kernel not to wait.

**Host test and self-test.** `tools/pvstream-test.cpp` builds the reply, runs a mirror of Apple's parser over all 512 pairs and checks the fields, the defaults and the shader-version clamp. The self-test (level 2) additionally sends the three commands below through the ring (one across the wrap) and checks the reply bytes, the parsed struct, `stamp[0]`, the interrupt and the cleared status bit.

## What the kext does next, and where it will stall (to judge the first VM run)

In the order they should happen after the reply (`rdna4-pvgpu=1` in Anvil's full-install VM; expected host log lines first):

1. **Our log:** `host: fifo @0x...: cmd 0x3a GetDeviceInfo, 24 bytes ... reply buffer page 0x..., 4096 bytes` then `host: GetDeviceInfo answered: ... 31 pairs written, GpuCoreCount 64` then `host: stamp[0] = N written ... interrupt 0 raised`. Not seen: the driver did not get that far (look at the earlier steps in Tests B).
2. **`Accelerator::start` returns true and `registerService()` publishes the accelerator**: `ioreg -c AppleParavirtAccelerator` shows it with `MetalPluginName AppleParavirtGPUMetal` and `MetalPluginClassName AppleParavirtDevice`; `IOAccelerator` matching now finds it. Nothing in `Accelerator::start` waits after this.
3. **The display pipe sets itself up (child-ring polling, hub-task-447; display side, hub-task-451, details in `docs/m1-stream.md` s.1b).** `AppleParavirtGPUControl` is an `IOFramebuffer` (display count register +0x22c = 0 is taken as one pipe). `AppleParavirtDisplayPipe::init` creates its **own virtual channel**, number `port + 5` (5 for port 0), with a **one-page ring** [measured: `VirtualChannel::init(accel, port + 5, 8, 0x90, 0x480, "Display<port>", 0x1000, port + 5)`]: our log shows `host: root page 0x... mapped`, `host: fifo @...: cmd 0x30 DefineChannel ... channel 5` and `host: channel 5 defined: stamp index K, ring of 1 pages (4096 bytes) from page list 0x...` (K = 5 if the record's `kind` is the channel number [INFER]); then `setupSharedState` sends `0x01 DisplaySetupSharedState {u32 port, u32 page}` on that ring and waits for its stamp: `host: ch5 @...: cmd 0x01 DisplaySetupSharedState ... pipe port 0`, `host: display shared state page 0x...: port 0 written at +0x12`, `host: stamp[K] = V written ..., INTR_STATUS_GPU bit K set`. The driver then asserts `fSharedState->port == fPort` (u16 at +0x12; a mismatch would panic the kernel) and reads the u32 at +0x1c (0: transactions are FIFO 0x06). **If the log shows the `DefineChannel` but no `channel N defined` line, the record or the page list was not what the host expects** (the host logs the record words when the page list is missing); if it shows the channel defined but no command, the guest did not ring the doorbell or the ring is empty; if Apple logs `timed out waiting for stamp` the stamp index is not the record's `kind` as assumed.
   **The display side after setup [measured in the kext, hub-task-451].** Only `setupSharedState` waits. What follows needs the host in two ways, and the first brief had one of them wrong: **(a) bits 2 and 3 of the pending word (+0x100) are display *online/offline*, not transaction events.** Without an online event the pipe never calls `connectionChange`: **no display is ever reported to IOFramebuffer** (no connection, no EDID, no mode): this is the real next stall, and `rdna4-pvgpu-disp=1` (or 3) removes it: after the answer to 0x01 the host fills the display info (name, 1920x1080, one mode at 60 Hz, cursor 64x64, FIFO 0x06) and sets +0x100 bit 2; the guest's `enable()` stores the enabled mask 0xc and signals its own pending bits, so `process_online` runs, builds the mode list and an EDID, sets `ParavirtDisplayPrefs`, and sends **0x02 DisplayProcessOnline {port, cookie}** (our log: `display pipe N acknowledged the online event`). Log lines to expect in order: `host: display pipe N: enabled mask 0x0 -> 0xc (enable())`, `host: chK @...: cmd 0x02 DisplayProcessOnline`, `host: display pipe N acknowledged the online event (cookie 0x1, as filled)`. **(b) VBL for the accelerator side** (`enableVBLInterrupt`, +0x104 bit 0): the host sets +0x100 bit 0 once per ~16.7 ms and asserts `INTR_STATUS_DISP` bit `port` (level-triggered until the guest's `signalDisplay` takes the pending word): `host: display pipe N: enabled mask 0xc -> 0xd`, `host: VBL event 1 on display pipe N (+0x100 bit 0)`. The framebuffer's *own* `'vbl '` interrupt (what WindowServer paces on) is a guest-side timer and needs none of this. **Transaction completion needs no display-specific signal**: `SubmitTransaction` (0x06 `{port, surface, task}` or, when the host sets +0x1c, 0x07 with the gamma table) is attached to a fence that completes when the pipe channel's stamp reaches the command's `signal`: the generic stamp store the host already does after every batch (`stamp[K] = signal`, `INTR_STATUS_GPU` bit K). The commands are logged with their fields (`cmd 0x06 DisplaySubmitTransaction ... surface S, task T`); `host: ctrl+0x220 (cursor kick)` and `display pipe N cursor: position (x, y), visible` show the hardware-cursor path (the driver negotiates version 6, which selects it).
4. **User space, if Metal runs in the VM:** `-[AppleParavirtDevice initWithAcceleratorPort:]` runs the generic `MTLIOAccelDevice` init (E1c: type 5/6 selectors, served by Apple's own kernel classes now), then `setupDeviceInfo` (selector 0x105: our values + the feature flags), `setupCompiler` (`libAppleParavirtCompilerPlugin`), `setupResourcePools`; the first `newCommandQueue` and the first command buffer produce Exec-ring traffic (`0x37 ExecIndirect`) that the host cannot see yet either.

So **"M0 passed" = the log shows `GetDeviceInfo answered`, `ioreg` shows the accelerator, the Display channel is defined and its `DisplaySetupSharedState` answered (`stamp[K]` line), and nothing waits any more inside the driver**; the **display side** is a second run of the same kind with `rdna4-pvgpu-disp=3` (the `acknowledged the online event` line, `ioreg -c AppleParavirtFramebuffer` / `IODisplayConnect` showing a connection with the 1920x1080 mode and the registry property `ParavirtDisplayPrefs`; VBL lines when something enables VBL; transactions logged and completed by stamps once WindowServer attaches); what is still missing after that is everything user-space (item 4). A panic instead means one of: an assertion on a reply (items 3/4), a BAR/config access the nub does not model, or the interrupt path (`registerInterrupt` for index 0 must have been logged before the first stamp).

## Open items / unknowns

- Whether the unresolved symbols and the vtable patch are acceptable to OpenCore's injection (the self-test in Recovery answers it: the kext must still load and publish).
- `deviceMemoryRead`'s receiver, and every IOPCIDevice virtual Apple's `IOFramebuffer` base class calls on the provider (none seen in our scan, but the base class is in IOGraphicsFamily, outside the paravirt kext): the nub implements all header virtuals, so an unexpected call returns a harmless value rather than crashing; the log of the VM run would show the driver stopping.
- Whether `AppleParavirtGPUControl::start` behaves with `FramebufferCount 0` and a display count of 0 on a machine that already has a framebuffer (ours). Worst case in the VM: a second, empty display adapter.
- `IOPCIDevice` instance size on Tahoe (logged at publish).

## Tools

- `tools/m0/pvdis.py list|dis|xref|vt`: functions and disassembly of the paravirt kext with imports resolved to names (branch stubs and GOT through the collection's chained fixups), rip-relative strings annotated, virtual calls annotated with Tahoe's `IOPCIDevice` slot names. Needs `~/work/tools/macos-full` (Apple's collections stay outside the repo).
- `tools/m0/check-vtable.py <clang -fdump-vtable-layouts output>`; `tools/m0/vtprobe.cpp` is the class to dump (compile with the kext flags and `-Xclang -fdump-vtable-layouts -c`).
