# W30 vmfix4: round 4 re-read, fault default page, remaining candidates (branch premetal/vmfix4, off int 4f8bca4)

Log: premetal/hw-logs/rdna4fb-diag-20260929-081555.txt (boot 8). amdgpu paths are relative to drivers/gpu/drm/amd.

## 1. The round-4 premise ("after the first fault the whole MEC stops") is not shown by the log

What the log does show:
- Baseline (pipe 0 queue 1): fault 0x00800b3b as before; the queue's doorbell was consumed.
- E4 (pipe 1 queue 3, a real slot): IB fetch in VMID 8 faults 0x008009ba (CID 4, VA 0x100006000, WALKER_ERROR 5
  again); doorbell control 0x400000c8 = its own doorbell (0x32 << 2), consumed. Its dequeue did not finish, so the
  code marked pipe 1 unusable and moved the tests to "pipe 2".
- The control on "pipe 2 queue 0" then printed the HQD registers of the **kernel ring**: `PQ base 0x8008c020`
  (= kMqdOffset+0x2000, the kernel PM4 queue), `vmid 0`, `doorbell 0xc0000018` (dword 6 = kComputeDoorbellDword),
  `rptr/wptr 0xa1`. A distinct queue would have read back its own PQ base 0x1000000, VMID 8 and doorbell 0x1a-0x30.
  Reason: GC 12.0.0/12.0.1 has **one MEC with 2 pipes of 4 queues** (gfx_v12_0.c:1415-1423:
  `num_mec = 1, num_pipe_per_mec = 2, num_queue_per_pipe = 4`). Pipe ids 2-3 and queue ids 4-7 do not exist and, as this
  log shows, alias real slots ("pipe 2 queue 0" = pipe 0 queue 0 = the kernel ring). So the control never ran on a fresh
  slot; it hit the kernel ring's HQD (hqdInitFor's "drain a queue left behind" step, the doorbell HIT on the kernel
  doorbell, `dequeue timed out` on it). Everything after E4 in round 4 is invalid, and it is not evidence that the MEC
  stops globally. What is established: a queue that took a VMID 8 fault does not come back (rounds 3 and 4).
- Side effect that mattered: the kernel ring itself was touched by that control, which is why compute/runtime failed on boot 8.

Fixes: `hqdInitFor` refuses pipe >= 2 or queue >= 4 (compute.cpp); `rtOpen` allocates only pipes 0-1 x queues 0-3 (7 client
queues, was 4 pipes x 8 queues, i.e. clients 5-7 aliased clients/kernel ring); the emulator now aliases the missing GRBM_GFX_CNTL
pipe/queue id bits the same way (a queue programmed on "pipe 2" lands on pipe 0), so the round-4 kext's control collides with the
kernel queue in the emulator too (the negative control for this finding). Valid slots: pipe 0: q0 kernel ring, q1 baseline, q2/q3;
pipe 1: q0-q3. Tests use pipe 1 q0-q2, then pipe 0 q2-q3 (E4: pipe 1 q3) and reuse them round-robin.

## 2. Fault default page like amdgpu (mask bit 512, "F")

amdgpu (gfxhub_v12_0.c): `GCVM_L2_CNTL.ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY = 1` (l.248),
`GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR = dummy_page_addr` (l.186-191, a system page), the per-context and L2 `*_PROTECTION_FAULT_ENABLE_DEFAULT`
bits set (`setup_vmid_config`, `set_fault_enable_default`) and `SYSTEM_APERTURE_DEFAULT_ADDR` = the VRAM scratch page (l.174-180).
The kext already has the DEFAULT bits (FAULT_CNTL 0x3ffffffc, CONTEXT8 0x00fffc07), the system-aperture default page in VRAM, and a fault default
address in VRAM, but bit 11 of L2_CNTL cleared (a deliberate deviation: "a stray access must not reach a host address").

