# RDNA4FB real-card batch plan

## How to run it (one loop per boot)

The USB stick "OPENCORE" holds the new kext, `rdna4-run`, `diagnostic-log.sh`,
`set-boot.sh`, this plan, and a backup of the previous working setup in
`backup-before-premetal/`. The stick is already set for **boot 1**.

1. Boot the USB into macOS Recovery, open Terminal.
2. Run the batch: `bash /Volumes/OPENCORE/diagnostic-log.sh`. It ends with the
   PASS/FAIL/SKIPPED table and saves the log on the stick.
3. Select the next boot: `bash /Volumes/OPENCORE/set-boot.sh 2` (then 3, 4, 5;
   6 is optional), then reboot and repeat.

You can stop after any boot; each one is useful on its own.
`bash /Volumes/OPENCORE/set-boot.sh 0` returns to this morning's arguments.

**To roll back the kext** if a boot misbehaves even with `set-boot.sh 0`,
replace `EFI/OC/Kexts/RDNA4FB.kext` with the copy in
`backup-before-premetal/` (from Windows, or from Recovery with
`cp -R /Volumes/OPENCORE/backup-before-premetal/RDNA4FB.kext /Volumes/OPENCORE/EFI/OC/Kexts/`
after removing the new one).

## The boots

Each boot's summary is described below. The arguments listed are the feature
arguments; `set-boot.sh` adds them to the usual base arguments.

The script writes a log on the USB stick and ends with a PASS, FAIL or
SKIPPED line for runtime, SubmitIb, fault-page scrub, VM, IH, vblank, GFX,
flip, animation, queue recovery, sensors and the optional sleep cycle. Save
the log before changing boot arguments. Every test is bounded; a failed step
is recorded and the script continues.

1. **Interrupt ring.** Boot with
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=1`. The summary should show
   `ih PASS` with `ring up`, `submitib PASS`, `fault PASS`, runtime selftest
   and bench PASS, and sensor values. The kernel log should contain
   `RDNA4FB: ih: ring up` and the bounded SDMA/CP self-test result. A missing
   MSI or IH source is a FAIL or polling fallback; keep the log and continue
   to the next boot only after a reboot. If the interrupt source hangs, the
   next boot falls back to polling for the `ih:` trail and still reports the
   other features.
2. **Per-client GPUVM.** Add `rdna4-vm=1`:
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=1 rdna4-vm=1`. The summary should
   show `vm PASS` with VM isolation and concurrent queue checks, and keep
   `fault PASS` from the freed-VA dispatch. The kernel log should show
   separate VMIDs, client queue setup and the fault recovery. A VM hang uses
   the `vm:` trail; reboot and let that feature be skipped while preserving
   the rest of the report.
3. **Display interrupts and flips.** Use
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=2 rdna4-vm=1 rdna4-flip=1`.
   The summary should show `vblank PASS` for 120 waits and `flip PASS` after
   `show 3` restores the desktop. Check the vblank interval, pflip/flip
   lines, and the visible display. A vblank or pflip hang leaves the `ih:` or
   `flip:` trail; reboot and continue with the other rows. W5c should show
   `anim PASS` with rendered frames and `desktop restored`; it is SKIPPED when
   that command is absent from the diagnostic image, and a hang falls back to
   the `flip:` trail.
4. **GFX ring and first draw.** On a separate boot add `rdna4-gfx=1` to the
   previous arguments. A real card should show `gfx PASS` with
   `THE TRIANGLE IS RIGHT` and `8192` pixels, with the draw pixel count in the
   kernel log. If the dmesg ring wrapped, the bring-up trail
   `finished ... gfx draw right` is an equivalent bounded proof and is shown
   in the summary. A hang should leave a `gfx:` trail; reboot and run the
   script again so the next boot shows GFX SKIPPED while the other features
   still report results.
5. **Doorbell GFX ring.** On another boot replace the GFX argument with
   `rdna4-gfx=2`:
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=2 rdna4-vm=1 rdna4-flip=1 rdna4-gfx=2`.
   Expect the same ring/draw signs, now using the doorbell path. This is a
   separate risk from mode 1; do not combine it with a failed mode-1 retry.

W6 queue recovery, W9 sensors, and the runtime selftests run automatically
when their required runtime is present. The debug-only sleep cycle runs only
with `rdna4-sleeptest=1`; use that argument in the emulator dry run, where
the monitor supplies the device power reset. Native macOS S3 remains a
platform test and is not required for this diagnostic selector.

6. **Optional: macOS vblank interrupts and the hardware cursor (W14).**
   `set-boot.sh 6`: `rdna4-ih=2 rdna4-vm=1 rdna4-flip=1 rdna4-vbl=1 rdna4-cursor=1`.
   This one changes the live desktop, so run it last. Check by eye: the mouse
   pointer shows and moves normally (a hardware-cursor failure falls back to
   macOS's software cursor, which also looks normal). The kernel log should
   show the VBL service created, `vblank: OTG0 count 60` and cursor csc calls;
   the table still reports the other rows as in boot 3.

W12 (`rdna4-run tri`, a triangle drawn by an app) is not merged yet and is not
part of this batch.

## Known risks

VMIDs 8-15 use the GC-hub `GCVM_INVALIDATE_ENG17` MMIO path. On this card it
acknowledges after `gfx_v12_0_config_gfx_rs64` has run (PFP/ME/MEC start
addresses plus pipe reset); stage 5 performs that configuration first. The
emulator reproduces a missing acknowledgement with
`RDNA4_DEV=inv-noack=on`. If `vmid N: VM invalidate timeout` still appears in
the VM or on hardware, this is an expected VM FAIL result, not a crash.
Compute remains up and per-client GPUVM turns off, while the rest of the batch
continues. The compact
`RDNA4FB,Results` property records `vm = FAIL invalidate timeout` even if the
dmesg buffer wrapped. The fix path is in-ring invalidation on the gfx/compute
ring, planned by W12 D1.

If a boot freezes before the summary is written, power-cycle into the USB
Recovery environment and run the script with the same arguments. The last
NVRAM trail identifies the risky step. On the next boot the matching feature
is skipped once, so the summary can collect the other features; preserve
that log and report the trail before trying the feature again.
