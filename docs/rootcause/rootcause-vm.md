# VMID 8 page-walk fault on the real card: root cause (read-only review, 2026-09-29)

Tags: **[FACT]** = read in source, header or log (cited); **[INFER]** = follows from facts; **[GUESS]** = plausible, unproven.
Sources and snapshots:
- amdgpu: `~/src/linux-amd/drivers/gpu/drm/amd` (WSL, torvalds d266640, 2026-09-27). Paths below are relative to it.
- umr: AMD's register and VM debugger, `gitlab.freedesktop.org/tomstdenis/umr` @ 8438c15 (2026-09-23), shallow clone.
- Mesa: `~/src/mesa`.
- Kext: `RDNA4FB-int/src`.
- Logs: `premetal/hw-logs/rdna4fb-diag-20260929-081555.txt` (round 4 boot 8, "L" below), -013433 (round 3 boot 2), -015807 (round 3 boot 8).

## 0. Answer

**#1 root cause (about 80%): our leaf PTEs lack bit 63, `AMDGPU_PTE_IS_PTE`.** On GC 12 a valid leaf entry without bit 63 is not a
PTE. The walker takes it as a directory entry and "translates further". It reads one more entry from the page the PTE points at
(our zeroed EOP/PQ/data page) and finds it invalid. That gives MAPPING_ERROR 1 and PERMISSION bit 0 ("PTE not valid") on the first
access of every client, while all four table levels in VRAM are exactly as built.

The earlier finding that amdgpu leaves bit 63 clear on normal PTB leaves is **wrong**. It missed that `amdgpu_ttm_tt_pte_flags()`
ORs `adev->gart.gart_pte_flags` into every BO mapping, and on gmc v12 that value contains `AMDGPU_PTE_IS_PTE`.

