# RDNA4FB real-card batch plan

Run each boot from Recovery as root:

```sh
bash /Volumes/OPENCORE/diagnostic-log.sh
```

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

The remaining planned slots are intentionally reserved until their
workstreams land. W14 will add a boot with `rdna4-vbl=1 rdna4-cursor=1`; its
summary must report vblank and cursor PASS, with the `vbl:` or `cursor:` trail
as the per-feature fallback. W12 will add `rdna4-run tri`; its summary must
report the triangle PASS and fall back to the `gfx:` trail on a hang. These
selectors are not read by this branch yet, so do not add them to a real-card
boot until W14/W12 are merged; the current G3 slot above is the active
triangle acceptance.

If a boot freezes before the summary is written, power-cycle into the USB
Recovery environment and run the script with the same arguments. The last
NVRAM trail identifies the risky step. On the next boot the matching feature
is skipped once, so the summary can collect the other features; preserve
that log and report the trail before trying the feature again.
