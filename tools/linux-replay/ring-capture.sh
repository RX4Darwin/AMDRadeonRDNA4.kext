#!/bin/bash
# W12k ground truth: what amdgpu writes on the gfx ring around a user gfx IB on this card
# (VM flush, syncs, INDIRECT_BUFFER with VMID, fences). Read-only: runs the replay as the
# invoking user, then copies the gfx rings from debugfs and decodes the packets around our IB.
#   sudo bash tools/linux-replay/ring-capture.sh [outdir]
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo (debugfs)"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
USER_=${SUDO_USER:-root}
OUT=${1:-$HERE/ring-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
DRI=
for d in /sys/kernel/debug/dri/*/; do
	grep -q 'amdgpu' "$d/name" 2>/dev/null && [ -e "$d/amdgpu_ring_gfx_0.0.0" ] && { DRI=$d; break; }
done
[ -n "$DRI" ] || { echo "no amdgpu debugfs dir (lockdown? debugfs not mounted?)"; exit 1; }
echo "debugfs: $DRI"
[ -x "$HERE/build/replay" ] || sudo -u "$USER_" bash "$HERE/run.sh" >/dev/null || true
sudo -u "$USER_" "$HERE/build/replay" > "$OUT/replay.txt" 2>&1 || true
for r in "$DRI"/amdgpu_ring_gfx*; do cp "$r" "$OUT/$(basename "$r").bin"; done
cp "$DRI/name" "$OUT/name.txt"; cat "$DRI/amdgpu_fence_info" > "$OUT/fence_info.txt" 2>/dev/null || true
chown -R "$USER_" "$OUT"
cat "$OUT/replay.txt" | grep -E 'IB VA|pixels'
python3 "$HERE/ring-decode.py" "$OUT" | tee "$OUT/decoded.txt"
echo "saved in $OUT"
