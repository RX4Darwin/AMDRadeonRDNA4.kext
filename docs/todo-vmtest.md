# Card tests for the runtime's client address spaces

Written 2026-10-06 on `devel/address-space`. **Passed on the card 2026-10-07** (eighth run, below): with
`rdna4-vm=1` every row of the runtime's self-test passes, the two fault tests included, and the GPU idles at 3 %.
What it took is in the runs below and in `docs/vm-client-rootcause.md` sections 11 to 19. Still owed: boot B (the
control with `rdna4-vmshared=0`).

## First run, 2026-10-07

Kext `52493F80`, boot C's arguments (`rdna4-compute=7 rdna4-vm=1 rdna4-gfx=2 rdna4-gfxclient=1 rdna4-hang=1
rdna4-trace=1`), log `rdna4fb-diag-20261007-130428`.

- **Passed**: both shared queues active; the boot test (`address space 8 from shared queue 0 ...: PASS`, about 1 ms a
  job); the graphics-ring client self-test (`gfx-client PASS`); a real compute kernel in a client's address space
  (`ok zero-copy vadd: 65536 items`).
- **The GPU is not pinned** with `rdna4-vm=1`: 3 % and 18 W at idle, where the September boots read 100 % and 75 W.
- **Failed**: the isolation test and the freed-buffer test, which make a shader touch an address that is not mapped
  and expect the job to finish anyway. On the card the job never finished, the queue could not be recovered, and
  every test after that failed with it. That is why the table shows `runtime`, `submitib`, `fault`, `vm` and
  `gfx-app-tri` as FAIL.
- Only this one log arrived. Boot B's (the control with `rdna4-vmshared=0`) is not among the files.

## Second run, 2026-10-07

Kext `6F7D19C7`, the same arguments, log `rdna4fb-diag-20261007-131756`. The fault page is in system memory as Linux
has it (`GCVM_L2_CNTL 0x00080601 -> 0x00080e01`), everything that passed before passes again, and **the two fault
tests hang exactly as before**. So that was not it.

## Third run, 2026-10-07

Kext `8CAB1DCC`, log `rdna4fb-diag-20261007-132834`. The kext saw each fault within 6 ms and mapped the dummy page
at the address (`a shader touched 0x100030000, which is not mapped ...`), and the job still never finished: nothing
tried the access again. As the address space was set up (the way Linux sets it up on this card), a fault is final.

## Fourth run, 2026-10-07: a wrong bit

Kext `2CA51B3E`, log `rdna4fb-diag-20261007-133727`. The bit that kext set was not the retry bit on this chip but
part of the page-table block size, so the card could not read the client's tables at all: the boot test failed, the
address spaces were switched off for the boot, and the GPU sat at 100 %. With them off, the runtime's own self-test
and benchmarks passed. The bit is corrected; the run below is the one that was meant.

## Fifth run, 2026-10-07

Kext `E71A9E01`, log `rdna4fb-diag-20261007-134456`. With the right bit the earlier results are back (boot test
PASS, `gfx-client PASS`, idle 3 %), and the two fault tests are exactly as in the third run. So a shader fault ends
its job on this card whatever is done about the page, as it does under Linux. The question was the wrong one: not
how the job can finish, but how the queue is got back.

## Sixth run, 2026-10-07

Kext `61554AD5` and the new `rdna4-run`, log `rdna4fb-diag-20261007-135449`. **Both fault tests pass**: the fault is
caught in under 3 ms and the client is told its job faulted. The queue's reset goes through cleanly (`reset (RLC safe
mode acknowledged): inactive`) and the fresh queue still runs nothing, so everything after the first fault on each
queue fails as before.

## Seventh run, 2026-10-07

Kext `F15C3784`, log `rdna4fb-diag-20261007-140244`. Both fault tests pass again. The pipe restart ran and changed
nothing (`instruction pointer 0x5044 -> 0x5044`, `NOT recovered`), and is taken out. The new dumps show where the
trouble is: before the reset the queue is in order and only the shader's waves hang; the queue made afresh after the
reset has its doorbell switched off and answers nothing (`docs/vm-client-rootcause.md` section 18).

## Eighth run, 2026-10-07: passed

Kext `8C8B6470`, log `rdna4fb-diag-20261007-141737`. After each fault: `waves reset, the queue left as it was (RLC
safe mode acknowledged): it runs`, `recovered (WRITE_DATA proof landed)`, 1.1 ms after recovery began. Every test of
the self-test `ok`; the table has `runtime`, `submitib`, `fault`, `vm`, `gfx-client`, `gfx-app-tri` and
`gfx-app-tricol` at PASS, `idle-pin` and `post-idle` at 3 % and 18 W.

The kext built after this run (`39B99DC7`) only drops the full reset that did not run here. It ran the same boot
right after (log `rdna4fb-diag-20261007-142500`) and the log reads the same: both recoveries `it runs`, all rows PASS.

What changed: with `rdna4-vm=1` a client of the compute runtime (`RDNA4ComputeClient`, `rdna4-run`) gets its own
GPU address space. Until now each such client also got a compute queue of its own inside that address space, and
on the card no such queue was ever serviced. Now a client's work runs on two shared kernel queues that stay in
address space 0, the address space named in each command-buffer packet, which is the route the Vulkan interface
was verified with on 2026-10-06 (graphics ring and copy engine). The boot's own proof of the address space goes
the same way. `rdna4-vmshared=0` gives the old path back.

What is not known: whether a kernel **compute** queue honours the address space in the packet on this card under
this kext. The graphics ring and the copy engine do. That is what boot A shows.

Rig as for the Vulkan tests: RX 9070 XT, Big Sur 11.6.6, the firmware in the build (`make clean && make` after
`tools/fetch-firmware.sh`). For every boot:

- Take `rdna4-cursor`, `rdna4-vbl`, `rdna4-dmubhist` and `rdna4-dmubver` out of the boot-args: their lines push the
  bring-up's out of the kernel log.
- Put `build/rdna4-run` **beside** `diagnostic-log.sh` (in the earlier runs it was missing, and the script then
  skips every runtime test). The script runs the tests itself; nothing else has to be typed.
- Run the script within the first minutes after login, and reboot between boots A and B.
- The Vulkan programs do not work in these boots: the Vulkan interface refuses to open with `rdna4-vm=1` (both
  would use address space 8).

## Boot A: the new default

```
rdna4-compute=7 rdna4-vm=1 rdna4-hang=1 rdna4-vmid-test=6 rdna4-trace=1
```

(`rdna4-vmid-test=6` adds register surveys at fixed points of the boot and a trace of each client operation; it
does **not** run the queue probes, which would build queues inside address spaces. `rdna4-hang=1` lets a shared
queue recover from a job that does not finish.)

```bash
sudo bash diagnostic-log.sh
```

Report:

1. Does the desktop come up and stay usable, both displays?
2. The log.

What a pass looks like, in the feature table at the end of the log:

- `vm PASS isolation + concurrent queues`
- `runtime PASS`, `submitib PASS`, `fault PASS`
- `idle-pin PASS` with a few percent and some tens of watts (the failing boots read 100 % and about 75 W before
  any client existed)
- `vmshared INFO shared mode 1 (the default with rdna4-vm=1) ...`

Lines to find, in order:

- `vmshared: shared queue 0: MEC1 pipe 0 queue 2, VMID 0, doorbell dword 74: active`, and the same for queue 1
  (pipe 1, doorbell dword 76)
- `vmshared: rdna4-vmshared=1: client jobs run on 2 shared VMID-0 queues ...`
- `runtime: vmid 8: shared-queue client (rdna4-vmshared) on shared queue 0 ...`
- `vmshared: boot test: address space 8 from shared queue 0 (MEC1 pipe 0 queue 2, VMID 0): job 1 submit 0x0 fence
  reached data ok (N us), job 2 submit 0x0 fence reached data ok (N us): PASS`
- in the registry results: `"vm"="PASS shared queue: two command buffers in address space 8 from a VMID-0 kernel
  queue, written through the client's tables (N / N us)"`