Why not a VRAM page with bit 11 set: with that bit set the L2 fault default address is a SYSTEM address, so a VRAM offset would be sent to
the bus as a host physical address, and a faulting write would land in whatever memory sits there. Why not a VRAM page with the bit clear:
that is today's configuration; if the bit selects "default page lives in system memory" (as its name says) this is the case that may leave
a faulted engine without an answer. So F does the amdgpu thing safely: it allocates **one zeroed, physically contiguous, IOMapper-mapped 4 KiB
page of our own** (same allocation pattern as the DMA bounce buffer, PCI bus master already on), programs its bus address as the fault
default page, sets bit 11, and restores both at the end of the boot self-test, then frees the page. A faulting read returns zeros, a faulting
write lands in that private page. F is applied **before the baseline queue runs**, so the baseline and every test run under it; if F is what
was missing, the baseline itself stops faulting-and-stalling. It needs `rdna4-vm-diag` (default off) and DMA up.

## 3. Root-cause candidates from all round 2-4 evidence, each with a cheap log or test

Evidence: tables in VRAM are exactly the built ones (SDMA readback ok), windows equal on both hubs, CONTEXT8 CNTL 0x00fffc07 / BASE 0x10000001 /
START 0 / END 0xf:ffffffff, L2 CNTL/CNTL2/3/4/5 amdgpu-equal, PTE/PDE bits per amdgpu, the first access of every client faults (CPC EOP fetch
CID 5, CPF IB fetch CID 4) with WALKER_ERROR 5, MAPPING_ERROR 1. Only the very first fault of a boot is a valid observation.

| Candidate | Check in boot 8 |
|---|---|
| Invalidation not taking effect | new read-only line in every `vm: diag` dump: invalidate engine 17 REQ/ACK/SEM and GCVM_L2_STATUS; vmInvalidate already fails loudly on an ACK timeout (never seen); variant `o` adds the L2_CNTL2 INVALIDATE_ALL_L1_TLBS/INVALIDATE_L2_CACHE pulses |
| CONTEXT8 programmed in the wrong order (enable before base) | variant `o`: CNTL := 0, then BASE, START, END, L2 invalidate, then CNTL := enable |
| L2/L1 caching an old "no table" state (GOP/previous boot) | variant `o` (cache invalidate pulses + per-VMID invalidate) |
| VMID 8 is special (MES/RLC-owned, KFD range) | variant `V`: the control on VMID 12 (same tables, a fresh context) |
| PDE/PTE bit 1 SYSTEM vs VRAM | not a safe test: SYSTEM=1 makes table fetches and data accesses host physical addresses (a WRITE_DATA/RELEASE_MEM would write host RAM). The readback proves bit 1 is clear (PTE 0x9a0x065 = bits 0,2,5,6, PDE ...001) and amdgpu leaves it clear for VRAM (amdgpu_ttm.c:1447-1452). Skipped on purpose |
| PAGE_TABLE_BASE must be an MC address on GC 12.0.1 | variant `c` (MC-form PDEs and base); amdgpu writes the physical form, gmc_v12_0.c:485-491, gfxhub_v12_0.c:120-135 |
| Missing PTE bit (IS_PTE), SNOOPED, EXECUTABLE | variants d, e, g (W22) |
| SDMA-written tables not what the walker reads | variant T (CPU-written tables in the pool) |
| Fault handling stalls the engine (dummy page) | F |
| MES-only per-VMID state we cannot see (there is no MMIO HQD-load path on gfx12, amdgpu_amdkfd_gfx_v12.c:519-533) | not testable without MES; E4/control/V decide whether it is the queue or the hub |

## 4. Mask for boot 8

`rdna4-vm-diag=4065` = F 512 + V 2048 + o 1024 + T 256 + g 128 + e 64 + d 32 + E4 1 (+ control). Boot 16 (mask 30) stays the a/b/c/E2 hub-write round.
Order: F, baseline, E4 (pipe 1 q3), control, o, V, d, e, g, T. Read: E4 passes -> walker fine, HQD side; control faulting like the baseline on a
real fresh slot -> the method works; V passing -> VMID 8 is special; o/d/e/g/T passing names the fix; nothing passing under F -> the missing piece is
outside what the kext can see (MES-programmed state).
