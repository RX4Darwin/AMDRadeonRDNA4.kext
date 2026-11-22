#!/bin/sh
# Install the Metal driver bundle (`make metal-driver`) where Metal's loader trusts it (run as root, on the rig; docs/packaging-plan.md):
#   sudo sh install.sh <AMDRadeonRDNA4MTLDriver.bundle>
# Metal loads /Library/GPUBundles/<name>.bundle if gpu_bundle_is_path_trusted (libsystem_sandbox) accepts it: the rootless storage class
# KernelExtensionManagement, which only kext management can set, or SIP's filesystem protection off (csr-active-config bit 1, as on the
# rig), which makes every path trusted. Removes what earlier builds installed: the driver's old name (AMDRadeonRDNA4Metal.bundle) and
# /Library/Frameworks/AMDRadeonRDNA4.framework. Undo: --remove.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
dest=/Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle
old="/Library/GPUBundles/AMDRadeonRDNA4Metal.bundle /Library/Frameworks/AMDRadeonRDNA4.framework"
if [ "${1:-}" = --remove ]; then
	rm -rf "$dest" $old
	echo "removed $dest"
	exit 0
fi
src=${1:?usage: install.sh <AMDRadeonRDNA4MTLDriver.bundle> | --remove}
[ -f "$src/Contents/MacOS/AMDRadeonRDNA4MTLDriver" ] || { echo "not the driver bundle: $src" >&2; exit 1; }
mkdir -p /Library/GPUBundles
rm -rf "$dest" $old
cp -R "$src" "$dest"
chown -R root:wheel "$dest"
echo "installed $dest"
