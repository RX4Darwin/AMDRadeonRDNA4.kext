#!/bin/bash
#
# Run Linux's DMUB service on the host (dmubgen.c) and write test/ref/dmub_start_linux.inc: how amdgpu starts the
# display microcontroller on DCN 4.01 once the PSP has loaded its firmware (the windows' layout for this firmware
# and the register steps), which test/dmubload-test.cpp holds src/dmubload.cpp against.
#
#   tools/gen/pipegen/dmubgen.sh <linux tree> [firmware] [vbios bytes]
#
# The Linux tree is the one run.sh takes (its sparse checkout is enough). The firmware defaults to
# firmware/amdgpu/dcn_4_0_1_dmcub.bin (tools/build/fetch-firmware.sh), the VBIOS size to 128 KiB.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
L=$(cd "$1" && pwd)
FW=${2:-$repo/firmware/amdgpu/dcn_4_0_1_dmcub.bin}
VBIOS=${3:-0x20000}
D=$L/drivers/gpu/drm/amd/display
B=$repo/Build/dmubgen
mkdir -p "$B"

CF="-std=gnu11 -O0 -g -w -fno-strict-aliasing -include limits.h -Wno-error=implicit-function-declaration"
INC="-I$here/shim/include -I$here -I$D/dmub/inc -I$D -I$D/dc -I$D/include -I$D/dc/inc
     -I$L/drivers/gpu/drm/amd/include/asic_reg -I$L/drivers/gpu/drm/amd/include -I$L/include"
objs=""
for f in "$here/dmubgen.c" "$D"/dmub/src/{dmub_srv,dmub_reg,dmub_dcn401,dmub_srv_stat}.c; do
	o=$B/$(basename "$f" .c).o
	# shellcheck disable=SC2086
	clang $CF $INC -c "$f" -o "$o"
	objs="$objs $o"
done

# The other ASICs' functions dmub_srv.c names: stubs that abort (the shim's delay and warning do nothing).
: > "$B/stubs.c"
for pass in 1 2; do
	clang -std=gnu11 -w -c "$B/stubs.c" -o "$B/stubs.obj"
	# shellcheck disable=SC2086
	if clang $objs "$B/stubs.obj" -o "$B/dmubgen" 2> "$B/link.err"; then break; fi
	grep -o '^  "_[A-Za-z0-9_]*"' "$B/link.err" | tr -d ' "' | sed 's/^_//' | sort -u > "$B/undef.txt"
	[ $pass = 1 ] && [ -s "$B/undef.txt" ] || { cat "$B/link.err" >&2; exit 1; }
	{	echo '#include <stdio.h>'; echo '#include <stdlib.h>'
		echo 'void dcn41_shim_udelay(unsigned long us) { }'
		echo 'void dcn41_shim_warn(const char *f, int l) { fprintf(stderr, "dmubgen: WARN_ON at %s:%d\n", f, l); }'
		grep -v '^dcn41_shim_' "$B/undef.txt" | while read -r s; do
			echo "void $s(void) { fprintf(stderr, \"dmubgen: Linux called $s, a stub here\\n\"); abort(); }"
		done; } > "$B/stubs.c"
done

"$B/dmubgen" "$FW" "$VBIOS" "$(git -C "$L" rev-parse --short=12 HEAD)" > "$B/dmub_start_linux.inc"
cp -f "$B/dmub_start_linux.inc" "$repo/test/ref/dmub_start_linux.inc"
echo "wrote test/ref/dmub_start_linux.inc ($(grep -c "^	{ '" "$repo/test/ref/dmub_start_linux.inc") steps)"
