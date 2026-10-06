#!/bin/bash
#
# Build Mesa's RADV with the Darwin patches of this directory, into a directory of its own.
#
#   vulkan/build-mesa.sh <work directory>
#
# Fetches Mesa at the commit the patches are made for (about 150 MB), applies the patches, makes a private
# Python environment with meson, and builds the AMD Vulkan driver alone: x86_64 (the card's machines; an
# Apple-silicon Mac cross-builds it and runs it under Rosetta), for macOS 11 and later, without Mesa's window-system
# layer, which would bring in Apple's Metal and two macOS 12 symbols. The result links IOKit, libSystem, libz and
# libc++ only: <work>/build/src/amd/vulkan/libvulkan_radeon.dylib. vkprobe.c says how to try it.
#
# Needs clang, ninja, bison, flex, pkg-config and glslangValidator. Mesa's own tests stay off: with them two AMD
# tests fail to link (the patches make libamd_common need IOKit and those tests do not ask for it).
#
set -e
here=$(cd "$(dirname "$0")" && pwd)
W=${1:?usage: build-mesa.sh <work directory>}
BASE=f5cb8ee032adabef599ae892ec4e42d796b84da9
mkdir -p "$W" && cd "$W" && W=$PWD
if [ ! -d mesa/.git ]; then
	git init -q mesa
	git -C mesa remote add origin https://gitlab.freedesktop.org/mesa/mesa.git
	git -C mesa fetch -q --depth 1 origin $BASE
	git -C mesa checkout -q FETCH_HEAD
	git -C mesa -c user.name=local -c user.email=local@invalid am --quiet "$here"/mesa-patches/*.patch
fi
[ -x venv/bin/meson ] || { python3 -m venv venv; venv/bin/pip -q install meson mako pyyaml packaging; }
export PATH=$W/venv/bin:$PATH MACOSX_DEPLOYMENT_TARGET=11.0
# Homebrew's libraries on an Apple-silicon Mac are arm64 only: keep pkg-config away from them.
[ -d build ] || PKG_CONFIG_LIBDIR=/nonexistent meson setup build mesa --cross-file mesa/cross-x86_64.ini \
	-Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dopengl=false \
	-Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dvideo-codecs= -Dzstd=disabled -Dexpat=disabled \
	-Dxmlconfig=disabled -Dbuild-tests=false -Dbuildtype=debugoptimized
ninja -C build
