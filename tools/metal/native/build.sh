#!/bin/sh
# Build the native-Metal spike's bundle (x86_64, macOS 11+): Build/native/AMDRadeonRDNA4Metal.bundle, ad-hoc signed.
set -eu
cd "$(dirname "$0")/../../.."
out=Build/native/AMDRadeonRDNA4Metal.bundle
rm -rf "$out"
mkdir -p "$out/Contents/MacOS"
cp tools/metal/native/Info.plist "$out/Contents/Info.plist"
clang -arch x86_64 -mmacosx-version-min=11.0 -fobjc-arc -Wall -Wno-protocol -Wno-objc-protocol-property-synthesis -bundle tools/metal/native/RDNA4MTLDevice.m \
	-framework Foundation -framework Metal -framework IOKit -o "$out/Contents/MacOS/AMDRadeonRDNA4Metal"
codesign -s - -f "$out" >/dev/null
echo "built $out"
