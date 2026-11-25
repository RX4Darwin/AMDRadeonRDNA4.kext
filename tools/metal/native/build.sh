#!/bin/sh
# Build the Metal driver bundle (x86_64, macOS 13+: MTLBuffer.gpuAddress), everything of the driver's user space in one place
# (docs/packaging-plan.md), into the product dir:
#   AMDRadeonRDNA4MTLDriver.bundle/Contents/
#       MacOS/AMDRadeonRDNA4MTLDriver     the Metal driver (Metal's loader finds it by the kext's MetalPluginName)
#       Frameworks/AMDRadeonRDNA4.framework  librdna4, the kext's runtime client, linked by the driver (@rpath), with the user-space
#                                         headers and its Info.plist (version, the kext ABI it was built for)
#       Frameworks/libvulkan_radeon.dylib RADV, when `make mesa` built it; Resources/radeon_icd.json names it where it is installed
#       Helpers/                          air2amdgpu (when `make metal-helper` built it), rdna4-run, rdna4-iop, rdna4-logd
#       Resources/rdna4.metallib          the driver's own functions (rdna4.metal)
# Everything stays inside /Library/GPUBundles: WindowServer's sandbox maps libraries from there but not from /Library/Frameworks
# ("file system sandbox blocked mmap()", 2026-11-22: no desktop with the driver linked against a framework there).
# Also the test functions Build/native/probe.metallib (Xcode's Metal toolchain). Signed ad hoc.
set -eu
cd "$(dirname "$0")/../../.."
make -s userspace
dir=$(make -s product-dir)
version=$(sed -n 's/^VERSION *= *//p' Makefile)
name=AMDRadeonRDNA4MTLDriver
installed=/Library/GPUBundles/$name.bundle
out=$dir/$name.bundle
c=$out/Contents
rm -rf "$out"
mkdir -p "$c/MacOS" "$c/Frameworks" "$c/Helpers" "$c/Resources" Build/native
sed "s/@VERSION@/$version/" tools/metal/native/Info.plist > "$c/Info.plist"

# AMDRadeonRDNA4.framework, embedded: Versions/A/{AMDRadeonRDNA4, Headers, Resources/Info.plist}, the usual symlinks
fw=$c/Frameworks/AMDRadeonRDNA4.framework
a=$fw/Versions/A
abi=$(sed -n 's/^#define RDNA4_COMPUTE_ABI[^0-9]*\([0-9]*\)u.*/\1/p' include/rdna4compute.h)
mkdir -p "$a/Headers" "$a/Resources"
clang -x c -arch x86_64 -mmacosx-version-min=13.0 -std=c11 -O2 -Wall -Iinclude -dynamiclib \
	-install_name @rpath/AMDRadeonRDNA4.framework/Versions/A/AMDRadeonRDNA4 -current_version "$version" -compatibility_version 1 \
	userspace/rdna4-run/librdna4.c -framework IOKit -framework CoreFoundation -o "$a/AMDRadeonRDNA4"
cp userspace/rdna4-run/librdna4.h include/rdna4compute.h include/rdna4vulkan.h "$a/Headers/"
sed -i '' 's/#include "rdna4compute.h"/#include <AMDRadeonRDNA4\/rdna4compute.h>/' "$a/Headers/librdna4.h"
echo '#include <AMDRadeonRDNA4/librdna4.h>   /* the kext runtime client (librdna4.h) and its user-space ABI (rdna4compute.h) */' > "$a/Headers/AMDRadeonRDNA4.h"
sed -e "s/@VERSION@/$version/" -e "s/@ABI@/$abi/" tools/metal/native/Framework-Info.plist > "$a/Resources/Info.plist"
ln -s A "$fw/Versions/Current"
for l in AMDRadeonRDNA4 Headers Resources; do
	ln -s "Versions/Current/$l" "$fw/$l"
done
clang++ -std=c++17 -arch x86_64 -mmacosx-version-min=13.0 -fobjc-arc -Wall -Wno-protocol -Wno-objc-protocol-property-synthesis -Wno-objc-protocol-method-implementation -bundle \
	-Iinclude -Iuserspace/rdna4-run -Isrc tools/metal/native/RDNA4MTLDevice.mm -F"$c/Frameworks" -framework AMDRadeonRDNA4 -Wl,-rpath,@loader_path/../Frameworks \
	-framework Foundation -framework Metal -framework IOKit -framework IOSurface -framework QuartzCore -o "$c/MacOS/$name"

for p in "$dir/bin/rdna4-run" "$dir/bin/rdna4-iop" "$dir/bin/rdna4-logd" Build/m3/x86_64/air2amdgpu; do
	if [ -x "$p" ]; then cp "$p" "$c/Helpers/"; else echo "build.sh: $p not built, left out of Helpers/" >&2; fi
done
cp tools/package/fan-apply.sh "$c/Helpers/rdna4-fan-apply"   # (RDNA4Gadget's fan settings, as root: the LaunchDaemon org.rdna4.fan)
if [ -f "$dir/bin/libvulkan_radeon.dylib" ]; then
	cp "$dir/bin/libvulkan_radeon.dylib" "$c/Frameworks/"
	cat > "$c/Resources/radeon_icd.json" <<EOF
{
	"file_format_version": "1.0.1",
	"ICD": { "library_path": "$installed/Contents/Frameworks/libvulkan_radeon.dylib", "api_version": "1.4.363" }
}
EOF
else
	echo "build.sh: $dir/bin/libvulkan_radeon.dylib not built (make mesa), left out" >&2
fi
# Mesa's Anti-Lag layer (Anti-Lag 2 for Vulkan games): its manifest is an implicit layer's, loaded while ENABLE_LAYER_MESA_ANTI_LAG=1
# (RDNA4Gadget sets it for the session); the package puts it where Khronos's loader looks
if [ -f "$dir/bin/libVkLayer_MESA_anti_lag.dylib" ]; then
	cp "$dir/bin/libVkLayer_MESA_anti_lag.dylib" "$c/Frameworks/"
	sed "s|@library_path@|$installed/Contents/Frameworks/libVkLayer_MESA_anti_lag.dylib|" \
		lib/Mesa/src/vulkan/anti-lag-layer/VkLayer_MESA_anti_lag.json.in > "$c/Resources/VkLayer_MESA_anti_lag.json"
fi

xcrun -sdk macosx metal -c tools/metal/native/rdna4.metal -o Build/native/rdna4.air
xcrun -sdk macosx metallib Build/native/rdna4.air -o "$c/Resources/rdna4.metallib"
sh tools/package/licenses.sh bundle "$out"
for f in "$c/Frameworks/"* "$c/Helpers/"*; do
	[ -e "$f" ] && codesign -s - -f "$f" > /dev/null 2>&1
done
codesign -s - -f "$out" > /dev/null
xcrun -sdk macosx metal -c tools/metal/native/probe.metal -o Build/native/probe.air
xcrun -sdk macosx metallib Build/native/probe.air -o Build/native/probe.metallib
echo "built $out ($version) and Build/native/probe.metallib"
