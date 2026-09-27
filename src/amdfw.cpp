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

} // namespace AmdFw
