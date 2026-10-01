#!/usr/bin/env bash
# Dry-run one boot of docs/real-card-plan.md (tools/set-boot.sh's table) in the
# emulator on the Linux box: macOS Recovery (Tahoe) headless on the emulated
# RX 9070 XT, our kext injected by OpenCore, the kernel log on a serial file.
#
#   tools/emu-linux.sh <boot> [options]
#
#   --wait SECS     give up waiting after SECS (default 420); the run normally ends
#                   GRACE_S (45) seconds after Recovery is up and the kext is done
#   --extra "ARGS"  more boot-args after the table's (e.g. "rdna4-gfxcol=0")
#   --args "ARGS"   replace the table: use exactly these boot-args
#   --dev OPTS      rdna4 device options, e.g. trace=on or ih-dead=on
#   --diag          after the kext is done, open the Recovery Terminal (Cmd+Shift+T) and
#                   run tools/diagnostic-log.sh with build/rdna4-run from a read-only
#                   FAT disk, as the user does on the stick; its feature table and full
#                   log come back over the serial console (diag-summary.txt, diag.txt)
#   --census        E1 of docs/metal-spike.md (kext boot-arg rdna4-accelcensus=1|2, pass it with --extra): after the kext is up run build/rdna4-census
#                   (tools/accelcensus/census.m: registry, MTLCopyAllDevices, IOServiceOpen) in the Recovery Terminal; its output and the
#                   unified-log lines about Metal come back over the serial console (census.txt), the kext's census lines in census-kernel.log.
#                   Not combinable with --diag.
#   --sleep-reset   with boot 11s: trigger the emulator's compute power reset (device property sleep-reset)
#                   in the sleep window, right after "power: quiesce complete": the power-loss case
#   --pre "CMD"     with --diag: a shell command the guest runs first, before diagnostic-log.sh
#                   (e.g. "pmset displaysleepnow; sleep 8"), in the same Terminal session
#   --keep          leave the VM running afterwards (monitor socket in the run dir)
#   --no-build      reuse the OpenCore image of the previous run of this boot
#   -h              this text
#
# Everything big lives outside the repo, under $EMU (default ~/work/tools/emu,
# see docs/emu-linux.md for how it is built): qemu-10.0.13/ (with the rdna4
# device), OSX-KVM/ (stock OpenCore image + OVMF pair), images/BaseSystem.img
# (Recovery, made from the USB's BaseSystem.dmg), kexts/Lilu.kext, local/ (mtools).
# Env: TAG (suffix of the run dir, e.g. the emulator name), EMU, QEMU_BIN (the emulator binary; default the QEMU tree's build), KEXT (default build/RDNA4FB.kext of this checkout), RAM_MB (8192).
#
# Each run gets $EMU/runs/<time>-boot<N>/ with serial.log (kernel log),
# rdna4fb.log (the RDNA4FB lines), qemu.out, screen-*.png (monitor screendumps)
# and summary.txt; nothing is ever written to the OpenCore USB.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
EMU=${EMU:-$HOME/work/tools/emu}
export QEMU_SRC=${QEMU_SRC:-$EMU/qemu-10.0.13}
export OSXKVM=${OSXKVM:-$EMU/OSX-KVM}
KEXT=${KEXT:-$HERE/build/RDNA4FB.kext}
LILU=${LILU:-$EMU/kexts/Lilu.kext}
BASE_IMG=${BASE_IMG:-$EMU/images/BaseSystem.img}
export QEMU_BIN=${QEMU_BIN:-$QEMU_SRC/build/qemu-system-x86_64}   # e.g. $EMU/bin/w13/qemu-system-x86_64
export PATH="$QEMU_SRC/build:$EMU/local/bin:$PATH"   # qemu-img, mcopy/mdeltree

WAIT=420 EXTRA="" ARGS="" DEV="" KEEP=0 BUILD=1 BOOT="" DIAG=0 CENSUS=0 SLEEPRESET=0 PRE=""
while [ $# -gt 0 ]; do
	case $1 in
		--wait) WAIT=$2; shift ;;
		--extra) EXTRA=$2; shift ;;
		--args) ARGS=$2; shift ;;
		--dev) DEV=$2; shift ;;
		--keep) KEEP=1 ;;
		--diag) DIAG=1 ;;
		--census) CENSUS=1 ;;
		--sleep-reset) SLEEPRESET=1 ;;
		--pre) PRE=$2; shift ;;
		--no-build) BUILD=0 ;;
		-h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
		[0-9]*) BOOT=$1 ;;   # a boot of set-boot.sh: 0-13, 9b ...
		*) echo "unknown argument: $1 (try -h)" >&2; exit 2 ;;
	esac
	shift
