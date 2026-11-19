#!/bin/sh
# The native-Metal spike's checks (gates 2 and 3) on the rig (docs/native-metal-spike.md): run with sudo from the folder holding mtlprobe-native and
# probe.metallib (~/rdna4/native). Publishes the native service if this boot has not yet, runs the check, keeps its output in probe.log.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo: sudo sh $0" >&2; exit 1; }
cd "$(dirname "$0")"
[ "$(sysctl -n debug.rdna4_nativemtl)" = 1 ] || sysctl debug.rdna4_nativemtl=1
env RDNA4_NATIVE_MTL=1 ./mtlprobe-native native ./probe.metallib 2>&1 | tee probe.log
