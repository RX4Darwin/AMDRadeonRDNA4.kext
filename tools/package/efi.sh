#!/bin/sh
# AMDRadeonRDNA4 in an OpenCore EFI, with a way back. It needs a mounted EFI and never mounts one.
#
#   sh efi.sh check   --efi /Volumes/EFI                what is set and what is wrong; changes nothing
#   sh efi.sh install --efi /Volumes/EFI --kext AMDRadeonRDNA4.kext [--pkg AMDRadeonRDNA4-x.y.z.pkg] [--dry-run]
#   sh efi.sh remove  --efi /Volumes/EFI                back to the config.plist and kext from before the last install
#
# install keeps the old config.plist (and kext) in EFI/rdna4-backups/efi-<time>/, adds the kext after Lilu, turns off the kexts
# that claim the card, removes old rdna4-* and WhateverGreen boot-args (rdna4-trace stays), adds amfi_get_out_of_my_way=0x1 unless
# AMFIPass is on, lists boot-args under NVRAM -> Delete so that OpenCore applies them, and sets ResizeAppleGpuBars to 10 unless it
# is 0 to 10 (macOS gives a larger VRAM BAR no address: Resizable BAR on would leave it without a desktop). SIP is reported, never changed.
# --pkg needs root. --dry-run prints the result and writes nothing.
set -eu
set -f   # (boot-args are split on spaces, never expanded)

GUID=7C436110-AB2A-4BBB-A880-FE41995C9F82
KEXT_NAME=AMDRadeonRDNA4.kext
OTHERS="WhateverGreen.kext NootRX.kext RDNA4FB.kext Navi48Bringup.kext"

die() { echo "efi.sh: $*" >&2; exit 1; }
pb() { /usr/libexec/PlistBuddy -c "$2" "$1" 2> /dev/null; }

# The index of the Kernel -> Add entry with this BundlePath; fails when there is none.
entry() {
	_i=0
	while _b=$(pb "$1" "Print :Kernel:Add:$_i:BundlePath"); do
		if [ "$_b" = "$2" ]; then echo "$_i"; return 0; fi
		_i=$((_i + 1))
	done
	return 1
}
enabled() { _n=$(entry "$1" "$2") && [ "$(pb "$1" "Print :Kernel:Add:$_n:Enabled")" = true ]; }
in_delete() { pb "$1" "Print :NVRAM:Delete:$GUID" | sed 's/^ *//' | grep -qx "$2"; }

ok() { echo "  ok    $*"; }
warn() { echo "  WARN  $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }

# What a config needs. $1 the config, $2 our kext (where it is or will be), $3 EFI/OC/Kexts.
check_config() {
	FAILS=0
	if plutil -lint -s "$1" > /dev/null; then ok "config.plist is a valid plist"; else bad "config.plist is not a valid plist"; return 0; fi

	if l=$(entry "$1" Lilu.kext) && enabled "$1" Lilu.kext; then
		v=$(pb "$3/Lilu.kext/Contents/Info.plist" "Print :CFBundleVersion") || v=
		if [ -z "$v" ]; then bad "Lilu.kext is not in EFI/OC/Kexts"
		elif [ "$(printf '%s\n1.7.1\n' "$v" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)" = 1.7.1 ]; then ok "Lilu $v"
		else bad "Lilu $v: 1.7.1 or newer is needed"; fi
	else
		l=; bad "Lilu.kext is not enabled in Kernel -> Add"
	fi

	if n=$(entry "$1" "$KEXT_NAME") && enabled "$1" "$KEXT_NAME"; then
		if [ -n "$l" ] && [ "$n" -gt "$l" ]; then ok "$KEXT_NAME is enabled, after Lilu"; else bad "$KEXT_NAME must come after Lilu"; fi
		if [ -f "$2/Contents/Info.plist" ] && [ -f "$2/Contents/MacOS/AMDRadeonRDNA4" ]; then ok "the kext's files are there"
		else bad "$2 is not a complete kext"; fi
	else
		bad "$KEXT_NAME is not enabled in Kernel -> Add"
	fi

	for k in $OTHERS; do
		if enabled "$1" "$k"; then bad "$k is enabled and claims the card"; fi
	done

	if in_delete "$1" boot-args; then ok "boot-args is under NVRAM -> Delete: OpenCore applies it"
	else bad "boot-args is not under NVRAM -> Delete: OpenCore keeps the old value (\"ignored, exists\")"; fi

	# SIP: Filesystem Protections off (bit 0x2), or WindowServer will not load the Metal driver
	csr=$(plutil -extract "NVRAM.Add.$GUID.csr-active-config" raw -o - "$1" 2> /dev/null | base64 -D | od -An -tu1 -N1 | tr -d ' ') || csr=
	if [ -n "$csr" ] && [ $((csr & 2)) -ne 0 ]; then ok "csr-active-config has Filesystem Protections off"
	else warn "csr-active-config needs bit 0x2 (usually 03000000) for the Metal driver; not changed here"; fi
	if ! in_delete "$1" csr-active-config; then warn "csr-active-config is not under NVRAM -> Delete: a stored value wins"; fi

	s=$(pb "$1" "Print :Misc:Security:SecureBootModel") || s=
	if [ "$s" = Disabled ]; then ok "SecureBootModel is Disabled"; else warn "SecureBootModel is ${s:-missing}: Apple Secure Boot refuses this kext (use Disabled)"; fi

	# Resizable BAR: macOS gives a VRAM BAR above 1 GiB no address (no desktop); 10 caps it at 1 GiB for macOS only
	r=$(pb "$1" "Print :Booter:Quirks:ResizeAppleGpuBars") || r=-1
	case $r in
	-1) warn "ResizeAppleGpuBars is -1: with Resizable BAR on in the BIOS, macOS gets no VRAM BAR and no desktop (use 10, 1 GiB)" ;;
	[0-9]|10) ok "ResizeAppleGpuBars is $r: macOS gets a VRAM BAR of at most $(( r < 8 ? 256 : 1 << r )) MiB" ;;
	*) bad "ResizeAppleGpuBars is $r: macOS gives a VRAM BAR above 1 GiB no address (use 10)" ;;
	esac
}

