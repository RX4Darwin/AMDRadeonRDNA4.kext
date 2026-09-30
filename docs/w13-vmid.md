# W13: more than 8 GPU clients — design (phase 1, no kext code)

Branch `premetal/w13` (from `premetal/g4` 789606f). Written 2026-09-30 on the Linux box, from the code and from amdgpu in
Linux 7.2.2 (the Windows-side "design A = per-job VMIDs" notes are not available; this redoes the design).

Evidence tags used throughout:

- **[READ]** read in source (kext `file:line`, or amdgpu in `~/work/tools/linux-amdgpu/src`, v7.2.2, read-only checkout).
- **[MEASURED]** observed on the card or in a checked-in log (cited).
- **[INFER]** follows from the above, not observed.
- **[UNPROVEN]** an assumption the design relies on that nothing here has shown on this card.

Kernel paths below are relative to `drivers/gpu/drm/amd/` (`amdgpu/…`, `amdkfd/…`).

## 0. Summary

1. Today a client = one fixed VMID (8-15) + one dedicated MEC HQD + one 4 MiB page-table image, all taken at `rtOpen`.
   The effective cap is **7 clients** (7 usable HQDs), not 8. Nine other limits sit behind that one (section 1).
2. amdgpu does not work this way for its own clients. It **virtualises** VMIDs: a VMID is granted per *submission*
   (`amdgpu_vmid_grab`), reused when the same page directory still owns it, and otherwise rebound with a TLB flush
   (`amdgpu_vm_flush` → `gfx_v12_0_ring_emit_vm_flush`). The IB packet carries the VMID, and kernel queues have HQD VMID 0.
   The sequence amdgpu wraps around a user IB is **measured on this card** (`docs/hw-logs/2026-09-30-linux-gfx-ring/decoded.txt`)
   and the VM-related registers in it are now confirmed against the 7.2.2 headers (section 2.2).
