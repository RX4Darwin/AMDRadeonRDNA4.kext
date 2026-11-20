# The native driver without the paravirtual layer: plan for steps 2 and 3

Status 2026-11-20. Step 1 (apps on the native Metal device) is in `docs/native-metal-spike.md`: Safari's own process runs on it; Maps
runs with its map black until stencil and MSAA. This file is about the two steps after it:

- **Step 2, our own display path:** presents, VBL events, the hardware cursor and mode changes, today done by Apple's paravirtual display
  driver (`AppleParavirtGPU`, its display pipes) talking to the kext's paravirtual host.
- **Step 3, our own accelerator:** the `IOAccelerator` service macOS needs before Metal, IOSurface, CoreDisplay and WindowServer use a
  GPU, today Apple's paravirtual accelerator on the kext's fake PCI device.

They are one piece of work: WindowServer presents through its accelerator's display pipes (`IOAccelDisplayPipeUserClient2`,
`IOAccelDisplayPipeTransaction2`), so a display path of our own is part of an accelerator of our own.

## What it is

A driver of the IOAcceleratorFamily2 family, as Apple's for Navi (RDNA 1/2), `AMDRadeonX6000.kext` with `AMDRadeonX6000Framebuffer.kext`.
Both are in the KDK (`/Library/Developer/KDKs/KDK_26.5.2_25F84.kdk`) with full symbols (some 4000 exported functions), a reference
for every class to subclass:

| IOAcceleratorFamily2 base | AMD's (AMDRadeonX6000_...) | What it is for us |
|---|---|---|
| `IOGraphicsAccelerator2` | `AMDGraphicsAccelerator`, `AMDNavi21GraphicsAccelerator` | the service; publishes `MetalPluginName` = our bundle |
| `IOAccelDevice2`, `IOAccelShared2`, `IOAccelSharedUserClient2` | `AMDAccelDevice`, `AMDAccelShared(UserClient)` | Metal's connection: could stay our runtime client (the bundle already speaks it) |
| `IOAccelCommandQueue`, `IOAccelContext2`, `IOAccelEventMachine2` | `AMDAccelCommandQueue`, `AMDAccelEventMachine` | submits and stamps, if Metal's own path is used |
| `IOAccelMemory`, `IOAccelVidMemory`, `IOAccelSysMemory`, `IOAccelResource2` | `AMDAccelVidMemory`, ... | memory, IOSurface backing |
| `IOAccelDisplayMachine`, `IOAccelDisplayPipe`, `IOAccelDisplayPipeTransaction2` | `AMDAccelDisplayMachine`, `AMDAccelDisplayPipe`, `AMDDCNDisplay` | **step 2**: WindowServer's presents, VBLs, cursor, gamma, onto our DCN code (mode sets, hot plug, flips, cursor: in the kext today) |

The headers are private: the classes' layouts and virtual methods come from the KDK's symbols and AMD's kext (reverse engineering, as
for the paravirtual host's protocol). The kext's DCN, ring, VM and memory code is what these classes call.

## The blocker: where the code may load

Our kext is injected by OpenCore into the **boot** kernel collection. IOGraphicsFamily and IOAcceleratorFamily2 are in the **system**
kernel collection (checked on the rig: `kmutil inspect -B` lists IOPCIFamily but not IOGraphicsFamily; IOGraphicsFamily loads at
0xffffff7f..., our kext at 0xffffff80...). A boot-collection kext cannot link to them, so it cannot subclass `IOAccelerator` or
`IOGraphicsAccelerator2` (that is also why step 1 reached sandboxed apps through a hook on the paravirtual accelerator, not with a
service of our own). Two ways out, both changing how the Mac boots:

