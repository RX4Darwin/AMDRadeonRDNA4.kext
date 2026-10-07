//
//  n48nkext.cpp
//  RDNA4FB
//
//  The Vulkan interface on the card (docs/vulkan-port.md, step 1): the user
//  client that carries the calls of vulkan/navi48_native_abi.h, and what
//  stands behind the client engine of n48n.cpp here, the runtime's own
//  pieces:
//
//    VRAM the CPU can reach   the runtime's pool heap, behind BAR0
//    VRAM it cannot           the device heap, past the BAR
//    system memory            wired pages, at the addresses the card reaches
//                             them by
//    the client's page table  4 KiB pages of the pool heap, written by the
//                             CPU through the BAR
//    its address space        number 8: table root, range and enable in that
//                             context's registers, a flush of its translation
//                             caches after every change of the table
//
//    its work                 the kernel's graphics ring (rdna4-gfx=2), which
//                             stays in address space 0: each command-buffer
//                             packet names the client's, and a last packet
//                             reports the submission's sequence
//
//    a display                the boot display's plane, flipped to one of
//                             the client's buffers with the runtime's flip
//                             and given back at the release or the close
//
//  One client at a time, root only. Its address space is the lowest of 8 to
//  15 that no client of the runtime holds (rdna4-vm), so both can be open in
//  one boot; not with rdna4-vmshared=2. What shows that the address space
//  works before anything is drawn in it is the copy test at the end
//  (include/rdna4vulkan.h).
//
//  Not run on the card.
//

#include "compute.hpp"
#include "flip.hpp"
#include "n48n.hpp"
#include "pipe.hpp"
#include "rdna4vulkan.h"
#include "userclient.hpp"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>

using namespace GfxReg;

#define VLOG(fmt, ...)  IOLog("RDNA4FB: vulkan: " fmt "\n", ## __VA_ARGS__)

namespace {

constexpr uint32_t kBuild = 1;             // what Hello and QueryInfo report as the kext's build
constexpr uint64_t kPage = GpuVm::kPageBytes;
constexpr uint32_t kBad = 0xffffffff;        // what RDNA4Compute::rd gives for a register it could not read
// The copy test's command page sits below every address a client may map at (N48N::kVaFirst).
// HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS and _HIGH (DMU segment 2), as src/flip.cpp has them.
constexpr uint32_t kHubpSurfaceAddress = 0x060a, kHubpSurfaceAddressHigh = 0x060b;
constexpr uint64_t kCopyTestVa = 0x1000;
static_assert(kCopyTestVa + kPage <= N48N::kVaFirst, "the copy test's page is outside the client's range");

struct Locked {
	IOLock *l;
	explicit Locked(IOLock *lock) : l(lock) { IOLockLock(l); }
	~Locked() { IOLockUnlock(l); }
};

// A buffer in system memory, and where the card finds each of its pages.
struct SystemMemory {
	IOBufferMemoryDescriptor *memory;
	IODMACommand             *dma;
	uint64_t                 *pageBus;
	uint64_t                  pages;
};

void release(SystemMemory *s) {
	if (s->dma) {
		(void)s->dma->clearMemoryDescriptor();
		s->dma->release();
	}
	if (s->pageBus)
		IOFree(s->pageBus, s->pages * sizeof(uint64_t));
	OSSafeReleaseNULL(s->memory);
	IOFree(s, sizeof(*s));
}

// The card's address of every page of `s->memory` (as rtAllocHost has it for its host buffers).
bool mapForCard(SystemMemory *s) {
	s->dma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 48, 0, IODMACommand::kMapped, 0, kPage);
	s->pageBus = static_cast<uint64_t *>(IOMalloc(s->pages * sizeof(uint64_t)));
	const uint64_t segmentBytes = s->pages * sizeof(IODMACommand::Segment64);
	auto *segments = static_cast<IODMACommand::Segment64 *>(IOMalloc(segmentBytes));
	UInt64 offset = 0;
	UInt32 count = static_cast<UInt32>(s->pages);
	uint64_t page = 0;
	bool ok = s->dma && s->pageBus && segments && s->dma->setMemoryDescriptor(s->memory) == kIOReturnSuccess &&
	          s->dma->gen64IOVMSegments(&offset, segments, &count) == kIOReturnSuccess;
	for (UInt32 i = 0; ok && i < count; i++) {
		const uint64_t address = segments[i].fIOVMAddr, length = segments[i].fLength;
		ok = length && !((address | length) & (kPage - 1)) && length / kPage <= s->pages - page;
		for (uint64_t at = 0; ok && at < length; at += kPage)
			s->pageBus[page++] = address + at;
	}
	if (segments)
		IOFree(segments, segmentBytes);
	return ok && page == s->pages;
}

} // namespace

