#!/usr/bin/env bash
# A FULL macOS 26.6.2 (25G83) install in a plain QEMU VM on this Linux box (docs/emu-full.md): the base the Metal phase's M0 needs, because
# Apple's AppleParavirtGPU.kext only exists in the full install's kernel collection, not in Recovery. No rdna4 device, no RDNA4FB kext needed.
#
#   tools/emu-full.sh <command> [options]
#
#   oc [--kext K.kext]... [--args "boot-args"] [--timeout SECS]
#                    build the OpenCore image (stock OSX-KVM image + Lilu 1.7.x, extra kexts injected after Lilu, boot-args, picker timeout)
#   serve start|stop|status
#                    the HTTP server (127.0.0.1:8088, reachable from the guest as http://10.0.2.2:8088/) that hands app.tar and the
#                    18 GB SharedSupport.dmg to the Recovery VM: the installer is NOT downloaded again from Apple
#   start <mode> [--wait SECS] [--extra "QEMU args"] [--rdna4 ["dev opts"]]   (--rdna4: the emulated RX 9070 XT instead of vmware-svga)
#                    boot the VM headless, in the background. mode = which disks are attached:
#                      recovery   OpenCore + Recovery (BaseSystem) + target + stage + media   (prepare the installer media)
#                      installer  OpenCore + media + target                                   (boot the installer, install)
#                      run        OpenCore + target                                           (the installed macOS)
#                      DISKS=oc,base,target,...  any list of oc|base|target|stage|media
#   stop             quit the VM this script started (its own pid file; never a name pattern)
#   status           pid, ports, last serial lines
#   mon "cmd"...     QEMU monitor commands          shot [name]   screendump -> $FULL/shots/<name>.png
#   click X Y [d]    pointer click at screen pixel X,Y (d = double) through the VNC server (the installer GUI needs it)
#   key <qemu key>...  e.g. key ret meta_l-shift-t   type "text\n"   type through the monitor (US layout)
#   wait "regex" [SECS]   wait for a line in the serial log
#   con "shell cmd"  Recovery/installer Terminal: type the command so that its output goes to /dev/console (= the serial log) and wait for it to end
#   ssh [cmd...]     run a command in the installed macOS over SSH (user/key from `provision`); no args = a login shell
#   snap list|save N|restore N|delete N   qcow2 snapshots of the VM disk (+ its NVRAM), VM stopped: roll back an experiment in seconds
#   provision        (from Recovery, target disk mounted) create the admin user + Remote Login + the SSH key offline: no Setup Assistant
#   verify           the facts M0 needs, read from the installed volume (AppleParavirtGPU, version, kernel collections)
#
# Everything big lives outside the repo: $FULL (default ~/work/tools/emu/full: qcow2 disks, serve/, shots/, logs/), the QEMU binary is a COPY at
# ~/work/tools/emu-full/bin/qemu-system-x86_64 so that tools/emu-linux.sh's "one VM at a time" kill pattern (^$EMU/.../qemu-system-x86_64 ) can never match
# this VM. ONE VM at a time with Kiln's emulator runs: ask it for the slot. Ports: monitor unix socket, VNC :9 (5909), ssh 127.0.0.1:10122, http 8088.
# Env: EMU, FULL, QEMU_BIN, RAM_MB (6144), SMP (4), NICE (10), VNC_DISPLAY (9), SSH_PORT (10122), HTTP_PORT (8088), DISKS, OC_IMG (OpenCore-full.qcow2).
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
EMU=${EMU:-$HOME/work/tools/emu}
FULL=${FULL:-$EMU/full}
QEMU_BIN=${QEMU_BIN:-$HOME/work/tools/emu-full/bin/qemu-system-x86_64}
PCBIOS=${PCBIOS:-$EMU/qemu-10.0.13/pc-bios}
OSXKVM=${OSXKVM:-$EMU/OSX-KVM}
LILU=${LILU:-$EMU/kexts/Lilu.kext}
OC_IMG=${OC_IMG:-OpenCore-full.qcow2}
OC_RUN_IMG=${OC_RUN_IMG:-OpenCore-full-run.qcow2}
BASE_IMG=${BASE_IMG:-$FULL/BaseSystem.img}   # OUR copy of the Recovery disk (Kiln's VMs hold a write lock on images/BaseSystem.img)
D_TARGET=${D_TARGET:-$FULL/macos-26.6.2.qcow2}
D_STAGE=${D_STAGE:-$FULL/stage.qcow2}
D_MEDIA=${D_MEDIA:-$FULL/install-media.qcow2}
VARS=${VARS:-$FULL/OVMF_VARS.fd}
MON=$FULL/monitor.sock
SERIAL=$FULL/serial.log
PIDF=$FULL/vm.pid
SPID=$FULL/serve.pid
SSH_PORT=${SSH_PORT:-10122}
HTTP_PORT=${HTTP_PORT:-8088}
SSH_KEY=${SSH_KEY:-$FULL/ssh/id_ed25519}
SSH_USER=${SSH_USER:-dev}
export PATH="$EMU/qemu-10.0.13/build:$EMU/local/bin:$PATH"   # qemu-img, mcopy

