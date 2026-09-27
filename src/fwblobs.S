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

// GC 12.0.1 set for the RLC autoload (stage 3), and SDMA 7.0.1.
.macro FWBLOB name, file
	.p2align 4
	.globl _rdna4_fw_\name
_rdna4_fw_\name:
	.incbin "\file"
	.globl _rdna4_fw_\name\()_end
_rdna4_fw_\name\()_end:
.endm

	FWBLOB sdma, "firmware/amdgpu/sdma_7_0_1.bin"
	FWBLOB pfp,  "firmware/amdgpu/gc_12_0_1_pfp.bin"
	FWBLOB me,   "firmware/amdgpu/gc_12_0_1_me.bin"
	FWBLOB mec,  "firmware/amdgpu/gc_12_0_1_mec.bin"
	FWBLOB mes,  "firmware/amdgpu/gc_12_0_1_uni_mes.bin"
	FWBLOB imu,  "firmware/amdgpu/gc_12_0_1_imu.bin"
	FWBLOB rlc,  "firmware/amdgpu/gc_12_0_1_rlc.bin"
