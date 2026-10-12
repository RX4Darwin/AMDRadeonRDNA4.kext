# W22 vmfix3: round-3 results vs amdgpu (branch premetal/vmfix3, off premetal/int 39786af)

Logs: premetal/hw-logs/rdna4fb-diag-20260929-013433.txt (boot 2), -015807.txt (boot 8).
amdgpu references: ~/src/linux-amd/drivers/gpu/drm/amd (paths below are relative to it).

## Round-3 facts (real card)

- Baseline VMID 8 queue (pipe 0 queue 1): fault `0x00800b3b` = MORE 1, WALKER_ERROR 5, PERMISSION 3, MAPPING 1,
  CID 5 (CPC), VMID 8, read, VA 0x100001000 (the EOP fetch).
- E4 (IB fetched in VMID 8 from the VMID0 kernel ring): fault `0x008009ba` = WALKER_ERROR 5, PERMISSION **0xb**,
  MAPPING 1, CID 4 (CPF), VA 0x100006000. Same walker error, from a different client: nothing about the queue.
  PERMISSION_FAULTS gains bit 3 (0x8) for an IB fetch; the walker reports "no valid translation" the same way.
- Tables in VRAM are exactly what we built (SDMA readback `ok`), CONTEXT8 CNTL 0x00fffc07, BASE 0x10000001, START 0,
  END 0xf:ffffffff; GC and MM windows identical (D1 refuted).

## (a) gfx12 PTE/PDE bits: nothing the walker must have is missing (per the source)

| Bit | amdgpu (gfx12.0) | Ours | Verdict |
|---|---|---|---|
| bit 63, `AMDGPU_PTE_IS_PTE` = `AMDGPU_PDE_PTE_GFX12` (same bit, amdgpu_vm.h:133,140) | Set on directory entries that act as leaf PTEs: amdgpu_vm_pt.c:411, 685-686 (`level != AMDGPU_VM_PTB` -> `flags \|= AMDGPU_PDE_PTE_FLAG`). On PTB (leaf) PTEs of the normal BO path it is NOT set: amdgpu_vm.c:1346-1387 builds flags from `amdgpu_ttm_tt_pte_flags` + `amdgpu_gmc_get_vm_pte` (gmc_v12_0.c:507-549), which adds IS_PTE only for PRT (:540). `init_pte_flags = IS_PTE` (gmc_v12_0.c:648) is inside `case IP_VERSION(12,1,0)` only. GART PTEs carry it (gmc_v12_0.c:794-796). KFD's SVM path adds it to every valid leaf PTE for gc >= 12.0.0 (amdkfd/kfd_svm.c:1375). | clear on leaf, clear on directories | The normal Linux path runs gfx12.0 without it on PTB leaves, so "leaf PTEs without IS_PTE are walked as PDEs" is not supported by the source. KFD's SVM path does set it. Cheap to test: variant d. |
| `AMDGPU_PDE_BFS_GFX12` (bits 62:58) | gmc_v12_0.c:498-499, only `if (adev->gmc.translate_further)`; translate_further is set only in gmc_v9_0.c (:1902-1943), never for gfx12 | clear | equal |
| MTYPE bits 55:54 (`AMDGPU_PTE_MTYPE_GFX12`, amdgpu_vm.h:125-129) | `MTYPE_NC` = 0 for default/NC mappings (gmc_v12_0.c:520-524) | 0 | equal |
| SNOOPED (bit 2) on a VRAM PTE | only for cached BOs: amdgpu_ttm.c:1457-1458 | set | legal, but variant e removes it |
| SYSTEM (bit 1) | only for GTT/doorbell/MMIO-remap BOs: amdgpu_ttm.c:1447-1452 | clear (VRAM) | equal |
| FRAG (bits 11:7) | added per contiguous 64K run by amdgpu_vm_pte_fragment | 0 now (W17 fix) | equal |
| PDE = address | VALID: amdgpu_gmc.c:112-130 (`amdgpu_gmc_get_pde_for_bo`), gmc_v12_0.c:485-505 | address + VALID | equal |
| depth / block size | num_level 3 (root PDB2), block_size 9: amdgpu_vm.c:2426-2456 with `amdgpu_vm_adjust_size(adev, 256*1024, 9, 3, 48)` (gmc_v12_0.c:841) | 3 / 0 | equal |
| START/END | `0` / `max_pfn - 1` = 0xf:ffffffff (gfxhub_v12_0.c:333-341) | identical on the card | equal |

Result: the PTE/PDE encodings and the context registers match amdgpu for gfx12.0. There is no bit I can cite as
"required by the walker and missing", so I did NOT make the emulator reject leaf PTEs without IS_PTE: that would
model something the normal Linux path contradicts, and the round-3 kext could not then be justified as
"faulting because it violates amdgpu". IS_PTE is instead a diagnostic variant (d) for the card.

