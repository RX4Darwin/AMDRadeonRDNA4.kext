# Adversarial check of the VM root cause "leaf PTE missing IS_PTE (bit 63)" (read-only, 2026-09-29)

Tags: **[FACT]** = read in source, a commit, or a log (cited). **[INFER]** = follows from facts. **[GUESS]** = plausible, unproven.

Sources:
- amdgpu: `~/src/linux-amd/drivers/gpu/drm/amd` (WSL, torvalds d266640). It is a *shallow* clone, so `git log -S` is not possible there; commit history comes from the amd-gfx archives (URLs below).
- umr @ 8438c15 (shallow clone in the session scratchpad).
- tinygrad @ 7bfaa18 (sparse clone, `tinygrad/runtime/support/am`).
- Kext: `RDNA4FB-int` @ 5651214 (contains merge e7817ce).
- Card logs: `premetal/hw-logs/rdna4fb-diag-20260929-081555.txt` ("L" below) and -013433.

## Verdict: **LIKELY**, about 85%

| Sub-claim | Confidence | Basis |
|---|---|---|
| A. GC 12 hardware needs bit 63 on a *valid* leaf; it is not a software marker | ~95% | s.1, s.3 |
| B. Its absence is what caused 0x00800b3b / 0x008009ba on the card | ~85% | s.4 |
| C. The walker specifically "goes one more level" (as opposed to failing at the PTB) | ~70% | umr model only, s.2. It does not change the fix. |
| D. The fixed build walks VMID 8 successfully on the first cold boot | ~80% | s.6 |
| E. It also passes the full self-test | ~75% | s.6 |

Why not CONFIRMED:
- The fix has never run on the card. Variant d never ran either (rootcause-vm.md E9). Every later-variant result in the logs is `fault status 0x00000000` on a wedged slot (L:362-370), so none of them is an A/B.
- No AMD document or commit states the walker mechanism.
- The fault fields are the generic gfx12 "no valid translation" signature (s.4).

I could not refute the claim. Nothing found writes a working valid gfx12 leaf without bit 63.

## 1. Linux history of AMDGPU_PTE_IS_PTE / AMDGPU_PDE_PTE_GFX12 / gart_pte_flags

