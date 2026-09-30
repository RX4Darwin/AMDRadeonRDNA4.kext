#!/bin/bash
# Stage the FINAL real-card round on the OpenCore stick, reproducibly (docs/final-test-plan.md).
#
#   bash tools/stage-stick.sh [--dry-run] [--rebuild] <stick mountpoint>
#
# What it does, in this order (--dry-run prints each step and changes nothing):
#   1. refuses unless the git tree is clean (tracked files) and build/RDNA4FB.kext + build/rdna4-run are newer than every build input of HEAD
#      (--rebuild runs tools/build-osxcross.sh first; OSXCROSS must be set or ~/.local/share/osxcross exists);
#   2. backs up what it will replace into backup-<YYYYMMDD>-<commit>/ on the stick (kext, set-boot.sh, diagnostic-log.sh, rdna4-run, START-HERE.txt,
#      and a READ-ONLY copy of EFI/OC/config.plist); refuses if that folder already exists (never overwrites a backup); never writes to
#      backup-prev-*/ or backup-before-premetal/ or old-logs/ or any rdna4fb-diag-*.txt;
#   3. copies the kext (EFI/OC/Kexts/RDNA4FB.kext), rdna4-run, tools/set-boot.sh, tools/diagnostic-log.sh and a generated START-HERE.txt
#      (docs/start-here.template.txt + the boot order table of docs/final-test-plan.md);
#   4. verifies every copy with cmp (kext: diff -r) and prints a manifest (sha1 of each staged file, the commit); the manifest is also saved on the
#      stick as MANIFEST-<commit>.txt;
#   5. leaves the stick on boot 1: the only thing that touches EFI/OC/config.plist is `set-boot.sh 1` (which keeps its own first backup);
#   6. --dry-run: only prints.
set -eu

DRY=0; REBUILD=0; STICK=""
for a in "$@"; do
	case "$a" in
		--dry-run) DRY=1 ;;
		--rebuild) REBUILD=1 ;;
		-h|--help) sed -n 2,18p "$0"; exit 0 ;;
		-*) echo "unknown option $a"; exit 2 ;;
		*) [ -z "$STICK" ] && STICK="$a" || { echo "one mountpoint only"; exit 2; } ;;
	esac
