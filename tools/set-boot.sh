#!/bin/bash
# Set the boot-args for one boot of docs/real-card-plan.md on the OpenCore USB.
#
#   bash /Volumes/OPENCORE/set-boot.sh <0-7>
#
#   Round 5 plan (D23): every test boot runs on the PROVEN base, rdna4-ih=2 rdna4-flip=1 rdna4-hang=1
#   (interrupts, vblank + page flips, queue recovery; clock gating is on by default since W29).
#   The round 1-4 table (boots 0-18) is in git history (premetal/int before this commit).
#
#   0  known-good arguments (no new feature enabled): the way back
#   1  the proven base: also the setting for daily use
#   2  COMBINED: base + per-app GPU memory (W36 IS_PTE + EXECUTE) + the REAL macOS pointer   rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1
#   3  COMBINED: boot 2 + first triangle (sane clip state, SRM, CP-side probe); run AFTER boot 2, it is the one that can hang the GPU   ... rdna4-gfx=2 rdna4-gfxprobe=1 rdna4-gfxdiag=11
#   6  boot 3 with SRM only, no replay and no sane-clip block (amdgpu-exact; attributes the fix)   ... rdna4-gfxcsb=0 rdna4-gfxsane=0
#   4  base + the REAL macOS pointer (W45): rdna4-cursor=1 rdna4-vbl=1; CM_BYPASS is cleared at arming by default (rdna4-cursorcm=0 is the escape)
#   5  base + GFXOFF (optional, ALWAYS LAST, then power off)   rdna4-gfxpm=24 rdna4-gfxoff=1
#   7  base + cursor self-test: magenta 64x64 square at (100,100), macOS pointer ignored   rdna4-vbl=1 rdna4-cursor=2 rdna4-cursorcm=1 (CRC A/B: GOP state, after the CM_BYPASS clear)
#
# The config lists boot-args under NVRAM Delete, so the value written here is
# the one the next boot uses. A copy of the config is kept next to it first.
set -eu
BASE="keepsyms=1 debug=0x100 npci=0x2000 -v -lilubetaall rdna4-trace=1 rdna4-compute=7 rdna4-pspdump=1"
case "${1:-}" in
	0) EXTRA="" ;;
	1) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1" ;;
	2) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1" ;;
	3) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1 rdna4-gfx=2 rdna4-gfxprobe=1 rdna4-gfxdiag=11" ;;
	4) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vbl=1 rdna4-cursor=1" ;;
	5) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-gfxpm=24 rdna4-gfxoff=1" ;;
	7) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vbl=1 rdna4-cursor=2 rdna4-cursorcm=1" ;;
	6) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1 rdna4-gfx=2 rdna4-gfxprobe=1 rdna4-gfxdiag=11 rdna4-gfxcsb=0 rdna4-gfxsane=0" ;;
	*) echo "usage: bash $0 <0-7>   (see docs/real-card-plan.md)"; exit 1 ;;
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
