//
//  pvgpu.cpp
//  AMDRadeonRDNA4
//
//  See pvgpu.hpp and docs/m0-pvgpu.md.
//

#include "pvgpu.hpp"
#include "pvstream.hpp"
#include "compute.hpp"
#include "mtlkernels.h"
#include "../userspace/rdna4-run/gfx12tri.h"     // M4: the triangle stream the host's draws are built from
#include "../userspace/rdna4-run/gfx12trirun.h"
#include "rdna4compute.h"
#include "rdna4mtlc.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

OSDefineMetaClassAndStructors(RDNA4PvNub, IOPCIDevice)

using namespace pvgpu;

namespace {

constexpr uint32_t kLoggedCmds = 1500;    // FIFO commands logged one by one
constexpr uint32_t kMaxLogLines = 4000;   // M1 reads the protocol from this log (docs/m1-stream.md)
// Compute units of the card behind GpuCoreCount: RX 9070 XT (Navi 48) has 64. From the product specification, not read from the card (the kext's IP discovery
// exposes shader-engine and render-backend counts only).
constexpr uint32_t kGpuCoreCount = 64;
uint32_t gLogLines { 0 };
RDNA4PvNub *gNub { nullptr };
IOService *gLateParent { nullptr };   // RDNA4PvNub::setLateParent
RDNA4Compute *gCompute { nullptr };   // RDNA4PvNub::setCompute

// M3: the one request slot between the host and the translator helper (include/rdna4mtlc.h). The host posts a function (id != 0) and
// waits on &done; the helper's kMtlcNext waits on &id, copies the metallib out and sets taken; kMtlcDeliver fills elf and sets done.
struct Translation {
	IOLock *lock;                 // allocated with the host (RDNA4PvHost::start)
	bool helper;                  // a helper is connected
	uint32_t seq, id;
	bool taken, done;
	const uint8_t *lib;
	uint32_t libLen;
	char name[64];
	uint8_t *elf;                 // IOMalloc'd by kMtlcDeliver, freed by the host
	uint32_t elfLen, buffers, written;
} gTr;

void pvlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void pvlog(const char *fmt, ...) {
	const uint32_t n = __atomic_fetch_add(&gLogLines, 1u, __ATOMIC_RELAXED);
	if (n >= kMaxLogLines) {
		if (n == kMaxLogLines)
			IOLog("AMDRadeonRDNA4: pvgpu: log budget (%u lines) used up\n", kMaxLogLines);
		return;
	}
	char buf[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	IOLog("AMDRadeonRDNA4: pvgpu: %s\n", buf);
}

// The first three calls of each of the nub's methods: what macOS and Apple's driver do with the nub, in order (the M0 runs on the rig, 2026-10-15/21).
// With rdna4-steptrace=<ms> each of those lines is followed by a pause, so that freeze-watch.sh (a write every 0.5 s) keeps the last one before a stop.
void pvPause() {
	uint32_t ms = 0;
	if (PE_parse_boot_argn("rdna4-steptrace", &ms, sizeof(ms)) && ms)
		IOSleep(ms);
}
#define PVCALL(name) do { static uint32_t calls_; if (__atomic_fetch_add(&calls_, 1u, __ATOMIC_RELAXED) < 3) { pvlog("call: %s", name); pvPause(); } } while (0)

// The PCI class code the nub reports (rdna4-pvgpu-class=<24 bits>, default 0x030000 = display controller). Apple's driver matches by IOPCIMatch alone.
// On the rig AppleGPUWrangler took the nub with 0x030000 and with 0xff0000 alike (it follows the accelerator, not the class).
uint32_t pvClass() {
	uint32_t c = 0x030000;
	PE_parse_boot_argn("rdna4-pvgpu-class", &c, sizeof(c));
	return c & 0xffffff;
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

// ---- Tahoe's IOPCIDevice slots that MacKernelSDK's header calls reserved (tools/metal/m0/check-vtable.py): free functions with `this` first, SysV ABI ----
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
	// A source of FIFO commands: the root FIFO ring (channel 0, stamp 0) or the ring of a child channel.
	struct CmdSource {
		const char *tag;
		const uint8_t *ring;      // contiguous mapping of the ring
		uint32_t ringLen;
		uint32_t stampIndex;      // stamp slot the completions of this source go to
		uint32_t channel;         // 0 = root FIFO
	};
	void consume(const CmdSource &src, uint32_t read, uint32_t n);
	void dumpPayload(const char *tag, const pvstream::FifoCommand &c, const uint8_t *cmd);
	void dumpPage(const char *what, uint32_t pfn);
	uint32_t guestPfn(uint32_t task, uint64_t va);
	void dumpGuest(uint32_t task, uint64_t va, uint32_t len);
	bool copyGuest(uint32_t task, uint64_t va, uint8_t *buf, uint32_t len, bool toGuest);
	bool readGuest(uint32_t task, uint64_t va, uint8_t *dst, uint32_t len) { return copyGuest(task, va, dst, len, false); }
	void decodeExec(uint32_t task, uint64_t va, uint64_t len);
	void dmaSelfTest();
	bool functionOf(uint32_t task, uint32_t pipeline, char *name, size_t nameCap, uint8_t hash[32], uint64_t *lib);
	bool functionName(uint32_t task, uint32_t fn, char *name, size_t nameCap, uint8_t hash[32], uint64_t *lib);
	uint32_t resourceType(uint32_t task, uint32_t id, uint64_t *desc = nullptr, uint32_t *bytes = nullptr);
	uint32_t load(const uint8_t *elf, uint32_t size, const char *name);
	uint32_t program(unsigned kernel);
	struct Jit {
		uint8_t hash[32];
		uint32_t program, buffers, written;   // a compute kernel loaded into the runtime
		uint8_t *gfx;                         // or a vertex or fragment shader: the helper's RGFX blob (air2amdgpu.cpp translateShader)
		uint32_t gfxLen;
	};
	const Jit *translated(uint32_t task, uint64_t lib, const char *name, const uint8_t hash[32]);
	bool bufferRange(uint32_t task, uint32_t id, uint64_t *va, uint64_t *len);
	bool textureRange(uint32_t task, uint32_t id, uint64_t *va, uint64_t *len, uint32_t *bytesPerRow = nullptr, uint32_t *width = nullptr);
	bool dmaGuest(uint32_t task, uint64_t dstVa, uint64_t srcVa, uint32_t pattern, uint64_t bytes);
public:
	void runBlit(uint32_t task, uint32_t id, const uint8_t *p, uint32_t bytes);
	uint32_t runRenderPass(uint32_t task, const uint8_t *p, uint32_t bytes);
	void runDraw(uint32_t task, uint32_t target, uint32_t pipeline, uint32_t type, uint32_t start, uint32_t count, uint32_t instances);
	void runCompute(uint32_t task, uint32_t pipeline, const uint32_t *buffer, const uint64_t *offset, const uint64_t *size);
	void describeRenderPipeline(uint32_t task, uint32_t pipeline);
private:
	void answerDeviceInfo(const pvstream::FifoCommand &c, const uint8_t *cmd);
	void answerDisplaySharedState(const pvstream::FifoCommand &c, const uint8_t *cmd, uint32_t channel);
	void answerDisplayOnline(const pvstream::FifoCommand &c, const uint8_t *cmd);
	void completeStamp(uint32_t index, uint32_t value);
	// display pipes (docs/m1-stream.md s.1b): the shared-state pages the guest announced, the events the host puts into them
	void dropPipes();
	void dropPipe(uint32_t port, const char *why);
	bool pipeStillOurs(uint32_t port);
	void pollDisplay();
	void unmapFifo();
	// child channels (docs/m1-stream.md s.1): DefineChannel -> state record in the root page -> page-list ring
	void mapRoot();
	void defineChannel(uint32_t n);
	void freeChannel(uint32_t n);
	void dropChannels();
	void pollChannels();
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
	uint32_t mIrqBits { 0 };           // INTR_STATUS_GPU bits we raised and have not cleared yet
	uint32_t mReplies { 0 };           // GetDeviceInfo replies sent
	uint64_t mTicks { 0 };
	uint32_t mCmds { 0 };               // FIFO commands decoded so far
	uint32_t mBadHeaders { 0 };
	uint8_t mDumped[256] {};            // M1: payload dumps per FIFO id so far
	uint32_t mTaskRoot[64] {};          // M1: page-table root PFN per task id (DefineHostTask), for the dumps after a commit
	bool mRtOpen { false };              // M2: this host is a client of the card's runtime
	uint32_t mProgram[kMtlKernelCount] {};   // M3: kMtlKernels loaded into it, 0 = not yet
	uint32_t mFnLines { 0 };              // M3: pipeline -> function lines so far
	uint32_t mRenderPipeSeen[16] {};
	struct GfxBuffers { uint64_t handle, gpu; } mGfxCode {}, mGfxRings {}, mGfxMisc {}, mGfxIb {};   // M4: kept for the boot once made      // M4: (task << 16) | pipeline of the render pipelines described so far
	Jit mJit[32] {};                     // M3: functions the helper translated, by hash (program 0: it could not); oldest replaced first
	uint32_t mJits { 0 };
	uint32_t mRenderLines { 0 };          // M2: render-pass and texture lines so far (other processes render every frame)
	uint8_t mWin[1024] {};              // one FIFO command copied out of the ring for decoding

	// Display side. rdna4-pvgpu-disp: bit 0 = report the display online (fill the pipe's info, set the online event), bit 1 = ~60 Hz VBL events for pipes whose
	// guest enabled VBL; default 2, the self-test runs with 3. rdna4-pvgpu-width / -height set the mode (default 1920x1080).
	static constexpr uint32_t kDispOnline = 1, kDispVbl = 2;
	static constexpr uint64_t kVblPeriodNs = 16666667;
	struct Pipe {
		bool live { false };
		IOMemoryDescriptor *desc { nullptr };
		IOMemoryMap *map { nullptr };
		uint32_t page { 0 };
		uint32_t channel { 0 };             // the channel the pipe's DisplaySetupSharedState arrived on (0 = root FIFO): the pipe goes when it does
		uint32_t lastEnabled { 0 };
		uint32_t cursorPos { 0xffffffff };
		uint8_t cursorVisible { 0 };
		uint32_t cursorLines { 0 };
	};
	Pipe mPipe[pvstream::disp::kMaxPorts];
	uint32_t mDisp { kDispVbl };
	pvstream::disp::Info mDispInfo;
	uint32_t mDispBits { 0 };           // INTR_STATUS_DISP bits asserted: level-triggered, dropped once the guest took the pending event
	uint64_t mNextVblNs { 0 };
	uint64_t mLastDispIrqNs { 0 };
	uint32_t mVblFrames { 0 };          // VBL events set (frames due while some pipe had VBL enabled)
	uint32_t mOnlineEvents { 0 }, mOnlineAcks { 0 };
	uint32_t mTransactions { 0 }, mCursorCmds { 0 }, mFlushes { 0 };
	uint32_t mDispCmdLines { 0 };
	uint32_t mKickPrev { 0 };

	// Guest-supplied sizes are validated before anything is mapped: at most kMaxChannels channels, rings of at most kMaxRingPages pages.
	static constexpr uint32_t kMaxChannels = 16;
	static constexpr uint32_t kMaxRingPages = 256;
	static constexpr uint32_t kRecordOffset = 0x400, kRecordBytes = 20;   // channel n's state record: root page + 0x400 + (n - 1) * 20
	struct Channel {
		bool live { false };
		uint32_t kind { 0 };               // the record's u16 at +0xc: the channel's stamp index
		uint32_t ringBytes { 0 };
		IOMemoryDescriptor *desc { nullptr };
		IOMemoryMap *map { nullptr };
	};
	Channel mCh[kMaxChannels + 1];
	IOMemoryDescriptor *mRootDesc { nullptr };
	IOMemoryMap *mRootMap { nullptr };
	uint32_t mRootPfn { 0 };
	uint32_t mPendingDefine { 0 };      // channels defined before the root page was announced
	uint32_t mChannelPolls { 0 };

	// self-test state machine (level 2)
	enum Step { kStepIdle, kStepWaitFifo, kStepWaitHostIrq, kStepWaitInterrupt, kStepDisplay, kStepTeardown, kStepDone } mStep { kStepIdle };
	uint32_t mDispPhase { 0 };
	uint64_t mDispT0 { 0 };
	uint32_t mTestVbls { 0 };
	uint32_t mTestSawBit { 0 };
	uint32_t mTestExpectTransactions { 0 };
	uint64_t mStepDeadline { 0 };
	bool mTestOk { true };
	IOBufferMemoryDescriptor *mTestFifo { nullptr };
	IOBufferMemoryDescriptor *mTestReply { nullptr };
	IOBufferMemoryDescriptor *mTestShared { nullptr };  // the display shared state page of the self-test's DisplaySetupSharedState on the root FIFO
	IOBufferMemoryDescriptor *mTestShared2 { nullptr }; // ... and of the one on the child channel
	IOBufferMemoryDescriptor *mTestRoot { nullptr };    // the root page (channel state records)
	IOBufferMemoryDescriptor *mTestChan { nullptr };    // a child channel's page list (page 0) and ring (pages 1..2)
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
uint64_t nowNs() {
	uint64_t abs = mach_absolute_time(), ns = 0;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}
uint64_t nowMs() { return nowNs() / 1000000ull; }
} // namespace

RDNA4PvHost *RDNA4PvHost::start(RDNA4PvNub *nub, uint32_t level) {
	auto *h = OSTypeAlloc(RDNA4PvHost);
	if (!h || !h->init()) {
		OSSafeReleaseNULL(h);
		return nullptr;
	}
	h->mNub = nub;
	h->mLevel = level;
	if (!gTr.lock)
		gTr.lock = IOLockAlloc();   // ponytail: never freed, like the host
	uint32_t disp = 0;
	if (PE_parse_boot_argn("rdna4-pvgpu-disp", &disp, sizeof(disp)))
		h->mDisp = disp & 3;
	if (level >= 2)
		h->mDisp = 3;   // the self-test plays the guest's side of the display events
	uint32_t w = 0, ht = 0;
	PE_parse_boot_argn("rdna4-pvgpu-width", &w, sizeof(w));
	PE_parse_boot_argn("rdna4-pvgpu-height", &ht, sizeof(ht));
	if (w || ht) {
		if (w >= 64 && w <= 8192 && ht >= 64 && ht <= 8192) {
			h->mDispInfo.width = h->mDispInfo.modes[0].width = static_cast<uint16_t>(w);
			h->mDispInfo.height = h->mDispInfo.modes[0].height = static_cast<uint16_t>(ht);
		} else {
			pvlog("host: rdna4-pvgpu-width/-height %u x %u ignored (64..8192 each)", w, ht);
		}
	}
	pvlog("host: display side: online event %s, VBL events %s, mode %ux%u", h->mDisp & kDispOnline ? "on" : "off", h->mDisp & kDispVbl ? "on" : "off",
	      h->mDispInfo.modes[0].width, h->mDispInfo.modes[0].height);
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
	if (mTicks++ < 3)
		pvlog("host: tick %llu", mTicks);
	if (mTicks == 1)
		dmaSelfTest();
	if (mIrqBits) {
		// The real register clears on read; RAM cannot, so the bits we raised are dropped a tick later (the handler has had its chance: the driver also
		// re-reads the stamp after every timed wait, so a missed wake-up costs at most the 1 s wait of IOAccelEventMachine2::waitForStamp).
		if (volatile uint32_t *c = mNub->ctrl())
			c[kRegIntrStatusGpu / 4] &= ~mIrqBits;
		mIrqBits = 0;
	}
	pollRegisters();
	mapRoot();
	pollFifo();
	pollChannels();
	pollDisplay();
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
		if (i == kRegFifoWritten / 4 || i == kRegFifoRead / 4 || i == kRegIntrStatusGpu / 4 || i == kRegIntrStatusDisp / 4)
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
		if (mFifoMap) {
			dropChannels();
			unmapFifo();
		}
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
	consume(CmdSource { "fifo", ring, ringLen, 0, 0 }, read, n);
	mConsumed += n;
	c[kRegFifoRead / 4] = written;   // acknowledge everything: AppleParavirtAccelerator::writeFifo spins until the host has read enough
}

void RDNA4PvHost::consume(const CmdSource &src, uint32_t read, uint32_t n) {
	// Decode what the guest wrote as FIFO commands ({u16 id, u16 barriers, u32 length, u32 signal, ...}, pvstream.hpp), log each by name and size.
	uint32_t at = 0, lastSignal = 0;
	bool haveSignal = false;
	while (at < n) {
		auto copy = [&](uint32_t from, uint8_t *dst, uint32_t count) {
			for (uint32_t i = 0; i < count; i++)
				dst[i] = src.ring[(read + from + i) % src.ringLen];
		};
		const uint32_t left = n - at;
		pvstream::FifoCommand c;
		uint8_t head[pvstream::kFifoHeaderBytes];
		bool ok = left >= pvstream::kFifoHeaderBytes;
		if (ok) {
			copy(at, head, sizeof(head));
			ok = pvstream::fifoHeader(head, sizeof(head), &c, src.ringLen) && c.length <= left;
		}
		if (!ok) {
			// Not a command boundary (or a command split across guest writes): show the bytes once and give up on this batch.
			if (mBadHeaders++ < 12) {
				uint8_t raw[32];
				const uint32_t take = left < sizeof(raw) ? left : static_cast<uint32_t>(sizeof(raw));
				copy(at, raw, take);
				char hex[sizeof(raw) * 2 + 1];
				for (uint32_t i = 0; i < take; i++)
					snprintf(hex + i * 2, 3, "%02x", raw[i]);
				pvlog("host: %s +%u bytes at ring offset 0x%x do not start with a command header: %s%s", src.tag, left, (read + at) % src.ringLen, hex, left > take ? "..." : "");
			}
			break;
		}
		mCmds++;
		const bool display = c.id == 0x02 || (c.id >= 0x04 && c.id <= 0x07) || c.id == 0x1e;   // the display pipe's commands keep being shown
		if (mCmds <= kLoggedCmds || (display && mDispCmdLines++ < 60)) {
			char line[200];
			if (c.length <= sizeof(mWin)) {
				copy(at, mWin, c.length);
				pvstream::describeFifo(mWin, c, line, sizeof(line));
			} else {
				snprintf(line, sizeof(line), "cmd 0x%02x %s, %u bytes (too large to decode here)", c.id, pvstream::fifoName(c.id) ? pvstream::fifoName(c.id) : "?", c.length);
			}
			pvlog("host: %s @0x%x: %s", src.tag, (read + at) % src.ringLen, line);
		}
		// M1: the payloads of the commands whose layout is still being worked out.
		if (c.length <= sizeof(mWin) && (c.id == 0x33 || c.id == 0x37 || c.id == 0x3b || c.id == 0x25 || c.id == 0x28 || c.id == 0x22 || c.id == 0x38) &&
		    mDumped[c.id] < 40) {
			mDumped[c.id]++;
			copy(at, mWin, c.length);
			dumpPayload(src.tag, c, mWin);
		}
		if (c.id == 0x38 && c.length <= sizeof(mWin) && c.length - c.payloadOffset >= 16) {
			copy(at, mWin, c.length);
			uint32_t task;
			memcpy(&task, mWin + c.payloadOffset, 4);   // DefineHostTask {u32 task << 1, u64 space size, u32 page-table root PFN}
			memcpy(&mTaskRoot[(task >> 1) & 63], mWin + c.payloadOffset + 12, 4);
		}
		if (c.id == 0x37 && c.length <= sizeof(mWin) && c.length - c.payloadOffset >= 4) {
			copy(at, mWin, c.length);
			uint32_t task;
			memcpy(&task, mWin + c.payloadOffset, 4);   // ExecIndirect {u32 task, u32 n, n resource entries, u64 gpu va, u64 length} (measured 2026-10-21)
			// the command buffer: the last two qwords of the payload, {u64 gpu va, u64 length}
			const uint32_t plen = c.length - c.payloadOffset;
			if (plen >= 24) {
				uint64_t va, len;
				memcpy(&va, mWin + c.payloadOffset + plen - 16, 8);
				memcpy(&len, mWin + c.payloadOffset + plen - 8, 8);
				decodeExec(task, va, len);
			}
		}
		if (c.id == 0x39 && c.length <= sizeof(mWin) && c.length - c.payloadOffset >= 4 && mDumped[0x39] < 16) {
			mDumped[0x39]++;
			copy(at, mWin, c.length);
			uint32_t task;
			memcpy(&task, mWin + c.payloadOffset, 4);                 // {u32 task, u64 gpu address, u64 length}
			dumpPayload(src.tag, c, mWin);
			dumpPage("task page-table root after a commit", mTaskRoot[task & 63]);
		}
		if ((c.id == 0x30 || c.id == 0x31) && src.channel == 0 && c.length <= sizeof(mWin) && c.length - c.payloadOffset >= 4) {
			if (mCmds > kLoggedCmds)
				copy(at, mWin, c.length);
			uint32_t n32;
			memcpy(&n32, mWin + c.payloadOffset, 4);
			if (c.id == 0x30)
				defineChannel(n32);
			else
				freeChannel(n32);
		}
		if (c.id == 0x3a && c.length <= sizeof(mWin)) {
			if (mCmds > kLoggedCmds)
				copy(at, mWin, c.length);   // logged commands already sit in mWin
			answerDeviceInfo(c, mWin);
		}
		if (c.id == 0x01 && c.length <= sizeof(mWin)) {
			if (mCmds > kLoggedCmds)
				copy(at, mWin, c.length);
			answerDisplaySharedState(c, mWin, src.channel);
		}
		if (c.id == 0x02 && c.length <= sizeof(mWin)) {
			copy(at, mWin, c.length);
			answerDisplayOnline(c, mWin);
		}
		if (c.id == 0x06 || c.id == 0x07)
			mTransactions++;   // completion is the generic stamp below: the pipe's fence waits for the command's signal (docs/m1-stream.md s.1b)
		else if (c.id == 0x04 || c.id == 0x05)
			mCursorCmds++;
		else if (c.id == 0x1e)
			mFlushes++;
		if (c.signal) {
			// the command asks for a stamp when it completes (AppleParavirtCommandAllocator::addSignal): remember the last value of this batch
			lastSignal = c.signal;
			haveSignal = true;
		}
		at += c.length;
	}
	if (haveSignal)
		completeStamp(src.stampIndex, lastSignal);
}

// One kernel mapping of guest physical memory, read-only unless asked (the map keeps the descriptor); nullptr when it cannot be made.
static IOMemoryMap *mapPhys(IOPhysicalAddress pa, IOByteCount len, bool writable = false) {
	IOMemoryDescriptor *d = IOMemoryDescriptor::withPhysicalAddress(pa, len, writable ? kIODirectionInOut : kIODirectionIn);
	IOMemoryMap *m = d ? d->createMappingInTask(kernel_task, 0, kIOMapAnywhere | (writable ? 0 : kIOMapReadOnly)) : nullptr;
	OSSafeReleaseNULL(d);
	return m;
}

// M1: a command's payload as dwords (up to 32), to work its layout out against what the guest asked for (tools/metal/m1/mtlprobe.m).
void RDNA4PvHost::dumpPayload(const char *tag, const pvstream::FifoCommand &c, const uint8_t *cmd) {
	const uint32_t bytes = c.length - c.payloadOffset, words = bytes / 4 < 32 ? bytes / 4 : 32;
	char line[32 * 9 + 1];
	for (uint32_t i = 0; i < words; i++) {
		uint32_t w;
		memcpy(&w, cmd + c.payloadOffset + i * 4, 4);
		snprintf(line + i * 9, 10, " %08x", w);
	}
	line[words * 9] = 0;
	pvlog("host: %s cmd 0x%02x payload (%u bytes):%s%s", tag, c.id, bytes, line, bytes / 4 > words ? " ..." : "");
}

// M1: the non-zero qwords of one guest page (read-only), for pages the protocol names as RAM (a task's page-table root).
void RDNA4PvHost::dumpPage(const char *what, uint32_t pfn) {
	if (!pfn || pfn >= (1u << 28))   // 1 TiB of physical address space at most
		return;
	IOMemoryMap *m = mapPhys(static_cast<IOPhysicalAddress>(pfn) << 12, 0x1000);
	if (!m) {
		pvlog("host: %s page 0x%x: cannot map", what, pfn);
		return;
	}
	const volatile uint64_t *q = reinterpret_cast<const volatile uint64_t *>(m->getVirtualAddress());
	char line[220];
	int len = 0;
	unsigned nonzero = 0;
	for (unsigned i = 0; i < 512; i++) {
		const uint64_t v = q[i];
		if (!v)
			continue;
		if (nonzero++ < 8 && len < 180)
			len += snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " [%u]=%llx", i, v);
	}
	line[len < static_cast<int>(sizeof(line)) ? len : static_cast<int>(sizeof(line)) - 1] = 0;
	pvlog("host: %s page 0x%x: %u non-zero qwords%s", what, pfn, nonzero, nonzero ? line : "");
	m->release();
}

// A task's GPU page table (measured on the rig, 2026-10-21; DefineHostTask names the root): the root page is a header whose first dword is the page
// number of the top table (then 3, ..., the dword at +0x18 = 4, the top table's entry count for the 16 GiB space); below it three tables of dwords:
// top [va >> 32], middle [(va >> 22) & 1023], leaf [(va >> 12) & 1023]. A leaf entry is 0x80000000 | the data page's number (bit 31 = mapped),
// the upper two levels hold bare page numbers. 0 = not mapped.
uint32_t RDNA4PvHost::guestPfn(uint32_t task, uint64_t va) {
	const unsigned index[4] = {0, static_cast<unsigned>(va >> 32) & 1023, static_cast<unsigned>(va >> 22) & 1023, static_cast<unsigned>(va >> 12) & 1023};
	uint32_t pfn = mTaskRoot[task & 63];
	for (unsigned level = 0; level < 4; level++) {
		IOMemoryMap *m = pfn && pfn < (1u << 28) ? mapPhys(static_cast<IOPhysicalAddress>(pfn) << 12, 0x1000) : nullptr;
		if (!m)
			return 0;
		pfn = reinterpret_cast<const volatile uint32_t *>(m->getVirtualAddress())[index[level]];
		m->release();
	}
	return (pfn & 0x80000000u) ? pfn & 0x0fffffffu : 0;
}

// M1: `len` bytes (<= 256) of a task's GPU address space, as dwords, 16 per line.
void RDNA4PvHost::dumpGuest(uint32_t task, uint64_t va, uint32_t len) {
	uint32_t w[64];
	len = len < sizeof(w) ? len : static_cast<uint32_t>(sizeof(w));
	if (!readGuest(task, va, reinterpret_cast<uint8_t *>(w), len)) {
		pvlog("host: task %u gpu va 0x%llx (%u bytes) is not mapped", task, va, len);
		return;
	}
	pvlog("host: task %u gpu va 0x%llx, %u bytes:", task, va, len);
	for (uint32_t i = 0; i < len / 4; i += 16) {
		char line[16 * 9 + 1];
		uint32_t n = 0;
		for (uint32_t k = i; k < i + 16 && k < len / 4; k++, n++)
			snprintf(line + n * 9, 10, " %08x", w[k]);
		line[n * 9] = 0;
		pvlog("host:   +0x%03x:%s", i * 4, line);
	}
}

// M1: [va, va + len) of a task's GPU address space, page by page through its page table, copied out (or, for writeGuest, in); false at the first
// page that is not mapped.
bool RDNA4PvHost::copyGuest(uint32_t task, uint64_t va, uint8_t *buf, uint32_t len, bool toGuest) {
	while (len) {
		const uint32_t pfn = guestPfn(task, va), off = static_cast<uint32_t>(va & 0xfff), take = len < 0x1000 - off ? len : 0x1000 - off;
		IOMemoryMap *m = pfn ? mapPhys((static_cast<IOPhysicalAddress>(pfn) << 12) + off, take, toGuest) : nullptr;
		if (!m)
			return false;
		if (toGuest)
			memcpy(reinterpret_cast<void *>(m->getVirtualAddress()), buf, take);
		else
			memcpy(buf, reinterpret_cast<const void *>(m->getVirtualAddress()), take);
		m->release();
		buf += take;
		va += take;
		len -= take;
	}
	return true;
}

namespace {
struct ExecTally {
	const char *segment;
	uint32_t commands, unknown, lines;
	RDNA4PvHost *host;
	uint32_t task;
	uint32_t pipeline;                  // Compute.setComputePipelineState
	uint32_t target, renderPipeline;    // Render: colour attachment 0's texture (writeDescriptor), setRenderPipelineState
	uint32_t buffer[8];                 // Compute.setBuffers: resource id per index (0 = none)
	uint64_t offset[8];
};
void execSink(void *user, uint32_t id, const char *name, const uint8_t *payload, uint32_t bytes, size_t offset) {
	auto *t = static_cast<ExecTally *>(user);
	t->commands++;
	if (!name)
		t->unknown++;
	if (id == 0x132 || id == 0x12d || id == 0x12e)
		t->host->runBlit(t->task, id, payload, bytes);
	else if (id == 0x01a)
		t->target = t->host->runRenderPass(t->task, payload, bytes);
	else if (id == 0x074 && bytes >= 4) {   // Render.setRenderPipelineState {u32 pipeline id}
		memcpy(&t->renderPipeline, payload, 4);
		t->host->describeRenderPipeline(t->task, t->renderPipeline);
	} else if (id == 0x001 && bytes >= 8) {   // Render.drawPrimitives {u32 primitive type, u16 vertex start, u16 count} (measured)
		uint32_t w[2];
		memcpy(w, payload, 8);
		t->host->runDraw(t->task, t->target, t->renderPipeline, w[0], w[1] & 0xffff, w[1] >> 16, 1);
	} else if (id == 0x003 && bytes >= 8) {   // Render.drawPrimitives {u16 start, u16 count, u16 instances, u16 type}
		uint32_t w[2];
		memcpy(w, payload, 8);
		t->host->runDraw(t->task, t->target, t->renderPipeline, w[1] >> 16, w[0] & 0xffff, w[0] >> 16, w[1] & 0xffff);
	}
	else if (id == 0x0d0 && bytes >= 4)
		memcpy(&t->pipeline, payload, 4);
	else if (id == 0x0cb && bytes >= 8) {   // {u32 first index, u32 count, count x {u32 resource id, u64 offset}} (measured, docs/m1-stream.md s.5b)
		uint32_t first, count;
		memcpy(&first, payload, 4);
		memcpy(&count, payload + 4, 4);
		for (uint32_t k = 0; k < count && first + k < 8 && 8 + 12 * (k + 1) <= bytes; k++) {
			memcpy(&t->buffer[first + k], payload + 8 + 12 * k, 4);
			memcpy(&t->offset[first + k], payload + 12 + 12 * k, 8);
		}
	} else if (id == 0x0cf && bytes >= 12) {   // Compute.setBufferOffset {u32 index, u64 offset} (measured; setBytes is setBuffers on a driver buffer)
		uint32_t index;
		memcpy(&index, payload, 4);
		if (index < 8)
			memcpy(&t->offset[index], payload + 4, 8);
	} else if ((id == 0x0ca || id == 0x0c8) && bytes >= 48) {
		// Compute.dispatchThreads {u64 threads[3], u64 per group[3]}; dispatchThreadgroups {u64 groups[3], u64 per group[3]}: whole groups
		uint64_t size[6];
		memcpy(size, payload, sizeof(size));
		for (unsigned d = 0; d < 3 && id == 0x0c8; d++)
			size[d] *= size[d + 3];
		t->host->runCompute(t->task, t->pipeline, t->buffer, t->offset, size);
	}
	if (t->lines++ >= 80)
		return;
	char hex[6 * 9 + 1];
	uint32_t n = 0;
	for (; n < 6 && n * 4 + 4 <= bytes; n++) {
		uint32_t w;
		memcpy(&w, payload + n * 4, 4);
		snprintf(hex + n * 9, 10, " %08x", w);
	}
	hex[n * 9] = 0;
	pvlog("host:   stream %s +0x%zx: 0x%03x %s (%u bytes)%s%s", t->segment, offset, id, name ? name : "UNKNOWN", bytes, hex, bytes > 24 ? " ..." : "");
}
} // namespace

// M1: one command buffer of an ExecIndirect, decoded: segments {u32 length, u8 type, u8 continuation, u8 flags, u8 0}, in them stream commands
// {u32 id, u32 size} + payload (docs/m1-stream.md section 4; the framing and the ids measured against tools/metal/m1/mtlprobe.m on the rig, 2026-10-21).
void RDNA4PvHost::decodeExec(uint32_t task, uint64_t va, uint64_t len) {
	const uint32_t n = len < 0x10000 ? static_cast<uint32_t>(len) : 0x10000;
	auto *buf = static_cast<uint8_t *>(IOMalloc(n ? n : 1));
	if (!buf)
		return;
	if (!readGuest(task, va, buf, n)) {
		pvlog("host: stream: task %u command buffer at 0x%llx (%llu bytes) is not mapped", task, va, len);
		IOFree(buf, n ? n : 1);
		return;
	}
	ExecTally t {};
	t.host = this;
	t.task = task;
	uint32_t at = 0, segments = 0, undecoded = 0;
	while (at + 8 <= n) {
		uint32_t slen;
		memcpy(&slen, buf + at, 4);
		const uint8_t type = buf[at + 4];
		if (slen < 8 || slen > n - at) {
			undecoded = n - at;
			pvlog("host:   stream: segment framing breaks at +0x%x (length %u)", at, slen);
			break;
		}
		segments++;
		t.segment = pvstream::segmentTypeName(type);
		if (type != 5) {   // 5: the protection-options envelope, a raw u64
			const size_t ok = pvstream::walkCommands(buf + at + 8, slen - 8, false, execSink, &t);
			undecoded += slen - 8 - static_cast<uint32_t>(ok);
		}
		at += slen;
	}
	pvlog("host: stream: task %u command buffer at 0x%llx, %llu bytes: %u segment(s), %u command(s), %u unknown, %u byte(s) undecoded%s", task, va, len,
	      segments, t.commands, t.unknown, undecoded, len > n ? " (only the first 64 KiB read)" : "");
	IOFree(buf, n ? n : 1);
}

// M2: a buffer's GPU range, from the task's resource heap (SetResourceHeap: 1 MiB at VA 0x1000; measured on the rig, 2026-10-21): 12-byte records
// indexed by resource id, {u32 (descriptor bytes << 8) | type, u32 descriptor address, u32 0}; a buffer (type 1) has a 16-byte descriptor
// {u64 length, u64 page number of its start} (mtlprobe's two 4 KiB buffers: {0x1000, 0x101}, {0x1000, 0x102}).
bool RDNA4PvHost::bufferRange(uint32_t task, uint32_t id, uint64_t *va, uint64_t *len) {
	uint32_t rec[3];
	uint64_t desc[2];
	if (!id || id > 0x10000 || !readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(rec), sizeof(rec)) || (rec[0] & 0xff) != 1 ||
	    !readGuest(task, rec[1], reinterpret_cast<uint8_t *>(desc), sizeof(desc))) {
		pvlog("host: blit: task %u resource %u: no buffer", task, id);
		return false;
	}
	*len = desc[0];
	*va = desc[1] << 12;
	return desc[0] && desc[1] < (1ull << 36) && guestPfn(task, *va);
}