done
[ "$DIAG$CENSUS" != 11 ] || { echo "--diag and --census cannot be combined" >&2; exit 2; }
[ -n "$BOOT" ] || [ -n "$ARGS" ] || { echo "usage: $0 <boot> [options] (try -h)" >&2; exit 2; }

for f in "$QEMU_BIN" "$HERE/build-emu/rdna4.rom" "$KEXT/Contents/MacOS/RDNA4FB" \
	"$LILU/Contents/Info.plist" "$BASE_IMG" "$OSXKVM/OpenCore/OpenCore.qcow2" "$EMU/local/bin/mcopy"; do
	[ -e "$f" ] || { echo "emu-linux: missing $f (docs/emu-linux.md says how to make it)" >&2; exit 1; }
done
[ -w /dev/kvm ] || { echo "emu-linux: /dev/kvm is not writable" >&2; exit 1; }

# The boot-args of boot N, read from the real thing: set-boot.sh run against a
# scratch config (it prints "  <args>" on its second line).
if [ -z "$ARGS" ]; then
	T=$(mktemp -d)
	mkdir -p "$T/EFI/OC"
	printf '<plist><dict><key>boot-args</key>\n<string>x</string></dict></plist>\n' > "$T/EFI/OC/config.plist"
	cp "$HERE/tools/set-boot.sh" "$T/"
	ARGS=$(bash "$T/set-boot.sh" "$BOOT" 2>/dev/null | sed -n 2p | sed 's/^ *//')
	rm -rf "$T"
	[ -n "$ARGS" ] || { echo "emu-linux: could not read boot $BOOT's arguments" >&2; exit 1; }
fi
# The serial kernel log is how the run is observed.
case " $ARGS " in *" serial="*) ;; *) ARGS="$ARGS serial=3" ;; esac
[ -z "$EXTRA" ] || ARGS="$ARGS $EXTRA"

RUN=$EMU/runs/$(date +%Y%m%d-%H%M%S)-boot${BOOT:-custom}${TAG:+-$TAG}
mkdir -p "$RUN"
SERIAL=$RUN/serial.log
MON=$RUN/monitor.sock
OCIMG=OpenCore-emu-boot${BOOT:-custom}.qcow2
say() { echo "emu-linux: $*" | tee -a "$RUN/summary.txt"; }
say "boot ${BOOT:-custom}, kext $(sha256sum "$KEXT/Contents/MacOS/RDNA4FB" | cut -c1-12) ($(git -C "$HERE" rev-parse --short HEAD))"
say "boot-args: $ARGS"

if [ "$BUILD" = 1 ] || [ ! -f "$OSXKVM/OpenCore/$OCIMG" ]; then
	bash "$HERE/tools/vm-opencore.sh" --kext "$KEXT" --lilu "$LILU" --out "$OCIMG" --args "$ARGS" \
		> "$RUN/opencore.out" 2>&1 || { tail -20 "$RUN/opencore.out"; exit 1; }
fi

QPAT="^$EMU/.*/qemu-system-x86_64 "   # only our own QEMU builds: never another QEMU on the box (e.g. the Android emulator)
pkill -f "$QPAT" 2>/dev/null || true   # never two VMs at once
for i in $(seq 1 20); do pgrep -f "$QPAT" >/dev/null || break; sleep 0.5; done
cp "$OSXKVM/OVMF_VARS-1920x1080.fd" "$RUN/vars.fd"
if [ "$DIAG" = 1 ]; then
	[ -f "$HERE/build/rdna4-run" ] || { echo "emu-linux: --diag needs build/rdna4-run (tools/build-osxcross.sh)" >&2; exit 1; }
	mkdir -p "$RUN/share"
	cp "$HERE/tools/diagnostic-log.sh" "$HERE/build/rdna4-run" "$RUN/share/"
	# Run from /tmp (the disk is read-only) and hand the output to the serial console.
	cat > "$RUN/share/run-diag.sh" <<'GUEST'
