//
//  amdfw.cpp
//  RDNA4FB
//
//  See amdfw.hpp.
//

#include "amdfw.hpp"

namespace AmdFw {

namespace {
uint32_t le32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
// [off, off + len) lies inside [0, size).
bool inside(size_t size, uint64_t off, uint64_t len) {
	return off <= size && len <= size - off;
}
} // namespace

bool parseCommon(const uint8_t *d, size_t n, Common &c) {
	if (!d || n < 32)
		return false;
	c.sizeBytes             = le32(d + 0);
	c.headerSizeBytes       = le32(d + 4);
	c.headerMajor           = le16(d + 8);
	c.headerMinor           = le16(d + 10);
	c.ipMajor               = le16(d + 12);
	c.ipMinor               = le16(d + 14);
	c.ucodeVersion          = le32(d + 16);
	c.ucodeSizeBytes        = le32(d + 20);
	c.ucodeArrayOffsetBytes = le32(d + 24);
	c.crc32                 = le32(d + 28);
	return c.sizeBytes <= n && c.headerSizeBytes >= 32 && c.headerSizeBytes <= n;
}

bool payload(const uint8_t *d, size_t n, Blob &out) {
	out = Blob {};
	Common c;
	if (!parseCommon(d, n, c) || !c.ucodeSizeBytes ||
	    !inside(n, c.ucodeArrayOffsetBytes, c.ucodeSizeBytes))
		return false;
	out = { d + c.ucodeArrayOffsetBytes, c.ucodeSizeBytes };
	return true;
}

const char *pspPartName(uint32_t part) {
	switch (part) {
	case PspSos:         return "SOS";
	case PspSysDrv:      return "SYS_DRV";
	case PspKdb:         return "KDB";
	case PspToc:         return "TOC";
	case PspSpl:         return "SPL";
	case PspRl:          return "RL";
	case PspSocDrv:      return "SOC_DRV";
	case PspIntfDrv:     return "INTF_DRV";
	case PspDbgDrv:      return "DBG_DRV";
	case PspRasDrv:      return "RAS_DRV";
	case PspIpKeyMgrDrv: return "IPKEYMGR_DRV";
	case PspSpdmDrv:     return "SPDM_DRV";
	default:             return "?";
	}
}

bool parsePsp(const uint8_t *d, size_t n, PspPackage &out) {
	out = PspPackage {};
	Common c;
	if (!parseCommon(d, n, c) || c.headerMajor != 2 || n < 36)
		return false;
	uint32_t count = le32(d + 32);
	size_t descBase = 36;
	if (c.headerMinor == 1) {
		if (n < 40)
			return false;
		uint32_t auxIndex = le32(d + 36);   // aux images follow the primary ones
		descBase = 40;
		if (auxIndex > count)
			return false;
		count -= auxIndex;
	}
	if (count >= 32 || !inside(n, descBase, static_cast<uint64_t>(count) * 16))
		return false;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *desc = d + descBase + i * 16;
		uint32_t type = le32(desc), ver = le32(desc + 4);
		uint64_t off = static_cast<uint64_t>(c.ucodeArrayOffsetBytes) + le32(desc + 8);
		uint32_t len = le32(desc + 12);
		if (type == 0 || type >= PspPartCount || !len || !inside(n, off, len))
			return false;
		out.part[type] = { d + off, len };
		out.version[type] = ver;
	}
	out.count = count;
	return out.part[PspSos].valid() && out.part[PspSysDrv].valid();
}

// ---------------------------------------------------------------------------
// GC 12 firmware set
// ---------------------------------------------------------------------------

namespace {

// psp_gfx_fw_type values (psp_gfx_if.h).
enum : uint32_t {
	kTypeRlcG            = 8,
	kTypeRlcListGpm      = 20,
	kTypeRlcListSrm      = 21,
	kTypeRlcListCntl     = 22,
	kTypeRlcIram         = 26,
	kTypeCpMes           = 33,
	kTypeMesStack        = 34,
	kTypeRlcDramBoot     = 48,
	kTypeImuI            = 68,
	kTypeImuD            = 69,
	kTypeSdmaTh0         = 71,
	kTypeRs64Pfp         = 87,
	kTypeRs64Me          = 88,
	kTypeRs64Mec         = 89,
	kTypeRs64PfpStack0   = 90,
	kTypeRs64MeStack0    = 92,
	kTypeRs64MecStack0   = 94,
	kTypeRs64MecStack1   = 95,
};

// A payload [off, off + len) of `b`, or an invalid Blob.
Blob span(const Blob &b, uint64_t off, uint64_t len) {
	if (!b.valid() || !len || !inside(b.size, off, len))
		return Blob {};
	return { b.data + off, static_cast<uint32_t>(len) };
}

uint32_t at(const Blob &b, uint32_t off) {
	return (b.valid() && inside(b.size, off, 4)) ? le32(b.data + off) : 0;
}

struct Builder {
	GfxImage   *out;
	uint32_t    cap, n;
	const char *why;
	bool add(const char *name, uint32_t type, const Blob &payload) {
		if (!payload.valid()) {
			why = name;
			return false;
		}
		if (n >= cap) {
			why = "too many images";
			return false;
		}
		out[n++] = { name, type, payload };
		return true;
	}
};

} // namespace

