//
//  codeobj.cpp
//  RDNA4FB
//
//  See codeobj.hpp.
//

#include "codeobj.hpp"

namespace CodeObj {

namespace {
constexpr uint16_t kEmAmdgpu    = 224;
constexpr uint32_t kMachGfx1201 = 0x4e;           // EF_AMDGPU_MACH_AMDGCN_GFX1201
constexpr uint32_t kShtSymtab   = 2, kShtDynsym = 11;

uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t u64(const uint8_t *p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }
bool inside(size_t size, uint64_t off, uint64_t len) { return off <= size && len <= size - off; }

bool same(const uint8_t *s, size_t max, const char *name, const char *suffix) {
	size_t i = 0;
	for (; *name; name++, i++)
		if (i >= max || s[i] != static_cast<uint8_t>(*name))
			return false;
	for (; suffix && *suffix; suffix++, i++)
		if (i >= max || s[i] != static_cast<uint8_t>(*suffix))
			return false;
	return i < max && s[i] == 0;
}

struct Elf {
	const uint8_t *d;
	size_t n;
	uint64_t shoff;
	uint16_t shentsize, shnum;

	const uint8_t *sh(uint32_t i) const { return d + shoff + static_cast<uint64_t>(i) * shentsize; }

	// File offset of virtual address `va` (and bytes left in its section).
	bool fileOffset(uint64_t va, uint64_t &off, uint64_t &left) const {
		for (uint32_t i = 1; i < shnum; i++) {
			const uint8_t *s = sh(i);
			uint64_t addr = u64(s + 16), soff = u64(s + 24), size = u64(s + 32);
			if (addr && va >= addr && va < addr + size && inside(n, soff, size)) {
				off = soff + (va - addr);
				left = size - (va - addr);
				return true;
			}
		}
		return false;
	}
};
} // namespace

bool findKernel(const uint8_t *d, size_t n, const char *name, Kernel &out, const char **why) {
	auto fail = [why](const char *w) {
		if (why)
			*why = w;
		return false;
	};
	out = Kernel {};
	if (!d || n < 64 || d[0] != 0x7f || d[1] != 'E' || d[2] != 'L' || d[3] != 'F' ||
	    d[4] != 2 /* ELFCLASS64 */ || d[5] != 1 /* little-endian */)
		return fail("not an ELF64 little-endian file");
	if (u16(d + 18) != kEmAmdgpu)
		return fail("not an AMDGPU code object");
	if ((u32(d + 48) & 0xff) != kMachGfx1201)
		return fail("not built for gfx1201");

	Elf e { d, n, u64(d + 40), u16(d + 58), u16(d + 60) };
	if (e.shentsize < 64 || !inside(n, e.shoff, static_cast<uint64_t>(e.shnum) * e.shentsize))
		return fail("bad section headers");

	uint64_t codeVa = 0, kdVa = 0;
	bool haveCode = false, haveKd = false;
	for (uint32_t i = 1; i < e.shnum && !(haveCode && haveKd); i++) {
		const uint8_t *s = e.sh(i);
		uint32_t type = u32(s + 4);
		if (type != kShtSymtab && type != kShtDynsym)
			continue;
		uint64_t symOff = u64(s + 24), symSize = u64(s + 32), entSize = u64(s + 56);
		uint32_t link = u32(s + 40);
		if (entSize < 24 || link >= e.shnum || !inside(n, symOff, symSize))
			continue;
		const uint8_t *strSh = e.sh(link);
		uint64_t strOff = u64(strSh + 24), strSize = u64(strSh + 32);
		if (!inside(n, strOff, strSize))
			continue;
		for (uint64_t k = 1; k < symSize / entSize; k++) {
			const uint8_t *sym = d + symOff + k * entSize;
			uint32_t nameOff = u32(sym);
			if (nameOff >= strSize)
				continue;
			const uint8_t *nm = d + strOff + nameOff;
			size_t max = static_cast<size_t>(strSize - nameOff);
			uint8_t kind = sym[4] & 0xf;          // STT_*
			if (kind == 2 /* FUNC */ && same(nm, max, name, nullptr)) {
				codeVa = u64(sym + 8);
				haveCode = true;
			} else if (kind == 1 /* OBJECT */ && same(nm, max, name, ".kd")) {
				kdVa = u64(sym + 8);
				haveKd = true;
			}
		}
	}
	if (!haveKd)
		return fail("kernel descriptor <name>.kd not found");

	uint64_t kdOff, left;
	if (!e.fileOffset(kdVa, kdOff, left) || left < 64)
		return fail("kernel descriptor outside the file");
	const uint8_t *kd = d + kdOff;
	out.groupSegmentSize   = u32(kd + 0);
	out.privateSegmentSize = u32(kd + 4);
	out.kernargSize        = u32(kd + 8);
	const int64_t entry    = static_cast<int64_t>(u64(kd + 16));
	out.rsrc3      = u32(kd + 44);
	out.rsrc1      = u32(kd + 48);
	out.rsrc2      = u32(kd + 52);
	out.properties = u16(kd + 56);

	// The descriptor's entry offset is relative to the descriptor itself.
	const uint64_t entryVa = kdVa + static_cast<uint64_t>(entry);
	if (haveCode && codeVa != entryVa)
		return fail("kernel symbol and descriptor entry disagree");
	uint64_t codeOff, codeLeft;
	if (!e.fileOffset(entryVa, codeOff, codeLeft) || codeLeft < 4 || codeOff > 0xffffffffu)
		return fail("kernel code outside the file");
	out.codeOffset = static_cast<uint32_t>(codeOff);
	out.codeSize = codeLeft > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(codeLeft);
	if (why)
		*why = nullptr;
	return true;
}

} // namespace CodeObj
