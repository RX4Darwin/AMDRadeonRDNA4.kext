#!/bin/sh
# build.sh <out dir> <version>: RDNA4Gadget.app (x86_64, macOS 13 or later), ad-hoc signed, its icon made from AppIcon.png (1024x1024).
# `make gadget` runs it.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?out dir}
version=${2:-0.0.0}
app="$out/RDNA4Gadget.app"
tmp=$(mktemp -d)
cp "$here/check.swift" "$tmp/main.swift"   # (top-level code: compiled as a main.swift)
xcrun swiftc -target x86_64-apple-macos13.0 -o "$tmp/check" "$tmp/main.swift" "$here"/Driver.swift
"$tmp/check"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
xcrun swiftc -O -target x86_64-apple-macos13.0 -sdk "$(xcrun --show-sdk-path)" -module-name RDNA4Gadget \
	-o "$app/Contents/MacOS/RDNA4Gadget" "$here"/main.swift "$here"/Model.swift "$here"/Views.swift "$here"/Sensors.swift \
	"$here"/Driver.swift "$here"/Fan.swift
sed "s/@VERSION@/$version/g" "$here/Info.plist" > "$app/Contents/Info.plist"
# The icon: an asset catalog's (Assets.car, and AppIcon.icns for older systems) from AppIcon.png, a full-bleed square that macOS masks
# itself (an icon with its own rounded corners and transparency sat on a white plate on Tahoe)
set="$tmp/Assets.xcassets/AppIcon.appiconset"
mkdir -p "$set"
images=
for s in 16 32 128 256 512; do
	for k in 1 2; do
		sips -z $((s * k)) $((s * k)) "$here/AppIcon.png" --out "$set/icon_${s}_${k}x.png" > /dev/null
		images="$images{\"idiom\":\"mac\",\"size\":\"${s}x${s}\",\"scale\":\"${k}x\",\"filename\":\"icon_${s}_${k}x.png\"},"
	done
done
printf '{"images":[%s],"info":{"version":1,"author":"xcode"}}\n' "${images%,}" > "$set/Contents.json"
printf '{"info":{"version":1,"author":"xcode"}}\n' > "$tmp/Assets.xcassets/Contents.json"
xcrun actool "$tmp/Assets.xcassets" --compile "$app/Contents/Resources" --platform macosx --minimum-deployment-target 13.0 \
	--app-icon AppIcon --output-partial-info-plist "$tmp/icon.plist" > /dev/null
rm -rf "$tmp"
codesign --force --sign - "$app"
echo "built $app ($version)"
