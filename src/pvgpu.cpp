//
//  pvgpu.cpp
//  RDNA4FB
//
//  See pvgpu.hpp and docs/m0-pvgpu.md.
//

#include "pvgpu.hpp"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>

OSDefineMetaClassAndStructors(RDNA4PvNub, IOPCIDevice)

using namespace pvgpu;

namespace {

constexpr uint32_t kMaxLogLines = 400;
uint32_t gLogLines { 0 };
RDNA4PvNub *gNub { nullptr };

void pvlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void pvlog(const char *fmt, ...) {
	const uint32_t n = __atomic_fetch_add(&gLogLines, 1u, __ATOMIC_RELAXED);
	if (n >= kMaxLogLines) {
		if (n == kMaxLogLines)
			IOLog("RDNA4FB: pvgpu: log budget (%u lines) used up\n", kMaxLogLines);
		return;
	}
	char buf[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	IOLog("RDNA4FB: pvgpu: %s\n", buf);
}

// "0xEEEE106B", "0xEEEE106B&0xFFFFFFFF", several separated by blanks: does `ids` match one of them? (IOPCIMatch / IOPCIPrimaryMatch format)
bool matchIds(const char *spec, uint32_t ids) {
	while (*spec) {
		while (*spec == ' ' || *spec == '\t')
			spec++;
		if (!*spec)
			break;
		char *end = nullptr;
		const uint32_t value = static_cast<uint32_t>(strtoul(spec, &end, 0));
		if (end == spec)
			return false;
		uint32_t mask = 0xffffffff;
		if (*end == '&') {
			const char *m = end + 1;
			mask = static_cast<uint32_t>(strtoul(m, &end, 0));
			if (end == m)
				return false;
		}
		if ((ids & mask) == (value & mask))
			return true;
		spec = end;
	}
	return false;
}

const char *stringKey(OSDictionary *table, const char *key) {
	auto *s = OSDynamicCast(OSString, table->getObject(key));
	return s ? s->getCStringNoCopy() : nullptr;
}

// ---- Tahoe's IOPCIDevice slots that MacKernelSDK's header calls reserved (tools/m0/check-vtable.py): free functions with `this` first, SysV ABI ----
constexpr int kSlotConfigRead32 = 272, kSlotConfigRead16 = 273, kSlotConfigRead8 = 274;
constexpr int kSlotConfigWrite32 = 275, kSlotConfigWrite16 = 276, kSlotConfigWrite8 = 277;
constexpr int kSlotConfigureInterrupts = 300, kSlotDeviceMemoryRead = 301, kSlotDeviceMemoryWrite = 302;
constexpr int kVtableSlots = 340;
UInt32 tRead32(RDNA4PvNub *n, UInt8 o) { return n->tahoeConfigRead(32, o); }
UInt16 tRead16(RDNA4PvNub *n, UInt8 o) { return static_cast<UInt16>(n->tahoeConfigRead(16, o)); }
UInt8 tRead8(RDNA4PvNub *n, UInt8 o) { return static_cast<UInt8>(n->tahoeConfigRead(8, o)); }
void tWrite32(RDNA4PvNub *n, UInt8 o, UInt32 v) { n->tahoeConfigWrite(32, o, v); }
void tWrite16(RDNA4PvNub *n, UInt8 o, UInt16 v) { n->tahoeConfigWrite(16, o, v); }
void tWrite8(RDNA4PvNub *n, UInt8 o, UInt8 v) { n->tahoeConfigWrite(8, o, v); }
IOReturn tConfigureInterrupts(RDNA4PvNub *, UInt32, UInt32, UInt32, UInt32) { return kIOReturnSuccess; }
IOReturn tDevRead(RDNA4PvNub *n, UInt8 bar, UInt64 off, void *data, UInt8 size, UInt32) { return n->tahoeDeviceMemoryRead(bar, off, data, size); }
IOReturn tDevWrite(RDNA4PvNub *n, UInt8 bar, UInt64 off, UInt64 data, UInt8 size, UInt32) { return n->tahoeDeviceMemoryWrite(bar, off, data, size); }

} // namespace

// ======================================================================================================================================
// The polling host (M0: watches and consumes, executes nothing) and the self-test, on one work loop.
// ======================================================================================================================================

class RDNA4PvHost : public OSObject {
	OSDeclareDefaultStructors(RDNA4PvHost)
public:
	static RDNA4PvHost *start(RDNA4PvNub *nub, uint32_t level);
	void tick();

private:
	friend class RDNA4PvTestOwner;
	void pollRegisters();
	void pollFifo();
	void unmapFifo();
	void selfTest();

	RDNA4PvNub *mNub { nullptr };
	uint32_t mLevel { 0 };
	IOWorkLoop *mLoop { nullptr };
	IOTimerEventSource *mTimer { nullptr };
	uint32_t mPrev[24] {};
	uint32_t mRegLines { 0 };
	IOMemoryDescriptor *mFifoDesc { nullptr };
	IOMemoryMap *mFifoMap { nullptr };
	uint32_t mFifoPfn { 0 };
	uint32_t mFifoLen { 0 };
	uint64_t mConsumed { 0 };
	uint32_t mPackets { 0 };
	uint64_t mTicks { 0 };

