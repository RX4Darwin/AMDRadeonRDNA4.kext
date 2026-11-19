#!/bin/sh
# Build the native-Metal spike's bundle (x86_64, macOS 13+: MTLBuffer.gpuAddress): Build/native/AMDRadeonRDNA4Metal.bundle, ad-hoc signed, and gate 2's
# test kernels Build/native/gate2.metallib (Xcode's Metal toolchain).
set -eu
cd "$(dirname "$0")/../../.."
out=Build/native/AMDRadeonRDNA4Metal.bundle
rm -rf "$out"
mkdir -p "$out/Contents/MacOS"
cp tools/metal/native/Info.plist "$out/Contents/Info.plist"
clang -arch x86_64 -mmacosx-version-min=13.0 -fobjc-arc -Wall -Wno-protocol -Wno-objc-protocol-property-synthesis -bundle \
	-Iinclude -Iuserspace/rdna4-run tools/metal/native/RDNA4MTLDevice.m userspace/rdna4-run/librdna4.c \
	-framework Foundation -framework Metal -framework IOKit -o "$out/Contents/MacOS/AMDRadeonRDNA4Metal"
codesign -s - -f "$out" >/dev/null
xcrun -sdk macosx metal -c tools/metal/native/gate2.metal -o Build/native/gate2.air
xcrun -sdk macosx metallib Build/native/gate2.air -o Build/native/gate2.metallib
echo "built $out and Build/native/gate2.metallib"
