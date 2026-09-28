# RDNA4FB real-card batch plan

## How to run it (one loop per boot)

The USB stick `OPENCORE` holds the kext, `rdna4-run`, `diagnostic-log.sh`,
`set-boot.sh`, this plan, and the rollback copy in
`backup-before-premetal/`.

1. Boot the USB into macOS Recovery and open Terminal.
2. Run the normal batch with `bash /Volumes/OPENCORE/diagnostic-log.sh`.
   It saves an absolute-path log next to the script and prints the
   PASS/FAIL/SKIPPED table.
3. Select the next boot with `bash /Volumes/OPENCORE/set-boot.sh 2` (then 3,
   4, 5; boot 6 is optional), reboot, and repeat.

Boot 0 is today's known-good argument set: it enables no new behavior at all.
The summary reads the
durable `RDNA4FB,Results` IORegistry dictionary first and uses the current
boot's dmesg as supporting evidence. A PASS requires the command's result and
the registry result to agree. The optional
`bash /Volumes/OPENCORE/diagnostic-log.sh hang` step is last, on boot 2;
ordinary collection never runs a hang test.

To roll back the kext, replace `EFI/OC/Kexts/RDNA4FB.kext` with the copy in
`backup-before-premetal/`. `set-boot.sh` keeps the first config backup and
validates a new plist with `plutil` when that tool is available.

## The boots

The arguments below are added to the common base by `set-boot.sh`. Every
feature has a visible boot argument, a PASS sign in the summary, and a fallback
trail if it hangs.

1. **Interrupt ring (W1).**
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=1 rdna4-hang=1`.
   PASS requires the IH registry result plus SDMA/CP self-test lines and
   interrupt wakeups. Runtime selftest, bench, and sensors should also PASS.
   SubmitIb and freed-VA fault are SKIPPED because they require VM. On an IH
   failure preserve the `ih:` trail; the next boot can use its polling
   fallback while the remaining features continue.

2. **Per-client GPUVM (W2/W8).**
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=1 rdna4-vm=1 rdna4-hang=1`.
   PASS requires `vm PASS` from the registry plus the command's isolation and
   concurrent queue checks, `submitib PASS` for ordered IBs, and `fault PASS`
   for the freed-VA scrub. A VM failure leaves `vm FAIL`, keeps compute up,
   and falls back to non-VM queues; preserve the `vm:` trail after a hang.

3. **Display interrupts, flips, and animation (W1b/W5c).**
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=2 rdna4-vm=1 rdna4-flip=1 rdna4-hang=1`.
   `vblank PASS` requires 120 command waits and the vblank registry result;
   `flip PASS` requires `show 3` to restore the desktop and the flip registry
   result. `anim PASS` requires both `anim: frames rendered` and
   `anim: desktop restored`. A failed display test leaves `ih:` or `flip:` for
   the next boot; animation is skipped if its command is unavailable.

4. **GFX ring and first draw (G3).** Add `rdna4-gfx=2` to boot 3 (`=1`, the MMIO write pointer, halts PFP/ME on the card and is now an alias of 2).
   `gfx PASS` is recorded in the current boot's `RDNA4FB,Results` value,
   containing `THE TRIANGLE IS RIGHT` and 8192 pixels. The registry is read
   per boot, so this durable result is the batch evidence; a stale NVRAM trail
   never proves PASS. A ring or draw hang leaves a `gfx:` trail, and the next
   boot skips that feature once.

5. **GFX ring without the VM.** Same as boot 4 but without `rdna4-vm=1`, so the triangle is tested independently of the VM failure:
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=2 rdna4-flip=1 rdna4-gfx=2 rdna4-hang=1`.
   Expect the same ring and triangle signs through the doorbell path. A
   failure leaves the `gfx:` trail and mode 1 remains the fallback.

W6 recovery is enabled by `rdna4-hang=1`, but its destructive selftest and
`hangtest` run only when the script is explicitly invoked with `hang`. A
successful final step is `w6 PASS`; a timeout is reported as TIMEOUT and the
trail identifies the recovery step.

W9 sensors run only from `rdna4-run sensors`, with measured or emulator
synthetic values. There is no boot-time sensor read. Power management and the
W9 sleep cycle are not in this batch: they require the separate opt-in
`rdna4-pm=1` and, for the emulator selector, `rdna4-sleeptest=1`.
Do not put the Mac to sleep during this batch; leave sleep testing for the
separate power-management work.

6. **Optional W14 vblank service and hardware cursor.**
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=2 rdna4-vm=1 rdna4-flip=1 rdna4-vbl=1 rdna4-cursor=1 rdna4-hang=1`.
   Run it last. PASS is the VBL service result, stable vblank counts, and a
   working pointer; a cursor failure falls back to the software cursor and
   leaves the `cursor:` trail. W12's future `rdna4-run tri` gets the next plan
   slot; its PASS will require the app marker plus a durable registry result,
   with `tri:` as the hang fallback.

## Known risks

VMIDs 8-15 use the GC-hub `GCVM_INVALIDATE_ENG17` MMIO path. On this card it
acknowledges after `gfx_v12_0_config_gfx_rs64` has run (PFP/ME/MEC start
addresses plus pipe reset); stage 5 performs that configuration first. The
emulator reproduces a missing acknowledgement with
`RDNA4_DEV=inv-noack=on`. If `vmid N: VM invalidate timeout` appears, VM fails
gracefully: compute stays up, per-client GPUVM turns off, and the other rows
continue. The `RDNA4FB,Results` property records `vm = FAIL invalidate timeout`
even if dmesg wrapped. The fix path is in-ring invalidation on the
gfx/compute ring, planned by W12 D1.

If a boot freezes before the summary is written, power-cycle into Recovery and
run the script with the same arguments. The last NVRAM trail identifies the
risky step. On the next boot the matching feature is skipped once; preserve
that log and report the trail before retrying it.

