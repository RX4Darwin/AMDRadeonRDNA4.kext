#!/bin/bash
#
# Regenerate src/pipe2_linux.inc: the register sequence that lights a second
# display pipe (src/pipe2.hpp), taken from Linux's amdgpu display core by
# running it, not by reading it.
#
#   tools/pipegen/run.sh <linux tree> [pipe dig link hpd  hactive hfront hsync hback
#                                      vactive vfront vsync vback  khz hpositive vpositive vic]
#
# The default configuration is this project's test rig: pipe 1, DIG2 on link 2
# (UNIPHY C), HPD3, CEA 1920x1080@60 (VIC 16).
#
# The Linux tree only needs the display driver and its headers. A sparse
# checkout is enough (the commit the checked-in table was made from is in its
# first line):
#
#   git clone --depth 1 --filter=blob:none --no-checkout https://github.com/torvalds/linux.git
#   cd linux && git sparse-checkout init --no-cone && git sparse-checkout set \
#       /drivers/gpu/drm/amd/display/ '/drivers/gpu/drm/amd/include/*.h' \
#       '/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_*' \
#       '/drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_0_*' \
#       '/drivers/gpu/drm/amd/include/asic_reg/nbif/nbif_6_3_1_*' \
#       /include/drm/display/drm_dp.h /include/drm/display/drm_dsc.h /include/linux/hdmi.h && git checkout
#
# How it works: pipegen.c includes dcn401_resource.c for Linux's own register
# tables and block constructors, builds one fake pipe (stream, plane, link),
# and calls Linux's dce110_apply_single_controller_ctx_to_hw, link_set_dpms_on
# and dcn401_program_pipe on it, with the HUBP values from Linux's DML 2.1.
# rec.c stands in for the register helpers and records. Linux functions that
# nothing on this path reaches are stubbed to abort if called; the few listed
# in linux-noops.txt return 0 (each is a query that is false on this path).
# mkinc.py names every register from Linux's header and refuses a table that
# leaves the blocks of the pipe being lit.
#
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
L=${1:?usage: run.sh <linux tree> [configuration]}
shift
[ $# -eq 0 ] && set -- 1 2 2 3  1920 88 44 148  1080 4 5 36  148500 1 1 16
AMD=$L/drivers/gpu/drm/amd
D=$AMD/display
M=$D/dc/dml2_0/dml21
HDR=$AMD/include/asic_reg/dcn/dcn_4_1_0_offset.h
[ -f "$HDR" ] && [ -f "$L/include/linux/hdmi.h" ] || { echo "$L: not the Linux tree described in $0" >&2; exit 2; }
B=${PIPEGEN_BUILD:-${TMPDIR:-/tmp}/pipegen-build}
mkdir -p "$B/obj"

# -O0 and no inlining: rec.c names each step after the Linux function that made it (dladdr on the caller).
# CONFIG_DRM_AMD_DC_FP is what every DCN build of amdgpu has: without it DC leaves out, among other things,
# the scaler library that DCN 4.01's DPP takes its output rectangle from (the first table was built without,
# and lit a plane with a 0x0 rectangle: black).
CF="-std=gnu11 -O0 -g -w -fno-omit-frame-pointer -fno-inline -fno-strict-aliasing -fwrapv
    -Wno-error=implicit-function-declaration -include limits.h -Dnoinline_for_stack= -DCONFIG_DRM_AMD_DC_FP=1"
INC="-I$here/shim/include -I$here"
for d in dc/inc dc/inc/hw dc/clk_mgr dc/hwss dc/resource dc/dsc dc/optc dc/dpp dc/hubbub dc/dccg dc/hubp dc/dio \
         dc/dwb dc/hpo dc/mmhubbub dc/mpc dc/opp dc/pg dc/soc_and_ip_translator modules/inc dmub/inc . include dc \
         amdgpu_dm dc/link dc/link/hwss dc/link/protocols; do INC="$INC -I$D/$d"; done
INC="$INC -I$AMD/include/asic_reg -I$AMD/include -I$L/include"
for d in inc inc/bounding_boxes src/dml2_top src/dml2_core src/dml2_dpmm src/dml2_mcg src/dml2_pmo \
         src/dml2_standalone_libraries src/inc src/dml2_cga src/dml2_utm_soc_bb .; do INC="$INC -I$M/$d"; done

objs=""
cc() { # source, object: compile if the object is older
	[ "$2" -nt "$1" ] || clang $CF $INC -c "$1" -o "$2"
	objs="$objs $2"
}
cc "$here/pipegen.c" "$B/obj/pipegen.o"
cc "$here/rec.c" "$B/obj/rec.o"
while read -r f; do cc "$D/$f" "$B/obj/L_$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"; done < "$here/linux-sources.txt"
for f in $(find "$M/src" -name '*.c' ! -name dml2_top_legacy.c | sort); do cc "$f" "$B/obj/M_$(basename "$f" .c).o"; done

# Whatever is still undefined is Linux code this path never reaches: stub it.
: > "$B/stubs.c"
for pass in 1 2; do
	clang -std=gnu11 -w -c "$B/stubs.c" -o "$B/obj/stubs.o"
	if clang $objs "$B/obj/stubs.o" -o "$B/pipegen" -lm 2> "$B/link.err"; then break; fi
	grep -o '^  "_[A-Za-z0-9_]*"' "$B/link.err" | tr -d ' "' | sed 's/^_//' | sort -u > "$B/undef.txt"
	[ $pass = 1 ] && [ -s "$B/undef.txt" ] || { cat "$B/link.err" >&2; exit 1; }
	{	echo '#include <stdio.h>'; echo '#include <stdlib.h>'
		while read -r s; do
			if grep -qx "$s" "$here/linux-noops.txt"; then echo "long $s(void) { return 0; }"
			else echo "void $s(void) { fprintf(stderr, \"pipegen: Linux called $s, which is only a stub here\\n\"); abort(); }"; fi
		done < "$B/undef.txt"; } > "$B/stubs.c"
done

"$B/pipegen" "$@" > "$B/trace.txt"
python3 "$here/mkinc.py" "$HDR" "$B/trace.txt" "$(git -C "$L" rev-parse HEAD)" > "$B/pipe2_linux.inc"
cp -f "$B/pipe2_linux.inc" "$repo/src/pipe2_linux.inc"
echo "wrote src/pipe2_linux.inc ($(grep -c "^	{ '" "$repo/src/pipe2_linux.inc") entries)"

# The reference for the host test of the DisplayPort mode switch (src/modeset.cpp): the same Linux code
# retiming a lit DP stream on pipe 0 to 2560x1440@60 (CVT reduced blanking).
"$B/pipegen" dp 0 0 0 1  2560 48 32 80  1440 3 5 33  241500 1 0 0 > "$B/dp-trace.txt"
python3 "$here/mkgolden.py" "$B/dp-trace.txt" "$(git -C "$L" rev-parse HEAD)" > "$B/dp_retime_linux.inc"
cp -f "$B/dp_retime_linux.inc" "$repo/tools/dp_retime_linux.inc"
echo "wrote tools/dp_retime_linux.inc"