struct RDNA4Compute::N48nState {
	const void  *owner;
	uint32_t     vmid;           // its address space: the lowest of 8 to 15 no client of the runtime holds (vmidUsed)
	uint64_t     copyTestPage;   // pool offset of the copy test's command page; 0 until the first test
	uint64_t     fencePage;      // pool offset of the dword the graphics queue reports finished work in
	uint32_t     ringDwords;     // what this client has put on the graphics ring since the ring last ran dry
	uint32_t     ringSequence;   // the last sequence it put there
	bool         scanTaken;      // the client has the display's plane; scanSurface is what it showed before
	Flip::Surface scanSurface;
	N48N::Client client;         // a few hundred KiB
};

// N48N::Backend over the runtime. Every function runs under rtLock, from a call of the client.
struct N48nBackend {
	static RDNA4Compute &of(void *context) { return *static_cast<RDNA4Compute *>(context); }

	// A pool offset as the GPU-physical address a table entry holds, and back.
	static uint64_t poolPhysical(const RDNA4Compute &c, uint64_t offset) {
		uint64_t physical = 0;
		(void)c.gpuPhysical(c.pool.mcAddress + offset, physical);
		return physical;
	}
	static uint8_t *poolBytes(const RDNA4Compute &c, uint64_t physical, uint64_t bytes) {
		const uint64_t offset = physical - poolPhysical(c, 0);
		return physical >= poolPhysical(c, 0) && offset >= c.heap.base() && bytes <= c.pool.size &&
		       offset <= c.pool.size - bytes ? c.poolCpu + offset : nullptr;
	}
	// ponytail: zeroed by the CPU through the uncached window, about a second for the whole pool; the copy
	// engine's fill if buffers this large turn out to be common.
	static void zero(const RDNA4Compute &c, uint64_t offset, uint64_t bytes) {
		for (uint64_t at = 0; at < bytes; at += 8)
			*reinterpret_cast<volatile uint64_t *>(c.poolCpu + offset + at) = 0;
	}

	// `align` is not looked at: the heaps align to their granule, and the GPU reaches a buffer through 4 KiB
	// table entries, to which the buffer's own alignment in VRAM means nothing.
	static bool allocVram(void *context, uint64_t bytes, uint64_t, bool highAllowed, N48N::Memory &out) {
		RDNA4Compute &c = of(context);
		uint64_t offset = 0, physical = 0;
		// The pool the CPU reaches is small: a buffer that does not need it goes past the BAR when it can.
		if (highAllowed && c.devHeap.size() && c.devHeap.alloc(bytes, offset)) {
			if (c.gpuPhysical(c.vramMc(offset), physical)) {
				out = N48N::Memory { offset, physical, false, false, true, c.vramMc(offset) };
				return true;
			}
			c.devHeap.free(offset);
		}
		if (!c.heap.alloc(bytes, offset))
			return false;
		zero(c, offset, bytes);
		out = N48N::Memory { offset, poolPhysical(c, offset), false, true, false, c.pool.mcAddress + offset };
		return true;
	}

