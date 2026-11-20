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
