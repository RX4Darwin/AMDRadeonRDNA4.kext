# W12k: what amdgpu wraps around an application's gfx IB on this card

Captured 2026-09-30 on the RX 9070 XT under Linux (`tools/linux-replay/ring-capture.sh`, debugfs,
read-only). The raw ring and its decode are in `hw-logs/2026-09-30-linux-gfx-ring/`. Our own IB had
already been overwritten: the desktop wraps the 2048-dword gfx ring within milliseconds. The kernel
wraps every user gfx IB the same way, though, so this is the compositor's (VMID 5) most recent
submission.

## The sequence (gfx ring 0, one submission)

| # | Packet | Operands | What it is (amdgpu) |
|---|---|---|---|
| 1 | `COND_EXEC` | addr 0x1100, 0x27 dwords | `amdgpu_ring_init_cond_exec`: lets a reset/preemption skip the rest |
| 2 | `WAIT_REG_MEM` 0x113 | mem 0x10c0 == 0xa90eb (the previous ring fence) | `emit_pipeline_sync`: wait for the previous job, because the VMID is about to change |
| 3 | `WRITE_DATA` reg 0x28f9, 0x28fa | 0xf7deb001 / 0x3 | `GCVM_CONTEXT5_PAGE_TABLE_BASE_ADDR_LO32/HI32`: the VMID's page directory (bit 0 = valid) |
| 4 | `WAIT_REG_MEM` 0x143 | write 0x28a7 = 0x00f80020, wait 0x28b9 & 0x20 | `GCVM_INVALIDATE_ENG0_REQ/ACK`: TLB invalidate for VMID 5 (bit 5), all L2 levels |
| 5 | `PFP_SYNC_ME` | | the PFP waits for the ME before fetching with the new tables |
| 6 | `WRITE_DATA` reg 0x10a5 | 0xf | most likely `IH_VMID_5_LUT` = PASID 15 (OSSSYS; not confirmed): fault routing |
| 7 | `RELEASE_MEM` | fence 0xa90ec, IRQ | the VM-flush fence |
| 8 | `COND_EXEC` | 0x1e dwords | |
| 9 | `WAIT_REG_MEM` 0x143 | write 0xe26 = 1, wait 0xe27 & 1 | most likely the HDP flush REQ/DONE for CP0 (NBIO; not confirmed) |
| 10 | `CONTEXT_CONTROL` | 0x81018003, 0 | `gfx_v12_0_ring_emit_cntxcntl`: load enables for the context switch |
| 11 | **`INDIRECT_BUFFER`** | VA, control `0x05000bd0` | **length, VMID 5 in [27:24], no VALID bit, no PRIV**: the user's IB, unprivileged |
| 12 | `RELEASE_MEM` | 64-bit to 0x2ba000 | the context's user fence |
| 13 | `RELEASE_MEM` | fence 0xa90ed, IRQ | the ring fence |

## For the kext's gfx submit (W12k)

- **The IB packet**: `Pm4::indirectBufferGfx(addr, dwords, vmid)` already encodes exactly #11. What an
  app IB needs is a non-zero VMID; PRIV stays 0. The kext's own draw uses VMID 0.
- **The IB contents**: `userspace/gfx12tri.h`. The replay proves those bytes on the card as an
  unprivileged IB in a per-process VMID (`linux-replay.md`). They carry their own rings, CONTEXT_CONTROL,
  cache invalidation and end-of-pipe fence.
