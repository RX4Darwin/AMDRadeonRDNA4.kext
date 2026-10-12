# W40 vmfix6: round-5 VM fault 0x00800880 = EXECUTE permission on the EOP page (branch premetal/vmfix6, off int bacb27b)

Round 5 (E:\rdna4fb-diag-20260930-053316/053346): after the W36 IS_PTE fix the first VMID 8 access still faulted:
`fault status 0x00800880 (VMID 8, CID 0x4, read), VA 0x100001000`, IV src_data 0x00100001 0x00000050.

## Decode (gc_12_0_0_sh_mask.h GCVM_L2_PROTECTION_FAULT_STATUS_LO32)
MORE_FAULTS [0]=0, WALKER_ERROR [3:1]=0, PERMISSION_FAULTS [7:4]=8 (bit 7 = EXECUTE), MAPPING_ERROR [8]=0, CID [17:9]=4 (CPF), RW [18]=0 (read),
VMID [23:20]=8. IV src_data[1] 0x50 = READ (0x40) | EXE (0x10) (amdgpu_gmc.h:93-95). So the WALK SUCCEEDED (W36 was right); the CP fetched the EOP
buffer page with READ|EXE and the leaf allowed R|W only. The same log shows the ring ran: `data 0x600df00d` (WRITE_DATA landed), rptr 0xd; only the
RELEASE_MEM/EOP step stalled (fence 0).

## Level by level against Linux (E:\linux\...\vm-walk.txt, VMID 6/7)
- Base register: address | 1 (0x3f7d7e001 vs our 0x10000001): same format. CNTL 0x03fffc07 both. Depth 3, block 0.
- PDEs: valid only (0x3f7d7b001 ...), ours 0x10001001 etc.: equal. (Linux also uses PDE-as-PTE 2 MiB leaves with bit 63 and frag 9/11; we use 4 KiB PTEs.)
- Leaf PTE: Linux 0x80000000066003f1 / 0x80000003d38005f1 = bit63 | frag | **exe=1** read=1 write=1 valid, no SNOOPED, MTYPE 0.
  Ours after W36: 0x8000000009a01061 = bit63 | read | write | valid: **exe = 0**. That is the only difference left.
- Not causes: invalidation and visibility (tables read back exact through SDMA; the walk works), HDP, PTB alignment, block/frag (frag 0 is fine).

## Fix
`vmMap`/`vmMapHost`: every leaf PTE is EXECUTABLE (Linux ground truth: exe=1 on all leaves; Mesa maps every BO R|W|X, ac_linux_drm.c:235). Result for the EOP page
0x8000000009a01071. Boot-arg `rdna4-vm-exec=0` restores the old R|W-only pages (only ring/IB executable) as the negative control. Ladder variant g = that old encoding.
Emulator: the EOP-buffer access now requests EXECUTE (the card's IV), and an execute-only permission failure reads back like the card, 0x00800880
(PERMISSION 8, CID 4, no mapping error).

## Verification (emulator)
build clean, make test 'all checks passed'; boot-2 args, boot-2 + diag=4065, boot-4 args: baseline PASS, `PT[eop] 0x8000000009a01071 ok`, selftest PASS.
`rdna4-vm-exec=0` (+diag 4065): `fault status 0x00800880 (VMID 8, CID 0x4, read), VA 0x100001000` = the card's round-5 fault, VM disabled, selftest PASS.
Real-card expectation: no `vm: boot queue test failed`, fence 0x564d0001, data 0x600df00d, status 0, Results vm PASS.