die() { echo "emu-full: $*" >&2; exit 1; }
mkdir -p "$FULL/shots" "$FULL/logs"

vm_pid() {   # the pid of OUR qemu, or nothing: the pid file, and /proc/<pid>/exe must be our binary
	[ -f "$PIDF" ] || return 1
	local p; p=$(cat "$PIDF")
	[ -n "$p" ] && [ -e "/proc/$p/exe" ] && [ "$(readlink -f "/proc/$p/exe")" = "$(readlink -f "$QEMU_BIN")" ] || return 1
	echo "$p"
}

mon() {   # mon "cmd" ...
	[ -S "$MON" ] || die "no monitor socket (VM not running?)"
	python3 - "$MON" "$@" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(0.3); s.recv(65536)
for c in sys.argv[2:]:
    s.send((c + "\n").encode()); time.sleep(0.6 if c.startswith("sendkey") else 0.25)
time.sleep(0.2)
PY
}

shot() {   # shot [name]: screendump (PPM) -> PNG
	local n=${1:-shot-$(date +%H%M%S)} f
	f=$FULL/shots/$n
	mon "screendump $f.ppm" >/dev/null 2>&1 || true
	python3 - "$f.ppm" <<'PY'
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
raw = b''.join(b'\0' + px[y*w*3:(y+1)*w*3] for y in range(h))
def ch(t, b): c = struct.pack('>I', len(b)) + t + b; return c + struct.pack('>I', zlib.crc32(t + b))
open(sys.argv[1][:-4] + '.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
PY
	rm -f "$f.ppm"
	echo "$f.png"
}

cmd_oc() {
	# Two images: OC_IMG (the picker waits for a key: Recovery/installer, you choose the entry) and OC_RUN_IMG (oc --run: auto-boots after --timeout, default 4 s;
	# the default entry must be Macintosh HD: `start run` does that once, see s.2 of docs/emu-full.md). `start run` uses OC_RUN_IMG when it exists.
	local kexts=() args="-v keepsyms=1 debug=0x100 serial=3" timeout="" out=$OC_IMG
	while [ $# -gt 0 ]; do
		case $1 in
			--kext) kexts+=(--kext "$2"); shift ;; --args) args=$2; shift ;; --timeout) timeout=$2; shift ;;
			--run) out=$OC_RUN_IMG ;;
			*) die "oc: unknown $1" ;;
		esac
		shift
	done
	[ -n "$timeout" ] || { [ "$out" = "$OC_RUN_IMG" ] && timeout=4 || timeout=0; }
	# run image: ScanPolicy 0x10303 (APFS+HFS on SATA, no ESP) so the picker has no "EFI" entry and the timeout boots Macintosh HD
	[ "$out" = "$OC_RUN_IMG" ] && export OC_SCAN_POLICY=${OC_SCAN_POLICY:-0x10303}
	OSXKVM=$OSXKVM bash "$HERE/tools/vm-opencore.sh" --lilu "$LILU" --args "$args" --timeout "$timeout" "${kexts[@]}" --out "$out"
}