3. Recommended design, **B** (section 4): *VMID virtualisation + queue virtualisation*.
   - A global VMID pool over hardware VMIDs 1-15 (LRU, owner/PD/TLB-sequence tracked, amdgpu's grab rules).
   - Compute clients stop owning HQDs. They submit jobs (each wrapped in an `INDIRECT_BUFFER` that names the VMID) to a few
     **kernel-owned shared MEC queues** (HQD VMID 0, like amdgpu's KCQs). The gfx ring (W12k) is already shared and already
     names the VMID in the IB packet.
   - Rebinding is done by MMIO on an *idle* VMID first (the same plain register writes `gfxhub_v12_0_setup_vm_pt_regs` does and
     that the card already accepted for VMID 8 in W36/W40), with the in-ring amdgpu sequence as an optional second phase.
   - Clients become dynamic objects; page tables become demand-allocated. Client count is then bounded by memory, not hardware.
     At most 15 clients have a *bound* VMID at any instant; the rest are bound on their next submit.
4. The design rests on **two blocking assumptions that have not been shown on this card** (U1, U2 in section 8) and a third for the rebinding half (U3).
   U1 and U2 can be tested cheaply and early (step S1) before anything depends on them.
5. The emulator (`emu/qemu/rdna4.c`) **cannot show a missing flush**: it has no TLB, it acks invalidations instantly, it banks
   `SH_MEM_*` by queue instead of by VMID, it has no `IH_VMID_LUT`, and it rejects a ring-level flush for a VMID other than the
   IB's own. Section 7 lists the changes that are needed to model this faithfully; without the TLB model a passing emulator
   run for this work is circular (the W36 lesson, `docs/rootcause/verify-vm.md` s.7).
6. Found on the way (not W13 itself, but W13 must not build on it): **`rtFree` of a device buffer unmaps the PTEs and frees the
   VRAM without invalidating the client's TLB** (`src/runtime.cpp:2093-2111`; only host buffers invalidate). [READ]. Whether
   the stale translation can be used is [INFER] (no TLB in the emulator, never probed on the card). Fix is one line per path
   and is step S0.

## 1. Current limits (what caps the number of clients)

All [READ] unless noted. "Binding" = what actually stops client N+1.

| # | Limit | Where | Value | Binding? |
|---|---|---|---|---|
| 1 | MEC HQDs | `src/runtime.cpp:1507-1519`; `gfx_v12_0.c:1415-1423` (GC 12.0.0/12.0.1: 1 MEC × 2 pipes × 4 queues) | 8 HQDs; (pipe 0, queue 0) is the kernel's boot queue ⇒ **7 client queues** | **yes (7)** |
| 2 | VMIDs handed out | `runtime.cpp:1503-1506` (`for vmid = 8; vmid <= 15`) | 8 | no (8 > 7), but the same order of magnitude |
| 3 | `kMaxClients` | `compute.hpp:662`, array `clients[kMaxClients]` at `compute.hpp:728` | 8, static array of `RtClient` | yes, if 1-2 were lifted |
| 4 | Per-client pool slot | `compute.hpp:670-671` (`kVmQueueBase` 26 MiB, stride 64 KiB), check only against `pool.size` at `runtime.cpp:1523` | 4 MiB / 64 KiB = **64 slots** before the next slot lands in the gfx region at 30 MiB (`kGfxOffset`, `compute.hpp:375`). The check does not catch this: raising `kMaxClients` past 64 silently overlaps the gfx ring. | latent hazard |
| 5 | Page tables per client | `compute.hpp:663` `kVmTableBytes` = 4 MiB in VRAM (`devHeap.alloc`, `runtime.cpp:1521`) **plus** a 4 MiB `IOMalloc` shadow (`runtime.cpp:1534`) | 8 MiB per client, 4 MiB of it wired kernel memory. 100 clients = 400 MiB wired. | scales badly |
| 6 | VA per client | `runtime.cpp:509-511`: PT page at `0x3000 + ((va - kVaStart) >> 21) * 0x1000` must end ≤ 4 MiB | 1021 PT pages × 2 MiB ≈ **2042 MiB** of VA per client | related: table redesign |
| 7 | Global (not per-client) caps | `compute.hpp:660-661` `kMaxBuffers` 256, `kMaxPrograms` 32; `kHostMaxTotal` 4 GiB (`:681`) | shared by all clients | yes at ~10 clients |
| 8 | Doorbells | client doorbell `(0x0d + slot) * 2` (`runtime.cpp:1571`); MEC range 0..0x8a (`gfxregs.hpp:388-391`); amdgpu reserves `USERQUEUE_START 0x00D … USERQUEUE_END 0x08A` (`amdgpu_doorbell.h:202-203`) | 126 user-queue doorbells | no (shared queues need ~4) |
| 9 | Global serialisation | `rtDispatch` calls `launch()` under `rtLock` and waits for the fence there (`runtime.cpp:2313`, `compute.cpp:3149-3166`); `rtWaitFence` also waits under `rtLock` (`runtime.cpp:2393-2428`) | one dispatch or wait at a time, for all clients. The present timer only `TryLock`s it (`runtime.cpp:1684`) and retries. | yes for latency |
| 10 | Global hang state | `rtWedged` (`compute.hpp:719`), set when one queue's recovery fails (`runtime.cpp:2323`, `:2460`) | one bad client wedges every client | yes for robustness |
| 11 | GFXOFF | `gfxOffAllowedNow` (`compute.cpp:1321-1336`): refused while *any* client is open, "its HQD/SH_MEM/VMID state would not be restored after a wake" | GFXOFF off for the whole session once a client is open | power (W4) |
| 12 | Root only | `userclient.cpp:47-52` (`clientHasPrivilege(... Administrator)`) | a Metal process is not root | outside W13 (see section 8) |

Hardware limits behind the software ones:

- **15 non-zero VMIDs** on the GFX hub (`GCVM_CONTEXT1..15`, `gfxregs.hpp:221-222`). VMID 0 is the kernel/physical context. [READ]
- **18 TLB invalidation engines** `GCVM_INVALIDATE_ENG0..17` (`gc_12_0_0_offset.h:2962-2999`, regs 0x1647..0x1658 + ACK 0x1659..).
  amdgpu gives each ring its own engine (`amdgpu_gmc_allocate_vm_inv_eng`, `amdgpu_gmc.c:652-718`) and keeps 17 for MMIO flushes
  (`gmc_v12_0_flush_vm_hub`, `gmc_v12_0.c:213-260`, "Use register 17 for GART"). The kext uses only 17 (`runtime.cpp:688`).
- amdgpu's own split: `gmc_v12_0.c:951-961` "amdgpu graphics/compute will use VMIDs 1-7, amdkfd will use VMIDs 8-15".
  The kext uses **only 8-15**: VMIDs 1-7 are never used by the kext, so they are an unused budget. [READ]

What is **not** a limit, and should not be fixed "while here": the page-table format, the PTE flags (W36 IS_PTE, W40 EXECUTABLE), the
SDMA path that writes tables (`vmTableSync`, `runtime.cpp:635-664`). The design keeps all of them.

What has been shown on the real card: one translated MEC queue, VMID 8, pipe 0 queue 1, doorbell 0x1a, in `vmBootSelfTest`
(`runtime.cpp:742`) — round 6, "VM FIXED: rdna4-vm=1 boot self-test PASS" (project memory 2026-09-30). **No checked-in card log shows
`rtOpen`'s path** (`client queue activated`: zero hits in `docs/hw-logs/`), a second VMID, a second translated queue, or VMIDs 1-7/9-15
running anything. The two-client selftest in `userspace/rdna4-run.c:482-533` ("two VM clients dispatched concurrently") is part of real-card
boot 2 (`docs/real-card-plan.md`), but I found no log showing it ran on the card; in the emulator the two "concurrent" clients are serialised by
limit 9 anyway. [MEASURED absence of logs in the repo; I cannot rule out logs kept elsewhere]

## 2. How amdgpu does it (7.2.2, read)

### 2.1 VMID manager — `amdgpu/amdgpu_ids.c`

- `amdgpu_vmid_mgr_init` (`:574-612`): per hub, a list `ids_lru` of VMIDs `1 .. num_ids-1` (`num_ids = first_kfd_vmid` = 8 on GC 12.0;
  VMID 0 skipped, "it is the system VM"). Each has `owner`, `pd_gpu_addr`, `flushed_updates`, `last_flush` (fence), `active`
  (sync object of the fences of jobs on it), `pasid`, `pasid_mapping`.
- `amdgpu_vmid_grab` (`:385-450`), under `id_mgr->lock`:
  1. `amdgpu_vmid_grab_idle` (`:208-246`): find an **idle** VMID (`amdgpu_sync_peek_fence(&id->active, …)` returns no fence), scanning
     the LRU from the tail. If none is idle, return a fence to wait on (the oldest one) and remember it in `ring->vmid_wait` so *every*
     later submitter waits too ("let everybody wait for fairness").
  2. `amdgpu_vmid_grab_used` (`:322-377`): look for a VMID already owned by this VM (`id->owner == vm->immediate.fence_context`) whose
     `pd_gpu_addr` matches the job's. It is reusable as-is unless `needs_flush`: no `last_flush`, `flushed_updates < amdgpu_vm_tlb_seq(vm)`
     (PTEs were removed since the last flush), or the last flush was on another ring and is unsignalled. With `concurrent_flush`
     (true on GC 12: `amdgpu_vm.c:2840`) a needed flush is allowed on a used VMID.
  3. Otherwise take the idle VMID found in step 1 and set `job->vm_needs_flush = true`.
  4. `list_move_tail` (LRU), record `pd_gpu_addr`, `owner`, `flushed_updates`, `job->vmid`, `job->pasid`.
- `amdgpu_vmid_had_gpu_reset` (`:171`): after a GPU reset every VMID is treated as needing flush + pasid remap + GDS switch.
- `amdgpu_vmid_grab_reserved` / `alloc_reserved` (`:257`, `:473`): one VMID pinned to a VM (debug/SPM). Not needed here.
- PASID: `amdgpu_pasid_alloc` (`:63`) — a global cyclic id per VM, used by the IH to attribute faults (2.4).

### 2.2 Per-job flush — the ring sequence

`amdgpu_ib_schedule` (`amdgpu_ib.c:203-225`): `need_pipe_sync = explicit dependency || context switch || amdgpu_vm_need_pipeline_sync`,
then `amdgpu_vm_flush`.

- `amdgpu_vm_need_pipeline_sync` (`amdgpu_vm.c:741-762`): true if the job's VMID is non-zero and (`vm_needs_flush`, or a compute-VM bug
  workaround, or a GPU reset since last use).
- `amdgpu_vm_flush` (`amdgpu_vm.c:772-…`), in emission order: `COND_EXEC` (reset skip) → **pipeline sync** (wait for the ring's previous
  fence: the VMID is about to change) → **`emit_vm_flush`** (if the VMID was rebound or a flush is needed) → **pasid mapping** if the VMID's
  PASID changed → fence for the flush (`job->hw_vm_fence`, stored in `id->last_flush`).
- `gfx_v12_0_ring_emit_vm_flush` (`gfx_v12_0.c:4592-4604`) = `gmc_v12_0_emit_flush_gpu_tlb` (`gmc_v12_0.c:388-434`) then, **gfx ring only**,
  `PFP_SYNC_ME`. And `gmc_v12_0_emit_flush_gpu_tlb` is: `WRITE_DATA reg GCVM_CONTEXT<vmid>_PAGE_TABLE_BASE_ADDR_LO32`, `…_HI32`, then
  `WAIT_REG_MEM` in **write-register-and-wait** form on `GCVM_INVALIDATE_ENG<ring>_REQ/ACK` (`req = gfxhub_v12_0_get_invalidate_req(vmid, 0)`,
  `gfxhub_v12_0.c:61-79`: legacy flush, `PER_VMID_INVALIDATE_REQ = 1 << vmid`, L2 PTEs, PDE0-2, L1 PTEs, fault-status clear 0; ack mask
  `1 << vmid`). No semaphore on the GFX hub (`gmc_v12_0_use_invalidate_semaphore` is MMHUB-only, `:190-195`).
- `gmc_v12_0_emit_pasid_mapping` (`gmc_v12_0.c:436-447`): `WRITE_DATA reg IH_VMID_0_LUT + vmid` = pasid.
- The IB packet: `gfx_v12_0_ring_emit_ib_gfx` (`gfx_v12_0.c:4497-4518`: `control = length | vmid << 24`, **no VALID bit**) and
  `gfx_v12_0_ring_emit_ib_compute` (`:4520-4536`: `INDIRECT_BUFFER_VALID | length | vmid << 24`).
- Queues carry VMID 0: `cp_gfx_hqd_vmid = 0` (`gfx_v12_0.c:3007`), `cp_hqd_vmid = 0` (`:3268`) and `CP_MQD_CONTROL.VMID = 0` (`:3210`);
  compute kernel queues have `PRIV_STATE = 1, KMD_QUEUE = 1` in `CP_HQD_PQ_CONTROL` (`:3229-3230`). **The VMID a job runs in comes from the
  IB packet, not from the queue.**

**Cross-check against the card.** The compositor's submission in `docs/hw-logs/2026-09-30-linux-gfx-ring/decoded.txt` (ring at dword 1280,
VMID 5) is exactly this sequence; with the 7.2.2 headers the numbers decode [MEASURED + READ]:

| Ring packet (measured) | What it is | Confirmed by |
|---|---|---|
| `WAIT_REG_MEM 0x113 mem 0x10c0 == 0xa90eb` | `emit_pipeline_sync`: wait ring fence = `sync_seq` | `gfx_v12_0.c:4570-4577` |
| `WRITE_DATA 0x28f9 = 0xf7deb001`, `0x28fa = 3` | `GCVM_CONTEXT5_PAGE_TABLE_BASE_ADDR_LO32/HI32` (ctx base reg 0x168f + 2×5 = 0x1699, + seg base 0x1260 = 0x28f9) | `gc_12_0_0_offset.h:3126` |
| `WAIT_REG_MEM 0x143 write 0x28a7 = 0x00f80020, wait 0x28b9 & 0x20` | `GCVM_INVALIDATE_ENG0_REQ` (0x1647 + 0x1260 = 0x28a7) / `ENG0_ACK` (0x1659 + 0x1260 = 0x28b9); `0x00f80000` = the five invalidate-level bits, `0x20` = VMID 5 | `gfxhub_v12_0.c:61-79` |
| `PFP_SYNC_ME` | end of `ring_emit_vm_flush` | `gfx_v12_0.c:4598-4602` |
| `WRITE_DATA 0x10a5 = 0xf` | `IH_VMID_0_LUT + 5` = PASID 15 (OSSSYS seg-0 base 0x10a0 + `regIH_VMID_0_LUT` 0x0000 + 5) | `osssys_7_0_0_offset.h:30`, `emu/qemu/rdna4.c:310`. The task notes called this "not confirmed"; it is now. |
| `RELEASE_MEM … 0xa90ec` | the flush's own fence (`hw_vm_fence`) | `amdgpu_vm.c` |
| `INDIRECT_BUFFER 0x05000bd0` | length 0xbd0, VMID 5 in [27:24], no VALID/PRIV | `gfx_v12_0.c:4497` |

The gfx ring uses **engine 0**; the kext's MMIO path uses engine 17. The HDP `WAIT_REG_MEM 0xe26/0xe27` in the capture is not part of
the VM flush.

### 2.3 TLB-sequence tracking

`amdgpu_vm_tlb_seq` (`amdgpu_vm.h:636`) is bumped, through `amdgpu_vm_tlb_flush`/`amdgpu_vm_tlb_seq_cb` (`amdgpu_vm.c:1025-1075`), when a page-table
update with `needs_flush` completes: `flush_tlb = clear` (PTEs being cleared, `:1272`), a moved BO (`:1346`), or a freed PT (`amdgpu_vm_pt.c:926`).
A VMID remembers `flushed_updates`; if `flushed_updates < tlb_seq` the next job on it flushes. Mapping *new* pages (invalid → valid) does not bump
it, so amdgpu does not rely on invalid entries being flushed. [READ for the mechanism; "the hardware does not cache invalid PTEs" is an INFER from that
design choice, not something measured here.]

### 2.4 PASID and the IH

PASID is what the IH stamps into a fault vector: `gmc_v12_0_process_interrupt` (`gmc_v12_0.c:92-160`) logs `entry->vmid`, `entry->pasid`,
looks the task up by PASID, reads and clears `GCVM_L2_PROTECTION_FAULT_STATUS`. The vector carries `vmid` and `pasid` (`ih.hpp:28-36`,
`ihdecode.cpp:15-28` decode both; the kext does not program the LUT, so `pasid` is always 0 today). With per-job VMIDs the LUT is
what keeps a *late* fault attributable after the VMID was rebound.

### 2.5 KFD and MES (the other two models)

- **KFD without HWS** (`amdkfd/kfd_device_queue_manager.c:673-700`, `allocate_vmid`, called from `:782`): first free VMID in
  `first_vmid_kfd..last_vmid_kfd` (8-15), else `-ENOSPC` ("no more vmid to allocate"); mapped to the PASID with `set_pasid_vmid_mapping`, the
  page-table base written by MMIO through `kfd2kgd->set_vm_context_page_table_base` (`:711`, then `kfd_flush_tlb`). **This is the model the kext currently
  has** (fixed VMID per client, hard failure when out) and it is why the cap is 8. Note that **gfx12's `gfx_v12_kfd2kgd` does not provide
  `set_vm_context_page_table_base` or `get_atc_vmid_pasid_mapping_info`** (`amdgpu_amdkfd_gfx_v12.c:370-385`): on GC 12 KFD needs MES/HWS, which maps
  processes and assigns VMIDs itself. So the kext's model is the pre-HWS one, for an ASIC whose upstream driver no longer uses it. The page-table-base MMIO
  write itself is `gfxhub_v12_0_setup_vm_pt_regs` (`gfxhub_v12_0.c:126-139`), which amdgpu calls at init (VMID 0 GART, `:145`) and per VMID in setup.
  KFD user queues use `CP_MQD_CONTROL.PRIV_STATE` only (`kfd_mqd_manager_v12.c:124`); `PRIV_STATE | KMD_QUEUE` in `CP_HQD_PQ_CONTROL` is for the HIQ
  (`:301`), as for amdgpu's KCQs (`gfx_v12_0.c:3229-3230`). The kext's client queues are configured like kernel queues (`compute.cpp:2730-2732`).
- **MES** (`amdgpu/amdgpu_mes.c:109-182`): the MES firmware owns HQD and VMID masks (`vmid_mask_gfxhub = all & ~reserved`, `compute_hqd_mask`,
  `gfx_hqd_mask`), maps/unmaps queues and oversubscribes them; amdgpu reserves invalidation engines 5 and 6 for it
  (`amdgpu_gmc.c:664-667`). GC 12 *requires* MES for kernel queues upstream. **The kext loads the MES image through the PSP but never starts it**
  (`amdfw.cpp:204-212`; the only MES register touched is a read of `CP_MES_CNTL`, `compute.cpp:182`), and all queues are MMIO-activated
  HQDs ("no-MES", `compute.cpp:2885-2895`, `runtime.cpp:2850-2869`). [READ]

## 3. What the VMID is used for in this kext, and how it couples to everything else