# The config's changes for an install, in place.
edit_config() {
	l=$(entry "$1" Lilu.kext) || die "no Lilu.kext in Kernel -> Add: add Lilu first"
	enabled "$1" Lilu.kext || die "Lilu.kext is disabled in Kernel -> Add"

	for k in $OTHERS; do
		if enabled "$1" "$k"; then
			pb "$1" "Set :Kernel:Add:$(entry "$1" "$k"):Enabled false"
			echo "  turned off $k"
		fi
	done

	n=$(entry "$1" "$KEXT_NAME") || n=
	if [ -n "$n" ] && [ "$n" -lt "$l" ]; then   # before Lilu: taken out and put back below
		pb "$1" "Delete :Kernel:Add:$n"
		l=$((l - 1)); n=
		echo "  moved $KEXT_NAME after Lilu"
	fi
	if [ -z "$n" ]; then
		n=$((l + 1))
		pb "$1" "Add :Kernel:Add:$n dict" || die "could not add the Kernel -> Add entry"
		pb "$1" "Add :Kernel:Add:$n:Arch string x86_64"
		pb "$1" "Add :Kernel:Add:$n:BundlePath string $KEXT_NAME"
		pb "$1" "Add :Kernel:Add:$n:Comment string AMDRadeonRDNA4"
		pb "$1" "Add :Kernel:Add:$n:Enabled bool true"
		pb "$1" "Add :Kernel:Add:$n:ExecutablePath string Contents/MacOS/AMDRadeonRDNA4"
		pb "$1" "Add :Kernel:Add:$n:MaxKernel string"
		pb "$1" "Add :Kernel:Add:$n:MinKernel string"
		pb "$1" "Add :Kernel:Add:$n:PlistPath string Contents/Info.plist"
		echo "  added $KEXT_NAME after Lilu"
	fi
	pb "$1" "Set :Kernel:Add:$n:Enabled true"

	pb "$1" "Print :NVRAM:Add:$GUID" > /dev/null || die "no NVRAM -> Add -> $GUID in config.plist"
	old=$(pb "$1" "Print :NVRAM:Add:$GUID:boot-args") || old=
	new= dropped=
	for a in $old; do
		case $a in
		rdna4-trace=*) new="$new $a" ;;
		rdna4-*|agdpmod=*|shikigva=*|-wegnoegpu) dropped="$dropped $a" ;;
		*) new="$new $a" ;;
		esac
	done
	case " $new " in
	*" amfi_get_out_of_my_way="*) ;;
	*) enabled "$1" AMFIPass.kext || new="$new amfi_get_out_of_my_way=0x1" ;;
	esac
	new=${new# }
	[ -z "$dropped" ] || echo "  removed boot-args:$dropped"
	pb "$1" "Delete :NVRAM:Add:$GUID:boot-args" || true   # (replaced: Set refuses an empty value)
	pb "$1" "Add :NVRAM:Add:$GUID:boot-args string $new" || die "could not set boot-args"
	echo "  boot-args: $new"

	r=$(pb "$1" "Print :Booter:Quirks:ResizeAppleGpuBars") || r=
	case $r in
	[0-9]|10) ;;
	*)	pb "$1" "Delete :Booter:Quirks:ResizeAppleGpuBars" || true
		pb "$1" "Add :Booter:Quirks:ResizeAppleGpuBars integer 10" || die "could not set ResizeAppleGpuBars"
		echo "  ResizeAppleGpuBars: ${r:-missing} -> 10 (macOS's VRAM BAR at most 1 GiB, also with Resizable BAR on)" ;;
	esac

	pb "$1" "Print :NVRAM:Delete:$GUID" > /dev/null || pb "$1" "Add :NVRAM:Delete:$GUID array" || die "could not add NVRAM -> Delete -> $GUID"
	if ! in_delete "$1" boot-args; then
		pb "$1" "Add :NVRAM:Delete:$GUID: string boot-args" || die "could not list boot-args under NVRAM -> Delete"
		echo "  listed boot-args under NVRAM -> Delete"
	fi
}