cp /Volumes/QEMU*/diagnostic-log.sh /Volumes/QEMU*/rdna4-run /tmp/ && cd /tmp || exit 1
echo RDNA4DIAG-RUNNING > /dev/console
#PRE
bash ./diagnostic-log.sh > /tmp/diag.out 2>&1
(echo RDNA4DIAG-BEGIN; cat /tmp/diag.out; echo RDNA4DIAG-FULL; cat /tmp/rdna4fb-diag-*.txt; echo RDNA4DIAG-END) |
	sed 's/^/RDNA4DIAG|/' > /dev/console
GUEST
	if [ -n "$PRE" ]; then
		python3 - "$RUN/share/run-diag.sh" "$PRE" <<'PY'
import sys
p, pre = sys.argv[1], sys.argv[2]
s = open(p).read().replace("#PRE\n", pre + "\necho RDNA4DIAG-PRE-DONE > /dev/console\n", 1)
open(p, "w").write(s)
PY
	fi
	export EXTRA_QEMU="${EXTRA_QEMU:-} -drive id=share,if=none,format=raw,readonly=on,file=fat:ro:$RUN/share -device usb-storage,bus=xhci.0,drive=share"
fi
if [ "$CENSUS" = 1 ]; then
	[ -f "$HERE/build/rdna4-census" ] || { echo "emu-linux: --census needs build/rdna4-census (tools/build-osxcross.sh build/rdna4-census)" >&2; exit 1; }
	mkdir -p "$RUN/share"
	cp "$HERE/build/rdna4-census" "$RUN/share/"
	cat > "$RUN/share/run-census.sh" <<'GUEST'
cp /Volumes/QEMU*/rdna4-census /tmp/ && cd /tmp || exit 1
echo RDNA4CENSUS-RUNNING > /dev/console
./rdna4-census all > /tmp/census.out 2>&1
(log show --last 10m --style compact --predicate 'process == "rdna4-census" OR eventMessage CONTAINS[c] "MTL" OR eventMessage CONTAINS[c] "IOAccel" OR eventMessage CONTAINS[c] "plugin"' > /tmp/census.log 2>&1 &)
for i in $(seq 1 60); do sleep 1; pgrep -x log >/dev/null || break; done
pkill -x log 2>/dev/null
ioreg -l -w0 -c IOAccelerator > /tmp/census.ioreg 2>&1
(echo RDNA4CENSUS-BEGIN; cat /tmp/census.out; echo RDNA4CENSUS-IOREG; cat /tmp/census.ioreg; echo RDNA4CENSUS-LOG; head -400 /tmp/census.log; echo RDNA4CENSUS-END) |
	sed 's/^/RDNA4CENSUS|/' > /dev/console
GUEST
	export EXTRA_QEMU="${EXTRA_QEMU:-} -drive id=share,if=none,format=raw,readonly=on,file=fat:ro:$RUN/share -device usb-storage,bus=xhci.0,drive=share"
fi
: > "$SERIAL"
OC_IMAGE=$OCIMG RAM_MB=${RAM_MB:-8192} MONITOR=$MON SERIAL_LOG=$SERIAL BASE_IMG=$BASE_IMG MACHDD=none \
	OVMF_CODE=$OSXKVM/OVMF_CODE_4M.fd OVMF_VARS=$RUN/vars.fd VNC_DISPLAY=${VNC_DISPLAY:-0} \
	setsid nohup bash "$HERE/tools/emu-boot.sh" ${DEV:+"$DEV"} > "$RUN/qemu.out" 2>&1 < /dev/null &
QPID=$!
# Byte offsets of qemu.out and serial.log every 0.1 s: tools/emu-context.py uses them to
# show which kext lines an emulator message came after.
( while kill -0 "$QPID" 2>/dev/null; do
	echo "$(stat -c %s "$RUN/qemu.out" 2>/dev/null || echo 0) $(stat -c %s "$SERIAL" 2>/dev/null || echo 0)"
	sleep 0.1
done > "$RUN/offsets.txt" ) > /dev/null 2>&1 9>&- &
cleanup() { [ "$KEEP" = 1 ] || { pkill -f "$QPAT.*$MON" 2>/dev/null || true; }; }
trap cleanup EXIT

mon() { python3 - "$MON" "$@" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(0.3); s.recv(65536)
for c in sys.argv[2:]:
    s.send((c + "\n").encode()); time.sleep(0.6 if c.startswith("sendkey") else 0.2)
time.sleep(0.3)
PY
}
if [ "$SLEEPRESET" = 1 ]; then
	( for i in $(seq 1 1800); do
		if grep -aq "power: quiesce complete" "$SERIAL" 2>/dev/null; then
			mon "qom-set /machine/peripheral/rdna4 sleep-reset true" > /dev/null 2>&1
			echo "sleep-reset triggered $(date +%T)" >> "$RUN/summary.txt"
			break
		fi
		sleep 0.3
	done ) 9>&- &