	// self-test state machine (level 2)
	enum Step { kStepIdle, kStepWaitFifo, kStepWaitInterrupt, kStepDone } mStep { kStepIdle };
	uint64_t mStepDeadline { 0 };
	bool mTestOk { true };
	IOBufferMemoryDescriptor *mTestFifo { nullptr };
	IOMemoryMap *mTestBar { nullptr };
	uint64_t mTestExpect { 0 };
	IOWorkLoop *mTestLoop { nullptr };
	IOInterruptEventSource *mTestSource { nullptr };
	OSObject *mTestOwner { nullptr };   // IOEventSource does not retain its owner
public:
	volatile uint32_t mTestIrqSeen { 0 };
	void testCheck(bool ok, const char *what);
};

OSDefineMetaClassAndStructors(RDNA4PvHost, OSObject)

namespace {
RDNA4PvHost *gHost { nullptr };
void hostTick(OSObject *owner, IOTimerEventSource *) {
	if (auto *h = OSDynamicCast(RDNA4PvHost, owner))
		h->tick();
}
uint64_t nowMs() {
	uint64_t abs = mach_absolute_time(), ns = 0;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000000ull;
}
} // namespace

RDNA4PvHost *RDNA4PvHost::start(RDNA4PvNub *nub, uint32_t level) {
	auto *h = OSTypeAlloc(RDNA4PvHost);
	if (!h || !h->init()) {
		OSSafeReleaseNULL(h);
		return nullptr;
	}
	h->mNub = nub;
	h->mLevel = level;
	h->mLoop = IOWorkLoop::workLoop();
	h->mTimer = h->mLoop ? IOTimerEventSource::timerEventSource(h, hostTick) : nullptr;
	if (!h->mTimer || h->mLoop->addEventSource(h->mTimer) != kIOReturnSuccess) {
		pvlog("host: no work loop / timer");
		OSSafeReleaseNULL(h->mTimer);
		OSSafeReleaseNULL(h->mLoop);
		h->release();
		return nullptr;
	}
	h->mTimer->setTimeoutMS(500);   // first tick after the nub had time to register and match
	return h;
}

void RDNA4PvHost::tick() {
	mTicks++;
	pollRegisters();
	pollFifo();
	if (mLevel >= 2)
		selfTest();
	if (mTimer)
		mTimer->setTimeoutMS(mStep == kStepIdle || mStep == kStepDone ? 5 : 2);
}

void RDNA4PvHost::pollRegisters() {
	volatile uint32_t *c = mNub->ctrl();
	if (!c)
		return;
	for (unsigned i = 0; i < 24; i++) {
		// FIFO_WRITTEN / FIFO_READ move with every command: they are reported through the packet log, not as register changes.
		if (i == kRegFifoWritten / 4 || i == kRegFifoRead / 4)
			continue;
		const uint32_t v = c[i];
		if (v != mPrev[i]) {
			if (mRegLines++ < 60)
				pvlog("host: ctrl+0x%03x 0x%08x -> 0x%08x", i * 4, mPrev[i], v);
			mPrev[i] = v;
		}
	}
}

void RDNA4PvHost::unmapFifo() {
	OSSafeReleaseNULL(mFifoMap);
	OSSafeReleaseNULL(mFifoDesc);
	mFifoPfn = 0;
	mFifoLen = 0;
}

void RDNA4PvHost::pollFifo() {
	volatile uint32_t *c = mNub->ctrl();
	if (!c)
		return;
	const uint32_t pfn = c[kRegFifoBasePage / 4], len = c[kRegFifoLength / 4], enabled = c[kRegControlFifo / 4];
	if (!pfn || !len || !enabled) {
		if (mFifoMap)
			unmapFifo();
		return;
	}
	if (pfn != mFifoPfn || len != mFifoLen) {
		unmapFifo();
		if (len < 0x2000 || len > 0x100000) {
			pvlog("host: FIFO length 0x%x refused", len);
			return;
		}
		// The FIFO is guest RAM: the driver announces its physical page number. Map it (an alias of the driver's own mapping).
		mFifoDesc = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(pfn) << 12, len, kIODirectionInOut);
		mFifoMap = mFifoDesc ? mFifoDesc->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
		if (!mFifoMap) {
			pvlog("host: cannot map FIFO page 0x%x (+0x%x)", pfn, len);
			unmapFifo();
			return;
		}
		mFifoPfn = pfn;
		mFifoLen = len;
		mConsumed = c[kRegFifoRead / 4];
		pvlog("host: FIFO announced: page 0x%x (phys 0x%llx), length 0x%x, ring start 0x%x, root page 0x%x, version %u",
		      pfn, static_cast<unsigned long long>(pfn) << 12, len, c[kRegFifoStart / 4], c[kRegRootPage / 4], c[kRegVersion / 4]);
	}
	const uint32_t start = c[kRegFifoStart / 4];
	if (start >= len)
		return;
	const uint32_t ringLen = len - start;
	const uint32_t written = c[kRegFifoWritten / 4], read = c[kRegFifoRead / 4];
	if (written == read)
		return;
	const uint32_t n = written - read;           // counters are monotonic bytes modulo 2^32; the ring position is counter % ringLen
	const uint8_t *ring = reinterpret_cast<const uint8_t *>(mFifoMap->getVirtualAddress()) + start;
	if (n > ringLen) {
		pvlog("host: guest claims %u bytes pending in a %u byte ring (read 0x%x, written 0x%x): resynchronising", n, ringLen, read, written);
		c[kRegFifoRead / 4] = written;
		return;
	}
	if (mPackets++ < 80) {
		uint8_t buf[64];
		const uint32_t take = n < sizeof(buf) ? n : static_cast<uint32_t>(sizeof(buf));
		for (uint32_t i = 0; i < take; i++)
			buf[i] = ring[(read + i) % ringLen];
		char hex[sizeof(buf) * 2 + 1];
		for (uint32_t i = 0; i < take; i++)
			snprintf(hex + i * 2, 3, "%02x", buf[i]);
		pvlog("host: fifo +%u bytes at ring offset 0x%x: %s%s", n, read % ringLen, hex, n > take ? "..." : "");
	}
	mConsumed += n;
	c[kRegFifoRead / 4] = written;   // acknowledge everything: AppleParavirtAccelerator::writeFifo spins until the host has read enough
}