uint32_t buildGfxImages(const GfxBlobs &b, GfxImage *out, uint32_t cap, const char **why) {
	Builder bl { out, cap, 0, nullptr };
	Common c {};
	auto fail = [&](const char *w) -> uint32_t {
		if (why)
			*why = w;
		return 0;
	};

	// SDMA_RS64: sdma_firmware_header_v3_0.ucode_size_bytes at the array offset.
	if (!parseCommon(b.sdma.data, b.sdma.size, c) || c.headerMajor != 3)
		return fail("sdma header");
	if (!bl.add("SDMA_RS64", kTypeSdmaTh0, span(b.sdma, c.ucodeArrayOffsetBytes, at(b.sdma, 40))))
		return fail(bl.why);

	// RS64 CP: gfx_firmware_header_v2_0 — ucode at the array offset
	// (ucode_size_bytes @36), stack at data_offset_bytes @48 (size @44).
	struct Cp { const Blob *blob; const char *name, *stack0, *stack1; uint32_t type, t0, t1; };
	const Cp cps[] = {
		{ &b.pfp, "RS64_PFP", "RS64_PFP_P0_STACK", nullptr, kTypeRs64Pfp, kTypeRs64PfpStack0, 0 },
		{ &b.me,  "RS64_ME",  "RS64_ME_P0_STACK",  nullptr, kTypeRs64Me,  kTypeRs64MeStack0,  0 },
		{ &b.mec, "RS64_MEC", "RS64_MEC_P0_STACK", "RS64_MEC_P1_STACK", kTypeRs64Mec,
		  kTypeRs64MecStack0, kTypeRs64MecStack1 },
	};
	Blob stack[3] {};
	for (int i = 0; i < 3; i++) {
		const Blob &f = *cps[i].blob;
		if (!parseCommon(f.data, f.size, c) || c.headerMajor != 2)
			return fail(cps[i].name);
		if (!bl.add(cps[i].name, cps[i].type, span(f, c.ucodeArrayOffsetBytes, at(f, 36))))
			return fail(bl.why);
		stack[i] = span(f, at(f, 48), at(f, 44));
	}
	// amdgpu's id order puts all three ucodes first, then the stacks.
	for (int i = 0; i < 3; i++) {
		if (!bl.add(cps[i].stack0, cps[i].t0, stack[i]))
			return fail(bl.why);
		if (cps[i].stack1 && !bl.add(cps[i].stack1, cps[i].t1, stack[i]))
			return fail(bl.why);
	}

	// MES (unified): mes_firmware_header_v1_0 — ucode size/offset @36/@40,
	// data size/offset @48/@52.
	if (!parseCommon(b.mes.data, b.mes.size, c) || c.headerMajor != 1)
		return fail("mes header");
	if (!bl.add("CP_MES", kTypeCpMes, span(b.mes, at(b.mes, 40), at(b.mes, 36))) ||
	    !bl.add("CP_MES_DATA", kTypeMesStack, span(b.mes, at(b.mes, 52), at(b.mes, 48))))
		return fail(bl.why);

	// IMU: imu_firmware_header_v1_0 — IRAM (size @32) at the array offset,
	// DRAM (size @40) right after it.
	if (!parseCommon(b.imu.data, b.imu.size, c) || c.headerMajor != 1)
		return fail("imu header");
	const uint32_t iram = at(b.imu, 32);
	if (!bl.add("IMU_I", kTypeImuI, span(b.imu, c.ucodeArrayOffsetBytes, iram)) ||
	    !bl.add("IMU_D", kTypeImuD,
	            span(b.imu, static_cast<uint64_t>(c.ucodeArrayOffsetBytes) + iram, at(b.imu, 40))))
		return fail(bl.why);

	// RLC: rlc_firmware_header_v2_1 save/restore lists (optional, when
	// non-empty), v2_2 IRAM/DRAM, and RLC_G (the common payload), last.
	if (!parseCommon(b.rlc.data, b.rlc.size, c) || c.headerMajor != 2 || c.headerMinor < 2)
		return fail("rlc header");
	struct List { const char *name; uint32_t type, sizeOff, offOff; };
	const List lists[] = {
		{ "RLC_RESTORE_LIST_CNTL", kTypeRlcListCntl, 116, 120 },
		{ "RLC_RESTORE_LIST_GPM", kTypeRlcListGpm, 132, 136 },
		{ "RLC_RESTORE_LIST_SRM", kTypeRlcListSrm, 148, 152 },
	};
	for (const List &l : lists)
		if (at(b.rlc, l.sizeOff) && !bl.add(l.name, l.type, span(b.rlc, at(b.rlc, l.offOff),
		                                                           at(b.rlc, l.sizeOff))))
			return fail(bl.why);
	if (!bl.add("RLC_IRAM", kTypeRlcIram, span(b.rlc, at(b.rlc, 160), at(b.rlc, 156))) ||
	    !bl.add("RLC_DRAM", kTypeRlcDramBoot, span(b.rlc, at(b.rlc, 168), at(b.rlc, 164))) ||
	    !bl.add("RLC_G", kTypeRlcG, span(b.rlc, c.ucodeArrayOffsetBytes, c.ucodeSizeBytes)))
		return fail(bl.why);

	if (why)
		*why = nullptr;
	return bl.n;
}

} // namespace AmdFw
