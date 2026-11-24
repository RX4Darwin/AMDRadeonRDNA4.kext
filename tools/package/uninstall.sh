#!/bin/sh
# Remove what AMDRadeonRDNA4's package installed (run as root):
#   sudo sh /Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle/Contents/Resources/uninstall.sh
# Stops and removes the LaunchDaemons, removes the driver bundle, RADV's manifest, the rdna4-run link and RDNA4Gadget.app, and forgets
# the package receipts. The kext stays on the EFI: remove it and its Kernel -> Add entry there, or macOS will look for the driver at the next boot.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
for l in org.rdna4.air2amdgpu org.rdna4.iop org.rdna4.logd; do
	launchctl bootout "system/$l" 2> /dev/null || true
	rm -f "/Library/LaunchDaemons/$l.plist"
done
rm -f /usr/local/share/vulkan/icd.d/radeon_icd.json /usr/local/bin/rdna4-run
rm -rf /Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle /Applications/RDNA4Gadget.app
for c in core logd vulkan gadget; do
	pkgutil --forget "com.rx4darwin.AMDRadeonRDNA4.$c" > /dev/null 2>&1 || true
done
echo "AMDRadeonRDNA4's user space removed. The kext is still on the EFI (EFI/OC/Kexts/AMDRadeonRDNA4.kext and its Kernel -> Add"
echo "entry in config.plist): remove both before the next boot, or put the kext of a matching release back."
