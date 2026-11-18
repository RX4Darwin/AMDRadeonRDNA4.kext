#!/bin/sh
# Make Vulkan apps through MoltenVK (vkQuake, ...) start on the paravirtual GPU: run as yourself (not sudo), once:
#   userspace/moltenvk/install.sh
# Metal reports argument buffers on every device (Tier 1 at least), so MoltenVK makes an argument encoder at start-up, and Apple's
# paravirtual Metal driver for x86 has none: -[MTLIOAccelDevice newIndirectArgumentEncoderWithLayout:] calls abort() (Tahoe 26.6.2).
# MoltenVK's only switch for that is MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0. A LaunchAgent sets it in your login session (launchctl
# setenv) each login, so apps started from the Finder and the Dock have it; this one also sets it now. Undo: --remove.
set -eu
label=org.rdna4.moltenvk
plist=$HOME/Library/LaunchAgents/$label.plist
domain=gui/$(id -u)

[ "$(id -u)" -ne 0 ] || { echo "run as yourself, not with sudo" >&2; exit 1; }
launchctl bootout "$domain" "$plist" 2> /dev/null || true

if [ "${1:-}" = --remove ]; then
	rm -f "$plist"
	launchctl unsetenv MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS
	echo "removed $plist"
	exit 0
fi

mkdir -p "$HOME/Library/LaunchAgents"
cat > "$plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$label</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/launchctl</string>
		<string>setenv</string>
		<string>MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS</string>
		<string>0</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
</dict>
</plist>
PLIST
plutil -lint -s "$plist"
launchctl bootstrap "$domain" "$plist"
echo "installed $plist; MoltenVK apps started from now on run without Metal argument buffers"
