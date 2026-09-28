#!/bin/bash
# Set the boot-args for one boot of docs/real-card-plan.md on the OpenCore USB.
#
#   bash /Volumes/OPENCORE/set-boot.sh <0-6>
#
#   0  this morning's known-good arguments (no new feature enabled)
#   1  + interrupts and W6 opt-in            rdna4-ih=1 rdna4-hang=1
#   2  + per-app GPU memory and W6 opt-in   rdna4-ih=1 rdna4-vm=1 rdna4-hang=1
#   3  + display interrupts and page flips  ... rdna4-flip=1 rdna4-hang=1
#   4  + gfx ring and first triangle        ... rdna4-gfx=1 rdna4-hang=1
#   5  + gfx ring through doorbell          ... rdna4-gfx=2 rdna4-hang=1
#   6  optional: vblank + cursor            ... rdna4-vbl=1 rdna4-cursor=1 rdna4-hang=1
#
# The config lists boot-args under NVRAM Delete, so the value written here is
# the one the next boot uses. A copy of the config is kept next to it first.
set -eu
BASE="keepsyms=1 debug=0x100 npci=0x2000 -v -lilubetaall rdna4-trace=1 rdna4-compute=7 rdna4-pspdump=1"
case "${1:-}" in
	0) EXTRA="" ;;
	1) EXTRA="rdna4-ih=1 rdna4-hang=1" ;;
	2) EXTRA="rdna4-ih=1 rdna4-vm=1 rdna4-hang=1" ;;
	3) EXTRA="rdna4-ih=2 rdna4-vm=1 rdna4-flip=1 rdna4-hang=1" ;;
	4) EXTRA="rdna4-ih=2 rdna4-vm=1 rdna4-flip=1 rdna4-gfx=1 rdna4-hang=1" ;;
	5) EXTRA="rdna4-ih=2 rdna4-vm=1 rdna4-flip=1 rdna4-gfx=2 rdna4-hang=1" ;;
	6) EXTRA="rdna4-ih=2 rdna4-vm=1 rdna4-flip=1 rdna4-vbl=1 rdna4-cursor=1 rdna4-hang=1" ;;
	*) echo "usage: bash $0 <0-6>   (see docs/real-card-plan.md)"; exit 1 ;;
esac
DIR=$(cd "$(dirname "$0")" && pwd)
CFG="$DIR/EFI/OC/config.plist"
[ -f "$CFG" ] || { echo "no $CFG"; exit 1; }
ARGS="$BASE${EXTRA:+ $EXTRA}"
# Keep the FIRST original: a later run must not overwrite it with an edited
# config (backup-before-premetal/ on the stick has a second copy).
[ -f "$CFG.before-set-boot" ] || cp "$CFG" "$CFG.before-set-boot"
# Replace the <string> that follows <key>boot-args</key>.
awk -v args="$ARGS" '
	done == 0 && prev ~ /<key>boot-args<\/key>/ && $0 ~ /<string>/ {
		sub(/<string>.*<\/string>/, "<string>" args "</string>"); done = 1
	}
	{ print; prev = $0 }
	END { if (!done) exit 3 }
' "$CFG" > "$CFG.new" || { rm -f "$CFG.new"; echo "boot-args entry not found; config unchanged"; exit 1; }
if command -v plutil >/dev/null 2>&1; then
	if ! plutil -lint "$CFG.new" >/dev/null; then
		rm -f "$CFG.new"
		echo "new config failed plutil validation; config unchanged"
		exit 1
	fi
else
	echo "plutil unavailable; using the existing config parser as fallback" >&2
fi
mv "$CFG.new" "$CFG"
echo "boot $1: next boot uses boot-args:"
echo "  $ARGS"
