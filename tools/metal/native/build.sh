#!/bin/sh
# Build the native-Metal spike's bundle (x86_64, macOS 13+: MTLBuffer.gpuAddress): Build/native/AMDRadeonRDNA4Metal.bundle, ad-hoc signed, and the
# test functions Build/native/probe.metallib (Xcode's Metal toolchain).
set -eu
cd "$(dirname "$0")/../../.."
out=Build/native/AMDRadeonRDNA4Metal.bundle
rm -rf "$out"
mkdir -p "$out/Contents/MacOS"
cp tools/metal/native/Info.plist "$out/Contents/Info.plist"
clang -arch x86_64 -mmacosx-version-min=13.0 -fobjc-arc -Wall -Wno-protocol -Wno-objc-protocol-property-synthesis -bundle \
	-Iinclude -Iuserspace/rdna4-run tools/metal/native/RDNA4MTLDevice.m userspace/rdna4-run/librdna4.c \
	-framework Foundation -framework Metal -framework IOKit -framework IOSurface -framework QuartzCore -o "$out/Contents/MacOS/AMDRadeonRDNA4Metal"
# its own translator when one is built (tools/metal/m3/build.sh helper), so the system's helper, which WindowServer's paravirtual
# drawing uses, need not change for the native device
if [ -x Build/m3/x86_64/air2amdgpu ]; then
	mkdir -p "$out/Contents/Resources"
	cp Build/m3/x86_64/air2amdgpu "$out/Contents/Resources/air2amdgpu"
fi
codesign -s - -f "$out" >/dev/null
xcrun -sdk macosx metal -c tools/metal/native/probe.metal -o Build/native/probe.air
xcrun -sdk macosx metallib Build/native/probe.air -o Build/native/probe.metallib
echo "built $out and Build/native/probe.metallib"
