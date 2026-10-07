# Dry-running a real-card boot in the emulator (Linux box)

`tools/emu-linux.sh <boot>` boots macOS Recovery (Tahoe) headless in QEMU on the
emulated RX 9070 XT (`emu/qemu/rdna4.c`) with the kext from this checkout, using the
boot-args of boot `<boot>` of `tools/set-boot.sh` / `docs/real-card-plan.md`, and saves
the kext's log. `--diag` additionally runs `tools/diagnostic-log.sh` in the Recovery
Terminal, the way the user does on the stick, and brings the PASS/FAIL table back.

```sh
OSXCROSS=$HOME/.local/share/osxcross bash tools/build-osxcross.sh   # the kext + rdna4-run
bash tools/emu-linux.sh 3 --diag                                     # boot 3, ~2 min
```

(The scripts are not executable in git: run them with `bash`.)

## What a run produces

`$EMU/runs/<time>-boot<N>/` (`$EMU` = `~/work/tools/emu`, outside the repo):

| file | contents |
|---|---|
| `summary.txt` | what happened, with timings, then the kernel-log verdicts and (`--diag`) the feature table |
| `serial.log` | the whole kernel log (OpenCore, XNU, launchd, the kext); ~0.7 MB |
| `rdna4fb.log` | its `RDNA4FB:` lines (the serial console interleaves lines from other writers, so some are cut) |
| `diag-summary.txt`, `diag.txt` | `--diag`: the `Feature summary:` table, and the full `rdna4fb-diag-*.txt` (dmesg feature lines, the `RDNA4FB,Results` / `Compute,*` registry properties, every bounded test) |
| `screen-*.png` | monitor screendumps: `picker`, `recovery`, `terminal`, `diag`, `final` |
| `qemu.out` | QEMU's stderr, including the emulator's own messages |

Options: `--extra "args"` (append boot-args), `--args "..."` (replace the table),
`--dev trace=on` (rdna4 device options, e.g. `ih-dead=on`), `--keep` (leave the VM up; its
monitor socket is `monitor.sock` in the run dir), `--no-build` (reuse the OpenCore image of
the previous run of that boot), `--wait SECS`. `tools/emu-linux.sh -h` lists them.
Only one VM runs at a time (a new run kills the previous one).

Boot-args come from `tools/set-boot.sh` itself: it is run against a scratch config and its
printed line is taken, so a change to the table is followed automatically. `serial=3` is
appended (the log is how the run is observed) and `--extra` after that.

## Measured (this box: Ryzen 7 5700G, KVM, 8 GB guest)

| boot | picker answered | Recovery userland up | total |
|---|---|---|---|
| 1, log only | 17 s | 35 s | 87 s |
| 3, log only | 16 s | 34 s | 87 s |
| 1, `--diag` | 17 s | 35 s | 118 s (diagnostic 24 s) |
| 3, `--diag` | 16 s | 34 s | 127-133 s (diagnostic 33 s) |

Plus ~10 s to rebuild the OpenCore image unless `--no-build`. One-time setup: QEMU build
~1.5 min on 16 threads, edk2 clone several minutes (2.4 GB with submodules), the rest seconds.

## What boot 1 and boot 3 print (kext of `premetal/g4`, `789606f`; measured)

Boot 1: Recovery reached, `compute: bring-up finished at stage 7`, `runtime: user-space
runtime up`, vblank/flip self-tests pass. Table: runtime, ih, vblank, flip, anim, sensors,
gfxcg PASS; vm/gfx/gfx-col SKIPPED (not enabled).

Boot 3 (Recovery reached, no panic), kernel log:

```
RDNA4FB: gfx: draw: THE TRIANGLE IS RIGHT in 1186 us: 8192 pixels 0xff0000ff (want 8192), 0 others ...
RDNA4FB: gfx: col: wrong colour image in 76..122 us: 0 pixels (want 8192) ... target still empty
```

and the diagnostic table: runtime, submitib, fault, **vm**, ih, vblank, **gfx** ("ring test, THE
TRIANGLE IS RIGHT") PASS, **gfx-col FAIL** ("colour image, draw 0 px"), flip, anim, sensors,
gfxcg PASS; w6/gfxoff/gfxpm/sleep SKIPPED. That is the expected outcome: the emulator models the
NGG EXEC fix (G3 passes) and not the G4 attribute ring (gfx-col fails).

**Do not read more into the emulated PASS rows than they say.** The emulator models what the
kext polls; `vm PASS` here does not mean the real card's client VM work passes (on the card it
fails today, see the round-6 notes). This is a check that the boot reaches Recovery, the kext
loads, stages run, nothing panics and the test scripts work end to end, not a hardware result.

## One-time setup (what was downloaded and built; all user space, no sudo)

