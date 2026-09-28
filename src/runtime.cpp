//
//  runtime.cpp
//  RDNA4FB
//
//  The compute runtime user space reaches through RDNA4ComputeClient
//  (userclient.cpp, include/rdna4compute.h): VRAM buffers from a heap in
//  the compute pool, code objects loaded whole, and synchronous dispatches
//  on the stage-5 MEC queue through launch(). One lock serialises it all —
//  the queue has one ring and one kernarg slot.
//
//  A dispatch whose fence never comes leaves the queue in an unknown state
//  with no reset to recover it, so the runtime then refuses further
//  dispatches (RDNA4_FLAG_WEDGED) until the next boot.
//

#include "compute.hpp"
#include "userclient.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>

#define RLOG(fmt, ...)  IOLog("RDNA4FB: runtime: " fmt "\n", ## __VA_ARGS__)

namespace {

// Handles: (generation << 16) | (slot + 1), so 0 is never one and a stale
// handle of a reused slot does not match.
uint64_t makeHandle(uint32_t slot, uint16_t gen) {
	return (static_cast<uint64_t>(gen) << 16) | (slot + 1);
}

bool slotOf(uint64_t h, uint32_t max, uint32_t &slot, uint16_t &gen) {
	const uint32_t s = static_cast<uint32_t>(h & 0xffff);
	if ((h >> 32) || !s || s > max)
		return false;
	slot = s - 1;
	gen = static_cast<uint16_t>(h >> 16);
	return true;
}

uint16_t nextGen(uint16_t g) { return static_cast<uint16_t>(g == 0xffff ? 1 : g + 1); }

constexpr uint64_t kCopyChunk = 4ull << 20;   // wired at a time
constexpr uint32_t kCodePad = 0x100;          // zeroed past the image: instruction prefetch

struct Locked {
	IOLock *l;
	explicit Locked(IOLock *lock) : l(lock) { IOLockLock(l); }
	~Locked() { IOLockUnlock(l); }
};

// Copy between a user range and kernel memory, `length` bytes.
IOReturn userCopy(task_t task, mach_vm_address_t user, void *kernel, uint64_t length, bool fromUser) {
	for (uint64_t done = 0; done < length;) {
		const uint64_t n = length - done < kCopyChunk ? length - done : kCopyChunk;
		IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
			user + done, n, fromUser ? kIODirectionOut : kIODirectionIn, task);
		if (!md)
			return kIOReturnNoMemory;
		IOReturn r = md->prepare();
		if (r == kIOReturnSuccess) {
			uint8_t *k = static_cast<uint8_t *>(kernel) + done;
			const IOByteCount c = fromUser ? md->readBytes(0, k, n) : md->writeBytes(0, k, n);
			if (c != n)
				r = kIOReturnVMError;
			md->complete();
		}
		md->release();
		if (r != kIOReturnSuccess)
			return r;
		done += n;
	}
	return kIOReturnSuccess;
}

} // namespace

void RDNA4Compute::publishRuntime(uint32_t stage) {
	if (!rtLock || !poolCpu || pool.size <= kHeapOffset) {
		RLOG("not published: %s", !rtLock ? "no lock" : "no room for a heap in the pool");
		return;
	}
	IOLockLock(rtLock);
	heap.init(kHeapOffset, pool.size - kHeapOffset);
	rtStage = stage;
	rtReady = true;
	IOLockUnlock(rtLock);

	auto *svc = OSTypeAlloc(RDNA4ComputeService);
	if (!svc || !svc->init()) {
		OSSafeReleaseNULL(svc);
		RLOG("not published: could not create the service");
		return;
	}
	svc->compute = this;
	svc->setProperty("IOUserClientClass", "RDNA4ComputeClient");
	svc->setProperty("ABI", static_cast<uint64_t>(RDNA4_COMPUTE_ABI), 32);
	svc->setProperty("HeapBytes", heap.size(), 64);
	if (!svc->attach(env.pci)) {
		svc->release();
		RLOG("not published: could not attach to the GPU");
		return;
	}
	svc->registerService();
	rtService = svc;             // the registry keeps it
	svc->release();
	RLOG("user-space runtime up: %s, heap %llu MiB at MC 0x%llx", RDNA4_COMPUTE_SERVICE,
	     heap.size() >> 20, poolMc(kHeapOffset));
}

RDNA4Compute::RtBuffer *RDNA4Compute::bufferFor(const void *owner, uint64_t handle) {
	uint32_t slot;
	uint16_t gen;
	if (!owner || !slotOf(handle, kMaxBuffers, slot, gen))
		return nullptr;
	RtBuffer &b = buffers[slot];
	return b.owner == owner && b.gen == gen ? &b : nullptr;
}

