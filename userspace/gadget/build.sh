#!/bin/sh
# build.sh <out dir> <version>: RDNA4 Gadget.app (x86_64, macOS 13 or later), ad-hoc signed. `make gadget` runs it.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?out dir}
version=${2:-0.0.0}
app="$out/RDNA4 Gadget.app"
tmp=$(mktemp -d)
cp "$here/check.swift" "$tmp/main.swift"   # (top-level code: compiled as a main.swift)
xcrun swiftc -target x86_64-apple-macos13.0 -o "$tmp/check" "$tmp/main.swift" "$here"/Driver.swift
"$tmp/check"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
xcrun swiftc -O -target x86_64-apple-macos13.0 -sdk "$(xcrun --show-sdk-path)" -module-name RDNA4Gadget \
	-o "$app/Contents/MacOS/RDNA4Gadget" "$here"/main.swift "$here"/Model.swift "$here"/Views.swift "$here"/Sensors.swift \
	"$here"/Driver.swift
sed "s/@VERSION@/$version/g" "$here/Info.plist" > "$app/Contents/Info.plist"
codesign --force --sign - "$app"
echo "built $app ($version)"
