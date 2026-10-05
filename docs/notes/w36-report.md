# W36 vmfix5: IS_PTE (bit 63) on leaf PTEs (branch premetal/vmfix5, off premetal/int 6e996d7)

Root cause per premetal/rootcause-vm.md, re-verified in the source: `amdgpu_ttm_tt_pte_flags()` does
`flags |= adev->gart.gart_pte_flags` (amdgpu_ttm.c:1477) and gmc_v12_0.c:794-796 defines that as MTYPE_UC | EXECUTABLE | IS_PTE, so every
normal GC 12 leaf PTE carries bit 63; nothing clears it. My W22 finding ("not set on normal PTB leaves") was wrong: I read
gmc_v12_0_get_vm_pte and amdgpu_vm_pt.c but not the body of amdgpu_ttm_tt_pte_flags.

Changes: gpuvm.hpp `kIsPte`, gpuvm.cpp comment fixed and `GpuVm::walk` faults on a last-level entry without bit 63 (GFX12 walks it as a
directory, umr access_vram_ai.c:1063-1069); runtime.cpp vmMap/vmMapHost set VALID|READ|WRITE|IS_PTE (+EXEC where asked), no SNOOPED on
VRAM (amdgpu_ttm.c:1456-1458), host pages keep SYSTEM|SNOOPED; PQ ring, IB pages (boot self-test, E4, rtOpen) and user buffers are mapped
executable (the CP fetches with EXE); boot-arg `rdna4-vm-ispte=0` (negative control, logged); ladder d = old encoding (no IS_PTE), e = SNOOPED added,
g = every leaf EXECUTABLE. Emulator: last-level entry without bit 63 faults with status 0x00800b3b (WALKER 5, PERM 3, MAPPING, CID 5), PQ and IB
fetches require EXECUTABLE. Host test: a leaf without bit 63 fails the walk.

Dry runs: boot-2 args, boot-2 + rdna4-vm-diag=4065 and boot-4 args: baseline PASS, `PT[eop] 0x8000000009a01061 ok`, selftest PASS.
rdna4-vm-ispte=0 (+diag 4065): fault `0x00800b3b` on the baseline and E4, VM disabled, selftest PASS (the round 2-4 kext now fails in the VM like the card).
Real-card expectation (rootcause-vm.md s.6): no `vm: boot queue test failed`, fence 0x564d0001, data 0x600df00d, status 0, Results vm=PASS. If a fault remains with
MAPPING_ERROR 0 / WALKER_ERROR 0, read PERMISSION_FAULTS (bit 3 = EXE, 1 = read, 2 = write).