The VMID in this kext appears in: the client's `GCVM_CONTEXTn` (`vmContextInit`, `runtime.cpp:666`), the HQD (`CP_HQD_VMID`, `compute.cpp:2815`, and
the MQD dword 131), `GRBM_GFX_CNTL.VMID` for the *banked* `SH_MEM_CONFIG/SH_MEM_BASES` written at every dispatch/IB submit
(`compute.cpp:3101-3104`, `runtime.cpp:2373-2375`; once for 8-15 in the boot test, `runtime.cpp:820-825`), the IB packet (`pm4.cpp:118-131`),
`SQ_CMD` wave kill with `CHECK_VMID` (`compute.cpp:2903`), TLB invalidations (`vmInvalidate`), and fault decode (`logClientFault`,
`runtime.cpp:1462`).

Couplings the design has to respect:

| Area | Today | Constraint for W13 |
|---|---|---|
| **Queue addresses** | The client queue's PQ base, EOP, rptr-report and wptr-poll are **VAs in the client's VM** (KFD style: `runtime.cpp:1561-1566`; only the MQD is a VMID0 address) | A kernel-owned shared queue (HQD VMID 0) uses plain MC addresses: the four per-client mappings and the 64 KiB pool slot disappear |
| **Dispatch** | `rtDispatch` emits `SET_SH_REG… DISPATCH_DIRECT` **straight into the client's queue**, which runs as the queue's VMID (`compute.cpp:3114-3134`) | On a VMID-0 queue those packets would run as the kernel. Every job must be wrapped in an IB that names the VMID (as amdgpu does). `rtSubmitIb` already builds `ACQUIRE_MEM, INDIRECT_BUFFER(vmid), RELEASE_MEM` (`runtime.cpp:2378-2382`) |
| **Fences** | `RELEASE_MEM` to `c->fenceVa`, a VA in the client VM | Fence written by a ring-level `RELEASE_MEM` (VMID 0, kernel address) after the IB, as `amdgpu_fence_emit` does |
| **Faults** | Attributed by the VMID in the status register (`runtime.cpp:1462-1471`) | Must harvest and clear the fault status **before** a VMID is rebound; program `IH_VMID_LUT` so a vector is attributable to a client |
| **Hang recovery** | per-queue `recoverComputeQueue` (`compute.cpp:2885-2938`): `RESET_WAVES` dequeue + `SQ_CMD` kill with `CHECK_VMID`, re-init the client's HQD, fenced `WRITE_DATA` proof; failure sets global `rtWedged` | A shared queue's reset affects every client with work on that queue; `SQ_CMD CHECK_VMID` becomes the precise tool for the guilty VMID. `rtWedged` must become per queue |
| **Flip / display** | `rtPresent*` use `RtBuffer::mc` (physical MC of a VRAM buffer) into DCN; no VMID | Independent of client VMs. Only coupling: the present timer `TryLock`s `rtLock` (`runtime.cpp:1684`), so W13's lock split also helps vblank latency |
| **W12k gfx IBs** | One shared gfx ring (`gfxRing`, 16 KiB, doorbell 0x8b·2, HQD VMID 0, `gfxring.cpp:117`); IB packet names the VMID (`pm4.cpp:118`, matches the captured packet); `docs/w12k-gfx-submit.md` | The gfx side is *already* "queue shared, VMID per IB". W13 only has to supply the VMID binding and, if needed, the in-ring flush preamble before the IB. Contract in section 5.6 |
| **Sleep/wake, GFXOFF** | `powerWillSleep` drains every client HQD (`runtime.cpp:2712-2776`); `resetRuntimeForResume` marks every client `aborted` and frees their allocations (`:2778-…`); GFXOFF refused while a client is open | With lazy binding the VMID/context state is *soft*: after a wake mark all VMID slots unbound and rebind on the next submit. This can lift the GFXOFF restriction for idle clients (W4 decides) |
| **SDMA / DMA** | VMID 0, physical MC addresses through the bounce buffer; page tables written by SDMA COPY_LINEAR (`vmTableSync`) | unchanged |

## 4. Designs

### Design A — keep fixed VMIDs and per-client HQDs, use VMIDs 1-15

Change `rtOpen` to hand out 1-15. Leaves HQDs (7) as the cap.

