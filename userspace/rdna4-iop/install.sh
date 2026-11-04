#!/bin/sh
# Install the IOPresentment switch as a LaunchDaemon (org.rdna4.iop), run as root from the top of the repository:
#   sudo userspace/rdna4-iop/install.sh
# Copies rdna4-iop (make userspace) into /usr/local/libexec and (re)loads it; it runs once each boot and sets CoreDisplay's useIOP
# to what the boot needs (src/pvscanout.hpp), restarting WindowServer when that changed. Its output: /var/log/rdna4-iop.log.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
dest=/usr/local/libexec/rdna4-iop
plist=/Library/LaunchDaemons/org.rdna4.iop.plist
bin=$(ls -d Build/AMDRadeonRDNA4-*-RELEASE/bin 2> /dev/null | head -1)

[ -x "$bin/rdna4-iop" ] || { echo "missing: $bin/rdna4-iop (make userspace)" >&2; exit 1; }

launchctl bootout system "$plist" 2> /dev/null || true
mkdir -p /usr/local/libexec
cp "$bin/rdna4-iop" "$dest"

cat > "$plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>org.rdna4.iop</string>
	<key>ProgramArguments</key>
	<array>
		<string>$dest</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>StandardOutPath</key>
	<string>/var/log/rdna4-iop.log</string>
	<key>StandardErrorPath</key>
	<string>/var/log/rdna4-iop.log</string>
</dict>
</plist>
PLIST
chown root:wheel "$plist"
chmod 644 "$plist"
launchctl bootstrap system "$plist"
echo "installed $dest; what it did: /var/log/rdna4-iop.log"
