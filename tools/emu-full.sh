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
#   start <mode> [--wait SECS] [--extra "QEMU args"]
#                    boot the VM headless, in the background. mode = which disks are attached:
#                      recovery   OpenCore + Recovery (BaseSystem) + target + stage + media   (prepare the installer media)
#                      installer  OpenCore + media + target                                   (boot the installer, install)
#                      run        OpenCore + target                                           (the installed macOS)
#                      DISKS=oc,base,target,...  any list of oc|base|target|stage|media
#   stop             quit the VM this script started (its own pid file; never a name pattern)
#   status           pid, ports, last serial lines
#   mon "cmd"...     QEMU monitor commands          shot [name]   screendump -> $FULL/shots/<name>.png
#   key <qemu key>...  e.g. key ret meta_l-shift-t   type "text\n"   type through the monitor (US layout)
#   wait "regex" [SECS]   wait for a line in the serial log
#   con "shell cmd"  Recovery/installer Terminal: type the command so that its output goes to /dev/console (= the serial log) and wait for it to end
#   ssh [cmd...]     run a command in the installed macOS over SSH (user/key from `provision`); no args = a login shell
#   provision        (from Recovery, target disk mounted) create the admin user + Remote Login + the SSH key offline: no Setup Assistant
#   verify           the facts M0 needs, read from the installed volume (AppleParavirtGPU, version, kernel collections)
#
# Everything big lives outside the repo: $FULL (default ~/work/tools/emu/full: qcow2 disks, serve/, shots/, logs/), the QEMU binary is a COPY at
# ~/work/tools/emu-full/bin/qemu-system-x86_64 so that tools/emu-linux.sh's "one VM at a time" kill pattern (^$EMU/.../qemu-system-x86_64 ) can never match
# this VM. ONE VM at a time with Kiln's emulator runs: ask it for the slot. Ports: monitor unix socket, VNC :9 (5909), ssh 127.0.0.1:10122, http 8088.
# Env: EMU, FULL, QEMU_BIN, RAM_MB (8192), SMP (8), VNC_DISPLAY (9), SSH_PORT (10122), HTTP_PORT (8088), DISKS, OC_IMG (OpenCore-full.qcow2).
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
EMU=${EMU:-$HOME/work/tools/emu}
FULL=${FULL:-$EMU/full}
QEMU_BIN=${QEMU_BIN:-$HOME/work/tools/emu-full/bin/qemu-system-x86_64}
PCBIOS=${PCBIOS:-$EMU/qemu-10.0.13/pc-bios}
OSXKVM=${OSXKVM:-$EMU/OSX-KVM}
LILU=${LILU:-$EMU/kexts/Lilu.kext}
OC_IMG=${OC_IMG:-OpenCore-full.qcow2}
BASE_IMG=${BASE_IMG:-$EMU/images/BaseSystem.img}
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
	local kexts=() args="-v keepsyms=1 debug=0x100 serial=3" timeout=0
	while [ $# -gt 0 ]; do
		case $1 in --kext) kexts+=(--kext "$2"); shift ;; --args) args=$2; shift ;; --timeout) timeout=$2; shift ;; *) die "oc: unknown $1" ;; esac
		shift
	done
	OSXKVM=$OSXKVM bash "$HERE/tools/vm-opencore.sh" --lilu "$LILU" --args "$args" --timeout "$timeout" "${kexts[@]}" --out "$OC_IMG"
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
	local wait=0 extra=""
	while [ $# -gt 0 ]; do
		case $1 in --wait) wait=$2; shift ;; --extra) extra=$2; shift ;; *) die "start: unknown $1" ;; esac
		shift
	done
	vm_pid >/dev/null && die "a VM of ours is already running (pid $(vm_pid)): emu-full.sh stop"
	if pgrep -f "^$EMU/.*/qemu-system-x86_64 " >/dev/null 2>&1; then
		die "Kiln's emulator VM is running: ask Kiln for the slot (one VM at a time; this script never kills it)"
	fi
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
	local dargs=() port=0 d
	for d in ${disks//,/ }; do
		case $d in
			oc)     dargs+=(-drive "id=oc,if=none,snapshot=on,format=qcow2,file=$OSXKVM/OpenCore/$OC_IMG" -device "ide-hd,bus=sata.$port,drive=oc") ;;
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
		-L "$PCBIOS" -enable-kvm -m "${RAM_MB:-8192}"
		-cpu "Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt,check"
		-machine q35 -global ICH9-LPC.disable_s3=0
		-smp "${SMP:-8},cores=4,sockets=1"
		-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 -device usb-ehci,id=ehci
		-device "isa-applesmc,osk=ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc"
		-drive "if=pflash,format=raw,readonly=on,file=$OSXKVM/OVMF_CODE_4M.fd" -drive "if=pflash,format=raw,file=$VARS"
		-smbios type=2 -device ich9-intel-hda -device hda-duplex
		-device ich9-ahci,id=sata "${dargs[@]}"
		-netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22" -device virtio-net-pci,netdev=net0,id=net0,mac=52:54:00:c9:18:28
		-monitor "unix:$MON,server,nowait" -serial "file:$SERIAL" -pidfile "$PIDF"
		-device vmware-svga -display none -vnc "127.0.0.1:${VNC_DISPLAY:-9}"
	)
	# shellcheck disable=SC2206
	args+=($extra)
	echo "emu-full: $mode: disks $disks; RAM ${RAM_MB:-8192} MB; monitor $MON; VNC 127.0.0.1:${VNC_DISPLAY:-9}; ssh 127.0.0.1:$SSH_PORT; serial $SERIAL" | tee "$FULL/logs/last-start.txt"
	echo "$QEMU_BIN ${args[*]}" >> "$FULL/logs/last-start.txt"
	setsid nohup "$QEMU_BIN" "${args[@]}" > "$FULL/logs/qemu.out" 2>&1 < /dev/null &
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

cmd_type() { python3 "$HERE/tools/emu-type.py" "$MON" "$1"; }
cmd_key() { local k; for k in "$@"; do mon "sendkey $k" >/dev/null; done; }

cmd_con() {   # con "shell cmd" [SECS]: run in the guest Terminal (Recovery/installer), output to the serial log
	local c=$1 secs=${2:-600} tag n
	tag=EFULL-$(date +%s)-$RANDOM
	n=$(wc -c < "$SERIAL")
	cmd_type "( $c ) > /tmp/efull.out 2>&1; cat /tmp/efull.out | sed 's/^/EFULLOUT|/' > /dev/console; echo $tag-DONE > /dev/console\n"
	cmd_wait "$tag-DONE" "$secs" >/dev/null || { echo "emu-full: con: command did not finish in ${secs}s" >&2; return 1; }
	tail -c +"$((n + 1))" "$SERIAL" | grep -a "EFULLOUT|" | sed 's/^.*EFULLOUT|//' | tr -d '\r'
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
	type) cmd_type "$1" ;;
	wait) cmd_wait "$@" ;;
	con) cmd_con "$@" ;;
	ssh) cmd_ssh "$@" ;;
	-h|--help|help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//' ;;
	*) die "unknown command $c (try: help)" ;;
esac
