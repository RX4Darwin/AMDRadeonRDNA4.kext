//
//  fwblobs.S
//  AMDRadeonRDNA4
//
//  AMD firmware the compute bring-up hands to the PSP, embedded as-is from
//  lib/linux-firmware/amdgpu/ (linux-firmware blobs, redistributable under AMD's
//  license, lib/linux-firmware/LICENSES/LICENSE.amdgpu). The PSP authenticates them;
//  nothing here is modified. Built only when the files are present (see
//  the Makefile); otherwise the kext is built with AMDRadeonRDNA4_NO_FIRMWARE.
//
//  One set a chip (src/fwset.hpp), each embedded when its files are there:
//  fwconfig.h defines AMDRadeonRDNA4_FW_NAVI48 and AMDRadeonRDNA4_FW_NAVI44.
//

	.section __TEXT,__const

.macro FWBLOB name, file
	.p2align 4
	.globl _rdna4_fw_\name
_rdna4_fw_\name:
	.incbin "\file"
	.globl _rdna4_fw_\name\()_end
_rdna4_fw_\name\()_end:
.endm

#ifdef AMDRadeonRDNA4_FW_NAVI48
// Navi 48: PSP 14.0.3, SMU 14.0.3, the GC 12.0.1 set for the RLC autoload (stage 3), and SDMA 7.0.1.
	FWBLOB n48_psp_sos, "lib/linux-firmware/amdgpu/psp_14_0_3_sos.bin"
	FWBLOB n48_smu,     "lib/linux-firmware/amdgpu/smu_14_0_3.bin"
	FWBLOB n48_sdma,    "lib/linux-firmware/amdgpu/sdma_7_0_1.bin"
	FWBLOB n48_pfp,     "lib/linux-firmware/amdgpu/gc_12_0_1_pfp.bin"
	FWBLOB n48_me,      "lib/linux-firmware/amdgpu/gc_12_0_1_me.bin"
	FWBLOB n48_mec,     "lib/linux-firmware/amdgpu/gc_12_0_1_mec.bin"
	FWBLOB n48_mes,     "lib/linux-firmware/amdgpu/gc_12_0_1_uni_mes.bin"
	FWBLOB n48_imu,     "lib/linux-firmware/amdgpu/gc_12_0_1_imu.bin"
	FWBLOB n48_rlc,     "lib/linux-firmware/amdgpu/gc_12_0_1_rlc.bin"
#endif

#ifdef AMDRadeonRDNA4_FW_NAVI44
// Navi 44: PSP 14.0.2, SMU 14.0.2, the GC 12.0.0 set, and SDMA 7.0.0.
	FWBLOB n44_psp_sos, "lib/linux-firmware/amdgpu/psp_14_0_2_sos.bin"
	FWBLOB n44_smu,     "lib/linux-firmware/amdgpu/smu_14_0_2.bin"
	FWBLOB n44_sdma,    "lib/linux-firmware/amdgpu/sdma_7_0_0.bin"
	FWBLOB n44_pfp,     "lib/linux-firmware/amdgpu/gc_12_0_0_pfp.bin"
	FWBLOB n44_me,      "lib/linux-firmware/amdgpu/gc_12_0_0_me.bin"
	FWBLOB n44_mec,     "lib/linux-firmware/amdgpu/gc_12_0_0_mec.bin"
	FWBLOB n44_mes,     "lib/linux-firmware/amdgpu/gc_12_0_0_uni_mes.bin"
	FWBLOB n44_imu,     "lib/linux-firmware/amdgpu/gc_12_0_0_imu.bin"
	FWBLOB n44_rlc,     "lib/linux-firmware/amdgpu/gc_12_0_0_rlc.bin"
#endif

// The display microcontroller's firmware (DCN 4.0.1, both chips), loaded through the PSP with rdna4-dmubload=1.
	FWBLOB dmcub, "lib/linux-firmware/amdgpu/dcn_4_0_1_dmcub.bin"