done
[ -n "$STICK" ] || { echo "usage: bash $0 [--dry-run] [--rebuild] <stick mountpoint>"; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
say() { echo "$*"; }
act() { if [ "$DRY" = 1 ]; then echo "[dry-run] $*"; else echo "+ $*"; fi; }
die() { echo "REFUSED: $*" >&2; exit 1; }

# ---- 1. tree and build --------------------------------------------------------------------------------------------------------------------------
git rev-parse --git-dir >/dev/null 2>&1 || die "not a git tree"
[ -z "$(git status --porcelain --untracked-files=no)" ] || { git status --short --untracked-files=no | head; die "the working tree is not clean (commit or stash first)"; }
COMMIT="$(git rev-parse --short HEAD)"
BRANCH="$(git branch --show-current)"; [ -n "$BRANCH" ] || BRANCH="detached"
say "tree: $BRANCH @ $COMMIT (clean)"
KEXT_BIN=build/RDNA4FB.kext/Contents/MacOS/RDNA4FB
RUN_BIN=build/rdna4-run
if [ "$REBUILD" = 1 ]; then
	act "OSXCROSS=\${OSXCROSS:-\$HOME/.local/share/osxcross} bash tools/build-osxcross.sh"
	[ "$DRY" = 1 ] || OSXCROSS="${OSXCROSS:-$HOME/.local/share/osxcross}" bash tools/build-osxcross.sh >/tmp/stage-stick-build.$$.log 2>&1 || { tail -20 /tmp/stage-stick-build.$$.log; die "build failed"; }
fi
[ -f "$KEXT_BIN" ] || die "$KEXT_BIN does not exist (run tools/build-osxcross.sh, or pass --rebuild)"
[ -f "$RUN_BIN" ] || die "$RUN_BIN does not exist (run tools/build-osxcross.sh, or pass --rebuild)"
# Up to date = no tracked build input is newer than the binary. Conservative (a checkout or merge touches files): then rebuild.
stale_kext="$(git ls-files src include shaders Makefile tools/build-osxcross.sh Lilu MacKernelSDK 2>/dev/null | while read -r f; do [ -f "$f" ] && [ "$f" -nt "$KEXT_BIN" ] && echo "$f"; done | head -3)"
stale_run="$(git ls-files userspace include Makefile 2>/dev/null | while read -r f; do [ -f "$f" ] && [ "$f" -nt "$RUN_BIN" ] && echo "$f"; done | head -3)"
if [ "$DRY" = 0 ] || [ "$REBUILD" = 0 ]; then
	[ -z "$stale_kext" ] || die "build/RDNA4FB.kext is older than $(echo $stale_kext | tr '\n' ' ')- rebuild (tools/build-osxcross.sh or --rebuild)"
	[ -z "$stale_run" ] || die "build/rdna4-run is older than $(echo $stale_run | tr '\n' ' ')- rebuild (tools/build-osxcross.sh or --rebuild)"
fi
say "build: kext and rdna4-run are newer than every tracked build input"

# ---- stick checks --------------------------------------------------------------------------------------------------------------------------------
[ -d "$STICK" ] || die "$STICK is not a directory"
STICK="$(cd "$STICK" && pwd)"
[ -f "$STICK/EFI/OC/config.plist" ] && [ -d "$STICK/EFI/OC/Kexts" ] || die "$STICK does not look like the OpenCore stick (no EFI/OC/config.plist or EFI/OC/Kexts)"
BK="backup-$(date +%Y%m%d)-$COMMIT"
[ ! -e "$STICK/$BK" ] || die "$STICK/$BK already exists: a backup is never overwritten (remove it by hand if you really mean to stage again today)"
case "$BK" in backup-prev-*|backup-before-premetal*) die "internal: backup name clash" ;; esac
say "stick: $STICK; backup folder: $BK"

