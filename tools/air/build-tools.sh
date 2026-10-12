#!/bin/sh
# Build tools/air/air2amdgcn.cpp against the system LLVM (22.x, shared libLLVM) into build/air/air2amdgcn.
cd "$(dirname "$0")/../.." && mkdir -p build/air
exec g++ -std=c++17 -O1 -g0 $(llvm-config --cxxflags | sed 's/-std=c++[0-9a-z]*//; s/-fno-exceptions//; s/-fno-rtti//') -fexceptions \
	tools/air/air2amdgcn.cpp -o build/air/air2amdgcn $(llvm-config --ldflags --libs --link-shared)
