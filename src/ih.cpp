//
//  ih.cpp
//  RDNA4FB
//
//  OSSSYS IH v7 ring and MSI delivery.  The ring is deliberately a small
//  completion path: the fence in VRAM remains authoritative and this code
//  only drains vectors and wakes the runtime's bounded waits.
//

#include "compute.hpp"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/c++/OSObject.h>
#include <pexpert/pexpert.h>

#define HLOG(fmt, ...) IOLog("RDNA4FB: ih: " fmt "\n", ## __VA_ARGS__)

using namespace GfxReg;

namespace {

// OSSSYS v7.0.0 register offsets, relative to the discovery segment 0.
constexpr Reg IhRbCntl       { 0, 0x0080 };
constexpr Reg IhRbRptr       { 0, 0x0081 };
constexpr Reg IhRbWptr       { 0, 0x0082 };
constexpr Reg IhRbBase       { 0, 0x0083 };
constexpr Reg IhRbBaseHi     { 0, 0x0084 };
constexpr Reg IhWptrAddrHi   { 0, 0x0085 };
constexpr Reg IhWptrAddrLo   { 0, 0x0086 };
constexpr Reg IhDoorbellRptr { 0, 0x0087 };
constexpr Reg IhRbCntlRing1  { 0, 0x008c };
constexpr Reg IhChicken      { 0, 0x018a };

constexpr uint32_t kIhRbEnable              = 1u << 0;
constexpr uint32_t kIhRbSizeShift           = 1;
constexpr uint32_t kIhWptrWritebackEnable   = 1u << 8;
constexpr uint32_t kIhWptrOverflowEnable    = 1u << 16;
constexpr uint32_t kIhEnableIntr            = 1u << 17;
constexpr uint32_t kIhMcSnoop               = 1u << 20;
constexpr uint32_t kIhRptrRearm             = 1u << 21;
constexpr uint32_t kIhMcSpaceShift          = 28;
constexpr uint32_t kIhWptrOverflowClear     = 1u << 31;
constexpr uint32_t kIhWptrOverflow          = 1u;
constexpr uint32_t kIhMcSpaceBus            = 2u;

constexpr uint8_t kIhClientGfx              = 0x0a;
constexpr uint8_t kIhClientUtcl2            = 0x1b;
constexpr uint8_t kIhSrcSdmaTrap            = 49;
constexpr uint8_t kIhSrcCpEop               = 181;

class RDNA4IHContext : public OSObject {
	OSDeclareDefaultStructors(RDNA4IHContext);

public:
	RDNA4Compute *compute { nullptr };
	bool init() override { return OSObject::init(); }
};

OSDefineMetaClassAndStructors(RDNA4IHContext, OSObject);

bool mapDma(IOBufferMemoryDescriptor *memory, IODMACommand **command, uint64_t &bus) {
	*command = IODMACommand::withSpecification(kIODMACommandOutputHost64, 40, 0,
	                                            IODMACommand::kMapped, 0, 1);
	if (!*command || (*command)->setMemoryDescriptor(memory) != kIOReturnSuccess) {
		if (*command) {
			(*command)->release();
			*command = nullptr;
		}
		return false;
	}
	UInt64 offset = 0;
	IODMACommand::Segment64 segment {};
	UInt32 segments = 1;
	if ((*command)->gen64IOVMSegments(&offset, &segment, &segments) != kIOReturnSuccess ||
	    segments != 1 || !segment.fLength) {
		(*command)->clearMemoryDescriptor();
		(*command)->release();
		*command = nullptr;
		return false;
	}
	bus = segment.fIOVMAddr;
	return true;
}

RDNA4IHContext *contextFor(OSObject *owner) {
	return static_cast<RDNA4IHContext *>(owner);
}

bool ihEventFilter(OSObject *owner, IOFilterInterruptEventSource *) {
	RDNA4IHContext *context = contextFor(owner);
	return context && context->compute && context->compute->ihHasWork();
}

void ihEventAction(OSObject *owner, IOInterruptEventSource *, int) {
	RDNA4IHContext *context = contextFor(owner);
	if (context && context->compute)
		context->compute->ihAction();
}

} // namespace

