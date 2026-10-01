# M0, kext side: the fake Apple paravirtual GPU (hub-task-427)

Branch `premetal/m0` (from `premetal/metal-spike`). Code: `src/pvgpu.{hpp,cpp}`, boot-arg `rdna4-pvgpu=1|2` (default off). Helpers: `tools/m0/pvdis.py` (read Apple's kext), `tools/m0/check-vtable.py` (is our `IOPCIDevice` the one Tahoe has).
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
- **Matching.** `matchPropertyTable` accepts **only** a personality that carries `IOPCIMatch` or `IOPCIPrimaryMatch` that matches 0xEEEE106B (with `IOPCISecondaryMatch`/`IOPCIClassMatch` honoured if present); every table offered is logged once (first 40) as `match offered: <bundle> <class> (IOPCIMatch ...) -> MATCH|no`.

## How Apple's kext gets matched (and why no personality injection is needed)

On a **full** install `AppleParavirtGPU.kext` is in `SystemKernelExtensions.kc`: its personality is in the IOKit catalogue from boot and the kext is loaded when a provider matches (the ordinary mechanism for any system kext). The OpenCore restriction of `docs/metal-spike.md` s.9.7 is about **linking** our kext against collections that are not the boot collection, not about matching: we link only against the boot collection (`IOPCIFamily`, `kernel`), and our nub is a plain `IOService::registerService()`. So **the route is: publish the nub, let the catalogue match.**
Fallbacks if the catalogue does not offer the personality (checked from the `match offered` log lines, none for `AppleParavirtGPUControl`): (a) `IOCatalogue::addDrivers` with a copy of Apple's personality (MetalCyan's technique, Lilu has the pattern; needed in Recovery, where the kext is not present at all, so there it cannot work); (b) none needed otherwise. [inferred until the VM run]

## Safety

- Default off, **emulator/VM only**. Never in a real-card boot: with Apple's driver matched, `AppleParavirtGPUControl` is an `IOFramebuffer` and would present a second display adapter with no modes next to the card's display (WindowServer would see it). The boot-arg is not in `tools/set-boot.sh`.
- No GPU access of any kind: the nub only touches its own RAM, the host reads the FIFO pages the guest announces. Reading a physical page announced by a guest driver is the one place where a wrong value could touch foreign memory: the length is limited to 1 MiB and the page must come with `CONTROL_FIFO = 1`.

## Tests

**A. Self-test, no Apple kext, in Kiln's emulated Recovery** (level 2: the kext plays the guest driver against the nub):
`KEXT=~/work/rx4darwin/RDNA4FB-m0/build/RDNA4FB.kext tools/emu-linux.sh 1 --extra 'rdna4-pvgpu=2'`, then read `rdna4fb.log`. Expected lines (all prefixed `RDNA4FB: pvgpu:`):

```
fake Apple paravirtual GPU published under <GPU name>: PCI 106b:eeee, BAR0 16384 bytes of RAM (control block at +0x1000), level 2; IOPCIDevice is N bytes here (header M)
selftest: start
selftest: config space, capabilities, personality matching        ok
selftest: mapDeviceMemoryWithRegister(0x10)                        ok
selftest: driver's mapping and the host's pointer are the same RAM  ok
selftest: version handshake: 6 written, 6 read back                ok
selftest: FIFO buffer (0x10000 bytes, options 0x890) allocated and wired   ok
host: FIFO announced: page 0x..., length 0x10000, ring start 0x1000, root page 0x0, version 6
host: fifo +16 bytes at ring offset 0x...: 10000000dec0adde0102030405060708     (the command written across the ring's wrap)
selftest: host consumed the FIFO (FIFO_READ caught up with FIFO_WRITTEN)   ok
selftest: host mapped the announced FIFO page                      ok
selftest: host logged at least one packet                          ok
interrupt 0 registered
selftest: IOInterruptEventSource on the nub (index 0)              ok
selftest: raiseInterrupt(0): handler registered and enabled        ok
selftest: interrupt delivered once to the event source's action    ok
interrupt 0 unregistered
selftest: host dropped its FIFO mapping when the guest cleared it  ok
selftest: PASS
```
Between them `host: ctrl+0x... a -> b` lines show the register writes the host saw (0x034, 0x004, 0x010, 0x000, 0x030). Any `FAIL` line names the check. A guest panic is data: send the serial tail. [code, not yet run]