do_install() {
	[ -n "$KEXT" ] || die "install needs --kext"
	[ -f "$KEXT/Contents/MacOS/AMDRadeonRDNA4" ] || die "$KEXT is not the driver's kext"
	if [ -n "$PKG" ]; then
		[ -f "$PKG" ] || die "no package at $PKG"
		[ "$(id -u)" -eq 0 ] || [ "$DRY" = 1 ] || die "--pkg needs root: run with sudo"
	fi
	work=$(mktemp "${TMPDIR:-/tmp}/rdna4-config.XXXXXX")
	trap 'rm -f "$work"' EXIT
	cp "$CFG" "$work"
	echo "Changes:"
	edit_config "$work"
	echo "Result:"
	check_config "$work" "$KEXT" "$KEXTS"
	[ "$FAILS" -eq 0 ] || die "$FAILS problem(s) above: nothing was written"
	if [ "$DRY" = 1 ]; then echo "Dry run: nothing was written."; return 0; fi

	back=$EFI/rdna4-backups/efi-$(date -u +%Y%m%d-%H%M%S)
	[ ! -e "$back" ] || die "$back already exists"
	mkdir -p "$back"
	cp "$CFG" "$back/config.plist"
	cmp -s "$CFG" "$back/config.plist" || die "the backup of config.plist does not match: nothing was changed"

	rm -rf "$KEXTS/.$KEXT_NAME.new"
	COPYFILE_DISABLE=1 cp -R "$KEXT" "$KEXTS/.$KEXT_NAME.new"
	if [ -e "$KEXTS/$KEXT_NAME" ]; then mv "$KEXTS/$KEXT_NAME" "$back/$KEXT_NAME"; fi
	mv "$KEXTS/.$KEXT_NAME.new" "$KEXTS/$KEXT_NAME"
	cp "$work" "$CFG.new"
	mv "$CFG.new" "$CFG"
	echo "${back##*/}" > "$EFI/rdna4-backups/latest"
	echo "Installed. The way back: sh efi.sh remove --efi $EFI (backup in $back)"
	if [ -n "$PKG" ]; then installer -pkg "$PKG" -target /; fi
}

do_remove() {
	latest=$EFI/rdna4-backups/latest
	[ -f "$latest" ] || die "no install of this script to undo (no $latest)"
	back=$EFI/rdna4-backups/$(cat "$latest")
	[ -f "$back/config.plist" ] || die "the backup $back has no config.plist"
	plutil -lint -s "$back/config.plist" > /dev/null || die "the backup config.plist is not valid: nothing was changed"

	cp "$CFG" "$back/config.plist.replaced"
	cp "$back/config.plist" "$CFG.new"
	cmp -s "$back/config.plist" "$CFG.new" || die "could not copy the backup: nothing was changed"
	if [ -e "$KEXTS/$KEXT_NAME" ]; then mv "$KEXTS/$KEXT_NAME" "$back/$KEXT_NAME.removed"; fi
	if [ -e "$back/$KEXT_NAME" ]; then mv "$back/$KEXT_NAME" "$KEXTS/$KEXT_NAME"; fi
	mv "$CFG.new" "$CFG"
	mv "$latest" "$latest.used"
	echo "Back to the config.plist from before the install ($back)."
}

[ $# -ge 1 ] || die "usage: efi.sh check|install|remove --efi <mounted EFI> [--kext <kext>] [--pkg <pkg>] [--dry-run]"
CMD=$1; shift
EFI= KEXT= PKG= DRY=0
while [ $# -gt 0 ]; do
	case $1 in
	--efi) [ $# -ge 2 ] || die "--efi needs a path"; EFI=${2%/}; shift 2 ;;
	--kext) [ $# -ge 2 ] || die "--kext needs a path"; KEXT=${2%/}; shift 2 ;;
	--pkg) [ $# -ge 2 ] || die "--pkg needs a path"; PKG=$2; shift 2 ;;
	--dry-run) DRY=1; shift ;;
	*) die "unknown argument $1" ;;
	esac
done
[ -n "$EFI" ] || die "--efi <mounted EFI> is required"
CFG=$EFI/EFI/OC/config.plist
KEXTS=$EFI/EFI/OC/Kexts
[ -f "$CFG" ] || die "no config.plist at $CFG"

case $CMD in
check)
	check_config "$CFG" "$KEXTS/$KEXT_NAME" "$KEXTS"
	[ "$FAILS" -eq 0 ] || exit 1
	;;
install) do_install ;;
remove) do_remove ;;
*) die "unknown command $CMD" ;;
esac
