#!/usr/bin/env bash
# Build a development OpenCore disk for an OSX-KVM macOS VM that loads
# RDNA4FB the way a real Hackintosh does: injected by OpenCore after Lilu.
#
#   tools/vm-opencore.sh [--kext build-vmtest/RDNA4FB.kext] [--lilu path/Lilu.kext]
#                        [--args "<boot-args>"] [--resolution WxH] [--out NAME.qcow2]
#
# Starts from the stock $OSXKVM/OpenCore/OpenCore.qcow2 every time and writes
# $OSXKVM/OpenCore/OpenCore-dev.qcow2, or --out NAME in that directory (boot
# the VM with that file; tools/emu-boot.sh uses OpenCore-emu.qcow2). Changes:
#   - boot-args (default: verbose + kernel log on the serial port),
#   - picker waits for a choice, WhateverGreen disabled,
#   - GOP resolution (default 1920x1080, a 1080p monitor like the test
#     fixture's; "" keeps the image's own setting),
#   - --lilu replaces the image's Lilu.kext (OSX-KVM ships Lilu 1.6.8, which
#     disables itself on macOS 26; Lilu 1.7.1+ supports Tahoe),
#   - --kext copies RDNA4FB.kext in and adds it to Kernel -> Add after Lilu.
# QEMU keeps the old image open until it restarts; the new one is swapped in
# with a rename, so a running VM is unaffected.
#
# Needs: qemu-img, mtools (mcopy/mdir), python3. Env: OSXKVM (default ~/OSX-KVM).
set -euo pipefail

OSXKVM=${OSXKVM:-$HOME/OSX-KVM}
ARGS="-v keepsyms=1 debug=0x100 serial=3"
KEXT=""
LILU=""
RES="1920x1080"
OUTIMG="OpenCore-dev.qcow2"
while [ $# -gt 0 ]; do
	case $1 in
		--kext) KEXT=$(realpath "$2"); shift ;;
		--lilu) LILU=$(realpath "$2"); shift ;;
		--args) ARGS=$2; shift ;;
		--resolution) RES=$2; shift ;;
		--out) OUTIMG=$(basename "$2"); shift ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
	shift
done
HERE=$(cd "$(dirname "$0")" && pwd)

cd "$OSXKVM/OpenCore"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
qemu-img convert -O raw OpenCore.qcow2 "$TMP/oc.raw"

# The ESP is the image's first GPT partition.
START=$(sfdisk --dump "$TMP/oc.raw" | awk -F'[=,]' '/start=/{gsub(/ /,"",$2); print $2; exit}')
IMG="$TMP/oc.raw@@$((START * 512))"
export MTOOLS_SKIP_CHECK=1

mcopy -n -o -i "$IMG" ::/EFI/OC/config.plist "$TMP/config.plist"
if [ -n "$LILU" ]; then
	mdeltree -i "$IMG" ::/EFI/OC/Kexts/Lilu.kext
	mcopy -s -o -i "$IMG" "$LILU" ::/EFI/OC/Kexts/
	echo "replaced Lilu.kext with $LILU"
fi
RESOPT=()
[ -n "$RES" ] && RESOPT=(--resolution "$RES")
if [ -n "$KEXT" ]; then
	mdeltree -i "$IMG" "::/EFI/OC/Kexts/$(basename "$KEXT")" 2>/dev/null || true
	mcopy -s -o -i "$IMG" "$KEXT" ::/EFI/OC/Kexts/
	python3 "$HERE/vm-ocplist.py" "$TMP/config.plist" "$ARGS" "$(basename "$KEXT")" "${RESOPT[@]}"
else
	python3 "$HERE/vm-ocplist.py" "$TMP/config.plist" "$ARGS" "${RESOPT[@]}"
fi
mcopy -o -i "$IMG" "$TMP/config.plist" ::/EFI/OC/config.plist
mcopy -n -o -i "$IMG" ::/EFI/OC/config.plist "$TMP/config.check"
cmp -s "$TMP/config.plist" "$TMP/config.check" || { echo "config.plist write-back mismatch" >&2; exit 1; }

qemu-img convert -O qcow2 "$TMP/oc.raw" "$OUTIMG.new"
mv -f "$OUTIMG.new" "$OUTIMG"
echo "built $OSXKVM/OpenCore/$OUTIMG"
