//
//  linuxref.hpp
//  RDNA4FB
//
//  W42/W43: the registers Linux amdgpu 7.2.2 reads on this RX 9070 XT (E:\linux\rdna4-groundtruth-{idle,vkcube}-*\gc-regs.txt), for the
//  `linux diff:` lines gfxring.cpp logs before the draw. Kept in a header with a bounded formatter so a host test can feed it worst-case
//  strings (the first version accumulated lines in a char[300] with unchecked snprintf lengths: review BLOCKER).
//
//  `mask` limits the comparison to bits that are meaningful (ring size, addresses and doorbell index differ by design); `why` says what a
//  known difference is.
//

#ifndef LinuxRef_hpp
#define LinuxRef_hpp

#include <stddef.h>
#include <stdint.h>
// snprintf: the kext builds with -nostdinc and MacKernelSDK has no top-level stdio.h (only sys/stdio.h); an unqualified <stdio.h> there
// resolves to libc++'s wrapper, which does not declare snprintf, so the header only compiled when another header had already pulled
// libkern in first (W44). KERNEL is defined by the kext build (-DKERNEL); the host test (tools/atomdump.cpp) is a plain userland build.
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

#include "gfxregs.hpp"

namespace LinuxRefTable {

struct Ref {
	const char   *name;
	GfxReg::Reg   reg;
	uint32_t      linuxValue;
	uint32_t      mask;
	const char   *why;
};

// Longest line format() may produce for the real table is checked by the host test against this.
constexpr size_t kLineMax = 512;

static const Ref kRefs[] = {
	{ "GCVM_L2_CNTL", { 0, 0x15c4 }, 0x00080e01, 0xffffffff, "bit 11 ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY: Linux points faults at a system dummy page; ours is VRAM, so it stays off" },
	{ "GCVM_L2_CNTL2", { 0, 0x15c5 }, 0x00000003, 0xffffffff, "" },
	{ "GCVM_L2_CNTL3", { 0, 0x15c6 }, 0x80130009, 0xffffffff, "" },
	{ "GCVM_L2_CNTL4", { 0, 0x15dd }, 0x00000001, 0xffffffff, "" },
	{ "GCVM_L2_CNTL5", { 0, 0x15e3 }, 0x00003fe0, 0xffffffff, "" },
	{ "GCVM_L2_PROTECTION_FAULT_CNTL", { 0, 0x15cc }, 0x3ffffffc, 0xffffffff, "" },
	{ "GCVM_L2_PROTECTION_FAULT_CNTL2", { 0, 0x15cd }, 0x00060000, 0xffffffff, "" },
	{ "GCMC_VM_MX_L1_TLB_CNTL", { 0, 0x161b }, 0x00001859, 0xffffffff, "" },
	{ "GCVM_CONTEXTS_DISABLE", { 0, 0x1634 }, 0x00000000, 0xffffffff, "" },
	{ "GCVM_CONTEXT0_CNTL", { 0, 0x1624 }, 0x03fffc01, 0xffffffff, "Linux: 3-level GART range 0..0x1ffff pages; ours: flat, one page" },
	{ "GCVM_CONTEXT1_CNTL", { 0, 0x1625 }, 0x03fffc07, 0xffffffff, "bits 24-25 are reset defaults Linux keeps (RMW); ours wrote them as 0 until W42" },
	{ "GCVM_CONTEXT8_CNTL", { 0, 0x162c }, 0x03fffc07, 0xffffffff, "bits 24-25, as CONTEXT1" },
	{ "GCMC_VM_FB_LOCATION_BASE", { 0, 0x1614 }, 0x00008000, 0xffffffff, "" },
	{ "GCMC_VM_FB_LOCATION_TOP", { 0, 0x1615 }, 0x000083fb, 0xffffffff, "" },
	{ "GCMC_VM_FB_OFFSET", { 0, 0x15a7 }, 0x00000000, 0xffffffff, "" },
	{ "GCMC_VM_SYSTEM_APERTURE_LOW", { 0, 0x1619 }, 0x00200000, 0xffffffff, "" },
	{ "GCMC_VM_SYSTEM_APERTURE_HIGH", { 0, 0x161a }, 0x0020febf, 0xffffffff, "Linux ends the aperture 16 MiB below FB top; ours covers the whole FB (a superset)" },
	{ "GCMC_VM_AGP_BOT", { 0, 0x1617 }, 0x00ffffff, 0xffffffff, "" },
	{ "GCMC_VM_AGP_TOP", { 0, 0x1616 }, 0x00000000, 0xffffffff, "" },
	{ "GCMC_VM_AGP_BASE", { 0, 0x1618 }, 0x00000000, 0xffffffff, "" },
	{ "RLC_SRM_CNTL", { 1, 0x4c80 }, 0x00000003, 0xffffffff, "" },
	{ "RLC_CNTL", { 1, 0x4c00 }, 0x00000001, 0xffffffff, "" },
	{ "RLC_CSIB_LENGTH", { 1, 0x0989 }, 0x0000004b, 0xffffffff, "" },
	{ "CP_ME_CNTL", { 1, 0x0803 }, 0x0000a000, 0xffffffff, "bit 24 CE_HALT: amdgpu never writes it; Linux reads 0, ours still 1 (rdna4-gfxce=1 clears it)" },
	{ "CP_RB_ACTIVE", { 0, 0x1f40 }, 0x00000001, 0xffffffff, "" },
	{ "CP_RB0_CNTL", { 0, 0x1de1 }, 0x00f0088a, 0x00f0c0c0, "ring size differs by design (BUFSZ/BLKSZ masked); bits 20-23 MIN_AVAILSZ/MIN_IB_AVAILSZ = 3 come from the MQD's reset-default CNTL (rdna4-gfxrbmin=1 sets them)" },
	{ "CP_GFX_HQD_ACTIVE", { 0, 0x1e80 }, 0x00000001, 0xffffffff, "Linux runs the gfx queue through an HQD/MQD; ours is a bare RB0 (rootcause-draw.md #2)" },
	{ "CP_GFX_HQD_CNTL", { 0, 0x1e8f }, 0x00f0088a, 0x00f0c0c0, "as CP_RB0_CNTL: the MQD/HQD path" },
	{ "CP_GFX_MQD_BASE_ADDR_HI", { 0, 0x1e7f }, 0x00000080, 0xffffffff, "Linux: MQD in VRAM (0x80_00108000); ours has no MQD" },
	{ "CP_GFX_HQD_VMID", { 0, 0x1e81 }, 0x00000000, 0xffffffff, "" },
	{ "GB_ADDR_CONFIG", { 0, 0x13de }, 0x08200545, 0xffffffff, "" },
	{ "GRBM_CNTL", { 0, 0x0da0 }, 0x00300018, 0xffffffff, "" },
	{ "SH_MEM_CONFIG (VMID 0)", { 1, 0x09e4 }, 0x0000c00c, 0xffffffff, "" },
	{ "SH_MEM_BASES (VMID 0)", { 1, 0x09e3 }, 0x00000000, 0xffffffff, "" },
	{ "CP_MEC_RS64_CNTL", { 1, 0x2904 }, 0x3c000000, 0xffffffff, "" },
	{ "CP_MES_CNTL", { 1, 0x2807 }, 0x0c000000, 0xffffffff, "" },
	{ "CP_GFX_RS64_DC_BASE0_HI", { 1, 0x5865 }, 0x00000003, 0xffffffff, "Linux: data cache bases set by the PSP loader (0x3_f9600000 / 0x3_f9700000); 0 here = the loader did not set them" },
	{ "CP_GFX_RS64_DC_BASE1_HI", { 1, 0x5866 }, 0x00000003, 0xffffffff, "" },
	{ "GE_CNTL", { 1, 0x225b }, 0xa0010080, 0xffffffff, "" },
	{ "PA_SC_MODE_CNTL_0", { 1, 0x0292 }, 0x00000022, 0xffffffff, "" },
	{ "VGT_PRIMITIVE_TYPE", { 1, 0x2242 }, 0x00000000, 0xffffffff, "Linux reads 0 too (idle and with vkcube running): a 0 readback is NOT evidence of a missing primitive type" },
};

constexpr size_t kCount = sizeof(kRefs) / sizeof(kRefs[0]);

// "DIFF <name> ours 0x.. linux 0x.. (<why>)" into out[cap]. Always NUL-terminated (cap >= 1), never writes past cap, returns the
// length written (0 when cap is 0). A too-long why is truncated, never overflowed.
inline size_t format(char *out, size_t cap, const Ref &r, uint32_t ours) {
	if (!out || !cap)
		return 0;
	int n = snprintf(out, cap, "DIFF %s ours 0x%08x linux 0x%08x", r.name, ours, r.linuxValue);
	if (n < 0) {
		out[0] = '\0';
		return 0;
	}
	size_t len = static_cast<size_t>(n) >= cap ? cap - 1 : static_cast<size_t>(n);
	if (r.why && r.why[0] && len + 1 < cap) {
		n = snprintf(out + len, cap - len, " (%s)", r.why);
		if (n > 0)
			len = static_cast<size_t>(n) >= cap - len ? cap - 1 : len + static_cast<size_t>(n);
	}
	out[len] = '\0';
	return len;
}

} // namespace LinuxRefTable

#endif /* LinuxRef_hpp */