// ---- self-test: plays the guest driver against the nub (the register sequence of AppleParavirtAccelerator, s.6.1) ----

class RDNA4PvTestOwner : public OSObject {
	OSDeclareDefaultStructors(RDNA4PvTestOwner)
public:
	static void action(OSObject *owner, IOInterruptEventSource *, int) {
		if (auto *o = OSDynamicCast(RDNA4PvTestOwner, owner))
			__atomic_add_fetch(&o->host->mTestIrqSeen, 1u, __ATOMIC_RELAXED);
	}
	static RDNA4PvTestOwner *create(RDNA4PvHost *host) {
		auto *o = OSTypeAlloc(RDNA4PvTestOwner);
		if (o && !o->init())
			OSSafeReleaseNULL(o);
		if (o)
			o->host = host;
		return o;
	}
	RDNA4PvHost *host { nullptr };
};
OSDefineMetaClassAndStructors(RDNA4PvTestOwner, OSObject)

void RDNA4PvHost::testCheck(bool ok, const char *what) {
	pvlog("selftest: %-52s %s", what, ok ? "ok" : "FAIL");
	if (!ok)
		mTestOk = false;
}

void RDNA4PvHost::selfTest() {
	switch (mStep) {
	case kStepIdle: {
		if (mTicks < 4)
			return;
		pvlog("selftest: start");
		RDNA4PvNub *n = mNub;
		testCheck(n->configTest(), "config space, capabilities, personality matching");

		// 1. what AppleParavirtGPUControl::setupMMIO does: map register 0x10, control block at +0x1000
		mTestBar = n->mapDeviceMemoryWithRegister(0x10, 0);
		testCheck(mTestBar != nullptr, "mapDeviceMemoryWithRegister(0x10)");
		if (!mTestBar) {
			mStep = kStepDone;
			break;
		}
		volatile uint32_t *c = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *>(mTestBar->getVirtualAddress()) + kCtrl);
		c[0x200 / 4] = 0x600dc0de;   // a marker written through the driver's mapping must be visible through the host's
		testCheck(n->ctrl()[0x200 / 4] == 0x600dc0de, "driver's mapping and the host's pointer are the same RAM");
		c[0x200 / 4] = 0;

		// 2. setupVersion: write 6, read back; the host (here: plain RAM) accepts what was written
		c[kRegVersion / 4] = 6;
		testCheck(c[kRegVersion / 4] == 6, "version handshake: 6 written, 6 read back");

		// 3. setupFIFO: 64 KiB buffer (options 0x890 as Apple's driver), header zeroed, announce page/length/start, enable
		mTestFifo = IOBufferMemoryDescriptor::withOptions(0x890, 0x10000, 1);
		testCheck(mTestFifo && mTestFifo->prepare() == kIOReturnSuccess, "FIFO buffer (0x10000 bytes, options 0x890) allocated and wired");
		if (!mTestFifo) {
			mStep = kStepDone;
			break;
		}
		uint8_t *fifo = static_cast<uint8_t *>(mTestFifo->getBytesNoCopy());
		bzero(fifo, 0x1000);
		const uint64_t phys = mTestFifo->getPhysicalAddress();
		c[kRegFifoBasePage / 4] = static_cast<uint32_t>(phys >> 12);
		c[kRegFifoLength / 4] = static_cast<uint32_t>(mTestFifo->getLength());
		c[kRegFifoStart / 4] = 0x1000;
		c[kRegControlFifo / 4] = 1;

		// 4. writeFifo: two commands, one of them wrapping the ring; then FIFO_WRITTEN
		const uint32_t ringLen = 0x10000 - 0x1000;
		uint32_t written = ringLen - 8;    // start 8 bytes before the end of the ring so the second command wraps
		c[kRegFifoRead / 4] = written;     // (the host's view of where it stands; the poller resynchronises to it on the first announcement)
		mConsumed = written;
		static const uint8_t cmd1[16] = { 0x10, 0, 0, 0, 0xde, 0xc0, 0xad, 0xde, 1, 2, 3, 4, 5, 6, 7, 8 };
		for (uint32_t i = 0; i < sizeof(cmd1); i++)
			fifo[0x1000 + (written + i) % ringLen] = cmd1[i];
		written += sizeof(cmd1);
		c[kRegFifoWritten / 4] = written;
		mTestExpect = written;
		mStep = kStepWaitFifo;
		mStepDeadline = nowMs() + 300;
		break;
	}
	case kStepWaitFifo: {
		volatile uint32_t *c = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *>(mTestBar->getVirtualAddress()) + kCtrl);
		const bool consumed = c[kRegFifoRead / 4] == c[kRegFifoWritten / 4];
		if (!consumed && nowMs() < mStepDeadline)
			return;
		testCheck(consumed, "host consumed the FIFO (FIFO_READ caught up with FIFO_WRITTEN)");
		testCheck(mFifoMap != nullptr && mFifoPfn == c[kRegFifoBasePage / 4], "host mapped the announced FIFO page");
		testCheck(mPackets >= 1, "host logged at least one packet");

		// 5. setupInterrupts: an IOInterruptEventSource with a block handler on the provider, index 0, as Apple's driver builds it
		mTestOwner = RDNA4PvTestOwner::create(this);
		mTestLoop = IOWorkLoop::workLoop();
		if (mTestOwner && mTestLoop) {
			mTestSource = IOInterruptEventSource::interruptEventSource(mTestOwner, RDNA4PvTestOwner::action, mNub, 0);
			if (mTestSource && mTestLoop->addEventSource(mTestSource) == kIOReturnSuccess)
				mTestSource->enable();
			else
				OSSafeReleaseNULL(mTestSource);
		}
		testCheck(mTestSource != nullptr, "IOInterruptEventSource on the nub (index 0)");
		mTestIrqSeen = 0;
		testCheck(mTestSource && mNub->raiseInterrupt(0), "raiseInterrupt(0): handler registered and enabled");
		mStep = kStepWaitInterrupt;
		mStepDeadline = nowMs() + 300;
		break;
	}
	case kStepWaitInterrupt: {
		if (!mTestIrqSeen && nowMs() < mStepDeadline)
			return;
		testCheck(mTestIrqSeen == 1, "interrupt delivered once to the event source's action");
		// teardown, in the order Apple's driver would: interrupts, FIFO registers, buffers
		if (mTestSource) {
			mTestSource->disable();
			mTestLoop->removeEventSource(mTestSource);
			OSSafeReleaseNULL(mTestSource);
		}
		OSSafeReleaseNULL(mTestLoop);
		OSSafeReleaseNULL(mTestOwner);
		volatile uint32_t *c = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *>(mTestBar->getVirtualAddress()) + kCtrl);
		c[kRegControlFifo / 4] = 0;
		c[kRegFifoBasePage / 4] = 0;
		c[kRegFifoLength / 4] = 0;
		c[kRegFifoWritten / 4] = 0;
		c[kRegFifoRead / 4] = 0;
		c[kRegVersion / 4] = 0;
		mStep = kStepDone;
		mStepDeadline = nowMs() + 30;   // let the poller drop its mapping before the pages go back
		break;
	}
	case kStepDone: {
		if (nowMs() >= mStepDeadline) {
			if (mTestFifo) {
				testCheck(mFifoMap == nullptr, "host dropped its FIFO mapping when the guest cleared it");
				mTestFifo->complete();
				OSSafeReleaseNULL(mTestFifo);
			}
			OSSafeReleaseNULL(mTestBar);
			pvlog("selftest: %s", mTestOk ? "PASS" : "FAIL");
			mLevel = 1;   // done: keep polling, stop testing
		}
		break;
	}
	}
}

