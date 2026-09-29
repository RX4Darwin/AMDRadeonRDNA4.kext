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
   4, 5; boots 6 and 7 are optional), reboot, and repeat.

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
   The cursor evidence is also in the registry property `RDNA4FB,Cursor` (printed
   by diagnostic-log.sh). **Boot 9** is the same with `rdna4-cursor=2`: a magenta
   64x64 square at (100,100) that macOS's pointer does not replace; see
   `docs/cursor-audit.md` for how to read the result.

6b. **Round 4 draw (W23).** Boot 10 (`rdna4-gfx=2 rdna4-gfxdiag=11`): the gfx golden registers are written at ring
   bring-up (rev_id from the NBIF RCC_STRAP0 at dword 0x1c); if the draw is still empty the ladder re-runs it with one open
   question changed at a time, in the order 8 (a PS that stores a marker), 2 (INST_PREF_SIZE), 1 (USER_SGPR).
   **A/B control (boot 11, `rdna4-gfxgolden=0`): the goldens are not undone by skipping them and survive a warm restart, so
   power-cycle the machine before boot 11 (or run boot 11 before boot 10);** the log says `golden: already in force from
   an earlier boot` when a boot found them set. **Boot 12** (`rdna4-gfxdiag=4`) runs the GS_ALLOC_REQ NGG shader alone: it
   can hang the gfx pipeline and there is no reset, so if its result says `hang/` do a cold power cycle before anything else.
   `docs/gfx-draw-audit.md` lists the suspects and what each log line means. A `gfx-golden-strict` emulator result says
   nothing about the card, it only shows that the kext writes the registers.
   **Round 5 (W31): boots 10-12 also run `rdna4-gfxprobe=1`.** The draw stream is split at `NUM_INSTANCES` and `COPY_DATA` packets
   copy 22 context/SH/uconfig registers into memory on the ring (`draw probe mid/post: CP view ... N of M registers equal what the
   stream wrote`); `fault after <step>` shows which bring-up step makes the CPG read VA 0; `RS64 DC_BASE0 ...` prints the microengine
   base registers. The earlier MMIO context readback lags one context, so read the CP view instead (`docs/gfx-context-audit.md`).

