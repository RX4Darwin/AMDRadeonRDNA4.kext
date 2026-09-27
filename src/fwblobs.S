//
//  fwblobs.S
//  RDNA4FB
//
//  AMD firmware the compute bring-up hands to the PSP, embedded as-is from
//  firmware/amdgpu/ (linux-firmware blobs, redistributable under AMD's
//  license, firmware/amdgpu/LICENSE.amdgpu). The PSP authenticates them;
//  nothing here is modified. Built only when the files are present (see
//  the Makefile); otherwise the kext is built with RDNA4FB_NO_FIRMWARE.
//

	.section __TEXT,__const

	.p2align 4
	.globl _rdna4_fw_psp_sos
_rdna4_fw_psp_sos:
	.incbin "firmware/amdgpu/psp_14_0_3_sos.bin"
	.globl _rdna4_fw_psp_sos_end
_rdna4_fw_psp_sos_end:

	.p2align 4
	.globl _rdna4_fw_smu
_rdna4_fw_smu:
	.incbin "firmware/amdgpu/smu_14_0_3.bin"
	.globl _rdna4_fw_smu_end
_rdna4_fw_smu_end:
