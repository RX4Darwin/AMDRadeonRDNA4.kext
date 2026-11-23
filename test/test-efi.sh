#!/bin/sh
# tools/package/efi.sh against throwaway EFIs: install, its way back, and what it must refuse.
set -eu
EFISH=$(cd "$(dirname "$0")/.." && pwd)/tools/package/efi.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/rdna4-efi-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
GUID=7C436110-AB2A-4BBB-A880-FE41995C9F82
PB=/usr/libexec/PlistBuddy
n=0
pass() { n=$((n + 1)); }
fail() { echo "test-efi: FAIL: $*" >&2; exit 1; }
get() { "$PB" -c "Print :$2" "$1" 2> /dev/null; }

fake_kext() {   # dir, version
	mkdir -p "$1/Contents/MacOS"
	"$PB" -c "Add :CFBundleVersion string $2" "$1/Contents/Info.plist" > /dev/null
	echo "$2" > "$1/Contents/MacOS/$(basename "$1" .kext)"
}
add_kext() {   # config, bundle, enabled
	i=$("$PB" -c "Print :Kernel:Add" "$1" | grep -c 'BundlePath' || true)
	"$PB" -c "Add :Kernel:Add:$i dict" -c "Add :Kernel:Add:$i:BundlePath string $2" -c "Add :Kernel:Add:$i:Enabled bool $3" "$1" > /dev/null
}
make_efi() {   # dir
	mkdir -p "$1/EFI/OC/Kexts"
	c=$1/EFI/OC/config.plist
	"$PB" -c "Add :Kernel:Add array" -c "Add :NVRAM:Add:$GUID dict" -c "Add :NVRAM:Delete:$GUID array" \
	      -c "Add :NVRAM:Add:$GUID:boot-args string -v keepsyms=1 rdna4-compute=3 rdna4-trace=1 agdpmod=pikera" \
	      -c "Add :NVRAM:Delete:$GUID: string csr-active-config" -c "Add :Misc:Security:SecureBootModel string Disabled" "$c" > /dev/null
	plutil -insert "NVRAM.Add.$GUID.csr-active-config" -data AwAAAA== "$c"   # (PlistBuddy would store the text itself)
	add_kext "$c" Lilu.kext true
	add_kext "$c" VirtualSMC.kext true
	add_kext "$c" WhateverGreen.kext true
	fake_kext "$1/EFI/OC/Kexts/Lilu.kext" 1.7.2
}
index() { i=0; while b=$(get "$1" "Kernel:Add:$i:BundlePath"); do [ "$b" = "$2" ] && { echo "$i"; return; }; i=$((i + 1)); done; echo none; }

mkdir "$T/new" "$T/newer"
fake_kext "$T/new/AMDRadeonRDNA4.kext" 1.0
fake_kext "$T/newer/AMDRadeonRDNA4.kext" 2.0
E=$T/efi; C=$E/EFI/OC/config.plist
make_efi "$E"
cp "$C" "$T/original.plist"

# A dry run writes nothing
sh "$EFISH" install --efi "$E" --kext "$T/new/AMDRadeonRDNA4.kext" --dry-run > /dev/null
cmp -s "$C" "$T/original.plist" && [ ! -e "$E/EFI/OC/Kexts/AMDRadeonRDNA4.kext" ] && [ ! -e "$E/rdna4-backups" ] || fail "the dry run wrote something"; pass

# check on the untouched config fails
if sh "$EFISH" check --efi "$E" > /dev/null; then fail "check passed without the kext"; fi; pass

# install
sh "$EFISH" install --efi "$E" --kext "$T/new/AMDRadeonRDNA4.kext" > /dev/null
[ "$(index "$C" AMDRadeonRDNA4.kext)" = 1 ] || fail "the kext is not right after Lilu"; pass
[ "$(get "$C" "Kernel:Add:1:ExecutablePath")" = Contents/MacOS/AMDRadeonRDNA4 ] || fail "the entry's executable path"; pass
[ "$(get "$C" "Kernel:Add:$(index "$C" WhateverGreen.kext):Enabled")" = false ] || fail "WhateverGreen is still on"; pass
[ "$(get "$C" "NVRAM:Add:$GUID:boot-args")" = "-v keepsyms=1 rdna4-trace=1 amfi_get_out_of_my_way=0x1" ] || fail "boot-args: $(get "$C" "NVRAM:Add:$GUID:boot-args")"; pass
get "$C" "NVRAM:Delete:$GUID" | grep -q boot-args || fail "boot-args is not under NVRAM Delete"; pass
[ "$(plutil -extract "NVRAM.Add.$GUID.csr-active-config" raw -o - "$C")" = "AwAAAA==" ] || fail "csr-active-config changed"; pass
sh "$EFISH" check --efi "$E" > /dev/null || fail "check fails after the install"; pass
cp "$C" "$T/installed.plist"

