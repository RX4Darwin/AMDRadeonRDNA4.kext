#!/bin/sh
# Build tools/metal/air/air2amdgcn.cpp against the system LLVM (22.x, shared libLLVM) into Build/air/air2amdgcn.
cd "$(dirname "$0")/../../.." && mkdir -p Build/air
exec g++ -std=c++17 -O1 -g0 $(llvm-config --cxxflags | sed 's/-std=c++[0-9a-z]*//; s/-fno-exceptions//; s/-fno-rtti//') -fexceptions \
	tools/metal/air/air2amdgcn.cpp -o Build/air/air2amdgcn $(llvm-config --ldflags --libs --link-shared)
