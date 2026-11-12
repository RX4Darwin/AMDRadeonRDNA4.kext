#!/bin/sh
# Install the log follower as a LaunchDaemon (org.rdna4.logd), run as root from the top of the repository:
#   sudo userspace/rdna4-logd/install.sh
# Copies rdna4-logd (make userspace) into /usr/local/libexec and (re)loads the daemon; the kext's log of every boot then lands in
# /var/log/rdna4/boot-<boot time>.log, the last 10 boots kept.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
dest=/usr/local/libexec/rdna4-logd
plist=/Library/LaunchDaemons/org.rdna4.logd.plist
bin=$(ls -d Build/AMDRadeonRDNA4-*-RELEASE/bin 2> /dev/null | head -1)

[ -x "$bin/rdna4-logd" ] || { echo "missing: $bin/rdna4-logd (make userspace)" >&2; exit 1; }

launchctl bootout system "$plist" 2> /dev/null || true
mkdir -p /usr/local/libexec
cp "$bin/rdna4-logd" "$dest"

cat > "$plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>org.rdna4.logd</string>
	<key>ProgramArguments</key>
	<array>
		<string>$dest</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>ThrottleInterval</key>
	<integer>30</integer>
</dict>
</plist>
PLIST
chown root:wheel "$plist"
chmod 644 "$plist"
launchctl bootstrap system "$plist"
echo "installed $dest; the log of this boot: ls -t /var/log/rdna4 | head -1"