// ======================================================================================================================================
// The nub
// ======================================================================================================================================

bool RDNA4PvNub::build() {
	mLock = IOLockAlloc();
	mBar0Desc = IOBufferMemoryDescriptor::withCapacity(kBar0Size, kIODirectionInOut, false);
	if (!mLock || !mBar0Desc || mBar0Desc->prepare() != kIOReturnSuccess)
		return false;
	mBar0 = mBar0Desc->getBytesNoCopy();
	bzero(mBar0, kBar0Size);

	// Config space of a display controller function with one memory BAR and an MSI capability.
	auto put32 = [this](unsigned off, uint32_t v) { memcpy(&mConfig[off], &v, sizeof(v)); };
	put32(0x00, (static_cast<uint32_t>(kDevice) << 16) | kVendor);
	put32(0x04, 0x00100000);          // command 0, status: capabilities list
	put32(0x08, 0x03000001);          // class 03 (display), subclass 00, prog-if 00, revision 1
	put32(0x0c, 0x00000000);          // header type 0
	put32(0x10, 0xe0000000);          // BAR0: 32-bit memory, 16 KiB (never decoded: the RAM stands in for it)
	put32(0x2c, (0x0000u << 16) | kVendor);   // subsystem 106b:0000
	put32(0x34, 0x00000050);          // capabilities pointer
	put32(0x3c, 0x000001ff);          // interrupt pin A
	put32(0x50, 0x00800005);          // MSI capability, 64-bit capable, no next

	auto data = [this](const char *key, const void *bytes, unsigned len) {
		if (auto *d = OSData::withBytes(bytes, len)) {
			setProperty(key, d);
			d->release();
		}
	};
	data("vendor-id", "\x6b\x10\x00\x00", 4);
	data("device-id", "\xee\xee\x00\x00", 4);
	data("revision-id", "\x01\x00\x00\x00", 4);
	data("subsystem-vendor-id", "\x6b\x10\x00\x00", 4);
	data("subsystem-id", "\x00\x00\x00\x00", 4);
	data("class-code", "\x00\x00\x03\x00", 4);
	static const char kCompatible[] = "pci106b,eeee\0pciclass,030000\0pciclass,0300";
	data("compatible", kCompatible, sizeof(kCompatible));
	setProperty("RDNA4PvGpu", "fake Apple paravirtual GPU (106b:eeee), BAR0 in RAM, M0 of docs/metal-phase-plan.md");
	setName("PVGPU");
	return true;
}