fi
shot() {   # shot <name>: screendump -> PNG (QEMU writes PPM)
	mon "screendump $RUN/screen-$1.ppm" >/dev/null 2>&1 || return 0
	python3 - "$RUN/screen-$1.ppm" <<'PY' 2>/dev/null || true
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
raw = b''.join(b'\0' + px[y*w*3:(y+1)*w*3] for y in range(h))
def ch(t, b): c = struct.pack('>I', len(b)) + t + b; return c + struct.pack('>I', zlib.crc32(t + b))
open(sys.argv[1][:-4] + '.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
PY
	rm -f "$RUN/screen-$1.ppm"
}

for i in $(seq 1 30); do [ -S "$MON" ] && break; sleep 1; done
[ -S "$MON" ] && sleep 1 && kill -0 "$QPID" 2>/dev/null || { grep -v pcid "$RUN/qemu.out" | tail -5; say "QEMU did not start"; exit 1; }
START=$(date +%s)
# OpenCore's picker (Timeout 0) waits for a key. With the two entries of this
# setup (EFI, macOS Base System) the Recovery disk is one to the right.
for i in $(seq 1 90); do grep -aq "BdsDxe: starting" "$SERIAL" 2>/dev/null && break; sleep 1; done
sleep "${PICKER_DELAY:-14}"
shot picker
tile() {   # efi | base | none: which OpenCore picker tile is highlighted (the grey box behind it)
	mon "screendump $RUN/pick.ppm" >/dev/null 2>&1
	python3 - "$RUN/pick.ppm" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
g = lambda x, y: px[(y * w + x) * 3]
e, b = g(820, 470), g(972, 470)
print("efi" if e > 60 and b < 30 else "base" if b > 60 and e < 30 else "none")
PY
	rm -f "$RUN/pick.ppm"
}
# The picker sometimes drops a key (seen: both presses lost once in ten runs), and "right" from
# the Recovery tile wraps to EFI, which relaunches OpenCore: so look before every key press.
for try in 1 2 3 4 5 6; do
	t=$(tile)
	case $t in
		base) mon "sendkey ret" ;;
		efi) mon ${PICKER_KEYS:-"sendkey right" "sendkey ret"} ;;
		*) sleep 5; continue ;;
	esac
	say "picker ($t selected) answered after $(( $(date +%s) - START )) s"
	for i in $(seq 1 20); do grep -aq "Darwin Kernel" "$SERIAL" && break 2; sleep 2; done
	say "no kernel yet, looking at the picker again"; shot picker$try
done

# Done when Recovery is up and the kext has finished (its bring-up line or a
# failure), plus a grace period for late lines; or on panic / the time limit.
REC=0 DONE_AT=""
while [ $(( $(date +%s) - START )) -lt "$WAIT" ]; do
	sleep 3
	grep -aqiE 'panic\(cpu' "$SERIAL" && { say "GUEST PANIC"; break; }
	if [ "$REC" = 0 ] && grep -aqE "Recovery Spring|RecoveryOS Agen|Language Choose" "$SERIAL"; then
		REC=1; say "Recovery userland up after $(( $(date +%s) - START )) s"; shot recovery
	fi
	if [ -z "$DONE_AT" ] && [ "$REC" = 1 ] &&
		grep -aqE "RDNA4FB: (compute: (bring-up finished|.*failed; stopping)|accelcensus: IOAccelerator service published)" "$SERIAL"; then
		DONE_AT=$(date +%s)
	fi
	[ -n "$DONE_AT" ] && [ $(( $(date +%s) - DONE_AT )) -ge "${GRACE_S:-45}" ] && break
