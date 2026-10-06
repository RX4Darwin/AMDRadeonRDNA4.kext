# Card tests for the Vulkan interface

Written 2026-10-06. **Boots 1 and 2 passed on the card the same day** (below). What is still to run is the
last part of this guide: a triangle the driver renders into an image and reads back.

Rig as before: RX 9070 XT (revision 0xC0), Big Sur 11.6.6. Both displays come up next to the compute bring-up
(verified in boot 1). Do not let the machine sleep during these boots: the compute runtime does not survive sleep
without `rdna4-pm=1`, which is not part of this.

**Keep the kernel log small.** `rdna4-cursor=1` and `rdna4-vbl=1` write several lines per pointer movement with
`rdna4-trace=1`, and the kernel log then loses the bring-up's lines within minutes (first try, below). Either take
them out for these boots, or run the programs and the diagnostic script in the first two or three minutes after
login, as in the second try.

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
  ready (`runtime: dma: off` in the log: the interface needs the copy engine; or `rdna4-vm` is set).
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

## After the tests

For what passes, the README's file table, `docs/vulkan-port.md` and `vulkan/README.md` get the date and what ran.
With the triangle right, step 3, showing a picture, has something to stand on.