void RDNA4PvNub::patchVtable() {
	void **orig = *reinterpret_cast<void ***>(this);
	void **copy = static_cast<void **>(IOMalloc(kVtableSlots * sizeof(void *)));
	if (!copy) {
		pvlog("vtable copy failed: the Tahoe-only IOPCIDevice slots keep their real (bridge-dependent) implementations");
		return;
	}
	memcpy(copy, orig, kVtableSlots * sizeof(void *));
	copy[kSlotConfigRead32] = reinterpret_cast<void *>(&tRead32);
	copy[kSlotConfigRead16] = reinterpret_cast<void *>(&tRead16);
	copy[kSlotConfigRead8] = reinterpret_cast<void *>(&tRead8);
	copy[kSlotConfigWrite32] = reinterpret_cast<void *>(&tWrite32);
	copy[kSlotConfigWrite16] = reinterpret_cast<void *>(&tWrite16);
	copy[kSlotConfigWrite8] = reinterpret_cast<void *>(&tWrite8);
	copy[kSlotConfigureInterrupts] = reinterpret_cast<void *>(&tConfigureInterrupts);
	copy[kSlotDeviceMemoryRead] = reinterpret_cast<void *>(&tDevRead);
	copy[kSlotDeviceMemoryWrite] = reinterpret_cast<void *>(&tDevWrite);
	*reinterpret_cast<void ***>(this) = copy;   // the nub is never released, so the copy is never orphaned
}

bool RDNA4PvNub::publish(IOService *parent, uint32_t level) {
	if (gNub || !parent || !level)
		return false;
	// The nub appends data members to IOPCIDevice's layout as MacKernelSDK's header knows it. If Tahoe's real class is bigger, those would overlap its
	// own members: refuse.
	const OSSymbol *realName = OSSymbol::withCStringNoCopy("IOPCIDevice");
	const OSMetaClass *real = realName ? OSMetaClass::getMetaClassWithName(realName) : nullptr;
	OSSafeReleaseNULL(realName);
	const size_t realSize = real ? real->getClassSize() : 0;
	if (!real || realSize > sizeof(IOPCIDevice)) {
		pvlog("refused: IOPCIDevice is %zu bytes in this macOS, MacKernelSDK's header says %zu", realSize, sizeof(IOPCIDevice));
		return false;
	}
	auto *nub = OSTypeAlloc(RDNA4PvNub);
	if (!nub)
		return false;
	if (!nub->init(static_cast<OSDictionary *>(nullptr)) || !nub->build()) {
		pvlog("could not build the nub");
		nub->release();
		return false;
	}
	nub->patchVtable();
	if (!nub->attach(parent)) {
		pvlog("attach to %s failed", parent->getName());
		nub->release();
		return false;
	}
	gNub = nub;   // keeps the creation reference: never released
	nub->registerService();
	pvlog("fake Apple paravirtual GPU published under %s: PCI %04x:%04x, BAR0 %u bytes of RAM (control block at +0x%x), level %u; IOPCIDevice is %zu bytes here (header %zu)",
	      parent->getName(), kVendor, kDevice, kBar0Size, kCtrl, level, realSize, sizeof(IOPCIDevice));
	gHost = RDNA4PvHost::start(nub, level);
	if (!gHost)
		pvlog("polling host did not start (the nub stays published)");
	return true;
}

class RDNA4PvLate : public OSObject {
	OSDeclareDefaultStructors(RDNA4PvLate)
public:
	static void schedule(uint32_t level, uint32_t delayMs) {
		auto *l = OSTypeAlloc(RDNA4PvLate);
		if (!l || !l->init()) {
			OSSafeReleaseNULL(l);
			return;
		}
		l->mLevel = level;
		l->mDeadline = nowMs() + delayMs;
		l->mLoop = IOWorkLoop::workLoop();
		l->mTimer = l->mLoop ? IOTimerEventSource::timerEventSource(l, tickAction) : nullptr;
		if (!l->mTimer || l->mLoop->addEventSource(l->mTimer) != kIOReturnSuccess) {
			OSSafeReleaseNULL(l->mTimer);
			OSSafeReleaseNULL(l->mLoop);
			l->release();
			return;
		}
		l->mTimer->setTimeoutMS(500);   // kept alive by the creation reference: one per boot, never released
	}
private:
	static void tickAction(OSObject *owner, IOTimerEventSource *) {
		if (auto *l = OSDynamicCast(RDNA4PvLate, owner))
			l->tick();
	}
	void tick() {
		if (gNub)
			return;   // the GPU path got there first
		IOService *platform = nowMs() >= mDeadline ? IOService::getPlatform() : nullptr;
		if (platform) {
			pvlog("no GPU-side publish after the delay: publishing under the platform expert (%s)", platform->getName());
			RDNA4PvNub::publish(platform, mLevel);
			return;
		}
		mTimer->setTimeoutMS(500);
	}
	uint32_t mLevel { 0 };
	uint64_t mDeadline { 0 };
	IOWorkLoop *mLoop { nullptr };
	IOTimerEventSource *mTimer { nullptr };
};
OSDefineMetaClassAndStructors(RDNA4PvLate, OSObject)