RDNA4Compute::~RDNA4Compute() {
	ihStop();
}

bool RDNA4Compute::ihInit() {
	uint32_t requested = 0;
	if (!PE_parse_boot_argn("rdna4-ih", &requested, sizeof(requested)) || !requested)
		return false;
	if (!env.pci || !env.mmio || !env.disc || !env.disc->isValid() || !rtLock) {
		HLOG("off: missing PCI, MMIO, discovery or runtime lock");
		return false;
	}
	const uint16_t command = env.pci->configRead16(kIOPCIConfigCommand);
	if (!(command & kIOPCICommandBusMaster)) {
		HLOG("off: PCI bus mastering is not enabled (command 0x%04x)", command);
		return false;
	}
	ihLock = IOLockAlloc();
	if (!ihLock) {
		HLOG("off: could not allocate the IH wait lock");
		return false;
	}

	int msiIndex = -1;
	for (int i = 0; i < 32; i++) {
		int type = 0;
		const IOReturn r = env.pci->getInterruptType(i, &type);
		if (r != kIOReturnSuccess)
			break;
		HLOG("PCI interrupt %d: type 0x%x%s", i, type,
		     (type & kIOInterruptTypePCIMessaged) ? " (MSI)" : "");
		if (msiIndex < 0 && (type & kIOInterruptTypePCIMessaged))
			msiIndex = i;
	}
	if (msiIndex < 0) {
		HLOG("off: no PCI MSI interrupt source");
		return false;
	}

	const uint32_t beforeCntl = rd(IpDiscovery::HwOsssys, IhRbCntl);
	const uint32_t beforeBase = rd(IpDiscovery::HwOsssys, IhRbBase);
	const uint32_t beforeBaseHi = rd(IpDiscovery::HwOsssys, IhRbBaseHi);
	HLOG("before program: RB_CNTL 0x%08x BASE 0x%08x BASE_HI 0x%08x RPTR 0x%08x WPTR 0x%08x",
	     beforeCntl, beforeBase, beforeBaseHi, rd(IpDiscovery::HwOsssys, IhRbRptr),
	     rd(IpDiscovery::HwOsssys, IhRbWptr));
	HLOG("before program: WPTR_ADDR 0x%08x%08x DOORBELL_RPTR 0x%08x ring1 CNTL 0x%08x",
	     rd(IpDiscovery::HwOsssys, IhWptrAddrHi), rd(IpDiscovery::HwOsssys, IhWptrAddrLo),
	     rd(IpDiscovery::HwOsssys, IhDoorbellRptr), rd(IpDiscovery::HwOsssys, IhRbCntlRing1));

	ihRingMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kIhRingBytes,
		0x000000fffffff000ull);
	ihWptrMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kIhWptrBytes,
		0x000000fffffff000ull);
	if (!ihRingMemory || !ihWptrMemory ||
	    !mapDma(ihRingMemory, &ihRingDma, ihRingBus) ||
	    !mapDma(ihWptrMemory, &ihWptrDma, ihWptrBus)) {
		HLOG("off: could not allocate/map the 256 KiB ring and wptr page");
		ihStop();
		return false;
	}
	ihRingCpu = static_cast<volatile uint32_t *>(ihRingMemory->getBytesNoCopy());
	ihWptrCpu = static_cast<volatile uint32_t *>(ihWptrMemory->getBytesNoCopy());
	if (!ihRingCpu || !ihWptrCpu) {
		HLOG("off: ring or wptr has no CPU mapping");
		ihStop();
		return false;
	}
	for (uint32_t i = 0; i < kIhRingBytes / 4; i++)
		ihRingCpu[i] = 0;
	*ihWptrCpu = 0;
	ihRingMemory->performOperation(kIOMemoryIncoherentIOFlush, 0, kIhRingBytes);
	ihWptrMemory->performOperation(kIOMemoryIncoherentIOFlush, 0, sizeof(uint32_t));

	// Bus-address IH needs the OSSSYS GPA route in addition to MC_SPACE=2.
	trail("ih: program ring");
	wr(IpDiscovery::HwOsssys, IhChicken,
	   rd(IpDiscovery::HwOsssys, IhChicken) | (1u << 4));
	wr(IpDiscovery::HwOsssys, IhRbBase, static_cast<uint32_t>(ihRingBus >> 8));
	wr(IpDiscovery::HwOsssys, IhRbBaseHi, static_cast<uint32_t>(ihRingBus >> 40) & 0xff);
	uint32_t cntl = (16u << kIhRbSizeShift) | kIhWptrWritebackEnable |
	                kIhWptrOverflowEnable | kIhMcSnoop | kIhRptrRearm |
	                (kIhMcSpaceBus << kIhMcSpaceShift);
	wr(IpDiscovery::HwOsssys, IhRbCntl, cntl);
	wr(IpDiscovery::HwOsssys, IhWptrAddrLo, static_cast<uint32_t>(ihWptrBus) & ~3u);
	wr(IpDiscovery::HwOsssys, IhWptrAddrHi, static_cast<uint32_t>(ihWptrBus >> 32) & 0xffff);
	wr(IpDiscovery::HwOsssys, IhRbRptr, 0);
	wr(IpDiscovery::HwOsssys, IhRbWptr, 0);
	wr(IpDiscovery::HwOsssys, IhDoorbellRptr, 0);
	ihRptr = 0;

	RDNA4IHContext *context = OSTypeAlloc(RDNA4IHContext);
	if (!context || !context->init()) {
		OSSafeReleaseNULL(context);
		HLOG("off: could not allocate the MSI callback context");
		ihStop();
		return false;
	}
	context->compute = this;
	ihContext = context;
	ihWaitEvent = this;
	ihWorkLoop = env.owner->getWorkLoop();
	if (!ihWorkLoop) {
		HLOG("off: no kext work loop for MSI delivery");
		ihStop();
		return false;
	}
	trail("ih: register MSI");
	ihSource = IOFilterInterruptEventSource::filterInterruptEventSource(
		context, ihEventAction, ihEventFilter, env.pci, msiIndex);
	if (!ihSource) {
		HLOG("off: MSI registration failed (source %d may already be registered)", msiIndex);
		ihStop();
		return false;
	}
	IOReturn r = ihWorkLoop->addEventSource(ihSource);
	if (r != kIOReturnSuccess) {
		HLOG("off: adding MSI source to work loop failed 0x%08x", r);
		ihStop();
		return false;
	}

	// The kext's queues are on MEC1: pipe 0 queue 0 (the kernel's) and the
	// runtime clients' queues (W2), on pipes 0 and 1 (the pipes gfx12 has
	// CP_ME1_PIPEn_INT_CNTL for). The source bits are enabled after IOKit
	// registration, so a vector cannot arrive before the drain path exists.
	trail("ih: enable sources");
	wr(IpDiscovery::HwGc, CpMe1Pipe0IntCntl,
	   rd(IpDiscovery::HwGc, CpMe1Pipe0IntCntl) | kCpTimeStampIntEnable);
	wr(IpDiscovery::HwGc, CpMe1Pipe1IntCntl,
	   rd(IpDiscovery::HwGc, CpMe1Pipe1IntCntl) | kCpTimeStampIntEnable);
	wr(IpDiscovery::HwGc, sdma(0, SdmaCntl),
	   rd(IpDiscovery::HwGc, sdma(0, SdmaCntl)) | 1u); // TRAP_ENABLE
	trail("ih: enable ring");
	wr(IpDiscovery::HwOsssys, IhRbCntl, cntl | kIhRbEnable | kIhEnableIntr);
	ihActive = true;
	ihSource->enable();
	HLOG("ring up: bus 0x%llx, size %u KiB, wptr bus 0x%llx; MSI index %d registered; "
	     "CP EOP and SDMA trap enabled", ihRingBus, kIhRingBytes >> 10, ihWptrBus, msiIndex);

	// Exercise both interrupt sources before runStages writes its final
	// "finished" trail. A source can remain active while its waits fall back
	// to polling if the platform registers MSI but does not deliver this class
	// of vector reliably.
	trail("ih: self-test");
	const uint32_t sdmaBefore = ihSdmaTrapCount;
	uint32_t sdmaPacket[8];
	const bool sdmaFence = sdmaRun(sdmaPacket,
	                               Sdma::writeDword(sdmaPacket, poolMc(kSdmaTestOffset),
	                                                0x1a5a5a5u),
	                               2000);
	for (uint32_t ms = 0; ms < 20 && ihSdmaTrapCount == sdmaBefore; ms++)
		IOSleep(1);
	if (!sdmaFence || ihSdmaTrapCount == sdmaBefore) {
		ihSdmaPolling = true;
		HLOG("self-test: SDMA fence/trap %s; SDMA waits will use polling",
		     sdmaFence ? "did not deliver an IH source" : "failed");
	} else {
		HLOG("self-test: SDMA fence/trap delivered (source count %u)", ihSdmaTrapCount);
	}

	// stageCompute has already established the MEC queue by the time runtime
	// publication reaches this point, so prove the CP EOP source with the
	// smallest fenced packet as well.
	const uint32_t eopBefore = ihEopCount;
	const uint32_t eopFence = ++pm4Fence;
	*poolDw(kPm4FenceOffset) = 0;
	flushHdp();
	uint32_t eopPacket[8];
	const bool eopQueued = pm4Queue.emit(eopPacket,
	                                      Pm4::releaseMem(eopPacket, poolMc(kPm4FenceOffset),
	                                                       eopFence, true));
	if (eopQueued) {
		pm4Kick(pm4Queue.wptr());
		uint64_t ns = 0;
		const bool eopFenceDone = ihWaitFence(poolDw(kPm4FenceOffset), eopFence, 2000,
		                                     true, "IH self-test CP", ns);
		for (uint32_t ms = 0; ms < 20 && ihEopCount == eopBefore; ms++)
			IOSleep(1);
		if (!eopFenceDone || ihEopCount == eopBefore) {
			ihDispatchPolling = true;
			HLOG("self-test: CP RELEASE_MEM/EOP %s; dispatch waits will use polling",
			     eopFenceDone ? "did not deliver an IH source" : "failed");
		} else {
			HLOG("self-test: CP EOP delivered (source count %u)", ihEopCount);
		}
	} else {
		ihDispatchPolling = true;
		HLOG("self-test: could not queue CP RELEASE_MEM; dispatch waits will use polling");
	}
	return true;
}

