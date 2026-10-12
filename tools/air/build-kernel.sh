#!/usr/bin/env bash
# AIR compute kernel -> gfx1201 code object, with the evidence printed.
#   tools/air/build-kernel.sh IN.bc|IN.ll KERNEL TGX,TGY,TGZ OUTDIR
# Nothing runs on a GPU. Needs build/air/air2amdgcn (build/air/build.sh), llc and ld.lld (LLVM 22 with the AMDGPU target), and check-codeobj.
set -euo pipefail
cd "$(dirname "$0")/../.."
in=$1 name=$2 tg=$3 out=$4
mkdir -p "$out"
build/air/air2amdgcn kernel "$in" --entry "$name" --tg "$tg" -o "$out/$name.ll"
llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx1201 -O2 -filetype=obj "$out/$name.ll" -o "$out/$name.o"
ld.lld -shared "$out/$name.o" -o "$out/$name.hsaco"
llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx1201 -O2 "$out/$name.ll" -o "$out/$name.s"
llvm-objdump -d --mcpu=gfx1201 "$out/$name.hsaco" | sed 's/  *\/\/.*//' | sed -n "/<$name>:/,/s_endpgm/p" > "$out/$name.dis"
echo "== $name: $(grep -c . "$out/$name.dis") lines of ISA, $out/$name.hsaco"
