#!/bin/bash
#
# freeze-watch.sh — keep the kernel log of every boot's first minutes on disk, so that a boot that freezes leaves
# its last lines behind. On Big Sur the kernel lines of this driver are only in the kernel's own buffer (dmesg),
# which a freeze takes with it; the system log on disk does not have them (tools/rig/boot-history.sh showed that).
#
#   sudo bash freeze-watch.sh install [seconds]   start at every boot from now on (a launch daemon), and now;
#                                                 watch each boot for that long (default 300; 86400 = a day)
#   sudo bash freeze-watch.sh show         what the last boots left: when each was last alive, and its last lines
#   sudo bash freeze-watch.sh uninstall    stop and remove the daemon and its copy of this script (the records stay)
#
# What the daemon does: for the first five minutes after it starts, twice a second, it writes the whole of dmesg
# to one file for that boot in /var/log/rdna4-freeze-watch/ and asks for it to be flushed; after that, if it was
# installed for longer, once a second and only the last 400 lines (the whole buffer every second would be a lot of
# writing over a day). The file's last line is the time of that write, so the record of a frozen boot ends within
# about a second of the freeze. A boot that lives past the watched time gets a closing line, and the daemon exits.
# The last twelve boots are kept.
#
# install puts two files on the machine: /Library/LaunchDaemons/com.rx4darwin.rdna4.freeze-watch.plist and
# /usr/local/libexec/rdna4-freeze-watch.sh (a copy of this script, so that the daemon does not depend on where
# this one lies). uninstall removes both.
#
LABEL=com.rx4darwin.rdna4.freeze-watch
PLIST=/Library/LaunchDaemons/$LABEL.plist
COPY=/usr/local/libexec/rdna4-freeze-watch.sh
# The two below are only changed to try the script without root.
DIR=${WATCH_DIR:-/var/log/rdna4-freeze-watch}
DMESG=${WATCH_DMESG:-dmesg}
SECONDS_WATCHED=${WATCH_SECONDS:-300}

run() {
	mkdir -p "$DIR" || exit 1
	ls -t "$DIR"/boot-*.txt 2>/dev/null | tail -n +12 | while read -r old; do rm -f "$old"; done
	f="$DIR/boot-$(date +%Y%m%d-%H%M%S).txt"
	start=$(date +%s) i=0
	while [ $(($(date +%s) - start)) -lt "$SECONDS_WATCHED" ]; do
		i=$((i + 1))
		if [ $(($(date +%s) - start)) -lt 300 ]; then
			{ $DMESG 2>&1; echo "=== freeze-watch: alive at $(date '+%Y-%m-%d %H:%M:%S'), write $i ==="; } > "$f.new" && mv -f "$f.new" "$f"
			sync; sleep 0.5
		else
			{ $DMESG 2>&1 | tail -n 400; echo "=== freeze-watch: alive at $(date '+%Y-%m-%d %H:%M:%S'), write $i (the last 400 lines) ==="; } > "$f.new" && mv -f "$f.new" "$f"
			sync; sleep 1
		fi
	done
	echo "=== freeze-watch: this boot lived past the $SECONDS_WATCHED s that are watched ===" >> "$f"
	sync
}

show() {
	ls "$DIR"/boot-*.txt >/dev/null 2>&1 || { echo "no records in $DIR (run install, then reboot)"; exit 0; }
	echo "=== freeze-watch records in $DIR, oldest first ==="
	newest=$(ls -t "$DIR"/boot-*.txt | head -1)
	for f in $(ls -tr "$DIR"/boot-*.txt); do
		alive=$(grep "=== freeze-watch: alive at" "$f" | tail -1 | sed 's/.*alive at \([0-9: -]*\),.*/\1/')
		if grep -q "lived past" "$f"; then how="lived past the watched time"
		elif [ "$f" = "$newest" ]; then how="the newest record (this boot, if it is younger than the watched time)"
		else how="ENDED WHILE WATCHED, last alive at $alive (a freeze, or a shutdown in the first minutes)"; fi
		echo
		echo "$(basename "$f"): $(grep -c "" "$f") lines; $how"
		grep -E "AMDRadeonRDNA4: version|Previous shutdown cause" "$f" | cut -c1-160 | sed 's/^/  /'
		case "$how" in ENDED*)
			echo "  --- its last 60 kernel lines:"
			grep -v "=== freeze-watch" "$f" | tail -60 | cut -c1-230 | sed 's/^/  /'
			echo "  --- the last 20 lines of this driver in it:"
			grep "AMDRadeonRDNA4" "$f" | tail -20 | cut -c1-230 | sed 's/^/  /' ;;
		esac
	done
}

case "${1:-}" in
	run) [ -n "${2:-}" ] && SECONDS_WATCHED=$2; run ;;
	show) show ;;
	install)
		[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
		mkdir -p "$(dirname "$COPY")" && cp "$0" "$COPY" && chmod 755 "$COPY" || exit 1
		cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key><string>$LABEL</string>
	<key>ProgramArguments</key><array><string>/bin/bash</string><string>$COPY</string><string>run</string><string>${2:-300}</string></array>
	<key>RunAtLoad</key><true/>
</dict>
</plist>
EOF
		chown root:wheel "$PLIST" && chmod 644 "$PLIST"
		launchctl unload "$PLIST" 2>/dev/null
		launchctl load -w "$PLIST" && echo "installed: watching ${2:-300} s of every boot, from now; records in $DIR (sudo bash $0 show)" ;;
	uninstall)
		[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
		launchctl unload -w "$PLIST" 2>/dev/null
		rm -f "$PLIST" "$COPY"
		echo "removed the daemon; the records in $DIR stay (delete that directory to remove them)" ;;
	*) sed -n '2,/^LABEL=/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 2 ;;
esac
