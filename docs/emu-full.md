# A full macOS 26.6.2 (25G83) in a plain QEMU VM (`tools/emu-full.sh`)

Author: Anvil, branch `premetal/emu-full` (hub-task-428 / 432). Evidence tags as elsewhere: [M] measured here, [I] inferred, [U] unknown.

Why: the Metal phase (M0, `docs/metal-spike.md`) needs Apple's `AppleParavirtGPU.kext`, which exists only in a **full** install's kernel
collections, not in Recovery (the Recovery/`BaseSystem` VMs of `tools/emu-linux.sh` have no such kext). This VM is the base for that: a
plain QEMU/KVM macOS (OpenCore + Lilu, vmware-svga display), **no rdna4 device and no RDNA4FB kext needed**. `start --rdna4` swaps the
display for Kiln's emulated RX 9070 XT, the same device line as `tools/emu-boot.sh`.

## 1. What exists (all outside the repo, nothing downloaded again)

| Path (under `~/work/tools/emu/full`) | What | Size |
|---|---|---|
| `macos-26.6.2.qcow2` | the installed macOS 26.6.2 (25G83), 96 GB virtual, sparse | see s.7 |
| `OVMF_VARS.fd` | its own NVRAM | 1 MB |
| `BaseSystem.img` | own copy of the Recovery disk (Kiln's VMs hold a write lock on `images/BaseSystem.img`) | 2.5 GB |
| `stage.qcow2`, `install-media.qcow2` | build-time disks (the rebuilt installer app; `createinstallmedia` output). **Deletable after the install**; `install-media` is needed again only to reinstall | 18 GB / 37 GB |
| `serve/`, `shots/`, `logs/`, `serial.log`, `monitor.sock`, `vm.pid`, `ssh/` | HTTP server root, screendumps, QEMU log, the serial console, HMP socket, pid file, SSH key | |
| `~/work/tools/emu-full/bin/qemu-system-x86_64` | a COPY of Kiln's QEMU 10.0.13 build (includes the `rdna4` device), so Kiln's `pkill -f "^$EMU/.*/qemu-system-x86_64 "` can never match this VM | 81 MB |

The Apple installer is **not** downloaded again: `~/work/tools/macos-full/25G83/InstallAssistant.pkg` (Forge's download, kept on disk) is the only source.

## 2. Using it

```sh
tools/emu-full.sh start run --wait 600          # OpenCore -> the installed macOS, headless; serial log in ~/work/tools/emu/full/serial.log
tools/emu-full.sh ssh 'sw_vers'                 # run a command inside (SSH key from `provision`; user dev, password dev)
tools/emu-full.sh verify                        # sw_vers, AppleParavirtGPU files, loaded kexts, SIP/boot-args
tools/emu-full.sh stop                          # quit OUR qemu (pid file, /proc/<pid>/exe checked; never a name pattern)
```

Extra kexts and boot-args go into the OpenCore image, not the command line (macOS reads the boot-args from OpenCore's `config.plist`):

```sh
tools/emu-full.sh oc --kext ~/work/rx4darwin/RDNA4FB-m0/build/RDNA4FB.kext --args "-v keepsyms=1 debug=0x100 serial=3 rdna4-pvgpu=1" --timeout 3
tools/emu-full.sh start run --rdna4 "trace=on"   # the emulated card instead of vmware-svga (needs build-emu/rdna4.rom: RDNA4_ROM/RDNA4_STATE/RDNA4_FLASH override)
```

`oc` builds `OSX-KVM/OpenCore/OpenCore-full.qcow2` with `tools/vm-opencore.sh` (stock OSX-KVM OpenCore + Lilu 1.7.x; `--kext` is repeatable and injected after
Lilu; WhateverGreen stays disabled; `--timeout SECS` is the picker timeout, 0 = wait for a key; `serial=3` puts the kernel/console log into `serial.log`). The OpenCore
disk is attached with `snapshot=on`, so rebuilding it is the way to change boot-args and kexts; the VM disk is never touched by that.

Environment: `RAM_MB` (default 6144), `SMP` (4), `NICE` (10: QEMU runs under `nice -n 10` because the user works on this PC), `VNC_DISPLAY` (9 = 127.0.0.1:5909), `SSH_PORT` (10122), `HTTP_PORT` (8088).

Other commands (`tools/emu-full.sh help`): `status`, `mon "hmp cmd"`, `shot [name]` (screendump to `shots/<name>.png`), `key ret meta_l-shift-t`, `type "text"`, `click X Y`, `wait "regex" SECS`, `con "cmd"`.

## 3. How the install was done without macOS (the recipe, for reproduction)

1. **Rebuild `Install macOS Tahoe.app`** from `InstallAssistant.pkg` on Linux: the pkg is a flat xar; `Payload` is pbzx (xz chunks) of a cpio; `bsdtar` extracts the app
   (50 MB: it has no `SharedSupport` yet).
2. **`SharedSupport.dmg` must be the whole `InstallAssistant.pkg`, not a carved dmg.** The pkg's `postinstall` (`link_shared_support.bash`) hard-links (or `cp`s)
   `${PACKAGE_PATH}` itself to `Contents/SharedSupport/SharedSupport.dmg`; the pkg *is* a mountable dmg (its `koly` trailer has `DataForkOffset` 17953771, the xar header,
   TOC and the small payloads precede the dmg data). [M] A first attempt used the dmg carved out of the pkg (18,366,669,607 bytes, `koly` at offset 0): `hdiutil` mounts it,
   but the installer fails at once with "Installation cannot proceed because the installer is damaged" (GUI and `startosinstall`), and `install.log` says
   `Getting offset for dmg in pkg: ... SharedSupport.dmg` / `pkgdmg validation has failed` / `Operation failed - OSISVerifyBaseSystemOperation`: it verifies the pkg (xar) as a
   whole. With the whole pkg (18,384,624,402 bytes) copied over the carve the verification passes. **No Apple download and no Recovery "Reinstall macOS" was needed.**
3. **Recovery VM** (`start recovery`: OpenCore + BaseSystem + target + stage + media). The app and the pkg reach the guest over HTTP (`serve start`; guest address
   `10.0.2.2:8088`; 18 GB in ~2.5 minutes): `createinstallmedia` run inside Recovery builds the installer media.
4. **Installer VM** (`start installer`: OpenCore + media + target). `startosinstall --agreetolicense --volume "/Volumes/Macintosh HD" --nointeraction` from the installer's
   Terminal, or the GUI. "Preparing" runs ~15 minutes (extracting 19 GB, then "Validating integrity of extracted contents", which is quiet for ~10 minutes), then the VM
   reboots by itself into the second stage (OpenCore boots it unattended) and finishes in ~30 minutes, rebooting a few times.
5. `provision` (below), then `start run`.

## 4. Driving the guest without a working pointer

- **Keyboard**: `key`/`type` go through the HMP `sendkey`. Typing long text while the guest is busy drops or garbles key events (also `:` came out as `/`): `con` therefore
  does not type the command; it writes it to `serve/cmd/<tag>.sh` and types ONE short line (`curl -so /tmp/c.sh http://10.0.2.2:8088/cmd/<tag>.sh; bash /tmp/c.sh`).
  The script's output goes to `/dev/console`, which is the serial log (boot-arg `serial=3`), prefixed `EFULLOUT|`, ended by a `<tag>-DONE` marker. `con` waits for it and prints the output.
  Cmd+Shift+T opens Terminal in Recovery/the installer.
- **Pointer**: macOS Recovery/installer **ignore the `usb-tablet`** (the pointer stays at 0,0; HMP absolute moves and VNC absolute events do nothing). `click X Y`
  hot-plugs a relative `usb-mouse`, parks the pointer in the corner and steps to the target (pointer acceleration: ~1.12 x / ~1.06 y pixels per unit, compensated by
  `CLICK_SCALE_X/_Y`). The position drifts between calls; prefer the keyboard (Tab to focus, arrows, **Space** activates a button, Return only the default one).
- Screen: `shot` (HMP `screendump`), then look at the PNG.

## 5. Isolation from Kiln's runs (hub-task-432)

Own QEMU binary copy (above), own ports (VNC :9, ssh 10122, http 8088), own disks and NVRAM, `nice -n 10`, `-smp 4 -m 6G`. `stop` kills only the pid in `vm.pid`
after checking `/proc/<pid>/exe` is our binary. The `serve` server is stopped with `serve stop` (pid file).

## 6. `provision`: the offline Setup Assistant

`tools/emu-full.sh provision` (from Recovery with the target disk attached) runs `tools/emu-full-guest.sh provision`: on the installed **Data** volume it creates the
admin user `dev` (password `dev`) with `dscl -f .../dslocal/nodes/Default`, writes `~dev/.ssh/authorized_keys` (key generated at `ssh/id_ed25519`), touches
`/private/var/db/.AppleSetupDone` (no Setup Assistant) and sets `com.openssh.sshd` to enabled in `/private/var/db/com.apple.xpc.launchd/disabled.plist` (Remote Login).

## 7. Results

(filled in when the install boots; see the end of this document)