	static bool allocSystem(void *, uint64_t bytes, N48N::Memory &out) {
		auto *s = static_cast<SystemMemory *>(IOMalloc(sizeof(SystemMemory)));
		if (!s)
			return false;
		*s = SystemMemory {};
		s->pages = bytes / kPage;
		s->memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, bytes, kPage);
		if (!s->memory || !mapForCard(s)) {
			release(s);
			return false;
		}
		bzero(s->memory->getBytesNoCopy(), bytes);   // it goes to a process: nothing of the kernel's in it
		out = N48N::Memory { reinterpret_cast<uint64_t>(s), 0, true, true, false, 0 };
		return true;
	}

	static void free(void *context, const N48N::Memory &m, uint64_t) {
		RDNA4Compute &c = of(context);
		if (m.system)
			release(reinterpret_cast<SystemMemory *>(m.token));
		else
			(m.high ? c.devHeap : c.heap).free(m.token);
	}

	static uint64_t systemPage(void *, const N48N::Memory &m, uint64_t page) {
		const auto *s = reinterpret_cast<const SystemMemory *>(m.token);
		return page < s->pages ? s->pageBus[page] : 0;
	}

	static bool tableAlloc(void *context, uint64_t &physical) {
		RDNA4Compute &c = of(context);
		uint64_t offset = 0;
		if (!c.heap.alloc(kPage, offset))
			return false;
		zero(c, offset, kPage);
		physical = poolPhysical(c, offset);
		return true;
	}
	static void tableFree(void *context, uint64_t physical) {
		RDNA4Compute &c = of(context);
		c.heap.free(physical - poolPhysical(c, 0));
	}
	// ponytail: the table is read and written in place through the uncached window (about a microsecond an
	// entry, so tens of milliseconds to map 64 MiB); a copy in host memory if mapping shows up as slow.
	static uint64_t *tableEntries(void *context, uint64_t physical) {
		return reinterpret_cast<uint64_t *>(poolBytes(of(context), physical, kPage));
	}

	// The card sees what the CPU wrote through the BAR, then forgets what it had cached of this address space.
	static void flush(void *context) {
		RDNA4Compute &c = of(context);
		c.flushHdp();
		(void)c.vmInvalidate(c.n48n->vmid, "vulkan client");
	}

	static bool readRegs(void *context, uint32_t dword, uint32_t count, uint32_t *out) {
		bool ok = true;
		for (uint32_t i = 0; i < count; i++) {
			out[i] = of(context).rdGc(Reg { 0, static_cast<uint16_t>(GbAddrConfig.dword + (dword - 0x263e) + i) });
			ok &= out[i] != kBad;
		}
		return ok;
	}

	static void pools(void *context, N48N::Pools &out) {
		const RDNA4Compute &c = of(context);
		out = N48N::Pools { c.heap.size(), c.heap.freeBytes(), c.devHeap.size(), c.devHeap.freeBytes() };
	}

	// Work on the kernel's graphics ring, as the runtime puts its own clients' there (gfxClientEmit;
	// docs/w12k-gfx-submit.md has each packet against what Linux emits): the ring stays in address space 0, each
	// command-buffer packet names the client's, and a last packet reports the sequence to the fence dword.
	static uint32_t submit(void *context, const N48N::Ib *ibs, uint32_t count, uint32_t sequence) {
		RDNA4Compute &c = of(context);
		RDNA4Compute::N48nState &s = *c.n48n;
		const IOReturn ready = c.gfxClientReady();
		if (ready != kIOReturnSuccess)   // no graphics ring this boot (rdna4-gfx=2), not up yet, or halted after lost work
			return ready == kIOReturnNotResponding ? N48N::kAborted : static_cast<uint32_t>(ready);
		// ponytail: room on the ring is only counted back when all of this client's work has finished; per
		// submission if a client ever keeps half the ring busy.
		if (*c.poolDw(static_cast<uint32_t>(s.fencePage)) == s.ringSequence)
			s.ringDwords = 0;
		const uint32_t need = 3 + 4 * count + 8;
		if (s.ringDwords + need > c.gfxRing.sizeDwords() / 2)
			return N48N::kBusy;
		c.grbmSelect(0, 0, 0, s.vmid);     // the address space's shader memory setup, as gfx_v12_0_init_compute_vmid has it
		c.wr(IpDiscovery::HwGc, ShMemConfig, kShMemConfigDefault);
		c.wr(IpDiscovery::HwGc, ShMemBases, kShMemBasesDefault);
		c.grbmSelect(0, 0, 0, 0);
		uint32_t pkt[8];
		// Load enable alone and no shadow word: what Navi48-MacOS puts on its ring for this driver, and Linux's
		// third dword (Linux adds load bits for a context switch, 0x81018003; the driver's own command buffers
		// start with their own CONTEXT_CONTROL).
		bool ok = c.gfxRing.emit(pkt, Pm4::contextControl(pkt, 0x80000000u, 0));
		for (uint32_t i = 0; i < count; i++)
			ok = ok && c.gfxRing.emit(pkt, Pm4::indirectBufferGfx(pkt, ibs[i].va, ibs[i].dwords, s.vmid));
		ok = ok && c.gfxRing.emit(pkt, Pm4::releaseMem(pkt, c.poolMc(static_cast<uint32_t>(s.fencePage)), sequence));
		if (!ok)
			return N48N::kNoResources;
		c.flushHdp();                      // what the CPU wrote through the BAR, command buffers included
		c.gfxKick(c.gfxRing.wptr());
		s.ringDwords += need;
		s.ringSequence = sequence;
		return N48N::kSuccess;
	}
	static uint32_t finished(void *context) {
		const RDNA4Compute &c = of(context);
		return *c.poolDw(static_cast<uint32_t>(c.n48n->fencePage));
	}
	static void lost(void *context) {
		VLOG("work on the graphics queue did not finish in %llu s", N48N::kLostAfterNs / 1000000000ull);
		of(context).gfxClientWedge("vulkan client");
	}
	static uint64_t now(void *) {
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		return ns;
	}
	// ponytail: rtLock stays held while a client waits, as in the runtime's own waits, so a second thread's call
	// waits behind it (at most N48N_WAIT_CAP_NS at a time); drop the lock around the sleep if that shows.
	static void pause(void *) { IOSleep(1); }
	static void store(void *context, const N48N::Memory &m, uint64_t offset, uint64_t value) {
		if (m.system) {
			auto *s = reinterpret_cast<SystemMemory *>(m.token);
			memcpy(static_cast<uint8_t *>(s->memory->getBytesNoCopy()) + offset, &value, sizeof(value));
		} else if (m.cpuVisible) {
			*reinterpret_cast<volatile uint64_t *>(of(context).poolCpu + m.token + offset) = value;
		}
	}

	// The display a client may take: the lowest-numbered pipe that is running, which is the boot display. Its
	// plane is flipped with the runtime's own flip (src/flip.cpp): the address written under the pipe's update
	// lock, the latch and the next frame waited for. macOS goes on drawing its desktop into the console's
	// memory, unseen, and gets the plane back at the release.
	static uint32_t otgReg(const RDNA4Compute &c, const Flip::Surface &s, uint32_t dword) {
		return c.rd(IpDiscovery::HwDmu, Reg { 2, dword + s.otg * Pipe::Reg::kOtgStride });
	}
	static bool scanQuery(void *context, n48n_scan_query &q) {
		RDNA4Compute &c = of(context);
		Flip::Surface s {};
		if (!Flip::findPipe(c, s, false))
			return false;
		const uint32_t hTotal = otgReg(c, s, Pipe::Reg::kOtgHTotal), vTotal = otgReg(c, s, Pipe::Reg::kOtgVTotal);
		const uint32_t hBlank = otgReg(c, s, Pipe::Reg::kOtgHBlank), vBlank = otgReg(c, s, Pipe::Reg::kOtgVBlank);
		q = n48n_scan_query {};
		q.h_total = hTotal == kBad ? 0 : (hTotal & 0x7fff) + 1;
		q.v_total = vTotal == kBad ? 0 : (vTotal & 0x7fff) + 1;
		// Blank START [14:0] is where the active picture ends, END [30:16] where it begins.
		q.h_active = hBlank == kBad ? s.width : (hBlank & 0x7fff) - ((hBlank >> 16) & 0x7fff);
		q.v_active = vBlank == kBad ? s.height : (vBlank & 0x7fff) - ((vBlank >> 16) & 0x7fff);
		q.pitch_px = s.pitch;
		// The format is not read: every plane the plugin or the firmware sets up is linear 8:8:8:8 without DCC.
		q.hubp_format = N48N_SCAN_FMT_ARGB8888;
		q.otg = s.otg;
		q.flags = N48N_SCANQ_LIT | N48N_SCANQ_GEOM_OK;    // the refresh rate is left 0: not derived here
		q.frame_count = otgReg(c, s, Pipe::Reg::kOtgFrameCount) & 0xffffff;
		q.console_mc = c.n48n && c.n48n->scanTaken ? c.n48n->scanSurface.desktop : s.desktop;
		q.plane_mc = q.earliest_mc = s.desktop;           // a flip is waited for, so the two are the same between calls
		q.plane_w = s.width;
		q.plane_h = s.height;
		return true;
	}
	static uint32_t scanAcquire(void *context) {
		RDNA4Compute &c = of(context);
		RDNA4Compute::N48nState &n = *c.n48n;
		if (c.presentActive)                              // the runtime's own Present has the plane
			return N48N::kBusy;
		if (!Flip::findPipe(c, n.scanSurface))
			return N48N::kNotReady;
		n.scanTaken = true;
		VLOG("display taken: OTG%u HUBP%u, %ux%u pitch %u, the desktop at 0x%llx", n.scanSurface.otg, n.scanSurface.hubp,
		     n.scanSurface.width, n.scanSurface.height, n.scanSurface.pitch, n.scanSurface.desktop);
		return N48N::kSuccess;
	}
	static uint32_t scanShow(void *context, uint64_t address, uint32_t width, uint32_t height, uint32_t pitchBytes,
	                         uint64_t &frame) {
		RDNA4Compute &c = of(context);
		const RDNA4Compute::N48nState &n = *c.n48n;
		Flip::Surface s {};
		// The plane has to be what it was when the client took it: macOS may have changed the mode since.
		if (!n.scanTaken || !Flip::findPipe(c, s, false) || s.otg != n.scanSurface.otg || s.hubp != n.scanSurface.hubp ||
		    s.width != width || s.height != height || s.pitch * 4 != pitchBytes)
			return N48N::kNotReady;
		if (!Flip::flipTo(c, s, address, "vulkan present", nullptr, true))
			return N48N::kNotResponding;
		frame = otgReg(c, s, Pipe::Reg::kOtgFrameCount) & 0xffffff;
		return N48N::kSuccess;
	}
	static bool scanRelease(void *context) {
		RDNA4Compute &c = of(context);
		RDNA4Compute::N48nState &n = *c.n48n;
		if (!n.scanTaken)
			return true;
		n.scanTaken = false;
		Flip::Surface s {};
		if (!Flip::findPipe(c, s, false))
			return false;
		// The plane goes back to the desktop's memory (it is there already if nothing was ever shown).
		bool back = s.desktop == n.scanSurface.desktop || Flip::flipTo(c, s, n.scanSurface.desktop, "vulkan release");
		if (!back) {
			// The desktop must not stay hidden behind a flip that would not confirm: the address alone, without
			// the lock and the checks, which the pipe takes at its next frame unless something holds its lock.
			const uint32_t hubp = s.hubp * Pipe::Reg::kHubpStride;
			c.wr(IpDiscovery::HwDmu, Reg { 2, kHubpSurfaceAddressHigh + hubp }, static_cast<uint32_t>(n.scanSurface.desktop >> 32));
			c.wr(IpDiscovery::HwDmu, Reg { 2, kHubpSurfaceAddress + hubp }, static_cast<uint32_t>(n.scanSurface.desktop));
			IOSleep(50);
			Flip::Surface now {};
			back = Flip::findPipe(c, now, false) && now.desktop == n.scanSurface.desktop;
		}
		VLOG("display given back: the plane shows 0x%llx%s", n.scanSurface.desktop, back ? "" : " NOT: the register did not take it");
		return back;
	}

	static N48N::Backend make(RDNA4Compute *c) {
		N48N::Backend b {};
		b.context = c;
		b.allocVram = allocVram;
		b.allocSystem = allocSystem;
		b.free = free;
		b.systemPage = systemPage;
		b.tables = VmTree::Pages { c, tableAlloc, tableFree, tableEntries };
		b.flush = flush;
		b.readRegs = readRegs;
		b.pools = pools;
		b.submit = submit;
		b.finished = finished;
		b.lost = lost;
		b.now = now;
		b.pause = pause;
		b.store = store;
		b.scanQuery = scanQuery;
		b.scanAcquire = scanAcquire;
		b.scanShow = scanShow;
		b.scanRelease = scanRelease;
		return b;
	}

	static bool readEntry(void *context, uint64_t address, uint64_t &entry) {
		const uint8_t *p = poolBytes(of(context), address, sizeof(entry));
		if (p)
			entry = *reinterpret_cast<const volatile uint64_t *>(p);
		return p != nullptr;
	}
};