void RDNA4Compute::ihStop() {
	if (ihActive) {
		wr(IpDiscovery::HwOsssys, IhRbCntl,
		   rd(IpDiscovery::HwOsssys, IhRbCntl) & ~(kIhRbEnable | kIhEnableIntr));
		wr(IpDiscovery::HwGc, CpMe1Pipe0IntCntl,
		   rd(IpDiscovery::HwGc, CpMe1Pipe0IntCntl) & ~kCpTimeStampIntEnable);
		wr(IpDiscovery::HwGc, CpMe1Pipe1IntCntl,
		   rd(IpDiscovery::HwGc, CpMe1Pipe1IntCntl) & ~kCpTimeStampIntEnable);
		wr(IpDiscovery::HwGc, sdma(0, SdmaCntl),
		   rd(IpDiscovery::HwGc, sdma(0, SdmaCntl)) & ~1u);
		ihActive = false;
	}
	if (ihSource) {
		ihSource->disable();
		if (ihWorkLoop)
			ihWorkLoop->removeEventSource(ihSource);
		ihSource->release();
		ihSource = nullptr;
	}
	if (ihContext) {
		static_cast<RDNA4IHContext *>(ihContext)->compute = nullptr;
		ihContext->release();
		ihContext = nullptr;
	}
	ihWorkLoop = nullptr;
	ihWaitEvent = nullptr;
	if (ihLock) {
		IOLockFree(ihLock);
		ihLock = nullptr;
	}
	if (ihRingDma) {
		ihRingDma->clearMemoryDescriptor();
		ihRingDma->release();
		ihRingDma = nullptr;
	}
	if (ihWptrDma) {
		ihWptrDma->clearMemoryDescriptor();
		ihWptrDma->release();
		ihWptrDma = nullptr;
	}
	OSSafeReleaseNULL(ihRingMemory);
	OSSafeReleaseNULL(ihWptrMemory);
	ihRingCpu = nullptr;
	ihWptrCpu = nullptr;
	ihRingBus = ihWptrBus = 0;
}

