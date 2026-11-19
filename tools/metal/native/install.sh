#!/bin/sh
# Install the native-Metal spike's bundle where Metal's loader trusts it (run as root, on the rig):
#   sudo sh install.sh AMDRadeonRDNA4Metal.bundle      (from a copy of Build/native/)
# Metal loads /Library/GPUBundles/<name>.bundle if gpu_bundle_is_path_trusted (libsystem_sandbox) accepts it: the rootless storage class
# KernelExtensionManagement, which only kext management can set, or SIP's filesystem protection off (csr-active-config bit 1, as on
# the rig), which makes every path trusted. Undo: --remove.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
dest=/Library/GPUBundles/AMDRadeonRDNA4Metal.bundle
if [ "${1:-}" = --remove ]; then
	rm -rf "$dest"
	echo "removed $dest"
	exit 0
fi
src=${1:?usage: install.sh <AMDRadeonRDNA4Metal.bundle> | --remove}
[ -f "$src/Contents/MacOS/AMDRadeonRDNA4Metal" ] || { echo "not the bundle: $src" >&2; exit 1; }
mkdir -p /Library/GPUBundles
rm -rf "$dest"
cp -R "$src" "$dest"
chown -R root:wheel "$dest"
echo "installed $dest"
