#!/bin/bash
#
# boot-history.sh — what the last boots of this machine ended in, from the system log that is already on disk.
# For a machine that sometimes freezes: which boots did not shut down cleanly, how long they had been up, and the
# last kernel lines each of them got to disk. Run on the test machine:
#
#   sudo bash boot-history.sh [days]      (default 3; more days take longer: about a minute a day)
#
# Output: boot-history-<timestamp>.txt next to this script (and on the terminal).
#
# How it tells: the kernel writes "Previous shutdown cause: N" early in every boot (5 = a clean shutdown or
# restart; anything else, 3 or 0 for instance, = the boot before it was cut off: held power button, reset, power
# loss). A boot's last lines are the last the log daemon had written before the end, so the final seconds of a
# freeze can be missing. Boots are told apart by the log's boot marker, the kernel's version line and this driver's.
#
# It also notes three things about where the boot configuration lives, because a FAT partition written shortly
# before a hard reset is how a config.plist gets damaged: whether an EFI partition is mounted, what a read-only
# check of each says (fsck_msdos -n changes nothing), and whether NVRAM keeps a value across a reboot. For the last
# it sets one NVRAM variable, rdna4-nvtest, and reads back what an earlier run of this script set.
#
[ "$(id -u)" -eq 0 ] || { echo "run with sudo (the log and the EFI check need root)"; exit 1; }
DAYS=${1:-3}
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/boot-history-$(date +%Y%m%d-%H%M%S).txt"
exec > >(tee "$OUT") 2>&1

echo "=== boot-history: the last $DAYS day(s), read $(date '+%Y-%m-%d %H:%M:%S %z') ==="
echo "this boot: $(sysctl -n kern.bootsessionuuid 2>/dev/null), up since $(sysctl -n kern.boottime 2>/dev/null | sed 's/.*} //')"
sw_vers 2>/dev/null | tr '\n' ' '; echo

