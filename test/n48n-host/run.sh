#!/bin/bash
#
# Run the real RADV driver on this repository's client engine (src/n48n.cpp), on a host, without a kext:
#
#   test/n48n-host/run.sh <work directory of userspace/vulkan/build-mesa.sh>
#
# Builds the interposer and userspace/vulkan/vkprobe.c for x86_64 and runs the probe with the interposer inserted. The
# driver's device creation, buffers and mappings then go through the engine. N48N_HOST_TRACE=1 lists every call.
# Then the same for userspace/vulkan/n48nprobe.c, the program of the card test.
#
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
W=${1:?usage: run.sh <work directory of userspace/vulkan/build-mesa.sh>}
lib=$W/build/src/amd/vulkan/libvulkan_radeon.dylib
[ -f "$lib" ] || { echo "$lib: build it with userspace/vulkan/build-mesa.sh first" >&2; exit 2; }
cxx="clang++ -arch x86_64 -mmacosx-version-min=11.0 -std=c++17 -Wall -O1 -g"
$cxx -dynamiclib -o "$W/n48n-host.dylib" "$here/interpose.cpp" "$repo/src/n48n.cpp" "$repo/src/vmtree.cpp" \
	"$repo/src/gpuvm.cpp" -framework IOKit -framework CoreFoundation
clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu11 -I "$W/mesa/include" "$repo/userspace/vulkan/vkprobe.c" -o "$W/vkprobe"
# The card test's own program (no Mesa) first, with the CPU standing in for the GPU's copy engine: it has to pass.
clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu11 -Wall -I "$repo/include" -I "$repo/userspace/vulkan" \
	"$repo/userspace/vulkan/n48nprobe.c" -framework IOKit -framework CoreFoundation -o "$W/n48nprobe"
DYLD_INSERT_LIBRARIES="$W/n48n-host.dylib" "$W/n48nprobe"
# Then the driver: its submissions are checked and accepted, and reported finished at once. Nothing executes.
# "show 1": a second of frames on the stand-in display, as fast as they are accepted.
VKPROBE_NOGPU=1 DYLD_INSERT_LIBRARIES="$W/n48n-host.dylib" "$W/vkprobe" "$lib" show 1
# And a program with a swapchain (mesa-patches/0008): the driver takes the display, registers the images' buffers
# and presents by itself.
clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu11 -Wall -I "$W/mesa/include" -I "$repo/userspace/vulkan" \
	"$repo/userspace/vulkan/swapprobe.c" -o "$W/swapprobe"
DYLD_INSERT_LIBRARIES="$W/n48n-host.dylib" "$W/swapprobe" "$lib" 1
# The same with rows longer than the picture, as on the rig.
N48N_HOST_PITCH=3840 DYLD_INSERT_LIBRARIES="$W/n48n-host.dylib" "$W/swapprobe" "$lib" 1