7. **Optional W19 GFX power-management probe.**
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=1 rdna4-hang=1 rdna4-gfxpm=31`.
   Run it after boots 0-6 (the round-2 idle reading of 300 W / 3.2 GHz is
   what it investigates; see `docs/gfx-pm-audit.md`). Every boot's summary
   already carries `sensors-idle` (an SMU metrics sample taken before any
   selftest or bench) and `sensors-pm` (after them), each with two samples one
   second apart and a verdict. `rdna4-gfxpm` adds one boot-time experiment,
   logged as `pm:` lines (one `gfxpm` row): a baseline sample, then per bit 1
   `SetWorkloadMask(DEFAULT)`, bit 2 GFXCLK soft limits back to automatic,
   bit 4 a probe that caps the GFXCLK soft max at 1000 MHz for one sample and
   lifts it, each followed by a sample (GFXCLK, GFX/UCLK activity, VDD_GFX,
   power). `rdna4-gfxpm=8` only samples. PASS means the experiment finished,
   not that anything improved: read the `pm:` lines. A hang leaves a `pm:`
   trail and the next boot skips only this feature. Power is left as the SMU
   had it, except that a failed restore of the soft max is logged as a
   WARNING (compute stays capped at 1000 MHz until the next boot).
   Bit 16 (`gfxpm=31` includes it) adds the W24 engine survey: at the end of
   each bring-up stage (3-7, the gfx ring, the pm baseline and the end) the
   kext logs read-only `pm: survey <stage>:` lines with GRBM_STATUS/2 and
   GRBM_STATUS_SE0-3, CP_STAT/BUSY, CPC/CPF status/busy, RLC state, MES/ME/MEC
   halt state, SDMA status, the clock-gating registers, every active MEC and
   gfx HQD, and the SMU's average clock/activity/power, so the first stage at
   which the SMU reports 100 % activity is visible next to which engine is
   busy. `rdna4-gfxcap=<MHz>` (default off) sets the GFXCLK soft max through
   the proven SetSoftMaxByFreq path as a stopgap; the SMU keeps it across
   warm reboots, so remove it with `rdna4-gfxcap=0` (or a cold power cycle),
   not by dropping the boot-arg (`set-boot.sh 10` writes `rdna4-gfxcap=0`).


Boots 0-6 are not byte-for-byte unchanged in one respect: `rdna4-run sensors`
now also takes two `GetMetricsTable` samples one second apart (about five
`GetMetricsTable` messages per boot instead of one: idle baseline and post-bench,
each with its extra pair, plus 1 s of sleep each), and each request first
poisons `MetricsCounter` in the driver table. The messages are read-only SMU
traffic; no boot-arg is needed for them. If a `pm: cap probe` or `pm: cap
restore` step ever hangs, the following boot sends `SetSoftMaxByFreq(GFXCLK,
0xffff)` once (trail `pm: cap recover`); do a cold power cycle if compute still
looks capped at about 1000 MHz.

8. **W17 VM walker diagnostics (run last, only after boot 2 showed the VM failure).**
   `rdna4-compute=7 rdna4-trace=1 rdna4-ih=1 rdna4-vm=1 rdna4-hang=1 rdna4-vm-diag=481`
   (`set-boot.sh 8`). Boot 2 alone (no `rdna4-vm-diag`) already logs the read-only
   evidence: `vm: E1 after SMU enable / before kick / after fault` (both hubs'
   window and aperture registers), `vm: diag before kick` (context and L2
   registers, page-table entries read back through SDMA as `ok`/`MISMATCH`) and
   the fault status. Boot 8 adds hardware-writing diagnostics after the baseline
   fails. Round 3 showed a VM fault leaves that HQD slot unserviced (doorbell HIT
   stays set, no fault latches), so every test gets a FRESH queue slot on MEC
   pipes 1-3 (pipe 0 keeps the kernel ring; pipe 1 is filled first, pipe 3 last,
   because amdgpu warns about pipes 2/3, amdgpu_gfx.c:289-294), and the log says
   whether the MEC serviced the doorbell. Boot 8 (mask 481) runs only tests that
   write no hub register: `vm: E4` (one IB fetched in VMID 8 from a scratch
   privileged VMID0 queue on pipe 1 queue 3, so it cannot wedge compute), then
   `control` (nothing changed, fresh pipe: it must fault like the baseline, or
   the fresh-slot approach itself is not working), `d` (IS_PTE bit 63 on leaf PTEs,
   as KFD's SVM PTEs on gfx12, kfd_svm.c:1375), `e` (no SNOOPED on VRAM PTEs,
   amdgpu_ttm.c:1457), `g` (EXECUTABLE on leaf PTEs) and `T` (tables written by
   the CPU through the BAR into the pool instead of by SDMA). Boot 16 (mask 30) is
   the later round with the hub-write variants `a` (TAP_*_PHYSICAL=1), `b`
   (context 0 covers the tables), `c` (MC-form table pointers) and `E2` (mirror
   LOCAL_FB/LOCAL_SYSMEM, only where they differ): run it only if boot 8 still
   fails everywhere. Each test restores what it changed. The last log line
   `vm: variants (ran/PASS): ...` is the summary. `rdna4-vm-diag` is a bit mask:
   1 E4, 2 a, 4 b, 8 c, 16 E2, 32 d, 64 e, 128 g, 256 T (481 = boot 8, 30 = boot 16).
   Read: table MISMATCH = tables did not land; E4 PASS = walker and tables fine,
   the VMID 8 HQD is the problem; a variant that PASSes names the fix; control
   passing means the baseline fault was slot-specific. The runtime stays without
   per-client VM on this boot whatever happens. `rdna4-vm-force-fail` is an
   emulator-only test hook and is ignored without `rdna4-vm-diag`; never set it
   on the card.

14. **Optional W27 clock gating (`rdna4-gfxcg=15`).**
    `rdna4-compute=7 rdna4-trace=1 rdna4-ih=1 rdna4-hang=1 rdna4-gfxpm=24 rdna4-gfxcg=15`.
    Run it after the round-4 survey boot (7) has shown which engine is busy and CGCG/CGLS are off.
    After every boot self-test the kext mirrors `gfx_v12_0_update_gfx_clock_gating` (see
    `docs/gfx-pm-audit.md`, section 9): in RLC safe mode it writes the coarse-grain (CGCG/CGLS and 3D),
    medium-grain (MGCG), fine-grain (repeater/SRAM FGCG, perf clock) settings and the CP GUI-idle
    interrupt, one bit each of the mask (1, 2, 4, 8), logging every register old -> new, a survey and SMU
    activity/power samples before and 0.3 s / 1.3 s after. The `gfxcg` summary row shows the before and
    after average clock, activity and watts; the success criterion is the power at idle, not the
    activity percentage. Clock gating persists in the RLC across warm reboots: `rdna4-gfxcg=0` writes
    amdgpu's disable branch. A hang leaves a `pm: cg ...` trail and the next boot skips only the `pm`
    feature. To try one step at a time use `rdna4-gfxcg=1`, then 3, 7, 15.
    **Known exposure (S2, accepted as part of the experiment):** with clock gating on, the runtime's
    direct GC register programming (HQD init, SQ_CMD, the MMIO dispatch in `launch()`) runs outside RLC
    safe mode, which amdgpu never does on gfx12 (it maps queues through the CP). Register access
    normally works under CGCG, but this combination is untested on the card; a hang is a power cycle.

15. **Optional W27 clock gating + GFXOFF (`rdna4-gfxoff=1`).**
    Boot 14's arguments plus `rdna4-gfxoff=1`. As the very last bring-up step the kext sends
    `AllowGfxOff`, waits 1.5 s and samples the SMU table only (no GC register access). The `gfxoff`
    row reports that sample and whether the guard later had to wake GFX. **Guard:** every GC register
    access (`rd`/`wr` with the GC hardware id) and every doorbell write first goes through `GcAccess`, which
    sends `DisallowGfxOff` before touching the block if GFXOFF is allowed, and waits for in-flight
    accesses before an Allow; the wake is sticky until the next boot (the first `rdna4-run selftest`/`bench`
    wakes GFX for good; `rdna4-run info` and `sensors` are SMU-only and do not, so `sensors-idle` is the
    GFXOFF reading). If a wake ever fails (`WAKE FAILED` in the log, `gfxoff` FAIL) stop testing this
    boot and power-cycle. Highest risk of the set: a GC access to a powered-off block can hang the bus,
    which is why the guard sits in the register accessors themselves. Details in `docs/gfx-pm-audit.md`
    section 9. **Across boots:** the kext writes an NVRAM flag before `AllowGfxOff`, clears it after a
    successful `DisallowGfxOff` (any GC access, or the shutdown quiesce) and, when it finds the flag set at
    the next start, sends `DisallowGfxOff` before its first GC read; if the SMU does not answer, that boot
    drops every GC access and skips the bring-up (log: `gfxoff: ... dropped`). After ANY abnormal end of a
    boot-15 boot (crash, hard reset, power button) do a cold power cycle before the next boot: the flag
    logic covers the normal cases, not a card that stays in a state the SMU cannot answer for.

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