IOReturn RDNA4Compute::n48nOpen(const void *owner) {
	if (!rtLock)
		return kIOReturnNotReady;
	Locked g(rtLock);
	// System memory needs the card to reach host pages, which the DMA setup established.
	if (!rtReady || powerSleeping || rtWedged || !poolCpu || !heap.size() || !dmaReady || !busMasterSet)
		return kIOReturnNotReady;
	if (n48n)
		return kIOReturnExclusiveAccess;
	// ponytail: with rdna4-vmshared=2 the runtime hands address spaces out per job from a pool of its own (vmPool), which this does not
	// know; the interface stays shut there. Take one from that pool when both are wanted.
	if (vmShared == 2) {
		VLOG("not with rdna4-vmshared=2: its address spaces are handed out per job");
		return kIOReturnNotReady;
	}
	// An address space no client of the runtime holds, picked as rtOpenShared picks: 8 when none is open.
	uint32_t vmid = 8;
	for (; vmid <= 15 && vmidUsed[vmid]; vmid++) {}
	if (vmid > 15) {
		VLOG("open failed: all of the address spaces 8 to 15 are taken");
		return kIOReturnNoResources;
	}
	auto *s = static_cast<N48nState *>(IOMalloc(sizeof(N48nState)));
	if (!s)
		return kIOReturnNoMemory;
	bzero(s, sizeof(*s));
	s->owner = owner;
	s->vmid = vmid;
	n48n = s;                          // the backend reads the address space from it while the client opens
	RtClient context {};
	context.vmid = vmid;
	const uint32_t gc = (static_cast<uint32_t>(sv.gcMajor) << 16) | (static_cast<uint32_t>(sv.gcMinor) << 8) | sv.gcRev;
	if (!heap.alloc(kPage, s->fencePage) || !s->client.open(N48nBackend::make(this), vmid, kBuild, gc) ||
	    !(context.rootPhys = s->client.root()) || !vmContextInit(context)) {
		VLOG("open failed: %s", s->client.root() ? "the address space's registers did not take" : "no page for the table");
		s->client.close();
		if (s->fencePage)
			heap.free(s->fencePage);
		IOFree(s, sizeof(*s));
		n48n = nullptr;
		return kIOReturnNoMemory;
	}
	N48nBackend::zero(*this, s->fencePage, kPage);
	N48nBackend::flush(this);
	vmidUsed[vmid] = true;
	VLOG("client open: address space %u, table root at physical 0x%llx; %llu MiB of VRAM behind the BAR, %llu MiB past it",
	     vmid, s->client.root(), heap.size() >> 20, devHeap.size() >> 20);
	return kIOReturnSuccess;
}

