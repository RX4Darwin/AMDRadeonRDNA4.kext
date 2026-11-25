#!/bin/sh
# add-fan-helper.sh: on a test machine whose driver bundle was installed without the package, the package's fan job by hand: the
# settings file RDNA4Gadget writes (group admin) and the LaunchDaemon org.rdna4.fan that applies it as root from the bundle's
# Helpers/rdna4-fan-apply, at once, at boot and when the file changes (tools/package/package.sh does the same).
#   sudo sh add-fan-helper.sh            install (again)
#   sudo sh add-fan-helper.sh --remove   remove it
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
label=org.rdna4.fan
plist=/Library/LaunchDaemons/$label.plist
d="/Library/Application Support/AMDRadeonRDNA4"
helper=/Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle/Contents/Helpers/rdna4-fan-apply
launchctl bootout system/$label 2> /dev/null || true
if [ "${1:-}" = --remove ]; then
	rm -rf "$plist" "$d"
	echo "removed $label"
	exit 0
fi
[ -x "$helper" ] || { echo "no $helper: install a driver bundle with it first" >&2; exit 1; }
mkdir -p "$d"
touch "$d/fan.conf"
chown root:admin "$d" "$d/fan.conf"
chmod 775 "$d"
chmod 664 "$d/fan.conf"
cat > "$plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key><string>$label</string>
	<key>ProgramArguments</key><array><string>$helper</string></array>
	<key>RunAtLoad</key><true/>
	<key>WatchPaths</key><array><string>$d/fan.conf</string></array>
	<key>StandardErrorPath</key><string>/var/log/rdna4-fan.log</string>
	<key>StandardOutPath</key><string>/var/log/rdna4-fan.log</string>
</dict>
</plist>
PLIST
chown root:wheel "$plist"
chmod 644 "$plist"
launchctl bootstrap system "$plist"
echo "installed and started: $label (settings in $d/fan.conf, log /var/log/rdna4-fan.log)"
