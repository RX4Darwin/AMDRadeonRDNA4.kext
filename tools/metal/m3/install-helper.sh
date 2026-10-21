#!/bin/sh
# Install the Metal kernel translator helper (air2amdgpu --serve, built by tools/metal/m3/build.sh helper) on the rig as a root LaunchDaemon,
# so the fake paravirtual GPU's host can translate any Metal compute function at run time (include/rdna4mtlc.h). Log: /var/log/rdna4-air2amdgpu.log
#
#   sudo ./install-helper.sh ./air2amdgpu        install (or replace) and start it
#   sudo ./install-helper.sh --remove            stop and remove it
set -eu
label=com.hackintosh.AMDRadeonRDNA4.air2amdgpu
plist=/Library/LaunchDaemons/$label.plist
bin=/usr/local/libexec/rdna4-air2amdgpu
launchctl bootout "system/$label" 2>/dev/null || true
if [ "${1:-}" = --remove ]; then
	rm -f "$plist" "$bin"
	echo "removed"
	exit 0
fi
[ -f "${1:-}" ] || { echo "usage: sudo $0 <air2amdgpu> | --remove" >&2; exit 2; }
install -d -o root -g wheel -m 755 /usr/local/libexec
install -o root -g wheel -m 755 "$1" "$bin"
cat > "$plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key><string>$label</string>
	<key>ProgramArguments</key><array><string>$bin</string><string>--serve</string></array>
	<key>RunAtLoad</key><true/>
	<key>KeepAlive</key><true/>
	<key>ThrottleInterval</key><integer>10</integer>
	<key>StandardErrorPath</key><string>/var/log/rdna4-air2amdgpu.log</string>
</dict>
</plist>
PLIST
chown root:wheel "$plist"
chmod 644 "$plist"
launchctl bootstrap system "$plist"
echo "installed and started: $label"