void RDNA4Compute::n48nClose(const void *owner) {
	if (!rtLock)
		return;
	Locked g(rtLock);
	if (!n48n || n48n->owner != owner)
		return;
	if (powerSleeping) {
		n48nDiscard();     // the card is off: nothing to write to
		return;
	}
	if (!n48n->client.quiesce())       // its work first: that still needs the address space
		VLOG("client closes with work on the graphics queue that did not finish");
	wr(IpDiscovery::HwGc, Reg { 0, static_cast<uint16_t>(GcCtx1Cntl.dword + n48n->vmid - 1) }, 0);   // the context goes off before its table does
	vmidUsed[n48n->vmid] = false;
	if (n48n->copyTestPage)
		heap.free(n48n->copyTestPage);
	n48n->client.close();
	heap.free(n48n->fencePage);
	IOFree(n48n, sizeof(N48nState));
	n48n = nullptr;
	VLOG("client closed: %llu MiB free behind the BAR, %llu MiB past it", heap.freeBytes() >> 20, devHeap.freeBytes() >> 20);
}

// Called with rtLock held, by the wake's cleanup before it makes the heaps anew: the VRAM and the table went with
// the card's power. Only the system memory is still someone's.
void RDNA4Compute::n48nDiscard() {
	if (!n48n)
		return;
	for (uint32_t h = 1; h < N48N_MAX_BOS; h++) {
		const N48N::Buffer *b = n48n->client.buffer(h);
		if (b && b->memory.system)
			N48nBackend::free(this, b->memory, b->bytes);
	}
	vmidUsed[n48n->vmid] = false;
	IOFree(n48n, sizeof(N48nState));
	n48n = nullptr;
	VLOG("client dropped: the card lost its memory; its calls answer \"not ready\" from here on");
}

