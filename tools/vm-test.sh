#!/usr/bin/env bash
# The emulator regression, for any checkout (worktree) of this repo, in WSL:
# build this checkout's kext, rdna4-run and QEMU rdna4 device (any build failure aborts
# with a message and a non-zero exit BEFORE queueing; the log starts with a build stamp:
# git HEAD, kext/rdna4-run mtime + sha256), build the
# OpenCore image, boot the Tahoe VM on the emulated RX 9070 XT, and run
# `rdna4-run selftest` and `rdna4-run bench small` in it over SSH.
#
#   tools/vm-test.sh [extra boot-args...]     e.g. tools/vm-test.sh rdna4-ih=1
#   RDNA4_DEV=ih-dead=on,flip-stuck=on tools/vm-test.sh ...
#   RDNA4_POST='...bash...' tools/vm-test.sh    extra steps on the same boot, under the lock
#   RDNA4_STEP_MAX=600 tools/vm-test.sh     cap each guest step (default 1800 s); a
#                                             guest panic stops the run at once
#                                             emulated-card options (the rdna4
#                                             device's fault switches), comma-separated
#
# The VM, its ports, the QEMU build tree (~/qemu-10.0.13) and the OpenCore
# image are shared by every checkout, so the whole run holds
# ~/rdna4-vm.lock: parallel runs wait their turn. The VM is left running
# afterwards (VNC on 127.0.0.1:5900, noVNC http://localhost:6080/vnc.html?
# host=localhost&port=5700&path=&autoconnect=1&resize=scale).
# Logs: ~/tahoe-serial.log (kernel), ~/emu-boot.out (QEMU, emulator messages).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
die() { echo "vm-test: FAILED: $*" >&2; exit 1; }

# ---- Which build is this? ---------------------------------------------------
# WSL's git cannot read a worktree made by Windows git (its .git file points at
# a C:/ path), so use whichever git works here: the WSL one, else Windows git.exe
# through interop, else read HEAD out of the gitdir by hand.
GIT_BIN=""
for g in git "/mnt/c/Program Files/Git/cmd/git.exe" "/mnt/c/Program Files/Git/bin/git.exe"; do
	if command -v "$g" >/dev/null 2>&1 && "$g" rev-parse --git-dir >/dev/null 2>&1; then GIT_BIN=$g; break; fi
done
repo_stamp() {   # "<short hash> (<branch>, <n> modified tracked files)"
	local h b n
	if [ -n "$GIT_BIN" ]; then
		h=$("$GIT_BIN" rev-parse --short HEAD 2>/dev/null | tr -d '\r') || h=""
		b=$("$GIT_BIN" rev-parse --abbrev-ref HEAD 2>/dev/null | tr -d '\r') || b=""
		n=$("$GIT_BIN" status --porcelain 2>/dev/null | grep -vc '^??' || true)
		echo "${h:-?} (${b:-?}, ${n:-?} modified tracked files)"
		return
	fi
	# Last resort: HEAD of the worktree's gitdir, resolved by hand.
	local gd ref cd2
	gd=$(sed -n 's/^gitdir: //p' .git 2>/dev/null | sed 's|^\([A-Za-z]\):|/mnt/\L\1|; s|\|/|g')
	[ -n "$gd" ] && [ -f "$gd/HEAD" ] || { echo "? (no usable git)"; return; }
	ref=$(sed -n 's/^ref: //p' "$gd/HEAD")
	if [ -z "$ref" ]; then echo "$(cut -c1-7 "$gd/HEAD") (detached, unknown state)"; return; fi
	cd2=$gd; [ -f "$gd/commondir" ] && cd2=$(cd "$gd" && cd "$(cat commondir)" && pwd)
	h=$(cat "$cd2/$ref" 2>/dev/null || sed -n "s|^\([0-9a-f]*\) $ref\$|\1|p" "$cd2/packed-refs" 2>/dev/null)
	echo "$(echo "$h" | cut -c1-7) (${ref#refs/heads/}, working tree state unknown)"
}
stamp_file() {   # "<mtime> sha256 <hash> <size>" of a file or directory tree's main binary
	local p=$1
	[ -f "$p" ] || return 1
	echo "$(date -d "@$(stat -c %Y "$p")" '+%F %T') sha256 $(sha256sum "$p" | cut -c1-16) $(stat -c %s "$p") bytes"
}