// M2: a texture's GPU range: a type-2 resource with a 116-byte descriptor, dwords {u64 size in bytes, u32 page number of its start, ...,
// [13] bytes per row, [15] width, ...} (mtlprobe's 64x64 BGRA8 render target: 0x4000 bytes at page 0x101, 0x100 per row: linear; measured
// 2026-10-21). Where the pixel format is (0x04010101 at [8]?) is still open; the first descriptors of a boot are logged.
bool RDNA4PvHost::textureRange(uint32_t task, uint32_t id, uint64_t *va, uint64_t *len, uint32_t *bytesPerRow, uint32_t *width) {
	uint32_t rec[3], desc[29];
	if (!id || id > 0x10000 || !readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(rec), sizeof(rec)) || (rec[0] & 0xff) != 2 ||
	    (rec[0] >> 8) < sizeof(desc) || !readGuest(task, rec[1], reinterpret_cast<uint8_t *>(desc), sizeof(desc))) {
		if (mRenderLines++ < 24)
			pvlog("host: task %u resource %u: no texture", task, id);
		return false;
	}
	if (mRenderLines++ < 24)
		pvlog("host: task %u texture %u: descriptor %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", task, id, desc[0],
	      desc[1], desc[2], desc[3], desc[4], desc[5], desc[6], desc[7], desc[8], desc[9], desc[10], desc[11], desc[12], desc[13], desc[14], desc[15]);
	*len = desc[0] | static_cast<uint64_t>(desc[1]) << 32;
	*va = static_cast<uint64_t>(desc[2]) << 12;
	if (bytesPerRow)
		*bytesPerRow = desc[13];
	if (width)
		*width = desc[15];
	return *len && *len <= (1ull << 30) && guestPfn(task, *va);
}