cmd_serve() {
	case ${1:-status} in
		start)
			if [ -f "$SPID" ] && kill -0 "$(cat "$SPID")" 2>/dev/null; then echo "serve: already running (pid $(cat "$SPID"))"; return; fi
			python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 --directory "$FULL/serve" > "$FULL/logs/serve.log" 2>&1 &
			echo $! > "$SPID"; echo "serve: http://127.0.0.1:$HTTP_PORT/ (guest: http://10.0.2.2:$HTTP_PORT/), pid $(cat "$SPID")" ;;
		stop)
			if [ -f "$SPID" ] && kill -0 "$(cat "$SPID")" 2>/dev/null; then kill "$(cat "$SPID")"; echo "serve: stopped"; else echo "serve: not running"; fi
			rm -f "$SPID" ;;
		status) if [ -f "$SPID" ] && kill -0 "$(cat "$SPID")" 2>/dev/null; then echo "serve: running, pid $(cat "$SPID")"; else echo "serve: not running"; fi ;;
		*) die "serve start|stop|status" ;;
	esac
}

cmd_start() {
	local mode=${1:-run}; shift || true
	local wait=0 extra="" rdna4=0 rdna4_opts=""
	while [ $# -gt 0 ]; do
		case $1 in
			--wait) wait=$2; shift ;; --extra) extra=$2; shift ;;
			--rdna4) rdna4=1; case ${2:-} in ""|--*) ;; *) rdna4_opts=$2; shift ;; esac ;;   # the emulated RX 9070 XT instead of vmware-svga (options e.g. trace=on)
			*) die "start: unknown $1" ;;
		esac
		shift
	done
	vm_pid >/dev/null && die "a VM of ours is already running (pid $(vm_pid)): emu-full.sh stop"
	# Kiln's emulator VMs may run at the same time (lead, hub-task-432: isolated: own binary copy, ports, disks); only a note.
	pgrep -f "^$EMU/.*/qemu-system-x86_64 " >/dev/null 2>&1 && echo "emu-full: note: Kiln's emulator VM is running too (isolated; this script never touches it)"
	[ -w /dev/kvm ] || die "/dev/kvm is not writable"
	for f in "$QEMU_BIN" "$OSXKVM/OpenCore/$OC_IMG" "$OSXKVM/OVMF_CODE_4M.fd"; do [ -e "$f" ] || die "missing $f (docs/emu-full.md)"; done
	[ -f "$VARS" ] || cp "$OSXKVM/OVMF_VARS-1920x1080.fd" "$VARS"
	local disks=${DISKS:-}
	if [ -z "$disks" ]; then
		case $mode in
			recovery) disks=oc,base,target,stage,media ;;
			installer) disks=oc,media,target ;;
			run) disks=oc,target ;;
			*) die "start: mode recovery|installer|run (or DISKS=...)" ;;
		esac
	fi
	local dargs=() port=0 d ocimg=$OC_IMG
	[ "$mode" = run ] && [ -f "$OSXKVM/OpenCore/$OC_RUN_IMG" ] && ocimg=$OC_RUN_IMG
	for d in ${disks//,/ }; do
		case $d in
			oc)     dargs+=(-drive "id=oc,if=none,snapshot=on,format=qcow2,file=$OSXKVM/OpenCore/$ocimg" -device "ide-hd,bus=sata.$port,drive=oc") ;;
			base)   dargs+=(-drive "id=base,if=none,snapshot=on,format=raw,file=$BASE_IMG" -device "ide-hd,bus=sata.$port,drive=base") ;;
			target) dargs+=(-drive "id=target,if=none,format=qcow2,file=$D_TARGET" -device "ide-hd,bus=sata.$port,drive=target") ;;
			stage)  dargs+=(-drive "id=stage,if=none,format=qcow2,file=$D_STAGE" -device "ide-hd,bus=sata.$port,drive=stage") ;;
			media)  dargs+=(-drive "id=media,if=none,format=qcow2,file=$D_MEDIA" -device "ide-hd,bus=sata.$port,drive=media") ;;
			*) die "unknown disk $d" ;;
		esac
		port=$((port + 1))
	done
	rm -f "$MON"
	: > "$SERIAL"
	local args=(
		-L "$PCBIOS" -enable-kvm -m "${RAM_MB:-6144}"
		-cpu "Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt,check"
		-machine q35 -global ICH9-LPC.disable_s3=0
		-smp "${SMP:-4},cores=4,sockets=1"
		-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 -device usb-ehci,id=ehci
		-device "isa-applesmc,osk=ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc"
		-drive "if=pflash,format=raw,readonly=on,file=$OSXKVM/OVMF_CODE_4M.fd" -drive "if=pflash,format=raw,file=$VARS"
		-smbios type=2 -device ich9-intel-hda -device hda-duplex
		-device ich9-ahci,id=sata "${dargs[@]}"
		-netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22" -device virtio-net-pci,netdev=net0,id=net0,mac=52:54:00:c9:18:28
		-monitor "unix:$MON,server,nowait" -serial "file:$SERIAL" -pidfile "$PIDF"
		-display none -vnc "127.0.0.1:${VNC_DISPLAY:-9}"
	)
	if [ "$rdna4" = 1 ]; then
		# Same device line as tools/emu-boot.sh: alone on the root bus, -vga none (no stdvga for OVMF to pick as the boot display).
		local rom state flash r
		for r in "$HERE" "$HOME/work/rx4darwin/RDNA4FB-emu" "$HOME/work/rx4darwin/RDNA4FB"; do
			[ -z "${rom:-}" ] && [ -f "$r/build-emu/rdna4.rom" ] && rom=$r/build-emu/rdna4.rom
			[ -z "${state:-}" ] && [ -f "$r/emu/qemu/gop-state.txt" ] && state=$r/emu/qemu/gop-state.txt
			[ -z "${flash:-}" ] && [ -f "$r/firmware/Sapphire.RX9070XT.16384.241213.rom" ] && flash=$r/firmware/Sapphire.RX9070XT.16384.241213.rom
		done
		rom=${RDNA4_ROM:-${rom:-}}; state=${RDNA4_STATE:-${state:-}}; flash=${RDNA4_FLASH:-${flash:-}}
		for r in "$rom" "$state" "$flash"; do [ -f "${r:-/nonexistent}" ] || die "--rdna4: missing rom/state/flash ($rom $state $flash; env RDNA4_ROM/RDNA4_STATE/RDNA4_FLASH)"; done
		args+=(-vga none -device "rdna4,id=rdna4,bus=pcie.0,addr=0x10,romfile=$rom,state=$state,flash=$flash${rdna4_opts:+,$rdna4_opts}")
	else
		args+=(-device vmware-svga)
	fi
	# shellcheck disable=SC2206
	args+=($extra)
	echo "emu-full: $mode: disks $disks; display $([ "$rdna4" = 1 ] && echo "rdna4 device" || echo vmware-svga); RAM ${RAM_MB:-6144} MB; monitor $MON; VNC 127.0.0.1:${VNC_DISPLAY:-9}; ssh 127.0.0.1:$SSH_PORT; serial $SERIAL" | tee "$FULL/logs/last-start.txt"
	echo "$QEMU_BIN ${args[*]}" >> "$FULL/logs/last-start.txt"
	setsid nohup nice -n "${NICE:-10}" "$QEMU_BIN" "${args[@]}" > "$FULL/logs/qemu.out" 2>&1 < /dev/null &
	local i
	for i in $(seq 1 30); do [ -S "$MON" ] && break; sleep 1; done
	[ -S "$MON" ] || { tail -5 "$FULL/logs/qemu.out"; die "QEMU did not start"; }
	vm_pid >/dev/null || die "QEMU started but its pid file does not match our binary"
	echo "emu-full: running, pid $(vm_pid)"
	[ "$wait" -gt 0 ] && cmd_wait "BdsDxe: starting|Darwin Kernel" "$wait" || true
}

