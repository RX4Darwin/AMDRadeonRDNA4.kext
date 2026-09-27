import sys
# Generates emu/qemu/gop-state.txt:  python3 emu/qemu/gen-gop-state.py emu/qemu/gop-state.txt
BASE = {1: 0xc0, 2: 0x34c0, 3: 0x9000, 'mp0': 0x16000}
IDX = {1: 1, 2: 2, 3: 3, 'mp0': 0}
regs = [
    ('# OTG0: CEA 1920x1080@60 (148.5 MHz), images as src/otgtiming.cpp computes them', None),
    ('OTG0_OTG_H_TOTAL', 2, 0x1b2a, 0x00000897),
    ('OTG0_OTG_H_BLANK_START_END', 2, 0x1b2b, 0x00c00840),
    ('OTG0_OTG_H_SYNC_A', 2, 0x1b2c, 0x002c0000),
    ('OTG0_OTG_H_SYNC_A_CNTL', 2, 0x1b2d, 0x00000000),
    ('OTG0_OTG_V_TOTAL', 2, 0x1b2f, 0x00000464),
    ('OTG0_OTG_V_BLANK_START_END', 2, 0x1b38, 0x00290461),
    ('OTG0_OTG_V_SYNC_A', 2, 0x1b39, 0x00050000),
    ('OTG0_OTG_V_SYNC_A_CNTL', 2, 0x1b3a, 0x00000000),
    ('OTG0_OTG_CONTROL', 2, 0x1b43, 0x00010001),
    ('OTG0_OTG_CLOCK_CONTROL', 2, 0x1b84, 0x00000003),
    ('ODM0_OPTC_INPUT_CLOCK_CONTROL', 2, 0x1ad0, 0x00000003),
    ('VTG0_CONTROL', 2, 0x0530, 0x84610000),
    ('# ODM0 -> OPP0 -> MPCC0 -> HUBP0; other MPCCs unused', None),
    ('ODM0_OPTC_DATA_SOURCE_SELECT', 2, 0x1acb, 0x00000000),
    ('MPCC0_MPCC_OPP_ID', 3, 0x0002, 0x00000000),
    ('MPCC1_MPCC_OPP_ID', 3, 0x0017, 0x0000000f),
    ('MPCC2_MPCC_OPP_ID', 3, 0x002c, 0x0000000f),
    ('MPCC3_MPCC_OPP_ID', 3, 0x0041, 0x0000000f),
    ('DPG0_DPG_CONTROL', 2, 0x1854, 0x00000000),
    ('# HUBP0 scans VRAM offset 0 (MC 0x80_0000_0000), 1920x1080 ARGB8888, pitch 1920', None),
    ('HUBP0_DCSURF_SURFACE_CONFIG', 2, 0x05e5, 0x00000008),
    ('HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION', 2, 0x05eb, 0x04380780),
    ('HUBPREQ0_DCSURF_SURFACE_PITCH', 2, 0x0607, 0x0000077f),
    ('HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS', 2, 0x060a, 0x00000000),
    ('HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH', 2, 0x060b, 0x00000080),
    ('DSCL0_RECOUT_SIZE', 2, 0x0d1f, 0x04380780),
    ('DSCL0_MPC_SIZE', 2, 0x0d20, 0x04380780),
    ('DCN_VM_FB_LOCATION_BASE', 2, 0x0475, 0x00008000),
    ('DCN_VM_FB_LOCATION_TOP', 2, 0x0476, 0x000083fa),
    ('# DIG2 (HDMI) sources OTG0 and drives link 2 (UNIPHYC), HPD 3: the HDMI', None),
    ('# connector on DDC line 2 / hpd3 in the card\'s DisplayObjectInfo', None),
    ('DIG2_DIG_FE_CNTL', 2, 0x22db, 0x00000000),
    ('DIG2_DIG_FE_CLK_CNTL', 2, 0x22dc, 0x00000013),
    ('DIG2_DIG_FE_EN_CNTL', 2, 0x22dd, 0x00000001),
    ('DIG2_DIG_FIFO_CTRL0', 2, 0x22e3, 0x0000001d),
    ('DIG2_STREAM_MAPPER_CONTROL', 2, 0x1f0f, 0x00000002),
    ('DIG2_DIG_BE_CLK_CNTL', 2, 0x2303, 0x00000013),
    ('DIG2_DIG_BE_CNTL', 2, 0x2304, 0x30000400),
    ('DIG2_DIG_BE_EN_CNTL', 2, 0x2305, 0x00000001),
    ('# HPD: only hpd3 (HDMI) sees a sink; hpd1..4 = bits 0/8/16/24', None),
    ('DC_GPIO_HPD_Y', 2, 0x28f7, 0x00010000),
    ('# AUX engines enabled but idle (nothing on the DP connectors)', None),
    ('DP_AUX0_AUX_CONTROL', 2, 0x16b2, 0x00000001),
    ('DP_AUX1_AUX_CONTROL', 2, 0x16ce, 0x00000001),
    ('# DC_I2C left in soft reset with its RAM in light sleep (seen on hardware)', None),
    ('DC_I2C_CONTROL', 2, 0x1e98, 0x00000002),
    ('DIO_MEM_PWR_CTRL', 2, 0x1ede, 0x00000001),
    ('MICROSECOND_TIME_BASE_DIV', 1, 0x007b, 0x00120464),
    ('# DMUB inbox1 ring in firmware memory 48 MiB below the top of VRAM', None),
    ("# (REGION4 = MC 0x80_0000_0000 + 16256 MiB), idle; the GOP's own command", None),
    ('# history is not known, so the ring starts empty', None),
    ('DMCUB_REGION4_OFFSET', 2, 0x0196, 0xf8000000),
    ('DMCUB_REGION4_OFFSET_HIGH', 2, 0x0197, 0x00000083),
    ('DMCUB_INBOX1_SIZE', 2, 0x01d5, 0x00004000),
    ('DMCUB_INBOX1_WPTR', 2, 0x01d6, 0x00000000),
    ('DMCUB_INBOX1_RPTR', 2, 0x01d7, 0x00000000),
    ('# PSP (MP0 segment 0): bootloader ready; no sOS, the model runs no PSP firmware', None),
    ('MP0_SMN_C2PMSG_35', 'mp0', 0x0063, 0x80000000),
]
out = ['''# RX 9070 XT register image at OS handoff (what the GOP leaves behind),
# for the rdna4 QEMU device (state=). Format of tools/linux-capture.sh:
#   NAME base_idx dword_off byte_addr 0xVALUE
# base_idx/dword_off are segment-relative within the register's IP block
# (DMU = DCN 4.1.0 unless the name says MP0); byte_addr is the BAR5 offset
# through the card's IP discovery bases (DMU seg1 0xc0, seg2 0x34c0, seg3
# 0x9000; MP0 seg0 0x16000). Unlisted registers power on as 0.
#
# SYNTHESIZED, not captured: the layout follows amdgpu's register semantics
# and the values the kext expects (see tools/atomdump.cpp testPipeDiscovery).
# Replace with a real capture of the GOP-left state when one exists.
''']
for r in regs:
    if r[1] is None:
        out.append('\n' + r[0])
        continue
    name, seg, dw, val = r
    out.append(f'{name} {IDX[seg]} 0x{dw:04x} 0x{(BASE[seg] + dw) * 4:05x} 0x{val:08x}')
open(sys.argv[1], 'w', newline='\n').write('\n'.join(out) + '\n')
print('ok')