- (+) Tiny change; uses the 7 unused VMIDs. (−) **Does not raise the cap** (HQD-bound: 7). (−) VMIDs 1-7 would be in use on the card for the
  first time with no other change [UNPROVEN that they behave like 8-15; Linux's compositor uses VMID 5 on this card, [MEASURED]].
- **Rejected** as a goal. Its one useful part (use 1-15, init `SH_MEM_*` for all 15 once like `gfx_v12_0_constants_init`, `gfx_v12_0.c:1836-1846`) is
  kept by B.

### Design B — per-job VMID binding + shared kernel queues (amdgpu's model) — **recommended**

Two independent mechanisms, each testable alone:

**B-V (VMID virtualisation).** A global pool over VMIDs 1-15, amdgpu's rules (2.1) adapted to a kernel that submits under its own lock:
a slot is `{owner, pdPhys, boundSeq, lastUse, pending[fence domain]}`; a VMID is *idle* when every fence domain that ever received a job for it has
signalled. `grab(client)`:
1. a slot with `owner == client` and the same PD ⇒ reuse; flush only if `slot.boundSeq < client.tlbSeq` (amdgpu `grab_used`);
2. else the least-recently-used idle slot ⇒ rebind to this PD (write `GCVM_CONTEXTn_CNTL/BASE/START/END`, `SH_MEM_*` already set at init, `IH_VMID_LUT`),
   invalidate, record `owner/pd/boundSeq` (`grab_idle` + `needs_flush`);
3. else none idle ⇒ the submitter waits on the oldest pending fence (amdgpu's `vmid_wait`, with the same fairness rule), or returns `kIOReturnBusy`.

Steady state (a client running alone, or ≤15 active clients) costs **nothing per job**: the VMID stays bound, exactly like today.

*How the rebind is made* — two variants:

- **B-V1, MMIO rebind of an idle VMID** (recommended first): the CPU writes the context registers and invalidates via engine 17, under the lock,
  only when the pool says the VMID has no in-flight work. The writes are what `vmContextInit` + `vmInvalidate` already do and what
  `gfxhub_v12_0_setup_vm_pt_regs` is; W36/W40 proved them on the card for VMID 8 while the boot queue was idle. (Older KFD does the same per process,
  but not gfx12's KFD — section 2.5 — so there is **no upstream gfx12 precedent for rebinding a VMID by MMIO while other VMIDs run**: U3.) No ring
  packets, no `PFP_SYNC_ME`, runs in the existing emulator. Cost: ~7 register writes + an ack poll (tens of microseconds, not measured) per *rebind*,
  paid only on VMID contention.
- **B-V2, in-ring rebind** (amdgpu-faithful, phase 2): emit `pipeline_sync, WRITE_DATA ptb lo/hi, WAIT_REG_MEM req/ack, [PFP_SYNC_ME], WRITE_DATA IH_LUT` ahead of
  the IB, so a VMID can be re-targeted while earlier jobs are still queued (deep pipelining, no CPU wait for idleness). Needed only if B-V1's "wait until the
  VMID is idle" shows up in measurements. The exact bytes are in section 2.2 and the capture.

**B-Q (queue virtualisation).** Compute clients no longer own HQDs. The kext owns N shared MEC queues (proposal: N = 2, one per MEC pipe so the two pipes
run concurrently; keep the rest free for the boot/recovery queue and dedicated clients, design C). Each HQD: VMID 0, `CP_MQD_CONTROL.VMID = 0`,
`PRIV_STATE | KMD_QUEUE` (what `hqdInitFor` already programs, `compute.cpp:2730-2732`), PQ/EOP/rptr/wptr at MC addresses in the kernel pool.
A job is `[pipeline_sync if the VMID was rebound] ACQUIRE_MEM, INDIRECT_BUFFER(vmid, client IB), RELEASE_MEM(fence seq)`. `rtDispatch`'s packets move
into a kernel-written per-client IB page mapped in the client VM (plus the existing kernarg page), so a dispatch is a job like any other.
Clients are then bounded by memory, not by HQDs.

*Trade-offs:*

- (+) Client count unbounded by hardware; ≤15 bound at once, rebinding by LRU. Shared queues share the gfx ring's model, so W12k needs nothing client-specific.
- (+) Per-client kernel footprint drops (no 64 KiB pool slot, no 4 mappings, no HQD).
- (+) One precise hang tool, `SQ_CMD CHECK_VMID`, which works *because* the guilty VMID is known at timeout.
- (−) Head-of-line blocking inside a shared queue: a long IB delays later jobs on that queue (two queues help; priority needs design C).
- (−) A queue reset hits every client with jobs on it; innocent jobs must be failed (first version) or re-emitted (amdgpu does, `amdgpu_ring_reset_helper_end`).
- (−) Depends on U1/U2 (section 8): **[UNPROVEN on this card]** that an IB packet's VMID on a VMID-0 `PRIV_STATE` MEC queue is honoured as amdgpu relies on, and that VMIDs 1-7 and 9-15 translate like 8.
- (−) All job waits, fence bookkeeping and recovery must be restructured around fences that are not per-client (section 5).

### Design C — B plus pinned clients

B, plus up to K (proposal K = 2) **dedicated** HQD + fixed VMID clients for latency-critical processes (WindowServer/compositor), granted at open when a
flag asks and resources exist. Remaining clients use B. This is B + today's code path kept alive as a fast path. Decide **after** B works: if B's steady state
is indistinguishable (it should be: bound VMID, shared queue with 2 HQDs), C is not needed and the old path can be deleted. Costs: two code paths to keep
correct in the emulator, and the dedicated clients still cannot be rebound.

### Design D — MES-scheduled user queues

Start the MES (the PSP already loads its firmware), let it own HQD/VMID masks, map per-client user queues and oversubscribe (upstream's gfx12 model, KFD
HWS). (+) Would give preemption/priorities/queue oversubscription properly. (−) The MES is not running anywhere in this project; bringing up
`mes_v12_0` (KIQ/scheduler rings, `SET_HW_RSRC`, `ADD_QUEUE`, doorbells, its own invalidation engines 5/6, reset handling) is a workstream of its own with
the real card as the only judge and no emulator support; one wedged MES is a dead GPU until reboot. **Rejected for pre-Metal; revisit for Metal** if queue
priorities or preemption turn out to matter.

### Comparison

| | A | **B** | C | D |
|---|---|---|---|---|
| Max clients | 7 | memory-bound (≥100) | memory-bound | memory-bound |
| Bound VMIDs at once | 7 | 15 | 15 | MES-decided |
| Per-job cost, steady state | none | none | none | none |
| Needs MES | no | no | no | **yes** |
| New card behaviour relied on | VMIDs 1-7 | IB-VMID on VMID-0 queue (U1), VMIDs 1-15 (U2) | same as B | everything MES |
| Emulator can test it | yes | yes, **after** the TLB/INV model (section 7) | yes | no |
| Isolation of a hang | per client | per queue (kill by VMID) | per queue; pinned clients separate | MES |
| Effort | small | medium | B + medium | large, open-ended |

## 5. Recommended design in detail (B, with C left as a later option)

### 5.1 Objects

- `RtClient` becomes a heap object, found through a table keyed by owner (linear or hash; ≤ a few hundred). Holds: PD (root MC/physical), page-table
  state (5.2), `tlbSeq`, `hostBytes`, fence counter, a small in-flight job ring (client fence value → `{queue, seq}`; replaces `ibFences[16]`), the kernarg
  page and kernel-IB page, `aborted`. **No** `vmid`, `pipe`, `queue`, `doorbell`, `pm4`, queue VAs.
- `VmidPool` (new, freestanding `src/vmid.{hpp,cpp}` so the policy is host-testable the way `gpuvm.cpp` is, through `tools/atomdump.cpp`/`make test`):
  15 slots, `grab`, `noteSubmit(vmid, queue, seq)`, `forget(client)` (on close), `unbindAll()` (after wake/reset). Policy only; it takes a
  "has this fence been reached" callback and never touches hardware.
- `CmdQueue[]` (new): the shared compute queues and the gfx ring as two kinds of the same thing: a ring (`Pm4::Queue`), a kernel fence address + next
  seq, a doorbell, a state `{ok, resetting, wedged}`, a per-queue in-flight job list. Queue 0..N-1 = compute, queue N = gfx (W12k). The pool's "fence
  domains" are these.
- `rtLock` stays the coarse lock for table state, but **it is not held across a wait** (today it is, section 1 #9). Waits happen on fence memory with
  the lock dropped; recovery re-takes it. This is a prerequisite for anything with more than one client, and it is independent of the VMID work (step S3).

### 5.2 Page tables: demand-allocated, smaller

Keep the gfx12 format, PTE flags, `vmMap`/`vmUnmap` semantics and the SDMA sync. Change the *storage*: root (PDB2) + PDB1 + one PDB0 page allocated up
front (12 KiB); PT pages allocated from a 4 KiB-granule VRAM page-table pool when a 2 MiB region is first mapped, with a shadow only for pages that exist
(or no shadow, writing entries through SDMA directly). That removes the 4 MiB+4 MiB per client and the ~2 GiB VA ceiling (limits 5, 6). Needs a small
allocator (the existing `GpuHeap` granule is 64 KiB, `compute.hpp:795`). Independent of B-V/B-Q; required before 50+ clients are sensible.

### 5.3 Open / close

`rtOpen`: allocate the client object, PD, kernarg + IB pages, map them; **no HQD, no VMID**. Cannot fail for a hardware reason.
`rtRelease`: for every buffer/program `vmUnmap` and bump `tlbSeq`; wait for the client's in-flight jobs (or recover); `VmidPool.forget(client)` — if the client
owned a bound VMID, harvest faults for it, zero the context (`CNTL = 0`), invalidate, return the slot; then free tables. (Today's order — clear tables, invalidate,
free VMID — is kept: `runtime.cpp:2679-2706`.)

### 5.4 Submit (compute), `rtSubmitIb` / `rtDispatch`

1. Validate as today (owner, IB inside a buffer of this owner, length, outstanding ≤ limit).
2. `grab(client)` → `{vmid, needsBind, needsFlush}` or Busy. If `needsBind`: harvest + clear any latched fault for the old owner (attribute it), then B-V1 rebind
   (context regs, `IH_VMID_LUT[vmid] = client id`), invalidate, log `bind`. If `needsFlush` only: invalidate.
3. Pick the compute queue (round-robin / least outstanding). Emit `[ACQUIRE_MEM] INDIRECT_BUFFER(vmid, ibVa, dwords) RELEASE_MEM(queue fence, seq)`.
   For `rtDispatch`: first write the dispatch packets into the client's kernel-IB page (the `Launch` packet list from `launch()`, without the `RELEASE_MEM`).
4. `noteSubmit(vmid, queue, seq)`; record `clientFence → {queue, seq}`; kick the doorbell. Return.

`SH_MEM_CONFIG/BASES` are programmed **once for VMIDs 1-15 at runtime init** (as `gfx_v12_0_constants_init` and `init_compute_vmid` do, `gfx_v12_0.c:1836-1846`,
`:1773-1798`) and removed from the submit path (saves two banked MMIO writes plus a `GRBM_GFX_CNTL` select per submit, and the shared global register
they contend on).

### 5.5 Wait, fault, hang

- **Wait**: client fence → `{queue, seq}` → compare the queue's fence memory (`fenceReached`), sleeping on the IH event as now, lock dropped.
- **Fault**: IH vector with `vmid` + `pasid` (from `IH_VMID_LUT`) ⇒ the client; the next submit/wait of that client sees it. On rebind the old owner's latched
  status is harvested first (2.4, section 3). The fault-default-page scrub (`scrubFaultPage`, `runtime.cpp:1479`) stays.
- **Hang** (job timed out on queue Q, guilty = the VMID recorded for that seq): `SQ_CMD kill CHECK_VMID=guilty`, `RESET_WAVES` dequeue, re-init Q with the
  **same** ring (`hqdInitFor` with VMID 0), fenced `WRITE_DATA` proof (W6 logic, `compute.cpp:2885-2938`). Every other job with seq > guilty on Q is failed with a
  distinct status (first version) — re-emission is a later improvement. The guilty client is marked `aborted` (as a wedged client is today); **other queues
  and clients keep running**. `rtWedged` becomes `CmdQueue::state`; only a failed recovery of Q wedges Q. `rdna4-hang=1` gating stays.

### 5.6 Interaction with W12k (gfx submits by clients)

The gfx ring is one shared ring and the IB packet already names the VMID. The contract between W12k and W13:

```
struct VmBinding { uint32_t vmid; bool rebound; };            // what grab() decided
bool vmidGrab(RtClient &c, VmBinding &out, Waiter *&wait);     // same pool as compute
```

- **W12k first (fixed VMIDs)**: its submit path keeps using the client's fixed VMID; nothing to do now. Anvil's code should only call `indirectBufferGfx(…, vmid)`
  with `vmid` taken from one function (`vmidForSubmit(client)`), not from `c->vmid` scattered, so B can substitute the grab.
- **Under B** the gfx job is `[pipeline_sync if rebound] IB(vmid) RELEASE_MEM(gfx fence seq)` with `noteSubmit(vmid, GFX, seq)`. When a VMID is rebound while earlier
  *gfx* jobs on it are still queued, B-V1 is not allowed (not idle) → wait, or use B-V2's in-ring sequence (section 2.2, including `PFP_SYNC_ME`, which only the gfx
  ring needs). One fence-domain list per VMID handles gfx and compute together, as amdgpu's one `id_mgr` per hub does.
- The unprivileged-register check (`PRIV_REG`, emulator `rdna4_gfx_reg_allowed`) applies to the IB, never to the kernel's preamble, which is in the ring.

### 5.7 Display / flip

No change to `rtPresent*` (physical MC addresses, DCN). Two consequences to keep: (a) freeing a buffer that is on scanout must still restore presentation
(existing logic), now also invalidating the owner's TLB; (b) dropping `rtLock` across waits lets `presentTimerTick` make progress at vblank cadence.

### 5.8 Power

`powerWillSleep` drains the shared queues (few) instead of a loop over clients; `resetRuntimeForResume` calls `unbindAll()`; clients stay valid (today they are all
`aborted`, `runtime.cpp:2799-2810`) **only if** their page tables survive the sleep (VRAM content across S3 is a W4 question — not assumed here).
GFXOFF gating can be relaxed from "any client open" to "any VMID bound or job in flight" once B-V is in, because the soft state can be rebuilt.

## 6. Implementation plan — small steps, each with a test

"Emu" = QEMU `rdna4` device + macOS guest (the Linux emulator loop being built by Kiln, workstream #1) and the host unit tests
(`make test`, extended like `gpuvm.cpp` via `tools/atomdump.cpp`). "Card" = needs the real card, only after the lead's OK.
Every step is behind a boot-arg until the last (`rdna4-vmid=1`); default behaviour does not change until step S9.

| Step | Change | Emu test | Card proof needed |
|---|---|---|---|
| **S0** | `rtFree` (device buffers) and `vmUnmap` users bump a per-client `tlbSeq`; invalidate before the next submit / at free (amdgpu's `flushed_updates < tlb_seq`). Bug fix, no new design. | needs the TLB model (S4a) to *fail before / pass after*; until then a host test of the sequencing | none (write-only invalidates are what `host unmap` already does on the card) |
| **S1** | Make E4 a first-class, always-available boot check: queue (0,1) with **VMID 0** fetching an IB in VMID 8 (`rdna4-vm-diag` bit 0 today, only after the baseline fails, `runtime.cpp:834-838`). Add a variant for VMIDs 1 and 15. | emulator already models `PRIV_STATE` queues taking the IB's VMID (`rdna4.c:4205-4210`); both directions (priv and non-priv mismatch) as negative controls | **U1, U2: yes.** One boot of the current round, `rdna4-vmid-test=1`; 3 IBs, no gfx. Tiny blast radius (one HQD, the boot-test queue). |
| **S2** | `SH_MEM_CONFIG/BASES` for VMIDs 1-15 once at runtime init; remove from `launch`/`rtSubmitIb`. | SH_MEM banked by VMID in the emulator (S4c) and a read-back check | folded into S1's boot |
| **S3** | Drop `rtLock` across fence waits and recovery (`rtDispatch`, `rtWaitFence`); per-client state under the lock, waits outside. Split `rtWedged` per queue (still one queue per client at this point). | `rdna4-run` selftest: a client blocked in `WaitFence` must not stall another client's dispatch, nor the present timer; hang injection on one client leaves the other running (`rdna4.c` already has `hang-sticky`) | none until S9 |
| **S4** | **Emulator** fidelity (section 7): (a) TLB with explicit invalidation, (b) all 18 invalidation engines with ack latency, (c) `SH_MEM_*` banked by VMID, (d) `IH_VMID_LUT` + pasid in fault vectors, (e) ring-level flush for any VMID, (f) 15 VMIDs. Each with a negative control. | the new emulator tests themselves: every change must make a *wrong* kext fail (missing invalidate ⇒ stale read) before it is used as evidence | the card-backed claims in section 7 must be attached to each model item |
| **S5** | `VmidPool` (`src/vmid.{hpp,cpp}`) + host tests: reuse, LRU order, steal, idle detection with two fence domains and wrap-around, fairness wait, `forget`, `unbindAll`, tlbSeq-triggered flush. No kext use yet. | host | none |
| **S6** | Clients as dynamic objects and on-demand page tables (5.1, 5.2), still with fixed VMID + own HQD (so ≤7 by hardware). `kMaxClients`, per-slot pool layout and the per-client 8 MiB go away; global `kMaxBuffers/kMaxPrograms` become per-client. | `rdna4-run` with 7 clients; allocation/free churn; VA beyond 2 GiB | none: same hardware behaviour as today |
| **S7** | **B-Q with static VMIDs**: two kernel-owned shared compute queues (VMID 0); job = `[ACQUIRE_MEM] IB(vmid) RELEASE_MEM(queue fence)`; `rtDispatch` through the client kernel-IB page; per-queue fences and in-flight lists; clients lose their HQD and doorbell but **keep a VMID taken at open from 1-15 (through `VmidPool`, pinned)**. Cap becomes 15. | 15 clients, two shared queues, mixed dispatch/IB; a hung IB in one client (kill by VMID, the others finish); queue reset fails innocents with the defined status; host/queue isolation tests | **yes**: first time the client path runs on the card at all; 2-3 clients one after another, then two at once (U1 in anger, U3 not yet needed because nothing rebinds) |
| **S8** | **B-V1 lazy binding**: clients no longer take a VMID at open; `grab` at submit, LRU steal/rebind of idle VMIDs, fault harvest before rebind, `IH_VMID_LUT`. Cap becomes memory. | 16, 32, 100 clients; steal/rebind under load; **TLB-stale negative controls** (skip the invalidate ⇒ wrong data observed, needs S4a); attribution after rebind; fairness wait when all 15 are busy | **yes** for U3: two clients alternately stealing one VMID, with another client's job in flight on a second VMID |
| **S9** | Switch default on (`rdna4-vmid` default 1); delete the dedicated-HQD path unless design C is wanted; update `RDNA4_COMPUTE_ABI` (4 → 5) and `RDNA4_FLAG_VM` docs; GFXOFF gating reworked with W4 | full emulator suite, both paths until removal | folded into the same boot |
| **S10 (opt.)** | B-V2 in-ring rebind and/or design C pinned clients, only if measurements ask for them | emulator (needs S4e) | yes |

Ordering rationale: S0-S3 are valuable on their own and change no policy; S4 must precede any claim from the emulator; S5 is host-only; the first hardware
behaviour change on the card is S1 (a small test of U1/U2), then S7 (queue sharing, VMIDs still fixed, so a failure there is about queues and not about
rebinding), then S8 (rebinding). S6 has no hardware effect. The two halves of B (queue sharing, VMID sharing) thus land on the card one at a time.

## 7. Emulator changes needed to model this faithfully (`emu/qemu/rdna4.c`)

The project's past mistake (W36 review: the emulator was bent until the kext passed, then the real card failed) applies directly: **a VMID-virtualisation design
cannot be validated on an emulator that has no TLB.** Each item below says what the card does (with its evidence) and what the emulator does today.
Tests for the kext must be written so they *fail* on a kext that skips the step.

| # | What the hardware does | Evidence | Emulator today | Change |
|---|---|---|---|---|
| E1 | Translations are cached per (VMID, VA page) in the L1/L2 TLBs and **stay cached until an invalidation names the VMID** (`PER_VMID_INVALIDATE_REQ`) and the levels (`INVALIDATE_L2_PTES/PDE0-2/L1_PTES`); writing `GCVM_CONTEXTn_PAGE_TABLE_BASE` does **not** flush | amdgpu must emit the flush after the base write (`gmc_v12_0.c:388-434`); measured in the ring capture; `amdgpu_vm_tlb_seq` exists because unmaps need flushes | `rdna4_gc_span_vmid` (`rdna4.c:2160`) and `rdna4_vm_target` walk the tables **on every access** from the live registers; a missing or wrong flush can never misbehave | Add a TLB: (vmid, page) → PTE (and PDE-level entries for the PDE0-2 bits), filled on walk, consulted first, removed only by an invalidate matching the VMID and the level bits. No spontaneous eviction by default (stale forever = deterministic), optional seeded random eviction as a stress mode (hardware may evict at any time; a correct kext must not depend on it either way). A negative-permission/invalid PTE is not cached (INFER; matches amdgpu not flushing on map). Faults must not be cached. |
| E2 | 18 invalidation engines, each REQ/ACK, ACK bit per VMID, latency > 0 | `gc_12_0_0_offset.h:2962-3000`; gfx ring uses ENG0, MMIO uses ENG17 | only ENG17 via MMIO acks instantly (`rdna4.c:2831`); ENG0's REQ/ACK only as a fixed pattern inside `WAIT_REG_MEM` (`:5590-5600`) | Implement all 18 engines; REQ write triggers the E1 invalidate, ACK set after a configurable number of work slices (a kext that reads ACK without waiting must fail in some runs); one property to drop the ACK (`inv_noack` exists for ENG17 only) |
| E3 | Ring-level `WRITE_DATA reg` + `WAIT_REG_MEM` write-reg-wait for any VMID/engine in a **privileged** stream | captured sequence (section 2.2) | the ring/priv path accepts the write-wait only when `ref == (1 << st->vmid) \| 0xf80000` and `lo/hi` are ENG0 (`:5590-5597`): with `st->vmid = 0` on the ring, a flush for VMID 5 is **refused** | Accept any engine and any VMID in privileged streams (reject it in unprivileged IBs: a user IB must not be able to flush or rebind) |
| E4 | `SH_MEM_CONFIG/SH_MEM_BASES` (and `SPI_GDBG_PER_VMID_CNTL`) are **per VMID**, banked by `GRBM_GFX_CNTL.VMID` | `gfx_v12_0.c:1773-1798`, `:1836-1846` | banked by `hqd[pipe][queue]` only (`rdna4.c:774-795`, `rdna4_sh_reg`), i.e. by queue; the select's VMID just gates the bank | key `SH_MEM_*` by VMID (16 banks); the shader/dispatch path reads the running VMID's bank |
| E5 | `IH_VMID_LUT[vmid]` = PASID; every VM-fault vector carries `vmid` and the current `pasid` | `gmc_v12_0_emit_pasid_mapping`; capture `WRITE_DATA 0x10a5`; `ih.hpp:28-36` | no LUT; `rdna4_ih_emit_vmid(… vmid …)` (`:2016`) stamps only the VMID | LUT registers at `OSSSYS_SEG0 + 0..15`, stamp `pasid` at vector-generation time (so a vector generated before a rebind keeps the old PASID, as on hardware) |
| E6 | HQD with VMID 0 executing an IB whose packet names VMID *n* (`PRIV_STATE` kernel queue) runs the IB in VMID *n*; an unprivileged queue must match | amdgpu KCQ design (2.2). **[UNPROVEN on this card]** — this is U1 | modelled (`:4205-4210`, `mec_work.priv`) — the model is *a statement of U1*, not evidence for it | keep, and label as an assumption in the source; S1 is its real-card proof |
| E7 | Page-table base and context registers for VMIDs 1-15, per-VMID `CNTL/START/END` | `gfxregs.hpp:221-222` | present (`REG_GCVM_CTX1_*`), VMID 1-15 | OK; add a check that writing CONTEXTn for a VMID with work in flight is *detectable* (a warning counter) — the kext's B-V1 must never do it |
| E8 | Fault status is latched once, sticky until cleared; `MORE_FAULTS` for later ones; VMID field identifies the faulting VMID | `gc_12_0_0_sh_mask.h:9002-9025`; `rdna4_vm_fault` already models the first-fault latch | present | keep; add a test that a rebind between fault and harvest is caught (attribution bug) |
| E9 | CP/SH memory pipeline ordering across `PFP_SYNC_ME`; prefetch past a flush | `gfx_v12_0.c:4598-4602` | the gfx stream engine is a sequential checker, no prefetch | **Not modelled.** Must be stated in the test docs: an in-ring rebind (B-V2) is only as tested as the card makes it |
| E10 | MES not running | project fact | n/a | none (design D is out of scope) |

Also needed for test coverage, not fidelity: a `clients` mode in `rdna4-run` that opens N clients and drives them from one or several threads; hang injection
per client (the emulator has `hang-sticky`, `mec_hung`; per-VMID/per-queue would be better so a kill-by-VMID test is meaningful).

## 8. Unproven assumptions, risks, and open questions for the lead

**U1 (blocks B-Q).** On this card, an `INDIRECT_BUFFER` packet in a MEC queue whose `CP_HQD_VMID` = 0 and `PRIV_STATE | KMD_QUEUE` set runs the IB in the VMID in the
packet. Basis: amdgpu's KCQs work exactly so on gfx12 (2.2), and the gfx ring on this card does it [MEASURED for the gfx ring, VMID 5]. Not measured for a MEC queue; the
W17 "E4" experiment (`runtime.cpp:1130-1199`, the IB in VMID 8 from a VMID 0 queue on pipe 1 queue 3) was written for this and its verdict was never obtained once the
baseline passed. The emulator models it as true (E6), which is circular. *Test: S1.* *Fallback if false:* keep per-VMID HQD binding and virtualise by *re-loading an HQD*
with a new `CP_HQD_VMID` (deactivate, reprogram, reactivate) — slower, still MMIO, still unbounded clients; or design C/D.

**U2 (blocks B-V).** VMIDs 1-7 and 9-15 behave like 8: context registers, `SH_MEM_*`, invalidation by `PER_VMID_INVALIDATE_REQ`. Basis: Linux uses 1-7 on this card
(compositor VMID 5, measured) and 8-15 for KFD; the kext has only ever used 8 on the card. *Test: S1* (VMIDs 1, 8, 15).

**U3.** MMIO rebind of an **idle** VMID while other VMIDs run is safe (B-V1). Basis: the register writes are the ones `vmContextInit` does, card-proven for VMID 8 while
the boot queue was idle, and pre-HWS KFD did the same per process on older ASICs; gfx12 amdgpu never does it while running (MES or ring packets do). Not while another context
was busy, on this card. *Test: S8 on the card with two clients in flight.* The fallback is B-V2 (ring-emitted rebind, amdgpu's own sequence, measured on this card for the gfx ring).

**U4 (GFXOFF).** Every MMIO in B-V1 needs `GcAccess`/`gcEnsureAwake` like the existing paths; context registers are not in the GFXOFF restore snapshot
(`compute.cpp:1310-1336`). Hence "GFXOFF stays refused while a VMID is bound" until W4 decides.

**Risks.** (1) Lock restructuring (S3) touches every entry point; do it first and alone. (2) A shared queue turns one client's hang into a queue reset; the recovery path
(`recoverComputeQueue`) is exercised by the optional `diagnostic-log.sh hang` step of `docs/real-card-plan.md`; I found no log of it on the card, so treat it as emulator-proven only. (3) PASID/IH stamping order is a timing matter
the emulator can only approximate (E5). (4) `rtFree` without invalidate (S0) means the current code may already expose stale translations on the card; it has not been
probed.

**Questions for the lead.**

1. Is **root-only** (`userclient.cpp:47`) staying for the Metal phase? Many unprivileged processes need an entry point that grants a *client* (VM + submit) and not raw
   kernel access; W13's unbounded clients are pointless behind `clientHasPrivilege`. I did not touch it; it is a separate workstream.
2. OK to spend a real-card boot on S1 (VMID 0 queue + IB in VMIDs 1/8/15, no gfx)? Nothing else in this design can be shown on the emulator. If the lead prefers to prove U1
   under Linux first: a compute-ring replay with `AMDGPU_HW_IP_COMPUTE` in `tools/linux-replay` (today it submits a gfx IB) would show a KCQ running an IB in a per-process VMID on
   this very card — outside the currently allowed set, so it needs an explicit OK.
3. Queue count N for B-Q (proposal 2) and whether C (pinned WindowServer HQD) is wanted up front.
4. Should the current fixed-VMID path stay behind a boot-arg for one more round as a fallback? (My plan: yes, through S9.)
5. S0 (the `rtFree` invalidate) is a bug fix independent of W13; want it split off and merged first?

## 9. Sources read

Kext (this worktree, `premetal/g4`): `src/runtime.cpp` (rtOpen/rtRelease/rtDispatch/rtSubmitIb/rtWaitFence/vm*/power), `src/compute.cpp` (`hqdInitFor`,
`recoverComputeQueue`, `launch`, `gfxOffAllowedNow`), `src/compute.hpp`, `src/gpuvm.*`, `src/userclient.*`, `src/ih.*`/`ihdecode.cpp`, `src/gfxring.cpp`, `src/pm4.*`,
`src/gfxregs.hpp`, `include/rdna4compute.h`, `userspace/rdna4-run.c`; docs `rootcause/verify-vm.md`, `w12k-gfx-submit.md`, `linux-replay.md`, `hw-logs/2026-09-30-linux-gfx-ring/`;
`w36-report.md`, `w40-report.md`; `emu/qemu/rdna4.c` (VM walk `:2113-2420`, banked registers `:745-800`, MEC `:3789-4225`, gfx stream `:5560-5720`).

Linux 7.2.2 (`~/work/tools/linux-amdgpu/src`, sparse checkout of `drivers/gpu/drm/amd/{amdgpu,amdkfd,include}`, read-only):
`amdgpu/amdgpu_ids.c`, `amdgpu_vm.c` (`:733-880`, `:2830-2850`), `amdgpu_ib.c` (`:185-300`), `amdgpu_gmc.c` (`:652-718`), `amdgpu_mes.c` (`:100-230`),
`gfx_v12_0.c` (`:1403-1430`, `:1773-1850`, `:3000-3010`, `:3205-3270`, `:4497-4604`), `gmc_v12_0.c` (`:92-260`, `:380-460`, `:945-970`), `gfxhub_v12_0.c` (`:61-79`),
`amdkfd/kfd_device_queue_manager.c` (`:673-700`), `amdkfd/kfd_mqd_manager_v12.c` (`:124`, `:296-302`), `amdgpu_doorbell.h` (`:186-236`),
`include/asic_reg/gc/gc_12_0_0_offset.h`, `include/asic_reg/oss/osssys_7_0_0_offset.h`.