// A double in [0, 1] (its bits) as an 8-bit unorm, round(v * 255), with integers only; below 0 (or NaN) 0, from 1 up 255.
static uint32_t unorm8(uint64_t bits) {
	const uint32_t e = static_cast<uint32_t>(bits >> 52) & 0x7ff;
	if (bits >> 63 || e < 1023 - 60)
		return 0;   // negative, zero, tiny or NaN with the sign set
	if (e >= 1023)
		return e == 0x7ff && (bits & ((1ull << 52) - 1)) ? 0 : 255;   // 1 and up (NaN: 0)
	const uint64_t m = (bits & ((1ull << 52) - 1)) | (1ull << 52);    // 53-bit significand, v = m * 2^(e - 1075)
	const uint32_t shift = 52 + (1023 - e);
	return static_cast<uint32_t>((m * 255 + (1ull << (shift - 1))) >> shift);
}

// M2: a render pass with nothing drawn: Render.writeDescriptor (584 bytes) carries colour attachment 0 as {u32 texture at +0x4c, u32 (store action
// << 16) | load action at +0x60, double clear colour[4] at +0x68} (mtlprobe: texture 1, 0x00010002 = clear and store, 0.25 0.5 0.75 1; measured
// 2026-10-21). A clear is a fill of the texture's memory with the colour as a BGRA8 pixel: the format is assumed (mtlprobe's) until it is found in
// the texture's descriptor. Nothing else of a pass is carried out yet (draws need shaders).
uint32_t RDNA4PvHost::runRenderPass(uint32_t task, const uint8_t *p, uint32_t bytes) {
	if (!gCompute || bytes < 0x88)
		return 0;
	uint32_t tex, actions;
	uint64_t rgba[4];   // the clear colour as IEEE-754 doubles, converted with integers only (no floating point in the kernel)
	memcpy(&tex, p + 0x4c, 4);
	memcpy(&actions, p + 0x60, 4);
	memcpy(rgba, p + 0x68, sizeof(rgba));
	if ((actions & 0xffff) != 2 || !tex)   // MTLLoadActionClear
		return tex;
	const uint32_t r = unorm8(rgba[0]), g = unorm8(rgba[1]), b = unorm8(rgba[2]), a = unorm8(rgba[3]);
	const uint32_t pixel = b | g << 8 | r << 16 | a << 24;   // B, G, R, A in memory
	uint64_t va, len;
	const bool ok = textureRange(task, tex, &va, &len) && !(len & 3) && dmaGuest(task, va, 0, pixel, len);
	if (mRenderLines++ < 24)
		pvlog("host: render: clear texture %u to RGBA8 (%u, %u, %u, %u) = BGRA8 pixel 0x%08x on the card: %s", tex, r, g, b, a, pixel, ok ? "done" : "NOT done");
	return tex;
}

// M4: the value dword of register `reg` (offset as in the packet: context from 0xa000, SH from 0x2c00, uconfig from 0xc000) in a stream's
// SET_*_REG packet `opcode` (0x69 context, 0x76 SH, 0x79 uconfig), or null.
static uint32_t *streamReg(uint32_t *ib, uint32_t n, uint32_t opcode, uint32_t reg) {
	for (uint32_t i = 0; i + 1 < n;) {
		const uint32_t h = ib[i], count = ((h >> 16) & 0x3fff) + 1;   // PM4 type 3: dwords after the header
		if ((h >> 30) != 3)
			return nullptr;
		if (((h >> 8) & 0xff) == opcode && reg >= ib[i + 1] && reg - ib[i + 1] < count - 1 && i + 2 + (reg - ib[i + 1]) < n)
			return &ib[i + 2 + (reg - ib[i + 1])];
		i += 1 + count;
	}
	return nullptr;
}

// n / 2 as an IEEE-754 single's bits, negated if asked, for n < 2^24 (no floating point in the kernel).
static uint32_t halfAsFloat(uint32_t n, bool negative) {
	if (!n)
		return 0;
	const uint32_t e = 31 - static_cast<uint32_t>(__builtin_clz(n));
	return (negative ? 0x80000000u : 0) | ((126 + e) << 23) | ((n << (23 - e)) & 0x7fffff);
}

// M4: Render.drawPrimitives on the card: the pipeline's vertex and fragment functions translated by the helper (air2amdgpu.cpp
// translateShader), drawn with the stream of src/gfx12_draw.h (the triangle every gfx test of the kext draws, proven on the card) with the
// translated shaders in place of its own and the target, viewport and draw patched; the colour attachment staged through a linear RGBA8
// target (the texture is BGRA8, swapped on the way in and out). So far: triangle lists from vertex 0, one instance, no vertex buffers.
void RDNA4PvHost::runDraw(uint32_t task, uint32_t target, uint32_t pipeline, uint32_t type, uint32_t start, uint32_t count, uint32_t instances) {
	const bool talk = mRenderLines++ < 48;
	// The pipeline: {16-byte header, u8 n, n x {u8 key, u8 size, value}}: key 1 the vertex function, key 2 the fragment function (measured).
	uint64_t pd[8] = {};
	uint32_t pbytes = 0, fn[3] = {};
	if (resourceType(task, pipeline, pd, &pbytes) == 7 && pbytes >= 17) {
		const auto *pb = reinterpret_cast<const uint8_t *>(pd);
		for (uint32_t k = 0, at = 17; k < pb[16] && at + 2 <= pbytes; k++, at += 2 + pb[at + 1])
			if (pb[at] >= 1 && pb[at] <= 2 && pb[at + 1] == 4 && at + 6 <= pbytes)
				memcpy(&fn[pb[at]], pb + at + 2, 4);
	}
	const char *why = !fn[1] || !fn[2] ? "the pipeline has no vertex or no fragment function"
	                : type != 3 ? "only triangle lists so far" : start || instances > 1 ? "only from vertex 0, one instance, so far"
	                : !count || count > 0xffff ? "no vertices" : nullptr;
	const Jit *shader[3] = {};
	char name[3][64] = {};
	for (unsigned k = 1; k <= 2 && !why; k++) {
		uint8_t hash[32];
		uint64_t lib = 0;
		shader[k] = functionName(task, fn[k], name[k], sizeof(name[k]), hash, &lib) ? translated(task, lib, name[k], hash) : nullptr;
		// The RGFX header: {"RGFX", u32 stage, u32 code bytes, ...}; the code must fit its 4 KiB slot with room for the prefetch.
		const auto *head = shader[k] && shader[k]->gfx && shader[k]->gfxLen >= 32 ? reinterpret_cast<const uint32_t *>(shader[k]->gfx) : nullptr;
		if (!head || head[1] != k || head[2] > 0xf00 || head[2] + 32 > shader[k]->gfxLen)
			why = k == 1 ? "no translated vertex shader" : "no translated fragment shader";
	}
	uint64_t va = 0, len = 0;
	uint32_t bpr = 0, width = 0;
	if (!why && !(textureRange(task, target, &va, &len, &bpr, &width) && width && bpr == width * 4 && len % bpr == 0 && width <= 16384 && len / bpr <= 16384))
		why = "the colour attachment is not a linear 32-bit texture";
	if (why) {
		if (talk)
			pvlog("host: render: draw of %u vertices (type %u) with pipeline %u: %s: not run", count, type, pipeline, why);
		return;
	}
	const uint32_t height = static_cast<uint32_t>(len / bpr), pitch = (width + 31) & ~31u;   // a linear target's pitch: a multiple of 128 bytes
	const uint64_t targetBytes = static_cast<uint64_t>(pitch) * height * 4;

	// The buffers kept for the boot (made by the first draw), and the target for this one.
	auto make = [&](GfxBuffers &g, uint64_t bytes) { return g.handle || gCompute->rtAlloc(this, bytes, g.handle, g.gpu) == kIOReturnSuccess; };
	if (!mRtOpen)
		mRtOpen = gCompute->rtOpen(this) == kIOReturnSuccess;
	GfxBuffers tgt {};
	auto *stage = static_cast<uint32_t *>(IOMalloc(targetBytes));
	bool ok = mRtOpen && stage && make(mGfxCode, RDNA4_TRI_CODE_BYTES) && make(mGfxRings, rdna4_tri_ring_bytes() + RDNA4_TRIRUN_RING_SLACK) &&
	          make(mGfxMisc, RDNA4_TRIRUN_PAGE) && make(mGfxIb, RDNA4_TRIRUN_PAGE) &&
	          rdna4_trirun_rings_fit(mGfxRings.gpu, rdna4_tri_ring_bytes() + RDNA4_TRIRUN_RING_SLACK, rdna4_tri_ring_bytes()) &&
	          gCompute->rtAlloc(this, targetBytes, tgt.handle, tgt.gpu) == kIOReturnSuccess;

	// The shaders: VS at +0, PS at +RDNA4_TRI_PS_OFFSET, each slot padded with s_code_end for the SQ's prefetch.
	static uint32_t code[RDNA4_TRI_CODE_BYTES / 4];   // ponytail: one host, draws run one at a time on its work loop
	for (uint32_t i = 0; i < RDNA4_TRI_CODE_BYTES / 4; i++)
		code[i] = 0xbf9f0000u;
	for (unsigned k = 1; k <= 2; k++)
		memcpy(reinterpret_cast<uint8_t *>(code) + (k == 1 ? 0 : RDNA4_TRI_PS_OFFSET), shader[k]->gfx + 32, reinterpret_cast<const uint32_t *>(shader[k]->gfx)[2]);
	const uint32_t *vs = reinterpret_cast<const uint32_t *>(shader[1]->gfx), *ps = reinterpret_cast<const uint32_t *>(shader[2]->gfx);

	// The stream: the triangle's, with this draw's addresses, shaders' registers, target, viewport and vertex count.
	static uint32_t ib[RDNA4_TRIRUN_PAGE / 4];
	const rdna4_tri_va where = {mGfxCode.gpu, tgt.gpu, rdna4_trirun_rings_va(mGfxRings.gpu), mGfxMisc.gpu};
	const uint32_t n = ok ? rdna4_tri_record(ib, &where) : 0;
	struct Patch { uint32_t opcode, reg, value; } patch[] = {
		{0x76, 0x8a, vs[3]}, {0x76, 0x8b, vs[4]},                          // SPI_SHADER_PGM_RSRC1_GS, RSRC2_GS: as LLVM compiled the vertex shader
		{0x76, 0x0a, ps[3]}, {0x76, 0x0b, ps[4]},                          // SPI_SHADER_PGM_RSRC1_PS, RSRC2_PS
		{0x69, 0x197, ps[5]}, {0x69, 0x198, ps[6]},                        // SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR
		{0x69, 0x31e, (pitch - 1) << 16 | (height - 1)},                    // CB_COLOR0_ATTRIB2: MIP0_WIDTH (the pitch), MIP0_HEIGHT
		{0x69, 0x82, width | height << 16},                                // PA_SC_WINDOW_SCISSOR_BR
		{0x69, 0x95, width | height << 16},                                // PA_SC_VPORT_SCISSOR_0_BR
		{0x69, 0x10f, halfAsFloat(width, false)}, {0x69, 0x110, halfAsFloat(width, false)},   // PA_CL_VPORT_XSCALE, XOFFSET: w / 2
		{0x69, 0x111, halfAsFloat(height, true)}, {0x69, 0x112, halfAsFloat(height, false)},  // YSCALE -h / 2 (Metal's y points up), YOFFSET h / 2
	};
	for (const Patch &pt : patch)
		if (uint32_t *d = n ? streamReg(ib, n, pt.opcode, pt.reg) : nullptr)
			*d = pt.value;
		else
			ok = false;
	for (uint32_t i = 0; i + 2 < n && ok; i++)   // DRAW_INDEX_AUTO {u32 vertex count, u32 initiator}
		if (ib[i] == 0xc0012d00u) {
			ib[i + 1] = count;
			break;
		}

	// The attachment in (BGRA8 rows -> RGBA8 at the target's pitch), the draw, the attachment out.
	auto swap = [&]() {
		for (uint64_t i = 0; i < static_cast<uint64_t>(pitch) * height; i++)
			stage[i] = (stage[i] & 0xff00ff00u) | (stage[i] >> 16 & 0xff) | (stage[i] & 0xff) << 16;
	};
	for (uint32_t y = 0; y < height && ok; y++)
		ok = readGuest(task, va + static_cast<uint64_t>(y) * bpr, reinterpret_cast<uint8_t *>(stage + static_cast<uint64_t>(y) * pitch), bpr);
	const uint32_t zero = 0;
	uint64_t fence = 0, ns = 0;
	if (ok) {
		swap();
		ok = gCompute->rtCopy(this, tgt.handle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(stage), targetBytes, true) == kIOReturnSuccess &&
		     gCompute->rtCopy(this, mGfxCode.handle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(code), sizeof(code), true) == kIOReturnSuccess &&
		     gCompute->rtCopy(this, mGfxMisc.handle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(&zero), 4, true) == kIOReturnSuccess &&
		     gCompute->rtCopy(this, mGfxIb.handle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(ib), n * 4, true) == kIOReturnSuccess &&
		     gCompute->rtSubmitGfxIb(this, mGfxIb.gpu, n, 0, fence) == kIOReturnSuccess &&
		     gCompute->rtWaitGfxFence(this, static_cast<uint32_t>(fence), 2000, ns) == kIOReturnSuccess &&
		     gCompute->rtCopy(this, tgt.handle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(stage), targetBytes, false) == kIOReturnSuccess;
	}
	if (ok) {
		swap();
		for (uint32_t y = 0; y < height && ok; y++)
			ok = copyGuest(task, va + static_cast<uint64_t>(y) * bpr, reinterpret_cast<uint8_t *>(stage + static_cast<uint64_t>(y) * pitch), bpr, true);
	}
	if (tgt.handle)
		gCompute->rtFree(this, tgt.handle);
	if (stage)
		IOFree(stage, targetBytes);
	if (talk)
		pvlog("host: render: draw of %u vertices into texture %u (%ux%u) with \"%s\" and \"%s\" on the card: %s (%llu us)", count, target, width, height,
		      name[1], name[2], ok ? "done" : "NOT done", ns / 1000);
}

// M2: an SDMA fill (srcVa 0: `pattern`) or copy (from srcVa) over `bytes` of a task's GPU address space, page by page through its page table, the CPU
// caches flushed before and after each piece (the engine does not snoop them).
bool RDNA4PvHost::dmaGuest(uint32_t task, uint64_t dstVa, uint64_t srcVa, uint32_t pattern, uint64_t bytes) {
	while (bytes) {
		uint64_t n = 0x1000 - (dstVa & 0xfff);
		if (srcVa && 0x1000 - (srcVa & 0xfff) < n)
			n = 0x1000 - (srcVa & 0xfff);
		if (bytes < n)
			n = bytes;
		const uint32_t dp = guestPfn(task, dstVa), sp = srcVa ? guestPfn(task, srcVa) : 1;
		if (!dp || !sp)
			return false;
		const uint64_t dst = (static_cast<uint64_t>(dp) << 12) + (dstVa & 0xfff), src = (static_cast<uint64_t>(sp) << 12) + (srcVa & 0xfff);
		IOMemoryDescriptor *dd = IOMemoryDescriptor::withPhysicalAddress(dst, n, kIODirectionInOut);
		IOMemoryDescriptor *sd = srcVa ? IOMemoryDescriptor::withPhysicalAddress(src, n, kIODirectionInOut) : nullptr;
		if (dd)
			dd->performOperation(kIOMemoryIncoherentIOFlush, 0, n);
		if (sd)
			sd->performOperation(kIOMemoryIncoherentIOFlush, 0, n);
		const bool ok = srcVa ? gCompute->sysCopy(src, dst, static_cast<uint32_t>(n)) : gCompute->sysFill(dst, pattern, static_cast<uint32_t>(n));
		if (dd)
			dd->performOperation(kIOMemoryIncoherentIOFlush, 0, n);
		OSSafeReleaseNULL(dd);
		OSSafeReleaseNULL(sd);
		if (!ok)
			return false;
		dstVa += n;
		srcVa += srcVa ? n : 0;
		bytes -= n;
	}
	return true;
}