| Date, author | Patch | What it says | Tag |
|---|---|---|---|
| 2024-04-25, Hawking Zhang | "[PATCH 1/5] drm/amdgpu: Add gfx v12 pte/pde format change" ([msg105984](https://www.mail-archive.com/amd-gfx@lists.freedesktop.org/msg105984.html)) | Defines IS_PTE (bit 63), PDE_PTE_GFX12 (bit 63), PRT_GFX12 (bit 56), MTYPE_GFX12, BFS_GFX12. Comment: "PDE is handled as PTE for gfx v12". | FACT |
| 2024-04-25, Hawking Zhang | "[PATCH 3/5] drm/amdgpu: Set pte_is_pte flag in gmc v12 gart" ([msg105980](https://www.mail-archive.com/amd-gfx@lists.freedesktop.org/msg105980.html)) | "pte_is_pte is new flag introduced in gmc v12 that needs to be set by default for pte." Adds it to `gart_pte_flags`. | FACT |
| 2024-04-29, Sreekant Somasekharan | "[PATCH 13/14] drm/amd/amdkfd: Add GFX12 PTE flag to SVM get PTE function" ([msg106177](https://www.mail-archive.com/amd-gfx@lists.freedesktop.org/msg106177.html)) | Sets IS_PTE on SVM leaf PTEs for GC >= 12.0.0: "This resolves the issues related to SVM enablement in GFX12." | FACT |
| 2024-06-03, Frank Min | "drm/amdgpu: Set PTE_IS_PTE bit for gfx12" ([msg107959](https://www.mail-archive.com/amd-gfx@lists.freedesktop.org/msg107959.html)) | "Set PTE_IS_PTE bit while PRT is enabled on gfx12". Applies to PRT entries, which are VALID=0. | FACT |
| 2025-11-19, Mukul Joshi | "[PATCH 3/7] drm/amdgpu: Add per-ASIC PTE init flag" ([msg132659](http://www.mail-archive.com/amd-gfx@lists.freedesktop.org/msg132659.html)) | "On GFX12.1, default PTE setup needs an additional bit". `init_pte_flags = IS_PTE` for 12.1 only. | FACT |
| 2026-04-14, Siwei He | "drm/amdgpu: OR init_pte_flags into invalid leaf PTE updates" ([ratatoskr](https://ratatoskr.run/amd-gfx/2026/04/16015053/t)) | "...On GFX12 that includes AMDGPU_PTE_IS_PTE; without it, some cleared PTEs can fault as no-retry and bypass the SVM/XNACK handler" | FACT |
| 2026-08-28, Timur Kristóf (applied by Alex Deucher; reviewed by Christian König) | 08/11 "Use AMDGPU_PTE_IS_PTE flag for init_pte_flags on GFX12.0" ([msg150051](http://www.mail-archive.com/amd-gfx@lists.freedesktop.org/msg150051.html)); 09/11 "Use init PTE flags in amdgpu_vm_handle_fault()" ([msg150054](http://www.mail-archive.com/amd-gfx@lists.freedesktop.org/msg150054.html)) | "PTE_IS_PTE seems necessary for handling retry faults on GFX12." In the v1 thread ([ratatoskr 17060196](https://ratatoskr.run/amd-gfx/2026/05/17060196/t)): "I can confirm that it solves the problem for me". The same series says "The doorbell is not working on Navi 48", so it was tested on gfx1201. | FACT (quotes); INFER (tested on Navi 48) |

Reading of these messages:
- **None of them explains the mechanism.** No message says "without it the walker translates further". The wording is "needs to be set by default for pte" [FACT].
- **It is a hardware field, not a software marker.**
  - gmc_v12_0.c:451-468 lists the PTE format as "63 P". The SW bits are separate: "53:52 SW" [FACT].
  - amdgpu never reads bit 63 on a leaf. A grep finds only setters: gmc_v12_0.c:540, :648, :796; kfd_svm.c:1375. The only reads are PDB-level `AMDGPU_PDE_PTE_GFX12` tests in get_vm_pde (gmc_v12_0.c:488-503) [FACT].
  - A bit that software never reads exists for the hardware [INFER].
- **Two empirical data points with valid leaves.** Both say gfx12 misbehaves when a *valid* leaf lacks bit 63 [INFER from FACT]:
  - SVM on GFX12 was broken until the bit was added (2024-04). At that date the only GFX12 parts were GC 12.0.0 and 12.0.1.
  - Timur's handle_fault fix: the dummy-page redirect is VALID|SNOOPED|SYSTEM|EXE|R|W. Without bit 63, retry faults on Navi 48 were not resolved.
- **Siwei (2026) and Frank Min (2024) show the hardware reads bit 63 on invalid PTB entries too.** It changes how a fault is classified [INFER].

## 2. umr (AMD's VM debugger, Tom St Denis)

**Decoding** (src/lib/vm/decode_pte_entry.c) [FACT]:
- GFX9-11 (:90, :111, :130): an entry is a PDE when `is_pde = further`, i.e. bit 56 = TF is **set**.
- GFX12 (:147): `pte = bit 63 /* PTE flag: 1=PTE, 0=PDE */`.
- GFX12 (:152): `is_pde = !pte /* Inverted logic for GFX12 */`.
- PDE-form address (:164-171): bits 47:6 (`& 0xFFFFFFFFFFC0`).

**Walking** (src/lib/vm/access_vram_ai.c) [FACT]:
- :1063-1069: `if (maj >= 12 && !pte && is_valid) pte_is_pde = 1`.
- :1071-1151: the entry is re-decoded as a PDE (:1087). The index is `BFS(PDE0) - frag(entry)` bits (:1140-1142). Then `goto pte_further` (:1151, label :1000), which reads one more entry.

**Applied to our old leaf 0x0000000009a01065** [INFER]:
- VALID = 1 and bit 63 = 0, so umr treats it as a PDE.
- PDE address = 0x9a01040. frag = 0 and BFS = 0, so the index is 0.
- The next entry is read at EOP page + 0x40, which is zeroed (rootcause-vm.md s.1). It is invalid, so umr reports "invalid page".

The same holds for the PQ entry (+0x40 = ring dword 16, not written) and the E4 IB page (+0x40 = 0).

Caveat: umr is a debugger model written by AMD, not a hardware specification [INFER]. It matches the "P" naming in gmc_v12_0.c:451-452 and :470-471.

## 3. Searching for a working valid gfx12 leaf without bit 63 (none found)

| Path (GC 12.0.x, this tree) | Valid leaf carries bit 63? | Tag |
|---|---|---|
| GEM / KFD non-SVM / CSA / seq64 / userptr / doorbell | Yes (see chain below). Nothing clears it: the only `&= ~` on this path are R/W at amdgpu_vm.c:1382-1384. `set_pte_pde` writes `addr\|flags` (amdgpu_gmc.c:163-176). The SDMA PTEPDE packet carries the full 64-bit flags (sdma_v7_0.c:1128-1129). | FACT |
| GART, VMID0 flat depth 0 | Bound entries: yes. They get flags from `amdgpu_ttm_tt_pte_flags` (amdgpu_ttm.c:268-282; gart.c:354-376). Unbound entries = dummy \| 0 (gart.c:314-315), invalid. The initial fill `memset_io(ptr, gart_pte_flags, size)` (gart.c:276) is a byte memset, so every byte is 0x10 and the entry is 0x1010101010101010: no VALID, no bit 63. | FACT / INFER |
| Huge pages (PDE0/PDB1 used as PTE) | Yes. amdgpu_vm_pt.c:684-686 ORs `AMDGPU_PDE_PTE_FLAG` (= bit 63). The VMID0 PDB0 path does the same (amdgpu_gmc.c:1143-1159). **Every terminal entry at every level has bit 63.** | FACT |
| KFD SVM | Yes. It is set explicitly at kfd_svm.c:1374-1375 because SVM builds its flags from scratch (s.1). | FACT |
| PRT | Yes, together with VALID=0 (gmc_v12_0.c:535-541). | FACT |
| Invalid PTB entries (pt_clear / update_flags) | `EXE \| init_pte_flags` (amdgpu_vm_pt.c:405-417, :688-697). `init_pte_flags` is IS_PTE only for 12.1 in this tree (gmc_v12_0.c:644-649); Timur's 2026-08 series adds it for 12.0. These entries are invalid, so they say nothing about valid leaves. | FACT |
| `amdgpu_vm_handle_fault` | **The only valid leaf without bit 63 in this tree** (amdgpu_vm.c:3056-3069). On GC 12.0 it is off by default: `noretry = 1` for gc >= 10.1 (amdgpu_gmc.c:1004-1019). Timur's 09/11 fix adds the bit because retry handling failed without it. This supports the claim; it is not a counterexample. | FACT |
| tinygrad AM (userspace driver, runs gfx12) | Sets IS_PTE on PTB leaves and PDE_PTE_GFX12 on huge leaves (ip.py:178-192). Present since "am: rdna 4 support" (54e1e59b44, 2025-03-29). This is parity with amdgpu, not an independent A/B. | FACT |

The chain for the first row: `amdgpu_vm_bo_update` calls `amdgpu_ttm_tt_pte_flags` (amdgpu_vm.c:1348), which ORs `gart_pte_flags` (amdgpu_ttm.c:1477), which contains IS_PTE (gmc_v12_0.c:794-796).

## 4. Does the fault signature fit?

Field layout: gc_12_0_0_sh_mask.h:9002-9013 [FACT].

| Status | MORE | WALKER | PERM | MAPPING | CID | RW | VMID | Access (IV src_data[1], amdgpu_gmc.h:92-95) |
|---|---|---|---|---|---|---|---|---|
| 0x00800b3b (L:340) | 1 | 5 | 0x3 | 1 | 5 CPC | read | 8 | EOP 0x40 = READ (L:330) |
| 0x008009ba (L:353) | 0 | 5 | 0xb | 1 | 4 CPF | read | 8 | IB 0x50 = READ\|EXE (L:351) |

- **The signature is generic, not specific to bit 63** [FACT / INFER]. Real amdgpu on gfx12, whose mapped leaves all have bit 63, shows the same fields on ordinary faults:
  - Navi 44, VMID0 CPC: 0x00000B3A = WALKER 5, PERM 3, MAPPING 1 ([ratatoskr 17210951](https://ratatoskr.run/amd-gfx/2026/07/17210951/t)).
  - gfx1201, VMID 8 TCP write to an unmapped VA: 0x0084115B = WALKER 5, PERM 5, MAPPING 1 ([ROCm/rocm-libraries#459](https://github.com/ROCm/rocm-libraries/issues/459)).
  - So "WALKER 5 + MAPPING 1 + PERM = not-valid + requested access" means "the walk reached no valid PTE". Our fields fit, but so would any other cause that ends the walk on an invalid entry.
- **The discriminating part: PERMISSION bits 0 and 1 on the EOP read** [INFER]. These say the terminal entry had VALID=0 and READ=0. Our leaf had V=1, R=1, W=1 (L:317). So the walker did not use our leaf as the terminal PTE. That leaves two explanations:
  - (i) it walked past the leaf (the claim);
  - (ii) it read different memory than the SDMA readback shows (table address or visibility).
  - Under (i), the first access of every client fails, which matches L:330-353.
  - (ii) is made less likely by three things: the readback of all 4 levels is exact (L:313-321); the PDE format and base equal amdgpu's (s.5); and the SDMA write policy equals amdgpu's (rootcause-vm.md s.4, rank 4).
- **An unexplained point (weak minus).** The kext's VMID0 flat all-zero entry gave **WALKER 6** (0x00000d3d, hw-logs 081934:335; w31-review.md:15). Under the claim, our VMID-8 walk also ends on an all-zero entry, yet it gives WALKER 5.
  - WALKER codes are undocumented: docs.kernel.org/gpu/amdgpu/debugging.html gives no values, and umr has no decoder (grep).
  - The two cases also differ in depth, client, VA and L2_CNTL bit 11. The difference cannot be used either way [GUESS].
- **Why no fault moves to a different status in the logs.** Only the first fault of a boot is a clean observation (w30-findings.md:50). The later "variants" show status 0 with fence 0, i.e. a wedged slot (L:362-370). So no ladder data contradicts the claim.

## 5. Byte-level check of the fixed encoding (e7817ce, runtime.cpp:520-528, :577-580; gpuvm.cpp:11-28)

| Item | Kext after fix | amdgpu GC 12.0 dGPU | Equal? |
|---|---|---|---|
| VRAM leaf | VALID\|R\|W\|IS_PTE, (+EXE), MTYPE 0, FRAG 0/4 | pde_flags = VALID; SNOOPED only if `bus.caching == ttm_cached`, which needs `connected_to_cpu` (amdgpu_ttm.c:1456-1458; vram_mgr.c:610-611). Plus gart (UC\|EXE\|IS_PTE), R, W. Then get_vm_pte: EXE per mapping, MTYPE→NC = 0 (gmc_v12_0.c:513-523; soc24_enum.h:611), NOALLOC off, DCC only for DCC BOs. | Yes [FACT] |
| EXE policy | PQ, IB, user buffers, code: X. EOP/rptr/wptr/data/fence/kernarg: no X. | Mesa maps every BO R\|W\|X (ac_linux_drm.c:235) | Stricter than Mesa. Fine: the EOP IV is READ only (L:330) [INFER] |
| Host leaf | SYSTEM\|SNOOPED\|VALID\|R\|W\|IS_PTE (+EXE), MTYPE 0 | Cached TT: SYSTEM\|SNOOPED (amdgpu_ttm.c:1446-1453), plus gart flags, R/W, and get_vm_pte (NC) | Yes [FACT] |
| PDE | phys \| VALID. Bit 63 = 0, BFS 0 (gpuvm.cpp:22-28) | `get_pde_for_bo` VALID. `get_vm_pde` only rebases the address (translate_further is never set for v12; the only setter is gmc_v9_0) | Yes [FACT] |
| Base reg | 0x10000001 (L:302) | pd_addr \| VALID | Yes |
| Bit 63 survives | `encodePte` masks only PA/FRAG/MTYPE (gpuvm.cpp:12-14). `vmPteSet/Clear` default 0 (compute.hpp:631); only ladder d clears it (runtime.cpp:1277). | n/a | Yes [FACT] |
| Expected readback | EOP `0x8000000009a01061`; PQ `0x8000000009a02071` (kPqSize 0x1000, compute.hpp:301, so FRAG 0) | n/a | n/a |

## 6. What could still fail after the fix

| # | Risk | P | Notes |
|---|---|---|---|
| 1 | A warm reboot after a faulting boot leaves the MEC slot wedged. The fixed build then shows fence 0 and status 0, which would be misread as "fix failed". | ~10% per run if not cold-booted | L:362-370 pattern; memory rule "cold power cycle". Operational, not a walk failure. |
| 2 | The walker reads different memory than the SDMA readback (PDE address semantics, GL2/MALL visibility of SDMA-written tables) | 5-8% | Candidates 4 and 6 in rootcause-vm.md. Variant T (CPU-written tables) and c never ran cleanly. |
| 3 | VMIDs 8-15 need MES/firmware per-VMID state | 3-5% | Weakened: the fault is attributed to VMID 8 with a walker error, so context 8 is being walked. E4 from a VMID0 queue fails the same way. |
| 4 | A permission fault on another CP fetch that requests EXE (EOP, wptr poll) | ~3% | IV says EOP is READ only. A status with MAPPING 0 / WALKER 0 and PERM bit 3 would show it. |
| 5 | GPU→CPU visibility of fence/data with MTYPE NC | ~2% | RELEASE_MEM has GL2_WB, identical to gfx_v12_0.c:4565-4594 (pm4.cpp:73-86). |
| 6 | Hidden hub state (CONTEXTS_DISABLE, TRANSLATION_BYPASS_BY_VMID) | ~1-2% | A bypassed or disabled VMID would not produce a VMID-8 MAPPING fault. |
| 7 | FRAG=4 claimed wrongly | ~1% | Checked for alignment and containment (runtime.cpp:529-536). |

## 7. What would refute it

The first VM access of a **cold** boot with `rdna4-vm=1` and no diag (rootcause-vm.md s.6):
- Unchanged `0x00800b3b` at VA 0x100001000 means B is refuted (A may still hold).
- A status with MAPPING 0 / WALKER 0 and a PERM bit means B holds and a permission bit is still missing.
- Fence 0x564d0001 plus data 0x600df00d means CONFIRMED.
- Then, in a separate last boot, `rdna4-vm-ispte=0` must bring back 0x00800b3b. That is the A/B.

The emulator reproduction is circular and is not evidence (w36-review.md S1).
