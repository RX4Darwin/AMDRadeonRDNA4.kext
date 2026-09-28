//
//  ih.cpp
//  RDNA4FB
//
//  OSSSYS IH v7 ring and MSI delivery.  The ring is deliberately a small
//  completion path: the fence in VRAM remains authoritative and this code
//  only drains vectors and wakes the runtime's bounded waits.
//

#include "compute.hpp"
#include "ndrv.hpp"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/c++/OSDictionary.h>
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

static bool fenceReached(uint32_t current, uint32_t wanted) {
	return static_cast<int32_t>(current - wanted) >= 0;
}

constexpr uint8_t kIhClientGfx              = 0x0a;
constexpr uint8_t kIhClientUtcl2            = 0x1b;
constexpr uint8_t kIhClientDcn              = 0x04;
constexpr uint8_t kIhSrcSdmaTrap            = 49;
constexpr uint8_t kIhSrcCpEop               = 181;
constexpr uint8_t kIhSrcVblankBase          = 0x3c;
constexpr uint8_t kIhSrcPflipBase           = 0x4f;

// DCN 4.1.0 / dcn401 irq_service: OTG_GLOBAL_SYNC_STATUS and
// DCSURF_SURFACE_FLIP_INTERRUPT, both in DMU segment 2.
constexpr uint32_t kDcnOtgGlobalSync        = 0x1b88;
constexpr uint32_t kDcnHubpFlipInterrupt    = 0x0617;
constexpr uint32_t kDcnOtgStride            = 0x80;
constexpr uint32_t kDcnHubpStride           = 0xdc;
constexpr uint32_t kDcnVblankEnable        = 1u << 0;
constexpr uint32_t kDcnVblankAck           = 1u << 4;
constexpr uint32_t kDcnPflipEnable        = 1u << 0;
constexpr uint32_t kDcnPflipAck           = 1u << 8;

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

uint32_t ihPipeRead(void *ctx, uint8_t baseIdx, uint32_t dword) {
	return static_cast<RDNA4Compute *>(ctx)->ihDcnRead(baseIdx, dword);
}

} // namespace

RDNA4Compute::~RDNA4Compute() {
	stopPresentationTimer();
	ihStop();
	if (ihLock) {
		IOLockFree(ihLock);
		ihLock = nullptr;
	}
	if (resultDictionary) {
		resultDictionary->release();
		resultDictionary = nullptr;
	}
	if (resultLock) {
		IOLockFree(resultLock);
		resultLock = nullptr;
	}
}

uint32_t RDNA4Compute::ihDcnRead(uint8_t baseIdx, uint32_t dword) const {
	return rd(IpDiscovery::HwDmu, Reg { baseIdx, dword });
}

