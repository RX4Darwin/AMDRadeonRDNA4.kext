# tools/

Scripts and small programs for working on the driver, sorted by where they run and what they are for. Nothing
here is part of the kext. Each file starts with a comment that says how to call it.

What is built and shipped lives elsewhere: the kext in `src/`, the programs that talk to it in `userspace/`,
the host tests in `test/`.

| Directory | What is in it | Runs on |
|---|---|---|
| `build/` | What a build needs besides `make`: `fetch-firmware.sh` (AMD's firmware into `firmware/amdgpu/`), `build-osxcross.sh` (the build on Linux or WSL) | the development machine |
| `gen/` | Generators of files that are committed: `build-shaders.sh` (`src/shaders/` to `src/*_kernel.h`, `src/*_codeobj.h`), `gen-gfx12-draw.py` (`src/gfx12_draw*.h` from Mesa's register database), `pipegen/` (`src/pipe2_linux*.inc` and `test/ref/dp_*.inc` from Linux's display code) | Linux, with LLVM or a Linux tree |
| `rig/` | For the test machine and its OpenCore stick: `diagnostic-log.sh` (the log every card test ends with) `boot-history.sh` (the last boots from the log on disk: which ended young) and `freeze-watch.sh` (a launch daemon that keeps each boot's kernel lines on disk, for a machine that freezes), all three copied into the product directory's `bin/` by `make`; `set-boot.sh` (boot-args of one test boot), `stage-stick.sh` (puts a build on the stick), `cgmode.py` (switch a display mode), `spike-bar.sh` (the BAR size experiment) | the test machine (macOS), `stage-stick.sh` on the development machine |
| `vm/` | Virtual machines: `vm-opencore.sh` and `vm-ocplist.py` (an OpenCore disk with the kext for an OSX-KVM guest), `vm-test.sh` (the emulator regression), `logs-ssh.sh` (a guest's log over SSH), and `emu-*.sh`/`emu-*.py`, which build and drive QEMU with the emulated card of `emu/` | Linux |
| `linux/` | Ground truth from the card under Linux's amdgpu: `linux-capture.sh`, `linux-groundtruth.sh`, `dmub-trace.sh`, `cursor-test.py` (register and command captures), `linux-replay/` (the kext's command streams submitted through amdgpu), `radv-triangle/` (RADV's own triangle for comparison) | Linux, on the card |
| `metal/` | Research for the Metal phase: `accelcensus/` (what Apple's stack asks of an accelerator), `air/` (Metal's AIR to gfx1201), `e2/`, `e3/` (Apple's kernel collection, code objects across generations), `m0/`, `m1/` (the paravirtual GPU's device information and command stream) | Linux and the VM |

The scripts find the repository from their own place in it, so they can be called from anywhere. Those that take
a build's products ask make where they are (`make -s product-dir`).