IOReturn RDNA4Compute::n48nCall(const void *owner, uint32_t selector, IOExternalMethodArguments *a) {
	Locked g(rtLock);
	IOReturn r;
	if (!n48n || n48n->owner != owner || !rtReady || powerSleeping) {
		r = kIOReturnNotReady;
	} else if (a->structureInputDescriptor || a->structureOutputDescriptor) {
		r = kIOReturnBadArgument;          // nothing of this interface is large enough to arrive that way
	} else if (selector == RDNA4_VULKAN_SEL_COPY_TEST) {
		r = a->scalarInputCount == 3 && !a->scalarOutputCount && !a->structureInputSize && !a->structureOutputSize
			? n48nCopyTest(a->scalarInput[0], a->scalarInput[1], a->scalarInput[2]) : kIOReturnBadArgument;
	} else {
		uint32_t scalars = a->scalarOutputCount;
		size_t bytes = a->structureOutputSize;
		r = static_cast<IOReturn>(n48n->client.call(selector, a->scalarInput, a->scalarInputCount, a->structureInput, a->structureInputSize,
		                                            a->scalarOutput, &scalars, a->structureOutput, &bytes));
	}
	if (r != kIOReturnSuccess)
		a->scalarOutputCount = a->structureOutputSize = 0;
	return r;
}