cmd_stop() {
	local p
	p=$(vm_pid) || { echo "emu-full: no VM of ours is running"; rm -f "$PIDF"; return 0; }
	mon "quit" >/dev/null 2>&1 || true
	local i
	for i in $(seq 1 20); do [ -e "/proc/$p" ] || break; sleep 0.5; done
	[ -e "/proc/$p" ] && kill "$p" 2>/dev/null || true
	rm -f "$PIDF" "$MON"
	echo "emu-full: stopped pid $p"
}

cmd_status() {
	if p=$(vm_pid); then echo "VM: running, pid $p"; else echo "VM: not running"; fi
	cmd_serve status
	echo "disks:"; ls -la --block-size=M "$D_TARGET" "$D_STAGE" "$D_MEDIA" 2>/dev/null | awk '{print "  " $5, $NF}'
	du -h --apparent-size=0 "$D_TARGET" "$D_STAGE" "$D_MEDIA" 2>/dev/null | sed 's/^/  on disk: /' || true
	[ -s "$SERIAL" ] && { echo "serial tail:"; tail -n 5 "$SERIAL" | cut -c1-160 | sed 's/^/  /'; } || true
}

cmd_wait() {   # wait "regex" [SECS]
	local pat=$1 secs=${2:-300} i
	for i in $(seq 1 "$secs"); do
		grep -aqE "$pat" "$SERIAL" 2>/dev/null && { echo "emu-full: saw /$pat/ after ${i}s"; return 0; }
		vm_pid >/dev/null || { echo "emu-full: VM is gone"; return 1; }
		sleep 1
	done
	echo "emu-full: timeout waiting for /$pat/"; return 1
}