- later, from the script's own tests: more `shared-queue client` opens for address spaces 8 and 9, and no
  `dequeue timeout` line anywhere

What failure looks like:

- `shared queue N: ... FAILED to activate`: the kernel queue itself did not come up; below the address space.
- `boot test: ... job 1 ... fence NOT reached`: the shared queue did not get through a command buffer in address
  space 8. The lines after it give the queue's state and any fault the card recorded, then the recovery. The
  address spaces are then off for the boot (`vm: boot self-test failed`) and the client tests run without them.
  This is the result that would say a kernel compute queue does not take the address space from the packet here.
- `fence reached data WRONG`: the job ran and wrote elsewhere, or nothing.
- The boot test passes and `runtime` or `submitib` fails: the route works and something in the client path does
  not; the `vm-trace` rows and the `runtime:` lines name the first operation that failed.
- `idle-pin FAIL` although no queue was built inside an address space: the pinned GPU has another cause than the
  one suspected; the `vm-survey` rows say from which point of the boot on.

## Boot B: the old path, as a control

Only worth running if boot A passed. It is expected to fail, and that failure on the same rig and the same day
is what turns "the new path works" into "and this is why the old one did not".

```
rdna4-compute=7 rdna4-vm=1 rdna4-vmshared=0 rdna4-hang=1 rdna4-vmid-test=6 rdna4-trace=1
```

```bash
sudo bash diagnostic-log.sh
```

Report the log. Expected, from the September rounds: the old boot self-test may pass, `idle-pin FAIL` with the
GPU near 100 %, `runtime FAIL` with `dequeue timeout` lines for every client. The display is not affected; reboot
afterwards. If boot B **passes** on this rig, the old path's failure belongs to something else in the September
boots (they also carried `rdna4-vm-diag=4065`), and both paths stay.

## Boot C: a runtime client's graphics work (after A)

The runtime's own graphics submission (`SubmitGfxIb`) has only run in the emulator. With boot A's result it has
a working address space under it.

```
rdna4-compute=7 rdna4-vm=1 rdna4-gfx=2 rdna4-gfxclient=1 rdna4-hang=1 rdna4-trace=1
```

```bash
sudo bash diagnostic-log.sh
```

Rows to look at: `gfx PASS`, `gfx-client PASS` (a kernel-made client's command buffer on the graphics ring in its
address space), `gfx-app-tri` and `gfx-app-tricol` (`rdna4-run tri` / `tricol`: a triangle drawn by a user
program through the runtime). The colour triangle has not been confirmed on the card by any path.

## After the tests

`docs/vm-client-rootcause.md` section 11, `docs/metal-readiness.md` section 4 and `docs/w13-vmid.md` get the
results. If A passes and B fails, the old path and its boot test can be removed.