Everything lives in `$EMU` (`~/work/tools/emu`); nothing big is in the repo. The deps
(glib2, pixman, libslirp, meson, ninja, nasm, 7z, libuuid, bz2/zlib/openssl headers) were
already installed; `iasl` and `mtools` were not and are **not needed from the system**.

```sh
EMU=~/work/tools/emu; mkdir -p $EMU/{images,kexts} && cd $EMU

# 1. QEMU 10.0.13 + the rdna4 device (tools/emu-build.sh links emu/qemu/rdna4.c into the tree)
curl -sSL https://download.qemu.org/qemu-10.0.13.tar.xz | tar xJ
QEMU_SRC=$EMU/qemu-10.0.13 bash tools/emu-build.sh qemu      # x86_64-softmmu only
ninja -C qemu-10.0.13/build qemu-img                          # qemu-img, used by tools/vm-opencore.sh

# 2. The GOP option ROM. The RdnaGopPkg platform has no ASL, so edk2 needs no iasl.
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/tianocore/edk2
make -C edk2/BaseTools -j8
QEMU_SRC=$EMU/qemu-10.0.13 EDK2=$EMU/edk2 bash tools/emu-build.sh gop
QEMU_SRC=$EMU/qemu-10.0.13 EDK2=$EMU/edk2 bash tools/emu-build.sh rom   # -> build-emu/rdna4.rom (git-ignored, per checkout)

# 3. OVMF + the stock OpenCore image: OSX-KVM (its OVMF_CODE_4M.fd / OVMF_VARS-1920x1080.fd pair
#    and OpenCore/OpenCore.qcow2, which tools/vm-opencore.sh starts from)
git clone --depth 1 https://github.com/kholia/OSX-KVM

# 4. mtools (tools/vm-opencore.sh needs mcopy/mdeltree)
curl -sSL https://ftp.gnu.org/gnu/mtools/mtools-4.0.49.tar.gz | tar xz
(cd mtools-4.0.49 && ./configure --prefix=$EMU/local && make -j8 && make install)

# 5. Recovery: copy (read-only source!) and convert the stick's BaseSystem.dmg to a raw GPT disk
git clone --depth 1 https://github.com/Lekensteyn/dmg2img && make -C dmg2img
cp /run/media/miguer/OPENCORE/com.apple.recovery.boot/BaseSystem.dmg images/
dmg2img/dmg2img -i images/BaseSystem.dmg -o images/BaseSystem.img    # 2.5 GB raw

# 6. Lilu 1.7.2 (Tahoe needs >= 1.7.1; OSX-KVM's own is 1.6.8) from the stick
cp -r /run/media/miguer/OPENCORE/EFI/OC/Kexts/Lilu.kext kexts/
```

Paths are overridable: `EMU`, `QEMU_SRC`, `OSXKVM`, `KEXT` (default `build/RDNA4FB.kext`),
`LILU`, `BASE_IMG`, `RAM_MB` (default 8192).

## How it works, and where it differs from the real stick

- **OpenCore image**: `tools/vm-opencore.sh` starts from OSX-KVM's stock image, injects Lilu +
  RDNA4FB after it, disables WhateverGreen, sets the boot-args, a waiting picker and the
  1920x1080 GOP resolution. It is **not** the stick's `config.plist`: the stick also loads the
  AMD CPU kexts, VirtualSMC, NootRX etc. and real-board ACPI/SMBIOS; here only Lilu and RDNA4FB
  are loaded. The boot-args are exactly the table's; the kext set around them is not.
- **Display**: the emulated card's option ROM (AtomBIOS image + `RdnaGopDxe`) provides the GOP;
  OpenCore's picker and the Recovery UI are drawn through the card model at 1920x1080
  (`screen-picker.png`, `screen-recovery.png`).
- **Picker**: Timeout is 0, so the picker waits. With the entries of this setup (`EFI`, `macOS
  Base System`) the script sends `right`, `Enter` through the QEMU monitor (`PICKER_KEYS`
  overrides). The Windows flow's `right right Enter` was for a third entry (an installed disk)
  and here wraps back to `EFI`, which relaunches OpenCore.
- **Kernel log**: `serial=3` sends the kernel log to QEMU's `-serial file:`; launchd and the kext
  lines are in it. There is no installed macOS disk (`MACHDD=none`), so the SSH flow of
  `tools/vm-test.sh` does not apply.