**B. Apple's kext, in a full-install VM** (`rdna4-pvgpu=1`; coordinate with Anvil, hub-task-427): the checks, in order:
1. Our log: `pvgpu: fake Apple paravirtual GPU published ...`, and a `match offered: com.apple.driver.AppleParavirtGPU AppleParavirtGPUControl (IOPCIMatch 0xEEEE106B) -> MATCH` line. No such line = the catalogue never offered Apple's personality (fallback (a) above).
2. `ioreg -l -w0 -n PVGPU` shows the nub with `compatible pci106b,eeee`; `ioreg -c AppleParavirtGPUControl` shows Apple's class attached; `kextstat | grep Paravirt` (or `kmutil showloaded`) lists `com.apple.driver.AppleParavirtGPU`.
3. Our log: `host: ctrl+0x034 0x00000000 -> 0x00000006` (version handshake), `host: FIFO announced: ...` (setupFIFO), `host: fifo +N bytes ...` (first commands). Which of these appear is the M0 result.
4. `ioreg -c IOAccelerator` showing an Apple paravirt accelerator with `MetalPluginName AppleParavirtGPUMetal` means `setupDeviceInfo` completed, i.e. the host answered, which M0 does not do.

## What M0 should show, and the likely first stall

The go gate of `docs/metal-phase-plan.md` M0: Apple's driver gets through `configureDevice`, `setupMMIO`, `setupVersion`, `setupFIFO/Root` and writes to the FIFO. With the above nothing in those steps needs the host to act, so [inferred] they should pass and the first FIFO command (`GetDeviceInfo`, id 0x3a, then waits) should appear in our log. Then the driver waits for the reply that `setupDeviceInfo` parses; whether that wait is bounded, and what `AppleParavirtAccelerator::start` does on timeout (it returns false, `stop`), is unknown. That reply (the `APVDeviceInfoStruct` blob and the barrier/signal the command carries) is the first thing M1 must produce; its layout is in `parseDeviceInfo` (disassemblable with `tools/m0/pvdis.py dis parseDeviceInfo`) and in reims-vgpu's documentation (read only, LGPL: clean-room per the decision).

## Open items / unknowns

- Whether the unresolved symbols and the vtable patch are acceptable to OpenCore's injection (the self-test in Recovery answers it: the kext must still load and publish).
- `deviceMemoryRead`'s receiver, and every IOPCIDevice virtual Apple's `IOFramebuffer` base class calls on the provider (none seen in our scan, but the base class is in IOGraphicsFamily, outside the paravirt kext): the nub implements all header virtuals, so an unexpected call returns a harmless value rather than crashing; the log of the VM run would show the driver stopping.
- Whether `AppleParavirtGPUControl::start` behaves with `FramebufferCount 0` and a display count of 0 on a machine that already has a framebuffer (ours). Worst case in the VM: a second, empty display adapter.
- `IOPCIDevice` instance size on Tahoe (logged at publish).

## Tools

- `tools/m0/pvdis.py list|dis|xref|vt`: functions and disassembly of the paravirt kext with imports resolved to names (branch stubs and GOT through the collection's chained fixups), rip-relative strings annotated, virtual calls annotated with Tahoe's `IOPCIDevice` slot names. Needs `~/work/tools/macos-full` (Apple's collections stay outside the repo).
- `tools/m0/check-vtable.py <clang -fdump-vtable-layouts output>`; `tools/m0/vtprobe.cpp` is the class to dump (compile with the kext flags and `-Xclang -fdump-vtable-layouts -c`).
