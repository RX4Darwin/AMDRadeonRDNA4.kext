#!/bin/sh
# A Metal client's render pipeline -> two gfx1201 code objects, for the paravirtual host's compiler service (rdna4-pvexec).
#   tools/metal/air/compile-pipeline.sh VERTEX.metallib FRAGMENT.metallib OUTDIR
# Writes OUTDIR/vertex.{hsaco,abi,name} (the vertex function run once per vertex, air2amdgcn vertex) and OUTDIR/fragment.{hsaco,abi,name}
# (the fragment function inside a kernel that rasterizes the draw, air2amdgcn fragment, linked with rdna4raster.ll beside this script).
# Needs air2amdgcn ($AIR2AMDGCN, else Build/air/air2amdgcn), and llc, ld.lld, llvm-link and opt of an LLVM with the AMDGPU target on PATH
# (or in $LLVM/bin); metallib.py beside this script.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." 2> /dev/null && pwd)
air2amdgcn=${AIR2AMDGCN:-$repo/Build/air/air2amdgcn}
vs=$1 fs=$2 out=$3
[ -n "${LLVM:-}" ] && PATH=$LLVM/bin:$PATH
mkdir -p "$out/vs" "$out/fs"
rm -f "$out"/vs/*.bc "$out"/fs/*.bc

# one AIR module of a metallib; its function's name
module() {
	python3 "$here/metallib.py" extract "$1" "$2" > /dev/null
	set -- "$2"/*.bc
	if [ $# -ne 1 ] || [ ! -f "$1" ]; then
		echo "compile-pipeline: expected one AIR module" >&2
		exit 1
	fi
	echo "$1"
}

vbc=$(module "$vs" "$out/vs")
fbc=$(module "$fs" "$out/fs")
vname=$(basename "$vbc" .bc)
fname=$(basename "$fbc" .bc)

"$air2amdgcn" vertex "$vbc" -o "$out/vertex.ll" --abi "$out/vertex.abi"
llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx1201 -O2 -filetype=obj "$out/vertex.ll" -o "$out/vertex.o"
ld.lld -shared "$out/vertex.o" -o "$out/vertex.hsaco"
printf '%s\n' "${vname#*_}" > "$out/vertex.name"

# the rasterizer's helpers inlined first, then the generic pointers to its state resolved, so that the state lives in registers: the
# kernel runs without a stack
"$air2amdgcn" fragment "$fbc" --vertex-abi "$out/vertex.abi" -o "$out/fragment.ll" --abi "$out/fragment.abi"
llvm-link "$out/fragment.ll" "$here/rdna4raster.ll" -o "$out/fragment.bc"
opt -mcpu=gfx1201 -passes='always-inline,infer-address-spaces,default<O2>' "$out/fragment.bc" -o "$out/fragment.opt.bc"
llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx1201 -O2 -filetype=obj "$out/fragment.opt.bc" -o "$out/fragment.o"
ld.lld -shared "$out/fragment.o" -o "$out/fragment.hsaco"
printf '%s\n' "${fname#*_}" > "$out/fragment.name"
