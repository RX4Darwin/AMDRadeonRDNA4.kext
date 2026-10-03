# A second display under the Lilu / IONDRVFramebuffer design: is it possible?

Read-only source review, 2026-10-03. Nothing was built or booted. Tags: **[F]** fact with a citation, **[I]** inference
from facts, **[G]** guess.

Question: the plugin (`src/plugin.cpp`) lets Apple's `IONDRVFramebuffer` be the framebuffer and answers its NDRV
requests. Today that gives exactly one display. Can a second `IONDRVFramebuffer` exist for a second head, given that
the boot NDRV only knows the console?

Answer: **yes [I]**. IONDRVSupport has a multi-head mechanism, and the one thing that stops a second head is inside
`IONDRVFramebuffer::doDriverIO`, the function the plugin already routes. Section 6 lists what is still unknown.

Sources: Apple's open-source IOGraphics, `apple-oss-distributions/IOGraphics` at `7628538` (tag IOGraphics-600,
2026-04-17). `NDRV.cpp` below is `IONDRVSupport/IONDRVFramebuffer.cpp`, `FB.cpp` is
`IOGraphicsFamily/IOFramebuffer.cpp`, the plist is `Info-IONDRVSupport.plist`. The shipping binaries were not compared
with this source, except that the three personalities in macOS 26.5.2's `IONDRVSupport.kext/Contents/Info.plist` are
the same as in the source [F].

## 1. How IONDRVSupport does multi-head

- **[F]** `IONDRVDevice` is an `IOPlatformDevice` subclass commented "generic nub for multihead devices"
  (`NDRV.cpp:163-211`). Its `compareName` matches on the device-tree `name` property, its `getResources` resolves `reg`,
  and it joins the power tree of its device-tree parent.
- **[F]** Three personalities, all `IOClass IONDRVFramebuffer`, all in match category `IOFramebuffer` (plist):
  1. `IOPCIDevice` + `IONameMatch display`, probe score 20000;
  2. `IOPlatformDevice` + `IONameMatch display`, probe score 20000 (this is what matches an `IONDRVDevice`);
  3. `IOPCIDevice` + `IOPCIClassMatch 0x03000000&0xff000000`, probe score 0, no name match.
- **[F]** `IONDRVFramebuffer::start` branches on `gIONameMatchedKey` (`NDRV.cpp:341`). An instance that was NOT
  name-matched (personality 3) is the expander (`NDRV.cpp:341-493`):
  - properties of the form `@N,<key>` on the PCI device become device-tree children at location `N` carrying `<key>`
    (`:354-421`);
  - every first-level child whose `device_type` is `display` gets `IOFBDependentID` = the PCI device's registry entry ID
    and `IOFBDependentIndex` = 0, 1, ... (`:437-449`);
  - each child is wrapped in a new `IONDRVDevice`, given the PCI device's `IODeviceMemory` array, attached and
    registered (`:461-489`);
  - the instance then returns false (`:491-492`).
- **[F]** A name-matched instance takes the normal path (`NDRV.cpp:495-570`): `fNub` = its provider; `fDevice` = the
  device-tree parent when the provider is an `IONDRVDevice`, else the provider (`:537-539`); it copies
  `IOFBDependentID` / `IOFBDependentIndex` from the nub onto itself (`:545-549`).
- **[F]** `IOFramebuffer` groups framebuffers by `IOFBDependentID`: one `IOFBController` per ID, up to 32 framebuffers,
  slot = `IOFBDependentIndex` (`FB.cpp:387`, `:1466-1494`, `:9040-9056`). The controller is looked up or created in
  `start` (`copyController(kDepIDCreate)`, `FB.cpp:4813`); a framebuffer with no `OSNumber` ID gets none there
  (`:1470-1476`) and a controller of its own at `open` (`kForceCreate`, `:9019-9024`). At `open` the framebuffers that
  share an ID are linked into the `nextDependent` ring (`:9069-9104`).

## 2. Why we get one head today

- **[I]** On these PCs the PCI device's `name` property is `display` (the framebuffer attaches to the PCI device
  itself: `ndrv: framebuffer ... on provider VGA` in `docs/hw-logs/`, and `"IOName"="display"` in the standalone
  build's ioreg). Personalities 1 and 3 both match it, they share a match category, and personality 1 has the higher
  score, so it starts and the expander never runs.
- **[F]** `src/plugin.cpp` already allows for a framebuffer whose provider is a nub: `pciFor` tries the provider, then
  the provider's provider.

## 3. The obstacle: the boot NDRV refuses dependent index != 0

- **[F]** `IONDRVFramebuffer::doDriverIO` creates the NDRV lazily at `kIONDRVInitializeCommand`:
  `fNdrv = IOBootNDRV::fromRegistryEntry(fNub)` (`NDRV.cpp:1115-1147`). With no `fNdrv` every command returns
  `kIOReturnUnsupported`.
- **[F]** `IOBootNDRV::fromRegistryEntry` gives up when the nub's `IOFBDependentIndex` is non-zero, or when the device
  is a PCI device with a non-zero function number (`NDRV.cpp:4413-4418`). Otherwise it needs the console framebuffer
  address (`getConsoleInfo`) to lie inside one of the nub's device-memory ranges (`:4420-4453`).
- **[F]** So for a head with index 1: Initialize fails, `checkDriver` returns the error (`NDRV.cpp:1267-1293`),
  `enableController` logs "Not usable" (`:604-607`) and `IOFramebuffer::open` marks the framebuffer dead
  (`FB.cpp:9128-9138`).