echo
echo "=== boots (oldest first) ==="
log show --last "${DAYS}d" --predicate 'process == "kernel"' --style syslog 2>/dev/null | awk -v TAIL=40 -v OURS=12 '
# Days since 1970 for a civil date, so that durations survive midnight and month ends.
function days(y, m, d) { if (m <= 2) { y--; m += 12 } return 365 * y + int(y / 4) - int(y / 100) + int(y / 400) + int((153 * (m - 3) + 2) / 5) + d - 719469 }
function secs(date, time,   a, b) { split(date, a, "-"); split(time, b, "[:.+-]"); return days(a[1], a[2], a[3]) * 86400 + b[1] * 3600 + b[2] * 60 + b[3] }
function start(t) { n++; first[n] = t; firstText[n] = $1 " " substr($2, 1, 8); lines[n] = 0; cause[n] = ""; version[n] = ""; bringup[n] = "" }
/^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9] / {
	t = secs($1, $2)
	# A new boot: the boot marker of the log, the version line of the kernel, or a second version line of this
	# driver. The first two can both be there, a few lines apart: they start one boot, not two.
	# (No apostrophes in this program: it is inside a quoted shell word.)
	isKernel = $0 ~ /Darwin Kernel Version/
	if (n == 0 || pending || (isKernel && (kernelLine[n] || lines[n] > 50)) || ($0 ~ /AMDRadeonRDNA4: version / && version[n] != ""))
		start(t)
	pending = 0
	if (isKernel) kernelLine[n] = 1
	lines[n]++; last[n] = t; lastText[n] = $1 " " substr($2, 1, 8)
	line = substr($0, 1, 230)
	ring[n, lines[n] % TAIL] = line
	if ($0 ~ /Previous shutdown cause/) { cause[n] = $0; sub(/.*Previous shutdown cause: */, "", cause[n]) }
	if ($0 ~ /AMDRadeonRDNA4/) {
		ours[n]++; oring[n, ours[n] % OURS] = line
		if ($0 ~ /AMDRadeonRDNA4: version /) { version[n] = $0; sub(/.*AMDRadeonRDNA4: version /, "", version[n]) }
		if ($0 ~ /compute: stage|finished at stage|bring-up/) { bringup[n] = $0; sub(/.*AMDRadeonRDNA4: /, "", bringup[n]) }
	}
	next
}
/^=== system boot/ { if (lines[n] > 50) pending = 1 }
END {
	if (!n) { print "no kernel lines in the log for this period"; exit }
	for (i = 1; i <= n; i++) {
		# How boot i ended is what boot i+1 says about the shutdown before it.
		if (i == n) how = "still running (this boot)"
		else if (cause[i + 1] == "") how = "unknown (the next boot has no shutdown-cause line)"
		else if (cause[i + 1] + 0 == 5) how = "clean (the next boot says cause 5)"
		else { how = "CUT OFF (the next boot says cause " cause[i + 1] ")"; cut++ }
		printf "\nboot %d: %s to %s, up %d s, %d kernel lines; driver %s\n  ended: %s\n", i, firstText[i], lastText[i], last[i] - first[i], lines[i],
			(version[i] == "" ? "(no version line in the log)" : version[i]), how
		if (i < n) printf "  the next boot began %d s after its last line\n", first[i + 1] - last[i]
		if (bringup[i] != "") printf "  last bring-up line: %s\n", substr(bringup[i], 1, 170)
		if (cause[i + 1] != "" || i == n) known++
		# Without a shutdown cause to go by, the last lines are only worth printing for a boot that ended young.
		if (how ~ /^CUT OFF/ || (how ~ /^unknown/ && last[i] - first[i] < 300)) {
			printf "  --- its last %d kernel lines:\n", (lines[i] < TAIL ? lines[i] : TAIL)
			for (k = lines[i] - TAIL + 1; k <= lines[i]; k++) if (k > 0) print "  " ring[i, k % TAIL]
			if (ours[i]) {
				printf "  --- the last %d lines of this driver in it:\n", (ours[i] < OURS ? ours[i] : OURS)
				for (k = ours[i] - OURS + 1; k <= ours[i]; k++) if (k > 0) print "  " oring[i, k % OURS]
			}
		}
	}
	printf "\n%d boot(s) in the period, %d cut off\n", n, cut
	if (known <= 1 && n > 1) {
		print "No boot here has a shutdown-cause line: on this system the early kernel lines, and this driver, are not in"
		print "the log on disk (Big Sur keeps them in dmesg only). What is left to go by: a boot much shorter than the"
		print "others, followed by a longer pause than a restart takes, is what a freeze and a reset look like."
		print "freeze-watch.sh keeps the lines this cannot see."
	}
}'

echo
echo "=== EFI partitions: mounted? and a read-only check (nothing is changed) ==="
for dev in $(diskutil list 2>/dev/null | awk '/ EFI / { print $NF }'); do
	where=$(mount | awk -v d="/dev/$dev" '$1 == d { print $3 }')
	echo "$dev: ${where:+MOUNTED at $where}${where:-not mounted}"
	[ -n "$where" ] && ls -la "$where/EFI/OC/config.plist" "$where/nvram.plist" "$where/EFI/OC/Drivers/OpenVariableRuntimeDxe.efi" 2>/dev/null | sed 's/^/  /'
	fsck_msdos -n "/dev/r$dev" 2>&1 | tail -4 | sed 's/^/  fsck_msdos -n: /'
done

echo
echo "=== NVRAM: does a value survive a reboot? ==="
now="set $(date '+%Y-%m-%d %H:%M:%S') in boot $(sysctl -n kern.bootsessionuuid 2>/dev/null)"
before=$(nvram rdna4-nvtest 2>/dev/null | cut -f2-)
if [ -z "$before" ]; then
	echo "no rdna4-nvtest yet (first run of this script, or NVRAM did not keep it); run this script again after the next reboot"
elif echo "$before" | grep -q "$(sysctl -n kern.bootsessionuuid 2>/dev/null)"; then
	echo "rdna4-nvtest was set earlier in this same boot ($before): run this script again after the next reboot"
else
	echo "KEPT across a reboot: rdna4-nvtest = $before"
fi
nvram rdna4-nvtest="$now" 2>&1 | sed 's/^/  nvram: /'
echo "OpenCore: $(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version 2>/dev/null | cut -f2- || true)"
echo "this driver's breadcrumb now: $(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:rdna4-trail 2>/dev/null | cut -f2- || true)"
echo
echo "written to $OUT"
