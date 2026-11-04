#!/bin/sh
# A Metal client's compute function -> a gfx1201 code object, for the paravirtual host's compiler service (rdna4-pvexec).
#   tools/metal/air/compile-metallib.sh IN.metallib TX,TY,TZ OUTDIR
# IN is the metallib the guest sends (one AIR module, docs/m0-pvgpu.md); TX,TY,TZ the threadgroup size it is dispatched with (air2amdgcn
# compiles it in). Writes OUTDIR/kernel.hsaco, OUTDIR/kernel.abi (air2amdgcn --abi) and OUTDIR/kernel.name (the entry point).
# Needs air2amdgcn ($AIR2AMDGCN, else Build/air/air2amdgcn), and llc and ld.lld of an LLVM with the AMDGPU target on PATH (or in
# $LLVM/bin); metallib.py beside this script.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." 2> /dev/null && pwd)
in=$1 tg=$2 out=$3
[ -n "${LLVM:-}" ] && PATH=$LLVM/bin:$PATH
mkdir -p "$out/air"
rm -f "$out"/air/*.bc

python3 "$here/metallib.py" extract "$in" "$out/air" > /dev/null
set -- "$out"/air/*.bc
if [ $# -ne 1 ] || [ ! -f "$1" ]; then
	echo "compile-metallib: $in: expected one AIR module" >&2
	exit 1
fi

# extract names a module NNNN_<function>.bc
name=$(basename "$1" .bc)
name=${name#*_}
"${AIR2AMDGCN:-$repo/Build/air/air2amdgcn}" kernel "$1" --entry "$name" --tg "$tg" -o "$out/kernel.ll" --abi "$out/kernel.abi"
llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx1201 -O2 -filetype=obj "$out/kernel.ll" -o "$out/kernel.o"
ld.lld -shared "$out/kernel.o" -o "$out/kernel.hsaco"
printf '%s\n' "$name" > "$out/kernel.name"
