#!/bin/sh
# radv-app.sh <App.app> <vkcube.app> [<out.app>]: a copy of a Vulkan app that links MoltenVK (vkQuake) running on RADV instead.
# The copy's libMoltenVK.dylib becomes Khronos's loader (with MoltenVK's install name, so SDL's own lookup finds the same image),
# the driver and its manifest from build-app.sh's vkcube.app go beside it, a /usr/local/lib rpath (Homebrew's MoltenVK first) is
# dropped, and the copy is signed ad hoc. The original stays as it is. Run the copy with the loader kept to our driver:
#   open <out.app> --env VK_DRIVER_FILES=<out.app>/Contents/Resources/vulkan/icd.d/radeon_icd.json
# (after an app crashed, AppKit asks whether to reopen its windows, and waits: --args -ApplePersistenceIgnoreState YES skips that)
set -eu
usage='usage: radv-app.sh <App.app> <vkcube.app> [<out.app>]'
app=${1:?$usage}
cube=${2:?$usage}
out=${3:-$(dirname "$app")/$(basename "$app" .app)-radv.app}
[ ! -e "$out" ] || { echo "$out exists: remove it first" >&2; exit 1; }
exe=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")
[ -f "$app/Contents/Frameworks/libMoltenVK.dylib" ] || { echo "$app has no Contents/Frameworks/libMoltenVK.dylib" >&2; exit 1; }

cp -R "$app" "$out"
cp "$cube/Contents/MacOS/libvulkan.dylib" "$out/Contents/Frameworks/libMoltenVK.dylib"
install_name_tool -id @rpath/libMoltenVK.dylib "$out/Contents/Frameworks/libMoltenVK.dylib" 2> /dev/null
cp "$cube/Contents/Frameworks/libvulkan_radeon.dylib" "$out/Contents/Frameworks/"
mkdir -p "$out/Contents/Resources/vulkan/icd.d"
cp "$cube/Contents/Resources/vulkan/icd.d/radeon_icd.json" "$out/Contents/Resources/vulkan/icd.d/"
if otool -l "$out/Contents/MacOS/$exe" | grep -q 'path /usr/local/lib '; then
	install_name_tool -delete_rpath /usr/local/lib "$out/Contents/MacOS/$exe" 2> /dev/null
fi
codesign --force --deep -s - "$out"
echo "made $out: open it with --env VK_DRIVER_FILES=$out/Contents/Resources/vulkan/icd.d/radeon_icd.json"
