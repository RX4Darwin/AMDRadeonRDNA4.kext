# Ready for the Metal phase? The hardware foundation, what is proven where, what is still open

Written 2026-09-30 by the lead as the closing document of the pre-Metal phase. Status words, used strictly:

- **card**: measured on the RX 9070 XT under macOS with the kext (a `rdna4fb-diag-*` log on the stick or in `docs/hw-logs/`).
- **card (Linux)**: measured on the same card under Linux with the kext's own bytes (tools/linux-replay, tools/radv-triangle).
- **emulator**: passes in the QEMU model (`emu/qemu/rdna4.c`, `tools/emu-linux.sh`). The emulator has passed things the card then
  failed (VM clients, the G3 EXEC bug), so it is not evidence for the card.
- **code**: written and reviewed, never executed.

## 1. What a Metal driver needs from the hardware layer

| Need | Kext interface | Status |
|---|---|---|
| Display: modes, EDID, DPMS, scanout | IONDRVFramebuffer hook (Lilu plugin), DCN401 mode-set | card |
| Page flips, present, vblank | `rdna4-flip`, `Present`/`PresentAsync`/`WaitPresent`, IH vblank/pflip | card (round 2+) |
| Hardware cursor | `rdna4-cursor=1` (CM_BYPASS cleared) | card (round 6) |
| Interrupts | IH ring (`rdna4-ih=1|2`), CP EOP, SDMA traps, VM faults | card |
| GPU memory: VRAM heap, host (zero-copy) buffers, DMA | runtime `Alloc`/`AllocHost`/`Write`/`Read`, SDMA bounce buffer | card (non-VM path) |
| Per-application address spaces | `rdna4-vm=1`: VMIDs 8-15, page tables per client | **boot self-test only**; client jobs FAIL on the card (section 4) |
| Compute submission | `Dispatch`, `SubmitIb`/`WaitFence` (per-client MEC queue) | card on the non-VM path; FAILS with VM (section 4) |
| Graphics: the kext draws | gfx ring RB0 (VMID0), G3 triangle, G4 colour triangle | card (Linux) for the stream+shaders; kext path pending boot 3 |
| Graphics: applications draw | W12k `SubmitGfxIb`/`WaitGfxFence`, `rdna4-run tri/tricol` | code + emulator (boot 8); depends on section 4 |
| Many applications | W13: VMID pool + shared kernel queues (design B) | code (pool, tests); S7-lite in progress |
| Hang recovery | compute queue recovery (W6), gfx wedge-on-timeout | card (compute, non-VM) / code (gfx) |
| Power | clock gating ~20 W idle; GFXOFF dropped from pre-Metal; idle accounting, sleep/wake hardening | card (CG) / code |

## 2. Hard-won hardware facts the Metal phase must respect

These cost real-card rounds. Every one is measured unless marked.

- **NGG waves start with EXEC = lane 0 only** (card (Linux), `docs/linux-replay.md`). A vertex/NGG shader must SET exec from
  `merged_wave_info` (s3), never AND it. A shader compiler for Metal must do what ACO does.
- **VGPR allocation granules**: wave32 = 8, wave64 = 4 (`SPI_SHADER_PGM_RSRC1_GS.VGPRS`); under-allocating does not fault, it
  draws garbage or hangs the ring (one hang today under Linux, ring reset recovered).
- **NGG system SGPRs**: s2 `gs_tg_info`, s3 `merged_wave_info`, s5 `gs_attr_offset` (0 in 10/10 G3 draws), v0 = packed primitive
  (`0x040a0300`: 9-bit index stride + edge flags 8/17/26), v3 = VertexID. Unused system SGPRs (s4) are garbage.
- **Attributes (G4)**: the NGG stage stores them to the attribute ring with `buffer_store_b128 ... idxen` through a descriptor RADV
  builds over the whole GE ring block; the PS reads them with `ds_param_load` + `v_interp_p10/p2`; PrimMask in the first system
  SGPR after the PS user SGPRs. Card (Linux): exact to 0.5 of the barycentric colour.