bool RDNA4Compute::ihHasWork() const {
	if (!ihActive || !ihWptrCpu)
		return false;
	const uint32_t wptr = *ihWptrCpu;
	return (wptr & kIhWptrOverflow) || Ih::hasEntries(ihRptr, wptr, kIhRingBytes);
}

void RDNA4Compute::ihUnknown(uint8_t client, uint8_t source, uint8_t ring) {
	const uint8_t bit = static_cast<uint8_t>(1u << (source & 7));
	uint8_t &seen = ihUnknownSeen[client][source >> 3];
	if (seen & bit)
		return;
	seen |= bit;
	ihUnknownCount++;
	HLOG("unknown source: client %u source %u ring %u (pair logged once)", client, source, ring);
}

void RDNA4Compute::ihDecodeEntry(const uint32_t *dw) {
	Ih::Entry entry {};
	Ih::decode(dw, entry);
	// ring_id = queue [6:4] | me [3:2] | pipe [1:0] (gfx_v12_0_eop_irq): any
	// MEC1 queue, the kernel's or a runtime client's.
	if (entry.clientId == kIhClientGfx && entry.srcId == kIhSrcCpEop &&
	    ((entry.ringId >> 2) & 3) == 1) {
		ihEopCount++;
		if (ihEopCount == 1 || !(ihEopCount & 0x3f))
			HLOG("CP EOP interrupt: count %u ring %u", ihEopCount, entry.ringId);
		return;
	}
	if (entry.clientId == kIhClientGfx && entry.srcId == kIhSrcSdmaTrap) {
		ihSdmaTrapCount++;
		if (ihSdmaTrapCount == 1 || !(ihSdmaTrapCount & 0x3f))
			HLOG("SDMA trap interrupt: count %u", ihSdmaTrapCount);
		return;
	}
	if (entry.clientId == kIhClientUtcl2) {
		ihFaultCount++;
		if (ihFaultCount == 1 || ihFaultCount == 4 || (ihFaultCount & 0x3f) == 0) {
			HLOG("GC UTCL2 protection fault: source %u status 0x%08x address 0x%08x%08x",
			     entry.srcId, entry.srcData[0], entry.srcData[2], entry.srcData[1]);
			logGcFault("IH");
		}
		return;
	}
	ihUnknown(entry.clientId, entry.srcId, entry.ringId);
}

