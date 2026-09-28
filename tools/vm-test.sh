#!/usr/bin/env bash
# The emulator regression, for any checkout (worktree) of this repo, in WSL:
# build this checkout's kext, rdna4-run and QEMU rdna4 device, build the
# OpenCore image, boot the Tahoe VM on the emulated RX 9070 XT, and run
# `rdna4-run selftest` and `rdna4-run bench small` in it over SSH.
#
#   tools/vm-test.sh [extra boot-args...]     e.g. tools/vm-test.sh rdna4-ih=1
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
exec 9>"$HOME/rdna4-vm.lock"
echo "vm-test: waiting for the VM lock (another checkout may be testing)..."
flock 9
echo "vm-test: $REPO ($(git rev-parse --short HEAD 2>/dev/null || echo '?'))"

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
tools/emu-build.sh qemu 2>&1 | grep -E 'error|warning|built' | head -20
tools/vm-opencore.sh --kext build/RDNA4FB.kext --lilu ~/kexts/Lilu.kext \
	--out OpenCore-emu.qcow2 --args "$ARGS" 2>&1 | tail -1

# 2. Boot, with the display viewable in a browser.
setsid nohup python3 -m http.server 6080 --bind 127.0.0.1 --directory ~/noVNC \
	> ~/novnc-http.log 2>&1 < /dev/null &
: > ~/tahoe-serial.log
VNC_WS=127.0.0.1:5700 setsid nohup tools/emu-boot.sh > ~/emu-boot.out 2>&1 < /dev/null &
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
