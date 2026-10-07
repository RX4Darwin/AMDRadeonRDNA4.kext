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

	// The queue: nothing runs. Work is reported finished at once (`finishAt` 0), or once `pause` has been called
	// `finishAt` times since the last submission, or never (~0). The clock moves a millisecond with every pause.
	uint64_t finishAt = 0, pauses = 0, clockNs = 1, lostCalls = 0;
	bool     fault = false;      // what `faulted` answers
	uint32_t finishedSequence = 0, lastSequence = 0, lastCount = 0, busyReplies = 0, submitReply = N48N::kSuccess;
	N48N::Ib lastIbs[N48N_MAX_IBS] {};

	// The display: 2560x1440 showing the console; a buffer shown appears one frame later.
	static constexpr uint64_t kConsole = 0x8000000000ull;
	uint64_t shown = kConsole, frame = 100;
	uint32_t planeWidth = 2560, planeHeight = 1440, showReply = N48N::kSuccess;
	bool     displayTaken = false, displayElsewhere = false;

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
			out = N48N::Memory { h->nextPhysical, h->nextPhysical, false, visible, !visible, 0x9000000000ull + h->nextPhysical };
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
		b.submit = [](void *c, const N48N::Ib *ibs, uint32_t count, uint32_t sequence) -> uint32_t {
			auto *h = static_cast<HostBackend *>(c);
			if (h->submitReply != N48N::kSuccess)
				return h->submitReply;
			if (h->busyReplies) {
				h->busyReplies--;
				return N48N::kBusy;
			}
			memcpy(h->lastIbs, ibs, count * sizeof(*ibs));
			h->lastCount = count;
			h->lastSequence = sequence;
			h->pauses = 0;
			if (!h->finishAt)
				h->finishedSequence = sequence;
			return N48N::kSuccess;
		};
		b.finished = [](void *c) { return static_cast<HostBackend *>(c)->finishedSequence; };
		b.lost = [](void *c) { static_cast<HostBackend *>(c)->lostCalls++; };
		b.faulted = [](void *c) { return static_cast<HostBackend *>(c)->fault; };
		b.now = [](void *c) { return static_cast<HostBackend *>(c)->clockNs; };
		b.pause = [](void *c) {
			auto *h = static_cast<HostBackend *>(c);
			h->clockNs += 1000000;
			if (++h->pauses == h->finishAt)
				h->finishedSequence = h->lastSequence;
		};
		b.store = [](void *c, const N48N::Memory &m, uint64_t offset, uint64_t value) {
			memcpy(static_cast<uint8_t *>(static_cast<HostBackend *>(c)->bytesOf(m)) + offset, &value, sizeof(value));
		};
		b.scanQuery = [](void *c, n48n_scan_query &q) {
			auto *h = static_cast<HostBackend *>(c);
			q = n48n_scan_query {};
			q.h_active = q.plane_w = q.pitch_px = h->planeWidth;
			q.v_active = q.plane_h = h->planeHeight;
			q.h_total = 2720; q.v_total = 1481; q.refresh_mhz = 60000; q.pix_clk_khz = 241700;
			q.hubp_format = N48N_SCAN_FMT_ARGB8888;
			q.flags = N48N_SCANQ_LIT | N48N_SCANQ_GEOM_OK;
			q.frame_count = h->frame;
			q.console_mc = kConsole;
			q.plane_mc = q.earliest_mc = h->shown;
			return true;
		};
		b.scanAcquire = [](void *c) -> uint32_t {
			auto *h = static_cast<HostBackend *>(c);
			if (h->displayElsewhere)
				return N48N::kBusy;
			h->displayTaken = true;
			return N48N::kSuccess;
		};
		b.scanShow = [](void *c, uint64_t address, uint32_t width, uint32_t height, uint32_t pitchBytes, uint64_t &frame) -> uint32_t {
			auto *h = static_cast<HostBackend *>(c);
			if (h->showReply != N48N::kSuccess || width != h->planeWidth || height != h->planeHeight || pitchBytes != width * 4)
				return h->showReply != N48N::kSuccess ? h->showReply : N48N::kNotReady;
			h->shown = address;
			frame = ++h->frame;
			return N48N::kSuccess;
		};
		b.scanRelease = [](void *c) {
			auto *h = static_cast<HostBackend *>(c);
			h->shown = kConsole;
			h->displayTaken = false;
			return true;
		};
		return b;
	}
};

#endif /* RDNA4HostBackend_hpp */