- **A. OpenCore `Kernel -> Force`:** load IOGraphicsFamily, IOAcceleratorFamily2 and what they need (IOSurface, IOReportFamily,
  AppleMobileFileIntegrity's KPI, ...) into the boot collection, so that our kext (or a second one beside it) can link to them. A
  config.plist change; a mistake is a boot that does not come up (recovery: the EFI's previous config, as for other OpenCore changes).
- **B. A second kext in `/Library/Extensions`:** it goes into the auxiliary kernel collection, which may link to the system one. macOS
  asks to approve it in System Settings > Privacy & Security, then a reboot; updates go through `kmutil`. Its code runs late (after
  WindowServer may have started), so the paravirtual layer stays for the boot.

### A is not possible on Tahoe; D instead (2026-11-20)

`Kernel -> Force` loads a kext's executable from the system volume, and since macOS 11 the families have none there: on the rig
`/System/Library/Extensions/IOGraphicsFamily.kext`, `IOAcceleratorFamily2.kext`, `IOSurface.kext`, `IOReportFamily.kext` and
`IOPCIFamily.kext` have no `Contents/MacOS` (their code is only inside the kernel collections). Injecting the KDK's copies into the boot
collection would not help either: every system-collection kext that uses them (the paravirtual driver, IONDRVSupport, the wrangler, ...)
is prelinked against the system collection's copies.

- **D. Derive at run time (chosen, needs no boot change and no approval).** The base classes are declared from the KDK
  (`tools/gen/accelgen.py` -> `src/ioaccel.hpp`: vtable order and instance size; Tahoe's IOService vtable has the SDK's 266 slots).
  Every base function our classes reach is a thunk in our kext (`src/ioaccel_thunks.S`), a jump through a table that
  `src/rdna4accel.cpp` fills with Lilu's `solveSymbol` when IOAcceleratorFamily2 loads, after checking each virtual against the
  running vtable's slot and the class size. The base's `gMetaClass` is a placeholder; our metaclass's superclass link (+16) is set to the
  real one then. Our classes' metaclasses are written out, without the `superClass` pointer symbol that
  `OSDefineMetaClassAndStructors` makes: OpenCore's linker patches every class with one against its superclass's vtable, which must be
  one it linked itself (only the kext's own patched vtables and its dependencies count); a copy of the base's vtable in our kext did not
  count, and the first try made OpenCore drop the whole kext (the rig booted without it; restored from `~/rdna4/kext.prev`).
  Checked with the build: the base's vtable compiled from `src/ioaccel.hpp` equals the KDK's slot for slot.
- Milestone 1 under D: `debug.rdna4_accel=1` (a sysctl, not a boot-arg: a mistake costs one boot, not every boot) makes an
  `RDNA4Accelerator` under the GPU with `IOService::start` only, user clients refused, the 17 required functions empty.
- **Card, 2026-11-20 (Tahoe 26.6.2, kext 80b30b47):** the kext loads with the accelerator in it; at IOAcceleratorFamily2's load all 84
  thunks are found and all 83 checked virtuals sit in the KDK's (26.5.2) slots. The family's metaclass reads size 0 at that point (its
  static constructors have not run yet): the size is checked when one is made. **`debug.rdna4_accel=1` panicked the rig** (no reboot
  after it: it looked like a hang). The panic report: a page fault in our sysctl handler itself, before anything was made: `extern
  IOService *gNativeGpu` named a variable in accelcensus.cpp's anonymous namespace, so it linked as an unresolved lookup that OpenCore
  bound to junk. Fixed (`RDNA4AccelCensus::gpu()`), and the kext build now fails if any of our own globals is left undefined. The
  skeleton itself has not run yet. The boots after it: two warm restarts failed the compute bring-up at SDMA's test write (as once
  before after a warm restart), the cold boot hit the SimpleHDA race (IONDRVSupport loaded unseen, the GPU never attached).
- **MILESTONE 1 MET ON THE CARD (2026-11-20, kext cf910523):** `debug.rdna4_accel=1` makes one (the family's real constructor runs
  through the thunk) and attaches it under the GPU (`VGA`): `metaCast("IOGraphicsAccelerator2")` yes; `=2` starts and publishes it:
  `ioreg -c IOAccelerator` and `-c IOGraphicsAccelerator2` list `RDNA4Accelerator` beside AppleParavirtAccelerator; the desktop kept
  running, nobody asked it for a user client. (A boot with the UR22 plugged in never got a desktop: XH00 re-enumerated it in a loop and
  WindowServer waited in IOKitWaitQuiet: USB, not graphics.)
- **The family's own `start` runs on the card (2026-11-20, kext c8e05023):** `debug.rdna4_accel=2` after `=1`: `IOGraphicsAccelerator2::start`
  succeeds with our event machine (an `IOAccelEventMachineFast2` subclass; the family's init), display machine (`IOAccelDisplayMachine`
  subclass, kept, not started), kernel task (`IOAccelTask` subclass, `init(this, 0, nullptr)`), `configureDevice` true and the family's
  default config; it sets IOAccelRevision 2, AccelCaps, PerformanceStatistics, IOCFPlugInTypes; it does not register the service
  (AMD's driver does that after it). System stable. On the way: in `-fapple-kext` code `Base::f()` is a call through the base's vtable
  symbol, so every family class's vtable must be emitted in our kext (key function `getMetaClass`, `RDNA4_ACCEL_BASE`): undefined, it
  was bound to junk and panicked; the build now fails on any undefined IOAcceleratorFamily2 symbol.
- **The display machine runs on the card (2026-11-20, kext cce7c390):** `debug.rdna4_accel=3`: `IOAccelDisplayMachine::init` and
  `start(IOPCIDevice*)` walk the GPU's framebuffers and `found_framebuffer` makes a family `IOAccelDisplayPipe` for one: the pipe's
  init asks the accelerator for a resource (`newResource`: an `IOAccelResource2` subclass, its seven functions stubs; nil would have
  panicked: no check), registers a framebuffer notification and sets `IOAccelTypes` (our accelerator's path), `IOAccelIndex` 0 and
  `IOAccelRevision` on that IONDRVFramebuffer: how WindowServer ties a display to its accelerator. The other five: "trying to init too
  many framebuffers": the config's display count (accelerator +0xc88 config, +0x28; AMD sets its display count) defaulted to 1; now 8.
  With 8 (kext 8dc2bab3): all six IONDRV framebuffers get a pipe (IOAccelIndex 0-5), desktop unaffected. `IOAccelDisplayMachine::init`
  and `start` return nothing (rax holds the lock's address): judge them by the framebuffers' properties, not their "result".
  The "IOAccelResource2::map(): resource already has a map" faults in the log are the paravirtual path's (thousands a boot without ours).
- Next: WindowServer on these framebuffers through our pipes: today the kext hides the IONDRV framebuffers from it (the paravirtual
  displays are its displays) and our accelerator refuses user clients (`IOAccelDisplayPipeUserClient2` and the rest); the family's
  IOAccelLegacyDisplayMachine / IOAccelLegacyDisplayPipe implement flips through a plain framebuffer and may be the shortest way.
- Was planned as milestone 2: the family's own `start` (from `RDNA4Accelerator::start`), with what it calls implemented: configureDevice (312),
  populateAccelConfig (310), createEventMachine/newEventMachine (272/273), newDisplayMachine (320), createKernelGPUTask (314),
  calcMaxGPUPhysicalMemoryBytes (311), newStatistics (336) and the memory/command-descriptor factories. Each object it asks for is a
  family class to derive the same way (IOAccelEventMachine2, IOAccelDisplayMachine2, IOAccelTask, ...): `tools/gen/accelgen.py`'s
  CLASSES list.

## Milestones

1. A skeleton `IOGraphicsAccelerator2` subclass loads (A or B) beside today's stack and publishes without anything matching it.
2. Its display machine and one display pipe on the boot display: WindowServer presents to it (our DCN flips), with the native Metal
   device as today. The paravirtual display path off for that display.
3. Both displays, hot plug, mode changes, cursor, sleep: what the paravirtual host does today, moved.
4. The service itself what Metal finds (`MetalPluginName` our bundle): Apple's paravirtual accelerator, and with it the fake PCI device
   and the PVGPU build, no longer needed for the native desktop. Apps on the native device need stencil, MSAA and the other features
   step 1 finds missing first.

Each is weeks, not days; 2 is where most of the unknowns are (the display pipe's protocol with WindowServer).

## Not this way

- Adapting Apple's `AMDRadeonX6000` to RDNA4 (as NootRX does for RDNA 2 cards Apple left out): its Metal side
  (`AMDRadeonX6000MTLDriver`) compiles for gfx10 and the hardware layers are gfx10/DCN 3; RDNA4 is gfx12 and DCN 4.0.1. Useful as a
  reference, not as the driver.
- Web pages on the native device: WebKit's GPU process maps code only from the sealed system volume, so our bundle would have to be in
  `/System/Library/Extensions` (SIP and the sealed system volume off). Independent of steps 2 and 3.

## Decisions needed (Sunneva)

1. A (OpenCore Force) or B (a kext in /Library/Extensions, approved in System Settings).
2. Whether to start the multi-week work now, or first finish step 1's features (stencil, MSAA) so more apps run on the native device.
3. Whether web content on the native device is worth SIP and the sealed system volume off (no, by default).