# ---- START-HERE.txt from the template + the plan's order table -----------------------------------------------------------------------------------
TEMPLATE=docs/start-here.template.txt; PLAN=docs/final-test-plan.md
[ -f "$TEMPLATE" ] && [ -f "$PLAN" ] || die "$TEMPLATE or $PLAN missing"
TMPD="$(mktemp -d)"; trap 'rm -rf "$TMPD"' EXIT
# Table rows of "## 2. The order": | n | **id** ... | what | continue? |
awk -F'|' '
	/^## 2\. The order/ { on = 1; next }
	on && /^## / { exit }
	on && /^\| *([0-9]+|end) *\|/ {
		gsub(/\*\*|`/, "", $0)
		n = $2; id = $3; what = $4; cont = $5
		gsub(/^ +| +$/, "", n); gsub(/^ +| +$/, "", id); gsub(/^ +| +$/, "", what); gsub(/^ +| +$/, "", cont)
		print n "\t" id "\t" what "\t" cont
	}' "$PLAN" > "$TMPD/order.tsv"
[ -s "$TMPD/order.tsv" ] || die "could not read the order table of $PLAN"
IDS_PLAN="$(awk -F'\t' '$1 != "end" { print $2 }' "$TMPD/order.tsv" | sort -u | tr '\n' ' ')"
IDS_TPL="$(sed -n 's/^== BOOT \(.*\) ==$/\1/p' "$TEMPLATE" | sort -u | tr '\n' ' ')"
[ "$IDS_PLAN" = "$IDS_TPL" ] || die "boot ids differ: plan table [$IDS_PLAN] vs template sections [$IDS_TPL]: update docs/start-here.template.txt"
# The ids must also exist in set-boot.sh.
for id in $IDS_PLAN; do
	grep -Eq "^[[:space:]]+$id\) EXTRA=" tools/set-boot.sh || die "boot $id of the plan is not a case of tools/set-boot.sh"
done
{
	while IFS="$(printf '\t')" read -r n id what cont; do
		if [ "$n" = end ]; then
			printf '  LAST: boot %s\n' "$id" | fold -s -w 76 | sed '2,$s/^/        /'
		else
			printf '  STEP %s: type  bash /Volumes/OPENCORE/set-boot.sh %s\n' "$n" "$id"
			printf '        %s\n' "$what" | fold -s -w 76 | sed '2,$s/^/        /'
			case "$cont" in
				No:*)    msg="If it FAILS: STOP -${cont#No:}" ;;
				Yes:*)   msg="If it FAILS: carry on with the next step (${cont#Yes: })." ;;
				Yes)     msg="If it FAILS: carry on with the next step." ;;
				After*)  msg="If it hangs or freezes:${cont#After a hang:} (see IF SOMETHING GOES WRONG below)." ;;
				"(optional)") msg="This step is OPTIONAL (see OPTIONAL LAST STEP below); skip it if you are short of time." ;;
				"(last)") msg="This is the last test boot: afterwards Shut Down (power-cycle), then the OPTIONAL LAST STEP below or WHEN YOU ARE FINISHED." ;;
				*)       msg="If it FAILS: $cont" ;;
			esac
			printf '        %s\n' "$msg" | fold -s -w 76 | sed '2,$s/^/        /'
		fi
	done < "$TMPD/order.tsv"
} | sed 's/ *$//' > "$TMPD/order.txt"
sed -e "s|@COMMIT@|$COMMIT|g" -e "s|@BRANCH@|$BRANCH|g" -e "s|@DATE@|$(date +%Y-%m-%d)|g" -e "s|@BACKUP@|$BK|g" "$TEMPLATE" |
	awk -v f="$TMPD/order.txt" '/^@ORDER@$/ { while ((getline l < f) > 0) print l; next } { print }' > "$TMPD/START-HERE.txt"
grep -q '@[A-Z]*@' "$TMPD/START-HERE.txt" && die "unreplaced @PLACEHOLDER@ in the generated START-HERE.txt"
say "START-HERE.txt generated ($(wc -l < "$TMPD/START-HERE.txt") lines, boot ids: $IDS_PLAN)"

# ---- 2. backup ----------------------------------------------------------------------------------------------------------------------------------
act "mkdir $STICK/$BK"
[ "$DRY" = 1 ] || mkdir "$STICK/$BK"
backup_one() {   # <relative path on the stick> <name in the backup folder>
	if [ -e "$STICK/$1" ]; then
		act "cp -R $1 -> $BK/$2"
		if [ "$DRY" = 0 ]; then
			cp -R "$STICK/$1" "$STICK/$BK/$2"
			if [ -d "$STICK/$1" ]; then diff -r "$STICK/$1" "$STICK/$BK/$2" >/dev/null || die "backup of $1 differs from the original (nothing has been replaced yet)"
			else cmp -s "$STICK/$1" "$STICK/$BK/$2" || die "backup of $1 differs from the original (nothing has been replaced yet)"; fi
		fi
	else
		say "  (no $1 on the stick: nothing to back up)"
	fi
}
backup_one EFI/OC/Kexts/RDNA4FB.kext RDNA4FB.kext
backup_one set-boot.sh set-boot.sh
backup_one diagnostic-log.sh diagnostic-log.sh
backup_one rdna4-run rdna4-run
backup_one START-HERE.txt START-HERE.txt
backup_one EFI/OC/config.plist config.plist      # a read-only copy: staging never writes config.plist (only set-boot.sh 1 does, at the end)

# ---- 3. copy ------------------------------------------------------------------------------------------------------------------------------------
act "replace $STICK/EFI/OC/Kexts/RDNA4FB.kext with build/RDNA4FB.kext"
if [ "$DRY" = 0 ]; then
	rm -rf "$STICK/EFI/OC/Kexts/RDNA4FB.kext"
	cp -R build/RDNA4FB.kext "$STICK/EFI/OC/Kexts/RDNA4FB.kext"
fi
act "cp build/rdna4-run $STICK/rdna4-run";                         [ "$DRY" = 1 ] || cp "$RUN_BIN" "$STICK/rdna4-run"
act "cp tools/set-boot.sh $STICK/set-boot.sh";                     [ "$DRY" = 1 ] || cp tools/set-boot.sh "$STICK/set-boot.sh"
act "cp tools/diagnostic-log.sh $STICK/diagnostic-log.sh";         [ "$DRY" = 1 ] || cp tools/diagnostic-log.sh "$STICK/diagnostic-log.sh"
act "write $STICK/START-HERE.txt (generated)";                     [ "$DRY" = 1 ] || cp "$TMPD/START-HERE.txt" "$STICK/START-HERE.txt"

# ---- 4. verify + manifest -----------------------------------------------------------------------------------------------------------------------
MAN="$TMPD/manifest.txt"
{
	echo "RDNA4FB final round staging manifest"
	echo "commit: $COMMIT ($BRANCH), staged $(date '+%Y-%m-%d %H:%M')"
	echo "backup of the previous files: $BK/"
} > "$MAN"
if [ "$DRY" = 1 ]; then
	act "cmp / diff -r every copy against its source, sha1 manifest -> MANIFEST-$COMMIT.txt"
	echo "sha1 (of the sources that would be staged):" >> "$MAN"
	for f in "$KEXT_BIN" "$RUN_BIN" tools/set-boot.sh tools/diagnostic-log.sh "$TMPD/START-HERE.txt"; do sha1sum "$f" | sed "s|$TMPD/||" >> "$MAN"; done
else
	diff -r build/RDNA4FB.kext "$STICK/EFI/OC/Kexts/RDNA4FB.kext" >/dev/null || die "verify: the kext on the stick differs from build/RDNA4FB.kext (the backup in $BK is intact)"
	cmp -s "$RUN_BIN" "$STICK/rdna4-run" || die "verify: rdna4-run differs"
	cmp -s tools/set-boot.sh "$STICK/set-boot.sh" || die "verify: set-boot.sh differs"
	cmp -s tools/diagnostic-log.sh "$STICK/diagnostic-log.sh" || die "verify: diagnostic-log.sh differs"
	cmp -s "$TMPD/START-HERE.txt" "$STICK/START-HERE.txt" || die "verify: START-HERE.txt differs"
	say "verify: every copy identical to its source (cmp / diff -r)"
	echo "sha1 (as found on the stick after staging):" >> "$MAN"
	(cd "$STICK" && sha1sum EFI/OC/Kexts/RDNA4FB.kext/Contents/MacOS/RDNA4FB EFI/OC/Kexts/RDNA4FB.kext/Contents/Info.plist rdna4-run set-boot.sh diagnostic-log.sh START-HERE.txt) >> "$MAN"
fi

# ---- 5. leave the stick on boot 1 ---------------------------------------------------------------------------------------------------------------
act "bash $STICK/set-boot.sh 1      # the only thing that touches EFI/OC/config.plist (it keeps its own first backup, config.plist.before-set-boot)"
if [ "$DRY" = 0 ]; then
	bash "$STICK/set-boot.sh" 1 | sed 's/^/  set-boot: /'
	echo "config.plist sha1 after set-boot.sh 1 (the backup copy in $BK/ is the state before):" >> "$MAN"
	(cd "$STICK" && sha1sum EFI/OC/config.plist) >> "$MAN"
	cp "$MAN" "$STICK/MANIFEST-$COMMIT.txt"
fi
echo
echo "=== manifest ==="
cat "$MAN"
[ "$DRY" = 1 ] && echo "(dry-run: nothing was written)" || echo "staged; the stick is on boot 1. Manifest saved as MANIFEST-$COMMIT.txt on the stick."