// M2: Blit.fillBuffer {u32 buffer, u64 offset, u64 length, u32 value} and Blit.copyFromBuffer {u32 source, u32 destination, u64 source offset,
// u64 destination offset, u64 size} (measured against mtlprobe on the rig, 2026-10-21), run on the card.
void RDNA4PvHost::runBlit(uint32_t task, uint32_t id, const uint8_t *p, uint32_t bytes) {
	if (!gCompute)
		return;
	uint64_t va, len, va2, len2;
	if (id == 0x132 && bytes >= 24) {
		uint32_t buf, value;
		uint64_t off, n;
		memcpy(&buf, p, 4);
		memcpy(&off, p + 4, 8);
		memcpy(&n, p + 12, 8);
		memcpy(&value, p + 20, 4);
		const bool ok = bufferRange(task, buf, &va, &len) && off <= len && n <= len - off && !(n & 3) && !((va + off) & 3) &&
		                dmaGuest(task, va + off, 0, (value & 0xff) * 0x01010101u, n);
		pvlog("host: blit: fillBuffer buffer %u +0x%llx 0x%llx bytes with 0x%02x on the card: %s", buf, off, n, value & 0xff, ok ? "done" : "NOT done");
	} else if (id == 0x12d && bytes >= 32) {
		uint32_t src, dst;
		uint64_t so, dof, n;
		memcpy(&src, p, 4);
		memcpy(&dst, p + 4, 4);
		memcpy(&so, p + 8, 8);
		memcpy(&dof, p + 16, 8);
		memcpy(&n, p + 24, 8);
		const bool ok = bufferRange(task, src, &va, &len) && bufferRange(task, dst, &va2, &len2) && so <= len && n <= len - so && dof <= len2 &&
		                n <= len2 - dof && dmaGuest(task, va2 + dof, va + so, 0, n);
		pvlog("host: blit: copyFromBuffer %u +0x%llx -> %u +0x%llx, 0x%llx bytes on the card: %s", src, so, dst, dof, n, ok ? "done" : "NOT done");
	} else if (id == 0x12e && bytes >= 88) {
		// Blit.copyFromTexture {u32 texture, u32 buffer, u64 origin[3], u64 size[3], u64 destination offset, u64 bytes per row, u64 bytes per image,
		// u32 slice, u32 level} (mtlprobe: 64x64x1, 0x100 per row, 0x4000 per image; measured 2026-10-21). Only the whole of a linear texture, copied
		// as bytes: what a texture's memory looks like is not measured yet beyond its size (64x64 BGRA8 = exactly 0x4000).
		uint32_t tex, buf, slice, level;
		uint64_t origin[3], size[3], dof, bpr, bpi;
		memcpy(&tex, p, 4);
		memcpy(&buf, p + 4, 4);
		memcpy(origin, p + 8, 24);
		memcpy(size, p + 32, 24);
		memcpy(&dof, p + 56, 8);
		memcpy(&bpr, p + 64, 8);
		memcpy(&bpi, p + 72, 8);
		memcpy(&slice, p + 80, 4);
		memcpy(&level, p + 84, 4);
		const bool whole = !origin[0] && !origin[1] && !origin[2] && size[2] == 1 && !slice && !level && bpr == size[0] * 4 && bpi == bpr * size[1];
		const bool ok = whole && textureRange(task, tex, &va, &len) && bpi == len && bufferRange(task, buf, &va2, &len2) && dof <= len2 &&
		                bpi <= len2 - dof && dmaGuest(task, va2 + dof, va, 0, bpi);
		pvlog("host: blit: copyFromTexture %u (%llux%llu, %llu bytes per row) -> buffer %u +0x%llx on the card: %s", tex, size[0], size[1], bpr, buf, dof,
		      ok ? "done" : whole ? "NOT done" : "not run (not the whole of a texture)");
	}
}

// M2: the task's function: its one type-6 resource (several are not told apart yet), whose 32-byte descriptor starts with the GPU address of a
// metallib ('MTLB', measured 2026-10-21); the function's NAME and HASH tags from that library's first KiB.
// A resource's type from the task's heap (records at 0x1000 + 12 * id: {(descriptor bytes << 8) | type, u64 descriptor address}), 0 if
// none; with desc, its descriptor's first 64 bytes (and their count in bytes).
uint32_t RDNA4PvHost::resourceType(uint32_t task, uint32_t id, uint64_t *desc, uint32_t *bytes) {
	uint32_t rec[3];
	if (!id || id > 0x10000 || !readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(rec), sizeof(rec)))
		return 0;
	const uint32_t n = (rec[0] >> 8) < 64 ? rec[0] >> 8 : 64;
	if (desc && (!n || !readGuest(task, rec[1] | static_cast<uint64_t>(rec[2]) << 32, reinterpret_cast<uint8_t *>(desc), n)))
		return 0;
	if (bytes)
		*bytes = n;
	return rec[0] & 0xff;
}

// M3: the Metal function a compute pipeline runs: its name and hash from the metallib (tags {char[4], u16 length, bytes} per function, NAME
// then HASH), and the metallib's address. The pipeline's descriptor names the function (measured 2026-10-21, docs/m1-stream.md s.5b):
// {u32 0xb, u32 bytes (24), u32 its own id, u32 7, u32 (function id << 24) | 0x040001, u32 0}; seen with function ids 1 and 3 only, so an id
// above 255 is not known to fit. Else the task's only function. Each function has a metallib of its own (measured); one holding several
// is refused.
bool RDNA4PvHost::functionOf(uint32_t task, uint32_t pipeline, char *name, size_t nameCap, uint8_t hash[32], uint64_t *libOut) {
	const bool talk = mFnLines < 40;
	uint32_t fn = 0;
	const char *rule = "";
	uint64_t desc[8] = {};
	uint32_t bytes = 0;
	if (resourceType(task, pipeline, desc, &bytes) == 7 && bytes >= 20) {
		const auto *dw = reinterpret_cast<const uint32_t *>(desc);
		if (talk)
			pvlog("host: compute: pipeline %u descriptor (%u bytes): %08x %08x %08x %08x %08x %08x", pipeline, bytes, dw[0], dw[1], dw[2], dw[3], dw[4], dw[5]);
		if (resourceType(task, dw[4] >> 24) == 6) {
			fn = dw[4] >> 24;
			rule = "named by the pipeline";
		}
	}
	if (!fn) {
		unsigned found = 0;
		for (uint32_t id = 1; id <= 64; id++)
			if (resourceType(task, id) == 6 && found++ == 0)
				fn = id;
		rule = "the task's only function";
		if (found != 1)
			fn = 0;
	}
	if (!fn || resourceType(task, fn, desc, &bytes) != 6) {
		pvlog("host: compute: task %u pipeline %u: no function found", task, pipeline);
		return false;
	}
	if (talk) {
		mFnLines += 3;
		const auto *dw = reinterpret_cast<const uint32_t *>(desc);
		pvlog("host: compute: pipeline %u -> function %u (%s), descriptor: %08x %08x %08x %08x %08x %08x %08x %08x", pipeline, fn, rule, dw[0], dw[1],
		      dw[2], dw[3], dw[4], dw[5], dw[6], dw[7]);
	}
	return functionName(task, fn, name, nameCap, hash, libOut);
}

// A function resource's name and hash from its metallib (tags {char[4], u16 length, bytes}, NAME then HASH) and the metallib's address.
bool RDNA4PvHost::functionName(uint32_t task, uint32_t fn, char *name, size_t nameCap, uint8_t hash[32], uint64_t *libOut) {
	uint64_t desc[8] = {};
	if (resourceType(task, fn, desc) != 6)
		return false;
	const uint64_t lib = desc[0];
	uint8_t head[1024];
	if (!readGuest(task, lib, head, sizeof(head)) || memcmp(head, "MTLB", 4)) {
		pvlog("host: compute: task %u function %u: no metallib at 0x%llx", task, fn, lib);
		return false;
	}
	unsigned names = 0;
	name[0] = 0;
	memset(hash, 0, 32);
	for (size_t i = 0; i + 6 < sizeof(head); i++) {   // tags: {char[4] tag, u16 length, bytes}
		uint16_t n;
		memcpy(&n, head + i + 4, 2);
		if (!memcmp(head + i, "NAME", 4) && n && n < nameCap && i + 6 + n <= sizeof(head) && names++ == 0) {
			memcpy(name, head + i + 6, n);
			name[n] = 0;
		} else if (!memcmp(head + i, "HASH", 4) && n == 32 && i + 6 + 32 <= sizeof(head) && names == 1) {
			memcpy(hash, head + i + 6, 32);
		}
	}
	if (names != 1) {
		pvlog("host: compute: task %u function %u: the metallib holds %u functions, not one: not run", task, fn, names);
		return false;
	}
	*libOut = lib;
	return true;
}

// A code object loaded into the card's runtime: its program, or 0.
uint32_t RDNA4PvHost::load(const uint8_t *elf, uint32_t size, const char *name) {
	if (!gCompute)
		return 0;
	if (!mRtOpen)
		mRtOpen = gCompute->rtOpen(this) == kIOReturnSuccess;   // ponytail: never closed, the host lives as long as the boot
	uint64_t out[8] = {};
	const bool ok = mRtOpen && gCompute->rtLoad(this, kernel_task, reinterpret_cast<mach_vm_address_t>(elf), size, name, out) == kIOReturnSuccess;
	pvlog("host: compute: %s %s (program 0x%llx)", name, ok ? "loaded into the runtime" : "did NOT load", out[0]);
	return ok ? static_cast<uint32_t>(out[0]) : 0;
}

// kMtlKernels[kernel] loaded into the card's runtime (on first use), or 0.
uint32_t RDNA4PvHost::program(unsigned kernel) {
	if (!mProgram[kernel])
		mProgram[kernel] = load(kMtlKernels[kernel].elf, kMtlKernels[kernel].size, kMtlKernels[kernel].name);
	return mProgram[kernel];
}

// M3: the function translated at run time by the helper (include/rdna4mtlc.h) and loaded into the runtime, cached by its hash (failures too,
// so a function that cannot be translated is asked for once). Null without a helper, or when it did not answer within 3 s (not cached).
const RDNA4PvHost::Jit *RDNA4PvHost::translated(uint32_t task, uint64_t lib, const char *name, const uint8_t hash[32]) {
	for (uint32_t i = 0; i < mJits && i < 32; i++)
		if (!memcmp(mJit[i].hash, hash, 32))
			return mJit[i].program || mJit[i].gfx ? &mJit[i] : nullptr;
	if (!gTr.lock || !gTr.helper)
		return nullptr;
	uint64_t size = 0;   // the metallib's file size (header u64 at 0x10)
	auto *bytes = readGuest(task, lib + 0x10, reinterpret_cast<uint8_t *>(&size), 8) && size >= 0x58 && size <= RDNA4_MTLC_MAX_LIB
	              ? static_cast<uint8_t *>(IOMalloc(size)) : nullptr;
	if (!bytes || !readGuest(task, lib, bytes, static_cast<uint32_t>(size))) {
		pvlog("host: compute: \"%s\": metallib of %llu bytes not read", name, size);
		if (bytes)
			IOFree(bytes, size);
		return nullptr;
	}
	IOLockLock(gTr.lock);
	gTr.id = ++gTr.seq ? gTr.seq : ++gTr.seq;   // never 0 (0 = no request)
	gTr.taken = gTr.done = false;
	gTr.lib = bytes;
	gTr.libLen = static_cast<uint32_t>(size);
	strlcpy(gTr.name, name, sizeof(gTr.name));
	IOLockWakeup(gTr.lock, &gTr.id, false);
	uint64_t deadline = 0;
	clock_interval_to_deadline(3, kSecondScale, &deadline);
	while (!gTr.done && IOLockSleepDeadline(gTr.lock, &gTr.done, deadline, THREAD_UNINT) == THREAD_AWAKENED) {
	}
	const bool done = gTr.done;
	uint8_t *elf = gTr.elf;
	const uint32_t elfLen = gTr.elfLen, buffers = gTr.buffers, written = gTr.written;
	gTr.id = 0;
	gTr.lib = nullptr;
	gTr.elf = nullptr;
	IOLockUnlock(gTr.lock);
	IOFree(bytes, size);
	if (!done) {
		pvlog("host: compute: \"%s\": the helper did not translate it within 3 s", name);
		return nullptr;
	}
	Jit &j = mJit[mJits++ % 32];
	if (j.program)
		gCompute->rtUnload(this, j.program);
	if (j.gfx)
		IOFree(j.gfx, j.gfxLen);
	memcpy(j.hash, hash, 32);
	const bool shader = elf && elfLen >= 32 && !memcmp(elf, "RGFX", 4);
	j.program = elf && !shader && buffers <= 8 ? load(elf, elfLen, name) : 0;
	j.buffers = buffers;
	j.written = written;
	j.gfx = shader ? elf : nullptr;   // kept: the host places it into its code buffer for each draw
	j.gfxLen = shader ? elfLen : 0;
	if (elf && !shader)
		IOFree(elf, elfLen);
	pvlog("host: \"%s\" %s by the helper (%llu bytes of metallib, %u bytes of %s)", name, elf ? "translated" : "could NOT be translated", size, elfLen,
	      shader ? "shader" : "code");
	return j.program || j.gfx ? &j : nullptr;
}

// M4, first look at render pipelines: the pipeline's resource record and its whole descriptor, and the task's first resources with their
// types (functions with their names). Each (task, pipeline) once, the first 16 of the boot.
void RDNA4PvHost::describeRenderPipeline(uint32_t task, uint32_t pipeline) {
	const uint32_t key = (task << 16) | (pipeline & 0xffff);
	unsigned slot = 0;
	while (slot < 16 && mRenderPipeSeen[slot] && mRenderPipeSeen[slot] != key)
		slot++;
	if (slot == 16 || mRenderPipeSeen[slot] == key)
		return;
	mRenderPipeSeen[slot] = key;
	uint32_t rec[3];
	if (pipeline && pipeline <= 0x10000 && readGuest(task, 0x1000 + 12ull * pipeline, reinterpret_cast<uint8_t *>(rec), sizeof(rec))) {
		pvlog("host: render: task %u pipeline %u: type %u, %u-byte descriptor at 0x%x", task, pipeline, rec[0] & 0xff, rec[0] >> 8, rec[1]);
		dumpGuest(task, rec[1], (rec[0] >> 8) < 256 ? rec[0] >> 8 : 256);
	}
	for (uint32_t id = 1; id <= 24; id++) {
		uint64_t desc[8] = {};
		uint32_t bytes = 0;
		const uint32_t type = resourceType(task, id, desc, &bytes);
		if (!type)
			continue;
		const auto *dw = reinterpret_cast<const uint32_t *>(desc);
		char name[48] = "";
		uint8_t head[512];
		if (type == 6 && readGuest(task, desc[0], head, sizeof(head)) && !memcmp(head, "MTLB", 4))
			for (size_t i = 0; i + 6 < sizeof(head); i++)
				if (!memcmp(head + i, "NAME", 4)) {
					uint16_t n;
					memcpy(&n, head + i + 4, 2);
					if (n && n < sizeof(name) && i + 6 + n <= sizeof(head)) {
						memcpy(name, head + i + 6, n);
						name[n] = 0;
					}
					break;
				}
		pvlog("host: render: task %u resource %u: type %u, %u bytes: %08x %08x %08x %08x %08x %08x %08x %08x %s", task, id, type, bytes, dw[0], dw[1], dw[2],
		      dw[3], dw[4], dw[5], dw[6], dw[7], name);
	}
}

// M3: Compute.dispatchThreads (size: threads in the grid[3], threads per threadgroup[3]), run on the card with the function translated
// AIR -> gfx1201 (at run time by the helper, or ahead of time into src/mtlkernels.h by tools/metal/m3). Each bound buffer, from its offset
// to its end, is copied into a runtime buffer, the kernel dispatched, and the buffers it writes copied back into the guest's pages.
void RDNA4PvHost::runCompute(uint32_t task, uint32_t pipeline, const uint32_t *buffer, const uint64_t *offset, const uint64_t *size) {
	static constexpr uint64_t kMaxBuffer = 64ull << 20;   // ponytail: staged through one kernel allocation; map the guest pages if bigger is needed
	char name[64];
	uint8_t hash[32];
	uint64_t lib = 0;
	if (!functionOf(task, pipeline, name, sizeof(name), hash, &lib))
		return;
	pvlog("host: compute: task %u dispatches \"%s\" (hash %02x%02x%02x%02x%02x%02x%02x%02x...), %llu x %llu x %llu threads in groups of %llu x %llu x %llu",
	      task, name, hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], size[0], size[1], size[2], size[3], size[4], size[5]);
	// The kernel: translated now by the helper, else (no helper) from the table of src/mtlkernels.h by name.
	uint32_t prog = 0, nb = 0, written = 0;
	const char *from = "translated at run time";
	if (const Jit *j = translated(task, lib, name, hash)) {
		prog = j->program;
		nb = j->buffers;
		written = j->written;
	} else {
		for (unsigned i = 0; i < kMtlKernelCount && !prog; i++)
			if (!strcmp(name, kMtlKernels[i].name)) {
				prog = program(i);
				nb = kMtlKernels[i].buffers;
				written = kMtlKernels[i].written;
				from = "from the table";
			}
	}
	bool ok = prog && nb <= 8 && size[3] * size[4] * size[5] <= 1024;
	for (unsigned d = 0; d < 3 && ok; d++)
		ok = size[d] && size[d] < (1ull << 31) && size[d + 3];
	if (!ok) {
		pvlog("host: compute: no kernel for it (or the dispatch is out of range): not run");
		return;
	}
	uint64_t handle[8] = {}, gpu[8] = {}, va[8] = {}, bytes[8] = {}, most = 0;
	for (unsigned b = 0; b < nb && ok; b++) {
		uint64_t len = 0;
		ok = buffer[b] && bufferRange(task, buffer[b], &va[b], &len) && offset[b] < len && len - offset[b] <= kMaxBuffer;
		va[b] += offset[b];
		bytes[b] = len - offset[b];
		most = bytes[b] > most ? bytes[b] : most;
	}
	auto *stage = ok && most ? static_cast<uint8_t *>(IOMalloc(most)) : nullptr;
	ok = stage != nullptr;
	for (unsigned b = 0; b < nb && ok; b++)
		ok = gCompute->rtAlloc(this, bytes[b], handle[b], gpu[b]) == kIOReturnSuccess && readGuest(task, va[b], stage, static_cast<uint32_t>(bytes[b])) &&
		     gCompute->rtCopy(this, handle[b], 0, kernel_task, reinterpret_cast<mach_vm_address_t>(stage), bytes[b], true) == kIOReturnSuccess;
	uint64_t micros = 0;
	if (ok) {
		RDNA4Dispatch d {};
		d.program = prog;
		uint32_t dims[6];   // the kernargs after the buffers: threads per threadgroup[3], threads in the grid[3] (air2amdgpu.cpp)
		for (unsigned i = 0; i < 3; i++) {
			d.groupSize[i] = dims[i] = static_cast<uint32_t>(size[i + 3]);
			dims[i + 3] = static_cast<uint32_t>(size[i]);
			d.groups[i] = static_cast<uint32_t>((size[i] + size[i + 3] - 1) / size[i + 3]);
		}
		memcpy(d.kernargs, gpu, 8 * nb);
		memcpy(d.kernargs + 8 * nb, dims, sizeof(dims));
		d.kernargBytes = 8 * nb + sizeof(dims);
		ok = gCompute->rtDispatch(this, d, micros) == kIOReturnSuccess;
	}
	for (unsigned b = 0; b < nb && ok; b++)
		if (written & (1u << b))
			ok = gCompute->rtCopy(this, handle[b], 0, kernel_task, reinterpret_cast<mach_vm_address_t>(stage), bytes[b], false) == kIOReturnSuccess &&
			     copyGuest(task, va[b], stage, static_cast<uint32_t>(bytes[b]), true);
	for (unsigned b = 0; b < nb; b++)
		if (handle[b])
			gCompute->rtFree(this, handle[b]);
	if (stage)
		IOFree(stage, most);
	pvlog("host: compute: %s on the card (%s): %s (%llu us)", name, from, ok ? "done" : "NOT done", micros);
}

