#!/bin/sh
# The licences of what a shipped binary contains, copied in beside it. Run from the repository's root:
#   sh tools/package/licenses.sh kext <the kext's Contents/Resources>
#   sh tools/package/licenses.sh bundle <AMDRadeonRDNA4MTLDriver.bundle>
# docs/licenses.md says what each one covers. A component that is in the binary without its licence stops the build.
set -eu

die() { echo "licenses.sh: $*" >&2; exit 1; }
put() { [ -f "$1" ] || die "no licence at $1"; cp "$1" "$2"; }

case ${1:-} in
kext)
	d=${2:?the kext\'s Resources folder}
	mkdir -p "$d"
	put LICENSE "$d/LICENSE"
	put lib/Lilu/LICENSE.txt "$d/LICENSE.Lilu"                 # plugin_start.cpp is compiled in
	put lib/MacKernelSDK/LICENSE.txt "$d/LICENSE.MacKernelSDK"  # libkmod and libkmodc++ are linked in (APSL 2.0)
	sed -n '/Copyright/,/OTHER DEALINGS/p' src/amdgpu/clearstate_defs.h | sed -e 's/^ \* \{0,1\}//' > "$d/LICENSE.amdgpu-code"
	[ -s "$d/LICENSE.amdgpu-code" ] || die "no MIT notice in src/amdgpu/clearstate_defs.h"
	;;
bundle)
	b=${2:?the driver bundle}
	d=$b/Contents/Resources/Licenses
	mkdir -p "$d"
	put LICENSE "$d/LICENSE"
	if [ -e "$b/Contents/Frameworks/libvulkan_radeon.dylib" ]; then
		m=Build/radv-build/mesa
		put "$m/docs/license.rst" "$d/Mesa-license.rst"
		put "$m/licenses/MIT" "$d/Mesa-MIT"
		put "$m/licenses/BSL-1.0" "$d/Mesa-BSL-1.0"
		put userspace/vulkan/LICENSE-Navi48-MacOS "$d/Navi48-MacOS"
		put userspace/vulkan/LICENSE-Navi48-bringup "$d/Navi48-bringup"
	fi
	if [ -e "$b/Contents/Helpers/air2amdgpu" ]; then
		l=Build/llvm/src/llvm/LICENSE.TXT
		[ -f "$l" ] || l=${LLVM:-/opt/homebrew/opt/llvm}/LICENSE.TXT
		put "$l" "$d/LLVM"   # Apache-2.0 with LLVM exception, lld's the same
	fi
	;;
*)
	die "usage: licenses.sh kext <Resources> | bundle <bundle>"
	;;
esac