void RDNA4Compute::ihAction() {
	if (!ihActive || !ihRingCpu || !ihWptrCpu)
		return;
	const uint32_t raw = *ihWptrCpu;
	const uint32_t wptr = raw & ihRingMask;
	if (raw & kIhWptrOverflow) {
		ihRptr = Ih::overflowRecovery(wptr, kIhRingBytes);
		HLOG("ring overflow: raw wptr 0x%08x, skipping to 0x%08x", raw, ihRptr);
		uint32_t cntl = rd(IpDiscovery::HwOsssys, IhRbCntl);
		wr(IpDiscovery::HwOsssys, IhRbCntl, cntl | kIhWptrOverflowClear);
		wr(IpDiscovery::HwOsssys, IhRbCntl, cntl & ~kIhWptrOverflowClear);
	} else {
		uint32_t bytes = wptr >= ihRptr ? wptr - ihRptr : kIhRingBytes - ihRptr;
		if (bytes) {
			ihRingMemory->performOperation(kIOMemoryIncoherentIOFlush, ihRptr, bytes);
		}
		if (wptr < ihRptr && wptr)
			ihRingMemory->performOperation(kIOMemoryIncoherentIOFlush, 0, wptr);
		while (ihRptr != wptr) {
			uint32_t dw[Ih::kEntryDwords];
			for (uint32_t i = 0; i < Ih::kEntryDwords; i++)
				dw[i] = ihRingCpu[((ihRptr + i * 4) & ihRingMask) / 4];
			ihDecodeEntry(dw);
			ihRptr = Ih::advance(ihRptr, Ih::kEntryBytes, kIhRingBytes);
		}
	}
	wr(IpDiscovery::HwOsssys, IhRbRptr, ihRptr);
	if (ihLock) {
		IOLockLock(ihLock);
		IOLockWakeup(ihLock, ihWaitEvent, false);
		IOLockUnlock(ihLock);
	}
}

