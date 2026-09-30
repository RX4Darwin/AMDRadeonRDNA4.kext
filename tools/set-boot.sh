#!/bin/bash
# Set the boot-args for one boot of docs/real-card-plan.md on the OpenCore USB.
#
#   bash /Volumes/OPENCORE/set-boot.sh <0-9|9b>
#
#   Round 5 plan (D23): every test boot runs on the PROVEN base, rdna4-ih=2 rdna4-flip=1 rdna4-hang=1
#   (interrupts, vblank + page flips, queue recovery; clock gating is on by default since W29).
#   The round 1-4 table (boots 0-18) is in git history (premetal/int before this commit).
#
#   0  known-good arguments (no new feature enabled): the way back
#   1  the proven base + the real hardware pointer (W45, CM_BYPASS cleared by default): also the setting for daily use   rdna4-vbl=1 rdna4-cursor=1
#   2  boot 1 + per-app GPU memory (W36 IS_PTE + EXECUTE)   rdna4-vm=1 rdna4-vm-diag=4065
#   3  boot 2 + first triangle (sane clip state, SRM, CP-side probe); run AFTER boot 2, it is the one that can hang the GPU   ... rdna4-gfx=2 rdna4-gfxprobe=1 rdna4-gfxdiag=11 rdna4-gfxcol=1
#      rdna4-gfxcol=1 (default off elsewhere): after the G3 baseline PASSES, also draw the G4 colour triangle (vertex 0 red, 1 green, 2 blue through
#      the attribute ring, docs/g4-colour.md); diagnostic row "gfx-col". G4 never runs when the baseline failed, so it cannot hide a G3 result.
#   6  boot 3 with SRM only, no replay and no sane-clip block (amdgpu-exact; attributes the fix)   ... rdna4-gfxcsb=0 rdna4-gfxsane=0   (no gfxcol: a clean A/B control of the G3 fix)
#   4  (retired: the real pointer is part of boot 1 since round 6)
#   5  base + GFXOFF (optional, ALWAYS LAST, then power off)   rdna4-gfxpm=24 rdna4-gfxoff=1
#   8  boot 2 + the gfx ring left UP for applications (W12k): rdna4-gfx=2 rdna4-gfxcol=1 rdna4-gfxclient=1, WITHOUT rdna4-gfxprobe/gfxdiag (boot 3's probe parks PFP/ME after its
#      draws, so no client could draw there). Bring-up draws G3 (+G4) and runs the kernel's synthetic client-IB self-test (gfx-client); diagnostic-log.sh then runs
#      `rdna4-run info` (must show GFX), `rdna4-run tri` and `rdna4-run tricol` (rows gfx-app-tri / gfx-app-tricol). On the emulator: tri PASS, tricol FAIL (G4 is not modelled).
#      Needs docs/w12k-gfx-submit.md's dependency (W13 S0: rtFree invalidates device buffers) before it is enabled on the real card.
#   9  THE VM-CLIENT DIAGNOSTIC (docs/vm-client-rootcause.md, hub-task-311/320): the VM boot of round 6 WITHOUT rdna4-vbl/cursor (so the kernel log window keeps the bring-up
#      lines; round 6's began 146 s into the boot) and WITHOUT rdna4-vm-diag, plus rdna4-vmid-test=15 (1 = probes T0-T7 incl. the incremental-stream bisect T5c and T4d, 2 = read-only
#      surveys of engines + all 8 MEC HQDs + hub/L2 + SMU at the flow points, 4 = a bounded client-op trace, 8 = the late probes T4a/T4d/T5b repeated after clock gating) and rdna4-gfxpm=24 (the existing per-stage `pm: survey` lines + SMU samples; it only
#      samples). rdna4-compute stays 7: the compute: log lines (mec/vm/runtime) are what this boot is for; =1 would stop bring-up after the survey stage. Why no vm-diag: the
#      round 6 boots DID carry rdna4-vm-diag=4065 (061829 line 17; the diagnostic script did not list that arg), whose bit 512 "F" points the GC hub's fault default page at a
#      system page during the boot test, and the round 5 and 6 VM boots had it: "a clean VM boot pins the GPU at idle" is unproven. Run 9 first, then 9b.
#   9b boot 9 + rdna4-vm-diag=4065: the exact round 6 VM configuration with the full instrumentation (run after 9: A/B on the confound).
#   7  base + cursor self-test: magenta 64x64 square at (100,100), macOS pointer ignored   rdna4-vbl=1 rdna4-cursor=2 rdna4-cursorcm=1 (CRC A/B: GOP state, after the CM_BYPASS clear)
#
# The config lists boot-args under NVRAM Delete, so the value written here is
# the one the next boot uses. A copy of the config is kept next to it first.
set -eu
BASE="keepsyms=1 debug=0x100 npci=0x2000 -v -lilubetaall rdna4-trace=1 rdna4-compute=7 rdna4-pspdump=1"
case "${1:-}" in
	0) EXTRA="" ;;
	1) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vbl=1 rdna4-cursor=1" ;;
	2) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1" ;;
	3) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1 rdna4-gfx=2 rdna4-gfxprobe=1 rdna4-gfxdiag=11 rdna4-gfxcol=1" ;;
	5) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vbl=1 rdna4-cursor=1 rdna4-gfxpm=24 rdna4-gfxoff=1" ;;
	7) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vbl=1 rdna4-cursor=2 rdna4-cursorcm=1" ;;
	9) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vmid-test=15 rdna4-gfxpm=24" ;;
	9b) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vmid-test=15 rdna4-gfxpm=24" ;;
	6) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1 rdna4-gfx=2 rdna4-gfxprobe=1 rdna4-gfxdiag=11 rdna4-gfxcsb=0 rdna4-gfxsane=0" ;;
	8) EXTRA="rdna4-ih=2 rdna4-flip=1 rdna4-hang=1 rdna4-vm=1 rdna4-vm-diag=4065 rdna4-vbl=1 rdna4-cursor=1 rdna4-gfx=2 rdna4-gfxcol=1 rdna4-gfxclient=1" ;;
	*) echo "usage: bash $0 <0-9|9b>   (see docs/real-card-plan.md)"; exit 1 ;;
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