RDNA4Compute::RtProgram *RDNA4Compute::programFor(const void *owner, uint64_t handle) {
	uint32_t slot;
	uint16_t gen;
	if (!owner || !slotOf(handle, kMaxPrograms, slot, gen))
		return nullptr;
	RtProgram &p = programs[slot];
	return p.owner == owner && p.gen == gen ? &p : nullptr;
}

IOReturn RDNA4Compute::rtInfo(uint64_t out[6]) {
	Locked g(rtLock);
	out[0] = RDNA4_COMPUTE_ABI;
	out[1] = rtStage;
	out[2] = (rtReady ? RDNA4_FLAG_READY : 0) | (rtWedged ? RDNA4_FLAG_WEDGED : 0);
	out[3] = heap.size();
	out[4] = heap.freeBytes();
	out[5] = poolMc(kHeapOffset);
	return kIOReturnSuccess;
}

// Contents are undefined, as with any GPU allocation; callers write first.
IOReturn RDNA4Compute::rtAlloc(const void *owner, uint64_t bytes, uint64_t &handle, uint64_t &gpu) {
	Locked g(rtLock);
	if (!rtReady)
		return kIOReturnNotReady;
	if (!bytes || bytes > heap.size())
		return kIOReturnBadArgument;
	uint32_t slot = 0;
	while (slot < kMaxBuffers && buffers[slot].owner)
		slot++;
	if (slot == kMaxBuffers)
		return kIOReturnNoResources;
	uint64_t off;
	if (!heap.alloc(bytes, off))
		return kIOReturnNoMemory;
	RtBuffer &b = buffers[slot];
	b = { owner, off, bytes, nextGen(b.gen) };
	handle = makeHandle(slot, b.gen);
	gpu = poolMc(off);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtFree(const void *owner, uint64_t handle) {
	Locked g(rtLock);
	RtBuffer *b = bufferFor(owner, handle);
	if (!b)
		return kIOReturnBadArgument;
	heap.free(b->offset);
	b->owner = nullptr;
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtCopy(const void *owner, uint64_t handle, uint64_t offset, task_t task,
                              mach_vm_address_t user, uint64_t length, bool toGpu) {
	Locked g(rtLock);
	if (!rtReady)
		return kIOReturnNotReady;
	RtBuffer *b = bufferFor(owner, handle);
	if (!b || offset > b->bytes || length > b->bytes - offset)
		return kIOReturnBadArgument;
	if (!length)
		return kIOReturnSuccess;
	IOReturn r = userCopy(task, user, poolCpu + b->offset + offset, length, toGpu);
	if (toGpu)
		flushHdp();
	return r;
}

IOReturn RDNA4Compute::rtLoad(const void *owner, task_t task, mach_vm_address_t elf, uint64_t length,
                              const char *name, uint64_t out[8]) {
	Locked g(rtLock);
	if (!rtReady)
		return kIOReturnNotReady;
	if (!length || length > RDNA4_MAX_CODE_OBJECT)
		return kIOReturnBadArgument;
	uint32_t slot = 0;
	while (slot < kMaxPrograms && programs[slot].owner)
		slot++;
	if (slot == kMaxPrograms)
		return kIOReturnNoResources;

	auto *file = static_cast<uint8_t *>(IOMalloc(length));
	if (!file)
		return kIOReturnNoMemory;
	IOReturn r = userCopy(task, elf, file, length, true);
	CodeObj::Kernel k {};
	CodeObj::Image img {};
	const char *why = nullptr;
	uint64_t off = 0;
	if (r != kIOReturnSuccess) {
		why = "could not read the file from the caller";
	} else if (!CodeObj::findKernel(file, length, name, k, &why) ||
	           !CodeObj::parseImage(file, length, img, &why)) {
		r = kIOReturnNotFound;
	} else if (!kernelFits(k, &why)) {
		r = kIOReturnUnsupported;
	} else if ((k.entryVa & 0xff) || k.entryVa >= img.size) {
		why = "its code is not 256-byte aligned inside the image";
		r = kIOReturnUnsupported;
	} else if (!heap.alloc(img.size + kCodePad, off)) {
		why = "no room in the heap";
		r = kIOReturnNoMemory;
	} else {
		uint8_t *dst = poolCpu + off;
		bzero(dst, static_cast<size_t>(img.size + kCodePad));
		for (uint32_t i = 0; i < img.count; i++)
			memcpy(dst + img.seg[i].vaddr, file + img.seg[i].fileOffset,
			       static_cast<size_t>(img.seg[i].fileSize));
		flushHdp();
	}
	IOFree(file, length);
	if (r != kIOReturnSuccess) {
		RLOG("load of \"%s\" refused: %s", name, why ? why : "?");
		return r;
	}

	RtProgram &p = programs[slot];
	p = { owner, off, k, nextGen(p.gen) };
	out[0] = makeHandle(slot, p.gen);
	out[1] = k.kernargSize;
	out[2] = img.size;
	out[3] = k.rsrc1;
	out[4] = k.rsrc2;
	out[5] = k.rsrc3;
	out[6] = k.properties;
	out[7] = k.groupSegmentSize;
	RLOG("loaded \"%s\": %llu-byte image at MC 0x%llx, entry +0x%llx, %u bytes of kernargs, "
	     "%u of LDS", name, img.size, poolMc(off), k.entryVa, k.kernargSize, k.groupSegmentSize);
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtUnload(const void *owner, uint64_t program) {
	Locked g(rtLock);
	RtProgram *p = programFor(owner, program);
	if (!p)
		return kIOReturnBadArgument;
	heap.free(p->offset);
	p->owner = nullptr;
	return kIOReturnSuccess;
}

IOReturn RDNA4Compute::rtDispatch(const void *owner, const RDNA4Dispatch &d, uint64_t &micros) {
	Locked g(rtLock);
	if (!rtReady)
		return kIOReturnNotReady;
	if (rtWedged)
		return kIOReturnNotResponding;
	RtProgram *p = programFor(owner, d.program);
	if (!p || d.kernargBytes > RDNA4_MAX_KERNARG || d.timeoutMs > RDNA4_MAX_TIMEOUT_MS ||
	    d.dynamicLdsBytes > RDNA4_MAX_LDS ||
	    p->k.groupSegmentSize + d.dynamicLdsBytes > RDNA4_MAX_LDS)
		return kIOReturnBadArgument;
	uint64_t items = 1;
	for (int i = 0; i < 3; i++) {
		if (!d.groups[i] || !d.groupSize[i] ||
		    static_cast<uint64_t>(d.groups[i]) * d.groupSize[i] > 0xffffffffull)
			return kIOReturnBadArgument;
		items *= d.groupSize[i];
	}
	if (items > 1024)
		return kIOReturnBadArgument;

	// The kernarg block: the caller's bytes, zero up to the kernel's size
	// (hidden arguments the caller did not fill).
	const CodeObj::Kernel &k = p->k;
	uint32_t bytes = k.kernargSize > d.kernargBytes ? k.kernargSize : d.kernargBytes;
	bytes = (bytes + 3) & ~3u;
	for (uint32_t off = 0; off < bytes; off += 4)
		*poolDw(kKernargOffset + off) = 0;
	if (d.kernargBytes)
		memcpy(poolCpu + kKernargOffset, d.kernargs, d.kernargBytes);

	const uint64_t kernarg = poolMc(kKernargOffset);
	const uint32_t user[2] = { static_cast<uint32_t>(kernarg), static_cast<uint32_t>(kernarg >> 32) };
	Launch l {};
	l.code = poolMc(p->offset) + k.entryVa;
	l.rsrc1 = k.rsrc1;
	l.rsrc2 = k.rsrc2;
	l.rsrc3 = k.rsrc3;
	l.user = user;
	l.userCount = k.userSgprCount();
	for (int i = 0; i < 3; i++) {
		l.groups[i] = d.groups[i];
		l.groupSize[i] = d.groupSize[i];
	}
	l.wave32 = k.wave32();
	l.timeoutUs = (d.timeoutMs ? d.timeoutMs : 1000) * 1000;
	l.ldsBytes = k.groupSegmentSize + d.dynamicLdsBytes;

	uint64_t ns = 0;
	const bool done = launch(l, "runtime", ns);
	micros = ns / 1000;
	if (!done) {
		rtWedged = true;
		RLOG("dispatch timed out after %u ms: the queue is considered hung; no more dispatches "
		     "until reboot", l.timeoutUs / 1000);
		return kIOReturnTimeout;
	}
	return kIOReturnSuccess;
}

void RDNA4Compute::rtRelease(const void *owner) {
	if (!rtLock || !owner)
		return;
	Locked g(rtLock);
	uint32_t nb = 0, np = 0;
	for (RtBuffer &b : buffers) {
		if (b.owner == owner) {
			heap.free(b.offset);
			b.owner = nullptr;
			nb++;
		}
	}
	for (RtProgram &p : programs) {
		if (p.owner == owner) {
			heap.free(p.offset);
			p.owner = nullptr;
			np++;
		}
	}
	if (nb || np)
		RLOG("client closed: freed %u buffer(s), %u program(s)", nb, np);
}
