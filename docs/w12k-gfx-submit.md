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