cmd_click() {   # click X Y [d]: pointer click at screen pixel X,Y (d = double click)
	# The usb-tablet is ignored by macOS in Recovery/the installer (pointer stuck at 0,0), so a relative usb-mouse is hot-plugged on first use and
	# driven through the monitor: park in the corner (big negative move), then step to the target in <=100 unit moves (macOS pointer acceleration: measured
	# ~1.12 x / ~1.06 y pointer pixels per mouse unit at this step size, so the defaults CLICK_SCALE_X/_Y compensate; icons and buttons are big enough).
	local x=$1 y=$2 scx=${CLICK_SCALE_X:-0.895} scy=${CLICK_SCALE_Y:-0.942} i dx dy cmds=()
	mon "info usb" 2>/dev/null | grep -q rmouse || mon "device_add usb-mouse,bus=xhci.0,id=rmouse" >/dev/null 2>&1 || true
	sleep 1
	cmds+=("mouse_move -4000 -4000")
	dx=$(python3 -c "print(int($x*$scx))"); dy=$(python3 -c "print(int($y*$scy))")
	while [ "$dx" -gt 0 ] || [ "$dy" -gt 0 ]; do
		local sx=$(( dx > 100 ? 100 : dx )) sy=$(( dy > 100 ? 100 : dy ))
		cmds+=("mouse_move $sx $sy"); dx=$((dx - sx)); dy=$((dy - sy))
	done
	if [ -z "${MOVE_ONLY:-}" ]; then
		cmds+=("mouse_button 1" "mouse_button 0")
		[ -n "${3:-}" ] && cmds+=("mouse_button 1" "mouse_button 0")
	fi
	mon "${cmds[@]}" >/dev/null
}
cmd_type() { python3 "$HERE/tools/emu-type.py" "$MON" "$1"; }
cmd_key() { local k; for k in "$@"; do mon "sendkey $k" >/dev/null; done; }

cmd_con() {   # con "shell cmd" [SECS]: run in the guest Terminal (Recovery/installer), output to the serial log
	# The command text is NOT typed (a busy guest drops key events): it is written to $FULL/serve/cmd/<tag>.sh and the guest types one short line
	# that fetches and runs it from the HTTP server (cmd_serve start). Output comes back through /dev/console (the serial log).
	local c=$1 secs=${2:-600} tag n
	cmd_serve status | grep -q "running" || cmd_serve start >/dev/null
	tag=EFULL-$(date +%s)-$RANDOM
	mkdir -p "$FULL/serve/cmd"
	printf '( %s ) 2>&1 | awk '"'"'{print "EFULLOUT|" $0}'"'"' > /dev/console\necho %s-DONE > /dev/console\n' "$c" "$tag" > "$FULL/serve/cmd/$tag.sh"
	n=$(wc -c < "$SERIAL")
	cmd_type "curl -so /tmp/c.sh http://10.0.2.2:$HTTP_PORT/cmd/$tag.sh; bash /tmp/c.sh\n"
	cmd_wait "$tag-DONE" "$secs" >/dev/null || { echo "emu-full: con: command did not finish in ${secs}s" >&2; return 1; }
	tail -c +"$((n + 1))" "$SERIAL" | grep -a "EFULLOUT|" | sed 's/.*EFULLOUT|//' | tr -d '\r' || true
}