**Companion fix (needed once #1 is fixed, about 70%): map CP-fetched pages EXECUTABLE.** The PQ fetch and the IB fetch request
EXECUTE permission. amdgpu/Mesa map every BO R|W|X. Ours are R|W only.

## 1. Evidence chain for #1

| # | Claim | Tag | Citation |
|---|---|---|---|
| E1 | `AMDGPU_PTE_IS_PTE = 1<<63` and `AMDGPU_PDE_PTE_GFX12 = 1<<63` are the same bit. On gfx9 the analogous "PTE is handled as PDE" bit is TF, 1<<56. | FACT | amdgpu/amdgpu_vm.h:81, :133, :140 |
| E2 | gmc v12 GART flags = `MTYPE_UC \| EXECUTABLE \| IS_PTE`. The patch text says "pte_is_pte is new flag introduced in gmc v12 that needs to be set by default for pte" (Hawking Zhang, 2024-04-25). | FACT | amdgpu/gmc_v12_0.c:794-796; amd-gfx msg105980 |
| E3 | `amdgpu_ttm_tt_pte_flags()` does `flags \|= adev->gart.gart_pte_flags` for **every** BO: VRAM, GTT, doorbell. | FACT | amdgpu/amdgpu_ttm.c:1472-1484 (l.1477) |
| E4 | `amdgpu_vm_bo_update` builds each mapping's flags from it. It then calls `gmc_v12_0_get_vm_pte`, which sets or clears EXECUTABLE, rewrites MTYPE and NOALLOC, and **never clears bit 63**. `amdgpu_vm_update_range`, `amdgpu_vm_pte_update_flags` and the SDMA/CPU backends pass the flags through unchanged. The NORETRY mask only fires on bits 4\|54\|56. | FACT | amdgpu/amdgpu_vm.c:1346-1395, :1195-1240; gmc_v12_0.c:507-549; amdgpu_vm_pt.c:657-710; sdma_v7_0.c:1119-1135 |
| E5 | So every normal amdgpu leaf PTE on GC 12.0.x carries IS_PTE. KFD SVM sets it explicitly for gc >= 12.0.0, and the GART table is memset with it. | FACT/INFER | kfd_svm.c:1374-1375; amdgpu_gart.c:276; amdgpu_gmc.c:1143 |
| E6 | **umr models the hardware this way.** For GFX12, `is_pde = !pte` ("Inverted logic for GFX12"). A valid leaf with bit 63 = 0 becomes `pte_is_pde = 1` and the walker goes one level further (`goto pte_further`), reading at the entry's PDE-style address (bits 47:6). | FACT | umr src/lib/vm/decode_pte_entry.c:132-152; src/lib/vm/access_vram_ai.c:1063-1069, :1071-1151 |
| E7 | Our leaves have no bit 63. The readback shows `PT[eop] 0x0000000009a01065` (VALID\|SNOOPED\|READ\|WRITE). `vmMap` builds `kValid\|kSnooped\|kReadable\|kWritable`. `vmMapHost` builds the same set plus kSystem. Nothing adds bit 63. | FACT | L:313-321; runtime.cpp:520-531, :572-577; gpuvm.cpp:11-18 |
| E8 | The kext's own reasoning encodes the error: "PDE_PTE is reserved for a huge page leaf" (gpuvm.cpp:24-26). The W22 table says "On PTB (leaf) PTEs of the normal BO path it is NOT set". It cites `amdgpu_ttm_tt_pte_flags` but not its body. vm-deep-research D9 says the same. | FACT | premetal/w22-findings.md (a) row 1; premetal/vm-deep-research.md D9 |
| E9 | The fix was already coded as diag variant **d** (`vmPteSet = kPdePte`), but it **never ran on the card**. Round 3 used mask 31, which has no d bit. Round 4 used mask 481, which includes d, but every test after the aliased control was abandoned ("d 0/0"). | FACT | runtime.cpp:1258-1266; 015807 boot-args (diag=31); L:18, L:372-373 |

**How the failure follows [INFER from E6+E7].** Take VA 0x100001000. The walker reads PDB2[0], PDB1[4] and PDB0[0], all valid, with
no bit 63, so they are correctly treated as directories. It then reads PT[1] = 0x9a01065: valid, bit 63 = 0, so "translate further".
The next entry address is the PDE-form field, `0x9a01065 & 0xFFFFFFFFFFC0` = 0x9a01040, index 0 (PDE0 BFS 0, PTE FRAG 0). That
is offset 0x40 of the EOP page, which the self-test zeroes (runtime.cpp:768-769, :946-947). The entry is invalid, so the walk fails.
The same holds for the PQ page (13 ring dwords written, so +0x40 is 0) and for E4's IB/data page (+0x40 is 0).

## 2. It explains every card observation

| Observation (cited) | Explained by #1? |
|---|---|
| First access of every client faults: CPC EOP read (0x00800b3b, VA 0x100001000), CPF PQ fetch (IV VA 0x100000000), CPF IB fetch in E4 (0x008009ba, VA 0x100006000). L:330-340, L:351-353 | Yes. Every leaf lacks bit 63, so it does not depend on client, queue, pipe or VMID0/VMID8 queue. |
| MAPPING_ERROR 1, PERMISSION bit 0 ("the PTE was not valid", docs.kernel.org amdgpu debugging) | Yes. The last entry the walker read (in the data page) is 0. |
| SDMA readback of all 4 levels is exact (L:313-321); windows equal on both hubs (L:305-312); CONTEXT8/L2 regs amdgpu-equal (L:302-304) | Yes. The tables are "correct" bytes with the wrong leaf format. None of the checked items covers bit 63. |
| Deterministic across rounds 1-4, independent of IH/gfx/flip/F (round-2 table in vm-deep-research.md s.1) | Yes. It is a static encoding error. |
| QEMU emulator passes | Yes. It accepts any valid level-3 entry and ignores bit 63 (emu/qemu/rdna4.c:2155-2163). |
| VMID0 CPG fault 0x00000d3d (WALKER 6) on context 0's flat table (hw-logs 081934:335) | Neutral. That entry is all-zero (compute.cpp:2236-2240), so the fault proves nothing about reading or depth (see s.3). |

## 3. Premises in the brief that the evidence contradicts

- **"IS_PTE bit 63 is NOT set on normal PTB leaves (verified equal)": false** (E2-E5). This is the error that hid the cause.
- **"the multi-level walk is what fails; the flat walk can read VRAM": not shown.**
  - The flat-case entry is 0 (compute.cpp:2236-2247, ctx0 START=END=0), so a correct read and a failed read look the same. It was a
    stray CPG access, not a probe (premetal/w31-review.md s.1).
  - Three variables differ at once: depth, VMID (0 vs 8), and table location (pool below 256 MiB vs devHeap past the BAR).
  - Real amdgpu on gfx1200 reports **WALKER_ERROR 5 for a VMID0 CPC read of a flat GART page** (status 0x00000B3A, VA 0x2ba000,
    amd-gfx thread 2026-07, ratatoskr.run/amd-gfx/2026/07/17210951). So 5 does not mean "multi-level failure".
- **"umr decodes WALKER_ERROR": no.** A grep of umr `src/` finds no WALKER_ERROR decode; it appears only as a register field in
  `database/ip/gc_12_0_0.reg`. The kernel prints the raw 3 bits (gfxhub_v12_0.c:97-99).
  - Public data points: 5 on gfx1201 KFD VMID 8 unmapped user VA (status 0x0084115B, ROCm/rocm-libraries#459), 5 on gfx1200 VMID0
    GART (above), 5 on Navi10 (dxvk#5439), 6 on gfx1100 (ROCm#2642), 1 on gfx1103 VMID0 VA 0 (ROCm#6269).
  - [GUESS] 5 = "the walk ended on an entry that is not a valid PTE". The value is not diagnostic beyond that.
- **E4 could never have passed as built.** Its IB page is mapped without EXECUTABLE (runtime.cpp:1120, :914). The IB fetch requests
  EXE: the IV has src_data[1] = 0x50 = READ\|EXE (amdgpu_gmc.h:93-95, L:351) and PERMISSION_FAULTS is 0xb. Even with a working walk
  it would have logged a permission fault. The same applies to the PQ (IV 0x50 at VA 0x100000000, L:332; PQ mapped R|W,
  runtime.cpp:772).

## 4. Candidates ranked (the brief's list plus new ones)

| Rank | Candidate | Verdict | Key evidence |
|---|---|---|---|
| 1 | Leaf PTE missing IS_PTE (bit 63) | **~80%, top** [INFER on FACTs] | s.1 E1-E9, s.2 |
| 1b | PQ/IB pages missing EXECUTABLE | Next fault after #1, same patch [INFER] | IV 0x50 (L:332, :351); PERMISSION 0xb; Mesa maps every BO R\|W\|X: ac_linux_drm.c:228-235, radv_amdgpu_bo.c:40-41; `get_vm_pte` keeps EXE only when asked (gmc_v12_0.c:513-516) |
| 2 | VMIDs 8-15 need MES/firmware per-VMID state | ~7% [GUESS] | amdgpu's non-MES path uses VMIDs 1-7 only (gmc_v12_0.c:947-957, amdgpu_ids.c:652-661). KFD 8-15 only via MES (mes_v12_0.c:952-953, amdgpu_mes.c:126-130). No gfx12 MMIO hqd_load (amdgpu_amdkfd_gfx_v12.c:518-533). But E4 used a VMID0 queue and failed the same way, and #1 already explains it. |
| 3 | Hidden hub state never read: `GCVM_CONTEXTS_DISABLE` (0x1634), `GCUTCL2_TRANSLATION_BYPASS_BY_VMID` (seg1 0x5e41: TRANS_BYPASS_VMIDS[15:0], GPA_MODE_VMIDS[31:16]) | ~5% [GUESS]; free reads | gc_12_0_0_offset.h:2924, :3408; sh_mask.h:10021-10036, :11388-11389. amdgpu never writes them for GC (grep: only mmhub/gmc_v11 SR-IOV, gmc_v11_0.c:888-895). |
| 4 | SDMA-written tables not visible to the walker (GL2/MALL) | ~3% [INFER, weak] | Our SDMA write policy equals amdgpu's (WR BYPASS, RD NOA): sdma_v7_0.c:583-589, sdma_common.h:31,39 vs compute.cpp:2466-2467, gfxregs.hpp:185-186. COPY_LINEAR packet is identical: sdma_v7_0.c:1058-1075 vs sdma.cpp:44-54. System-aperture MTYPE UC: gfxhub_v12_0.c:192-210 (l.207), TLB 0x1859 (L:303). |
| 5 | Invalidation path (MMIO vs ring, semaphore) | ~2% | amdgpu itself flushes the GC hub by MMIO on engine 17 with no semaphore on bare metal (gmc_v12_0.c:190-195, :213-262). The kext does the same (runtime.cpp:675-692). The request bits equal `gfxhub_v12_0_get_invalidate_req`. |
| 6 | PDE address semantics (TAP_*_PHYSICAL, MC form, ctx0 coverage) | ~2%; vmfix2 hypothesis A is refuted | amdgpu puts the GART at MC 0 (gmc_v12_0.c:700, amdgpu_gmc.c:325-327), so ctx0 covers MC 0..512 MiB. It writes physical-form PDEs with TAP_PHYSICAL=0 (gfxhub_v12_0.c:254-255, gmc_v12_0.c:488-490). If PDE fetches were ctx0-translated, amdgpu's low page tables would hit GART pages. |
| 7 | GL2C golden / GPA mode (CPC/CPG_PSP_DEBUG.GPA_OVERRIDE) | ~1% | GPA override only in direct or backdoor load (gfx_v12_0.c:1350-1351, :3791-3792; definition :3658). VMID0 CP traffic works. |

## 5. The fix (exact code; amdgpu parity)

1. `gpuvm.hpp`: add `constexpr uint64_t kIsPte = 1ull << 63; // AMDGPU_PTE_IS_PTE, amdgpu_vm.h:133; gmc_v12_0.c:794-796 via amdgpu_ttm.c:1477`.
   Fix the comment at gpuvm.cpp:24-26: bit 63 is *required* on every PTB leaf, and means "huge page" only in PDB levels.
2. `runtime.cpp` `vmMap` (l.520). Replace with:
   `uint64_t flags = GpuVm::kValid | GpuVm::kReadable | GpuVm::kWritable | GpuVm::kIsPte;`
   - Keep `if (executable) flags |= GpuVm::kExecutable;`.
   - Drop `kSnooped` for VRAM. amdgpu sets SNOOPED on VRAM only for cached BOs (amdgpu_ttm.c:1456-1458). This is optional but
     byte-parity.
   - Result for the EOP page: `0x8000000009a01061`. For the PQ with EXE: `0x8000000009a02071`.
3. `runtime.cpp` `vmMapHost` (l.572): add `| GpuVm::kIsPte` (amdgpu GTT PTEs get it from the same l.1477).
4. EXECUTABLE for everything the CP fetches as commands (IV EXE bit):
   - PQ: `vmMap(..., kPqSize, true)` at l.772, l.908, l.1536.
   - E4 IB page: l.914 and l.1120 → `true`.
   - User buffers that can hold IBs (l.1924): `true`, simplest as Mesa does (ac_linux_drm.c:235).
   - Keep EOP/rptr/wptr/fence R|W (the EOP IV is 0x40, READ only, L:330).
5. Keep a negative control. Add boot-arg `rdna4-vm-ispte=0` that clears kIsPte, so the old fault (0x00800b3b) can be reproduced.
   Variant d (runtime.cpp:1263) becomes the default.
6. Emulator (so it can fail like the card): emu/qemu/rdna4.c:2155. At `level == 3`, a valid entry without `RDNA4_VM_PDE_PTE`
   (bit 63) must `goto fault` (umr access_vram_ai.c:1067-1069 walks further into the data page). Also require EXECUTABLE for CPF
   PQ/IB fetches (IV EXE). Update `GpuVm::walk` (gpuvm.cpp:47-68) and its host test (tools/atomdump.cpp:2102-2105) the same way.
   The pre-fix kext must then fail in QEMU; that is the negative control.
7. Nothing else changes: CONTEXT8, L2, windows and the invalidate path stay as they are (all amdgpu-equal, s.4).

## 6. One cheap real-card test

**Primary (after the patch in s.5): one boot, `rdna4-vm=1`, no diag.** Expected:
- no `vm: boot queue test failed` line;
- the fence reaches 0x564d0001 and the data word is 0x600df00d;
- fault status 0;
- `RDNA4FB,Results "vm"="PASS ..."`;
- the table dump shows `PT[eop] ... 0x8000000009a01061 ... ok`.

How to read other outcomes:
- A fault with **MAPPING_ERROR 0 and WALKER_ERROR 0** means #1 is confirmed and a permission bit is left. Read PERMISSION_FAULTS:
  bit 3 means EXE is missing, bit 1 read, bit 2 write.
- Unchanged 0x00800b3b means #1 is wrong; go to s.7.
- Optional second boot with `rdna4-vm-ispte=0` must reproduce 0x00800b3b. That is the A/B proof.

**Zero-code alternative on the current int build: `rdna4-vm=1 rdna4-vm-diag=32`.** This runs baseline, then control on MEC pipe 1
queue 0, then variant d on pipe 1 queue 1, each on a fresh slot (runtime.cpp:1195-1203, bitOf[kD]=32 at :1178). Expected for d:
- either PASS;
- or a changed status: MAPPING_ERROR 0, WALKER_ERROR 0, PERMISSION bit 3 set, CID 4 or 5, from the still non-executable PQ.

Either result confirms #1. Risk: a dequeue timeout on the control slot abandons d, and the log says so (runtime.cpp:1315-1318).

## 7. If #1 does not change the status (backup, cheapest first)

- **Free reads** in `dumpSetup`: `GCVM_CONTEXTS_DISABLE` 0x1634; `GCUTCL2_TRANSLATION_BYPASS_BY_VMID` seg1 0x5e41;
  `GCUTCL2_HARVEST_BYPASS_GROUPS` 0x15b7; `GCUTCL2_GROUP_RET_FAULT_STATUS` 0x15b9; `CPG/CPC_PSP_DEBUG` 0x5c10/0x5c11;
  `GCVM_L2_PROTECTION_FAULT_STATUS_HI32`. Offsets: gc_12_0_0_offset.h:2758-2761, :2924, :3408, :7099-7102.
  A set bit 8 in DISABLE, TRANS_BYPASS or GPA_MODE names candidate 3.
- **UTCL2 translation-assist probe** [GUESS semantics; unused by amdgpu, grep finds no hit]. This translates a VA by MMIO with no
  queue, so nothing wedges and many table variants fit in one boot.
  - Registers: `GCUTC_GPUVA_VMID_TRANSLATION_ASSIST_REQUEST_ADDR_LO32/HI32` 0x15ee/0x15ef (VA>>12), `REQUEST_ATTR` 0x15f0
    (VMID[3:0], RD/WR/EX_PERM [12:14], REQ [31]); `RESPONSE_ADDR` 0x15f1/0x15f2; `RESPONSE_ATTR` 0x15f3 (PERMS, FRAGMENT_SIZE,
    NO_PTE [13], MTYPE, NACK [18:17], ACK [31]); `..._CNTL.ENABLE` seg1 0x5e44 (offset.h:2844-2855, :3412; sh_mask.h:9252-9305,
    :11393-11395).
  - Use bounded polling only.
- **VMID 1 instead of 8** for the baseline and E4 (amdgpu's non-MES path, s.4 rank 2). This is a one-constant change
  (runtime.cpp:736).
- Always make the test under study the **first** VM access of a boot. A faulted MEC slot is not serviced again (w22-findings.md (b),
  (c)).

## 8. What was checked and found equal (no action)

- CONTEXT8 CNTL/BASE/START/END: gfxhub_v12_0.c:298-345 vs runtime.cpp:656-673 and L:302.
- L2 CNTL through CNTL5 defaults: gfxhub_v12_0.c:32-34, :212-261 vs gfxregs.hpp:280-286 and L:303. The only difference is bit 11,
  default-page-to-sys, which the kext clears on purpose.
- PDE format: amdgpu_ttm.c:1439-1461 (`pde_flags`: VALID only for WC VRAM), gmc_v12_0.c:485-505 vs gpuvm.cpp:22-28.
- PT base: amdgpu_gmc_pd_addr uses pde_flags, so there is no bit 63 in the base register.
- SDMA packet and cache policy: s.4 rank 4.
- MMIO invalidate: s.4 rank 5.
- MM/GC windows: L:305-312.
- The emulator and host walker are **not** equal to hardware on bit 63: s.5 item 6.
