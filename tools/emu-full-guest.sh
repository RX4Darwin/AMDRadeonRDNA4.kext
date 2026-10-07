#!/bin/bash
# Guest side of tools/emu-full.sh: runs in the Recovery/installer Terminal of the VM (root, bash 3.2), fetched from the host's HTTP server. Not for the Mac.
#
#   emu-full-guest.sh provision USER PASSWORD "ssh-ed25519 AAAA..."   create the admin user, Remote Login and the SSH key on the INSTALLED volume
#   emu-full-guest.sh verify                                          print the facts M0 needs from the installed volume
#
# provision does what Setup Assistant would, offline, on the Data volume of the installed system: a local admin user (dscl -f on the Data volume's
# dslocal node), /private/var/db/.AppleSetupDone (no Setup Assistant), sshd enabled in the system launchd override database, and the key in
# ~USER/.ssh/authorized_keys. Idempotent: running it again resets the password and the key.
set -u

die() { echo "emu-full-guest: $*"; exit 1; }

# The installed system's volumes: the System volume ("Macintosh HD", sealed) and its Data volume ("Macintosh HD - Data"). Mount both, Data read-write.
mount_target() {
	local name=${TARGET_NAME:-Macintosh HD}
	diskutil mount "$name" >/dev/null 2>&1 || true
	diskutil mount "$name - Data" >/dev/null 2>&1 || true
	SYS="/Volumes/$name"
	DATA="/Volumes/$name - Data"
	[ -d "$DATA/private/var/db" ] || die "the Data volume is not mounted at $DATA (diskutil list: is the install finished?)"
	mount -uw "$DATA" 2>/dev/null || true
	[ -d "$SYS/System/Library" ] || echo "emu-full-guest: note: the System volume is not mounted at $SYS"
}

provision() {
	local user=$1 pass=$2 pub=$3 uid=501
	[ -n "$user" ] && [ -n "$pass" ] && [ -n "$pub" ] || die "usage: provision USER PASSWORD \"ssh-ed25519 ...\""
	mount_target
	local ds="$DATA/private/var/db/dslocal/nodes/Default" u="/Local/Default/Users/$user"

	# the user (idempotent: -create on an existing record only overwrites the key)
	dscl -f "$ds" localhost -create "$u" || die "dscl create failed"
	dscl -f "$ds" localhost -create "$u" UserShell /bin/zsh
	dscl -f "$ds" localhost -create "$u" RealName "$user"
	dscl -f "$ds" localhost -create "$u" UniqueID "$uid"
	dscl -f "$ds" localhost -create "$u" PrimaryGroupID 20
	dscl -f "$ds" localhost -create "$u" NFSHomeDirectory "/Users/$user"
	dscl -f "$ds" localhost -create "$u" GeneratedUID "$(uuidgen)"
	dscl -f "$ds" localhost -passwd "$u" "$pass" || echo "emu-full-guest: warning: dscl -passwd failed (login by key still works only if the record has a hash)"
	dscl -f "$ds" localhost -delete /Local/Default/Groups/admin GroupMembership "$user" 2>/dev/null || true   # delete + merge: idempotent
	dscl -f "$ds" localhost -merge /Local/Default/Groups/admin GroupMembership "$user" 2>/dev/null || true
	local guid; guid=$(dscl -f "$ds" localhost -read "$u" GeneratedUID | awk '{print $2}')
	dscl -f "$ds" localhost -delete /Local/Default/Groups/admin GroupMembers "$guid" 2>/dev/null || true
	dscl -f "$ds" localhost -merge /Local/Default/Groups/admin GroupMembers "$guid" 2>/dev/null || true

	# the home and the key
	mkdir -p "$DATA/Users/$user/.ssh"
	printf '%s\n' "$pub" > "$DATA/Users/$user/.ssh/authorized_keys"
	chmod 700 "$DATA/Users/$user" "$DATA/Users/$user/.ssh"
	chmod 600 "$DATA/Users/$user/.ssh/authorized_keys"
	chown -R "$uid:20" "$DATA/Users/$user"

	# no Setup Assistant
	touch "$DATA/private/var/db/.AppleSetupDone"
	chown 0:80 "$DATA/private/var/db/.AppleSetupDone" 2>/dev/null || true

	# Remote Login = sshd not disabled in the system domain's override database (what `systemsetup -setremotelogin on` / launchctl enable writes)
	local dis="$DATA/private/var/db/com.apple.xpc.launchd/disabled.plist"
	mkdir -p "$(dirname "$dis")"
	[ -f "$dis" ] || plutil -create xml1 "$dis" 2>/dev/null || printf '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict/></plist>\n' > "$dis"
	defaults write "${dis%.plist}" com.openssh.sshd -bool false   # (plutil -replace says "Key path not found" on this file; defaults works)
	chown 0:0 "$dis"; chmod 644 "$dis"
	sync
	echo "provision: done: user $user (uid $uid, admin), Setup Assistant skipped, sshd enabled, key installed"
}

verify() {
	mount_target
	echo "--- version"
	cat "$SYS/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null | grep -A1 -E 'ProductVersion|ProductBuildVersion|ProductName' | grep -v '^--'
	echo "--- Apple paravirtual GPU files"
	find "$SYS/System/Library/Extensions" -maxdepth 1 -iname '*Paravirt*' 2>/dev/null
	find "$SYS/System/Library/Extensions" "$SYS/System/Library/DriverExtensions" -maxdepth 4 -iname '*Paravirt*' 2>/dev/null | head
	echo "--- kernel collections"
	ls -la "$SYS/System/Library/KernelCollections" 2>/dev/null
	for kc in "$SYS"/System/Library/KernelCollections/*.kc; do echo "$kc: $(grep -a -c com.apple.driver.AppleParavirtGPU "$kc") matches for com.apple.driver.AppleParavirtGPU (its code is in the collection; the bundle in Extensions has Info.plist only)"; done
	ls -la "$DATA/private/var/db/KernelExtensionManagement" 2>/dev/null | head
	ls -d "$SYS"/System/Volumes/Preboot/*/boot/*/System/Library/Caches/com.apple.kernelcaches 2>/dev/null
	ls -d /Volumes/*Preboot*/*/boot/*/System/Library/Caches/com.apple.kernelcaches 2>/dev/null
}

c=${1:-}; shift || true
case $c in
	provision) provision "$@" ;;
	verify) verify ;;
	*) die "provision|verify" ;;
esac