// What IOConnectMapMemory64 maps for a buffer's handle. A process keeps such a mapping for as long as it likes,
// also after it freed the buffer: one reason this interface is root only for now.
IOReturn RDNA4Compute::n48nMemory(const void *owner, uint32_t handle, IOMemoryDescriptor **memory) {
	Locked g(rtLock);
	if (!n48n || n48n->owner != owner || !rtReady || powerSleeping)
		return kIOReturnNotReady;
	const N48N::Buffer *b = n48n->client.buffer(handle);
	if (!b)
		return kIOReturnNotFound;
	if (!b->memory.cpuVisible)
		return kIOReturnNotPermitted;
	if (b->memory.system) {
		*memory = reinterpret_cast<SystemMemory *>(b->memory.token)->memory;
		(*memory)->retain();
		return kIOReturnSuccess;
	}
	IODeviceMemory *bar0 = env.pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	*memory = bar0 ? IODeviceMemory::withSubRange(bar0, pool.offset + b->memory.token, b->bytes) : nullptr;
	return *memory ? kIOReturnSuccess : kIOReturnNoMemory;
}

// RDNA4_VULKAN_SEL_COPY_TEST. The kernel's copy queue stays in address space 0; the packet that starts a command
// buffer names the address space the buffer and its addresses are read in, which is how Linux runs a process's
// copy work (sdma_v7_0_ring_emit_ib). The command page is the kext's, mapped below the client's range.
IOReturn RDNA4Compute::n48nCopyTest(uint64_t source, uint64_t destination, uint64_t bytes) {
	if (!bytes || bytes > RDNA4_VULKAN_COPY_TEST_MAX || ((source | destination | bytes) & (kPage - 1)))
		return kIOReturnBadArgument;
	// An address that is not mapped would stop the copy engine for the rest of the boot: look first.
	const uint64_t root = n48n->client.root(), low48 = (1ull << N48N::kVaBits) - 1;
	for (uint64_t at = 0; at < bytes; at += kPage) {
		uint64_t physical = 0, from = 0, to = 0;
		if (!GpuVm::walk(root, (source + at) & low48, N48nBackend::readEntry, this, physical, from) ||
		    !GpuVm::walk(root, (destination + at) & low48, N48nBackend::readEntry, this, physical, to) ||
		    !(from & GpuVm::kReadable) || !(to & GpuVm::kWritable))
			return kIOReturnBadArgument;
	}
	const N48N::Backend be = N48nBackend::make(this);
	if (!n48n->copyTestPage) {
		uint64_t offset = 0;
		if (!heap.alloc(kPage, offset))
			return kIOReturnNoMemory;
		if (!VmTree::map(be.tables, root, kCopyTestVa, N48nBackend::poolPhysical(*this, offset), 1,
		                 GpuVm::kValid | GpuVm::kReadable | GpuVm::kWritable | GpuVm::kExecutable)) {
			heap.free(offset);
			return kIOReturnNoMemory;
		}
		n48n->copyTestPage = offset;
	}
	uint32_t copy[Sdma::kCopyDwords];
	Sdma::copyLinear(copy, source, destination, static_cast<uint32_t>(bytes));
	for (uint32_t i = 0; i < Sdma::kCopyDwords; i++)
		*poolDw(static_cast<uint32_t>(n48n->copyTestPage) + 4 * i) = copy[i];
	N48nBackend::flush(this);

	uint32_t ring[7 + Sdma::kIndirectDwords] {};
	uint32_t n = Sdma::indirectPad(sdmaRing.wptr());
	n += Sdma::indirect(ring + n, kCopyTestVa, Sdma::kCopyDwords, n48n->vmid);
	if (!sdmaRun(ring, n, 2000)) {
		VLOG("copy test: the copy engine did not finish 0x%llx -> 0x%llx (%llu bytes) in address space %u", source,
		     destination, bytes, n48n->vmid);
		return kIOReturnNotResponding;
	}
	VLOG("copy test: 0x%llx -> 0x%llx, %llu bytes, in address space %u", source, destination, bytes, n48n->vmid);
	return kIOReturnSuccess;
}