void RDNA4Compute::ihRecordWait(bool dispatch, bool woke, bool completed, uint32_t eventsBefore) {
	const uint32_t events = dispatch ? ihEopCount : ihSdmaTrapCount;
	uint32_t &misses = dispatch ? ihDispatchMisses : ihSdmaMisses;
	bool &polling = dispatch ? ihDispatchPolling : ihSdmaPolling;
	if (events != eventsBefore) {
		misses = 0;
		if (completed)
			HLOG("%s wait woken by interrupt (IH source count %u)", dispatch ? "dispatch" : "SDMA",
			     events);
	} else if (completed && !woke) {
		misses++;
		if (misses >= 3 && !polling) {
			polling = true;
			HLOG("%s interrupt source did not wake three completed waits; falling back to polling",
			     dispatch ? "dispatch" : "SDMA");
		}
	}
}

bool RDNA4Compute::ihWaitFence(volatile uint32_t *fence, uint32_t value, uint32_t timeoutMs,
                               bool dispatch, const char *tag, uint64_t &ns) {
	const uint64_t t0 = mach_absolute_time();
	uint64_t span = 0;
	nanoseconds_to_absolutetime(static_cast<uint64_t>(timeoutMs) * 1000000, &span);
	const uint64_t deadline = t0 + span;
	const bool useIh = ihActive && !(dispatch ? ihDispatchPolling : ihSdmaPolling) && ihLock;
	uint32_t &observed = dispatch ? ihDispatchObserved : ihSdmaObserved;
	const uint32_t eventsBefore = dispatch ? ihEopCount : ihSdmaTrapCount;
	bool done = false;
	bool recorded = false;
	if (!useIh) {
		for (uint32_t polls = 0;; polls++) {
			done = *fence == value;
			if (done || mach_absolute_time() > deadline)
				break;
			if (polls < 200)
				IODelay(10);
			else
				IOSleep(1);
		}
	} else {
		// This lock is intentionally separate from rtLock: bring-up and later
		// queue owners may call this path without holding the runtime lock.
		IOLockLock(ihLock);
		for (;;) {
			done = *fence == value;
			if (done || mach_absolute_time() > deadline)
				break;
			uint64_t sleepSpan = 0;
			nanoseconds_to_absolutetime(2000000, &sleepSpan);
			const wait_result_t wr = IOLockSleepDeadline(ihLock, ihWaitEvent,
		                                                mach_absolute_time() + sleepSpan,
		                                                THREAD_UNINT);
			const bool woke = wr != THREAD_TIMED_OUT;
			done = *fence == value;
			ihRecordWait(dispatch, woke, done, eventsBefore);
			recorded = true;
			if (done || mach_absolute_time() > deadline)
				break;
		}
		IOLockUnlock(ihLock);
	}
	if (useIh && done && !recorded) {
		const uint32_t events = dispatch ? ihEopCount : ihSdmaTrapCount;
		const bool source = events != observed;
		ihRecordWait(dispatch, source, true, source ? observed : eventsBefore);
	}
	if (!useIh && done)
		ihRecordWait(dispatch, false, true, eventsBefore);
	observed = dispatch ? ihEopCount : ihSdmaTrapCount;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	if (!done && tag)
		HLOG("%s wait timed out: fence 0x%08x want 0x%08x", tag, *fence, value);
	return done;
}