# A second install changes nothing in the config, replaces the kext, and its way back returns to the first
sleep 1
sh "$EFISH" install --efi "$E" --kext "$T/newer/AMDRadeonRDNA4.kext" > /dev/null
[ "$("$PB" -c 'Print :Kernel:Add' "$C" | grep -c 'BundlePath = AMDRadeonRDNA4.kext')" = 1 ] || fail "a second entry"; pass
[ "$(cat "$E/EFI/OC/Kexts/AMDRadeonRDNA4.kext/Contents/MacOS/AMDRadeonRDNA4")" = 2.0 ] || fail "the kext was not replaced"; pass
sh "$EFISH" remove --efi "$E" > /dev/null
cmp -s "$C" "$T/installed.plist" || fail "remove did not restore the first install's config"; pass
[ "$(cat "$E/EFI/OC/Kexts/AMDRadeonRDNA4.kext/Contents/MacOS/AMDRadeonRDNA4")" = 1.0 ] || fail "remove did not put the old kext back"; pass
if sh "$EFISH" remove --efi "$E" > /dev/null 2>&1; then fail "a second remove ran"; fi; pass

# The way back from a first install: the config byte for byte, no kext left in Kexts
E2=$T/efi2; C2=$E2/EFI/OC/config.plist
make_efi "$E2"
cp "$C2" "$T/original2.plist"
sh "$EFISH" install --efi "$E2" --kext "$T/new/AMDRadeonRDNA4.kext" > /dev/null
sh "$EFISH" remove --efi "$E2" > /dev/null
cmp -s "$C2" "$T/original2.plist" || fail "remove did not restore config.plist exactly"; pass
[ ! -e "$E2/EFI/OC/Kexts/AMDRadeonRDNA4.kext" ] || fail "the kext stayed in Kexts"; pass

# An entry before Lilu is moved after it
E3=$T/efi3; C3=$E3/EFI/OC/config.plist
make_efi "$E3"
"$PB" -c "Add :Kernel:Add:0 dict" -c "Add :Kernel:Add:0:BundlePath string AMDRadeonRDNA4.kext" -c "Add :Kernel:Add:0:Enabled bool false" "$C3" > /dev/null
sh "$EFISH" install --efi "$E3" --kext "$T/new/AMDRadeonRDNA4.kext" > /dev/null
[ "$(index "$C3" Lilu.kext)" = 0 ] && [ "$(index "$C3" AMDRadeonRDNA4.kext)" = 1 ] || fail "the entry before Lilu was not moved"; pass

# Refused, and nothing written: no Lilu; Lilu too old
E4=$T/efi4; C4=$E4/EFI/OC/config.plist
make_efi "$E4"
"$PB" -c "Delete :Kernel:Add:0" "$C4"
cp "$C4" "$T/nolilu.plist"
if sh "$EFISH" install --efi "$E4" --kext "$T/new/AMDRadeonRDNA4.kext" > /dev/null 2>&1; then fail "installed without Lilu"; fi
cmp -s "$C4" "$T/nolilu.plist" && [ ! -e "$E4/rdna4-backups" ] || fail "a refused install wrote something"; pass
E5=$T/efi5; C5=$E5/EFI/OC/config.plist
make_efi "$E5"
"$PB" -c "Set :CFBundleVersion 1.6.9" "$E5/EFI/OC/Kexts/Lilu.kext/Contents/Info.plist"
cp "$C5" "$T/oldlilu.plist"
if sh "$EFISH" install --efi "$E5" --kext "$T/new/AMDRadeonRDNA4.kext" > /dev/null 2>&1; then fail "installed with Lilu 1.6.9"; fi
cmp -s "$C5" "$T/oldlilu.plist" || fail "a refused install changed config.plist"; pass

# AMFIPass on: no amfi_get_out_of_my_way added
E6=$T/efi6; C6=$E6/EFI/OC/config.plist
make_efi "$E6"
add_kext "$C6" AMFIPass.kext true
sh "$EFISH" install --efi "$E6" --kext "$T/new/AMDRadeonRDNA4.kext" > /dev/null
case " $(get "$C6" "NVRAM:Add:$GUID:boot-args") " in *amfi_get_out*) fail "AMFI boot-arg added beside AMFIPass" ;; esac; pass

# Nothing left in boot-args is still a valid install
E7=$T/efi7; C7=$E7/EFI/OC/config.plist
make_efi "$E7"
add_kext "$C7" AMFIPass.kext true
"$PB" -c "Set :NVRAM:Add:$GUID:boot-args rdna4-compute=3" "$C7"
sh "$EFISH" install --efi "$E7" --kext "$T/new/AMDRadeonRDNA4.kext" > /dev/null || fail "an empty boot-args stopped the install"
[ -z "$(get "$C7" "NVRAM:Add:$GUID:boot-args")" ] || fail "boot-args should be empty"; pass

echo "all $n EFI script tests passed"
