#!/usr/bin/env bash
# The emulator regression, for any checkout (worktree) of this repo, in WSL:
# build this checkout's kext, rdna4-run and QEMU rdna4 device, build the
# OpenCore image, boot the Tahoe VM on the emulated RX 9070 XT, and run
# `rdna4-run selftest` and `rdna4-run bench small` in it over SSH.
#
#   tools/vm-test.sh [extra boot-args...]     e.g. tools/vm-test.sh rdna4-ih=1
#   RDNA4_DEV=ih-dead=on,flip-stuck=on tools/vm-test.sh ...
#   RDNA4_POST='...bash...' tools/vm-test.sh    extra steps on the same boot, under the lock
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
# First come, first served: flock alone is not fair, and a checkout that
# re-runs quickly could starve the others. Each run takes a ticket
# (arrival time + pid) in ~/rdna4-vm.queue and waits until it is the oldest
# live one; tickets of runs that died are dropped. The lock stays as the
# guard around the VM itself.
QUEUE=$HOME/rdna4-vm.queue
mkdir -p "$QUEUE"
TICKET=$(date +%s%N)-$$
touch "$QUEUE/$TICKET"
trap 'rm -f "$QUEUE/$TICKET"' EXIT
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
echo "vm-test: $REPO ($(git rev-parse --short HEAD 2>/dev/null || echo '?')), boot-args: $*, device options: ${RDNA4_DEV:-none}"

ARGS="-v keepsyms=1 debug=0x100 serial=3 rdna4-trace=1 rdna4-compute=7 $*"
SSH_KEY=$HOME/.ssh/tahoe_vm
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=20 -i $SSH_KEY -p 10022 miguer@127.0.0.1"

pkill -9 -f qemu-system-x86_64 || true
pkill -f 'http.server 6080' || true
sleep 1

# 1. Build: host tests, kext (+ rdna4-run), emulator, OpenCore image.
make test 2>&1 | tail -2
tools/build-osxcross.sh 2>&1 | grep -E 'error:|warning: [^o]|Built' || true
[ -d build/RDNA4FB.kext ] || { echo "vm-test: kext build failed"; exit 1; }
# The QEMU build tree is shared and links whichever checkout's rdna4.c ran
# last; make sure this checkout's is what gets compiled, whatever the mtimes.
touch emu/qemu/rdna4.c
tools/emu-build.sh qemu 2>&1 | grep -E 'error|warning|built' | head -20
# A failed build must not boot the previous binary.
QEMU_BIN=${QEMU_SRC:-$HOME/qemu-10.0.13}/build/qemu-system-x86_64
[ "$QEMU_BIN" -nt emu/qemu/rdna4.c ] || { echo "vm-test: the emulator did not build (see the errors above)"; exit 1; }
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
tools/vm-opencore.sh --kext build/RDNA4FB.kext --lilu ~/kexts/Lilu.kext \
	--out OpenCore-emu.qcow2 --args "$ARGS" 2>&1 | tail -1

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
for i in $(seq 1 30); do $SSH true 2>/dev/null && break; sleep 4; done
scp -q -o StrictHostKeyChecking=no -i "$SSH_KEY" -P 10022 build/rdna4-run miguer@127.0.0.1:/tmp/ ||
	{ echo "vm-test: scp failed"; exit 1; }
$SSH 'cd /tmp; echo "== selftest"; echo 1234 | sudo -S ./rdna4-run selftest 16384 2>&1 | grep -v Password;
      echo "== bench small"; echo 1234 | sudo -S ./rdna4-run bench small 2>&1 | grep -v Password'
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
	SSH="$SSH" SSH_KEY="$SSH_KEY" VM_SCP_PORT=10022 MONITOR="$HOME/tahoe-monitor.sock" REPO="$REPO" \
		bash -c "$RDNA4_POST" 9>&-
fi