void RDNA4PvNub::publishLater(uint32_t level, uint32_t delayMs) { RDNA4PvLate::schedule(level, delayMs); }

bool RDNA4PvNub::raiseInterrupt(int source) {
	if (source < 0 || source >= kMaxInterrupts)
		return false;
	IOInterruptAction h;
	OSObject *t;
	void *r;
	IOLockLock(mLock);
	const bool ok = mInt[source].registered && mInt[source].enabled;
	h = mInt[source].handler;
	t = mInt[source].target;
	r = mInt[source].refCon;
	IOLockUnlock(mLock);
	if (ok && h)
		h(t, r, this, source);
	return ok;
}

bool RDNA4PvNub::configTest() {
	bool ok = true;
	IOPCIAddressSpace sp {};
	ok &= configRead32(sp, 0) == ((static_cast<uint32_t>(kDevice) << 16) | kVendor);
	UInt8 off = 0;
	ok &= findPCICapability(5, &off) != 0 && off == 0x50;
	ok &= findPCICapability(0x10, &off) == 0;
	configWrite16(sp, 0x04, 0x0006);
	ok &= (configRead16(sp, 0x04) & 0x0006) == 0x0006;
	// Personality matching: Apple's table matches, others do not.
	auto *t = OSDictionary::withCapacity(2);
	bool matched = false, notMatched = true;
	if (t) {
		auto *good = OSString::withCString("0xEEEE106B");
		t->setObject("IOPCIMatch", good);
		SInt32 score = 0;
		matched = matchPropertyTable(t, &score);
		OSSafeReleaseNULL(good);
		auto *bad = OSString::withCString("0x12341234 0x56785678&0xffff0000");
		t->setObject("IOPCIMatch", bad);
		notMatched = !matchPropertyTable(t, &score);
		OSSafeReleaseNULL(bad);
		t->removeObject("IOPCIMatch");
		notMatched &= !matchPropertyTable(t, &score);   // no PCI key at all: not ours
		t->release();
	}
	return ok && matched && notMatched;
}

// ---- lifecycle: the IOService behaviour, none of IOPCIDevice's bridge-dependent code ----

bool RDNA4PvNub::init(OSDictionary *propTable) { return IOService::init(propTable); }
bool RDNA4PvNub::init(IORegistryEntry *from, const IORegistryPlane *plane) { return IOService::init(from, plane); }
void RDNA4PvNub::free() {
	if (mBar0Desc) {
		mBar0Desc->complete();
		OSSafeReleaseNULL(mBar0Desc);
	}
	if (mLock) {
		IOLockFree(mLock);
		mLock = nullptr;
	}
	IOService::free();
}
bool RDNA4PvNub::attach(IOService *provider) { return IOService::attach(provider); }
void RDNA4PvNub::detach(IOService *provider) { IOService::detach(provider); }
void RDNA4PvNub::detachAbove(const IORegistryPlane *plane) { IOService::detachAbove(plane); }
IOReturn RDNA4PvNub::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) {
	return IOService::newUserClient(owningTask, securityID, type, properties, handler);
}
bool RDNA4PvNub::handleOpen(IOService *forClient, IOOptionBits options, void *arg) { return IOService::handleOpen(forClient, options, arg); }
void RDNA4PvNub::handleClose(IOService *forClient, IOOptionBits options) { IOService::handleClose(forClient, options); }
IOReturn RDNA4PvNub::requestProbe(IOOptionBits) { return kIOReturnUnsupported; }
IOReturn RDNA4PvNub::powerStateWillChangeTo(IOPMPowerFlags, unsigned long, IOService *) { return IOPMAckImplied; }
IOReturn RDNA4PvNub::setPowerState(unsigned long, IOService *) { return IOPMAckImplied; }
unsigned long RDNA4PvNub::maxCapabilityForDomainState(IOPMPowerFlags) { return 0; }
unsigned long RDNA4PvNub::initialPowerStateForDomainState(IOPMPowerFlags) { return 0; }
unsigned long RDNA4PvNub::powerStateForDomainState(IOPMPowerFlags) { return 0; }
bool RDNA4PvNub::compareName(OSString *name, OSString **matched) const { return IOService::compareName(name, matched); }
IOService *RDNA4PvNub::matchLocation(IOService *client) { return IOService::matchLocation(client); }
IOReturn RDNA4PvNub::getResources() { return IOService::getResources(); }
IOReturn RDNA4PvNub::setProperties(OSObject *properties) { return IOService::setProperties(properties); }
IOReturn RDNA4PvNub::callPlatformFunction(const OSSymbol *f, bool w, void *p1, void *p2, void *p3, void *p4) {
	return IOService::callPlatformFunction(f, w, p1, p2, p3, p4);
}
IOReturn RDNA4PvNub::callPlatformFunction(const char *f, bool w, void *p1, void *p2, void *p3, void *p4) {
	return IOService::callPlatformFunction(f, w, p1, p2, p3, p4);
}
IODeviceMemory *RDNA4PvNub::getDeviceMemoryWithIndex(unsigned int) { return nullptr; }

