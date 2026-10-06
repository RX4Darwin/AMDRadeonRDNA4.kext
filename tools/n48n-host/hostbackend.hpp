//
//  hostbackend.hpp
//  RDNA4FB
//
//  N48N::Backend on a host: buffers are ordinary memory and GPU-physical
//  addresses are made up (they only have to be distinct and page aligned),
//  so the client engine of src/n48n.cpp can run without a card. Used by
//  tools/atomdump.cpp's checks and by the interposer that puts the engine
//  under the real RADV driver (interpose.cpp).
//

#ifndef RDNA4HostBackend_hpp
#define RDNA4HostBackend_hpp

#include <stdlib.h>
#include <string.h>
#include <map>

#include "../../src/n48n.hpp"

struct HostBackend {
	static constexpr uint64_t kVisible = 168ull << 20, kHigh = 15ull << 30;
	uint64_t visibleUsed = 0, highUsed = 0, nextPhysical = 0x100000000ull, nextSystem = 0x8000000000ull;
	uint64_t flushes = 0, tablePages = 0;
	std::map<uint64_t, void *> memory;   // by token: a buffer's bytes, or a table page

	void *bytesOf(const N48N::Memory &m) { return memory.count(m.token) ? memory[m.token] : nullptr; }

	N48N::Backend backend() {
		N48N::Backend b {};
		b.context = this;
		b.allocVram = [](void *c, uint64_t bytes, uint64_t, bool highAllowed, N48N::Memory &out) {
			auto *h = static_cast<HostBackend *>(c);
			const bool visible = h->visibleUsed + bytes <= kVisible;
			if (!visible && (!highAllowed || h->highUsed + bytes > kHigh))
				return false;
			(visible ? h->visibleUsed : h->highUsed) += bytes;
			out = N48N::Memory { h->nextPhysical, h->nextPhysical, false, visible, !visible };
			h->nextPhysical += bytes;
			h->memory[out.token] = calloc(1, bytes);
			return true;
		};
		b.allocSystem = [](void *c, uint64_t bytes, N48N::Memory &out) {
			auto *h = static_cast<HostBackend *>(c);
			out = N48N::Memory { h->nextSystem, 0, true, true, false };
			h->nextSystem += bytes;
			h->memory[out.token] = calloc(1, bytes);
			return true;
		};
		b.free = [](void *c, const N48N::Memory &m, uint64_t bytes) {
			auto *h = static_cast<HostBackend *>(c);
			if (!m.system)
				(m.high ? h->highUsed : h->visibleUsed) -= bytes;
			::free(h->memory[m.token]);
			h->memory.erase(m.token);
		};
		// A system buffer's pages are scattered on a card; here they are just told apart.
		b.systemPage = [](void *, const N48N::Memory &m, uint64_t page) { return m.token + page * 4096; };
		b.tables.context = this;
		b.tables.alloc = [](void *c, uint64_t &physical) {
			auto *h = static_cast<HostBackend *>(c);
			physical = h->nextPhysical;
			h->nextPhysical += 4096;
			h->memory[physical] = calloc(1, 4096);
			h->tablePages++;
			return true;
		};
		b.tables.free = [](void *c, uint64_t physical) {
			auto *h = static_cast<HostBackend *>(c);
			::free(h->memory[physical]);
			h->memory.erase(physical);
			h->tablePages--;
		};
		b.tables.entries = [](void *c, uint64_t physical) {
			auto *h = static_cast<HostBackend *>(c);
			return h->memory.count(physical) ? static_cast<uint64_t *>(h->memory[physical]) : nullptr;
		};
		b.flush = [](void *c) { static_cast<HostBackend *>(c)->flushes++; };
		b.readRegs = [](void *, uint32_t dword, uint32_t count, uint32_t *out) {
			for (uint32_t i = 0; i < count; i++)
				out[i] = dword + i == 0x263e ? 0x08200545u : 0;   // GB_ADDR_CONFIG as Mesa has it for this chip
			return true;
		};
		b.pools = [](void *c, N48N::Pools &out) {
			auto *h = static_cast<HostBackend *>(c);
			out = N48N::Pools { kVisible, kVisible - h->visibleUsed, kHigh, kHigh - h->highUsed };
		};
		return b;
	}
};

#endif /* RDNA4HostBackend_hpp */
