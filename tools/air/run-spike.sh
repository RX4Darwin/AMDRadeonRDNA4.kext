#!/usr/bin/env bash
# M3 track (b) spike (docs/m3-air-spike.md): build the AIR -> gfx1201 tools, lower two of Apple's own compute kernels, and (only with the reviewer's
# go-ahead: REPLAY_COMPUTE_OK=1) run them on the card and compare with a CPU reference.
#   tools/air/run-spike.sh            host only: build + lower + disassemble, NOTHING runs on a GPU
#   REPLAY_COMPUTE_OK=1 tools/air/run-spike.sh run
# The AIR modules come from the extracted macOS installer (tools/air/disall.py output, outside the repo): AIRDIR=~/work/tools/macos-full/25G83/metallib/air
set -euo pipefail
cd "$(dirname "$0")/../.."
AIRDIR=${AIRDIR:-$HOME/work/tools/macos-full/25G83/metallib/air}
O=build/air
mkdir -p $O/k
tools/air/build-tools.sh
g++ -std=c++17 -O1 -Wall -Isrc -o $O/check-codeobj tools/e3/check-codeobj.cpp src/codeobj.cpp
g++ -std=c++17 -O1 -g -Wall -Iuserspace -Isrc $(pkg-config --cflags libdrm_amdgpu) -o $O/air-run tools/air/air-run.cpp src/codeobj.cpp $(pkg-config --libs libdrm_amdgpu) -ldrm
GU=$(ls -d $AIRDIR/*Photogrammetry_GaussianSplatting_Kernels.metallib | head -1)
RS=$(ls -d $AIRDIR/*CoreRE3DGSFoundation*default.metallib | head -1)
tools/air/build-kernel.sh "$GU/0062_group_uniform_add_float.bc" group_uniform_add_float 64,1,1 $O/k
tools/air/build-kernel.sh "$RS/0009_ReduceSumKernel_uint.bc" ReduceSumKernel_uint 1024,1,1 $O/k
for k in group_uniform_add_float ReduceSumKernel_uint; do echo "== descriptor: $k"; { $O/check-codeobj $O/k/$k.hsaco $O/k/$k.hsaco $k || true; } | sed -n 2,11p; done
if [ "${1:-}" = run ]; then
	exec python3 tools/air/test-kernels.py
fi
echo "host-only: nothing was run on a GPU (REPLAY_COMPUTE_OK=1 $0 run runs the two kernels on the card)"
