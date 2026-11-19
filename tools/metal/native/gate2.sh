#!/bin/sh
# The native-Metal spike's gate 2 on the rig (docs/native-metal-spike.md): run with sudo from the folder holding mtlprobe-native and
# gate2.metallib (~/rdna4/native). Publishes the native service if this boot has not yet, runs the check, keeps its output in gate2.log.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo: sudo sh $0" >&2; exit 1; }
cd "$(dirname "$0")"
[ "$(sysctl -n debug.rdna4_nativemtl)" = 1 ] || sysctl debug.rdna4_nativemtl=1
env RDNA4_NATIVE_MTL=1 ./mtlprobe-native native ./gate2.metallib 2>&1 | tee gate2.log