// ---- matching: only a personality that names this device by IOPCIMatch / IOPCIPrimaryMatch matches; everything else is offered and refused ----

bool RDNA4PvNub::matchPropertyTable(OSDictionary *table) {
	SInt32 score = 0;
	return matchPropertyTable(table, &score);
}

bool RDNA4PvNub::matchPropertyTable(OSDictionary *table, SInt32 *score) {
	if (!table || !IOService::matchPropertyTable(table, score))
		return false;
	const uint32_t ids = (static_cast<uint32_t>(kDevice) << 16) | kVendor;
	const uint32_t sub = (0x0000u << 16) | kVendor;
	const char *primary = stringKey(table, "IOPCIMatch");
	if (!primary)
		primary = stringKey(table, "IOPCIPrimaryMatch");
	const char *secondary = stringKey(table, "IOPCISecondaryMatch");
	const char *klass = stringKey(table, "IOPCIClassMatch");
	bool ok = primary != nullptr && matchIds(primary, ids);
	if (ok && secondary && !matchIds(secondary, sub))
		ok = false;
	if (ok && klass && !matchIds(klass, 0x03000000))
		ok = false;
	static uint32_t offered = 0;
	if (offered++ < 40) {
		const char *cls = stringKey(table, "IOClass"), *bundle = stringKey(table, "CFBundleIdentifier");
		pvlog("match offered: %s %s (IOPCIMatch %s) -> %s", bundle ? bundle : "?", cls ? cls : "?", primary ? primary : "-", ok ? "MATCH" : "no");
	}
	return ok;
}

// ---- config space ----