- **`--diag`**: Cmd+Shift+T opens Terminal in Recovery (root prompt); `tools/emu-type.py` types
  `bash /Volumes/QEMU*/run-diag.sh` with monitor `sendkey`. The scripts (`diagnostic-log.sh`,
  `build/rdna4-run`) arrive on a read-only `fat:ro:` disk (QEMU vvfat, attached as usb-storage,
  mounted as `/Volumes/QEMU VVFAT`), are run from `/tmp`, and the output goes to
  `/dev/console`, which is the serial log (lines prefixed `RDNA4DIAG|`). Why not write back to
  the FAT disk: vvfat's `rw` mode left the host copy of the output file stuck at its first 1200
  bytes (measured), so results travel over the console instead. Serial-log lines from `/dev/console`
  and the kernel can interleave mid-line; the table and registry values came through intact in
  the three `--diag` runs so far (boot 1 once, boot 3 twice).
- **OVMF**: OSX-KVM's pair (`OVMF_CODE_4M.fd` + `OVMF_VARS-1920x1080.fd`, a fresh copy per run),
  the combination the Windows flow used; Arch's `/usr/share/edk2/x64/OVMF_CODE.4m.fd` was not
  tried (its VARS layout differs from OSX-KVM's 128 KB file).

## Options added later (all in `tools/emu-linux.sh -h`)

- `QEMU_BIN=<binary>`: which emulator build to use (A/B of `emu/qemu/rdna4.c` versions; keep each build as a copy, e.g.
  `$EMU/bin/<name>/qemu-system-x86_64`, because rebuilding in `$QEMU_SRC` overwrites it). `TAG=<name>` suffixes the run dir.
  After `emu-boot.sh`/`emu-linux.sh` start a VM, a new run only stops QEMU binaries under `$EMU` (never another QEMU on the box).
- Any boot id of `set-boot.sh` (`9b`, `11s`, `10`..`13`). `--args "..."` replaces the table, `--extra "..."` appends.
- Every run also writes `emu-warnings.txt` (QEMU stderr without host-CPU noise) and `emu-context.txt` (each distinct emulator message
  with the kext lines printed just before it, from the 0.1 s byte-offset sampler `offsets.txt`; `tools/emu-context.py`).
- `--pre "CMD"` (with `--diag`): a guest command run before `diagnostic-log.sh` in the same Terminal session, e.g.
  `--pre 'pmset displaysleepnow; sleep 8'` (output over the console; the blanked display does not matter).
- `--sleep-reset` (boot 11s): triggers the emulator's compute power reset (`qom-set /machine/peripheral/rdna4 sleep-reset true`) right
  after `power: quiesce complete`: the power-loss case; without it 11s is a wake without power loss.
- `--census` (E1 of `docs/metal-spike.md`, kext boot-arg `rdna4-accelcensus`): mounts `build/rdna4-census`, runs it in the Recovery Terminal and
  returns `census.txt` / `census-kernel.log`. Not combinable with `--diag`.
- `tools/emu-type.py <monitor.sock> <text>` types into the guest through the QEMU monitor (`\n` is Enter). Cmd+Shift+T opens the Recovery
  Terminal; the mouse does not reach the guest through the monitor here, but Ctrl+F2 + Enter opens the Apple menu.
- The picker is read from the screendump before each key press (efi vs Recovery tile) and retried; one run in ten used to lose both presses.

## What the emulated Recovery cannot do (measured)

- `pmset sleepnow` is a display sleep only (`power: HDMI display blanked via DPG`, powerd stays in DarkWake); the display does not come back
  and there is no system S3. The Apple menu in Recovery has no Sleep entry. Use boot `11s` (the driver's simulated cycle) for the sleep path.
- The SMU/sensors are synthetic (3 % / 120 W): the `post-idle` row always FAILs on the emulator.
- G4 (the attribute-ring colour draw) is not modelled: `gfx-col`, `gfx-app-tricol` FAIL.
- A kext that links IOGraphicsFamily/IOAcceleratorFamily2 does not load when OpenCore injects it (those families are in
  `BaseSystemKernelExtensions.kc`, not in the boot KC), see `docs/metal-spike.md`.

## Pitfalls met

- `pkill -f monitor.sock` kills the shell that runs it if its own command line contains the
  string; the script matches `^<qemu path> ` instead.
- The previous VM must be gone before the next starts (QEMU holds VNC `127.0.0.1:5900`, port
  `VNC_DISPLAY` picks another); the script waits for it.
- The VM is headless (`-display none`); `-vnc 127.0.0.1:0` is there for a look while debugging
  with `--keep` (`vncviewer 127.0.0.1:0`), never exposed beyond localhost.
- The USB stick is only read (BaseSystem.dmg and Lilu.kext copied out once); the set-boot.sh table
  is read from the repo copy, not the stick's.
- `emu-boot.sh` gained env overrides (`OVMF_CODE`, `OVMF_VARS`, `BASE_IMG`, `MACHDD`, `MONITOR`,
  `SERIAL_LOG`, `VNC_DISPLAY`, `EXTRA_QEMU`); their defaults keep the Windows/WSL flow unchanged.