# ---- 1. Build first, before queueing ----------------------------------------
# A build failure must never boot the previous build (the kext and rdna4-run
# used to be built behind `| grep ... || true`, so a compile error silently ran
# the old binary), and must not take a VM slot: everything private to this
# checkout is built and checked here; only the shared QEMU tree waits for the lock.
BUILD_LOG=$(mktemp /tmp/vm-test-build.XXXXXX)
SNAP=$(mktemp -d /tmp/vm-test-snap.XXXXXX)
BUILD_START=$(date +%s)
echo "vm-test: building the host tests, the kext and rdna4-run first (a failure here never takes a VM slot)..."
make test >"$BUILD_LOG" 2>&1 || { tail -30 "$BUILD_LOG"; die "make test failed (host unit tests)"; }
tail -2 "$BUILD_LOG"
# No stale artefact may survive a failed build.
rm -rf build/RDNA4FB.kext build/rdna4-run
tools/build-osxcross.sh >"$BUILD_LOG" 2>&1 || {
	grep -E 'error|Error' "$BUILD_LOG" | head -30; tail -5 "$BUILD_LOG"
	die "the kext / rdna4-run build failed (tools/build-osxcross.sh returned non-zero)"
}
grep -E 'error:|warning: [^o]|Built' "$BUILD_LOG" || true
KEXT_BIN=build/RDNA4FB.kext/Contents/MacOS/RDNA4FB
[ -f "$KEXT_BIN" ] || die "the build reported success but there is no $KEXT_BIN"
[ -x build/rdna4-run ] || die "the build reported success but there is no build/rdna4-run"
for p in "$KEXT_BIN" build/rdna4-run; do
	[ "$(stat -c %Y "$p")" -ge "$BUILD_START" ] || die "$p is older than this run's build (stale artefact)"
done
# The kext links even with undefined symbols of ours and then fails to load in the guest
# (that is how a missing function once made every dry run bail out silently): refuse it here.
NM=$(ls "$HOME"/osxcross/target/bin/x86_64-apple-darwin*-nm 2>/dev/null | head -1 || true)
if [ -n "$NM" ]; then
	UNDEF=$("$NM" -u "$KEXT_BIN" 2>/dev/null | grep -E '__ZN[K]?[0-9]+RDNA4|__ZN[0-9]+RDNA4|rdna4_' || true)
	[ -z "$UNDEF" ] || { echo "$UNDEF" | head; die "the kext has undefined RDNA4FB symbols (it would not load in the guest)"; }
fi
# Snapshot what will boot: a rebuild of this checkout while the run waits in the queue
# must not change it.
cp -a build/RDNA4FB.kext "$SNAP/RDNA4FB.kext"
cp -a build/rdna4-run "$SNAP/rdna4-run"
SNAP_KEXT="$SNAP/RDNA4FB.kext/Contents/MacOS/RDNA4FB"
STAMP_HEAD=$(repo_stamp)
echo "vm-test: build stamp: git HEAD $STAMP_HEAD"
echo "vm-test: build stamp: kext      $(stamp_file "$SNAP_KEXT")"
echo "vm-test: build stamp: rdna4-run $(stamp_file "$SNAP/rdna4-run")"

# First come, first served: flock alone is not fair, and a checkout that
# re-runs quickly could starve the others. Each run takes a ticket
# (arrival time + pid) in ~/rdna4-vm.queue and waits until it is the oldest
# live one; tickets of runs that died are dropped. The lock stays as the
# guard around the VM itself.
QUEUE=$HOME/rdna4-vm.queue
mkdir -p "$QUEUE"
TICKET=$(date +%s%N)-$$
touch "$QUEUE/$TICKET"
trap 'rm -f "$QUEUE/$TICKET" "$BUILD_LOG"; rm -rf "$SNAP"' EXIT
echo "vm-test: queued as $TICKET (other checkouts may be testing)..."
while :; do
	first=""
	for t in $(ls "$QUEUE" | sort); do
		if kill -0 "${t##*-}" 2>/dev/null; then first=$t; break; fi
		rm -f "$QUEUE/$t"
	done
	[ "$first" = "$TICKET" ] && break
	sleep 2