// M2: can the card write memory like the guest's (system RAM by physical address, through the AGP aperture)? Two kernel pages: SDMA fills the first
// with 0x5a5a5a5a and copies it to the second; the CPU checks both. The caches are flushed around the transfer (without, the CPU read its own
// zeros back: 0/1024 on the rig, 2026-10-21).
void RDNA4PvHost::dmaSelfTest() {
	if (!gCompute) {
		pvlog("host: dma self-test: no runtime (the GPU has not attached)");
		return;
	}
	IOBufferMemoryDescriptor *a = IOBufferMemoryDescriptor::withCapacity(4096, kIODirectionInOut, true);
	IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withCapacity(4096, kIODirectionInOut, true);
	if (a && b && a->prepare() == kIOReturnSuccess && b->prepare() == kIOReturnSuccess) {
		auto *pa = static_cast<volatile uint32_t *>(a->getBytesNoCopy());
		auto *pb = static_cast<volatile uint32_t *>(b->getBytesNoCopy());
		for (unsigned i = 0; i < 1024; i++)
			pa[i] = pb[i] = 0;
		a->performOperation(kIOMemoryIncoherentIOFlush, 0, 4096);   // the engine does not snoop the CPU's caches (seen on the rig, 2026-10-21)
		b->performOperation(kIOMemoryIncoherentIOFlush, 0, 4096);
		const uint64_t busA = a->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone), busB = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
		const bool fill = gCompute->sysFill(busA, 0x5a5a5a5a, 4096);
		const bool copy = fill && gCompute->sysCopy(busA, busB, 4096);
		a->performOperation(kIOMemoryIncoherentIOFlush, 0, 4096);
		b->performOperation(kIOMemoryIncoherentIOFlush, 0, 4096);
		unsigned goodA = 0, goodB = 0;
		for (unsigned i = 0; i < 1024; i++) {
			goodA += pa[i] == 0x5a5a5a5a;
			goodB += pb[i] == 0x5a5a5a5a;
		}
		pvlog("host: dma self-test: pages at 0x%llx and 0x%llx: fill %s, %u/1024 dwords; copy %s, %u/1024 dwords -> %s", busA, busB, fill ? "ran" : "FAILED",
		      goodA, copy ? "ran" : "FAILED", goodB, goodA == 1024 && goodB == 1024 ? "PASS" : "FAIL");
		a->complete();
		b->complete();
	} else {
		pvlog("host: dma self-test: no test pages");
	}
	OSSafeReleaseNULL(a);
	OSSafeReleaseNULL(b);
}

// GetDeviceInfo (FIFO 0x3a): the record 0x2d names the 4 KiB buffer the host fills; the layout and the values are docs/m0-pvgpu.md "The GetDeviceInfo reply".
void RDNA4PvHost::answerDeviceInfo(const pvstream::FifoCommand &c, const uint8_t *cmd) {
	const uint8_t *pl = cmd + c.payloadOffset;
	const uint32_t plen = c.length - c.payloadOffset;
	uint32_t rec[3] = {};
	if (plen >= sizeof(rec))
		memcpy(rec, pl, sizeof(rec));
	if (plen < sizeof(rec) || rec[0] != pvstream::kDeviceInfoRecord) {
		pvlog("host: GetDeviceInfo without a 0x2d record (%u payload bytes): not answered", plen);
		return;
	}
	const uint32_t bytes = rec[1] * 8, page = rec[2];
	if (bytes < 8 || bytes > 0x10000 || !page) {
		pvlog("host: GetDeviceInfo reply buffer refused: page 0x%x, %u bytes", page, bytes);
		return;
	}
	IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(page) << 12, bytes, kIODirectionInOut);
	IOMemoryMap *map = desc ? desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
	if (!map) {
		pvlog("host: cannot map the GetDeviceInfo reply buffer (page 0x%x, %u bytes)", page, bytes);
		OSSafeReleaseNULL(desc);
		return;
	}
	const size_t pairs = pvstream::buildDeviceInfoReply(reinterpret_cast<uint8_t *>(map->getVirtualAddress()), bytes, kGpuCoreCount);
	__sync_synchronize();
	map->release();
	desc->release();
	mReplies++;
	pvlog("host: GetDeviceInfo answered: reply buffer page 0x%x (%u bytes, %u pair slots), %u pairs written, GpuCoreCount %u", page, bytes, bytes / 8,
	      static_cast<unsigned>(pairs), kGpuCoreCount);
}

// DisplaySetupSharedState (FIFO 0x01, AppleParavirtDisplayPipe::setupSharedState): {u32 port, u32 page}. After its wait the driver asserts that the host wrote the
// pipe's port number into the shared state page as a u16 at +0x12 (a mismatch panics the guest kernel: "fSharedState->port == fPort") and copies the u32 at +0x1c
// into the pipe (0 = SubmitTransaction is FIFO 0x06). The page stays mapped: it is where the host's display events go (docs/m1-stream.md s.1b). With the
// online event on, the host also fills the display's info and sets the *online* pending bit: process_online runs when the guest enables the pipe (enable()
// signals its own pending bits) or when the interrupt arrives afterwards.
void RDNA4PvHost::answerDisplaySharedState(const pvstream::FifoCommand &c, const uint8_t *cmd, uint32_t channel) {
	const uint8_t *pl = cmd + c.payloadOffset;
	if (c.length - c.payloadOffset < 8)
		return;
	uint32_t port, page;
	memcpy(&port, pl, 4);
	memcpy(&page, pl + 4, 4);
	if (!page || port >= pvstream::disp::kMaxPorts) {
		pvlog("host: DisplaySetupSharedState refused: port %u, page 0x%x (ports 0..%u)", port, page, pvstream::disp::kMaxPorts - 1);
		return;
	}
	Pipe &p = mPipe[port];
	if (p.live) {
		OSSafeReleaseNULL(p.map);
		OSSafeReleaseNULL(p.desc);
		p = Pipe();
	}
	p.desc = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(page) << 12, 0x1000, kIODirectionInOut);
	p.map = p.desc ? p.desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
	if (!p.map) {
		pvlog("host: cannot map the display shared state page 0x%x", page);
		OSSafeReleaseNULL(p.desc);
		p = Pipe();
		return;
	}
	uint8_t *va = reinterpret_cast<uint8_t *>(p.map->getVirtualAddress());
	p.page = page;
	p.channel = channel;
	if (mDisp & kDispOnline) {
		pvstream::disp::Info info = mDispInfo;
		info.id = port + 1;
		pvstream::disp::fillInfo(va, port, info);
		__sync_synchronize();
		__atomic_fetch_or(reinterpret_cast<uint32_t *>(va + pvstream::disp::kPending), pvstream::disp::kEventOnline, __ATOMIC_SEQ_CST);
		mOnlineEvents++;
		pvlog("host: display shared state page 0x%x: port %u written at +0x12; display info filled (%ux%u, %u mode(s), name \"%s\", transactions via FIFO 0x%02x), "
		      "online event pending (+0x100 bit 2)",
		      page, port, info.width, info.height, info.modeCount, info.name, info.txnProtocol ? 0x07 : 0x06);
	} else {
		const uint16_t port16 = static_cast<uint16_t>(port);
		const uint32_t zero = 0;
		memcpy(va + pvstream::disp::kPort, &port16, 2);
		memcpy(va + pvstream::disp::kTxnProtocol, &zero, 4);
		__sync_synchronize();
		pvlog("host: display shared state page 0x%x: port %u written at +0x12", page, port);
	}
	p.live = true;
	p.lastEnabled = *reinterpret_cast<volatile uint32_t *>(va + pvstream::disp::kEnabled);
	memcpy(&p.cursorPos, va + pvstream::disp::kCursorPos, 4);      // what the guest left there is the baseline for the change log
	p.cursorVisible = va[pvstream::disp::kCursorVisible];
}

// DisplayProcessOnline (FIFO 0x02): the guest ran process_online (connectionChange done) and acknowledges with {port, shared+0x200}.
void RDNA4PvHost::answerDisplayOnline(const pvstream::FifoCommand &c, const uint8_t *cmd) {
	const uint8_t *pl = cmd + c.payloadOffset;
	if (c.length - c.payloadOffset < 8)
		return;
	uint32_t port, cookie;
	memcpy(&port, pl, 4);
	memcpy(&cookie, pl + 4, 4);
	mOnlineAcks++;
	pvlog("host: display pipe %u acknowledged the online event (cookie 0x%x, %s)", port, cookie, cookie == mDispInfo.cookie ? "as filled" : "NOT the filled value");
}

void RDNA4PvHost::dropPipes() {
	for (Pipe &p : mPipe) {
		OSSafeReleaseNULL(p.map);
		OSSafeReleaseNULL(p.desc);
		p = Pipe();
	}
	if (volatile uint32_t *c = mNub->ctrl())
		c[kRegIntrStatusDisp / 4] &= ~mDispBits;
	mDispBits = 0;
}

// The shared-state page is guest memory the host only borrows: the guest frees it when the pipe goes (teardownSharedState sends nothing), after which the
// physical page can be anything, and the host writes into it every frame. So before every host write the page must still look like the pipe's: the u16 at +0x12
// is the port the host wrote there, and the enabled mask holds only the three event bits the driver ever stores (0xc, 0, 1, 0xd). If not, the pipe is let go and
// never written again. (The check cannot close the race between it and the write; it closes the long window after a teardown.)
bool RDNA4PvHost::pipeStillOurs(uint32_t port) {
	const Pipe &p = mPipe[port];
	if (!p.live || !p.map)
		return false;
	const uint8_t *va = reinterpret_cast<const uint8_t *>(p.map->getVirtualAddress());
	uint16_t seen;
	memcpy(&seen, va + pvstream::disp::kPort, 2);
	const uint32_t enabled = *reinterpret_cast<const volatile uint32_t *>(va + pvstream::disp::kEnabled);
	const uint32_t known = pvstream::disp::kEventVbl | pvstream::disp::kEventOnline | pvstream::disp::kEventOffline;
	if (seen == port && !(enabled & ~known))
		return true;
	pvlog("host: display pipe %u: shared state page 0x%x is no longer ours (port field %u, enabled mask 0x%x): unmapped, no more writes to it", port, p.page, seen, enabled);
	return false;
}

void RDNA4PvHost::dropPipe(uint32_t port, const char *why) {
	Pipe &p = mPipe[port];
	if (!p.live && !p.map)
		return;
	if (why)
		pvlog("host: display pipe %u dropped: %s", port, why);
	OSSafeReleaseNULL(p.map);
	OSSafeReleaseNULL(p.desc);
	p = Pipe();
	if (volatile uint32_t *c = mNub->ctrl())
		c[kRegIntrStatusDisp / 4] &= ~(1u << port);
	mDispBits &= ~(1u << port);
}

// Each tick, for every live pipe: log what the guest enabled and what the cursor does, add a VBL event when a frame is due and the guest enabled VBL, and keep
// INTR_STATUS_DISP bit `port` asserted for as long as an enabled event is pending (the guest's signalDisplay takes the pending bits atomically, which is what
// drops it again). Level-triggered on purpose: the handler reads the register when its work-loop thread gets to it, which can be later than our next tick, and a
// missed *online* event would stall the display for good (a missed stamp costs a timed wait, a missed VBL costs a frame). The interrupt itself is re-raised when
// a bit is new and otherwise at most every 10 ms.
void RDNA4PvHost::pollDisplay() {
	volatile uint32_t *c = mNub->ctrl();
	if (!c)
		return;
	{   // the cursor kick (hardware-cursor mode): informational only, the shared-state fields below carry the data
		const uint32_t kick = c[kRegCursorKick / 4];
		if (kick != mKickPrev) {
			pvlog("host: ctrl+0x%03x (cursor kick) 0x%08x -> 0x%08x", kRegCursorKick, mKickPrev, kick);
			mKickPrev = kick;
		}
	}
	const uint64_t now = nowNs();
	bool frame = false;
	if (mDisp & kDispVbl) {
		if (!mNextVblNs)
			mNextVblNs = now + kVblPeriodNs;
		if (now >= mNextVblNs) {
			frame = true;
			// the next frame is one period after the last due time; after a long stall start over instead of firing a burst
			mNextVblNs = now - mNextVblNs > 4 * kVblPeriodNs ? now + kVblPeriodNs : mNextVblNs + kVblPeriodNs;
		}
	}
	uint32_t want = 0;
	for (uint32_t port = 0; port < pvstream::disp::kMaxPorts; port++) {
		Pipe &p = mPipe[port];
		if (!p.live)
			continue;
		if (!pipeStillOurs(port)) {
			dropPipe(port, nullptr);
			continue;
		}
		uint8_t *va = reinterpret_cast<uint8_t *>(p.map->getVirtualAddress());
		uint32_t *pending = reinterpret_cast<uint32_t *>(va + pvstream::disp::kPending);
		const uint32_t enabled = *reinterpret_cast<volatile uint32_t *>(va + pvstream::disp::kEnabled);
		if (enabled != p.lastEnabled) {
			pvlog("host: display pipe %u: enabled mask 0x%x -> 0x%x%s", port, p.lastEnabled, enabled,
			      (enabled & pvstream::disp::kEventOnline) && !(p.lastEnabled & pvstream::disp::kEventOnline) ? " (enable())" : "");
			p.lastEnabled = enabled;
		}
		if (frame && (enabled & pvstream::disp::kEventVbl)) {
			__atomic_fetch_or(pending, pvstream::disp::kEventVbl, __ATOMIC_SEQ_CST);
			if (mVblFrames++ < 3 || mVblFrames % 600 == 0)
				pvlog("host: VBL event %u on display pipe %u (+0x100 bit 0)", mVblFrames, port);
		}
		if (__atomic_load_n(pending, __ATOMIC_SEQ_CST) & enabled)
			want |= 1u << port;
		// the cursor state the guest keeps in the page (hardware-cursor mode): log changes
		uint32_t pos;
		memcpy(&pos, va + pvstream::disp::kCursorPos, 4);
		const uint8_t visible = va[pvstream::disp::kCursorVisible];
		if ((pos != p.cursorPos || visible != p.cursorVisible) && p.cursorLines++ < 12)
			pvlog("host: display pipe %u cursor: position (%u, %u)%s, %s", port, pos & 0xffff, pos >> 16, pos == 0xffffffff ? " [unset]" : "", visible ? "visible" : "hidden");
		p.cursorPos = pos;
		p.cursorVisible = visible;
	}
	const uint32_t fresh = want & ~mDispBits;
	if (want != mDispBits)
		c[kRegIntrStatusDisp / 4] = (c[kRegIntrStatusDisp / 4] & ~mDispBits) | want;
	mDispBits = want;
	if (want && (fresh || now - mLastDispIrqNs >= 10000000ull)) {
		mLastDispIrqNs = now;
		mNub->raiseInterrupt(0);
	}
}

// Completion of a command that asked for a stamp: the driver's event machine tests `*stampPtr[index] >= value` with stampPtr[index] = (FIFO buffer page)+4*index
// (IOAccelEventMachine2::getStampOffset); the root channel is index 0, a child channel's index is the `kind` of its state record. Then INTR_STATUS_GPU bit
// `index` and the MSI wake the waiter (signalStamps); without the interrupt the waiter finds the stamp after its next timed wake-up.
void RDNA4PvHost::completeStamp(uint32_t index, uint32_t value) {
	if (!mFifoMap || index >= 0x1000 / 4)
		return;
	volatile uint32_t *stamps = reinterpret_cast<volatile uint32_t *>(mFifoMap->getVirtualAddress());
	if (static_cast<int32_t>(value - stamps[index]) > 0)
		stamps[index] = value;
	__sync_synchronize();
	if (volatile uint32_t *c = mNub->ctrl()) {
		if (index < 32) {
			c[kRegIntrStatusGpu / 4] |= 1u << index;
			mIrqBits |= 1u << index;
		}
	}
	const bool delivered = mNub->raiseInterrupt(0);
	static uint32_t logged = 0;
	if (logged++ < 12 || !delivered)
		pvlog("host: stamp[%u] = %u written (FIFO page offset 0x%x), INTR_STATUS_GPU bit %u set, interrupt 0 %s", index, value, index * 4, index,
		      delivered ? "raised" : "not raised (no handler registered yet)");
}

// ---- child channels ----
//
// The driver defines a channel in two steps (AppleParavirtVirtualChannel::init): it fills the channel's 20-byte state record in the ROOT page at
// +0x400 + (n - 1) * 20 = {u32 write, u32 read, u32 0, u16 kind (the stamp index), u16 0, u32 page-list page}, then sends FIFO command 0x30 {n}. The ring is the
// pages listed in the page-list page (one u32 page number per 4 KiB of ring, zero-terminated since the page was zeroed). Commands are appended to the ring
// with the root FIFO's framing; write/read are monotonic byte counters (position = counter % ring size); the guest rings ctrl+0x20 with the channel number
// only when the ring was empty, so the host polls every defined channel.

void RDNA4PvHost::mapRoot() {
	volatile uint32_t *c = mNub->ctrl();
	if (!c)
		return;
	const uint32_t pfn = c[kRegRootPage / 4];
	if (pfn == mRootPfn)
		return;
	dropChannels();
	OSSafeReleaseNULL(mRootMap);
	OSSafeReleaseNULL(mRootDesc);
	mRootPfn = 0;
	if (!pfn)
		return;
	mRootDesc = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(pfn) << 12, 0x1000, kIODirectionInOut);
	mRootMap = mRootDesc ? mRootDesc->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
	if (!mRootMap) {
		static uint32_t fails = 0;
		if (fails++ < 4)
			pvlog("host: cannot map the root page 0x%x", pfn);
		OSSafeReleaseNULL(mRootDesc);
		return;
	}
	mRootPfn = pfn;
	pvlog("host: root page 0x%x mapped (channel state records at +0x%x)", pfn, kRecordOffset);
	for (uint32_t n = 1; n <= kMaxChannels; n++)
		if (mPendingDefine & (1u << n)) {
			mPendingDefine &= ~(1u << n);
			defineChannel(n);
		}
}

