#!/usr/bin/env bash
# Fetch the RX 9070 XT (gfx1201, PSP 14.0.3, SMU 14.0.3, SDMA 7.0.1) firmware
# the compute bring-up embeds, from upstream linux-firmware, into
# firmware/amdgpu/ (git-ignored). Redistributable under AMD's license, fetched
# alongside as LICENSE.amdgpu. Cards with PCI revision 0xC8 need the
# *_kicker variants instead (not handled here; this card is revision 0xC0).
#
#   tools/fetch-firmware.sh
set -euo pipefail
cd "$(dirname "$0")/.."
D=firmware/amdgpu
B=https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main
mkdir -p "$D"
for f in psp_14_0_3_sos.bin psp_14_0_3_ta.bin smu_14_0_3.bin sdma_7_0_1.bin \
         gc_12_0_1_imu.bin gc_12_0_1_me.bin gc_12_0_1_mec.bin gc_12_0_1_pfp.bin \
         gc_12_0_1_rlc.bin gc_12_0_1_toc.bin gc_12_0_1_mes.bin gc_12_0_1_mes1.bin \
         gc_12_0_1_uni_mes.bin; do
	curl -fsSL -o "$D/$f" "$B/amdgpu/$f"
	echo "fetched $f ($(wc -c < "$D/$f") bytes)"
done
curl -fsSL -o "$D/LICENSE.amdgpu" "$B/LICENSES/LICENSE.amdgpu"