done
exec 9>"$HOME/rdna4-vm.lock"
flock 9
echo "vm-test: $REPO, git HEAD $STAMP_HEAD, boot-args: $*, device options: ${RDNA4_DEV:-none}"
echo "vm-test: booting kext $(stamp_file "$SNAP_KEXT")"

ARGS="-v keepsyms=1 debug=0x100 serial=3 rdna4-trace=1 rdna4-compute=7 $*"
SSH_KEY=$HOME/.ssh/tahoe_vm
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=20 -i $SSH_KEY -p 10022 miguer@127.0.0.1"

pkill -9 -f qemu-system-x86_64 || true
# ...and any other QEMU on the macOS disk, whatever its binary is called: a
# VM started outside this script would hold the image and block every run.
pkill -9 -f 'mac_hdd_ng[.]img' || true
pkill -f 'http.server 6080' || true
sleep 1

# 2. The emulator (the QEMU tree is shared, so this waits for the lock) and the OpenCore image.
# The QEMU build tree links whichever checkout's rdna4.c ran last; make sure this
# checkout's is what gets compiled, whatever the mtimes.
touch emu/qemu/rdna4.c
tools/emu-build.sh qemu >"$BUILD_LOG" 2>&1 || {
	grep -E 'error|Error' "$BUILD_LOG" | head -30; tail -5 "$BUILD_LOG"
	die "the emulator build failed (tools/emu-build.sh qemu returned non-zero)"
}
grep -E 'error|warning|built' "$BUILD_LOG" | head -20 || true
# A failed build must not boot the previous binary.
QEMU_BIN=${QEMU_SRC:-$HOME/qemu-10.0.13}/build/qemu-system-x86_64
[ "$QEMU_BIN" -nt emu/qemu/rdna4.c ] || die "the emulator did not build (see the errors above)"
echo "vm-test: emulator   $(stamp_file "$QEMU_BIN")"
# The option ROM (GOP driver + VBIOS image, build-emu/, not in git) is the
# same for every checkout: a worktree without one takes the main checkout's.
if [ ! -f build-emu/rdna4.rom ]; then
	MAIN=$(dirname "$REPO")/RDNA4FB     # worktrees sit next to the main checkout
	if [ -f "$MAIN/build-emu/rdna4.rom" ]; then
		mkdir -p build-emu && cp "$MAIN"/build-emu/* build-emu/
		echo "vm-test: option ROM copied from $MAIN/build-emu"
	else
		tools/emu-build.sh rom 2>&1 | tail -2
	fi
fi
[ -f build-emu/rdna4.rom ] || { echo "vm-test: no build-emu/rdna4.rom"; exit 1; }
tools/vm-opencore.sh --kext "$SNAP/RDNA4FB.kext" --lilu ~/kexts/Lilu.kext \
	--out OpenCore-emu.qcow2 --args "$ARGS" >"$BUILD_LOG" 2>&1 ||
	{ tail -20 "$BUILD_LOG"; die "the OpenCore image build failed"; }
tail -1 "$BUILD_LOG"

# 2. Boot, with the display viewable in a browser.
setsid nohup python3 -m http.server 6080 --bind 127.0.0.1 --directory ~/noVNC \
	> ~/novnc-http.log 2>&1 < /dev/null 9>&- &
: > ~/tahoe-serial.log
# (9>&-: the VM outlives this run and must not keep the lock.)
VNC_WS=127.0.0.1:5700 setsid nohup tools/emu-boot.sh ${RDNA4_DEV:+"$RDNA4_DEV"} \
	> ~/emu-boot.out 2>&1 < /dev/null 9>&- &
pick() {        # the OpenCore picker: the macOS entry
	python3 - <<'PY'
import socket, time, os
s = socket.socket(socket.AF_UNIX); s.connect(os.path.expanduser('~/tahoe-monitor.sock'))
time.sleep(0.3); s.recv(4096)
for k in ['right', 'right', 'ret']:
    s.send(('sendkey %s\n' % k).encode()); time.sleep(0.6)
PY
}
for i in $(seq 1 60); do grep -aq "BdsDxe: starting" ~/tahoe-serial.log 2>/dev/null && break; sleep 1; done
sleep 12
pick
# Keys sent too early are lost: if the kernel has not started, pick again.
for i in $(seq 1 30); do grep -aq "Darwin Kernel\|RDNA4FB" ~/tahoe-serial.log && break; sleep 2; done
grep -aq "Darwin Kernel\|RDNA4FB" ~/tahoe-serial.log || { echo "vm-test: picking again"; pick; }

# 3. Wait for the compute bring-up, then test from user space.
for i in $(seq 1 200); do
	grep -aqE "compute: (bring-up finished|.*failed; stopping)|panic|Kernel trap" ~/tahoe-serial.log && break
	sleep 2
done
grep -aE "RDNA4FB: (compute: (bring-up finished|stage [0-9])|runtime: user-space)|panic" \
	~/tahoe-serial.log | tail -3
# A guest that panics leaves an SSH session to it hanging forever (09:20: a
# run sat 15 minutes on a panicked VM, holding the lock). So every step in
# the guest runs under a watchdog: it is stopped as soon as the serial log
# shows a panic, or after RDNA4_STEP_MAX seconds (default 1800).
panicked() {    # panicked <what>: fail the run if the guest has panicked
	grep -aqiE 'panic\(cpu' ~/tahoe-serial.log || return 0
	echo "vm-test: the guest PANICKED during $1:"
	grep -aiE 'panic\(cpu' ~/tahoe-serial.log | head -3 | cut -c1-200
	exit 1
}
guarded() {     # guarded <what> <bash command>
	local pid t=0
	setsid bash -c "$2" 9>&- &
	pid=$!
	while kill -0 "$pid" 2>/dev/null; do
		grep -aqiE 'panic\(cpu' ~/tahoe-serial.log &&
			{ kill -- -"$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true; panicked "$1"; }
		if [ "$t" -ge "${RDNA4_STEP_MAX:-1800}" ]; then
			kill -- -"$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
			echo "vm-test: $1 timed out after ${t}s"
			exit 1
		fi
		sleep 2; t=$((t + 2))
	done
	wait "$pid"
}
selftest_bench() {
	$SSH 'cd /tmp; echo "== selftest"; echo 1234 | sudo -S ./rdna4-run selftest 16384 2>&1 | grep -v Password;
	      echo "== bench small"; echo 1234 | sudo -S ./rdna4-run bench small 2>&1 | grep -v Password'
}
export SSH SSH_KEY REPO
export -f selftest_bench
panicked "the boot"
for i in $(seq 1 30); do $SSH true 2>/dev/null && break; sleep 4; done
scp -q -o StrictHostKeyChecking=no -i "$SSH_KEY" -P 10022 "$SNAP/rdna4-run" miguer@127.0.0.1:/tmp/ ||
	{ echo "vm-test: scp failed"; exit 1; }
guarded "selftest/bench" selftest_bench
echo "== emulator"
grep -aE "rdna4: cs: (unsupported|LDS access|dispatch stopped|work-item ran|WMMA)|rdna4: .*(fault|not modelled)" \
	~/emu-boot.out | head -10 || true

# 4. Optional: more steps on the same boot, still holding the lock (nobody
#    can reboot the VM under them). RDNA4_POST is a bash script; it gets
#    SSH (a command prefix to the VM), SSH_KEY, VM_SCP_PORT=10022,
#    MONITOR (the QEMU monitor socket, e.g. for "screendump /tmp/x.ppm")
#    and REPO. Example:
#      RDNA4_POST='$SSH "echo 1234 | sudo -S /tmp/rdna4-run show 3"' tools/vm-test.sh
if [ -n "${RDNA4_POST:-}" ]; then
	echo "== post (RDNA4_POST)"
	export VM_SCP_PORT=10022 MONITOR="$HOME/tahoe-monitor.sock"
	guarded "RDNA4_POST" "$RDNA4_POST"
fi