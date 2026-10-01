# The Metal phase: milestones, go/no-go gates, dependencies

Written 2026-10-01 for hub-task-366, from `docs/metal-spike.md` (evidence: sections 6.1 and 9; route B' first, B fallback, A dropped) and `docs/metal-readiness.md` (what the pre-Metal foundation proves where). Efforts are **inference**, one developer, rough. Every milestone is behind a boot-arg (default off): a half-working `MTLDevice` must never make today's working display worse (WindowServer would start trying Metal on it).

Status words as in metal-readiness: **card** (real RX 9070 XT under macOS), **card (Linux)**, **emulator**, **code**.

## The bet in one paragraph

Apple already ships, for x86, a virtual GPU stack: `AppleParavirtGPU.kext` (binds PCI 106b:eeee), `AppleParavirtGPUMetal.bundle` (x86_64 slice; `MTLIOAccelDevice` subclass, command serialiser) and the compile-service path. We play the virtual GPU's **host**: a fake 106b:eeee device in our kext, a ring-polling decoder, and an executor that runs the decoded Metal work on the RDNA4 through our existing runtime. Route B (our own kernel accelerator + Metal bundle) is the fallback at every gate.

## What every milestone shares (foundation dependencies)

| Needed from the pre-Metal work | Needed by | Real-card evidence it depends on |
|---|---|---|
| Client VM: a process's GPU work in its own VMID | M2, M4 | **card: still failing** (metal-readiness s.4); W13 S7-lite/S8 pass in the emulator only |
| Many clients (a Metal app and WindowServer are each a client/task; >8 concurrent) | M2+ | W13 S8 (emulator: 12 clients, VAs past 3 GiB); card: unproven |
| W12k client gfx submit (`SubmitGfxIb`, VM, fences) | M4 | code + emulator; card: pending boot 8/13 |
| G4 NGG + attribute-ring draw from a client | M3, M4 | card (Linux) exact; kext path pending boot 3 |
| Flip/present, IOSurface-backed scanout | M4 | card (flip); IOSurface path not built |
| Hang recovery per client | M2+ | compute W6 card non-VM; gfx code |
| Test loops: `tools/emu-linux.sh`, `tools/linux-replay`, `tools/radv-triangle` | all | exist |

If the card's client-VM failure is not root-caused, **M2 and later are blocked whatever the route** (B has the same dependency). M0 and M1 do not touch the card.

## Milestones

### M0 (E4a): does Apple's kext accept our fake device? (go/no-go for B')
- **Do:** install macOS 26.6.2 in the emulator (standard OSX-KVM path, Recovery installer; ~60 GB disk, hours of emulator time [INFER: it downloads from Apple; the installer we hold on disk is not required]); in that VM, load our kext with a **fake `IOPCIDevice` nub 106b:eeee** (RAM-backed BAR0 of 16 KiB, our own interrupt controller), boot-arg `rdna4-pvgpu=1`.
- **Gate (go):** the stock `AppleParavirtGPUControl`/`AppleParavirtAccelerator` get through `configureDevice` -> `setupMMIO` -> `setupVersion` (we log the VERSION write and answer the read-back) -> `setupFIFO`/`setupRoot` (we read `FIFO_BASE_PAGE`/`ROOT_PAGE`) and then write bytes into the FIFO; we capture the first root-FIFO packets by polling `FIFO_WRITTEN`.
- **Evidence:** a log of the register/FIFO sequence from a full boot; the first decoded packet types; `ioreg` showing the `IOAccelerator` node with `MetalPluginName=AppleParavirtGPUMetal`.
- **Needs:** nothing from the card. Needs one full-install VM in Kiln's emulator queue (the final real-card round waits on that queue: schedule M0 after it, or on a second slot).
- **Effort:** 3-6 weeks incl. the VM setup [INFER].
- **Loading constraint (found by E1, metal-spike s.9.7):** a kext injected by OpenCore lives in the boot kernel collection and cannot link `IOGraphicsFamily`/`IOAcceleratorFamily2` (they are in the base-system/system collection); B' does not need to (the nub derives from `IOPCIDevice`), B does (see below).
- **No-go / fallback -> route B:** if no nub variant (fake `IOPCIDevice`, runtime personality copy, Lilu patch of the match) gets past `configureDevice`, or the driver requires trapped writes we cannot emulate, start B: the E1 census (loader + selector evidence) plus reconstructing `IOGraphicsAccelerator2` subclasses from the measured vtable (metal-spike s.9.5); B's first task is then to find a way to load such subclasses at all (run-time class derivation, or a kext in a later collection), because of the loading constraint above.

### M1: ring polling and stream decode on Tahoe
- **Do:** poll the root FIFO and the four child channels (Exec, Immediate, Uploads, Downloads); maintain tasks/page tables (`DefineTask2`), resources, resource lists; decode the serialised ops for a test app (buffer/texture create, compute encoder dispatch, blit, render pass, present). Ground the layouts in Apple's **own serialiser as an oracle** (run `PGSerializer` from the Tahoe bundle in the full-install VM and diff bytes), using reims-vgpu's documented ops as a reference.
- **Gate (go):** a scripted Metal test (compute add + blit + one render pass) in the guest produces a stream that decodes with **zero unknown opcodes** and whose buffer/texture contents we can reconstruct on the host side; the op set is stable across the 26.6.2 bundle (64.4.7).
- **Evidence:** decode log + a coverage table (selectors seen / decoded), the oracle fixtures' results (kept outside git if they are Apple bytes).
- **Needs:** M0; no card. **Effort:** 1-2 months [INFER].
- **Fallback -> B:** if Tahoe's op set drifts past reims' macOS 13/15 knowledge faster than we can follow (measure: share of seen selectors undecodable), B's own protocol avoids that maintenance.

### M2: compute-only execution on the RDNA4
- **Do:** the user-space host daemon executes decoded compute work through our runtime (`Alloc`, `Load`, `SubmitIb`/`WaitFence`, SDMA copies). Shader bootstrap: a **table of precompiled gfx12 kernels keyed by AIR hash** (no compiler yet), e.g. `vadd`.
- **Gate (go):** a Metal compute app in the guest runs `vadd` and returns correct results **on the card**, with two such apps concurrently (two clients, separate VMIDs), hang recovery exercised.
- **Evidence:** card boot log + app output; `diagnostic-log.sh` row; client-VM fault/stale counters = 0.
- **Needs (card):** client VM working; many-clients path (W13); compute recovery. **Effort:** 2-4 months after M1 [INFER].
- **Fallback:** none route-specific: a missing card client-VM result blocks B equally. If only the non-VM legacy path works, M2 can run with <= 8 clients as a demo.

### M3: the AIR compiler
- **Do:** two tracks, about two weeks each, then pick: (a) `metal2vulkan` (AIR -> SPIR-V) + Mesa NIR/ACO built for macOS; (b) AIR -> LLVM AMDGPU (upstream LLVM supports gfx1201). Start with the **two SkyLight composite shaders** (obtained from the M1 stream) and the conformance compute kernels; compile and test them **off macOS first** with `tools/linux-replay` on the card (ISA correctness without the whole stack).
- **Gate (go):** both shaders compile to gfx12 NGG/PS code that draws the expected pixels through linux-replay (attribute ring, EXEC set from `merged_wave_info`, VGPR granules per metal-readiness s.2), and the compute kernels match a CPU reference.
- **Evidence:** linux-replay outputs (pixels, PS invocations), a coverage list of AIR features supported.
- **Needs (card (Linux)):** the G4 facts (have). **Effort:** 3-6 months, partly parallel with M2 [INFER].
- **Fallback:** if neither track reaches correct NGG output, ship the **whitelist** (prebuilt ISA for WindowServer's shaders only): the desktop still composites, apps compile nothing. B has the same compiler problem, so B is no help here; scope shrinks instead.

### M4: WindowServer renders on our device; display pairing
- **Do:** render path (textures, samplers, render passes, present into IOSurface-backed scanout via the flip path); answer the open question whether macOS pairs the paravirt `MTLDevice` with the display our IONDRV framebuffer drives (two "GPUs" in the registry, one card).
- **Gate (go):** `MTLCopyAllDevices()` returns our device, WindowServer composites on it for >= 30 minutes (no hang, 0 faults), with default-off boot-arg still the shipped default.
- **Evidence:** card log, screenshots (`ashot`), idle-accounting and fault counters, a reboot/sleep-wake run.
- **Needs (card):** W12k client gfx on the card, flip/IOSurface path, hang recovery for gfx. **Effort:** 4-8 months after M2 [INFER].
- **Fallback:** if macOS refuses the pairing, present through our own path instead: render on the GPU and copy into the IONDRV framebuffer (blit present) [INFER: slower, workable]; the pairing question is route-independent, so B gains nothing.

## Decisions the USER must make

1. **reims-vgpu reuse.** Its decode/device model is the best map of the paravirt protocol, but it is **LGPL-3.0-or-later** and this repo is **BSD-3-Clause**. Options: (a) **clean-room**: read reims as a reference, derive every layout ourselves from Apple's serialiser oracle and Apple's kext (slower, no licence issue); (b) **reuse as a separate program**: the user-space host daemon depends on reims' crates under LGPL (dynamic/separate process), the kernel shim stays BSD-3 (faster; the combined distribution must honour LGPL). Recommendation: (a) for the kernel shim in any case; decide (a)/(b) for the daemon after M1 shows how much decodes unchanged on Tahoe.
2. **The full-install VM (M0):** ~60 GB disk (614 GB free) and hours of emulator time that compete with Kiln's queue (the final real-card round waits on it). Approve it now, or schedule M0 after the real-card round.
3. **Scope of "done":** stop at M2 (compute-only Metal device, opt-in) or commit to M4 (desktop on our device). M2 is useful on its own (Metal compute apps) but cannot be the machine's default device until M4.
4. **Risk acceptance:** M0-M4 use Apple's private kernel and user-space interfaces (and may need SIP/AMFI relaxed beyond what the OpenCore setup already needs [INFER: untested for a third-party Metal bundle]); each new macOS point release can change the paravirt op set (M1's coverage table is the early warning).

## Order and what runs in parallel

E1 census (queued with Kiln, hub-task-356) and E2b (cryptex, low priority) continue as queued; they serve both routes. M0 after the real-card round's queue. M3's compile-and-test-on-Linux work needs no macOS and can start as soon as the two shaders are known (M1) or, earlier, with RADV/ACO-generated NGG shaders the project already has. M2's card dependency is the project's existing top blocker, which is being worked on independently of this plan.
