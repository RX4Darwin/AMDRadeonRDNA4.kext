#!/bin/bash
#
# Run the real RADV driver on this repository's client engine (src/n48n.cpp), on a host, without a kext:
#
#   tools/n48n-host/run.sh <work directory of vulkan/build-mesa.sh>
#
# Builds the interposer and vulkan/vkprobe.c for x86_64 and runs the probe with the interposer inserted. The
# driver's device creation, buffers and mappings then go through the engine. N48N_HOST_TRACE=1 lists every call.
#
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
W=${1:?usage: run.sh <work directory of vulkan/build-mesa.sh>}
lib=$W/build/src/amd/vulkan/libvulkan_radeon.dylib
[ -f "$lib" ] || { echo "$lib: build it with vulkan/build-mesa.sh first" >&2; exit 2; }
cxx="clang++ -arch x86_64 -mmacosx-version-min=11.0 -std=c++17 -Wall -O1 -g"
$cxx -dynamiclib -o "$W/n48n-host.dylib" "$here/interpose.cpp" "$repo/src/n48n.cpp" "$repo/src/vmtree.cpp" \
	"$repo/src/gpuvm.cpp" -framework IOKit -framework CoreFoundation
clang -arch x86_64 -mmacosx-version-min=11.0 -std=gnu11 -I "$W/mesa/include" "$repo/vulkan/vkprobe.c" -o "$W/vkprobe"
RADV_DARWIN_FAKE=1 DYLD_INSERT_LIBRARIES="$W/n48n-host.dylib" "$W/vkprobe" "$lib"
