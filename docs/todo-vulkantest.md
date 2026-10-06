# Card tests for the Vulkan interface

Written 2026-10-06. Everything here is built and passes its checks on a Mac, and none of it has run on the card
(`docs/vulkan-port.md`, sections 8 and 9). Two boots, in order: the second only makes sense once the first has
passed.

Rig as before: RX 9070 XT, Big Sur 11.6.6. **One display is enough and all there will be:** with `rdna4-compute`
the plugin does not create the second display, so the Lenovo stays as the firmware left it. Do not let the machine
sleep during these boots: the compute runtime does not survive sleep without `rdna4-pm=1`, which is not part of
this.

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

- `kext loaded?` and `active:` near the top: the UUID of the kext you built, and both boot-args.
- `compute: bring-up to stage 7 requested`, then `compute: bring-up finished at stage 7`.
- `runtime: dma: on` and `runtime: dma: buffers from VRAM+0x...` (the copy engine and the VRAM past the BAR), then
  `runtime: user-space runtime up: RDNA4ComputeService, DMA transfers, ...`.
- `vulkan: client open: address space 8, table root at physical 0x...; N MiB of VRAM behind the BAR, N MiB past it`
- three times `vulkan: copy test: 0x... -> 0x..., 262144 bytes, in address space 8`
- `vulkan: client closed: ...`, and a second open and close (the probe opens the interface once more at the end).

What failure looks like, from the bottom layer up:

- `compute: psp: this build carries no firmware`: step 3 above was skipped or the old kext is still in the EFI.
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

On the development Mac first, the driver and its test program (the first command downloads Mesa, about 150 MB,
into the directory you name, and needs clang, ninja, bison, flex, pkg-config and glslangValidator):

```bash
vulkan/build-mesa.sh ~/radv-build
```

```bash
clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu11 -I ~/radv-build/mesa/include vulkan/vkprobe.c -o build/vkprobe
```

To the rig, next to the other files: `build/vkprobe` and
`~/radv-build/build/src/amd/vulkan/libvulkan_radeon.dylib` (27 MB). Neither has run under macOS 11 yet; if either
does not start, the message it prints is the result.

```
rdna4-compute=7 rdna4-gfx=2 rdna4-trace=1
```

A minute after login:

```bash
sudo ./n48nprobe
```

```bash
sudo ./vkprobe ./libvulkan_radeon.dylib
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

## After the tests

For what passes, the README's file table, `docs/vulkan-port.md` and `vulkan/README.md` get the date and what ran.
Then step 3, showing a picture, has something to stand on.
