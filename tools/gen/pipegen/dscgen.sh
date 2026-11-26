#!/bin/bash
#
# Run Linux's DSC configuration and PPS code on the host (dscgen.c) and write test/ref/dsc_linux.inc, which
# test/dsc-test.cpp holds src/dsc.cpp against (docs/dsc-plan.md step 2).
#
#   tools/gen/pipegen/dscgen.sh <linux tree>
#
# The Linux tree is the one run.sh takes, with /drivers/gpu/drm/display/drm_dsc_helper.c in its sparse set.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
L=$(cd "$1" && pwd)
AMD=$L/drivers/gpu/drm/amd
D=$AMD/display
B=$repo/Build/dscgen
mkdir -p "$B"

# run.sh's flags and include list (the float RC code needs CONFIG_DRM_AMD_DC_FP)
CF="-std=gnu11 -O0 -g -w -fno-strict-aliasing -fwrapv -include limits.h -Wno-error=implicit-function-declaration
    -Dnoinline_for_stack= -DCONFIG_DRM_AMD_DC_FP=1"
INC="-I$here/shim/include -I$here"
for d in dc/inc dc/inc/hw dc/clk_mgr dc/dsc dc/dml dc/dml/dsc modules/inc dmub/inc . include dc amdgpu_dm; do
	INC="$INC -I$D/$d"
done
INC="$INC -I$AMD/include/asic_reg -I$AMD/include -I$L/include"
objs=""
for f in "$here/dscgen.c" "$D"/dc/dsc/{dc_dsc,rc_calc,rc_calc_dpi}.c "$D"/dc/dsc/dcn20/dcn20_dsc.c \
         "$D"/dc/dsc/dcn401/dcn401_dsc.c "$D"/dc/dml/dsc/rc_calc_fpu.c "$D"/dc/basics/fixpt31_32.c \
         "$L"/drivers/gpu/drm/display/drm_dsc_helper.c; do
	o=$B/$(basename "$f" .c).o
	# shellcheck disable=SC2086
	clang $CF $INC -c "$f" -o "$o"
	objs="$objs $o"
done

# What these files name and no DSC path here reaches (register writes, logging): stubs that abort; the kernel's FPU
# brackets do nothing on the host.
: > "$B/stubs.c"
for pass in 1 2; do
	clang -std=gnu11 -w -c "$B/stubs.c" -o "$B/stubs.obj"
	# shellcheck disable=SC2086
	if clang $objs "$B/stubs.obj" -o "$B/dscgen" -lm 2> "$B/link.err"; then break; fi
	grep -o '^  "_[A-Za-z0-9_]*"' "$B/link.err" | tr -d ' "' | sed 's/^_//' | sort -u > "$B/undef.txt"
	[ $pass = 1 ] && [ -s "$B/undef.txt" ] || { cat "$B/link.err" >&2; exit 1; }
	{	echo '#include <stdio.h>'; echo '#include <stdlib.h>'
		echo 'void dcn41_shim_warn(const char *f, int l) { fprintf(stderr, "dscgen: WARN_ON at %s:%d\n", f, l); }'
		grep -v '^dcn41_shim_' "$B/undef.txt" | while read -r s; do
			case $s in dc_fpu_begin|dc_fpu_end|dc_assert_fp_enabled) echo "void $s(void) { }"; continue;; esac
			echo "void $s(void) { fprintf(stderr, \"dscgen: Linux called $s, a stub here\\n\"); abort(); }"
		done; } > "$B/stubs.c"
done

"$B/dscgen" "$(git -C "$L" rev-parse --short=12 HEAD)" > "$B/dsc_linux.inc"
cp -f "$B/dsc_linux.inc" "$repo/test/ref/dsc_linux.inc"
echo "wrote test/ref/dsc_linux.inc"
