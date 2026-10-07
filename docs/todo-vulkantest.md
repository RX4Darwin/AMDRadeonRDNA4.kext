# Card tests for the Vulkan interface

Written 2026-10-06. **All four boots passed on the card the same day** (below): the memory half, the driver's
first work, a rendered triangle read back, and a picture on the display. Left to run: two more runs of boot 4 at other display settings, near the end. Otherwise this is the record and
the way to repeat the tests.

Rig as before: RX 9070 XT (revision 0xC0), Big Sur 11.6.6. Both displays come up next to the compute bring-up
(verified in boot 1). Do not let the machine sleep during these boots: the compute runtime does not survive sleep
without `rdna4-pm=1`, which is not part of this.

**Keep the kernel log small.** `rdna4-cursor=1` and `rdna4-vbl=1` write several lines per pointer movement with
`rdna4-trace=1`, and the kernel log then loses the bring-up's lines within minutes (first try, below). Either take
them out for these boots, or run the programs and the diagnostic script in the first two or three minutes after
login, as in the second try.

## Boot 4, 2026-10-06: passed

Kext `4AB6CFC5-...`, the same boot-args, both displays lit, the Samsung at 2560x1440; `vkprobe-4.txt`, log
`rdna4fb-diag-20261006-223154`, and the triangle seen on the screen.

- `display: 2560x1440, pitch 3840 pixels, pipe 0`, then **`show: 301 frames in 5.01 s (60.0 a second), the
  display counted 300; the desktop (0x8000000000) given back: yes: ok`**. Every frame made its vertical blank.
- The kernel log has the display taken (`OTG0 HUBP0, 2560x1440 pitch 3840`), no `flip: failure`, and the release:
  the flip back to the desktop latched in 16.6 ms, one frame.
- It ran on a pipe the plugin's own mode setting had programmed (2560x1440 is not the firmware's mode), with a
  pitch wider than the picture: both were open questions.
- Two earlier runs that day did the usual checks only: the `show` argument was missing. `vkprobe` now says so.
- Not tried: 3840x2160, a refresh rate other than 60 Hz, a mode switch while the picture is up.

## Boot 3, 2026-10-06: passed

Kext `D86B7F8C-...`, the same boot-args, both displays lit; `vkprobe-2.txt` and log
`rdna4fb-diag-20261006-214537`.

- Both fills right again, then **`triangle: 2016 red, 2080 blue, 0 other of 4096 pixels; (8,8) 0xff0000ff,
  (56,56) 0xffff0000: ok`**: exactly the pixel counts the rasterisation rules give for that triangle. A vertex and
  a fragment shader the driver compiled, a render target cleared and drawn into, and the picture copied back
  through a buffer. `done: all ok`.
- The kernel log has the driver's two connections (one to look at the card, one for the device), the second open
  for 68 ms from first call to close, no `vulkan: work on the graphics queue did not finish`, and no fault beyond
  the known one from the ring's own bring-up.

## Boot 2, 2026-10-06: passed

The same boot as boot 1's second try (kext `F8A4B018-...`, `rdna4-compute=7 rdna4-gfx=2`), `vkprobe` with the
driver built on the development Mac; `vkprobe.txt`, no diagnostic log was taken after it.

- The driver found the kext's interface (`ABI 1.9, build 1, VMID 8`, 96 MiB of VRAM behind the BAR and 7896 MiB
  past it), offered `AMD Radeon RX 9070 XT (RADV GFX1201)`, Vulkan 1.4.363, and created its device.
- **Both fills came out right**: 1024 bytes by the command processor's own copy, 61440 bytes by a compute
  shader the driver compiled, the bytes between them untouched. Each was a submission through this kext onto the
  graphics ring, in address space 8, waited for through `WaitSeq`. `done: all ok`.
- First run of the driver and of `vkprobe` under macOS 11.

## Boot 1, 2026-10-06: passed (second try)