bool RDNA4Compute::ihInit() {
	uint32_t requested = 0;
	if (!PE_parse_boot_argn("rdna4-ih", &requested, sizeof(requested)) || !requested)
		return false;
	auto fail = [this](const char *why) {
		char value[128];
		snprintf(value, sizeof(value), "FAIL %s", why ? why : "initialisation failed");
		publishResult("ih", value);
		return false;
	};
	ihDcnRequested = requested >= 2;
	uint32_t vbl = 0;
	ihVblRequested = ihDcnRequested &&
	                 PE_parse_boot_argn("rdna4-vbl", &vbl, sizeof(vbl)) && vbl != 0;
	if (!env.pci || !env.mmio || !env.disc || !env.disc->isValid() || !rtLock) {
		HLOG("off: missing PCI, MMIO, discovery or runtime lock");
		return fail("missing PCI/MMIO/discovery/runtime lock");
	}
	const uint16_t command = env.pci->configRead16(kIOPCIConfigCommand);
	if (!(command & kIOPCICommandBusMaster)) {
		HLOG("off: PCI bus mastering is not enabled (command 0x%04x)", command);
		return fail("PCI bus mastering disabled");
	}
	if (!ihLock) {
		ihLock = IOLockAlloc();
		if (!ihLock) {
			HLOG("off: could not allocate the IH wait lock");
			return fail("IH wait lock allocation");
		}
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
		return fail("no PCI MSI interrupt source");
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
		return fail("ring or wptr allocation/map");
	}
	ihRingCpu = static_cast<volatile uint32_t *>(ihRingMemory->getBytesNoCopy());
	ihWptrCpu = static_cast<volatile uint32_t *>(ihWptrMemory->getBytesNoCopy());
	if (!ihRingCpu || !ihWptrCpu) {
		HLOG("off: ring or wptr has no CPU mapping");
		ihStop();
		return fail("ring or wptr CPU mapping");
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
		return fail("MSI callback context allocation");
	}
	context->compute = this;
	ihContext = context;
	ihWaitEvent = this;
	ihWorkLoop = env.owner->getWorkLoop();
	if (!ihWorkLoop) {
		HLOG("off: no kext work loop for MSI delivery");
		ihStop();
		return fail("kext work loop unavailable");
	}
	trail("ih: register MSI");
	ihSource = IOFilterInterruptEventSource::filterInterruptEventSource(
		context, ihEventAction, ihEventFilter, env.pci, msiIndex);
	if (!ihSource) {
		HLOG("off: MSI registration failed (source %d may already be registered)", msiIndex);
		ihStop();
		return fail("MSI registration");
	}
	IOReturn r = ihWorkLoop->addEventSource(ihSource);
	if (r != kIOReturnSuccess) {
		HLOG("off: adding MSI source to work loop failed 0x%08x", r);
		ihStop();
		return fail("adding MSI source to work loop");
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

	if (ihDcnRequested) {
		Pipe::State pipe {};
		if (!Pipe::discover(ihPipeRead, this, pipe) || !pipe.valid() ||
		    pipe.otg >= Pipe::kMaxOtg || pipe.hubp >= Pipe::kMaxOtg) {
			HLOG("DCN off: lit pipe discovery failed");
			publishResult("vblank", "FAIL DCN pipe discovery");
			HLOG("self-test totals: SDMA traps %u, CP EOP %u, interrupt wakeups %u",
			     ihSdmaTrapCount, ihEopCount, ihDispatchWakeups + ihSdmaWakeups);
			ihPublishResult();
			return true;
		}
		ihDcnOtg = pipe.otg;
		ihDcnHubp = pipe.hubp;
		ihDcnExpectedFrameNs = env.scanoutFrameNs;
		if (!ihDcnExpectedFrameNs && pipe.hTotal && pipe.vTotal)
			ihDcnExpectedFrameNs = 16666667;
		HLOG("DCN pipe: OTG%u HUBP%u, expected frame %llu ns", ihDcnOtg, ihDcnHubp,
		     ihDcnExpectedFrameNs);

		trail("ih: dcn enable");
		const Reg otgStatus { 2, kDcnOtgGlobalSync + ihDcnOtg * kDcnOtgStride };
		const Reg flipStatus { 2, kDcnHubpFlipInterrupt + ihDcnHubp * kDcnHubpStride };
		const uint32_t otgBefore = rd(IpDiscovery::HwDmu, otgStatus);
		const uint32_t flipBefore = rd(IpDiscovery::HwDmu, flipStatus);
		wr(IpDiscovery::HwDmu, otgStatus, otgBefore | kDcnVblankEnable);
		wr(IpDiscovery::HwDmu, flipStatus, flipBefore | kDcnPflipEnable);
		ihDcnActive = true;
		Ndrv::setVblankEnabled(ihVblRequested);
		ihDcnVblankFrames = 0;
		ihDcnFrameCounter = 0;
		ihDcnFrameEvents = 0;
		ihDcnStormFrames = 0;
		ihDcnShortIntervals = 0;
		ihDcnFrameCounterValid = false;
		ihDcnLastIvTimestamp = 0;
		ihDcnExpectedIvTicks = 0;
		ihDcnIvShortIntervals = 0;
		ihDcnIvTimestampValid = false;
		HLOG("DCN sources enabled: OTG_GLOBAL_SYNC_STATUS 0x%08x -> 0x%08x, "
		     "HUBP%u FLIP_INTERRUPT 0x%08x -> 0x%08x (vblank src %u, pflip src %u)",
		     otgBefore, rd(IpDiscovery::HwDmu, otgStatus), ihDcnHubp, flipBefore,
		     rd(IpDiscovery::HwDmu, flipStatus), kIhSrcVblankBase + ihDcnOtg,
		     kIhSrcPflipBase + ihDcnHubp);

		trail("ih: vblank self-test");
		uint64_t span = 0;
		nanoseconds_to_absolutetime(200000000, &span);
		const uint64_t end = mach_absolute_time() + span;
		uint64_t previousNs = 0;
		bool vblankOk = true;
		for (uint32_t i = 0; i < 5; i++) {
			const uint64_t now = mach_absolute_time();
			if (now >= end) {
				vblankOk = false;
				break;
			}
			uint64_t remaining = 0;
			absolutetime_to_nanoseconds(end - now, &remaining);
			uint64_t count = 0, timeNs = 0;
			if (!ihWaitVblank(ihDcnOtg, static_cast<uint32_t>((remaining + 999999) / 1000000),
			                  count, timeNs)) {
				vblankOk = false;
				break;
			}
			if (previousNs)
				HLOG("vblank self-test %u/5: count %llu interval %llu ns", i + 1, count,
				     timeNs - previousNs);
			else
				HLOG("vblank self-test %u/5: count %llu", i + 1, count);
			previousNs = timeNs;
		}
		if (!vblankOk) {
			ihDcnStop("vblank self-test timed out");
			publishResult("vblank", "FAIL vblank self-test timeout");
		} else {
			HLOG("vblank self-test passed: 5 frames, expected %llu ns", ihDcnExpectedFrameNs);
			publishResult("vblank", "PASS self-test, 5 frames");
			// IOGraphics must not be handed a VBL service until the interrupt
			// path has demonstrated that it can actually deliver five frames.
			if (ihVblRequested && env.vblankServiceReady)
				env.vblankServiceReady(env.owner);
		}
	}
	HLOG("self-test totals: SDMA traps %u, CP EOP %u, interrupt wakeups %u",
	     ihSdmaTrapCount, ihEopCount, ihDispatchWakeups + ihSdmaWakeups);
	ihPublishResult();
	return true;
}

void RDNA4Compute::ihDcnStopLocked(const char *why) {
	// Stop callbacks before touching the DCN source. IONDRV owns the VSL
	// service lifetime and disposes it through wrapVslDispose.
	Ndrv::setVblankEnabled(false);
	if (!ihDcnActive)
		return;
	if (ihDcnOtg < Pipe::kMaxOtg) {
		const Reg r { 2, kDcnOtgGlobalSync + ihDcnOtg * kDcnOtgStride };
		wr(IpDiscovery::HwDmu, r, rd(IpDiscovery::HwDmu, r) & ~kDcnVblankEnable);
	}
	if (ihDcnHubp < Pipe::kMaxOtg) {
		const Reg r { 2, kDcnHubpFlipInterrupt + ihDcnHubp * kDcnHubpStride };
		wr(IpDiscovery::HwDmu, r, rd(IpDiscovery::HwDmu, r) & ~kDcnPflipEnable);
	}
	ihDcnActive = false;
	ihDcnStorms++;
	HLOG("DCN sources disabled: %s (vblank %llu, pflip %llu)", why ? why : "failure",
	     ihDcnOtg < Pipe::kMaxOtg ? ihVblankCount[ihDcnOtg] : 0,
	     ihDcnHubp < Pipe::kMaxOtg ? ihPflipCount[ihDcnHubp] : 0);
	if (ihLock && ihWaitEvent)
		IOLockWakeup(ihLock, ihWaitEvent, false);
}

void RDNA4Compute::ihDcnStop(const char *why) {
	if (!ihLock) {
		ihDcnStopLocked(why);
		return;
	}
	IOLockLock(ihLock);
	ihDcnStopLocked(why);
	IOLockUnlock(ihLock);
}

void RDNA4Compute::ihDcnAckVblank() {
	if (ihDcnOtg >= Pipe::kMaxOtg)
		return;
	const Reg r { 2, kDcnOtgGlobalSync + ihDcnOtg * kDcnOtgStride };
	const uint32_t value = rd(IpDiscovery::HwDmu, r);
	wr(IpDiscovery::HwDmu, r, (value & ~kDcnVblankAck) | kDcnVblankAck);
}

void RDNA4Compute::ihDcnAckFlip() {
	if (ihDcnHubp >= Pipe::kMaxOtg)
		return;
	const Reg r { 2, kDcnHubpFlipInterrupt + ihDcnHubp * kDcnHubpStride };
	const uint32_t value = rd(IpDiscovery::HwDmu, r);
	wr(IpDiscovery::HwDmu, r, (value & ~kDcnPflipAck) | kDcnPflipAck);
}

void RDNA4Compute::ihDcnObserveVblank(uint64_t now, uint64_t ivTimestamp) {
	if (ihDcnOtg >= Pipe::kMaxOtg)
		return;
	uint64_t previous = ihVblankTime[ihDcnOtg];
	uint64_t interval = 0;
	if (previous)
		absolutetime_to_nanoseconds(now - previous, &interval);
	ihVblankTime[ihDcnOtg] = now;
	ihVblankCount[ihDcnOtg]++;
	if (ihDcnVblankFrames < 100) {
		ihDcnVblankFrames++;
		// The IH action timestamp is the drain time, not the vblank time. A
		// delayed work loop can therefore make two ordinary entries look like
		// a storm. Count entries against OTG_FRAME_COUNT instead: this is the
		// DCN 4.1.0 OTG_FRAME_COUNT register (base 2, 0x1b4d), also used by
		// Flip::waitNextVblank. Require two consecutive overloaded frames.
		const Reg frameReg { 2, Pipe::Reg::kOtgFrameCount + ihDcnOtg * Pipe::Reg::kOtgStride };
		const uint32_t frameImage = rd(IpDiscovery::HwDmu, frameReg);
		const bool frameValid = frameImage != 0xffffffffu;
		bool storm = false;
		uint32_t frameDelta = 0;
		uint32_t events = ihDcnFrameEvents;
		uint64_t ivDelta = 0;
		bool ivStorm = false;
		// IH v7 timestamps are 48 bits (dw1 | (dw2 & 0xffff) << 32),
		// captured when the vector is written. They keep queued vectors at
		// their real spacing even when ihAction drains them together.
		constexpr uint64_t kIvTimestampMask = (1ull << 48) - 1;
		if (ivTimestamp) {
			ivTimestamp &= kIvTimestampMask;
			if (ihDcnIvTimestampValid) {
				ivDelta = (ivTimestamp - ihDcnLastIvTimestamp) & kIvTimestampMask;
				if (ivDelta && !ihDcnExpectedIvTicks) {
					ihDcnExpectedIvTicks = ivDelta;
				} else if (ivDelta && ihDcnExpectedIvTicks) {
					const bool shortInterval = ivDelta * 2 < ihDcnExpectedIvTicks;
					ihDcnIvShortIntervals = shortInterval ? ihDcnIvShortIntervals + 1 : 0;
					ivStorm = ihDcnIvShortIntervals >= 8;
				}
			}
			ihDcnLastIvTimestamp = ivTimestamp;
			ihDcnIvTimestampValid = true;
		}
		if (frameValid) {
			const uint32_t frame = frameImage & 0xffffffu;
			if (!ihDcnFrameCounterValid) {
				ihDcnFrameCounter = frame;
				ihDcnFrameEvents = 1;
				ihDcnFrameCounterValid = true;
			} else {
				frameDelta = (frame - ihDcnFrameCounter) & 0xffffffu;
				if (frameDelta) {
					events = ihDcnFrameEvents;
					storm = frameDelta <= 100 && events > frameDelta * 2;
					ihDcnStormFrames = storm ? ihDcnStormFrames + 1 : 0;
					ihDcnFrameCounter = frame;
					ihDcnFrameEvents = 1;
				} else if (ihDcnFrameEvents != 0xffffffffu) {
					ihDcnFrameEvents++;
				}
			}
		} else if (!ivTimestamp || !ihDcnIvTimestampValid) {
			// If neither the frame counter nor the IV timestamp is available,
			// keep a conservative bounded fallback. A single short interval is
			// never enough to disable DCN.
			const bool shortInterval = interval && ihDcnExpectedFrameNs &&
			                           interval * 2 < ihDcnExpectedFrameNs;
			ihDcnShortIntervals = shortInterval ? ihDcnShortIntervals + 1 : 0;
			storm = ihDcnShortIntervals >= 8;
		}
		if ((frameValid && ihDcnStormFrames >= 2) || ivStorm || (!frameValid && storm)) {
			HLOG("DCN IRQ storm: %u vblanks in frame window, frame delta %u, "
			     "OTG_FRAME_COUNT %s0x%06x, IV delta %llu (expected %llu), "
			     "drain interval %llu ns (expected %llu ns)",
			     events, frameDelta, frameValid ? "" : "unreadable/", frameValid ?
			     (frameImage & 0xffffffu) : 0u,
			     ivDelta, ihDcnExpectedIvTicks, interval, ihDcnExpectedFrameNs);
			publishResult("vblank", "FAIL DCN IRQ storm");
			ihDcnStopLocked("interrupt rate exceeded 2x OTG timing");
			return;
		}
	}
	// First frame, the first second, then once a minute: with rdna4-ih=2 the
	// desktop takes vblanks forever, and a line a second would wrap the
	// kernel log that the diagnostic batch still reads.
	const uint64_t n = ihVblankCount[ihDcnOtg];
	if (n == 1 || n == 60 || !(n % 3600)) {
		HLOG("vblank: OTG%u count %llu%s", ihDcnOtg, ihVblankCount[ihDcnOtg],
		     interval ? " (acknowledged)" : "");
		char value[96];
		snprintf(value, sizeof(value), "PASS IRQ frames %llu", ihVblankCount[ihDcnOtg]);
		publishResult("vblank", value);
	}
}

bool RDNA4Compute::ihWaitVblank(uint32_t otg, uint32_t timeoutMs, uint64_t &count,
                                uint64_t &timeNs) {
	IOLock *lock = ihLock;
	if (!lock)
		return false;
	IOLockLock(lock);
	if (otg == Pipe::kNone)
		otg = ihDcnOtg;
	if (!ihDcnActive || otg >= Pipe::kMaxOtg || otg != ihDcnOtg || !ihWaitEvent) {
		IOLockUnlock(lock);
		return false;
	}
	const uint32_t boundedTimeout = timeoutMs > RDNA4_MAX_TIMEOUT_MS ?
	                                RDNA4_MAX_TIMEOUT_MS : timeoutMs;
	uint64_t span = 0;
	nanoseconds_to_absolutetime(static_cast<uint64_t>(boundedTimeout) * 1000000, &span);
	const uint64_t deadline = mach_absolute_time() + span;
	const uint64_t before = ihVblankCount[otg];
	for (;;) {
		if (!ihDcnActive)
			break;
		if (ihVblankCount[otg] > before) {
			count = ihVblankCount[otg];
			absolutetime_to_nanoseconds(ihVblankTime[otg], &timeNs);
			IOLockUnlock(lock);
			return true;
		}
		const uint64_t now = mach_absolute_time();
		if (now >= deadline)
			break;
		uint64_t sleepSpan = 0;
		nanoseconds_to_absolutetime(2000000, &sleepSpan);
		const uint64_t wake = now + sleepSpan < deadline ? now + sleepSpan : deadline;
		IOLockSleepDeadline(lock, ihWaitEvent, wake, THREAD_INTERRUPTIBLE);
	}
	IOLockUnlock(lock);
	return false;
}

bool RDNA4Compute::ihWaitFlip(uint32_t hubp, uint64_t sinceCount, uint32_t timeoutMs) {
	IOLock *lock = ihLock;
	if (!lock)
		return false;
	IOLockLock(lock);
	if (!ihDcnActive || hubp >= Pipe::kMaxOtg || hubp != ihDcnHubp || !ihWaitEvent) {
		IOLockUnlock(lock);
		return false;
	}
	const uint32_t boundedTimeout = timeoutMs > RDNA4_MAX_TIMEOUT_MS ?
	                                RDNA4_MAX_TIMEOUT_MS : timeoutMs;
	uint64_t span = 0;
	nanoseconds_to_absolutetime(static_cast<uint64_t>(boundedTimeout) * 1000000, &span);
	const uint64_t deadline = mach_absolute_time() + span;
	for (;;) {
		if (!ihDcnActive)
			break;
		if (ihPflipCount[hubp] > sinceCount) {
			IOLockUnlock(lock);
			return true;
		}
		const uint64_t now = mach_absolute_time();
		if (now >= deadline)
			break;
		uint64_t sleepSpan = 0;
		nanoseconds_to_absolutetime(2000000, &sleepSpan);
		const uint64_t wake = now + sleepSpan < deadline ? now + sleepSpan : deadline;
		IOLockSleepDeadline(lock, ihWaitEvent, wake, THREAD_INTERRUPTIBLE);
	}
	IOLockUnlock(lock);
	return false;
}

void RDNA4Compute::ihStop() {
	if (ihLock)
		IOLockLock(ihLock);
	ihDcnStopLocked("IH stopped");
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
	if (ihLock) {
		if (ihWaitEvent)
			IOLockWakeup(ihLock, ihWaitEvent, false);
		IOLockUnlock(ihLock);
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

void RDNA4Compute::ihPublishResult() {
	if (!ihActive)
		return;
	const uint32_t wakeups = ihDispatchWakeups + ihSdmaWakeups;
	char value[160];
	snprintf(value, sizeof(value), "PASS ring up, %u interrupt wakeups%s", wakeups,
	         (ihDispatchPolling || ihSdmaPolling) ? "; polling fallback" : "");
	publishResult("ih", value);
}

bool RDNA4Compute::ihInterruptLogAllowed() {
	const uint64_t now = mach_absolute_time();
	if (!ihLastInterruptLog) {
		ihLastInterruptLog = now;
		return true;
	}
	uint64_t elapsed = 0;
	absolutetime_to_nanoseconds(now - ihLastInterruptLog, &elapsed);
	if (elapsed < 250000000)
		return false;
	ihLastInterruptLog = now;
	return true;
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
		if (ihInterruptLogAllowed())
			HLOG("CP EOP interrupt: count %u ring %u", ihEopCount, entry.ringId);
		return;
	}
	if (entry.clientId == kIhClientGfx && entry.srcId == kIhSrcSdmaTrap) {
		ihSdmaTrapCount++;
		if (ihInterruptLogAllowed())
			HLOG("SDMA trap interrupt: count %u", ihSdmaTrapCount);
		return;
	}
	if (entry.clientId == kIhClientDcn && ihDcnActive &&
	    entry.srcId == kIhSrcVblankBase + ihDcnOtg) {
		ihDcnObserveVblank(mach_absolute_time(), entry.timestamp);
		if (ihDcnActive && ihVblRequested)
			Ndrv::signalVblank();
		ihDcnAckVblank();
		return;
	}
	if (entry.clientId == kIhClientDcn && ihDcnActive &&
	    entry.srcId == kIhSrcPflipBase + ihDcnHubp) {
		if (ihDcnHubp < Pipe::kMaxOtg)
			ihPflipCount[ihDcnHubp]++;
		// Same thinning as vblank: once presents run every frame, a line per
		// 64 flips would still wrap the log.
		const uint64_t f = ihPflipCount[ihDcnHubp];
		if (f == 1 || f == 64 || !(f % 4096)) {
			HLOG("page flip: HUBP%u count %llu", ihDcnHubp, ihPflipCount[ihDcnHubp]);
			char value[96];
			snprintf(value, sizeof(value), "PASS pflip interrupts %llu", ihPflipCount[ihDcnHubp]);
			publishResult("flip", value);
		}
		ihDcnAckFlip();
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
	if (!ihLock)
		return;
	IOLockLock(ihLock);
	if (!ihActive || !ihRingCpu || !ihWptrCpu || !ihRingMemory || !ihWaitEvent) {
		IOLockUnlock(ihLock);
		return;
	}
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
	IOLockWakeup(ihLock, ihWaitEvent, false);
	IOLockUnlock(ihLock);
}

void RDNA4Compute::ihRecordWait(bool dispatch, bool slept, bool completed,
                                bool recheckElapsed, uint32_t eventsBefore) {
	const uint32_t events = dispatch ? ihEopCount : ihSdmaTrapCount;
	uint32_t &misses = dispatch ? ihDispatchMisses : ihSdmaMisses;
	bool &polling = dispatch ? ihDispatchPolling : ihSdmaPolling;
	const bool sourceAdvanced = events != eventsBefore;
	if (sourceAdvanced) {
		misses = 0;
		if (completed) {
			if (dispatch)
				ihDispatchWakeups++;
			else
				ihSdmaWakeups++;
			ihPublishResult();
		}
	} else if (Ih::missEligible(slept, completed, sourceAdvanced, recheckElapsed)) {
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
	if (!useIh) {
		for (uint32_t polls = 0;; polls++) {
			done = fenceReached(*fence, value);
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
		bool slept = false;
		for (;;) {
			done = fenceReached(*fence, value);
			if (done || mach_absolute_time() > deadline)
				break;
			uint64_t sleepSpan = 0;
			nanoseconds_to_absolutetime(2000000, &sleepSpan);
			IOLockSleepDeadline(ihLock, ihWaitEvent, mach_absolute_time() + sleepSpan,
			                    THREAD_UNINT);
			slept = true;
			done = fenceReached(*fence, value);
			if (!done)
				continue;

			// A fence may become visible before the IH work-loop drains its
			// vector.  Check the source once, then perform the required 5 ms
			// recheck without holding ihLock so the action can drain it.
			const uint32_t eventsNow = dispatch ? ihEopCount : ihSdmaTrapCount;
			if (eventsNow != eventsBefore) {
				ihRecordWait(dispatch, slept, true, false, eventsBefore);
				break;
			}
			uint64_t recheckSpan = 0;
			nanoseconds_to_absolutetime(5000000, &recheckSpan);
			const uint64_t recheckStart = mach_absolute_time();
			if (recheckStart + recheckSpan > deadline)
				break;
			IOLockUnlock(ihLock);
			IOSleep(5);
			IOLockLock(ihLock);
			ihRecordWait(dispatch, slept, true, true, eventsBefore);
			break;
		}
		IOLockUnlock(ihLock);
	}
	if (!useIh && done)
		ihRecordWait(dispatch, false, true, false, eventsBefore);
	observed = dispatch ? ihEopCount : ihSdmaTrapCount;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	if (!done && tag)
		HLOG("%s wait timed out: fence 0x%08x want 0x%08x", tag, *fence, value);
	return done;
}