done
[ "$REC" = 1 ] || say "Recovery NOT reached"
if [ "$DIAG" = 1 ] && [ "$REC" = 1 ]; then
	mon "sendkey meta_l-shift-t"
	sleep 5
	shot terminal
	python3 "$HERE/tools/emu-type.py" "$MON" 'bash /Volumes/QEMU*/run-diag.sh\n'
	D0=$(date +%s)
	while [ $(( $(date +%s) - D0 )) -lt "${DIAG_WAIT:-900}" ]; do
		sleep 3
		grep -aq "RDNA4DIAG|RDNA4DIAG-END" "$SERIAL" && break
		grep -aqiE 'panic\(cpu' "$SERIAL" && { say "GUEST PANIC during the diagnostic"; break; }
	done
	sleep 2
	shot diag
	if grep -aq "RDNA4DIAG|RDNA4DIAG-END" "$SERIAL"; then
		say "diagnostic finished after $(( $(date +%s) - D0 )) s"
		sed -n 's/^.*RDNA4DIAG|//p' "$SERIAL" | tr -d '\r' > "$RUN/diag.txt"
		sed -n '/^RDNA4DIAG-BEGIN/,/^RDNA4DIAG-FULL/p' "$RUN/diag.txt" | sed '1d;$d' > "$RUN/diag-summary.txt"
		{ echo "== diagnostic-log.sh summary (guest)"; sed -n '/^Feature summary:/,/^Key log sections/p' "$RUN/diag-summary.txt"; } | tee -a "$RUN/summary.txt"
	else
		say "the diagnostic did not finish in ${DIAG_WAIT:-900} s (see screen-diag.png)"
	fi
fi
if [ "$CENSUS" = 1 ] && [ "$REC" = 1 ]; then
	mon "sendkey meta_l-shift-t"
	sleep 5
	shot terminal
	python3 "$HERE/tools/emu-type.py" "$MON" 'bash /Volumes/QEMU*/run-census.sh\n'
	D0=$(date +%s)
	while [ $(( $(date +%s) - D0 )) -lt "${CENSUS_WAIT:-300}" ]; do
		sleep 3
		grep -aq "RDNA4CENSUS|RDNA4CENSUS-END" "$SERIAL" && break
		grep -aqiE 'panic\(cpu' "$SERIAL" && { say "GUEST PANIC during the census"; break; }
	done
	sleep 2
	shot census
	if grep -aq "RDNA4CENSUS|RDNA4CENSUS-END" "$SERIAL"; then
		say "census finished after $(( $(date +%s) - D0 )) s"
		sed -n 's/^.*RDNA4CENSUS|//p' "$SERIAL" | tr -d '\r' > "$RUN/census.txt"
	else
		say "the census tool did not finish in ${CENSUS_WAIT:-300} s (see screen-census.png); partial output in census.txt"
		sed -n 's/^.*RDNA4CENSUS|//p' "$SERIAL" | tr -d '\r' > "$RUN/census.txt" || true
	fi
	grep -a "accelcensus" "$SERIAL" | sed 's/^.*RDNA4FB: //' | tr -d '\r' > "$RUN/census-kernel.log" || true
	say "$(wc -l < "$RUN/census-kernel.log") kernel census lines -> census-kernel.log"
fi
LAST=$(stat -c %s "$SERIAL")
say "kernel log ${LAST} bytes after $(( $(date +%s) - START )) s"
shot final
grep -a "RDNA4FB" "$SERIAL" | grep -av "RDNA4DIAG|" > "$RUN/rdna4fb.log" || true
say "$(wc -l < "$RUN/rdna4fb.log") RDNA4FB lines -> $RUN/rdna4fb.log"
# The headline results the kext itself prints (user-space rows such as "vm PASS"
# come from diagnostic-log.sh, not from the kernel log).
{
	echo "== verdicts from the kernel log"
	grep -aE "compute: bring-up finished|runtime: user-space runtime up|THE TRIANGLE IS RIGHT|gfx: draw: .*(EMPTY|wrong|empty)|gfx: col: (wrong|THE|target still)|ih: (self-test totals|page flip|vblank self-test passed)" "$RUN/rdna4fb.log" |
		sed 's/^.*RDNA4FB: /RDNA4FB: /' | cut -c1-230 | awk '!seen[$0]++'
} | tee -a "$RUN/summary.txt"
# What the emulator itself said (stderr of QEMU, without the host-CPU feature noise).
grep -av "host doesn't support requested feature" "$RUN/qemu.out" > "$RUN/emu-warnings.txt" || true
python3 "$HERE/tools/emu-context.py" "$RUN" > "$RUN/emu-context.txt" 2>/dev/null || true
say "emulator: $QEMU_BIN, $(wc -l < "$RUN/emu-warnings.txt") message lines -> emu-warnings.txt, emu-context.txt"
echo "$RUN"