// ---- the user client ---------------------------------------------------------------------------------------

class RDNA4VulkanClient : public IOUserClient {
	OSDeclareDefaultStructors(RDNA4VulkanClient)
public:
	RDNA4Compute *compute;
	IOReturn      opened;        // what the open came to, for newUserClient to hand to the caller

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	                        OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RDNA4VulkanClient, IOUserClient)

bool RDNA4VulkanClient::start(IOService *provider) {
	auto *svc = OSDynamicCast(RDNA4ComputeService, provider);
	if (!svc || !svc->compute || !IOUserClient::start(provider)) {
		opened = kIOReturnNotReady;
		return false;
	}
	opened = svc->compute->n48nOpen(this);
	if (opened == kIOReturnSuccess)
		compute = svc->compute;
	return opened == kIOReturnSuccess;
}

// Also what IOUserClient::clientDied comes to, when the process goes without closing.
IOReturn RDNA4VulkanClient::clientClose() {
	if (compute)
		compute->n48nClose(this);
	compute = nullptr;
	terminate();
	return kIOReturnSuccess;
}

// No dispatch table: the engine checks each call's counts and sizes itself, exactly, as the interface states them.
IOReturn RDNA4VulkanClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                           IOExternalMethodDispatch *, OSObject *, void *) {
	return compute ? compute->n48nCall(this, selector, args) : kIOReturnNotReady;
}

IOReturn RDNA4VulkanClient::clientMemoryForType(UInt32 type, IOOptionBits *, IOMemoryDescriptor **memory) {
	return compute ? compute->n48nMemory(this, type, memory) : kIOReturnNotReady;
}

IOReturn RDNA4ComputeService::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                                            IOUserClient **handler) {
	if (type != N48N_UC_TYPE)
		return IOService::newUserClient(owningTask, securityID, type, properties, handler);
	// Root only until there is a policy for who may have the GPU (docs/vulkan-port.md, "What a client may reach").
	if (IOUserClient::clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	auto *client = OSTypeAlloc(RDNA4VulkanClient);
	if (!client)
		return kIOReturnNoMemory;
	if (!client->initWithTask(owningTask, securityID, type, properties) || !client->attach(this)) {
		client->release();
		return kIOReturnNoResources;
	}
	if (!client->start(this)) {
		const IOReturn why = client->opened;
		client->detach(this);
		client->release();
		return why != kIOReturnSuccess ? why : kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}