- **Export order does not matter** on gfx12 (card (Linux)); Mesa exports position before primitive by choice.
- **C_PRIMITIVES is not a reliable signature** (0/4/8 across identical failing runs): judge draws by pixels and PS_INVOCATIONS.
- **User gfx IBs**: amdgpu wraps a user gfx IB with pipeline sync, VM flush (PT base + ENG0 invalidate), PFP_SYNC_ME, IH VMID LUT,
  CONTEXT_CONTROL, IB (VMID in [27:24], no VALID, no PRIV), user + ring fences (`docs/w12k-gfx-submit.md`, captured on the card).
- **Compute in a VMID**: a VMID-0 kernel queue taking the VMID from the IB packet runs our code correctly in a per-process VMID
  (card (Linux), `tools/linux-replay` compute). amdgpu has no gfx12 precedent for MMIO-loaded HQDs with a non-zero VMID (KFD needs
  MES).
- **Unmap needs an invalidate before memory reuse** (code: W13 S0 fixed `rtFree`/`rtUnload`).
- **Per-client VA is limited to 1 GiB** above `kVaStart` in the fixed-VMID layouts (one PDB1 entry; found by W13's differential
  table test, pre-existing). The sparse layout of W13 mode 2 lifts it. Metal apps with large resources need mode 2 (or the fix
  backported).
- **Emulator TLB model**: a faulting access is not a stale translation (the 16640 false STALE lines of 2026-09-30); only walks
  that complete are compared.

## 3. Test loops available to the Metal phase

| Loop | What it proves | Cost |
|---|---|---|
| `tools/linux-replay` | any gfx/compute stream + shaders on the real card through amdgpu | seconds, no reboot (GPU hangs possible: warn the user) |
| `tools/radv-triangle` + patched Mesa | RADV ground truth; hypothesis tests by patching RADV | seconds (+ one Mesa build) |
| `tools/linux-replay/ring-capture.sh` | what amdgpu puts on the ring (sudo) | one run |
| `tools/emu-linux.sh <boot> --diag` | the kext + diagnostic script in macOS Recovery on the QEMU model | ~2 min per boot |
| real-card boots (`tools/set-boot.sh`, `tools/diagnostic-log.sh`) | the truth | a reboot per boot, the user's time |

## 4. The open blocker: applications' GPU work in their own VM

**Update 2026-10-06 (Sunneva; card: Sunneva's RX 9070 XT under Big Sur 11.6.6, branches `devel/vulkan` and
`devel/address-space`).** An application's GPU work in its own address space **runs on the card**, by another route than
the one this section describes as failing:

- **card**: the bring-up to stage 7, the graphics ring's own tests and the G3 triangle (`THE TRIANGLE IS RIGHT`, 8192 px)
  under Big Sur with two displays lit; idle after clock gating 786 MHz / 3 % / 19 W, not pinned.
- **card**: address space 8, its context registers set over MMIO (`vmContextInit`), a four-level table written by the CPU
  through the BAR, and work in it from kernel queues that stay in address space 0, the address space named in the packet:
  the copy engine (`INDIRECT` with VMID 8, three copies between VRAM and system memory) and the graphics ring
  (`INDIRECT_BUFFER` with VMID 8). On the ring, Mesa's RADV ran fills, a compute shader, and a draw with a vertex and a
  fragment shader, and showed frames through `Flip::flipTo`. `docs/vulkan-port.md` sections 8 to 10 have the logs.
- In none of those boots was a queue ever built **inside** a non-zero address space; `rdna4-vm` was not set, so neither
  the boot self-test nor a client queue ran. That is the one thing every failing boot of rounds 2 to 6 did and these did
  not. It is a correlation, not a root cause: `docs/vm-client-rootcause.md` section 11.
