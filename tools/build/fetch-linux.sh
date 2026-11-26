#!/usr/bin/env bash
# Check out the parts of Linux this repository builds from: the submodule lib/linux (torvalds/linux, at the commit
# the parent records), sparse, one commit deep and without other blobs. The kext compiles amdgpu's GFX 12 clear-state
# tables from it (src/gfxring.cpp); the generators in tools/gen/pipegen run its display code and DSC helpers on the
# host. The submodule is "update = none", so a recursive clone does not fetch all of Linux.
#
#   tools/build/fetch-linux.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
S=lib/linux
url=$(git config -f .gitmodules submodule.$S.url)
commit=$(git ls-tree HEAD "$S" | awk '{print $3}')
[ -n "$commit" ] || { echo "fetch-linux: $S is not a submodule here" >&2; exit 1; }
if [ ! -e "$S/.git" ]; then
	command rm -rf "$S"
	git init -q "$S"
	git -C "$S" remote add origin "$url"
	git submodule absorbgitdirs "$S" 2>/dev/null || true
fi
git -C "$S" sparse-checkout init --no-cone
git -C "$S" sparse-checkout set \
	/drivers/gpu/drm/amd/display/ '/drivers/gpu/drm/amd/include/*.h' \
	'/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_*' \
	'/drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_0_*' \
	'/drivers/gpu/drm/amd/include/asic_reg/nbif/nbif_6_3_1_*' \
	/drivers/gpu/drm/amd/amdgpu/clearstate_defs.h /drivers/gpu/drm/amd/amdgpu/clearstate_gfx12.h \
	/drivers/gpu/drm/display/drm_dsc_helper.c \
	/include/drm/display/drm_dp.h /include/drm/display/drm_dsc.h /include/drm/display/drm_dsc_helper.h \
	/include/linux/hdmi.h /COPYING /LICENSES/preferred/GPL-2.0 /LICENSES/preferred/MIT
git -C "$S" cat-file -e "$commit^{commit}" 2>/dev/null ||
	git -C "$S" fetch -q --depth 1 --filter=blob:none origin "$commit"
git -C "$S" checkout -q "$commit"
echo "lib/linux at $(git -C "$S" log -1 --format='%h %ad %s' --date=short)"
