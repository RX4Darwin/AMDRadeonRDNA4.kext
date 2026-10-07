#!/bin/bash
# Build tools/linux-replay and run the kext's G3 draw on the card through amdgpu (no root).
#   tools/linux-replay/run.sh [REPLAY_VS=old|new] [REPLAY_VARIANT=n]   (env, see replay.cpp)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-$HERE/build}
mkdir -p "$OUT"
# The ngg.s the card ran in rounds 5/6 (prim export first), before premetal/radv-order.
git -C "$ROOT" show edc5f81:src/ngg_kernel.h | sed 's/kNggKernel/kNggOldKernel/' > "$OUT/ngg_old_kernel.h"
c++ -std=c++17 -O1 -g -Wall -I"$ROOT/userspace" -I"$ROOT/src" -I"$OUT" $(pkg-config --cflags libdrm_amdgpu) \
	-o "$OUT/replay" "$HERE/replay.cpp" $(pkg-config --libs libdrm_amdgpu) -ldrm
exec timeout 30 "$OUT/replay"   # REPLAY_VS=old|new|file:<hex>, REPLAY_SET, REPLAY_VARIANT, REPLAY_DUMP_MARKER