- Since `devel/address-space`, `rdna4-vm=1` uses the shared kernel compute queues by default (W13 S7-lite,
  `rdna4-vmshared=1`), and the boot self-test goes through them too (`vmSharedBootTest`, which is W13's step S1), so
  that a `rdna4-vm=1` boot no longer builds such a queue at all. `rdna4-vmshared=0` gives the old path.
- **card, 2026-10-07** (`docs/vm-client-rootcause.md` section 12): with that default the boot test passes (a kernel MEC
  queue runs a command buffer in the address space its packet names), the graphics-ring client self-test passes, a
  compute kernel runs in a client's address space on the shared queue (`rdna4-run`'s zero-copy vadd), and the GPU is
  not pinned at idle. **Still failing**: a job whose shader faults never finishes and its queue cannot be recovered, so
  the two deliberate-fault tests of the self-test hang and take the rest with them. Setting the fault default page up
  as Linux does changed nothing (card, second run). Answering the fault as amdgpu does, by mapping a dummy page at the
  faulting address, is **code, not run** (`docs/vm-client-rootcause.md` section 13, `docs/todo-vmtest.md`).

The table in section 1 and the text below are as written on 2026-09-30.

(Filled in when the final real-card round has run.) Summary so far: in every real-card VM boot every client job fails (the first
dispatch times out, then "device not responding"; every client close logs a dequeue timeout; the GPU reads ~100 % busy before any
client). Only the kernel's boot VM test passes, and it runs no shader. Candidates and the boots that decide them:
`docs/vm-client-rootcause.md`, `docs/boot9-vm-diagnostic.md` (boots 9 / 9b), W13 S7-lite (shared VMID-0 queues).

## 5. Definition of "ready to start the Metal phase" (adopted from docs/metal-spike.md s.6)

1. Client VM works on the card, including more than 1 GiB per client (W13 mode 2).
2. Client graphics submission works on the card (W12k boot 8 / 13).
3. The Metal route is decided from experiment E1 (impostor-IOAccelerator census in the emulator) and E2 (static survey of a
   full macOS 26.6.2 install: needs a ~15 GB download, the user's call).
4. An access policy for the accelerator's user client (docs/w13-client-access.md).
5. A compiler plan (AIR to gfx12; see route B in docs/metal-spike.md).

### Route findings so far (docs/metal-spike.md, Forge)

- Recovery has no AMD Metal stack; the full x86 install of 26.6.2 has the AMD stack for Navi 1x/2x only (no gfx11/12 target
  found yet; libSC itself sits in the OS cryptex, E2b).
- **Route B' is alive**: the full install carries AppleParavirtGPU.kext (PCI 106b:eeee) and AppleParavirtGPUMetal.bundle
  (x86_64), a complete third-party-style MTLIOAccelDevice plug-in that serialises Metal commands into a FIFO. Presenting that
  device would let Apple's own Metal driver run; our side would execute the command stream on RDNA4 and compile AIR to gfx12.
- Route B (own bundle + IOAccelerator classes + AIR to gfx12 via LLVM AMDGPU) remains the general route; route A (reuse the
  RDNA2 bundle) is limited by its gfx10-only compiler. MetalCyan (BC-250, docs/metal-spike.md s.10) confirms route A only
  works where Apple's gfx10 output already runs (a Navi-10-generation chip).
- E1 (emulator, s.11): an impostor IOAccelerator is found by matching without breaking Recovery; system processes read only
  `MetalPluginName`; a missing bundle fails softly (0 Metal devices, no crash); no user client is opened before a bundle loads.
  A kext injected by OpenCore cannot link IOGraphicsFamily/IOAcceleratorFamily2, which hurts route B (subclassing
  IOGraphicsAccelerator2 from our kext), not B' (Apple's paravirt kext does that).

Metal-phase plan with milestones and gates: `docs/metal-phase-plan.md` (premetal/metal-spike): M0 fake paravirt device in a
full-install VM, M1 command-stream decode, M2 compute on the card (needs the client-VM blocker solved), M3 AIR compiler,
M4 WindowServer.

## 6. What the Metal phase starts with (Apple side, not started)

IOAccelerator service + user client with an access policy (`docs/w13-client-access.md`), a Metal driver bundle, a shader
compiler (AIR/Metal IR to gfx12 ISA respecting section 2), command encoding onto the W12k/W13 submission paths, IOSurface-backed
scanout through the flip path. A shader-translation feasibility spike is the recommended first step.
