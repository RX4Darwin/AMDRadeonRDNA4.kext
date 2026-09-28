# RDNA4FB real-card batch plan

Run each boot from Recovery as root:

```sh
bash /Volumes/OPENCORE/diagnostic-log.sh
```

The script writes a log on the USB stick and ends with a PASS, FAIL or
SKIPPED line for runtime, VM, IH, vblank, GFX, flip, animation, queue
recovery, sensors and the optional sleep cycle. Save the log before changing
boot arguments.

1. **Interrupt ring.** Boot with
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=1`. The summary should show
   `ih PASS` with `ring up`, runtime selftest and bench PASS, and sensor
   values. The kernel log should contain `RDNA4FB: ih: ring up` and the
   bounded SDMA/CP self-test result. A missing MSI or IH source is a FAIL or
   polling fallback; keep the log and continue to the next boot only after a
   reboot.
2. **Per-client GPUVM.** Add `rdna4-vm=1`:
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=1 rdna4-vm=1`. The summary should
   show `vm PASS` with VM isolation and concurrent queue checks. The kernel
   log should show separate VMIDs and client queue setup.
3. **Display interrupts and flips.** Use
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=2 rdna4-vm=1 rdna4-flip=1`.
   The summary should show `vblank PASS` for 120 waits and `flip PASS` after
   `show 3` restores the desktop. Check the vblank interval, pflip/flip
   lines, and the visible display. Keep `rdna4-run anim 5` SKIPPED when that
   command is absent from the diagnostic image.
4. **GFX ring and first draw.** On a separate boot add `rdna4-gfx=1` to the
   previous arguments. A real card should show the GFX ring test and first
   draw PASS, with the draw pixel count in the kernel log. A hang should leave
   a `gfx:` trail; reboot and run the script again so the next boot shows GFX
   SKIPPED while the other features still report results.
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

If a boot freezes before the summary is written, power-cycle into the USB
Recovery environment and run the script with the same arguments. The last
NVRAM trail identifies the risky step. On the next boot the matching feature
is skipped once, so the summary can collect the other features; preserve
that log and report the trail before trying the feature again.
