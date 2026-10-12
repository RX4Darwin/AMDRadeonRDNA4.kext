#!/usr/bin/env bash
# E3 preparation (docs/metal-spike.md, hub-task-354): compile vadd for gfx1030, translate it for gfx1201, and print the evidence. RUNS NOTHING ON ANY GPU.
#   tools/e3/run-e3.sh            (needs clang/ld.lld/llvm-mc/llvm-objdump with the AMDGPU target, and g++)
# The run on the card is a separate, reviewed step:
#   REPLAY_IP=compute REPLAY_COMPUTE_OK=1 REPLAY_CODEOBJ=build/e3/vadd-translated.hsaco tools/linux-replay/run.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
O=build/e3
mkdir -p $O
for g in gfx1030 gfx1201; do
	clang -target amdgcn-amd-amdhsa -mcpu=$g -x cl -cl-std=CL2.0 -O2 -nogpulib -c shaders/vadd.cl -o $O/vadd-$g.o
	ld.lld -shared $O/vadd-$g.o -o $O/vadd-$g.hsaco
	clang -target amdgcn-amd-amdhsa -mcpu=$g -x cl -cl-std=CL2.0 -O2 -nogpulib -S shaders/vadd.cl -o $O/vadd-$g.s
done
python3 tools/e3/g10to12.py $O/vadd-gfx1030.hsaco --kernel vadd --src1030 $O/vadd-gfx1030.s --native $O/vadd-gfx1201.s -o $O/vadd-translated.hsaco --keep $O/vadd-translated.s
g++ -std=c++17 -O1 -Wall -Isrc -o $O/check-codeobj tools/e3/check-codeobj.cpp src/codeobj.cpp
echo; echo "== kernel descriptors: clang's native gfx1201 build vs the translation (the kext's own code-object reader)"
$O/check-codeobj $O/vadd-gfx1201.hsaco $O/vadd-translated.hsaco vadd
echo; echo "== gfx1030 (input)"; llvm-objdump -d --mcpu=gfx1030 $O/vadd-gfx1030.hsaco | sed -n '/<vadd>:/,/^$/p' | sed 's/  *\/\/.*//'
echo; echo "== gfx1201 translated (output)"; llvm-objdump -d --mcpu=gfx1201 $O/vadd-translated.hsaco | sed -n '/<vadd>:/,/s_endpgm/p' | sed 's/  *\/\/.*//'
echo; echo "== gfx1201 native clang build (reference)"; llvm-objdump -d --mcpu=gfx1201 $O/vadd-gfx1201.hsaco | sed -n '/<vadd>:/,/s_endpgm/p' | sed 's/  *\/\/.*//'
echo; echo "== negative controls: the translator must refuse what it has no rule for"
for bad in "s_barrier" "v_max_f32 v0, v1, v2" "s_waitcnt lgkmcnt(1)" "s_cbranch_scc0 0"; do
	printf '%s\n' "$bad" > $O/neg.txt
	python3 - "$bad" <<'PY'
import sys, importlib.util
spec = importlib.util.spec_from_file_location("g", "tools/e3/g10to12.py"); g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
info = {"user_sgpr_private_segment_buffer": 1, "user_sgpr_kernarg_segment_ptr": 1, "user_sgpr_count": 6, "system_sgpr_workgroup_id_x": 1,
        "system_sgpr_workgroup_id_y": 0, "system_sgpr_workgroup_id_z": 0, "system_vgpr_workitem_id": 0}
try:
    g.translate([sys.argv[1]], info)
    print("  NOT REFUSED (bug): %s" % sys.argv[1]); sys.exit(1)
except g.Refuse as e:
    print("  refused: %s" % e)
PY
done
echo; echo "E3 prepared; nothing was run on a GPU. The card run needs the lead's review (REPLAY_COMPUTE_OK=1 REPLAY_CODEOBJ=$O/vadd-translated.hsaco)."
