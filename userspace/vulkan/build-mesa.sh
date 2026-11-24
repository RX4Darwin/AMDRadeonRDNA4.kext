#!/bin/bash
#
# Build Mesa's RADV with the Darwin patches of this directory, into a directory of its own.
#
#   userspace/vulkan/build-mesa.sh <work directory>
#
# Copies Mesa from the lib/Mesa submodule (the commit the patches are made for; `git submodule update --init lib/Mesa` fetches it,
# about 150 MB, if it is not there yet) into the work directory, applies the patches to that copy, makes a private
# Python environment with meson, and builds the AMD Vulkan driver alone: x86_64 (the card's machines; an
# Apple-silicon Mac cross-builds it and runs it under Rosetta), for macOS 11 and later, with Mesa's Metal window-system
# layer (VK_EXT_metal_surface: windows through CAMetalLayer, mesa-patches/0009). The result links IOKit, Metal,
# QuartzCore, libSystem, libz and libc++: <work>/build/src/amd/vulkan/libvulkan_radeon.dylib. vkprobe.c says how to try
# it, winprobe.m how to try a window.
#
# Needs clang, ninja, bison, flex, pkg-config and glslangValidator. Mesa's own tests stay off: with them two AMD
# tests fail to link (the patches make libamd_common need IOKit and those tests do not ask for it).
#
set -e
here=$(cd "$(dirname "$0")" && pwd)
W=${1:?usage: build-mesa.sh <work directory>}
root=$(cd "$here/../.." && pwd)
src=$root/lib/Mesa
[ -e "$src/.git" ] || git -C "$root" submodule update --init --depth 1 lib/Mesa
BASE=$(git -C "$src" rev-parse HEAD)   # the submodule's commit: the one the patches are made for
mkdir -p "$W" && cd "$W" && W=$PWD
if [ ! -d mesa/.git ]; then   # a local copy (no download): the patches go into it, the submodule stays as it is
	git clone -q --no-checkout "$src" mesa
	git -C mesa checkout -q $BASE
fi
# The patches the tree does not have yet: it has as many commits after the base as patches applied.
have=$(git -C mesa rev-list --count $BASE..HEAD)
for p in "$here"/mesa-patches/*.patch; do
	n=$(basename "$p" | cut -c1-4)
	[ "$((10#$n))" -le "$have" ] || git -C mesa -c user.name=local -c user.email=local@invalid am --quiet "$p"
done
[ -x venv/bin/meson ] || { python3 -m venv venv; venv/bin/pip -q install meson mako pyyaml packaging; }
export PATH=$W/venv/bin:$PATH MACOSX_DEPLOYMENT_TARGET=11.0
# Homebrew's libraries on an Apple-silicon Mac are arm64 only: keep pkg-config away from them.
[ -d build ] || PKG_CONFIG_LIBDIR=/nonexistent meson setup build mesa --cross-file mesa/cross-x86_64.ini \
	-Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms=macos -Dglx=disabled -Degl=disabled -Dopengl=false \
	-Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dvideo-codecs= -Dzstd=disabled -Dexpat=disabled \
	-Dxmlconfig=disabled -Dbuild-tests=false -Dbuildtype=debugoptimized
ninja -C build
