#!/bin/bash
#
# Build a Vulkan application somebody else wrote, with everything it needs to run on the driver of build-mesa.sh:
#
#   vulkan/build-app.sh <work directory> <libvulkan_radeon.dylib> <output directory>
#
# Fetches Khronos's Vulkan-Headers, Vulkan-Loader and Vulkan-Tools at one SDK tag (about 65 MB), builds the loader
# and vkcube for x86_64 and macOS 11 and later, and fills the output directory: vkcube, the loader
# (libvulkan.1.dylib, which vkcube opens by name from the directory it is started in), the driver and the manifest
# that tells the loader where the driver is. vkcube's own macOS version draws through Metal; here it is built from
# cube.c alone with vkcube-headless.patch, which gives its "display" mode a headless surface. The driver shows a
# headless swapchain on the display the kext lends (mesa-patches/0008).
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
git -C Vulkan-Tools diff --quiet && git -C Vulkan-Tools apply "$here"/vkcube-headless.patch
cm="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"
[ -d prefix ] || { cmake -S Vulkan-Headers -B headers-build $cm -DCMAKE_INSTALL_PREFIX="$W/prefix" > /dev/null
	cmake --install headers-build > /dev/null; }
[ -d loader-build ] || cmake -S Vulkan-Loader -B loader-build $cm -DCMAKE_PREFIX_PATH="$W/prefix" -DBUILD_TESTS=OFF > /dev/null
cmake --build loader-build > /dev/null
clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu11 -O2 -DVK_USE_PLATFORM_DISPLAY_KHR -DCUBE_HEADLESS \
	-I prefix/include -I Vulkan-Tools/cube Vulkan-Tools/cube/cube.c -o "$out/vkcube"
cp loader-build/loader/libvulkan.1.dylib "$out/libvulkan.1.dylib"
cp "$driver" "$out/libvulkan_radeon.dylib"
cat > "$out/radeon_icd.json" <<JSON
{
	"file_format_version": "1.0.1",
	"ICD": { "library_path": "./libvulkan_radeon.dylib", "api_version": "1.4.363" }
}
JSON
