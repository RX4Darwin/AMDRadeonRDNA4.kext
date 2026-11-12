#!/usr/bin/env bash
# Build AMDRadeonRDNA4.kext on Linux / WSL with an osxcross toolchain
# (https://github.com/tpoechtrager/osxcross: Apple ld64 + a macOS SDK).
#
#   tools/build/build-osxcross.sh            Build/AMDRadeonRDNA4-<version>-RELEASE/
#   tools/build/build-osxcross.sh VMTEST=1   Build/vmtest/AMDRadeonRDNA4-<version>-RELEASE/
#
# OSXCROSS defaults to ~/osxcross; the target triple and SDK are taken from
# its target/ directory. Extra arguments are passed through to make.
set -euo pipefail

OSXCROSS=${OSXCROSS:-$HOME/osxcross}
BIN=$OSXCROSS/target/bin
SDK=$(ls -d "$OSXCROSS"/target/SDK/MacOSX*.sdk | sort -V | tail -1)
TRIPLE=$(ls "$BIN" | grep -E '^x86_64-apple-darwin[0-9.]+-clang$' | sort -V | tail -1)
TRIPLE=${TRIPLE%-clang}

cd "$(dirname "$0")/../.."
export PATH=$BIN:$PATH

# The Makefile's -target overrides the osxcross wrapper's, which makes clang
# look for a host "ld"; point it at Apple's ld64 explicitly.
make SDK="$SDK" \
     CXX="$TRIPLE-clang++ --ld-path=$BIN/$TRIPLE-ld" \
     CC="$TRIPLE-clang" \
     "$@"