- **VM**: the kext's clients have fixed VMIDs (8-15) programmed over MMIO at client creation (W36/W40).
  amdgpu instead reprograms the VMID on the ring (#2-#5), because it shares VMIDs between processes. With
  fixed VMIDs, the ring-side flush (#2, #4, #5) is only needed when a client's page tables change while
  its work is in flight. The pipeline sync (#2) is what makes that safe.
- **Fences**: amdgpu signals a user fence (#12) and a ring fence with an interrupt (#13). The kext's
  compute `rtSubmitIb` does the same with one `RELEASE_MEM` to the client's fence VA (interrupt when IH
  is up).
- **Open**: #6 and #9 are named from their shape only. Confirming them needs the OSSSYS/NBIO offsets
  for this ASIC (IP discovery bases). The kext already has an HDP flush path and IH VMID routing, so
  they are not blockers.

## Decisions: the kext's client gfx submit (W12k, implemented)

Selectors `SubmitGfxIb` (IB VA, dwords, flags -> fence) and `WaitGfxFence` (fence, timeout -> ns) in `include/rdna4compute.h` (additive: the ABI stays 4, new selectors at the end,
`RDNA4_FLAG_GFX` in Info says a submit can run *now*); `rtSubmitGfxIb` / `rtWaitGfxFence` in `src/runtime.cpp`; `librdna4` wraps them; `rdna4-run tri` and `tricol` are the clients.
Nothing else changed in existing behaviour: the G3/G4 draws, the compute runtime and the display paths are untouched (the kext diff is insertions only: the new selectors and helpers, a flag set in `gfxPark`,
a reset call in `stageGfxRing`, the self-test hook in `runStages`, the gfx-fence fields in `rtOpen`/`rtInfo`/`rtFree`/`rtRelease`/the resume reset).

### The ring packets, against amdgpu's 13 steps (table above)

| amdgpu step | kext | why |
|---|---|---|
| 1 `COND_EXEC` (+8) | not emitted | lets a reset/preemption skip the rest of a submission; the kext has neither (see "Hangs") |
| 2 pipeline sync (`WAIT_REG_MEM` on the previous fence), 5 `PFP_SYNC_ME` | not emitted | needed because the VM flush that follows changes the page-table base of a VMID that may still have work in flight. The kext never changes a VMID's tables from the ring |
| 3-4 page-table base `WRITE_DATA` + `GCVM_INVALIDATE_ENG0` | not emitted | the kext's clients have **fixed VMIDs (8-15)**: `vmContextInit` programs `GCVM_CONTEXTn` once over MMIO at open, and every map/unmap/close invalidates through MMIO (`vmInvalidate`, the GC hub's engine, which covers the gfx CP's UTCL1 and the compute queues alike: the same hub and the same VMID contexts compute clients use today). A ring-side flush would only be needed if tables changed while work is queued, and `rtFree`/`rtRelease` drain the client's gfx work first (below) |
| 6 `IH_VMID_LUT` write | not emitted | routes VM-fault interrupts to a PASID; the kext identifies faults by VMID (`logClientFault`) |
| 7 VM-flush fence | not emitted | belongs to 3-5 |
| 9 HDP flush `WAIT_REG_MEM` | done from the CPU: `flushHdp()` before the doorbell | host writes through the BAR (IB, shader code, buffers via `rdna4_write`) must reach VRAM before the CP fetches them; `rtSubmitIb` does it the same way |
| 10 `CONTEXT_CONTROL` | **emitted**, 0x80000000 / 0x80000000 | the same operands `gfx12_draw.h` phase 0 uses (update load/shadow enables, nothing loaded or shadowed): amdgpu emits it per submission for the context switch; the stream's own copy is redundant but harmless, and a client stream that omitted it still gets a defined start |
| 11 `INDIRECT_BUFFER` | **emitted**: `Pm4::indirectBufferGfx(va, dwords, client vmid)` | length (20 bits) and VMID in [27:24]; **no VALID, no CHAIN, no PRIV**: an unprivileged IB, exactly amdgpu's `0x05000bd0` form (`test-w12k-pm4.cpp` checks the encoding against the captured packet) |
| 12 user-fence `RELEASE_MEM` | replaced by the per-client fence | see below |
| 13 ring-fence `RELEASE_MEM` (+ IRQ) | **emitted** as the per-client fence: EOP event with GL2 write-back, 32-bit data, no interrupt, to the client's own fence dword | the kext's gfx ring has no IRQ wiring for fences (polled, like `gfxFenceWait`); the write-back makes the client's rendering visible before the fence is |

So a submission is **15 ring dwords**: CONTEXT_CONTROL (3) + IB (4) + RELEASE_MEM (8). The stream itself starts with its own `ACQUIRE_MEM` (GLI/GLK/GLV/GL2 invalidate + GL2 write-back), so stale
shader code or data from an earlier run is not fetched; the kernel does not add another.

### Sharing the ring: who may use it when

The gfx ring is used without `rtLock` by the kext's own work, all of it on the bring-up thread with `bringupRunning` set: `stageGfxRing`, the G3/G4 draws, the ladder, `gfxPark`, the
`rdna4-gfxclient=1` self-test. **Client submissions take `rtLock` and refuse (`kIOReturnNotReady`) while `bringupRunning` is set** (and while the ring is parked: probe boots halt PFP/ME after
their draws, `rdna4-gfxpark=0` leaves them running; or during sleep/shutdown). So the ring has exactly one user at a time without touching any of the kernel's own gfx code; clients are
serialised among themselves by `rtLock` (held during a submit and during a wait, like `rtSubmitIb`/`rtWaitFence`). After a wake the bring-up runs again (`resumeMain`): `stageGfxRing` resets
the gfx state (`gfxClientReset`) and the old clients are aborted. A client that calls while the wake bring-up draws gets NotReady and retries.

Ring space: at most 16 submissions are outstanding across all clients (and 16 per client), i.e. <= 240 of the 4096 ring dwords, so the producer can never overrun the consumer; the ring's
write pointer is 64-bit, never wraps, and goes out through the same `gfxKick` doorbell path as the kernel's own draws. A full quota returns `kIOReturnBusy`.

### Fences

Each client has its own gfx fence dword (+0x40 of the page the compute fence lives in: `c->gfxFenceCpu`, VMID0 MC address `c->gfxFenceMc` for the ring's `RELEASE_MEM`; the client can read it
at `fenceVa + 0x40`), its own counter and its own list of unretired values; `WaitGfxFence` polls only that dword, so **a client waits only for its own work**. The ring is in order, so its
fence completes after whatever other clients queued before it: a client cannot be starved but is not independent of a hung neighbour (see "Hangs"). It is a different namespace from the compute
fence (`SubmitIb`/`WaitFence`), so the two never alias.

### Ownership and validation: what the kext checks and what the hardware enforces

The kext validates what `rtSubmitIb` does: a runtime-ready VM client, dword alignment, `0 < dwords <= 0xfffff` (the IB_SIZE field), `flags == 0`, and **the whole IB inside one buffer this client owns**
(device or host). It builds the IB packet itself, so the VMID is always the caller's and PRIV/VALID/CHAIN are never set. It does **not** parse the stream: what the stream touches is bounded by
(1) the client's page tables (all addresses in an IB, packet operands and register-held addresses alike, are translated in the IB's VMID: the kernel's rings, the pool and other clients' memory are
not mapped there, except the pages `rtOpen` maps read/write for the client's own queue/fence/kernarg) and (2) the CP's rules for an unprivileged IB. The second is **inferred, not measured on this
kext's path**: amdgpu submits user gfx IBs the same way with no CS parsing on gfx12, and the W12e emulator models the traps (PRIV_REG on privileged register writes from an unprivileged IB, OPCODE_ERROR,
fault on unmapped memory); `rdna4-gfxclient=1` tests the positive path only. The IB's own `RELEASE_MEM`/`EVENT_WRITE`/`WRITE_DATA` memory operands go through the client VMID, which is what lets the G3/G4
streams write their fences into the client's buffers.

### Hangs and timeouts

What the kext's own gfx work does on a hang (W23): nothing; the ring has no per-queue reset without MES (amdgpu's `Starting gfx_0.0.0 ring reset` goes through MES firmware, which this kext does not
run), so a draw that does not finish means "power-cycle before the next boot". For clients the same limit applies: a gfx fence that is not reached within the caller's timeout (default 1 s, at most 10 s)
-> `WaitGfxFence` returns `kIOReturnTimeout`, **the gfx ring is wedged**: PFP/ME are halted (`CP_ME_CNTL`, the same halt `gfxPark` and the ring failure path use) so the CP fetches nothing
more, all pending counters are dropped, and every further `SubmitGfxIb`/`WaitGfxFence` returns `kIOReturnNotResponding` (Info's `RDNA4_FLAG_GFX` clears) until the next bring-up
(a sleep/wake cycle re-initialises the ring; otherwise a reboot). What is **not** affected: the display (DCN scan-out does not use the gfx ring), the compute queues (separate MEC queues with
their own recovery, `rtWedged` is independent), SDMA, the page-flip and present paths, and every other runtime selector. What can be: a shader wave that is stuck in the SQ keeps its CU
busy and the GC powered (this is unmeasured; a stuck wave of a *gfx* pipeline does not block MEC queues on other CUs in principle). `rtFree` and `rtRelease` wait (up to 2 s) for the
client's outstanding gfx IBs before unmapping anything, and wedge the ring the same way if they do not finish. A client that dies with a gfx IB in flight is handled by `rtRelease`
(`clientClose`/`clientDied`) the same way. While a wait runs `rtLock` is held (as for compute): other runtime calls wait, the present timer retries.

### State between clients, and the GE rings

The ring's context/SH/uconfig registers are global: a client stream leaves whatever it set. **Decision: the kernel does not reset them per submission.** Each stream (`gfx12_draw.h`,
`gfx12_draw_col.h`, which the builders record) sets every register it depends on (the Linux replay proves a stream works on a ring that ran other work first: amdgpu's ring does not replay
clear state either), and the kext's one-time CSB replay + sane-clip block (the `gfxDrawRun` mitigation for the bare-ring SRM problem) runs in its own draws during bring-up, *before* clients can submit,
so the registers start defined. A client that relies on state it did not set may see another client's values (an isolation leak of *state*, not of memory: memory is per VMID); replaying the CSB
before every client IB would cost ~1000 ring dwords per submission and is left out (it can be added behind a boot-arg if a client ever needs a defined start; not measured).

The **GE/SPI ring registers** (`SPI_ATTRIBUTE_RING_BASE/SIZE`, `GE_POS_RING_*`, `GE_PRIM_RING_*`) are global, but their contents are addresses *in the VMID of the IB that draws*: the client's stream
carries its own (`gfx12tri.h`: the app allocates the rings, 2 MiB aligned, in its own VM), and the hardware resolves them with that VMID. Consequences: (1) the kext's own draws cope without any
re-emit: G3/G4 streams set all of these themselves on every draw (and they run only in bring-up, before clients); after a wake the bring-up draws again with its own streams; (2) a client's
rings are never reachable from another VMID; (3) the registers keep the last client's values after it exits, harmlessly: nothing uses them until a stream sets them again. A stream that
forgot them would fault in its *own* VM (an unmapped address), not touch anyone else's memory.

### Testing without a Mac

- **Kext boot self-test:** `rdna4-gfxclient=1` (with `rdna4-vm=1 rdna4-gfx=2`, not a probe boot if the result should be seen after park; it runs *before* park, after the G3 baseline passed): opens a synthetic client (internal token, own VMID
  and tables), maps two pool pages in it (IB, data), submits an IB of two `WRITE_DATA` packets to the data page twice, checks the data, the two per-client fences (`f2 = f1 + 1`) and that nothing is
  pending afterwards; registry key `gfx-client` (`PASS ...` / `FAIL ...` / `SKIPPED ...`), `gfx-client` row in `tools/diagnostic-log.sh`, `RDNA4FB: vmid N: submitted unprivileged gfx IB ...` and `gfx client self-test: ...` lines.
- **Host tests:** `userspace/test-gfx12tri.c` (G3/G4 builders, the layout with a simulated allocator, refusals, the fact that the replay's stream and rdna4-run's differ only in relocated dwords, the image
  check), `userspace/test-gfx12tricol.c`, `userspace/test-w12k-pm4.cpp` (the ring packets against amdgpu's captured IB packet and the emulator's acceptance rules). The IB bytes `rdna4-run tri` records are the ones
  `tools/linux-replay` proves on the card through amdgpu (baseline and `REPLAY_VARIANT=4096` re-run: 8192 px).
- **Emulator / macOS later:** `rdna4-run tri` / `tricol` on a booted kext with `rdna4-vm=1 rdna4-gfx=2` (no probe): `info` shows the GFX flag; `tri` prints `PASS tri: 8192 px ...`. For `tricol` the emulator refuses the draw (G4 is
  not modelled, `docs/g4-colour.md` section 7), so there `tricol` FAILs with 0 px by design.

### What is not measured

Everything above is built and unit-tested on Linux only. Not measured: the kext ring path on the card or in the emulator (first run is Kiln's loop or the next real-card boot), the CP's PRIV enforcement on this path (inferred from
amdgpu/W12e), a real gfx hang and what the wedge leaves behind (halt + reboot is the conservative response, chosen without a hang to observe), the behaviour of a stuck wave for other queues, and the per-submission cost.

### Dependency: every unmap must invalidate (W13 S0) — W12k must not be enabled or merged without it

The "no ring-side VM flush" decision above rests on one invariant: **a VMID's translations are invalidated (over MMIO, `vmInvalidate`) whenever a mapping goes away**, so the CP/UTCL
never walks a stale PTE of a freed buffer that has been reused. Forge found, and the lead verified, that `rtFree` does **not** do this for **device** buffers today (it calls `vmUnmap` and frees the heap
range; `vmInvalidate` only runs for host buffers). For compute that hole already exists; for gfx it would let a client's stream reach freed-and-reallocated VRAM. The fix is W13 step S0 on
`premetal/w13` (not made here, to avoid a conflict). **Do not enable W12k (boot 8, `rdna4-run tri/tricol` against a real card) or merge `premetal/w12k` without S0.** `rtRelease` is not affected: it clears the
whole table and invalidates (`vmInvalidate(vmid, "client close")`) before the VMID is reused.

### One place for the IB's VMID (W13 contract, docs/w13-vmid.md 5.6)

The VMID an IB runs in comes from **one function**, `RDNA4Compute::vmidForSubmit(const RtClient &)` (`src/compute.hpp`), which returns the client's fixed `vmid` today. It supplies the VMID field of the gfx
`INDIRECT_BUFFER` (`gfxClientEmit`), the compute `INDIRECT_BUFFER` (`rtSubmitIb`) and the `SH_MEM_CONFIG/BASES` selection made for it. W13's VMID pool replaces the body later (a per-submission grab);
nothing else in the submit paths names `c.vmid` for this purpose. No behaviour change.

### Boot 8: clients on a live ring (tools/set-boot.sh, tools/diagnostic-log.sh)

Boot 3 (probe) halts PFP/ME after its draws (`gfxPark`), so applications cannot draw there. **Boot 8** = boot 2 (`rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1`) +
`rdna4-gfx=2 rdna4-gfxcol=1 rdna4-gfxclient=1`, **without** `rdna4-gfxprobe` / `rdna4-gfxdiag`, so the ring stays up after bring-up. Bring-up draws G3 and (after a passing baseline) G4, then runs the kernel's
synthetic client-IB self-test (`gfx-client`); the ring is not parked. `diagnostic-log.sh`, when `rdna4-gfx` is set, it is not a probe boot, the runtime is up and `rdna4-vm=1`:
1. `rdna4-run info` (already a step): must print the line `GFX: client gfx IBs available ...` (RDNA4_FLAG_GFX); if it does not, rows `gfx-app-tri` and `gfx-app-tricol` are **FAIL** ("no GFX flag");
2. `rdna4-run tri`, behind `run_step` (the same 120 s hard bound as the other `rdna4-run` steps), row **`gfx-app-tri`**: PASS only when the command prints its own success line `  PASS  tri: ...` and exits 0;
3. `rdna4-run tricol`, same bound, row **`gfx-app-tricol`**: PASS only on `  PASS  tricol: ...`; it runs only after `tri` passed (a client IB that hangs wedges the gfx ring until the next bring-up, so a
   second submit after a failed first one would only add noise), else the row is SKIPPED ("tri did not pass").
Otherwise both rows are SKIPPED with the reason (gfx not enabled / probe boot / runtime unavailable / needs rdna4-vm=1).

**Which binary the stick needs:** the script uses `$HERE/rdna4-run`, i.e. a file named `rdna4-run` **next to `diagnostic-log.sh`** (the stick root, where the script lives), copies it to `/tmp/rdna4-run` and runs
that. For boot 8 it must be the `build/rdna4-run` of a build that contains `tri`/`tricol` and the new selectors (this branch's `make`/`tools/build-osxcross.sh` output); an older `rdna4-run` has no `tri` command and
would print its usage and exit non-zero (row FAIL). The kext on the stick must be built from the same tree (the selectors are new).

**Expected on the emulator (Kiln's / the Windows dry run), boot 8:** `gfx` PASS (G3), `gfx-client` PASS (the self-test's WRITE_DATA IB in a client VMID is modelled by W12e), `gfx-app-info` implicit (the GFX line is
printed), **`gfx-app-tri` PASS** (the same stream the replay proves; the emulator models it in a client VMID), **`gfx-app-tricol` FAIL** and `gfx-col` FAIL: the G4 attribute-ring path is not modelled there
(`docs/g4-colour.md` section 7: the draw is refused, 0 px), so `rdna4-run tricol` reports `FAIL  tricol: 0 px ...`. That FAIL is expected on the emulator and is not a regression; on the real card both should PASS.

## Integration note (hub-task-357): shared-queue clients need the gfx fence fields

`rtOpenInner` sets `c->gfxFenceCpu` / `c->gfxFenceMc` (the client's gfx fence dword, `kVmFence + kGfxFenceSlot`); `rtOpenShared` (W13 S7-lite, `rdna4-vmshared=1`) did not, so boot 13 failed (`gfx-client FAIL setup`,
`SubmitGfxIb: resource shortage`, Kiln's whole-plan dry run). Fixed on `premetal/final` (4365f84). **S8's open path (`rdna4-vmshared=2`) must set the same two fields** when S8 merges.