void RDNA4PvHost::dropChannels() {
	dropPipes();
	for (Channel &ch : mCh) {
		OSSafeReleaseNULL(ch.map);
		OSSafeReleaseNULL(ch.desc);
		ch = Channel();
	}
	mPendingDefine = 0;
}

void RDNA4PvHost::freeChannel(uint32_t n) {
	if (n < 1 || n > kMaxChannels || !mCh[n].live)
		return;
	OSSafeReleaseNULL(mCh[n].map);
	OSSafeReleaseNULL(mCh[n].desc);
	mCh[n] = Channel();
	pvlog("host: channel %u freed", n);
	for (uint32_t port = 0; port < pvstream::disp::kMaxPorts; port++)
		if (mPipe[port].live && mPipe[port].channel == n)
			dropPipe(port, "its channel was freed");
}

void RDNA4PvHost::defineChannel(uint32_t n) {
	if (n < 1 || n > kMaxChannels) {
		pvlog("host: DefineChannel %u refused (channels 1..%u are supported)", n, kMaxChannels);
		return;
	}
	if (!mRootMap) {
		mPendingDefine |= 1u << n;
		pvlog("host: DefineChannel %u before the root page was announced: kept for later", n);
		return;
	}
	freeChannel(n);
	const uint8_t *root = reinterpret_cast<const uint8_t *>(mRootMap->getVirtualAddress());
	const uint32_t off = kRecordOffset + (n - 1) * kRecordBytes;       // < 0x1000 for every n <= kMaxChannels
	uint32_t rec[kRecordBytes / 4];
	memcpy(rec, root + off, sizeof(rec));
	const uint32_t listPfn = rec[4];
	const uint32_t kind = rec[3] & 0xffff;
	if (!listPfn) {
		pvlog("host: channel %u: state record has no page list (record %08x %08x %08x %08x %08x)", n, rec[0], rec[1], rec[2], rec[3], rec[4]);
		return;
	}
	IOMemoryDescriptor *listDesc = IOMemoryDescriptor::withPhysicalAddress(static_cast<IOPhysicalAddress>(listPfn) << 12, 0x1000, kIODirectionIn);
	IOMemoryMap *listMap = listDesc ? listDesc->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
	if (!listMap) {
		pvlog("host: channel %u: cannot map the page list 0x%x", n, listPfn);
		OSSafeReleaseNULL(listDesc);
		return;
	}
	uint32_t pages = 0;
	const uint32_t *list = reinterpret_cast<const uint32_t *>(listMap->getVirtualAddress());
	while (pages < kMaxRingPages && pages < 0x1000 / 4 && list[pages])
		pages++;
	IOAddressRange *ranges = pages ? static_cast<IOAddressRange *>(IOMalloc(pages * sizeof(IOAddressRange))) : nullptr;
	if (ranges) {
		for (uint32_t i = 0; i < pages; i++) {
			ranges[i].address = static_cast<mach_vm_address_t>(list[i]) << 12;
			ranges[i].length = 0x1000;
		}
		mCh[n].desc = IOMemoryDescriptor::withAddressRanges(ranges, pages, kIODirectionInOut | kIOMemoryTypePhysical64, nullptr);
		IOFree(ranges, pages * sizeof(IOAddressRange));
	}
	listMap->release();
	listDesc->release();
	mCh[n].map = mCh[n].desc ? mCh[n].desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
	if (!mCh[n].map) {
		pvlog("host: channel %u: cannot map its ring (%u pages listed in page 0x%x)", n, pages, listPfn);
		OSSafeReleaseNULL(mCh[n].desc);
		mCh[n] = Channel();
		return;
	}
	mCh[n].live = true;
	mCh[n].kind = kind;
	mCh[n].ringBytes = pages * 0x1000;
	pvlog("host: channel %u defined: stamp index %u, ring of %u pages (%u bytes) from page list 0x%x, counters write %u read %u", n, kind, pages, pages * 0x1000, listPfn,
	      rec[0], rec[1]);
}

void RDNA4PvHost::pollChannels() {
	if (!mRootMap)
		return;
	uint8_t *root = reinterpret_cast<uint8_t *>(mRootMap->getVirtualAddress());
	for (uint32_t n = 1; n <= kMaxChannels; n++) {
		Channel &ch = mCh[n];
		if (!ch.live)
			continue;
		volatile uint32_t *rec = reinterpret_cast<volatile uint32_t *>(root + kRecordOffset + (n - 1) * kRecordBytes);
		const uint32_t written = rec[0], read = rec[1];
		if (written == read)
			continue;
		const uint32_t pending = written - read;
		if (pending > ch.ringBytes) {
			pvlog("host: channel %u claims %u bytes pending in a %u byte ring (read %u, written %u): resynchronising", n, pending, ch.ringBytes, read, written);
			rec[1] = written;
			continue;
		}
		char tag[12];
		snprintf(tag, sizeof(tag), "ch%u", n);
		mChannelPolls++;
		consume(CmdSource { tag, reinterpret_cast<const uint8_t *>(ch.map->getVirtualAddress()), ch.ringBytes, ch.kind, n }, read, pending);
		rec[1] = written;   // the guest compares it with the write counter to see that the ring is empty and how much room is left
	}
}

// ---- self-test: plays the guest driver against the nub (the register sequence of AppleParavirtAccelerator, s.6.1) ----

class RDNA4PvTestOwner : public OSObject {
	OSDeclareDefaultStructors(RDNA4PvTestOwner)
public:
	static void action(OSObject *owner, IOInterruptEventSource *, int count) {
		// `count`: how many interrupts were coalesced into this call of the work loop
		if (auto *o = OSDynamicCast(RDNA4PvTestOwner, owner))
			__atomic_add_fetch(&o->host->mTestIrqSeen, count > 0 ? static_cast<uint32_t>(count) : 1u, __ATOMIC_RELAXED);
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
		// setupRoot: the 4 KiB root page, announced before the FIFO; and one child channel the way VirtualChannel::init builds it: a 12 KiB buffer whose first
		// page is the page list, pages 1 and 2 the ring (8 KiB), the channel's state record in the root page, then (below) FIFO command DefineChannel
		mTestRoot = IOBufferMemoryDescriptor::withOptions(0x890, 0x1000, 1);
		mTestChan = IOBufferMemoryDescriptor::withOptions(0x890, 0x3000, 1);
		testCheck(mTestRoot && mTestChan && mTestRoot->prepare() == kIOReturnSuccess && mTestChan->prepare() == kIOReturnSuccess,
		          "root page and a child channel (page list + 2 ring pages) allocated and wired");
		if (!mTestRoot || !mTestChan) {
			mStep = kStepDone;
			break;
		}
		uint8_t *rootPage = static_cast<uint8_t *>(mTestRoot->getBytesNoCopy());
		uint8_t *chanMem = static_cast<uint8_t *>(mTestChan->getBytesNoCopy());
		bzero(rootPage, 0x1000);
		bzero(chanMem, 0x3000);
		const uint32_t chanPfn = static_cast<uint32_t>(mTestChan->getPhysicalAddress() >> 12);
		const uint32_t chanRing = 0x2000, chanNumber = 3, chanKind = 3;
		reinterpret_cast<uint32_t *>(chanMem)[0] = chanPfn + 1;     // page list: the ring's pages
		reinterpret_cast<uint32_t *>(chanMem)[1] = chanPfn + 2;
		uint32_t *record = reinterpret_cast<uint32_t *>(rootPage + 0x400 + (chanNumber - 1) * 20);
		record[0] = chanRing - 8;     // write counter: start 8 bytes before the end of the ring so the first command wraps
		record[1] = chanRing - 8;     // read counter
		record[2] = 0;
		record[3] = chanKind;         // u16 kind (stamp index), u16 0
		record[4] = chanPfn;          // page-list page
		c[kRegRootPage / 4] = static_cast<uint32_t>(mTestRoot->getPhysicalAddress() >> 12);
		c[kRegFifoBasePage / 4] = static_cast<uint32_t>(phys >> 12);
		c[kRegFifoLength / 4] = static_cast<uint32_t>(mTestFifo->getLength());
		c[kRegFifoStart / 4] = 0x1000;
		c[kRegControlFifo / 4] = 1;

		// 4. setupInterrupts first (the driver registers its handler before it submits anything): an IOInterruptEventSource on the provider, index 0
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

		// 5. setupDeviceInfo: the 4 KiB reply buffer, then writeFifo of two FIFO commands as the driver builds them, the first crossing the ring's end:
		//    DefineChannel 3 (16 bytes), GetDeviceInfo with its 0x2d record and signal 1 (24 bytes)
		mTestReply = IOBufferMemoryDescriptor::withOptions(0x890, 0x1000, 1);
		testCheck(mTestReply && mTestReply->prepare() == kIOReturnSuccess, "GetDeviceInfo reply buffer (4 KiB) allocated and wired");
		if (!mTestReply) {
			mStep = kStepDone;
			break;
		}
		bzero(mTestReply->getBytesNoCopy(), 0x1000);
		const uint32_t replyPage = static_cast<uint32_t>(mTestReply->getPhysicalAddress() >> 12);
		const uint32_t ringLen = 0x10000 - 0x1000;
		uint32_t written = ringLen - 8;
		c[kRegFifoRead / 4] = written;     // (the host's view of where it stands; the poller resynchronises to it on the first announcement)
		mConsumed = written;
		mTestShared = IOBufferMemoryDescriptor::withOptions(0x890, 0x1000, 1);
		testCheck(mTestShared && mTestShared->prepare() == kIOReturnSuccess, "display shared state page (4 KiB) allocated and wired");
		if (!mTestShared) {
			mStep = kStepDone;
			break;
		}
		bzero(mTestShared->getBytesNoCopy(), 0x1000);
		const uint32_t sharedPage = static_cast<uint32_t>(mTestShared->getPhysicalAddress() >> 12);
		mTestShared2 = IOBufferMemoryDescriptor::withOptions(0x890, 0x1000, 1);
		testCheck(mTestShared2 && mTestShared2->prepare() == kIOReturnSuccess, "second display shared state page (for the child channel) allocated and wired");
		if (!mTestShared2) {
			mStep = kStepDone;
			break;
		}
		bzero(mTestShared2->getBytesNoCopy(), 0x1000);
		// the Display pipe's command on the child channel: DisplaySetupSharedState {port 5, page}, signal 7, 20 bytes across the ring's wrap; then the write counter
		const uint32_t childCmd[5] = { 0x00000001, 20, 7, 5, static_cast<uint32_t>(mTestShared2->getPhysicalAddress() >> 12) };
		for (uint32_t i = 0; i < sizeof(childCmd); i++)
			chanMem[0x1000 + (record[0] + i) % chanRing] = reinterpret_cast<const uint8_t *>(childCmd)[i];
		__sync_synchronize();
		record[0] += sizeof(childCmd);
		const uint32_t cmds[15] = {
			0x00000030, 16, 0, 3,                                          // {u16 id 0x30, u16 0}, length 16, signal 0, channel 3
			0x0000003a, 24, 1, pvstream::kDeviceInfoRecord, 0x200, replyPage, // {u16 id 0x3a, u16 0}, length 24, signal 1, record {0x2d, 4096/8, page}
			0x00000001, 20, 2, 2, sharedPage                                 // DisplaySetupSharedState: length 20, signal 2, {port 2, page}
		};
		const uint32_t cmdBytes = sizeof(cmds);
		for (uint32_t i = 0; i < cmdBytes; i++)
			fifo[0x1000 + (written + i) % ringLen] = reinterpret_cast<const uint8_t *>(cmds)[i];
		written += cmdBytes;
		c[kRegFifoWritten / 4] = written;
		mTestExpect = written;
		mStep = kStepWaitFifo;
		mStepDeadline = nowMs() + 300;
		break;
	}
	case kStepWaitFifo: {
		volatile uint32_t *c = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *>(mTestBar->getVirtualAddress()) + kCtrl);
		const uint8_t *fifo = static_cast<const uint8_t *>(mTestFifo->getBytesNoCopy());
		const bool consumed = c[kRegFifoRead / 4] == c[kRegFifoWritten / 4];
		const volatile uint32_t *stamps = reinterpret_cast<const volatile uint32_t *>(fifo);
		if (!(consumed && mReplies >= 1 && stamps[0] == 2 && stamps[3] == 7) && nowMs() < mStepDeadline)
			return;
		testCheck(consumed, "host consumed the FIFO (FIFO_READ caught up with FIFO_WRITTEN)");
		testCheck(mFifoMap != nullptr && mFifoPfn == c[kRegFifoBasePage / 4], "host mapped the announced FIFO page");
		testCheck(mCmds >= 4, "host decoded the four FIFO commands (three on the root FIFO, one on the child channel)");
		testCheck(mReplies == 1, "host answered GetDeviceInfo once");
		// the reply: exactly what buildDeviceInfoReply gives, and Apple's parser (mirror) reads it into the struct the Metal bundle will see
		uint8_t expect[0x1000];
		pvstream::buildDeviceInfoReply(expect, sizeof(expect), kGpuCoreCount);
		const uint8_t *reply = static_cast<const uint8_t *>(mTestReply->getBytesNoCopy());
		testCheck(memcmp(reply, expect, sizeof(expect)) == 0, "GetDeviceInfo reply buffer holds the documented key/value pairs");
		uint8_t info[pvstream::kDeviceInfoBytes] = {};
		pvstream::parseDeviceInfoReference(reply, 0x1000 / 8, info);
		uint32_t msaa, gpuCores, shaderVersion;
		memcpy(&msaa, info + 0x00, 4);
		memcpy(&gpuCores, info + 0x84, 4);
		memcpy(&shaderVersion, info + 0x44, 4);
		testCheck(msaa == 4 && info[0xac] == 1 && gpuCores == kGpuCoreCount && info[0xcd] == 1 && info[0xb5] == 0 && shaderVersion == 0x20002,
		          "parseDeviceInfo (mirror): MSAASamples 4, GpuCoreCount 64, DeserializerVersion undefined, shader version defaulted to 2.2");
		// the completion: stamp 0 = the command's signal, the way IOAccelEventMachine2 tests it
		testCheck(*reinterpret_cast<const volatile uint32_t *>(fifo) == 2, "stamp[0] == 2 in the FIFO page (the last signal of the batch: GetDeviceInfo 1, display 2)");
		uint16_t port16;
		memcpy(&port16, static_cast<const uint8_t *>(mTestShared->getBytesNoCopy()) + 0x12, 2);
		testCheck(port16 == 2, "display shared state page: port 2 written at +0x12 (what AppleParavirtDisplayPipe asserts)");
		// the child channel: defined from its state record, its ring read through the page list, the command on it answered, its stamp and its record updated
		testCheck(mCh[3].live && mCh[3].kind == 3 && mCh[3].ringBytes == 0x2000, "child channel 3 defined from the root page record (stamp index 3, ring of 2 pages)");
		testCheck(stamps[3] == 7, "stamp[3] == 7 in the FIFO page (the child command's signal)");
		memcpy(&port16, static_cast<const uint8_t *>(mTestShared2->getBytesNoCopy()) + 0x12, 2);
		testCheck(port16 == 5, "child channel: DisplaySetupSharedState answered: port 5 written at +0x12");
		const uint32_t *rec = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(mTestRoot->getBytesNoCopy()) + 0x400 + 2 * 20);
		testCheck(rec[1] == rec[0] && rec[0] == 0x1ff8 + 20, "child channel record: read counter caught up with write counter (0x200c)");
		mStep = kStepWaitHostIrq;
		mStepDeadline = nowMs() + 300;
		break;
	}
	case kStepWaitHostIrq: {
		volatile uint32_t *c = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *>(mTestBar->getVirtualAddress()) + kCtrl);
		if (mTestIrqSeen < 2 && nowMs() < mStepDeadline)
			return;
		testCheck(mTestIrqSeen == 2, "host raised interrupt 0 for both stamps (root 0, child 3) and the registered handler saw two");
		testCheck((c[kRegIntrStatusGpu / 4] & 9) == 0, "INTR_STATUS_GPU bits 0 and 3 dropped again by the host");
		// and an explicit interrupt, as before
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
		mStep = kStepDisplay;
		mDispPhase = 0;
		break;
	}
	case kStepDisplay: {
		// The display side, with the test playing the guest's half of AppleParavirtDisplayPipe: enable(), signalDisplay's atomic take of the pending bits,
		// enableVBLInterrupt, then the pipe's commands on its channel and the cursor fields. Pipe port 2 is the root-FIFO one (page mTestShared), port 5 the
		// child channel's (page mTestShared2, channel 3, stamp 3).
		using namespace pvstream::disp;
		volatile uint32_t *c = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *>(mTestBar->getVirtualAddress()) + kCtrl);
		uint8_t *page1 = static_cast<uint8_t *>(mTestShared->getBytesNoCopy());
		uint8_t *page2 = static_cast<uint8_t *>(mTestShared2->getBytesNoCopy());
		auto pending = [](uint8_t *pg) { return reinterpret_cast<uint32_t *>(pg + kPending); };
		auto enabled = [](uint8_t *pg) { return reinterpret_cast<uint32_t *>(pg + kEnabled); };
		auto take = [&](uint8_t *pg) {   // DisplayPipe::signalDisplay: clear the enabled bits of the pending word with a compare-exchange, act on those
			const uint32_t en = __atomic_load_n(enabled(pg), __ATOMIC_SEQ_CST);
			uint32_t old = __atomic_load_n(pending(pg), __ATOMIC_SEQ_CST);
			while (!__atomic_compare_exchange_n(pending(pg), &old, old & ~en, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
			return old & en;
		};
		switch (mDispPhase) {
		case 0: {
			InfoRead r;
			readInfo(page1, &r);
			testCheck(r.port == 2 && r.id == 3 && r.width == 1920 && r.height == 1080 && r.cursorWidth == 64 && r.cursorHeight == 64 && r.txnProtocol == 0 &&
			              r.count == 1 && r.modes[0].width == 1920 && r.modes[0].height == 1080 && r.modes[0].refresh == (60u << 16) && r.cookie == 1 &&
			              !strcmp(r.name, "AMD RDNA4 PV"),
			          "display info in the shared state page: port, 1920x1080 @ 60, cursor 64x64, FIFO 0x06, one mode");
			testCheck(__atomic_load_n(pending(page1), __ATOMIC_SEQ_CST) == kEventOnline && __atomic_load_n(pending(page2), __ATOMIC_SEQ_CST) == kEventOnline &&
			              mOnlineEvents == 2,
			          "online event (+0x100 bit 2) pending on both pipes");
			testCheck((c[kRegIntrStatusDisp / 4] & 0xff) == 0, "INTR_STATUS_DISP quiet while no pipe is enabled");
			mTestIrqSeen = 0;
			__atomic_store_n(enabled(page1), 0xc, __ATOMIC_SEQ_CST);   // DisplayPipe::enable()
			mDispT0 = nowMs();
			mDispPhase = 1;
			return;
		}
		case 1: {
			if (!(c[kRegIntrStatusDisp / 4] & (1u << 2)) && nowMs() < mDispT0 + 300)
				return;
			testCheck(c[kRegIntrStatusDisp / 4] & (1u << 2), "INTR_STATUS_DISP bit 2 (pipe port 2) set once the pipe is enabled");
			testCheck(c[kRegIntrStatusDisp / 4] == (1u << 2), "... and only that bit (pipe 5 is not enabled)");
			testCheck(take(page1) == kEventOnline, "signalDisplay finds the online event and clears it");
			mDispT0 = nowMs();
			mDispPhase = 2;
			return;
		}
		case 2: {
			if ((c[kRegIntrStatusDisp / 4] & (1u << 2)) && nowMs() < mDispT0 + 300)
				return;
			testCheck(!(c[kRegIntrStatusDisp / 4] & (1u << 2)), "INTR_STATUS_DISP bit 2 dropped once the event was taken");
			testCheck(mTestIrqSeen >= 1, "the interrupt was raised for it");
			// enableVBLInterrupt on the child channel's pipe: ~60 Hz for 250 ms
			__atomic_store_n(enabled(page2), kEventVbl, __ATOMIC_SEQ_CST);
			mTestVbls = 0;
			mTestSawBit = 0;
			mDispT0 = nowMs();
			mDispPhase = 3;
			return;
		}
		case 3: {
			if (c[kRegIntrStatusDisp / 4] & (1u << 5))
				mTestSawBit = 1;
			if (take(page2) & kEventVbl)
				mTestVbls++;
			if (nowMs() < mDispT0 + 250)
				return;
			testCheck(mTestVbls >= 5 && mTestVbls <= 20, "VBL: 5..20 events in 250 ms on the pipe that enabled VBL (60 Hz = 15)");
			testCheck(mTestSawBit, "... each raised INTR_STATUS_DISP bit 5");
			testCheck(!(__atomic_load_n(pending(page1), __ATOMIC_SEQ_CST) & kEventVbl), "no VBL event on the pipe whose guest did not enable it");
			pvlog("selftest: %u VBL events in 250 ms", mTestVbls);
			__atomic_store_n(enabled(page2), 0, __ATOMIC_SEQ_CST);     // disableVBLInterrupt / disable()
			mDispT0 = nowMs();
			mDispPhase = 4;
			return;
		}
		case 4: {
			if (nowMs() < mDispT0 + 20)   // let the host see the mask and drop bit 5
				return;
			testCheck((c[kRegIntrStatusDisp / 4] & 0xff) == 0, "INTR_STATUS_DISP quiet again after VBL was disabled");
			// the pipe's other commands on its channel (child channel 3, ring after the DisplaySetupSharedState written earlier): ProcessOnline ack, cursor state and
			// glyph, SubmitTransaction 0x06 and 0x07, FlushChannelEvent; the last three carry signals 21, 22, 23
			const uint32_t chanRing = 0x2000;
			uint8_t *rootPage = static_cast<uint8_t *>(mTestRoot->getBytesNoCopy());
			uint8_t *chanMem = static_cast<uint8_t *>(mTestChan->getBytesNoCopy());
			uint32_t *record = reinterpret_cast<uint32_t *>(rootPage + 0x400 + (3 - 1) * 20);
			uint32_t words[80];
			uint32_t n = 0;
			auto cmd = [&](uint16_t id, uint32_t signal, const uint32_t *payload, uint32_t count) {
				words[n++] = id;                                    // {u16 id, u16 0}
				words[n++] = 12 + count * 4;
				words[n++] = signal;
				for (uint32_t i = 0; i < count; i++)
					words[n++] = payload[i];
			};
			const uint32_t ack[] = { 5, 1 };                                                                     // ProcessOnline acknowledgement {port, cookie}
			const uint32_t cursorState[] = { 5, 1 };                                                              // {port, visible}
			const uint32_t glyph[] = { 5, 3, 0x7000, 0, 0x8000, 0, 256, 0, 64 | (48u << 16), 3 | (5u << 16), 0x99 };   // cursor glyph, 44 bytes
			const uint32_t txn6[] = { 5, 77, 3 };                                                                  // SubmitTransaction {port, surface, task}
			const uint32_t txn7[] = { 5, 3, 77, 0, 0, 0, 0, 0, 0 };                                                // SubmitTransaction2: 36 bytes, no gamma table
			cmd(0x02, 0, ack, 2);
			cmd(0x05, 0, cursorState, 2);
			cmd(0x04, 0, glyph, 11);
			cmd(0x06, 21, txn6, 3);
			cmd(0x07, 22, txn7, 9);
			cmd(0x1e, 23, nullptr, 0);                                                                             // FlushChannelEvent: no payload
			for (uint32_t i = 0; i < n * 4; i++)
				chanMem[0x1000 + (record[0] + i) % chanRing] = reinterpret_cast<const uint8_t *>(words)[i];
			__sync_synchronize();
			record[0] += n * 4;
			mTestExpectTransactions = mTransactions + 2;
			mDispT0 = nowMs();
			mDispPhase = 5;
			return;
		}
		case 5: {
			const uint32_t *rec = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(mTestRoot->getBytesNoCopy()) + 0x400 + 2 * 20);
			const volatile uint32_t *stamps = reinterpret_cast<const volatile uint32_t *>(mTestFifo->getBytesNoCopy());
			if (!(rec[0] == rec[1] && stamps[3] == 23) && nowMs() < mDispT0 + 300)
				return;
			testCheck(rec[0] == rec[1], "the pipe's commands were consumed from its channel ring");
			testCheck(stamps[3] == 23, "stamp[3] == 23: the last signal of the batch (SubmitTransaction 21, SubmitTransaction2 22, FlushChannelEvent 23)");
			testCheck(mTransactions == mTestExpectTransactions && mCursorCmds == 2 && mFlushes == 1 && mOnlineAcks == 1,
			          "host counted 2 transactions, 2 cursor commands, 1 flush and the online acknowledgement");
			// the cursor fields the guest keeps in the shared state (hardware-cursor mode)
			const uint32_t pos = 10 | (20u << 16);
			memcpy(page2 + kCursorPos, &pos, 4);
			page2[kCursorVisible] = 1;
			mDispT0 = nowMs();
			mDispPhase = 6;
			return;
		}
		case 6: {
			if (nowMs() < mDispT0 + 20)
				return;
			testCheck(mPipe[5].cursorPos == (10u | (20u << 16)) && mPipe[5].cursorVisible == 1, "host noticed the cursor position and visibility in the shared state");
			// "the page is no longer ours" (hub-task-456): VBL on again for pipe 5, wait for the host's first write, then the guest "frees" the pages: pipe 5's
			// port field is overwritten, pipe 2's enabled mask becomes one the driver never stores. Neither pipe may be written again.
			__atomic_store_n(enabled(page2), kEventVbl, __ATOMIC_SEQ_CST);
			mDispT0 = nowMs();
			mDispPhase = 7;
			return;
		}
		case 7: {
			if (!(__atomic_load_n(pending(page2), __ATOMIC_SEQ_CST) & kEventVbl) && nowMs() < mDispT0 + 300)
				return;
			testCheck(take(page2) & kEventVbl, "(control) the host still writes VBL events into pipe 5's page while it is ours");
			const uint16_t scribble = 0xdead;
			memcpy(page2 + kPort, &scribble, 2);                                  // pipe 5's page reused by someone else
			__atomic_store_n(enabled(page1), 0x80, __ATOMIC_SEQ_CST);              // pipe 2's page reused: not a mask the driver stores
			mDispT0 = nowMs();
			mDispPhase = 8;
			return;
		}
		default: {
			if (nowMs() < mDispT0 + 60)   // more than three frames
				return;
			testCheck(!mPipe[5].live && !mPipe[2].live, "host let go of both pipes: port field overwritten (pipe 5), implausible enabled mask (pipe 2)");
			testCheck(!(__atomic_load_n(pending(page2), __ATOMIC_SEQ_CST) & kEventVbl) && __atomic_load_n(pending(page1), __ATOMIC_SEQ_CST) == 0,
			          "... and wrote nothing more into either page");
			testCheck((c[kRegIntrStatusDisp / 4] & 0xff) == 0 && mDispBits == 0, "... and dropped their INTR_STATUS_DISP bits");
			mStep = kStepTeardown;
			return;
		}
		}
	}
	case kStepTeardown: {
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
		c[kRegRootPage / 4] = 0;
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
				testCheck(mFifoMap == nullptr && mRootMap == nullptr && !mCh[3].live && !mPipe[2].live && !mPipe[5].live && mDispBits == 0,
				          "host dropped its FIFO, root page, channel and display pipe mappings when the guest cleared the registers");
				mTestFifo->complete();
				OSSafeReleaseNULL(mTestFifo);
			}
			if (mTestReply) {
				mTestReply->complete();
				OSSafeReleaseNULL(mTestReply);
			}
			if (mTestShared) {
				mTestShared->complete();
				OSSafeReleaseNULL(mTestShared);
			}
			if (mTestShared2) {
				mTestShared2->complete();
				OSSafeReleaseNULL(mTestShared2);
			}
			if (mTestChan) {
				mTestChan->complete();
				OSSafeReleaseNULL(mTestChan);
			}
			if (mTestRoot) {
				mTestRoot->complete();
				OSSafeReleaseNULL(mTestRoot);
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
	const uint32_t klass = pvClass();
	put32(0x08, klass << 8 | 1);      // class (default 03 display, subclass 00, prog-if 00), revision 1
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
	data("class-code", &klass, 4);   // little-endian, as in the device tree
	char compatible[48];   // "pci106b,eeee", "pciclass,<class>", "pciclass,<class without prog-if>", each NUL-terminated
	const int len = snprintf(compatible, sizeof(compatible), "pci106b,eeee%cpciclass,%06x%cpciclass,%04x", 0, klass, 0, klass >> 8);
	data("compatible", compatible, static_cast<unsigned>(len) + 1);
	setProperty("RDNA4PvGpu", "fake Apple paravirtual GPU (106b:eeee), BAR0 in RAM, M0 of docs/metal-phase-plan.md");
	setName("PVGPU");
	return true;
}

