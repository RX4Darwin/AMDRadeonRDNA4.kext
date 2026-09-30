#!/bin/bash
# Build radv-triangle and capture RADV's view of the kext's first triangle:
# pixel count + pipeline stats, the shader ISA/config (RADV_DEBUG=shaders)
# and the command stream (RADV_DEBUG=dumpibs). No root needed.
#   tools/radv-triangle/run.sh [outdir]
# Variants: default (RADV's choice), nonggc (NGG culling off = passthrough,
# like shaders/ngg.s).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out-$(date +%Y%m%d-%H%M%S)}
VKH=${VULKAN_HEADERS:-$HOME/work/tools/vulkan-headers/include}
mkdir -p "$OUT"
glslc -o "$OUT/tri.vert.spv" "$HERE/tri.vert"
glslc -o "$OUT/tri.frag.spv" "$HERE/tri.frag"
cc -O1 -g -Wall -I"$VKH" -o "$OUT/radv-triangle" "$HERE/radv-triangle.c" -lvulkan
{ uname -a; pacman -Q mesa linux-firmware 2>/dev/null || true; } > "$OUT/env.txt"
for v in default nonggc; do
	dbg=shaders,dumpibs
	[ "$v" = default ] || dbg="$dbg,$v"
	mkdir -p "$OUT/$v"
	# dumpibs writes into the working directory.
	(cd "$OUT/$v" && RADV_DEBUG=$dbg "$OUT/radv-triangle" "$OUT" > run.txt 2> shaders.raw) || echo "$v: exit $?"
	sed 's/\x1b\[[0-9;]*m//g' "$OUT/$v/shaders.raw" > "$OUT/$v/shaders.txt" && rm "$OUT/$v/shaders.raw"   # drop the colour codes
	head -3 "$OUT/$v/run.txt"
done
echo "results in $OUT"
# Hypothesis tests without rebooting: build Mesa 26.2.2 RADV with
# mesa-26.2.2-force-early-prim.patch (meson -Dvulkan-drivers=amd -Dllvm=disabled ...),
# point VK_ICD_FILENAMES at it and run with RADV_FORCE_EARLY_PRIM=1. TRI_COMPILE_ONLY=1
# only compiles (dumps the ISA) without submitting anything to the GPU.
