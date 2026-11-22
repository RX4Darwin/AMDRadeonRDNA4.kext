#!/bin/bash
#
# Build a Vulkan application somebody else wrote, with everything it needs to run on the driver of build-mesa.sh:
#
#   userspace/vulkan/build-app.sh <work directory> <libvulkan_radeon.dylib> <output directory>
#
# Fetches Khronos's Vulkan-Headers, Vulkan-Loader and Vulkan-Tools at one SDK tag (about 65 MB), builds the loader
# and vkcube for x86_64 and macOS 11 and later, and puts into the output directory (`make vkcube`: the product
# directory's bin/): vkcube, the loader (libvulkan.dylib), the driver, and in vulkan/icd.d/ the manifest that tells
# the loader where the driver is. The loader looks in that directory beside the program it is loaded into, so
# vkcube needs no environment variable, and it opens the loader beside itself, so it can be started from anywhere.
# vkcube's own macOS version draws through Metal; here it is built from cube.c alone with vkcube-headless.patch,
# which gives its "display" mode a headless surface and makes it open that loader first (a Vulkan SDK's loader in
# /usr/local/lib would otherwise be taken, with the SDK's drivers). The driver shows a headless swapchain on the
# display the kext lends (mesa-patches/0008).
#
# Also vkcube.app, its own macOS version (a window, VK_EXT_metal_surface: mesa-patches/0009), with the loader inside it
# and the driver and its manifest in Contents/Resources/vulkan/icd.d, where the loader looks in an application. A
# MoltenVK installed on the machine is a driver too: VK_DRIVER_FILES=<vkcube.app>/Contents/Resources/vulkan/icd.d/
# radeon_icd.json keeps the loader to ours (open -a ... --env VK_DRIVER_FILES=...).
#
# Needs clang, cmake and ninja.
#
set -e
here=$(cd "$(dirname "$0")" && pwd)
W=${1:?usage: build-app.sh <work directory> <libvulkan_radeon.dylib> <output directory>}
driver=${2:?the driver built by build-mesa.sh}
out=${3:?the output directory}
TAG=vulkan-sdk-1.4.363.0
mkdir -p "$W" "$out" && out=$(cd "$out" && pwd) && driver=$(cd "$(dirname "$driver")" && pwd)/$(basename "$driver")
cd "$W" && W=$PWD
for r in Vulkan-Headers Vulkan-Loader Vulkan-Tools; do
	[ -d $r/.git ] || git -c advice.detachedHead=false clone -q --depth 1 --branch $TAG https://github.com/KhronosGroup/$r.git
done
# The clone is only ever this patch away from the tag: start from the tag each time, so that a changed patch arrives.
git -C Vulkan-Tools checkout -q -- . && git -C Vulkan-Tools apply "$here"/vkcube-headless.patch
cm="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"
[ -d prefix ] || { cmake -S Vulkan-Headers -B headers-build $cm -DCMAKE_INSTALL_PREFIX="$W/prefix" > /dev/null
	cmake --install headers-build > /dev/null; }
[ -d loader-build ] || cmake -S Vulkan-Loader -B loader-build $cm -DCMAKE_PREFIX_PATH="$W/prefix" -DBUILD_TESTS=OFF > /dev/null
cmake --build loader-build > /dev/null
clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu11 -O2 -DVK_USE_PLATFORM_DISPLAY_KHR -DCUBE_HEADLESS \
	-I prefix/include -I Vulkan-Tools/cube Vulkan-Tools/cube/cube.c -o "$out/vkcube"
cp loader-build/loader/libvulkan.1.dylib "$out/libvulkan.dylib"
cp "$driver" "$out/libvulkan_radeon.dylib"
mkdir -p "$out/vulkan/icd.d"
cat > "$out/vulkan/icd.d/radeon_icd.json" <<JSON
{
	"file_format_version": "1.0.1",
	"ICD": { "library_path": "../../libvulkan_radeon.dylib", "api_version": "1.4.363" }
}
JSON
rm -f "$out/libvulkan.1.dylib" "$out/radeon_icd.json"   # where an earlier version of this script put them
# vkcube.app: the system's ICD discovery, not a MoltenVK of its own (APPLE_USE_SYSTEM_ICD)
[ -d tools-build ] || cmake -S Vulkan-Tools -B tools-build $cm -DCMAKE_PREFIX_PATH="$W/prefix" -DAPPLE_USE_SYSTEM_ICD=ON \
	-DBUILD_CUBE=ON -DBUILD_VULKANINFO=OFF -DBUILD_ICD=OFF -DTOOLS_CODEGEN=OFF > /dev/null
cmake --build tools-build --target vkcube > /dev/null
app="$out/vkcube.app"
rm -rf "$app" && cp -R tools-build/cube/macOS/cube/vkcube.app "$app"
cp loader-build/loader/libvulkan.1.dylib "$app/Contents/MacOS/libvulkan.dylib"
mkdir -p "$app/Contents/Frameworks" "$app/Contents/Resources/vulkan/icd.d"
cp "$driver" "$app/Contents/Frameworks/libvulkan_radeon.dylib"
cat > "$app/Contents/Resources/vulkan/icd.d/radeon_icd.json" <<JSON
{
	"file_format_version": "1.0.1",
	"ICD": { "library_path": "../../../Frameworks/libvulkan_radeon.dylib", "api_version": "1.4.363" }
}
JSON
codesign -s - -f --deep "$app" > /dev/null 2>&1