void **RDNA4PvNub::patchVtable() {
	void **orig = *reinterpret_cast<void ***>(this);
	void **copy = static_cast<void **>(IOMalloc(kVtableSlots * sizeof(void *)));
	if (!copy) {
		pvlog("vtable copy failed: the Tahoe-only IOPCIDevice slots keep their real (bridge-dependent) implementations");
		return nullptr;
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
	*reinterpret_cast<void ***>(this) = copy;   // a published nub is never released, so the copy is never orphaned; publish() frees it if attach fails
	return copy;
}

bool RDNA4PvNub::publish(IOService *parent, uint32_t level) {
	if (!parent || !level)
		return false;
	// publish() is reached from two threads (the GPU path in attach() and RDNA4PvLate's work loop): claim the single slot atomically before building
	// anything, give it back on the paths that can be retried.
	static bool claimed = false;
	bool expected = false;
	if (!__atomic_compare_exchange_n(&claimed, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return false;
	// The nub appends data members to IOPCIDevice's layout as MacKernelSDK's header knows it. If Tahoe's real class is bigger, those would overlap its
	// own members: refuse (for good: the claim stays).
	const OSSymbol *realName = OSSymbol::withCStringNoCopy("IOPCIDevice");
	const OSMetaClass *real = realName ? OSMetaClass::getMetaClassWithName(realName) : nullptr;
	OSSafeReleaseNULL(realName);
	const size_t realSize = real ? real->getClassSize() : 0;
	if (!real || realSize > sizeof(IOPCIDevice)) {
		pvlog("refused: IOPCIDevice is %zu bytes in this macOS, MacKernelSDK's header says %zu", realSize, sizeof(IOPCIDevice));
		return false;
	}
	auto *nub = OSTypeAlloc(RDNA4PvNub);
	if (!nub) {
		__atomic_store_n(&claimed, false, __ATOMIC_RELEASE);
		return false;
	}
	if (!nub->init(static_cast<OSDictionary *>(nullptr)) || !nub->build()) {
		pvlog("could not build the nub");
		nub->release();
		__atomic_store_n(&claimed, false, __ATOMIC_RELEASE);
		return false;
	}
	void **vtableCopy = nub->patchVtable();
	if (!nub->attach(parent)) {
		pvlog("attach to %s failed", parent->getName());
		nub->release();   // its free() runs through the patched vtable, whose dtor/free slots are the original ones
		if (vtableCopy)
			IOFree(vtableCopy, kVtableSlots * sizeof(void *));
		__atomic_store_n(&claimed, false, __ATOMIC_RELEASE);
		return false;
	}
	gNub = nub;   // keeps the creation reference: never released
	nub->registerService();
	pvlog("registerService returned");
	pvPause();
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
		if (nowMs() < mDeadline) {
			mTimer->setTimeoutMS(500);
			return;
		}
#ifdef AMDRadeonRDNA4_VM_TEST
		IOService *parent = gLateParent ? gLateParent : IOService::getPlatform();   // a VM without the RDNA4 device: under the platform
#else
		// On a real machine a fake GPU without the card behind it is not published: three boots on the rig (2026-10-21) froze with
		// AppleGPUWrangler panicking 1.5-2 s after the publish under the platform expert; in the two whose log could be read, the kext's
		// IONDRVSupport hook had never run (no bring-up, only the firmware's picture on the DP display).
		IOService *parent = gLateParent;
		if (!parent) {
			pvlog("not publishing after the delay: the GPU has not attached");
			return;
		}
#endif
		pvlog("publishing after the delay under %s (%s)", parent->getName(), parent == gLateParent ? "the GPU" : "the platform expert: no GPU attached");
		RDNA4PvNub::publish(parent, mLevel);
	}
	uint32_t mLevel { 0 };
	uint64_t mDeadline { 0 };
	IOWorkLoop *mLoop { nullptr };
	IOTimerEventSource *mTimer { nullptr };
};
OSDefineMetaClassAndStructors(RDNA4PvLate, OSObject)

void RDNA4PvNub::publishLater(uint32_t level, uint32_t delayMs) { RDNA4PvLate::schedule(level, delayMs); }
void RDNA4PvNub::setLateParent(IOService *gpu) { gLateParent = gpu; }   // the GPU stays for the boot: no reference taken
void RDNA4PvNub::setCompute(RDNA4Compute *compute) { gCompute = compute; }


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

// IOPCIDevice::init allocates the class's private data (`reserved`), which Tahoe's IOPCIDevice methods use without checking.
bool RDNA4PvNub::init(OSDictionary *propTable) { PVCALL("init"); return IOPCIDevice::init(propTable); }
bool RDNA4PvNub::init(IORegistryEntry *from, const IORegistryPlane *plane) { PVCALL("init"); return IOPCIDevice::init(from, plane); }
void RDNA4PvNub::free() {
	PVCALL("free");
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
bool RDNA4PvNub::attach(IOService *provider) { PVCALL("attach"); return IOService::attach(provider); }
void RDNA4PvNub::detach(IOService *provider) { PVCALL("detach"); IOService::detach(provider); }
void RDNA4PvNub::detachAbove(const IORegistryPlane *plane) { PVCALL("detachAbove"); IOService::detachAbove(plane); }
// The overrides that keep the kext linker from redirecting base-class symbols (pvgpu.hpp). Not traced: every property access comes here.
bool RDNA4PvNub::setProperty(const OSSymbol *aKey, OSObject *anObject) { return IORegistryEntry::setProperty(aKey, anObject); }
bool RDNA4PvNub::setProperty(const OSString *aKey, OSObject *anObject) { return IORegistryEntry::setProperty(aKey, anObject); }
bool RDNA4PvNub::setProperty(const char *aKey, OSObject *anObject) { return IORegistryEntry::setProperty(aKey, anObject); }
bool RDNA4PvNub::setProperty(const char *aKey, const char *aString) { return IORegistryEntry::setProperty(aKey, aString); }
bool RDNA4PvNub::setProperty(const char *aKey, bool aBoolean) { return IORegistryEntry::setProperty(aKey, aBoolean); }
bool RDNA4PvNub::setProperty(const char *aKey, unsigned long long aValue, unsigned int aNumberOfBits) {
	return IORegistryEntry::setProperty(aKey, aValue, aNumberOfBits);
}
bool RDNA4PvNub::setProperty(const char *aKey, void *bytes, unsigned int length) { return IORegistryEntry::setProperty(aKey, bytes, length); }
OSObject *RDNA4PvNub::getProperty(const OSSymbol *aKey) const { return IORegistryEntry::getProperty(aKey); }
IOReturn RDNA4PvNub::addPowerChild(IOService *theChild) { return IOService::addPowerChild(theChild); }
IOReturn RDNA4PvNub::removePowerChild(IOPowerConnection *theChild) { return IOService::removePowerChild(theChild); }
void RDNA4PvNub::detachFromChild(IORegistryEntry *child, const IORegistryPlane *plane) {
	PVCALL("detachFromChild");
	IORegistryEntry::detachFromChild(child, plane);
}
IOReturn RDNA4PvNub::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) {
	PVCALL("newUserClient");
	return IOService::newUserClient(owningTask, securityID, type, properties, handler);
}
bool RDNA4PvNub::handleOpen(IOService *forClient, IOOptionBits options, void *arg) { PVCALL("handleOpen"); return IOService::handleOpen(forClient, options, arg); }
void RDNA4PvNub::handleClose(IOService *forClient, IOOptionBits options) { PVCALL("handleClose"); IOService::handleClose(forClient, options); }
IOReturn RDNA4PvNub::requestProbe(IOOptionBits) { PVCALL("requestProbe"); return kIOReturnUnsupported; }
IOReturn RDNA4PvNub::powerStateWillChangeTo(IOPMPowerFlags, unsigned long, IOService *) { PVCALL("powerStateWillChangeTo"); return IOPMAckImplied; }
IOReturn RDNA4PvNub::setPowerState(unsigned long, IOService *) { PVCALL("setPowerState"); return IOPMAckImplied; }
unsigned long RDNA4PvNub::maxCapabilityForDomainState(IOPMPowerFlags) { PVCALL("maxCapabilityForDomainState"); return 0; }
unsigned long RDNA4PvNub::initialPowerStateForDomainState(IOPMPowerFlags) { PVCALL("initialPowerStateForDomainState"); return 0; }
unsigned long RDNA4PvNub::powerStateForDomainState(IOPMPowerFlags) { PVCALL("powerStateForDomainState"); return 0; }
bool RDNA4PvNub::compareName(OSString *name, OSString **matched) const { PVCALL("compareName"); return IOService::compareName(name, matched); }
IOService *RDNA4PvNub::matchLocation(IOService *client) { PVCALL("matchLocation"); return IOService::matchLocation(client); }
IOReturn RDNA4PvNub::getResources() { PVCALL("getResources"); return IOService::getResources(); }
IOReturn RDNA4PvNub::setProperties(OSObject *properties) { PVCALL("setProperties"); return IOService::setProperties(properties); }
IOReturn RDNA4PvNub::callPlatformFunction(const OSSymbol *f, bool w, void *p1, void *p2, void *p3, void *p4) {
	PVCALL("callPlatformFunction");
	return IOService::callPlatformFunction(f, w, p1, p2, p3, p4);
}
IOReturn RDNA4PvNub::callPlatformFunction(const char *f, bool w, void *p1, void *p2, void *p3, void *p4) {
	PVCALL("callPlatformFunction");
	return IOService::callPlatformFunction(f, w, p1, p2, p3, p4);
}
IODeviceMemory *RDNA4PvNub::getDeviceMemoryWithIndex(unsigned int) { PVCALL("getDeviceMemoryWithIndex"); return nullptr; }

// ---- matching: only a personality that names this device by IOPCIMatch / IOPCIPrimaryMatch matches; everything else is offered and refused ----

// IOService::matchPropertyTable(table, score) calls the virtual one-argument version, which this class overrides: the PCI logic therefore lives in a helper
// that calls neither (the first run of this code in the emulator overflowed the kernel stack between the two overloads, hub-task-427).
bool RDNA4PvNub::matchPci(OSDictionary *table) {
	if (!table)
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
	if (ok && klass && !matchIds(klass, pvClass() << 8))
		ok = false;
	// rdna4-pvgpu-match=0: the nub stays published but no driver matches it (to tell the nub's own effects from those of Apple's driver).
	uint32_t allow = 1;
	PE_parse_boot_argn("rdna4-pvgpu-match", &allow, sizeof(allow));
	const bool refused = ok && !allow;
	if (refused)
		ok = false;
	static uint32_t offered = 0;
	if (__atomic_fetch_add(&offered, 1u, __ATOMIC_RELAXED) < 40) {
		const char *cls = stringKey(table, "IOClass"), *bundle = stringKey(table, "CFBundleIdentifier");
		pvlog("match offered: %s %s (IOPCIMatch %s) -> %s", bundle ? bundle : "?", cls ? cls : "?", primary ? primary : "-",
		      ok ? "MATCH" : refused ? "no (refused: rdna4-pvgpu-match=0)" : "no");
	}
	return ok;
}

bool RDNA4PvNub::matchPropertyTable(OSDictionary *table) { PVCALL("matchPropertyTable"); return matchPci(table) && IOService::matchPropertyTable(table); }

bool RDNA4PvNub::matchPropertyTable(OSDictionary *table, SInt32 *score) { PVCALL("matchPropertyTable"); return matchPci(table) && IOService::matchPropertyTable(table, score); }

// ---- config space ----

UInt32 RDNA4PvNub::configRead32(IOPCIAddressSpace, UInt8 offset) {
	PVCALL("configRead32");
	uint32_t v = 0;
	memcpy(&v, &mConfig[offset & 0xfc], sizeof(v));
	return v;
}
UInt16 RDNA4PvNub::configRead16(IOPCIAddressSpace, UInt8 offset) {
	PVCALL("configRead16");
	uint16_t v = 0;
	memcpy(&v, &mConfig[offset & 0xfe], sizeof(v));
	return v;
}
UInt8 RDNA4PvNub::configRead8(IOPCIAddressSpace, UInt8 offset) { PVCALL("configRead8"); return mConfig[offset]; }
void RDNA4PvNub::configWrite32(IOPCIAddressSpace, UInt8 offset, UInt32 data) {
	PVCALL("configWrite32");
	if (offset >= 0x04)   // vendor/device are read-only
		memcpy(&mConfig[offset & 0xfc], &data, sizeof(data));
}
void RDNA4PvNub::configWrite16(IOPCIAddressSpace, UInt8 offset, UInt16 data) {
	PVCALL("configWrite16");
	if (offset >= 0x04)
		memcpy(&mConfig[offset & 0xfe], &data, sizeof(data));
}
void RDNA4PvNub::configWrite8(IOPCIAddressSpace, UInt8 offset, UInt8 data) {
	PVCALL("configWrite8");
	if (offset >= 0x04)
		mConfig[offset] = data;
}
UInt32 RDNA4PvNub::tahoeConfigRead(UInt32 width, UInt8 offset) {
	PVCALL("tahoeConfigRead");
	IOPCIAddressSpace sp {};
	return width == 32 ? configRead32(sp, offset) : width == 16 ? configRead16(sp, offset) : configRead8(sp, offset);
}
void RDNA4PvNub::tahoeConfigWrite(UInt32 width, UInt8 offset, UInt32 data) {
	PVCALL("tahoeConfigWrite");
	IOPCIAddressSpace sp {};
	if (width == 32)
		configWrite32(sp, offset, data);
	else if (width == 16)
		configWrite16(sp, offset, static_cast<UInt16>(data));
	else
		configWrite8(sp, offset, static_cast<UInt8>(data));
}
IOReturn RDNA4PvNub::tahoeDeviceMemoryRead(UInt8, UInt64 offset, void *data, UInt8 size) {
	PVCALL("tahoeDeviceMemoryRead");
	if (!mBar0 || !data || !size || offset + size > kBar0Size)
		return kIOReturnBadArgument;
	memcpy(data, static_cast<uint8_t *>(mBar0) + offset, size);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::tahoeDeviceMemoryWrite(UInt8, UInt64 offset, UInt64 data, UInt8 size) {
	PVCALL("tahoeDeviceMemoryWrite");
	if (!mBar0 || !size || size > 8 || offset + size > kBar0Size)
		return kIOReturnBadArgument;
	memcpy(static_cast<uint8_t *>(mBar0) + offset, &data, size);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::saveDeviceState(IOOptionBits) { PVCALL("saveDeviceState"); return kIOReturnSuccess; }
IOReturn RDNA4PvNub::restoreDeviceState(IOOptionBits) { PVCALL("restoreDeviceState"); return kIOReturnSuccess; }
UInt32 RDNA4PvNub::setConfigBits(UInt8 offset, UInt32 mask, UInt32 value) {
	PVCALL("setConfigBits");
	IOPCIAddressSpace sp {};
	const UInt32 old = configRead32(sp, offset);
	configWrite32(sp, offset, (old & ~mask) | (value & mask));
	return old;
}
bool RDNA4PvNub::setMemoryEnable(bool enable) {
	PVCALL("setMemoryEnable");
	IOPCIAddressSpace sp {};
	const UInt16 cmd = configRead16(sp, 0x04);
	configWrite16(sp, 0x04, enable ? (cmd | 0x2) : (cmd & ~0x2));
	return (cmd & 0x2) != 0;
}
bool RDNA4PvNub::setIOEnable(bool enable, bool) {
	PVCALL("setIOEnable");
	IOPCIAddressSpace sp {};
	const UInt16 cmd = configRead16(sp, 0x04);
	configWrite16(sp, 0x04, enable ? (cmd | 0x1) : (cmd & ~0x1));
	return (cmd & 0x1) != 0;
}
bool RDNA4PvNub::setBusMasterEnable(bool enable) {
	PVCALL("setBusMasterEnable");
	IOPCIAddressSpace sp {};
	const UInt16 cmd = configRead16(sp, 0x04);
	configWrite16(sp, 0x04, enable ? (cmd | 0x4) : (cmd & ~0x4));
	return (cmd & 0x4) != 0;
}
UInt32 RDNA4PvNub::findPCICapability(UInt8 capabilityID, UInt8 *offset) {
	PVCALL("findPCICapability");
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
	PVCALL("extendedFindPCICapability");
	if (capabilityID >= 0x80000000u || capabilityID > 0xff)
		return 0;   // no extended capabilities
	UInt8 o = offset ? static_cast<UInt8>(*offset) : 0;
	const UInt32 r = findPCICapability(static_cast<UInt8>(capabilityID), &o);
	if (offset)
		*offset = o;
	return r;
}
UInt8 RDNA4PvNub::getBusNumber() { PVCALL("getBusNumber"); return 0; }
UInt8 RDNA4PvNub::getDeviceNumber() { PVCALL("getDeviceNumber"); return 0; }
UInt8 RDNA4PvNub::getFunctionNumber() { PVCALL("getFunctionNumber"); return 0; }
IODeviceMemory *RDNA4PvNub::getDeviceMemoryWithRegister(UInt8) { PVCALL("getDeviceMemoryWithRegister"); return nullptr; }
IOMemoryMap *RDNA4PvNub::mapDeviceMemoryWithRegister(UInt8 reg, IOOptionBits) {
	PVCALL("mapDeviceMemoryWithRegister");
	if (reg != 0x10 || !mBar0Desc)
		return nullptr;
	// An alias of the RAM the host polls: a second mapping of the same pages, retained for the caller.
	return mBar0Desc->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
}
IODeviceMemory *RDNA4PvNub::ioDeviceMemory() { PVCALL("ioDeviceMemory"); return nullptr; }
void RDNA4PvNub::ioWrite32(UInt16, UInt32, IOMemoryMap *) { PVCALL("ioWrite32");}
void RDNA4PvNub::ioWrite16(UInt16, UInt16, IOMemoryMap *) { PVCALL("ioWrite16");}
void RDNA4PvNub::ioWrite8(UInt16, UInt8, IOMemoryMap *) { PVCALL("ioWrite8");}
UInt32 RDNA4PvNub::ioRead32(UInt16, IOMemoryMap *) { PVCALL("ioRead32"); return 0xffffffff; }
UInt16 RDNA4PvNub::ioRead16(UInt16, IOMemoryMap *) { PVCALL("ioRead16"); return 0xffff; }
UInt8 RDNA4PvNub::ioRead8(UInt16, IOMemoryMap *) { PVCALL("ioRead8"); return 0xff; }
bool RDNA4PvNub::hasPCIPowerManagement(IOOptionBits) { PVCALL("hasPCIPowerManagement"); return false; }
IOReturn RDNA4PvNub::enablePCIPowerManagement(IOOptionBits) { PVCALL("enablePCIPowerManagement"); return kIOReturnUnsupported; }

// ---- interrupts: the nub is its own interrupt controller; the host side calls the handler ----

IOReturn RDNA4PvNub::registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refCon) {
	PVCALL("registerInterrupt");
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
	PVCALL("unregisterInterrupt");
	if (source < 0 || source >= kMaxInterrupts)
		return kIOReturnBadArgument;
	IOLockLock(mLock);
	mInt[source] = Interrupt();
	IOLockUnlock(mLock);
	pvlog("interrupt %d unregistered", source);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::getInterruptType(int source, int *type) {
	PVCALL("getInterruptType");
	if (source < 0 || source >= kMaxInterrupts || !type)
		return kIOReturnBadArgument;
	*type = kIOInterruptTypeEdge;   // MSI
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::enableInterrupt(int source) {
	PVCALL("enableInterrupt");
	if (source < 0 || source >= kMaxInterrupts)
		return kIOReturnBadArgument;
	IOLockLock(mLock);
	mInt[source].enabled = true;
	IOLockUnlock(mLock);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::disableInterrupt(int source) {
	PVCALL("disableInterrupt");
	if (source < 0 || source >= kMaxInterrupts)
		return kIOReturnBadArgument;
	IOLockLock(mLock);
	mInt[source].enabled = false;
	IOLockUnlock(mLock);
	return kIOReturnSuccess;
}
IOReturn RDNA4PvNub::causeInterrupt(int source) { PVCALL("causeInterrupt"); return raiseInterrupt(source) ? kIOReturnSuccess : kIOReturnNotReady; }

// ======================================================================================================================================
// M3: the translator helper's user client (include/rdna4mtlc.h): one helper at a time, root only.
// ======================================================================================================================================

class RDNA4MtlcClient : public IOUserClient {
	OSDeclareDefaultStructors(RDNA4MtlcClient)
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose() APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) APPLE_KEXT_OVERRIDE;

private:
	bool mHelper { false };   // this client is the helper (another was not connected)
	IOReturn next(IOExternalMethodArguments *a);
	IOReturn deliver(IOExternalMethodArguments *a);
};

OSDefineMetaClassAndStructors(RDNA4MtlcClient, IOUserClient)

bool RDNA4MtlcClient::start(IOService *provider) {
	if (!IOUserClient::start(provider))
		return false;
	IOLockLock(gTr.lock);
	mHelper = !gTr.helper;
	gTr.helper = true;
	IOLockUnlock(gTr.lock);
	pvlog("host: translator helper %s", mHelper ? "connected" : "refused: one is connected already");
	return mHelper;
}

// Also what IOUserClient::clientDied comes to. A kMtlcNext still waiting is aborted with the process's thread.
IOReturn RDNA4MtlcClient::clientClose() {
	if (mHelper) {
		IOLockLock(gTr.lock);
		gTr.helper = false;
		IOLockUnlock(gTr.lock);
		mHelper = false;
		pvlog("host: translator helper gone");
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn RDNA4MtlcClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a, IOExternalMethodDispatch *, OSObject *, void *) {
	if (!mHelper)
		return kIOReturnNotOpen;
	if (selector == kMtlcNext && a->scalarInputCount == 2 && a->scalarOutputCount == 2 && a->structureOutput &&
	    a->structureOutputSize == sizeof(gTr.name))
		return next(a);
	if (selector == kMtlcDeliver && a->scalarInputCount == 5 && a->scalarInput[2] <= RDNA4_MTLC_MAX_ELF)
		return deliver(a);
	return kIOReturnBadArgument;
}

IOReturn RDNA4MtlcClient::next(IOExternalMethodArguments *a) {
	IOLockLock(gTr.lock);
	while (!gTr.id || gTr.taken)
		if (IOLockSleep(gTr.lock, &gTr.id, THREAD_ABORTSAFE) != THREAD_AWAKENED) {
			IOLockUnlock(gTr.lock);
			return kIOReturnAborted;
		}
	IOReturn r = kIOReturnNoSpace;
	if (gTr.libLen <= a->scalarInput[1]) {
		IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(a->scalarInput[0], gTr.libLen, kIODirectionIn, current_task());
		r = md ? md->prepare() : kIOReturnNoMemory;
		if (r == kIOReturnSuccess) {
			r = md->writeBytes(0, gTr.lib, gTr.libLen) == gTr.libLen ? kIOReturnSuccess : kIOReturnVMError;
			md->complete();
		}
		OSSafeReleaseNULL(md);
	}
	if (r == kIOReturnSuccess) {
		a->scalarOutput[0] = gTr.id;
		a->scalarOutput[1] = gTr.libLen;
		memcpy(a->structureOutput, gTr.name, sizeof(gTr.name));
		gTr.taken = true;
	}
	IOLockUnlock(gTr.lock);
	return r;
}

IOReturn RDNA4MtlcClient::deliver(IOExternalMethodArguments *a) {
	const uint64_t *in = a->scalarInput;
	const uint32_t len = static_cast<uint32_t>(in[2]);
	auto *elf = len ? static_cast<uint8_t *>(IOMalloc(len)) : nullptr;
	if (len) {
		IOMemoryDescriptor *md = elf ? IOMemoryDescriptor::withAddressRange(in[1], len, kIODirectionOut, current_task()) : nullptr;
		IOReturn r = md ? md->prepare() : kIOReturnNoMemory;
		if (r == kIOReturnSuccess) {
			r = md->readBytes(0, elf, len) == len ? kIOReturnSuccess : kIOReturnVMError;
			md->complete();
		}
		OSSafeReleaseNULL(md);
		if (r != kIOReturnSuccess) {
			if (elf)
				IOFree(elf, len);
			return r;
		}
	}
	IOLockLock(gTr.lock);
	const bool current = in[0] && in[0] == gTr.id && gTr.taken && !gTr.done;
	if (current) {
		gTr.elf = elf;
		gTr.elfLen = len;
		gTr.buffers = static_cast<uint32_t>(in[3]);
		gTr.written = static_cast<uint32_t>(in[4]);
		gTr.done = true;
		IOLockWakeup(gTr.lock, &gTr.done, false);
	}
	IOLockUnlock(gTr.lock);
	if (!current && elf)
		IOFree(elf, len);
	return current ? kIOReturnSuccess : kIOReturnNotFound;   // NotFound: the host stopped waiting for it
}

IOReturn pvTranslatorClient(IOService *service, task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) {
	if (IOUserClient::clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	if (!gTr.lock)
		return kIOReturnNotReady;   // the host has not started yet (rdna4-pvgpu-delay)
	auto *client = OSTypeAlloc(RDNA4MtlcClient);
	if (!client)
		return kIOReturnNoMemory;
	if (!client->initWithTask(owningTask, securityID, type, properties) || !client->attach(service)) {
		client->release();
		return kIOReturnNoResources;
	}
	if (!client->start(service)) {
		client->detach(service);
		client->release();
		return kIOReturnBusy;
	}
	*handler = client;
	return kIOReturnSuccess;
}