- **[I]** `doDriverIO` is the routed function (`wrapDoDriverIO`). For a head-1 framebuffer the plugin can answer
  Initialize and Open with success and never call the original. It must then answer every csc request itself, because
  there is no `IOBootNDRV` behind it. What `IOBootNDRV` provides is small (`NDRV.cpp:4479-4616`): Initialize/Open
  succeed; `cscSetEntries` and `cscSetGamma` succeed and do nothing; `cscGetCurMode`, `cscGetNextResolution`,
  `cscGetVideoParameters`, `cscGetModeTiming` describe the one boot mode; everything else is unsupported.
  `Ndrv::Translator` already answers those four status calls and more, so a head-1 backend is the Translator plus
  "SetEntries / SetGamma succeed, the rest unsupported".

## 4. The other places that depend on the boot NDRV

`fNdrv` is used in three places besides `start`/`setProperties` housekeeping [F] (`grep fNdrv`):

| Use | With no `IOBootNDRV` | Consequence |
|---|---|---|
| `doDriverIO` (`NDRV.cpp:1123-1143`) | returns unsupported | section 3: the plugin answers instead |
| `findVRAM` (`:2042-2055`) | returns NULL, so `getVRAMRange` is NULL | both `IOFramebuffer` callers check for NULL (`FB.cpp:6378-6384`, `:10771-10783`); `IOFBMemorySize` is not published |
| `initForPM` (`:3977`) | `dozeOnly` then depends on `cscGetPowerState` | unsupported there also gives doze-only (`:3970-3979`): same as head 0 |

The framebuffer memory does not come from the NDRV object [F]: `getCurrentConfiguration` takes `csBaseAddr` from
`cscGetCurMode` (`1 | physical address`) into `physicalFramebuffer` (`NDRV.cpp:1938-1951`), and `getApertureRange`
makes a sub-range of the nub's device memory for it, falling back to a plain address range (`:1966-2040`).
**[I]** So head 1's surface must be at a physical address the CPU reaches through BAR0. With Resizable BAR off that
is the first 256 MiB of VRAM; a surface directly after the GOP framebuffer qualifies, which is where the cursor
sprite already goes (`initHardwareCursor`).

## 5. Two ways to create the second nub

**A. Let Apple's expander run.** Make the PCI device not name-match `display` and inject `@0,name`, `@0,device_type`,
`@1,name`, `@1,device_type`. Both heads then sit on `IONDRVDevice` nubs with the dependent properties set before
either framebuffer starts [F, section 1]. Head 0 still gets its `IOBootNDRV`: index 0 passes the check and the nub
carries the PCI device's memory ranges [I]. Cost: the working head moves onto a nub, and it depends on renaming the
PCI device (other kexts set that name to `display` on purpose).

**B. The plugin creates one nub (recommended) [I].** Leave head 0 exactly as it is. For head 1: a child registry
entry with `name` and `device_type` = `display`, `IOFBDependentID` = the PCI device's registry entry ID,
`IOFBDependentIndex` = 1; an `IONDRVDevice` made with `OSMetaClass::allocClassWithName("IONDRVDevice")` and
initialised from that entry; `setDeviceMemory` with the PCI device's array; attach; `registerService`. Personality 2
then starts the second `IONDRVFramebuffer`. `fbEntry()` sees it through `pciFor`, and the plugin treats a framebuffer
whose `IOFBDependentIndex` is 1 as head 1 (section 3).

Either way the device side needs a split: today each `FbState` owns a whole `RDNA4Device` (BAR mapping, VBIOS, the
one lit pipe, compute). Two heads need one shared device and a per-head part (connector, EDID, mode table, pipe,
surface).

## 6. Not known yet

1. **Head 0's controller.** With route B, head 0 has no `IOFBDependentID` unless the property is on the PCI device
   before head 0's `IOFramebuffer::start` runs (`FB.cpp:4813`). The plugin may be routed too late for that (see the
   comment on `attach` in `src/plugin.cpp`). Without it the two heads get separate controllers [F] and are not linked
   as dependents. Whether that matters (power sequencing, mirroring, WindowServer's view of the card) is not
   known [G: probably it works, the way two cards do].
2. **WindowServer.** Whether Big Sur and Tahoe accept a second unaccelerated NDRV head and extend the desktop onto
   it.
3. **The shipping binaries.** `IONDRVDevice` and the index check were read in the source, not confirmed in the
   kernel collections of 11.x or 26.x.
4. **`IONDRVDevice::getResources`** calls `IODTResolveAddressing(this, "reg", 0)` only when the nub has no device
   memory (`NDRV.cpp:203-211`); route B sets the memory first, so it should not run [I].

## 7. The experiment that answers section 6: a phantom head

No display register is written. Behind a boot-arg, the plugin creates the head-1 nub (route B), serves the second
sink's EDID (already read by `probeEDID` for every connector) and one 1920x1080 mode, and points `csBaseAddr` at a
spare VRAM surface after the GOP framebuffer.

- Pass: `ioreg` shows a second `IONDRVFramebuffer` that is online, System Settings shows the second display, the
  desktop extends onto it (nothing appears on the monitor: its pipe is still dark).
- It also shows which controller each head lands in (unknown 1).
- First run in the VM (`tools/vm-test.sh`), then on the card. Escape: `rdna4-off=1`.

Lighting the pipe is the separate hardware half: `src/modeset.cpp` only re-times a pipe the GOP already lit, and it
has not run on the card yet.