UInt32 RDNA4PvNub::configRead32(IOPCIAddressSpace, UInt8 offset) {
	uint32_t v = 0;
	memcpy(&v, &mConfig[offset & 0xfc], sizeof(v));
	return v;
}
UInt16 RDNA4PvNub::configRead16(IOPCIAddressSpace, UInt8 offset) {
	uint16_t v = 0;
	memcpy(&v, &mConfig[offset & 0xfe], sizeof(v));
	return v;
}
UInt8 RDNA4PvNub::configRead8(IOPCIAddressSpace, UInt8 offset) { return mConfig[offset]; }
void RDNA4PvNub::configWrite32(IOPCIAddressSpace, UInt8 offset, UInt32 data) {
	if (offset >= 0x04)   // vendor/device are read-only
		memcpy(&mConfig[offset & 0xfc], &data, sizeof(data));
}
void RDNA4PvNub::configWrite16(IOPCIAddressSpace, UInt8 offset, UInt16 data) {
	if (offset >= 0x04)
		memcpy(&mConfig[offset & 0xfe], &data, sizeof(data));
}
void RDNA4PvNub::configWrite8(IOPCIAddressSpace, UInt8 offset, UInt8 data) {
	if (offset >= 0x04)
		mConfig[offset] = data;
}
UInt32 RDNA4PvNub::tahoeConfigRead(UInt32 width, UInt8 offset) {
	IOPCIAddressSpace sp {};
	return width == 32 ? configRead32(sp, offset) : width == 16 ? configRead16(sp, offset) : configRead8(sp, offset);
}
void RDNA4PvNub::tahoeConfigWrite(UInt32 width, UInt8 offset, UInt32 data) {
	IOPCIAddressSpace sp {};
	if (width == 32)
		configWrite32(sp, offset, data);
	else if (width == 16)
		configWrite16(sp, offset, static_cast<UInt16>(data));
	else
		configWrite8(sp, offset, static_cast<UInt8>(data));
}
IOReturn RDNA4PvNub::tahoeDeviceMemoryRead(UInt8, UInt64 offset, void *data, UInt8 size) {
	if (!mBar0 || !data || !size || offset + size > kBar0Size)
		return kIOReturnBadArgument;
	memcpy(data, static_cast<uint8_t *>(mBar0) + offset, size);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::tahoeDeviceMemoryWrite(UInt8, UInt64 offset, UInt64 data, UInt8 size) {
	if (!mBar0 || !size || size > 8 || offset + size > kBar0Size)
		return kIOReturnBadArgument;
	memcpy(static_cast<uint8_t *>(mBar0) + offset, &data, size);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::saveDeviceState(IOOptionBits) { return kIOReturnSuccess; }
IOReturn RDNA4PvNub::restoreDeviceState(IOOptionBits) { return kIOReturnSuccess; }
UInt32 RDNA4PvNub::setConfigBits(UInt8 offset, UInt32 mask, UInt32 value) {
	IOPCIAddressSpace sp {};
	const UInt32 old = configRead32(sp, offset);
	configWrite32(sp, offset, (old & ~mask) | (value & mask));
	return old;
}
bool RDNA4PvNub::setMemoryEnable(bool enable) {
	IOPCIAddressSpace sp {};
	const UInt16 cmd = configRead16(sp, 0x04);
	configWrite16(sp, 0x04, enable ? (cmd | 0x2) : (cmd & ~0x2));
	return (cmd & 0x2) != 0;
}
bool RDNA4PvNub::setIOEnable(bool enable, bool) {
	IOPCIAddressSpace sp {};
	const UInt16 cmd = configRead16(sp, 0x04);
	configWrite16(sp, 0x04, enable ? (cmd | 0x1) : (cmd & ~0x1));
	return (cmd & 0x1) != 0;
}
bool RDNA4PvNub::setBusMasterEnable(bool enable) {
	IOPCIAddressSpace sp {};
	const UInt16 cmd = configRead16(sp, 0x04);
	configWrite16(sp, 0x04, enable ? (cmd | 0x4) : (cmd & ~0x4));
	return (cmd & 0x4) != 0;
}
UInt32 RDNA4PvNub::findPCICapability(UInt8 capabilityID, UInt8 *offset) {
	UInt8 at = mConfig[0x34] & 0xfc;
	if (offset && *offset)   // continue after the previous hit
		at = mConfig[(*offset) + 1] & 0xfc;
	for (int guard = 0; at && guard < 48; guard++) {
		if (mConfig[at] == capabilityID) {
			if (offset)
				*offset = at;
			uint32_t v = 0;
			memcpy(&v, &mConfig[at], sizeof(v));
			return v;
		}
		at = mConfig[at + 1] & 0xfc;
	}
	return 0;
}
UInt32 RDNA4PvNub::extendedFindPCICapability(UInt32 capabilityID, IOByteCount *offset) {
	if (capabilityID >= 0x80000000u || capabilityID > 0xff)
		return 0;   // no extended capabilities
	UInt8 o = offset ? static_cast<UInt8>(*offset) : 0;
	const UInt32 r = findPCICapability(static_cast<UInt8>(capabilityID), &o);
	if (offset)
		*offset = o;
	return r;
}
UInt8 RDNA4PvNub::getBusNumber() { return 0; }
UInt8 RDNA4PvNub::getDeviceNumber() { return 0; }
UInt8 RDNA4PvNub::getFunctionNumber() { return 0; }
IODeviceMemory *RDNA4PvNub::getDeviceMemoryWithRegister(UInt8) { return nullptr; }
IOMemoryMap *RDNA4PvNub::mapDeviceMemoryWithRegister(UInt8 reg, IOOptionBits) {
	if (reg != 0x10 || !mBar0Desc)
		return nullptr;
	// An alias of the RAM the host polls: a second mapping of the same pages, retained for the caller.
	return mBar0Desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
}
IODeviceMemory *RDNA4PvNub::ioDeviceMemory() { return nullptr; }
void RDNA4PvNub::ioWrite32(UInt16, UInt32, IOMemoryMap *) {}
void RDNA4PvNub::ioWrite16(UInt16, UInt16, IOMemoryMap *) {}
void RDNA4PvNub::ioWrite8(UInt16, UInt8, IOMemoryMap *) {}
UInt32 RDNA4PvNub::ioRead32(UInt16, IOMemoryMap *) { return 0xffffffff; }
UInt16 RDNA4PvNub::ioRead16(UInt16, IOMemoryMap *) { return 0xffff; }
UInt8 RDNA4PvNub::ioRead8(UInt16, IOMemoryMap *) { return 0xff; }
bool RDNA4PvNub::hasPCIPowerManagement(IOOptionBits) { return false; }
IOReturn RDNA4PvNub::enablePCIPowerManagement(IOOptionBits) { return kIOReturnUnsupported; }

// ---- interrupts: the nub is its own interrupt controller; the host side calls the handler ----

IOReturn RDNA4PvNub::registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refCon) {
	if (source < 0 || source >= kMaxInterrupts || !handler)
		return kIOReturnBadArgument;
	IOLockLock(mLock);
	const bool busy = mInt[source].registered;
	if (!busy) {
		mInt[source].target = target;
		mInt[source].handler = handler;
		mInt[source].refCon = refCon;
		mInt[source].registered = true;
		mInt[source].enabled = false;
	}
	IOLockUnlock(mLock);
	pvlog("interrupt %d registered%s", source, busy ? " (refused: already registered)" : "");
	return busy ? kIOReturnNoResources : kIOReturnSuccess;
}
IOReturn RDNA4PvNub::unregisterInterrupt(int source) {
	if (source < 0 || source >= kMaxInterrupts)
		return kIOReturnBadArgument;
	IOLockLock(mLock);
	mInt[source] = Interrupt();
	IOLockUnlock(mLock);
	pvlog("interrupt %d unregistered", source);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::getInterruptType(int source, int *type) {
	if (source < 0 || source >= kMaxInterrupts || !type)
		return kIOReturnBadArgument;
	*type = kIOInterruptTypeEdge;   // MSI
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::enableInterrupt(int source) {
	if (source < 0 || source >= kMaxInterrupts)
		return kIOReturnBadArgument;
	IOLockLock(mLock);
	mInt[source].enabled = true;
	IOLockUnlock(mLock);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::disableInterrupt(int source) {
	if (source < 0 || source >= kMaxInterrupts)
		return kIOReturnBadArgument;
	IOLockLock(mLock);
	mInt[source].enabled = false;
	IOLockUnlock(mLock);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::causeInterrupt(int source) { return raiseInterrupt(source) ? kIOReturnSuccess : kIOReturnNotReady; }
