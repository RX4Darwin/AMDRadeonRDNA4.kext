#!/bin/sh
# rdna4raster.c -> rdna4raster.ll, the rasterizer half of the compiler service's fragment kernels (air2amdgcn fragment mode). Needs a
# clang with the AMDGPU target (the test Mac's LLVM has none: the .ll is built here and checked in, as the generated tables are).
set -eu
cd "$(dirname "$0")"
clang -x c -std=c11 -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib -O2 -ffp-contract=fast \
	-emit-llvm -S rdna4raster.c -o rdna4raster.ll
# The generated kernel inlines these; they need no symbol of their own once linked.
echo "wrote tools/metal/air/rdna4raster.ll ($(grep -c '^define' rdna4raster.ll) functions)"
