#!/usr/bin/env bash
# Check out the firmware the compute bring-up embeds: the submodule lib/linux-firmware (upstream linux-firmware, at
# the commit .gitmodules' parent records), sparse and without history blobs, so only these files are downloaded:
# the Navi 48 set (RX 9070 XT, RX 9070: gfx1201, PSP 14.0.3, SMU 14.0.3, SDMA 7.0.1), the Navi 44 set (RX 9060 XT:
# gfx1200, PSP 14.0.2, SMU 14.0.2, SDMA 7.0.0), the display firmware both take (DCN 4.1.0) and AMD's licence for
# them. The submodule is "update = none", so a recursive clone does not fetch all of linux-firmware. Cards with PCI
# revision 0xC8 need the *_kicker variants instead (not handled here; the cards seen are revision 0xC0).
#
#   tools/build/fetch-firmware.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
S=lib/linux-firmware
url=$(git config -f .gitmodules submodule.$S.url)
commit=$(git ls-tree HEAD "$S" | awk '{print $3}')
[ -n "$commit" ] || { echo "fetch-firmware: $S is not a submodule here" >&2; exit 1; }
files=""
for f in psp_14_0_3_sos.bin psp_14_0_3_ta.bin smu_14_0_3.bin sdma_7_0_1.bin \
         gc_12_0_1_imu.bin gc_12_0_1_me.bin gc_12_0_1_mec.bin gc_12_0_1_pfp.bin \
         gc_12_0_1_rlc.bin gc_12_0_1_toc.bin gc_12_0_1_mes.bin gc_12_0_1_mes1.bin \
         gc_12_0_1_uni_mes.bin \
         psp_14_0_2_sos.bin psp_14_0_2_ta.bin smu_14_0_2.bin sdma_7_0_0.bin \
         gc_12_0_0_imu.bin gc_12_0_0_me.bin gc_12_0_0_mec.bin gc_12_0_0_pfp.bin \
         gc_12_0_0_rlc.bin gc_12_0_0_toc.bin gc_12_0_0_mes.bin gc_12_0_0_mes1.bin \
         gc_12_0_0_uni_mes.bin \
         dcn_4_0_1_dmcub.bin; do
	files="$files /amdgpu/$f"
done
if [ ! -e "$S/.git" ]; then
	command rm -rf "$S"
	git clone -q --filter=blob:none --no-checkout "$url" "$S"
	git submodule absorbgitdirs "$S" 2>/dev/null || true
fi
git -C "$S" sparse-checkout init --no-cone
# shellcheck disable=SC2086
git -C "$S" sparse-checkout set $files /LICENSES/LICENSE.amdgpu /WHENCE
git -C "$S" cat-file -e "$commit^{commit}" 2>/dev/null || git -C "$S" fetch -q --filter=blob:none origin "$commit"
git -C "$S" checkout -q "$commit"
echo "lib/linux-firmware at $(git -C "$S" log -1 --format='%h %ad %s' --date=short): $(ls "$S/amdgpu" | wc -l | tr -d ' ') files"