Kext `F8A4B018-...`, firmware in the build, boot-args `rdna4-compute=7 rdna4-gfx=2` next to the usual ones, both
displays lit. Log `rdna4fb-diag-20261006-210519`.

- The bring-up finished at stage 7 under Big Sur, 5 s after the second display was lit: firmware loaded through
  the PSP, the copy engine up (`dma: on`, 13 GB/s both ways), 7896 MiB of VRAM past the BAR for buffers, default
  clock gating applied (298 W to 19 W by the card's own sensors).
- The graphics ring came up and **the kext's own test triangle was right** (`gfx PASS ring test, THE TRIANGLE IS
  RIGHT, draw 8192 px`): until now that had only been shown under Linux.
- `n48nprobe`: **all ok**. A buffer in VRAM behind the BAR, one past it and one in system memory, mapped at
  0x100000000, 0x7ffe00000000 and 0xffff800000200000; the GPU's copy engine, working in address space 8, carried
  the pattern through all three and back; the kernel log has the three `vulkan: copy test:` lines and the open
  and close around them.
- Not a failure, though the table says `runtime FAIL`: `rdna4-run` was not beside the diagnostic script, so the
  runtime's own self-test did not run. The ring's bring-up logs a latched fault at address 0 after its ring test
  (`GC hub fault status 0x00000d3d`); that one is known and older than this work (`docs/gfx-context-audit.md`).

## First try, 2026-10-06: the kext had no firmware in it

`n48nprobe` found no service. The kext that was loaded (UUID `0E80B175-...`) was the one built before the
firmware was fetched: the NVRAM trail said `stopped: stage 2 (psp) failed (no hang)` and `Compute,Stage` was 1.
The kernel log began at 324 s, after the bring-up's own lines. A kext without firmware now also says so in the
registry, and the diagnostic log prints it as a line starting with `FIRMWARE:`.

What is new on each boot, so a failure can be placed:

| Layer | Whose | Has it run on a card |
|---|---|---|
| Compute bring-up: firmware, copy engine, compute queue (`rdna4-compute=7`) | m1guer's | yes, in macOS Recovery in late September (`docs/hw-logs/`); not in any boot since 2026-10-03, not under Big Sur as far as the logs show |
| The Vulkan interface's memory half, and the copy test | this work | no |
| The graphics ring (`rdna4-gfx=2`) | m1guer's | yes, in the kernel's own address space |
| A client's command buffers on that ring, in its own address space | this work, on m1guer's ring code | no |

If something hangs: reboot. Nothing here is persistent except the bring-up's own breadcrumb in NVRAM, which makes
the **next** boot skip the step the last one died in and say so (`compute: the previous boot died during ...`).
If the machine does not get to the desktop at all, take `rdna4-compute` out of the boot-args.

## Before the first boot (on the development Mac)

1. The card's revision. The firmware the build embeds is for revision `0xC0`; a `0xC8` card needs other files,
   which `tools/fetch-firmware.sh` does not fetch. On the rig:

   ```bash
   system_profiler SPDisplaysDataType | grep -i revision
   ```

   Anything but `0x00c0`: stop and say so.

2. The firmware into the build. This downloads 13 files and AMD's licence text from the linux-firmware repository
   into `firmware/amdgpu/`, which git ignores:

   ```bash
   tools/fetch-firmware.sh
   ```

3. Build everything again; a kext built before the firmware was there has none in it.

   ```bash
   make clean && make && make n48nprobe
   ```

   Check that the firmware is in: this must print a number above 0.

   ```bash
   nm build/RDNA4FB.kext/Contents/MacOS/RDNA4FB | grep -c rdna4_fw_
   ```

   And note the kext's UUID, to compare with the `kext loaded?` line of the log after the boot:

   ```bash
   dwarfdump --uuid build/RDNA4FB.kext/Contents/MacOS/RDNA4FB
   ```

4. To the rig: `build/RDNA4FB.kext` (into the EFI as usual), and into one folder `build/n48nprobe`,
   `build/rdna4-run` and `tools/diagnostic-log.sh`. (With `rdna4-run` beside it the diagnostic script also runs the
   runtime's own self-test, a compute dispatch; that is m1guer's and useful to have in the same log.)

## Boot 1: the bring-up, then the memory half

```
rdna4-compute=7 rdna4-trace=1
```

The bring-up starts about 5 s after the desktop and takes some seconds. Wait a minute after login, then, in the
folder with the files:

```bash
sudo ./n48nprobe
```

```bash
sudo bash diagnostic-log.sh
```

Report:

1. Does the desktop come up on the Samsung as usual, and stay usable?
2. The whole output of `n48nprobe`.
3. The log.

`n48nprobe` ends in `all ok` when everything worked: three buffers (VRAM the CPU reaches, VRAM past the BAR,
system memory) mapped at three far-apart GPU addresses, and a pattern the GPU's copy engine carried from the first
to the second to the third and back, in the client's address space.

Lines to find in the log, in order:

- `kext loaded?` and `active:` near the top: the UUID of the kext you built, and both boot-args. No line starting
  with `FIRMWARE:` further down.
- `compute: bring-up to stage 7 requested`, then `compute: bring-up finished at stage 7`.
- `runtime: dma: on` and `runtime: dma: buffers from VRAM+0x...` (the copy engine and the VRAM past the BAR), then
  `runtime: user-space runtime up: RDNA4ComputeService, DMA transfers, ...`.
- `vulkan: client open: address space 8, table root at physical 0x...; N MiB of VRAM behind the BAR, N MiB past it`
- three times `vulkan: copy test: 0x... -> 0x..., 262144 bytes, in address space 8`
- `vulkan: client closed: ...`, and a second open and close (the probe opens the interface once more at the end).

What failure looks like, from the bottom layer up:

- `FIRMWARE: FAIL none in this kext`, or `compute: psp: this build carries no firmware`: step 2 or 3 above was
  skipped, or the old kext is still in the EFI.
- `compute: ... failed; stopping`, or no `bring-up finished at stage 7`: the bring-up itself did not get through.
  That is below everything of this work; the `compute:` lines and the NVRAM trail in the log say where.
- `n48nprobe` prints `open: 0x...` with a reason: no service (the bring-up did not finish), needs root, or not
  ready (`runtime: dma: off` in the log: the interface needs the copy engine; or `rdna4-vmshared=2` is set).
- A `FAILED` line before `the CPU reads back what it wrote to VRAM`: buffers, mappings or the mapping into the
  process; the probe then stops before the GPU is asked for anything.
- `GPU copy: ... FAILED` with `0xe00002ed` and, in the log, `runtime: dma: SDMA fence N never came` and
  `vulkan: copy test: the copy engine did not finish ...`: the copy engine could not do the copy in the client's
  address space. The log then also has the fault the card recorded. **The copy engine stays stopped until the next
  boot**; the display is not affected. This is the result that would say the address space is not right.
- A copy that answers ok but `holds the pattern ... FAILED`, with the first wrong dword printed: the copy ran and
  went somewhere else, or read something else.

## Boot 2: work on the graphics ring

Only if boot 1 ended in `all ok`.

The driver and its test program, on the development Mac (the first time this downloads Mesa, about 150 MB, into
`build/radv-build`, and needs clang, ninja, bison, flex, pkg-config and glslangValidator; `make clean` removes all
of it again):

```bash
make mesa
```

To the rig, next to the other files: `build/vkprobe` and `build/libvulkan_radeon.dylib` (27 MB).

```
rdna4-compute=7 rdna4-gfx=2 rdna4-trace=1
```

Boot 1 already ran with these boot-args and the ring came up, so this is the same boot again. In the first two
or three minutes after login:

```bash
sudo ./n48nprobe
```

```bash
sudo ./vkprobe ./libvulkan_radeon.dylib 2>&1 | tee vkprobe.txt
```

```bash
sudo bash diagnostic-log.sh
```

Report:

1. Does the desktop come up and stay usable?
2. The whole output of `n48nprobe` (it should be as in boot 1).
3. The whole output of `vkprobe`.
4. The log.

`vkprobe` ends in `done: all ok` when the GPU did two pieces of work the driver submitted: a fill of 1024 bytes,
which the driver has the command processor do by itself, and a fill of 60 KiB, which it does with a compute
shader. The first is the smaller question (does the ring run a command buffer in the client's address space at
all), the second adds a shader the driver compiled.

Lines to find in the log: the `gfx:` lines of the ring's own bring-up (its ring test, then the kext's own test
triangle) and the `gfx` row of the feature table, `vulkan: client open: ...` twice (the driver opens the interface once to look at the card and once
for the device), and no `vulkan: work on the graphics queue did not finish`.

What failure looks like:

- The `gfx` row of the feature table. `PASS` means the ring came up and the kext's own triangle was right. `FAIL
  gfx ring passed but the draw result was not proven` is about that triangle (never confirmed on the card) and
  does not by itself stop a client; a failure of the ring, or a test draw that never finished, is below this work
  and leaves nothing for `vkprobe` to run on.
- `vkCreateDevice` or an earlier line with a negative number, and a `radv-darwin:` message: the driver and the
  interface disagree before any work is submitted; the message says about what.
- `vkQueueSubmit ... -> -N` at once: the submission was refused. With `RADV_DARWIN_TRACE=1` in front of the
  command the driver prints the call and the answer (`0xe00002c7` = no graphics ring this boot, `0xe00002d8` = the ring is not ready, for
  instance still in its bring-up, `0xe00002c2` = a malformed submission).
- `vkWaitForFences` returns only after about ten seconds, or a later call fails, and the log has
  `vulkan: work on the graphics queue did not finish in 10 s` followed by `gfx: vulkan client: a client gfx IB did
  not finish ...`: the ring did not get through the command buffers. **The ring is halted until the next boot**;
  the display and the compute queues are not affected. Which of the two fills it was says a lot: the first means
  the ring cannot run a client's command buffer at all as it is set up here (the firmware scheduler is the first
  suspect, `docs/vulkan-port.md` section 9), the second that it can and the shader or its memory is the problem.
- `fill by ...: NOT FILLED`, with the first wrong dword: the work was reported finished and did not do what it
  should.

## Boot 3: a picture rendered and read back

`vkprobe` now also draws: a 64x64 image cleared to blue, a red triangle over its upper-left half, drawn with a
vertex and a fragment shader the driver compiles, then copied into the buffer and counted. This is the first use
of the graphics pipeline proper: the geometry stage, the rasteriser, a render target. On a Mac it gets as far as
it can without a GPU (every call succeeds, the submission is accepted by the engine).

Same boot-args as boots 1 and 2. To the rig, from the development Mac: the new `build/vkprobe` (the library is
unchanged). In the first two or three minutes after login:

```bash
sudo ./vkprobe ./libvulkan_radeon.dylib 2>&1 | tee vkprobe.txt
```

```bash
sudo bash diagnostic-log.sh
```

Report `vkprobe.txt` and the log.

The new line reads `triangle: 2016 red, 2080 blue, 0 other of 4096 pixels; (8,8) 0xff0000ff, (56,56) 0xffff0000:
ok` when it is right (the red count may differ by the diagonal's pixels), and the run ends in `done: all ok`.

What failure looks like:

- The two fills fail where they passed before: something else changed; send the log.
- `vkCreateGraphicsPipelines ... -> -N`: the driver could not build the pipeline; nothing reached the card.
- `vkWaitForFences` takes about ten seconds and the log has `vulkan: work on the graphics queue did not finish
  in 10 s`: the ring did not get through the draw. **The ring is halted until the next boot.** The `gfx:` and
  fault lines of the log say what the card recorded.
- `triangle: ... NOT AS DRAWN`: the counts say which part is wrong. All pixels `0x11111111`: the copy back did
  not happen. All blue: the clear ran and the triangle did not appear. Other colours: the picture or the copy is
  garbled; the two sample pixels help tell which.

## Boot 4: a picture on the display

`vkprobe` has a `show` mode: after its usual checks it takes the boot display, shows a red triangle sliding over a
dark blue ground for five seconds, and gives the desktop back. Each frame is drawn into an image of the display's
size, copied into one of two buffers and shown at a vertical blank (`docs/vulkan-port.md` section 10).

A new kext is needed for this one (the display calls are new), with the firmware in it as before, and the new
`build/vkprobe`; the library is unchanged. Same boot-args. Start it from a terminal **on the Lenovo, or over
SSH**: the Samsung shows the program's picture while it runs, and if the desktop should not come back you still
have a screen to work from. In the first two or three minutes after login:

```bash
sudo ./vkprobe ./libvulkan_radeon.dylib show 2>&1 | tee vkprobe.txt
```

```bash
sudo bash diagnostic-log.sh
```

Report:

1. Did the Samsung show a dark blue screen with a red triangle moving steadily to the right? Smooth, or
   stuttering or tearing?
2. Did the desktop come back by itself after about five seconds, looking as before?
3. Was the mouse pointer visible over the picture? (Expected with `rdna4-cursor=1`.)
4. `vkprobe.txt` and the log.

The new lines read `display: 3840x2160, pitch 3840 pixels, pipe 0, showing 0x...` and `show: N frames in 5.00 s
(F a second), the display counted M; the desktop (0x...) given back: yes: ok`. `F` near the display's refresh
rate means every frame made its vertical blank; half of it, that a frame takes longer than one refresh.

Lines to find in the log: `vulkan: display taken: OTG0 HUBP0, 3840x2160 pitch 3840, the desktop at 0x...` and
`vulkan: display given back: the plane shows 0x...`, and no `flip: failure:` line between them.

What failure looks like, and what to do:

- The program stops with `scanAcquire ... -> -N` or `scanRegister ... -> -N`: the display was not taken or a
  buffer was refused; nothing changed on the screen.
- `scanPresent ... -> -N` and `flip: failure: vulkan present ...` in the log: the flip did not confirm. The
  program gives the display back and ends; the `flip:` line says which step.
- The picture is wrong (colours, a shifted or torn image, garbage): describe it, or photograph it; the readback
  test just before it passed in the same run, so the difference is in the copy or the display's read of it.
- **The desktop does not come back** though the program has ended. The log's `vulkan: display given back` line
  says whether the kext believes it did. A reboot restores it (a resolution switch does not: it leaves the
  plane's address alone). Take the log first, from the Lenovo or over SSH.

## Two more runs of boot 4, no new code

What boot 4 did not cover, each one run of the same command with the Samsung set differently first:

- **3840x2160** (the "1920x1080" HiDPI setting): each frame is then a 33 MiB copy. Does `show:` still say 60 a
  second, and is the motion smooth?
- **2560x1440 at 120 Hz** (`/usr/bin/python tools/cgmode.py` lists the modes): does `show:` say 120 a second?

```bash
sudo ./vkprobe ./libvulkan_radeon.dylib show 2>&1 | tee vkprobe.txt
```

## With `rdna4-vm=1`: the Vulkan interface and the runtime's clients in one boot

Written 2026-10-07. Until now the interface refused to open with `rdna4-vm=1`. It now takes the lowest address
space of 8 to 15 that no client of the runtime holds. New kext (`make`), new `build/n48nprobe` (`make n48nprobe`; only
a message changed), the `vkprobe` and library of boot 4, and `build/rdna4-run` beside `diagnostic-log.sh`.

```
rdna4-compute=7 rdna4-vm=1 rdna4-gfx=2 rdna4-gfxclient=1 rdna4-trace=1
```

**Step 1, one after the other.**

```bash
sudo ./n48nprobe > n48nprobe.txt 2>&1
```

```bash
sudo ./vkprobe ./libvulkan_radeon.dylib show > vkprobe.txt 2>&1
```

```bash
sudo bash diagnostic-log.sh
```

Expected: `n48nprobe` prints `address space 8` and `all ok`; `vkprobe` as in boot 4 (fills, the triangle, the moving
triangle for 5 s, the desktop back); the script's table as in the runs of `docs/todo-vmtest.md` (all rows PASS).

**Step 2, at the same time.** Only if step 1 passed. The Vulkan program holds address space 8 and the boot display
for two minutes while the runtime's tests run beside it, in address spaces 9 and up, on the same graphics ring. The
triangle covers the Samsung, so both terminal windows go on the Lenovo (or use ssh). In the first:

```bash
sudo ./vkprobe ./libvulkan_radeon.dylib show 120 > vkprobe2.txt 2>&1
```

and at once, in the second:

```bash
sudo bash diagnostic-log.sh
```

Expected: the triangle keeps moving, `vkprobe2.txt` ends as in step 1, and the table is all PASS, with `runtime: vmid
9: shared-queue client` (not 8) in the kernel log. If the triangle stops or the script hangs, wait for both to end
(the Vulkan side gives up after 10 s without progress) and send the log anyway.

Report: the three text files and the two logs.

### Run 2026-10-07 (kext `D6755449`): the Vulkan side passes, the runtime's two fault tests time out

Logs `rdna4fb-diag-20261007-150805` (step 1) and `-151257` (step 2), `n48nprobe-2.txt`, `vkprobe-5.txt`, `vkprobe2.txt`.

- **Vulkan with `rdna4-vm=1`: passes.** `n48nprobe` all ok in address space 8; `vkprobe` fills, triangle, 300 frames in
  5.00 s, desktop back.
- **At the same time: works.** `vkprobe` showed 6957 frames in 120 s while the script ran; the runtime's clients got
  address spaces 9 and 10 (`runtime: vmid 9: shared-queue client`, `GPUVM: VMID 9`); 24 of the self-test's lines `ok`,
  `gfx-app-tri` and `gfx-app-tricol` PASS on the ring the Vulkan program was drawing on.
- **Failed in both steps:** `VM isolation dispatch: I/O Timeout`, and in step 2 also `dispatch through freed host VA
  timed out`; so `runtime`, `submitib`, `fault` and `vm` FAIL in the table. The queue came back each time (`waves
  reset ...: it runs`), after the 2 s timeout instead of after 3 ms.
- Cause: the hub keeps the first fault it latched until it is cleared. Every submission on the graphics ring
  leaves one there (`GC hub fault status 0x00000d3d (fault VMID 0)`, the known stray fetch at address 0), the Vulkan
  program one per frame, and the client's fault behind it is not recorded. In the earlier passing runs nothing had
  used the graphics ring between the boot's own clearing and the self-test.
- The 237 frames `vkprobe2` is short of the display's count are the two timeouts: 2 s each with the runtime's lock held.

**The fix** (kext `56AEEF41`, compile-checked): the fault check clears an entry that names address space 0, runs on
every poll of a wait and once before a job is kicked; and the Vulkan client's close clears an entry of its own.


### Second run 2026-10-07 (kext `56AEEF41`): the faults are seen at once; three tests after them are wrongly "faulted"

Logs `rdna4fb-diag-20261007-152847` (step 1) and `-152932` (step 2), `n48nprobe-2.txt`, `vkprobe-6.txt`, `vkprobe2-2.txt`.

- Vulkan as before: all ok, 300 frames in 5.01 s; beside the script 7137 frames in 120 s against the display's 7194.
  The two-second stalls are gone.
- **Both fault tests pass in both steps**, with the Vulkan program presenting in step 2: `dispatch ended by a fault in
  address space 10 after 46 us`, `... 9 after 38 us`. (The 3 ms of the earlier runs was the check only starting
  after 2 ms.)
- **New failure, from the fix:** `SubmitIb vadd/wait: misc. VM failure` (step 1: all three `SubmitIb` tests). These
  jobs did not fault. The hung waves of the fault test before them fault again as soon as the hub's entry is
  cleared, which the recovery's state dump does before it resets them; that second entry stayed, and the next job
  in the same address space was ended by it at its first look (`after 8 us`). Before the fix the check started
  after 2 ms, by when such a job had long finished, so the leftover never showed.

**The fix** (kext `7248396D`, compile-checked): the recovery clears the faulted address space's entry after the waves
are reset.

### Third run 2026-10-07 (kext `7248396D`): step 1 passes; in step 2 one wait ran out

Logs `rdna4fb-diag-20261007-153754` (step 1) and `-153835` (step 2), `n48nprobe-2.txt`, `vkprobe-7.txt`, `vkprobe2-3.txt`.

- **Step 1: everything passes.** `n48nprobe` all ok, `vkprobe` 301 frames in 5.01 s, the script's table all PASS (27
  `ok` lines, both faults seen after 31 and 76 us, idle 3 % and 18 W).
- **Step 2:** both fault tests pass beside the Vulkan program (166 and 39 us), 26 `ok` lines, the graphics rows PASS.
  One failure: `SubmitIb vadd/wait: I/O Timeout`. That job was submitted 30 ms after the second fault's recovery and
  its 5 s wait ran out; the queue was fine (`it runs`) and the same job passes three and ten times right after.
  `vkprobe` lost 358 frames to that wait (6837 of 7195).
- Cause **not established**: the state dump's lines for that timeout did not survive in the kernel log. Suspected:
  the faulted job's own fence. The command processor had stopped in front of it (read pointer 0x3d1, write pointer
  0x3d9), so it is written only after the waves are reset, late; it shares its word and numbering with the client's
  next job, and landing after that job's fence it takes the word back to 3 while the wait wants 4.

**The change** (kext `A4A96CB4`): the recovery sends a fence of its own through the queue after the proof packet and
waits for it, and a wait that runs out says in its one line what it last saw.

### Fourth run 2026-10-07 (kext `A4A96CB4`): the suspicion was wrong; one reset was not enough

Logs `rdna4fb-diag-20261007-155046` (step 1) and `-155129` (step 2), `n48nprobe-2.txt`, `vkprobe-5.txt`, `vkprobe2-4.txt`.

- **Step 1: everything passes again** (27 `ok` lines, all rows PASS, both recoveries `it runs, its fences are through`).
- **Step 2: the same one failure**, `SubmitIb vadd/wait: I/O Timeout`, and now with its reason in the log:

  ```
  runtime: shared queue 0: waves reset ...: it runs, a fence did NOT come through in 200 ms
  runtime: IB fence 4 timed out (fence word 0x0, hub fault 0x00000000, GRBM 0xa840382c); ...
  IB: shared queue 0: waves reset ...: it runs, its fences are through
  ```

  The fence word is 0, not 3: no late fence took it back, so **the suspicion of the third run is refuted**. What the
  lines say instead: after the freed-buffer fault's reset the queue runs packets but no fence comes through, and
  `GRBM_STATUS` still has SPI busy five seconds later. Waves were still there. The second reset, done by the timed-out
  wait's recovery, removed them. It is the reset after the second fault, on queue 0, with the Vulkan program
  presenting, in both runs that have it; why one reset is not enough there is not known.

**The change** (kext `DCD5BF79`, compile-checked): the recovery repeats the reset, up to four times, until a fence of
its own comes through, and only then counts the queue as recovered (`waves reset N time(s) ...: it runs, its fences
are through`).

### Fifth run 2026-10-07 (kext `DCD5BF79`): both steps pass

Logs `rdna4fb-diag-20261007-160112` (step 1) and `-160156` (step 2), `n48nprobe-2.txt`, `vkprobe-5.txt`, `vkprobe2-5.txt`.

- **Step 1:** `n48nprobe` all ok, `vkprobe` 300 frames in 5.00 s, the table all PASS, 27 `ok` lines.
- **Step 2:** the table all PASS, 27 `ok` lines, with the Vulkan program presenting in address space 8 and the
  runtime's clients in 9 and 10; both faults seen (85 and 166 us) and both queues recovered; `vkprobe` 7136 frames
  in 120 s against the display's 7194 (the difference is the script's own short holds of the lock).
- **Not exercised:** all four recoveries read `waves reset 1 time(s)`. The case the repeat was written for (one reset
  not enough, third and fourth run) did not come up, so the repeat itself has not run on the card.

### Step 2 twice more in the same boot: the repeat ran, and worked

Logs `rdna4fb-diag-20261007-160653` and `-160806`, `vkprobe2b.txt`, kext `DCD5BF79`. Both tables all PASS, 27 `ok` lines
each, `vkprobe` 7062 of 7194 frames. In the second:

```
runtime: shared queue 0: waves reset 2 time(s), the queue left as it was (RLC safe mode acknowledged): it runs, its fences are through
runtime: dispatch ended by a fault in address space 9 after 49 us; shared queue 0 recovered without a GPU reset
```

That is the case of the third and fourth run (one reset not enough, again the second fault, on queue 0), now caught
inside the recovery: the second reset follows at once, and the `SubmitIb` tests behind it pass. Seen in 3 of 6 runs of
step 2 so far, never in step 1.

**Done.** With `rdna4-vm=1` the Vulkan interface and the runtime's clients work in one boot, one after the other and
at the same time.

## A shader that faults

Written 2026-10-07. Until now a Vulkan program whose shader touched an address nothing is mapped at hung the
graphics ring: ten seconds later its work counted as lost and the ring was shut for everybody until the next boot.
Two changes, the second an **experiment**:

- The fault is looked for while the work is waited for (the hub's entry naming the client's address space). The
  client's work is then lost at once, and RADV reports the device lost. Host-tested.
- Before the ring is shut, the kext tries to get it back, leaving the queue alone as the compute recovery does:
  twice `CP_VMID_RESET` with the address space's bit and no queue, then `SPI_COMPUTE_QUEUE_RESET` with the graphics
  engine selected, each followed by 200 ms for the lost work's own fence. Nothing of this is a sequence read
  anywhere; amdgpu's own takes the queue down and has the MES map it again. If no step brings the fence, the ring
  is shut as before.

New kext (`make`) and the new `build/vkprobe` (it has a `fault` mode: a compute-shader fill into a buffer whose memory
it frees before submitting). Same library. Boot as for the last tests:

```
rdna4-compute=7 rdna4-vm=1 rdna4-gfx=2 rdna4-gfxclient=1 rdna4-trace=1
```

```bash
sudo ./vkprobe ./libvulkan_radeon.dylib fault > vkfault.txt 2>&1
```

```bash
sudo ./vkprobe ./libvulkan_radeon.dylib show > vkprobe.txt 2>&1
```

```bash
sudo bash diagnostic-log.sh
```

Report the two text files and the log, and whether anything on screen looked wrong.

What to look for:

- `vkfault.txt`, last line: `fault: submit -> 0, wait -> -4 after 0.0NN s: the device is reported lost at once: ok`.
  `THE WORK FINISHED` would mean the test did not produce a fault; `but late` or `NOT REPORTED LOST`, that the fault
  was not seen.
- Kernel log, `vulkan:` lines: `work lost in address space 8 (...)` with the state, then `step N: ...: the fence came
  through` or `did not come`, then either `the graphics ring is back and stays in service` or the old `a client gfx
  IB did not finish` message.
- If the ring is back: the second `vkprobe` ends `done: all ok` with its moving triangle, and the script's table is
  all PASS.
- If it is not: the second `vkprobe` fails at its first submission, `gfx-app-tri` and `gfx-app-tricol` FAIL, the
  compute rows still pass, and the GPU may stay busy (fans) until the reboot. The display is not affected. That
  outcome is as before this change, and the step lines say what each attempt did.

## After the tests

For what passes, the README's file table, `docs/vulkan-port.md` and `vulkan/README.md` get the date and what ran.
With the triangle right, step 3, showing a picture, has something to stand on.