cmd_provision() {   # provision: in Recovery (target installed, DISKS has target), create user/Remote Login/key/skip Setup Assistant
	local user=${SSH_USER} pass=${SSH_PASS:-Emu-full-2026}
	mkdir -p "$FULL/ssh" "$FULL/serve"
	[ -f "$SSH_KEY" ] || ssh-keygen -q -t ed25519 -N "" -C "emu-full" -f "$SSH_KEY"
	cp "$HERE/tools/emu-full-guest.sh" "$FULL/serve/emu-full-guest.sh"
	cmd_con "curl -so /tmp/g.sh http://10.0.2.2:$HTTP_PORT/emu-full-guest.sh && bash /tmp/g.sh provision '$user' '$pass' '$(cat "$SSH_KEY.pub")'" "${1:-120}"
}

cmd_verify() {   # verify: the facts M0 needs, read from the installed volume (Recovery, via con) or over SSH (installed macOS running)
	if [ -f "$SSH_KEY" ] && ( cmd_ssh true ) >/dev/null 2>&1; then
		cmd_ssh 'sw_vers; echo "--- AppleParavirtGPU"; ls -d /System/Library/Extensions/*Paravirt* 2>&1; for kc in /System/Library/KernelCollections/*.kc; do echo "$kc: $(grep -a -c com.apple.driver.AppleParavirtGPU "$kc") matches for com.apple.driver.AppleParavirtGPU"; done; kmutil showloaded 2>/dev/null | grep -i -E "paravirt|RDNA4" || echo "(no paravirt/RDNA4 kext loaded)"; echo "--- kernel"; uname -a; sysctl kern.bootargs machdep.cpu.brand_string kern.hv_vmm_present 2>&1; echo "--- SIP/AMFI"; csrutil status 2>&1; nvram boot-args 2>&1'
	else
		cp "$HERE/tools/emu-full-guest.sh" "$FULL/serve/emu-full-guest.sh"
		cmd_con "curl -so /tmp/g.sh http://10.0.2.2:$HTTP_PORT/emu-full-guest.sh && bash /tmp/g.sh verify" "${1:-120}"
	fi
}

cmd_snap() {   # snap list | save NAME | restore NAME | delete NAME: qcow2 internal snapshots of the VM disk (VM must be stopped)
	vm_pid >/dev/null && die "snap: stop the VM first (emu-full.sh stop)"
	case ${1:-list} in
		list) qemu-img snapshot -l "$D_TARGET" ;;
		save) [ -n "${2:-}" ] || die "snap save NAME"; qemu-img snapshot -c "$2" "$D_TARGET" && cp "$VARS" "$VARS.$2" && echo "snap: saved '$2' (disk + NVRAM copy $VARS.$2)" ;;
		restore) [ -n "${2:-}" ] || die "snap restore NAME"; qemu-img snapshot -a "$2" "$D_TARGET" && { [ -f "$VARS.$2" ] && cp "$VARS.$2" "$VARS"; echo "snap: restored '$2'"; } ;;
		delete) [ -n "${2:-}" ] || die "snap delete NAME"; qemu-img snapshot -d "$2" "$D_TARGET" && rm -f "$VARS.$2" && echo "snap: deleted '$2'" ;;
		*) die "snap list|save NAME|restore NAME|delete NAME" ;;
	esac
}

cmd_ssh() {
	[ -f "$SSH_KEY" ] || die "no SSH key at $SSH_KEY (run provision from Recovery first)"
	exec ssh -i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile="$FULL/ssh/known_hosts" -o BatchMode=yes -o ConnectTimeout=10 "$SSH_USER@127.0.0.1" "$@"
}

c=${1:-}; [ -n "$c" ] || { sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 2; }
shift
case $c in
	oc) cmd_oc "$@" ;;
	serve) cmd_serve "$@" ;;
	start) cmd_start "$@" ;;
	stop) cmd_stop ;;
	status) cmd_status ;;
	mon) mon "$@" ;;
	shot) shot "$@" ;;
	key) cmd_key "$@" ;;
	click) cmd_click "$@" ;;
	type) cmd_type "$1" ;;
	wait) cmd_wait "$@" ;;
	con) cmd_con "$@" ;;
	ssh) cmd_ssh "$@" ;;
	provision) cmd_provision "$@" ;;
	snap) cmd_snap "$@" ;;
	verify) cmd_verify "$@" ;;
	-h|--help|help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//' ;;
	*) die "unknown command $c (try: help)" ;;
esac