## (b) RETRY_PERMISSION_OR_INVALID_PAGE_FAULT and why variants a/b/c saw fault status 0

- 0x00fffc07 does NOT set the retry bit. CONTEXTn_CNTL: RETRY_PERMISSION_OR_INVALID_PAGE_FAULT is bit 8 (0x100),
  RETRY_OTHER_FAULT bit 9 (gc_12_0_0_sh_mask.h:9423-9424, 9442-9443). 0xfffc07 = bits 0, 1, 2 (enable, depth 3) and
  bits 10-23 (the fault-interrupt/default bits); bits 8 and 9 are 0. amdgpu sets the field to `!adev->gmc.noretry`
  (gfxhub_v12_0.c:325-326); with noretry = 1 that is 0, i.e. exactly ours.
- Variants a/b/c saw status 0 because their queue was never serviced, not because the walk worked. Boot-8 log, after the
  baseline fault, for every variant on the same slot (pipe 0 queue 1): `doorbell 0xc0000068` (bit 31 = HIT: the doorbell
  arrived and the MEC never consumed it; the baseline attempt showed `0x40000068`, consumed), `CPC_BUSY 0`,
  `EOP_RPTR 0x40000000` unchanged, `MEC pc 0x3fb5` (it was 0x7a0 in the baseline), no fault latched. The MEC stopped
  scheduling that slot after the fault, and a MMIO dequeue + reinit does not bring it back. So a/b/c and E2 were not
  valid tests; the window/ctx0/TAP hypotheses remain untested, not refuted.
- Fix in this branch: every test after the baseline gets a fresh (pipe, queue) slot on MEC pipes 1-3, and each result line
  now prints the HQD doorbell control so "queue not serviced" is visible; a `control` test (nothing changed) on a fresh
  slot runs first: it must fault like the baseline, or the fresh-slot approach does not work.

## (c) Why E4 recovery failed

E4 issued its IB from the kernel ring (pipe 0 queue 0). The IB fetch faulted (CPF, 0x008009ba); the ring's HQD showed
`vmid 0x800` (IB_VMID 8), `HQ_STATUS0 0x8040`, `EOP_RPTR 0x40000020`, `MEC pc 0x611`. `recoverComputeQueue` (RESET_WAVES
dequeue, SQ kill, HQD reprogram) got the HQD inactive but the re-initialised queue then never ran its WRITE_DATA
(`HQ_STATUS0 0x40000040`, `MEC pc 0x5044`, `CPC 0xa0000041`): the same "slot not serviced after a VM fault" state as (b),
now on the kernel ring. amdgpu resets a faulted MEC queue through MES (`gfx_v12_0_reset_kcq`), which we do not have, so
a queue that took a VM fault cannot be revived by MMIO. Consequence: the probe must never run on the working ring.
E4 now runs first on a scratch, privileged VMID0 queue (pipe 3 queue 3, slot 7's pool area; amdgpu's kernel compute rings
are PRIV_STATE + KMD_QUEUE, gfx_v12_0.c:3250-3251, and their IB packets carry the job's VMID in bits 27:24,
`gfx_v12_0_ring_emit_ib_compute`), so a fault can only wedge a pipe nobody uses; no recovery path and no `rdna4-hang=1`
are needed any more.

## What changed

Kext (src/runtime.cpp, src/compute.hpp):
- fresh queue slot per test on pipes 1-3 (doorbell dwords 0x1c...0x30), E4 as a scratch VMID0 queue (slot 11: pipe 3 queue 3,
  doorbell 0x32), per-test doorbell-control line;
- new diag bits (mask 511 = all): d IS_PTE on leaf PTEs, e no SNOOPED, g EXECUTABLE, T page tables written by the CPU
  through the BAR into the pool (kVmTableCpu = 28 MiB) instead of by SDMA; existing E4/a/b/c/E2 kept; `control` runs with any bit;
- I (L2_CNTL DEFAULT_PAGE_OUT_TO_SYSTEM) is deliberately NOT a variant: with it set, a faulting access is redirected to a
  system-memory address taken from the default-page registers (our VRAM scratch offset would then be a host physical address).

Emulator (emu/qemu/rdna4.c): a privileged queue (CP_HQD_PQ_CONTROL.PRIV_STATE) may fetch an IB in the VMID the IB packet names.
The old rule (IB VMID must equal the queue's) is right only for unprivileged client queues; kernel rings always name the job's
VMID (gfx_v12_0.c:4546-4562). This is what lets E4 pass in the emulator; no walker rule was tightened.

## What is still unknown

The walker error 5 is not decoded anywhere public. Everything we can check (tables in VRAM, registers, PTE/PDE bits, windows)
matches amdgpu. Remaining candidates for the next card run: the MEC/CP side needs something MES normally sets for VMIDs 8-15
(unknowable from the tree), IS_PTE (d), SDMA-vs-CPU table coherence (T), the never-tested fresh-slot behaviour (control).
