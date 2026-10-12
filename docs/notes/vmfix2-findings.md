# W17 vmfix2: VMID8 page walk faults on silicon (R2-1)

Status: root cause NOT proven. Static diff against amdgpu found no walker-breaking
difference; the emulator was NOT changed (see "Why no strict emulator yet").

Fault (real card, round 2): `GC hub fault status 0x00800b3b` = MORE_FAULTS 1, WALKER_ERROR 5,
PERMISSION_FAULTS 3, MAPPING_ERROR 1, CID 5 (CPC, gfxhub_v12_0.c gfxhub_client_ids), VMID 8,
read, VA 0x100001000 (EOP buffer, first access of the queue), rptr 0.

## Matches amdgpu (checked, no change needed)

| Item | Ours | amdgpu |
|---|---|---|
| Register offsets CONTEXT1_CNTL/BASE/START/END (0x1625/0x1691/0x16b1/0x16d1, stride 1/2/2) | gfxregs.hpp:159-165 | gc_12_0_0_offset.h:2894,3110,3174,3238; ctx_distance gfxhub_v12_0.c:492 |
| CNTL fields: DEPTH [2:1], BLOCK_SIZE [7:4], fault bits 10..23 | runtime.cpp:640 | gc_12_0_0_sh_mask.h:9420-9457 |
| PAGE_TABLE_DEPTH 3 / block size 9 (root = PDB2, 4 levels) | gpuvm.hpp:23-24 | amdgpu_vm.c:2426-2456 (num_level 3, block 9); no translate_further on gfx12 (gmc_v12_0.c:493 only) |
| PDE = phys | VALID only | gpuvm.cpp:22 | amdgpu_gmc_get_pde_for_bo (amdgpu_gmc.c:112-130), gmc_v12_0_get_vm_pde (gmc_v12_0.c:485) |
| PT_BASE = root phys | VALID | runtime.cpp:642 | amdgpu_gmc_pd_addr (amdgpu_gmc.c:144-145) |
| phys = FB_OFFSET<<24 + (mc - fb_start) | gpuvm.cpp:36 | gmc_v12_0.c:489,706-710 |
| PTE flags VALID/READ/WRITE/(EXEC), MTYPE NC(0), frag 4 for 64K | runtime.cpp:515 | gmc_v12_0_get_vm_pte (gmc_v12_0.c:507-549) |
| SNOOPED on a VRAM PTE | runtime.cpp:515 | legal: amdgpu_ttm.c:1457-1458 sets it for cached VRAM |
| L1 TLB, L2 CNTL/2/3/4/5 (BANK 9, BIGK 6, TAP PHYSICAL 0) | compute.cpp:1255-1273 | gfxhub_v12_0.c:242-289 |
| Invalidate request (VMID bit + PTES/PDE0-2/L1) | runtime.cpp:663 | gfxhub_v12_0_get_invalidate_req (gfxhub_v12_0.c:61-77) |
| Identity aperture off, invalidation ranges | compute.cpp:1276-1284 | gfxhub_v12_0_disable_identity_aperture / program_invalidation |

## Differences found (none obviously fatal)

1. **Context 0 (VMID0) range**: START=END=0 (compute.cpp:1219-1222); amdgpu programs the GART
   range `gart_start>>12 .. gart_end>>12` (gfxhub_v12_0.c:162-170). Matters only if table fetches
   are translated through VMID0 (next section).
2. **L2_CNTL ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY cleared** (compute.cpp:1266) vs set to 1
   (gfxhub_v12_0.c:248). Deliberate ("no host address reachable"); affects faulting accesses only.
3. **CONTEXTn_CNTL INTERRUPT bits set** (0xfffc00 mask, runtime.cpp:638) vs only *_DEFAULT bits
   (gfxhub_v12_0.c:305-322). Harmless.
4. **END_ADDR_HI32 = 0xffff** (runtime.cpp:649) vs `max_pfn-1` = 0xf_ffff_ffff (a 4-bit HI field;
   gfxhub_v12_0.c:337-341). Range still covers the VA.
5. Emulator compares VA (bytes) against START/END, which the hardware defines in 4 KiB pages
   (gfxhub_v12_0.c:170, 337). Emulator bug, not a kext bug (harmless with the values we write).

## Leading hypotheses for the walker error (unproven, ranked)

A. **Table fetches are not "physical".** amdgpu explicitly clears VMC_TAP_PDE/PTE_REQUEST_PHYSICAL
   (gfxhub_v12_0.c:262-263; reset value 0xc1 has both set, kext gfxregs.hpp:222). With them clear the
   walker's PDE/PTE reads may be *translated* (system aperture or context 0), not raw. Our PDE
   addresses are small VRAM offsets (FB_OFFSET reads 0 on this card: log line "mmhub fb ... offset=0x0"),
   which are outside the system aperture (MC 0x8000000000+) and outside context 0's one-page range ->
   an unmapped table read = WALKER_ERROR + MAPPING_ERROR on the very first access. Not provable from
   amdgpu sources: the semantics of the bits are undocumented there, and amdgpu itself needs
   phys == an aperture-visible address for this to work. Needs a card experiment.
B. **PDB-root/PT cache state**: the tables reach VRAM by SDMA (vmTableSync) and are never read back on
   the card. A readback-and-log of PDB2[0], PDB1[4], PDB0[0], PT[1] via BAR0 (or SDMA) before the
   HQD kick would prove the walker has valid entries to read.
C. **VMID 8 CP-side setup** (SH_MEM_*/APE1 for VMIDs 8-15, gfx_v12_0_init_compute_vmid) is not
   programmed; shader-side only, unlikely for a CPC fetch.

## Why no strict emulator yet

The rule is that the model follows amdgpu/the card. Nothing in the amdgpu tree defines what the
walker does with the TAP bits, so a strict model of hypothesis A would be a guess dressed as a
model. Only the PFN-unit START/END (difference 5) is citable, and the old kext already passes it,
so it cannot serve as the negative control.

## Proposed next real-card round (no emulator dependency)

In vmBootSelfTest, before the kick, log a readback of the four table entries (B) and, on failure,
retry once with each of: (1) context 0 START/END widened to the GART-style range covering the table
physical addresses; (2) table pointers expressed as MC addresses (phys = mc, i.e. inside the system
aperture). Log which variant lets the EOP read succeed. Each variant only affects VMID8's own
queue, so a failure keeps the existing graceful "VM disabled" outcome.
