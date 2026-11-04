#!/bin/sh
# Build tools/metal/air/air2amdgcn.cpp against the LLVM whose llvm-config is first on PATH (or in $LLVM/bin) into Build/air/air2amdgcn:
# a distribution's shared libLLVM, or a static build such as the compiler service's on the test Mac (~/opt/llvm).
cd "$(dirname "$0")/../../.." && mkdir -p Build/air
[ -n "${LLVM:-}" ] && PATH=$LLVM/bin:$PATH
mode=--link-shared
[ "$(llvm-config --shared-mode)" = static ] && mode="--link-static --system-libs"
# RTTI as LLVM was built (LLVM's own default is without; distributions build it with)
rtti='s/-fno-rtti//'
[ "$(llvm-config --has-rtti)" = NO ] && rtti=
cxx=g++
command -v "$cxx" > /dev/null || cxx=c++
# shellcheck disable=SC2046,SC2086
exec $cxx -std=c++17 -O1 -g0 $(llvm-config --cxxflags | sed "s/-std=c++[0-9a-z]*//; s/-fno-exceptions//; $rtti") -fexceptions \
	tools/metal/air/air2amdgcn.cpp -o Build/air/air2amdgcn $(llvm-config --ldflags) $(llvm-config $mode --libs)
