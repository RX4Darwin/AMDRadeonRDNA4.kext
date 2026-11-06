//
//  pvgpu.cpp
//  AMDRadeonRDNA4
//
//  See pvgpu.hpp and docs/m0-pvgpu.md.
//

#include "pvgpu.hpp"
#include "trial.hpp"
#include "klog.hpp"
#include "pvstream.hpp"
#include "pvdraw.hpp"
#include "pm4.hpp"
#include "pvscanout.hpp"
#include "sku.hpp"
#include "compute.hpp"
#include "isa.hpp"
#include "mtlkernels.h"
#include "nggfill_kernel.h"
#include "psfill_kernel.h"
#include "scale_codeobj.h"
#include "../userspace/rdna4-run/gfx12tri.h"     // M4: the triangle streams the host's draws are built from (plain, one attribute)
#include "../userspace/rdna4-run/gfx12tricol.h"
static_assert(Gfx12Draw::kRingBytes == Gfx12DrawCol::kColRingBytes, "one ring block serves both streams");
#include "../userspace/rdna4-run/gfx12trirun.h"
#include "../userspace/rdna4-run/pm4build.h"   // the COMPUTE_* registers of a dispatch
#include "rdna4compute.h"
#include "rdna4mtlc.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <sys/sysctl.h>
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

// debug.rdna4_pvtalk=1: every draw is logged as the first ones of a task are (what WindowServer draws when a window appears, say).
static int gTalk;
SYSCTL_INT(_debug, OID_AUTO, rdna4_pvtalk, CTLFLAG_RW | CTLFLAG_LOCKED, &gTalk, 0, "AMDRadeonRDNA4: log every paravirtual draw");
static int gPresentOnCard = 1;
SYSCTL_INT(_debug, OID_AUTO, rdna4_pvpresent, CTLFLAG_RW | CTLFLAG_LOCKED, &gPresentOnCard, 0,
	   "AMDRadeonRDNA4: a presented frame copied on the card and flipped to at vblank (1), copied into the display's scanout on the card "
	   "(2: it tears), or with the CPU (0)");
static int gProfile;
SYSCTL_INT(_debug, OID_AUTO, rdna4_pvprofile, CTLFLAG_RW | CTLFLAG_LOCKED, &gProfile, 0,
	   "AMDRadeonRDNA4: time each submit on the card and name the fragment functions (and copies) that took it longest in the draw stats");
static int gInPlace = 3;
SYSCTL_INT(_debug, OID_AUTO, rdna4_pvinplace, CTLFLAG_RW | CTLFLAG_LOCKED, &gInPlace, 0,
	   "AMDRadeonRDNA4: what draws use in place (1 textures, 2 colour targets; 4 not IOSurface textures, 8 not Metal's); the rest is staged "
	   "(16: without shadows, each draw's own copy; 32: the displays' frames not kept on the card)");

namespace {

constexpr uint32_t kLoggedCmds = 1500;    // FIFO commands logged one by one
constexpr uint32_t kMaxLogLines = 30000;   // M1 reads the protocol from this log (docs/m1-stream.md)
// GpuCoreCount: the compute units of the card, 64 on the RX 9070 XT and 56 on
// the RX 9070 (sku.hpp). From the product specification, not read from the
// card: the kext's IP discovery has shader-engine and render-backend counts
// only. publish() sets it from the GPU the nub hangs under; a VM without the
// card keeps the XT's.
uint32_t gGpuCoreCount { Sku::computeUnits(Sku::kNavi48, Sku::kRevRx9070Xt) };
uint32_t gLogLines { 0 };
RDNA4PvNub *gNub { nullptr };
IOService *gLateParent { nullptr };   // RDNA4PvNub::setLateParent
RDNA4Compute *gCompute { nullptr };   // RDNA4PvNub::setCompute
// The IOSurface kext's functions (RDNA4PvNub::setSurfaceFunctions, from the plugin's Lilu callback), in this order; methods take the object first.
struct {
	void *(*lookup)(void *root, uint32_t id, task_t task);   // IOSurfaceRoot::lookupSurface(unsigned int, task*)
	IOMemoryDescriptor *(*memory)(const void *surface);      // IOSurface::getMemoryDescriptor() const
	uint32_t (*bytesPerRow)(const void *), (*width)(const void *), (*height)(const void *), (*pixelFormat)(const void *), (*baseOffset)(const void *),
	    (*surfaceId)(const void *);
	uint32_t (*planeCount)(const void *);   // the planes: IOSurface::getPlaneCount() const, getPlaneOffset/BytesPerRow/Width/Height(unsigned) const
	uint64_t (*planeOffset)(const void *, uint32_t);
	uint32_t (*planeBytesPerRow)(const void *, uint32_t), (*planeWidth)(const void *, uint32_t), (*planeHeight)(const void *, uint32_t);
	uint32_t (*lockSeed)(const void *), (*unlockSeed)(const void *);   // IOSurface::getLockSeed() const, getUnlockSeed() const
} gSurfaceFns;
// The kernel's map of all physical memory (x86_64 PHYSMAP_PTOV: physmap_base + address, up to physmap_max), from the plugin's Lilu callback:
// the guest's pages read and written in place. Each access through an IOMemoryMap of its own made and dropped four or five mappings a page
// (every page-table level and the page), each drop a TLB shootdown on every CPU: thousands a frame, most of a draw's time.
uint8_t *gPhysmap { nullptr }, *gPhysmapEnd { nullptr };

// Physical memory at `pa` for `bytes` through the physmap, or nullptr without one or past its end.
static uint8_t *physPtr(uint64_t pa, uint64_t bytes)
{
	const uint64_t mapped = static_cast<uint64_t>(gPhysmapEnd - gPhysmap);

	if (!gPhysmap || pa + bytes < pa || pa + bytes > mapped)
		return nullptr;

	return gPhysmap + pa;
}

// M3: the one request slot between the host and the translator helper (include/rdna4mtlc.h). The host posts a function (id != 0) and
// waits on &done; the helper's kMtlcNext waits on &id, copies the metallib out and sets taken; kMtlcDeliver fills elf and sets done.
struct Translation {
	IOLock *lock;                 // allocated with the host (RDNA4PvHost::start)
	bool helper;                  // a helper is connected
	bool gaveUp;                  // the host waited for one in vain: it does not wait again until one connects
	uint32_t helpers;             // helpers connected so far: failures cached under an older one are asked again
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
	if (n >= kMaxLogLines && !gTalk) {   // asked for with debug.rdna4_pvtalk: logged past the budget
		if (n == kMaxLogLines)
			rdna4Log("AMDRadeonRDNA4: pvgpu: log budget (%u lines) used up\n", kMaxLogLines);
		return;
	}
	char buf[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	rdna4Log("AMDRadeonRDNA4: pvgpu: %s\n", buf);
}

// The lines that tell how the host fares (the draw statistics, hangs): past the budget too, which the protocol listings use up.
void pvstat(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void pvstat(const char *fmt, ...) {
	char buf[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	rdna4Log("AMDRadeonRDNA4: pvgpu: %s\n", buf);
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
// The last real slot (Sequoia 24H232): past the header's vtable, so the copy holds whatever follows ours there.
constexpr int kSlotPowerStateWillChangeToGated = 313;
constexpr int kVtableSlots = 340;
UInt32 tRead32(RDNA4PvNub *n, UInt8 o) { return n->tahoeConfigRead(32, o); }
UInt16 tRead16(RDNA4PvNub *n, UInt8 o) { return static_cast<UInt16>(n->tahoeConfigRead(16, o)); }
UInt8 tRead8(RDNA4PvNub *n, UInt8 o) { return static_cast<UInt8>(n->tahoeConfigRead(8, o)); }
void tWrite32(RDNA4PvNub *n, UInt8 o, UInt32 v) { n->tahoeConfigWrite(32, o, v); }
void tWrite16(RDNA4PvNub *n, UInt8 o, UInt16 v) { n->tahoeConfigWrite(16, o, v); }
void tWrite8(RDNA4PvNub *n, UInt8 o, UInt8 v) { n->tahoeConfigWrite(8, o, v); }
IOReturn tConfigureInterrupts(RDNA4PvNub *, UInt32, UInt32, UInt32, UInt32) { return kIOReturnSuccess; }
IOReturn tPowerGated(RDNA4PvNub *, unsigned long *, unsigned long *, IOService *) { return kIOPMAckImplied; }
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
	int dumpTexture(uint32_t task, uint32_t id, struct sysctl_req *req);

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
	uint32_t consume(const CmdSource &src, uint32_t read, uint32_t n);
	bool barriersMet(const CmdSource &src, const uint8_t *cmd, const pvstream::FifoCommand &c);
	void dumpPayload(const char *tag, const pvstream::FifoCommand &c, const uint8_t *cmd);
	void dumpPage(const char *what, uint32_t pfn);
	uint32_t guestPfn(uint32_t task, uint64_t va, bool write = false);
	bool guestPages(uint32_t task, uint64_t va, uint64_t count, uint64_t *pages);
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
	// Found by hash, name and, for a name not passed (empty), metallib: functions without a HASH tag share the all-zero hash, and once
	// kernels were loaded a vertex function found a kernel's entry (windows were not drawn).
	struct Jit {
		uint8_t hash[32];
		char name[64];
		uint64_t lib;
		uint32_t program, buffers, written;   // a compute kernel loaded into the runtime
		uint8_t *gfx;                         // or a vertex or fragment shader: the helper's RGFX blob (air2amdgpu.cpp translateShader)
		uint32_t gfxLen;
		uint32_t helper;                      // gTr.helpers when it was translated (or failed)
		uint64_t used;                        // mJitClock when last asked for
	};
	Jit &jitSlot();
	const Jit *translated(uint32_t task, uint64_t lib, const char *name, const uint8_t hash[32]);
	static void placeShaders(uint32_t *code, const Jit *const shader[3], uint64_t rings, uint32_t outs);
	bool bufferRange(uint32_t task, uint32_t id, uint64_t *va, uint64_t *len);
	bool textureRange(uint32_t task, uint32_t id, uint64_t *va, uint64_t *len, uint32_t *bytesPerRow = nullptr, uint32_t *width = nullptr,
	                  uint32_t *height = nullptr, uint32_t *format = nullptr, uint32_t *storage = nullptr);
	struct Surface;
	bool surfaceRange(uint32_t task, uint32_t id, Surface &out);
	void dumpTexture(uint32_t task, uint32_t id);
	bool dmaGuest(uint32_t task, uint64_t dstVa, uint64_t srcVa, uint32_t pattern, uint64_t bytes);
public:
	void runBlit(uint32_t task, uint32_t id, const uint8_t *p, uint32_t bytes);
	uint32_t runRenderPass(uint32_t task, const uint8_t *p, uint32_t bytes);
	struct Bindings { uint32_t buffer[RDNA4_MTLC_MAX_BUFFERS]; uint64_t offset[RDNA4_MTLC_MAX_BUFFERS]; uint32_t texture[16], sampler[16]; };   // by Metal index (0 = none)
	// A draw: Metal's primitive type, first vertex, vertices (indices), instances, base instance, and for an indexed draw the index buffer's
	// resource id, offset and index size (bytes; 0: not indexed).
	struct Draw { uint32_t type, start, count, instances, baseInstance, indexBuffer, indexOffset, indexBytes, blendColor[4]; uint64_t viewport[4]; bool viewportSet;
	              uint64_t scissor[4]; bool scissorSet; };
	void runDraw(uint32_t task, uint32_t target, uint32_t pipeline, const Draw &draw, const Bindings *stage);
	void runCompute(uint32_t task, uint32_t pipeline, const Bindings &bound, const uint64_t *size);
	struct StagedTexture { uint64_t handle, va; uint8_t *cpu; uint32_t rowBytes, pitch, rowLen, height; bool writable; };   // staged: a runtime buffer or the pool
	struct TextureSource;
	bool textureSource(uint32_t task, uint32_t id, TextureSource &out);
	bool readTextureRow(uint32_t task, const TextureSource &src, uint32_t y, uint8_t *dst, uint32_t bytes);
	int inkedBytes(uint32_t task, const TextureSource &src, uint32_t bpp);
	bool stageTextures(uint32_t task, const Bindings &bound, uint32_t *table, StagedTexture (&out)[16], bool talk, const char *stageName,
	                   bool writes);
	bool releaseTextures(uint32_t task, StagedTexture (&staged)[16], bool copyBack);
	bool mapGuest(uint32_t task, uint64_t va, uint64_t bytes, uint64_t &handle, uint64_t &gpu);
	bool mapMemory(IOMemoryDescriptor *memory, uint64_t offset, uint64_t bytes, uint64_t &handle, uint64_t &gpu);
private:
	struct Mapping;
public:
	bool mapPages(const uint64_t *pages, uint64_t count, uint64_t &handle, uint64_t &gpu, Mapping **entry = nullptr);
	Mapping *keyedMapping(const void *memory, uint32_t task, uint64_t first, uint64_t count);
	void keyMapping(Mapping *m, IOMemoryDescriptor *memory, uint32_t task, uint64_t first);
	void describeRenderPipeline(uint32_t task, uint32_t pipeline);
private:
	void answerDeviceInfo(const pvstream::FifoCommand &c, const uint8_t *cmd);
	void answerComputePipeline(const pvstream::FifoCommand &c, const uint8_t *cmd);
public:
	void noteDraw(uint64_t ns);
private:
	IOReturn submitGfx(uint64_t ib, uint32_t dwords, uint64_t &fence);
	// Draws in flight: submitted without a wait, each with its own state page and IB (slot n of mGfxMisc and mGfxIb, taken in turn), waited
	// for together by waitDraws before anything reads or reuses what they use (a stamp completed, a blit, new shader code, a mapping
	// dropped); only the oldest by roomForDraw when kInFlight are.
	static constexpr uint32_t kSlots = 512;   // batches of kBatchDraws draws, kInFlight of them, each draw a slot
	// At most this many in flight. With three a hang once halted the ring for good; the runtime now ends all of a client's IBs on the ring
	// together (gfxRingRelease waits for its newest fence), and with one each draw waited for the one before: most of a draw's time.
	static constexpr uint32_t kInFlight = 16;
	uint32_t mSlot { 0 }, mPending { 0 };
	// Shader pairs kept on the card, slot n at mGfxCode + n * RDNA4_TRI_CODE_BYTES: WindowServer switches between a few dozen pairs all the
	// time, and with one slot each switch waited for the draws in flight and copied the slot in again.
	static constexpr uint32_t kCodeSlots = 64;   // 640 KiB each: 40 MiB of VRAM
	struct CodeSlot { uint8_t hash[2][32]; uint64_t used; bool valid; };
	CodeSlot mCode[kCodeSlots] {};
	uint64_t mCodeClock { 0 };
	uint32_t codeSlot(const uint8_t *vs, const uint8_t *ps, bool &fresh);
	uint64_t mLastFence { 0 };
	// The fences of what is in flight, oldest first from mFlightAt (mPending of them): when kInFlight are, only the oldest is waited for,
	// and the card is not left idle while the CPU puts the next one together.
	uint64_t mFlight[kInFlight] {};
	uint32_t mFlightAt { 0 };
	// Submits left in flight so far, and the fences of the last kSubmitFences: what used a mapping is waited for alone before it goes.
	static constexpr uint32_t kSubmitFences = 64;
	uint64_t mSubmits { 0 }, mSubmitFence[kSubmitFences] {};
	void waitSubmit(uint64_t submit, uint32_t line = __builtin_LINE());
	// rdna4_pvprofile: each submit's time on the card (RELEASE_MEM of the GPU's 100 MHz clock at its start and end into its slot of
	// mTimes), added up by what it was (a fragment function's name, a copy, a clear, a present) once it is done.
	char mSubmitName[kSubmitFences][24] {};
	struct GpuTime { char name[24]; uint64_t ticks; uint32_t count; };
	GpuTime mGpuTimes[24] {};
	uint64_t mTimedTo { 0 }, mClearBytes { 0 };
	bool profiling();
	uint32_t timeStamp(uint32_t *out, bool end);
	void addTimes();
	struct Dirty { uint32_t key; uint64_t fence; };
	Dirty mDirty[32] {};   // the targets of the draws in flight, key (task << 16) | resource
	uint32_t mDirtyCount { 0 };
	void flying(uint64_t fence, const char *what = "?");
	void landed();
	void roomForDraw(uint32_t line = __builtin_LINE());
	void waitFor(uint64_t fence, uint32_t line = __builtin_LINE());
	void dirtied(uint32_t task, uint32_t id, uint64_t fence);
	void waitDraws(uint32_t line = __builtin_LINE());   // line: the caller's, counted for the draw stats
	void noteWait(uint32_t line);
	struct WaitSite { uint32_t line, count; };
	WaitSite mWaitSites[16] {};
	void readyDraws();
	// Stamps whose command buffers have draws in flight: completed when the card has written the fence of their last draw (each tick and
	// after each wait), so that the host goes on with the next command buffer meanwhile. In order, one queue for all channels.
	struct LateStamp { uint32_t index, value; uint64_t fence, at; };
	static constexpr uint32_t kLateStamps = 64;
	LateStamp mLate[kLateStamps] {};
	uint32_t mLateCount { 0 };
	uint64_t mLateSince { 0 };   // when the oldest was queued
	void stampLater(uint32_t index, uint32_t value);
	void completeLate();
	bool drawing(uint32_t task, uint32_t id) const;
	struct BlitRegion;
	bool copyTextureRegion(uint32_t task, bool toTexture, uint32_t tex, uint32_t buf, const BlitRegion &r);
	void presentTransaction(const pvstream::FifoCommand &c, const uint8_t *cmd);
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
	uint64_t mBusySince { 0 };           // when the guest last gave the host work (tick: the timer's pace)
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
	uint32_t mBarrierLines { 0 };
	uint64_t mBarrierSince[17] {};   // per source (0 the root FIFO, n channel n): when its first unmet barrier was seen, 0 none
	uint8_t mDumped[256] {};            // M1: payload dumps per FIFO id so far
	uint32_t mTaskRoot[64] {};          // M1: page-table root PFN per task id (DefineHostTask), for the dumps after a commit
	bool mRtOpen { false };              // M2: this host is a client of the card's runtime
	// mapPages' mappings kept from draw to draw, by their page list: WindowServer draws with the same textures, buffers and targets every
	// frame, and each mapping costs SDMA jobs (page tables) of about 2 ms.
	// Found without its page list by a key as well: an IOSurface's memory (held while cached, so its pages stay) or a task's VA, valid
	// while the task's page table is unchanged (mPtGen). Building the page list for each lookup walked the guest's tables every draw.
	struct Mapping {
		uint64_t *pages { nullptr };
		uint64_t count { 0 }, handle { 0 }, gpu { 0 }, used { 0 };
		uint64_t submit { 0 };   // the submit that uses it last (mSubmits then + 1)
		IOMemoryDescriptor *memory { nullptr };
		uint32_t task { 0 };
		uint64_t first { 0 }, gen { 0 };
		bool keyed { false };
	};
	// WindowServer uses well over 64 textures, targets and buffers a frame: the small ones pushed the large ones out, and each came back
	// as thousands of pages mapped anew.
	static constexpr uint32_t kMappings = 256;
	uint64_t mPtGen[64] {};             // per task: bumped when the guest changes its page table
	Mapping mMap[kMappings];
	uint64_t mMapClock { 0 };
	// mMapClock when the draw or dispatch being put together began: the mappings it uses (used after it) are not dropped for room, as
	// their addresses are in its tables already (dropped, the card read unmapped memory and hung, the mouse with it).
	uint64_t mMapFloor { 0 };
	void dropMapping(Mapping &m);
	uint32_t mProgram[kMtlKernelCount] {};   // M3: kMtlKernels loaded into it, 0 = not yet
	uint32_t mFnLines { 0 };              // M3: pipeline -> function lines so far
	uint32_t mRenderPipeSeen[16] {};
	struct GfxBuffers { uint64_t handle, gpu; uint8_t *cpu; } mGfxCode {}, mGfxRings {}, mGfxMisc {}, mGfxIb {}, mGfxSpare {};   // M4: kept for the boot once made      // M4: (task << 16) | pipeline of the render pipelines described so far
	bool makeSpare(GfxBuffers &g);
	// A frame copied into a display's scanout by the card (CP DMA in the graphics ring, after the draws that made it), left in flight: the
	// batch's stamp completes once it is done. With the CPU each frame took 2.4 ms (2560x1440) or 5.3 ms (4K) of the host's thread, and
	// two displays at 60 Hz could not be kept up with (a window dragged from one to the other).
	// A port's scanout at a VA of ours (fence: of its last frame's IB). When its frames are flipped at vblank (`flips`: they were copied
	// into the scanout the display reads, torn where the copy overtook the scan), three buffers: the head's own (0) and two of ours; a frame
	// goes into the one the HUBP neither reads nor was asked to show, and is asked for once its copy is done (`queued`: that buffer + 1).
	struct ScanoutMap {
		uint64_t phys, bytes, handle, gpu, fence;
		uint64_t back[2], mc[3], va[3], flipFence;
		uint8_t hubp, queued, mode;   // mode: rdna4_pvpresent when it was set up
		uint32_t triedW, triedH;      // the frame size a mode switch was last tried for
		bool flips;
	};
	static constexpr uint32_t kScanoutPorts = 8;
	static constexpr uint32_t kPresentIbBytes = 64 * 1024;
	ScanoutMap mScanout[kScanoutPorts] {};
	GfxBuffers mPresentIb {};
	bool presentOnCard(uint32_t port, uint32_t sid);
	// A frame of macOS's scaled modes, larger than any mode of the head ("looks like 2560x1440" draws 5120x2880 on a 3840x2160
	// monitor): scale.cl puts it into the scanout, dispatched in the frame's IB. Its kernargs a slot per port.
	struct ScaleKernel { uint64_t code, argsHandle, argsGpu; uint8_t *argsCpu; uint32_t rsrc[3]; bool wave32, tried; };
	static constexpr uint32_t kScaleArgs = 256;
	ScaleKernel mScale {};
	bool scaleKernel();
	uint32_t scaleFrame(uint32_t *ib, uint32_t port, uint64_t src, uint32_t srcPitch, uint32_t srcW, uint32_t srcH, uint64_t dst,
	                    const PvScanout::Head &h, bool tenBit);
	// debug.rdna4_pvframe: a frame copied into host memory with its present (port + 1 asked for), done once its fence is
	struct FrameGrab { uint64_t handle, gpu, bytes, fence; uint8_t *cpu; uint32_t width, height, pitch; };
	FrameGrab mGrab {};
	volatile uint32_t mGrabWant { 0 };
	volatile bool mGrabReady { false };
	bool mGrabFlying { false };
public:
	int grabFrame(uint32_t port, struct sysctl_req *req);
private:
	int grabFrameOnce(uint32_t port, struct sysctl_req *req);
	void setUpFlips(ScanoutMap &sm, uint32_t port);
	uint32_t flipTarget(ScanoutMap &sm);
	void flipDone();
	bool grabBuffer(uint64_t bytes);
	void scanoutHome(ScanoutMap &sm, uint64_t mc);
	void dropScanout(ScanoutMap &sm);
	bool clearOnCard(uint32_t task, uint32_t tex, uint64_t gpu, uint64_t bytes, uint32_t pattern);
	GfxBuffers mFillCode {};   // nggfill.s and psfill.s, as a shader pair's slot
	bool clearByDraw(uint32_t task, uint32_t tex, uint64_t gpu, uint32_t pitch, uint32_t width, uint32_t height, bool half, bool bgra,
	                 const uint64_t rgba[4]);
	// Copies by the card (CP DMA in the graphics ring, after the work before them), each with an IB of its own of kDmaIbBytes, taken in turn.
	static constexpr uint32_t kDmaSlots = 8, kDmaIbBytes = 64 * 1024;
	static constexpr uint32_t kDmaRows = kDmaIbBytes / 28 - 8;   // DMA_DATA packets an IB holds, the idle barrier besides
	GfxBuffers mDmaIb {};
	// Draws put together into one IB and submitted at once (rdna4_pvinplace bit 512 off): Safari's live resize drew 26000 times a second,
	// each draw its own submit. The batch goes before any other submit, a wait, a stamp, and at the end of each tick; a misc slot is taken
	// again only once the submit that used it is done (mSlotSubmit).
	static constexpr uint32_t kBatchSlots = 16, kBatchBytes = 128 * 1024, kBatchDraws = 32;
	GfxBuffers mBatchIb {};
	uint64_t mBatchFence[kBatchSlots] {};
	uint32_t mBatchSlot { 0 }, mBatchDwords { 0 }, mBatchDraws { 0 }, mBatchDirty[kBatchDraws] {};
	bool mFlushing { false };
	uint64_t mSlotSubmit[kSlots] {};
	// The submit (of flying()'s count) and the draw serial that what is being put together will go out with: one later while a batch is
	// open (it goes first). Taken by a draw that is put into the batch after all, they wait a submit longer than needed.
	uint64_t nextSubmit() const { return mSubmits + 1 + (mBatchDraws ? 1 : 0); }
	uint64_t nextSerial() const { return mDrawSerial + 1 + (mBatchDraws ? 1 : 0); }

	bool batchDraw(const uint32_t *ib, uint32_t dwords, uint32_t task, uint32_t target);
	void flushBatch();
	uint32_t takeSlot();
	GfxBuffers mTimes {};   // rdna4_pvprofile's slots
	uint64_t mDmaFence[kDmaSlots] {};
	uint32_t mDmaSlot { 0 };
	bool rowsOnCard(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t rowLen, uint32_t rows);
	// IOSurfaces kept on the card: the displays' frames (the surfaces presented) drawn, cleared, sampled and presented in VRAM, not across
	// the bus in the guest's memory (each pass of WindowServer's read and wrote a 4K frame there, and a present copied it once more). The
	// guest's memory has what they hold only once they are written back, before the CPU reads or writes it (cardToMemory).
	struct CardSurface { OSObject *surface; IOMemoryDescriptor *memory; uint64_t base, bytes, handle, gpu, used; bool loaded; };
	static constexpr uint32_t kCardSurfaces = 8;
	CardSurface mCardSurface[kCardSurfaces] {};
	uint64_t mCardClock { 0 };
	uint32_t mCardLoads { 0 }, mCardBacks { 0 };   // for the draw stats
	CardSurface *cardSurface(const void *surface);
	void keepOnCard(void *surface);
	bool onCard(const Surface &s, uint64_t bytes, uint64_t &gpu);
	void cardToMemory(const void *surface);
	void dropCardSurface(CardSurface &c);
	void dropReleasedSurfaces();
	// A dword in host memory the card writes when everything before it in the ring is done, caches flushed (idleBarrier), and waits for.
	GfxBuffers mIdle {};
	uint32_t mIdleSeq { 0 };
	uint32_t idleBarrier(uint32_t *out);
	// Staging for draws and dispatches: host memory written by the CPU in place, taken in turn (poolTake). A target or texture staged in a
	// runtime buffer of its own cost an allocation, two SDMA copies and a free a draw, each with its page-table syncs.
	static constexpr uint64_t kPoolBytes = 96ull << 20;
	GfxBuffers mGfxPool {};
	uint64_t mPoolUsed { 0 };
	// The pool in kPoolChunks pieces taken in turn: a piece is used again once the submit that used it last is done (it was used from the
	// start again only when nothing was in flight, and with draws in batches that was seldom: the host drained the card each 24 MiB).
	static constexpr uint32_t kPoolChunks = 8;
	uint64_t mPoolChunkSubmit[kPoolChunks] {};
	uint32_t mUsedChunks { 0 };   // the pieces the draw being put together takes from (a bit each)
	bool poolTake(uint64_t bytes, uint64_t &gpu, uint8_t *&cpu);
	uint32_t mGfxHangLines { 0 };          // M4: draws whose IB did not finish, described so far
	uint32_t mBigCmdLines { 0 };           // ExecIndirect commands too large to decode, reported so far
	static constexpr uint32_t kFaulted = 16;
	uint32_t mFaulted[kFaulted] {}, mFaultedCount { 0 };   // kernels (runtime programs) whose dispatch did not finish: not run again
	void logHang(const char (*name)[64], const Draw &draw, const Bindings *bound, uint32_t count, uint64_t indexGpu, const uint64_t table[2],
	             const uint32_t *seq, uint32_t m);
	// WindowServer alone uses about 60 functions: with 32 entries a translation pushed out one still in use, and a draw then waited
	// for the helper again (seconds for the largest shaders).
	static constexpr uint32_t kJits = 256;
	Jit mJit[kJits] {};                  // M3: functions the helper translated, by hash (program 0: it could not); least recently used replaced
	uint32_t mJits { 0 };
	uint64_t mJitClock { 0 };
	uint32_t mRenderLines { 0 };          // M2: render-pass and texture lines so far (other processes render every frame)
	uint8_t mDrawLines[64] {};            // M4: draw lines per task id, so far (40 each; a new task starts again)
	uint8_t mListed[64] {};               // M4: command buffers listed command by command per task id (the first 2)
	// One FIFO command copied out of the ring for decoding. ExecIndirect lists every resource its command buffer uses: with 1 KiB, a command
	// buffer of more than about 32 textures (a window of as many images, System Settings' panes) did not fit, was skipped, and its stamp
	// still came: WindowServer took the window as drawn and showed it empty.
	uint8_t mWin[64 * 1024] {};

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
	uint64_t mDrawStatSince { 0 }, mDrawStatNs { 0 }, mDrawStatMaxNs { 0 };   // noteDraw: the draws of the last 5 s
	uint64_t mPhaseNs[6] {};            // of those: textures in, target in and submit, the card, target out and the rest; the pipeline and
	                                     // its shaders, the target and the buffers
	uint64_t mMapNs { 0 };              // mapPages' share
	uint32_t mMapMisses { 0 };          // its mappings made anew
	uint64_t mMapPages { 0 }, mCopiedBytes { 0 }, mCopyNs { 0 };   // their pages; the copied textures' bytes and the time their rows took
	uint32_t mWaits { 0 }, mCodeIns { 0 }, mTranslations { 0 };   // and the waits for draws in flight, shader pairs copied in, helper requests
	uint32_t mPresents { 0 };            // frames presented meanwhile
	// Frame timing, per port: frames, frames later than one period after the one before (a frame missed), the time from the VBL to the
	// present (sum and most); and how long the stamps queued behind draws in flight waited (sum, most, count).
	struct PortTiming { uint64_t last, vblNs, vblMaxNs; uint32_t frames, missed, gaps[4]; };   // gaps: frames 1, 2, 3 and 4 to 8 VBLs after the one before
	PortTiming mPortTiming[8] {};
	void timePresent(PortTiming &t);
	uint64_t mVblAt { 0 }, mLateNs { 0 }, mLateMaxNs { 0 };
	uint64_t mVblLateNs { 0 }, mVblLateMaxNs { 0 };   // how late the VBL events fired (the host's tick was busy)
	uint32_t mVbls { 0 }, mVblsLate { 0 };            // VBL events, those 2 ms or more late
	uint64_t mClocksAt { 0 };   // when the GFX clocks were last counted
	// The draws by their target: a surface on the card, one in the guest's memory, a Metal texture shared, managed or private; their
	// scissor rectangles' bytes.
	uint32_t mTargetDraws[5] {};
	struct CopiedTop { uint64_t at, bytes; uint32_t task, width, height, rowBytes, times; bool surface; };
	CopiedTop mCopiedTop[8] {};
	uint64_t mTargetBytes[5] {};
	uint32_t mLateDone { 0 };
	uint32_t mInPlace { 0 }, mCopied { 0 };   // textures read in place, and copied
	// Shadows: textures the card cannot read or write in place (rows not 128 bytes apart) at a pitch it can, in host memory, kept while
	// they hold what the guest has. Translucency samples the same window contents every frame, and WindowServer draws into a surface and
	// samples it right after: copying them anew each time was gigabytes a few seconds.
	struct Shadow {
		OSObject *surface { nullptr };   // held; else a guest texture of `task`
		uint32_t task { 0 };
		uint64_t at { 0 };               // the surface's base, or the texture's VA
		uint32_t rowBytes { 0 }, width { 0 }, height { 0 }, depth { 0 }, bpp { 0 }, pitch { 0 }, lockSeed { 0 }, unlockSeed { 0 };
		uint64_t imageBytes { 0 }, slice { 0 };   // the guest's slices apart, and the shadow's (the card's: rows aligned to 256 then)
		uint64_t handle { 0 }, gpu { 0 }, bytes { 0 }, when { 0 }, used { 0 };
		uint64_t serial { 0 };           // the last draw that read it (mDrawSerial)
		uint8_t *cpu { nullptr };
		bool valid { false };
		bool readOnly { false };         // of a texture shaders only read (type 3): held longer (kShadowAgeReadOnly)
	};
	// A frame's icons and glyphs alone are hundreds of small textures: with 32 shadows they pushed each other out, and each was copied
	// again. Large ones are kept within kShadowBudget.
	static constexpr uint32_t kShadows = 256;
	static constexpr uint64_t kShadowBudget = 256ull << 20, kShadowMax = 96ull << 20;   // the client has 1 GiB of VA for all
	uint64_t mShadowBytes { 0 };
	uint32_t mShadowsFreed { 0 }, mShadowsMade { 0 };   // for the draw stats
	uint32_t mStagedLines { 0 };                         // textures copied for one draw, described in this stats period
	static constexpr uint32_t kRecent = 16;              // the draws submitted last, logged when the card does not finish them
	char mRecent[kRecent][192] {};
	uint32_t mRecentAt { 0 };
	// Draws not run in this stats period, and the first one's reason and fragment function: what is missing on screen.
	uint32_t mNotRun { 0 };
	uint32_t mShaderLines { 0 };
	uint32_t mTexLines { 0 };            // textures and samplers refused, described in this stats period
	const char *mNotRunWhy { nullptr };
	char mNotRunName[64] {};
	void noteNotRun(const char *why, const char *name)
	{
		if (!mNotRun++) {
			mNotRunWhy = why;
			strlcpy(mNotRunName, name, sizeof(mNotRunName));
		}
	}
	uint32_t mStale[3] {};                               // shadows filled again: dropped, too old, the surface locked since
	uint64_t mDrawSerial { 0 };          // draws submitted
	bool shadowIdle(const Shadow &s) const;
	// What the draw being put together uses, noted again with the submit it went out with once it has: a copy the draw itself needed (a
	// shadow filled) goes out before it, and a batch is closed by it, so the draw can go out later than nextSubmit() said.
	static constexpr uint32_t kUsedMax = 64;
	bool mBuilding { false };
	Mapping *mUsedMaps[kUsedMax] {};
	Shadow *mUsedShadows[kUsedMax] {};
	uint32_t mUsedMapCount { 0 }, mUsedShadowCount { 0 }, mBuildSlot { ~0u };
	void noteUsed(Mapping *m);
	void markUsed(uint64_t submit, uint64_t serial);
	// Held, or read by a draw not submitted yet (being put together, or batched): its address is in that draw's tables already.
	bool shadowBusy(const Shadow &s) const { return (mHeld.active && mHeld.shadow == &s) || (s.serial > mDrawSerial && s.serial != ~0ull); }
	void freeShadow(Shadow &s);
	static constexpr uint64_t kShadowAge = 250000000;   // ns
	// A read-only texture's shadow: what writes it are blits and the CPU before its first use (a desktop picture: 59 MB, read across the bus
	// each frame of Mission Control, 1.5 ms a draw, when sampled in place).
	static constexpr uint64_t kShadowAgeReadOnly = 5000000000ull;   // ns
	Shadow mShadows[kShadows];
	uint64_t mShadowClock { 0 };
	uint32_t mCopiesReused { 0 };
	struct ShadowKey {
		OSObject *surface;
		uint32_t task;
		uint64_t at;
		uint32_t rowBytes, width, height, depth, bpp;
		uint64_t imageBytes;
	};
	Shadow *findShadow(const ShadowKey &k);
	bool shadowCurrent(const Shadow &s) const;
	Shadow *makeShadow(const ShadowKey &k);
	bool fillShadow(Shadow &s, IOMemoryDescriptor *memory, bool written);
	bool fillShadowOnCard(Shadow &s, IOMemoryDescriptor *memory);
	uint32_t mCardFills { 0 }, mCardFlushes { 0 };   // shadows on the card filled and written back by the card, for the draw stats
	bool shadowTexture(uint32_t task, uint32_t id, const TextureSource &src, uint32_t bpp, uint64_t &gpu, uint32_t &pitch);
	void dropShadows(const OSObject *surface, uint32_t task, uint64_t at, bool all = false);
	void clearShadows(const OSObject *surface, uint32_t task, uint64_t at, uint32_t tex, uint32_t pattern);
	void dropShadowRange(uint32_t task, uint64_t va, uint64_t bytes);
	// A colour target the card cannot write in place, drawn into its shadow from the first draw into it until another target, the end of
	// the command buffer, a blit, a dispatch or a clear (flushHeld: its drawn rows written back), its draws left in flight; instead of each
	// draw copying its rows in and out and waiting for the card. The colour block swaps BGRA8 as for a target in place.
	struct HeldTarget {
		bool active { false };
		uint32_t task { 0 }, target { 0 };
		Shadow *shadow { nullptr };
		IOMemoryDescriptor *memory { nullptr };   // a surface's, held and prepared while active
		uint32_t top { 0 }, bottom { 0 };         // the rows drawn
	};
	HeldTarget mHeld;
	bool clearHeld(uint32_t task, uint32_t tex, uint64_t va, uint32_t width, uint32_t height, uint32_t bpr, bool half, bool bgra, const uint64_t rgba[4]);
	bool holdTarget(uint32_t task, uint32_t target, uint64_t va, const Surface &surface, uint32_t width, uint32_t height, uint32_t bpr,
	                uint32_t tp);
public:
	void flushHeld();
private:
	// A render pipeline as its draws need it: its functions with their names, its blend state and vertex descriptor. Kept by the pipeline's
	// and its functions' resource records (an id used again comes with a record of its own): each draw read the descriptor and both
	// metallibs' headers from the guest and searched them, a third of a draw's time.
	struct PipelineInfo {
		uint32_t task { 0 }, pipeline { 0 }, rec[3] {}, fnRec[3][3] {};
		uint64_t pd[8] {};
		uint32_t fn[3] {}, blend[10] {}, attributes[RDNA4_MTLC_MAX_ATTRIBUTES][4] {}, skipped[RDNA4_MTLC_MAX_ATTRIBUTES] {};
		bool valid { false }, attributesKnown { false }, named[3] {};
		char name[3][64] {};
		uint8_t hash[3][32] {};
		uint64_t lib[3] {}, used { 0 };
	};
	static constexpr uint32_t kPipelines = 128;
	PipelineInfo mPipelines[kPipelines];
	uint64_t mPipelineClock { 0 };
	const PipelineInfo &pipelineInfo(uint32_t task, uint32_t pipeline);
	uint32_t mDrawStatCount { 0 };
	uint32_t mDisp { kDispVbl };
	pvstream::disp::Info mDispInfo;
	uint32_t mDispBits { 0 };           // INTR_STATUS_DISP bits asserted: level-triggered, dropped once the guest took the pending event
	uint64_t mNextVblNs { 0 };
	uint64_t mLastDispIrqNs { 0 };
	uint32_t mVblFrames { 0 };          // VBL events set (frames due while some pipe had VBL enabled)
	uint32_t mOnlineEvents { 0 }, mOnlineAcks { 0 };
	uint32_t mTransactions { 0 }, mCursorCmds { 0 }, mFlushes { 0 };
	uint32_t mDispCmdLines { 0 };
	uint32_t mRareCmdLines { 0 };
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

// debug.rdna4_pvtex: the texture debug.rdna4_pvtexid names ((task << 16) | resource id) as the guest has it, level 0, slice 0:
// {u32 width, u32 height, u32 Metal pixel format, u32 bytes a pixel, u32 heap record[3], u32 descriptor[29]}, then its rows. To look at
// what a draw sampled or drew into.
static int gTexId;

SYSCTL_INT(_debug, OID_AUTO, rdna4_pvtexid, CTLFLAG_RW | CTLFLAG_LOCKED, &gTexId, 0,
	   "AMDRadeonRDNA4: the texture debug.rdna4_pvtex reads, (task << 16) | resource");

static int texSysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (!gHost)
		return ENOENT;

	return gHost->dumpTexture(static_cast<uint32_t>(gTexId) >> 16, gTexId & 0xffff, req);
}

SYSCTL_PROC(_debug, OID_AUTO, rdna4_pvtex, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, texSysctl, "S",
	    "AMDRadeonRDNA4: a guest texture's pixels");

// debug.rdna4_pvframe: the next frame presented on port debug.rdna4_pvframeport as WindowServer composed it, at half size: {u32 width,
// u32 height}, then BGRA rows. The scanout (rdna4_pvshot) changes while it is read; this is copied by the card with the frame.
static int gFramePort;

SYSCTL_INT(_debug, OID_AUTO, rdna4_pvframeport, CTLFLAG_RW | CTLFLAG_LOCKED, &gFramePort, 0,
	   "AMDRadeonRDNA4: the port debug.rdna4_pvframe reads");

static int frameSysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	return gHost ? gHost->grabFrame(static_cast<uint32_t>(gFramePort), req) : ENOENT;
}

SYSCTL_PROC(_debug, OID_AUTO, rdna4_pvframe, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, frameSysctl, "S",
	    "AMDRadeonRDNA4: the next presented frame at half size");


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
	if (Trial::option("rdna4-pvgpu-disp", disp))
		h->mDisp = disp & 3;
	if (level >= 2)
		h->mDisp = 3;   // the self-test plays the guest's side of the display events
	if (PvScanout::enabled())
		h->mDisp = kDispOnline | kDispVbl;   // the heads are the displays: they are there, and WindowServer paces on their VBL
	uint32_t w = 0, ht = 0;
	(void)Trial::option("rdna4-pvgpu-width", w);
	(void)Trial::option("rdna4-pvgpu-height", ht);
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
	// The real register clears on read; RAM cannot, so the bits we raised are dropped once the driver is done with them: its event source keeps
	// the line disabled from the interrupt until its work loop has read the status. Dropped a tick after the raise, a raise that came while the
	// line was disabled reached the driver with no bits left, and it only saw its stamps and VBLs after IOAccelEventMachine2::waitForStamp's 1 s
	// timed wait (most raises of a session: animations stood still, frames came late).
	if (mIrqBits && !mNub->interruptBusy(0)) {
		if (volatile uint32_t *c = mNub->ctrl())
			c[kRegIntrStatusGpu / 4] &= ~mIrqBits;
		mIrqBits = 0;
	}
	const uint32_t cmds = mCmds;

	completeLate();
	pollRegisters();
	mapRoot();
	pollFifo();
	pollChannels();
	flushBatch();   // what the commands drew, on the card while the host waits for more
	flipDone();
	pollDisplay();
	if (mLevel >= 2)
		selfTest();
	completeLate();   // what finished meanwhile: its stamps now, not a tick later

	// While the guest is busy, every 250 us: each command waited for the next tick and each stamp for the one after, up to 10 ms a frame at
	// 5 ms (two displays did not get past 35 frames a second each). Idle again after kBusyNs.
	static constexpr uint64_t kBusyNs = 200000000;
	const uint64_t now = nowNs();

	if (mCmds != cmds || mLateCount || mPending)
		mBusySince = now;

	if (!mTimer)
		return;

	// Idle every 1 ms: the first frame of an animation started up to a 5 ms tick late, its VBL events as late (they are set by the tick).
	if (now - mBusySince < kBusyNs)
		mTimer->setTimeoutUS(250);
	else
		mTimer->setTimeoutMS(1);
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

// A command's barriers, {u32 stamp index, u32 value} each (AppleParavirtCommandAllocator::addBarrier): it may run once every named stamp reached its
// value. A present's transaction waits so for the render that drew its surface on another channel; run before that render, it showed the
// surface's older frame (windows and the cursor jumped back and forth). A barrier still unmet after kBarrierWait is logged and let go.
bool RDNA4PvHost::barriersMet(const CmdSource &src, const uint8_t *cmd, const pvstream::FifoCommand &c) {
	static constexpr uint64_t kBarrierWait = 1000000000;   // ns
	const volatile uint32_t *stamps = reinterpret_cast<const volatile uint32_t *>(mFifoMap->getVirtualAddress());
	uint64_t &since = mBarrierSince[src.channel];
	uint64_t now;

	for (uint32_t i = 0; i < c.barriers; i++) {
		uint32_t b[2];

		memcpy(b, cmd + pvstream::kFifoHeaderBytes + 8 * i, sizeof(b));
		if (b[0] >= 0x1000 / 4 || static_cast<int32_t>(stamps[b[0]] - b[1]) >= 0)
			continue;

		absolutetime_to_nanoseconds(mach_absolute_time(), &now);
		if (!since)
			since = now;

		if (now - since < kBarrierWait)
			return false;

		pvlog("host: %s: cmd 0x%02x waited %llu ms for stamp[%u] to reach %u (it is %u): run anyway", src.tag, c.id, (now - since) / 1000000,
		      b[0], b[1], stamps[b[0]]);
		break;
	}

	since = 0;

	return true;
}

// The bytes of commands handled, from `read`: all n, or up to a command whose barriers are not met yet (it is seen again on a later tick).
uint32_t RDNA4PvHost::consume(const CmdSource &src, uint32_t read, uint32_t n) {
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
			at = n;   // skipped, as before: nothing later in it can be framed
			break;
		}

		if (c.barriers && src.channel && c.payloadOffset <= sizeof(mWin)) {
			uint32_t b[2];

			copy(at, mWin, c.payloadOffset);
			memcpy(b, mWin + pvstream::kFifoHeaderBytes, sizeof(b));

			if (mBarrierLines < 16 || gTalk) {
				mBarrierLines++;
				pvlog("host: %s: cmd 0x%02x has %u barrier(s), the first {%u, %u}", src.tag, c.id, c.barriers, b[0], b[1]);
			}

			if (!barriersMet(src, mWin, c))
				break;
		}

		mCmds++;
		const bool display = c.id == 0x02 || (c.id >= 0x04 && c.id <= 0x07) || c.id == 0x1e;   // the display pipe's commands keep being shown
		if (mCmds <= kLoggedCmds || (display && (mDispCmdLines++ < 60 || gTalk))) {
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
			mDrawLines[(task >> 1) & 63] = 0;
			mListed[(task >> 1) & 63] = 0;
		}
		// The guest changed a task's page table: the mappings found by that task's VAs are found by their pages again. A release's layout is
		// not known: all tasks then.
		const bool commit = c.id == 0x39 && c.length <= sizeof(mWin) && c.length - c.payloadOffset >= 4;

		if (commit) {
			uint32_t task;

			copy(at, mWin, c.length);
			memcpy(&task, mWin + c.payloadOffset, 4);   // CommitIntoGPUPageTable {u32 task, u64 gpu address, u64 length}
			mPtGen[task & 63]++;
		}

		if (c.id == 0x22 || c.id == 0x38 || (c.id == 0x39 && !commit)) {
			for (uint64_t &gen : mPtGen)
				gen++;
		}

		if (c.id == 0x37 && c.length > sizeof(mWin) && mBigCmdLines++ < 8)
			pvstat("host: %s: ExecIndirect of %u bytes does not fit the %zu-byte window: its command buffer is NOT run", src.tag, c.length,
			       sizeof(mWin));

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

				if (gTalk)
					pvlog("host: %s: command buffer of task %u, signal %u", src.tag, task, c.signal);

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
		if (c.id == 0x3b && c.length <= sizeof(mWin) && c.length - c.payloadOffset >= 24) {
			copy(at, mWin, c.length);
			answerComputePipeline(c, mWin);
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
		if ((c.id == 0x06 || c.id == 0x07) && PvScanout::enabled() && c.length <= sizeof(mWin)) {
			copy(at, mWin, c.length);
			presentTransaction(c, mWin);
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

		// A VBL due while a long run of commands is worked through raised now, not after them (Safari's live resize: some 30 a second
		// came 2 to 5 ms late)
		if (mNextVblNs && nowNs() >= mNextVblNs)
			pollDisplay();
	}
	if (haveSignal && gTalk)
		pvlog("host: %s: the batch signals stamp[%u] = %u", src.tag, src.stampIndex, lastSignal);

	if (haveSignal)
		stampLater(src.stampIndex, lastSignal);   // the guest takes the stamp for its command buffers being done

	return at;
}

// One kernel mapping of guest physical memory, read-only unless asked (the map keeps the descriptor); nullptr when it cannot be made.
static IOMemoryMap *mapPhys(IOPhysicalAddress pa, IOByteCount len, bool writable = false) {
	IOMemoryDescriptor *d = IOMemoryDescriptor::withPhysicalAddress(pa, len, writable ? kIODirectionInOut : kIODirectionIn);
	IOMemoryMap *m = d ? d->createMappingInTask(kernel_task, 0, kIOMapAnywhere | (writable ? 0 : kIOMapReadOnly)) : nullptr;
	OSSafeReleaseNULL(d);
	return m;
}

// `bytes` of physical memory at `pa` for the CPU: through the physmap, else through a mapping of their own (`map`, the caller releases it).
static uint8_t *physBytes(uint64_t pa, uint64_t bytes, bool writable, IOMemoryMap *&map)
{
	map = nullptr;

	if (uint8_t *p = physPtr(pa, bytes))
		return p;

	map = mapPhys(pa, bytes, writable);

	return map ? reinterpret_cast<uint8_t *>(map->getVirtualAddress()) : nullptr;
}

// The u32 at index `i` of the page-table page `pfn`; 0 if it cannot be read.
static uint32_t tableEntry(uint32_t pfn, unsigned i)
{
	IOMemoryMap *map;
	const uint8_t *page;
	uint32_t v = 0;

	if (!pfn || pfn >= (1u << 28))
		return 0;

	page = physBytes(static_cast<uint64_t>(pfn) << 12, 0x1000, false, map);
	if (page)
		v = reinterpret_cast<const volatile uint32_t *>(page)[i];

	OSSafeReleaseNULL(map);

	return v;
}

// CreateComputePipeline {u32 task, u32 pipeline's heap index, u32, u32, u64 address}: the pipeline's reflection at the address, up to six
// pairs {u32 key, u32 value} (docs/m0-pvgpu.md): 1 maxTotalThreadsPerThreadgroup, 3 threadExecutionWidth (a wave of 32). Unanswered both
// were 0, and MetalPerformanceShaders divided by the width: WindowServer died in its first desktop blur after login (SIGFPE in MPSImage).
void RDNA4PvHost::answerComputePipeline(const pvstream::FifoCommand &c, const uint8_t *cmd)
{
	uint32_t reply[12] = {1, 1024, 3, 32};
	uint32_t task;
	uint64_t address;

	memcpy(&task, cmd + c.payloadOffset, 4);
	memcpy(&address, cmd + c.payloadOffset + 16, 8);
	if (!copyGuest(task, address, reinterpret_cast<uint8_t *>(reply), sizeof(reply), true))
		pvlog("host: CreateComputePipeline: task %u: no reflection written at 0x%llx", task, address);
}

// The draw's IB submitted, waiting up to kGfxWait while the ring is not ready: WindowServer's first draws come while the kernel still
// brings the ring up, and a draw left out is not drawn again until its region changes.
IOReturn RDNA4PvHost::submitGfx(uint64_t ib, uint32_t dwords, uint64_t &fence)
{
	if (!mFlushing)
		flushBatch();   // the draws put together go first: the ring runs in order

	static constexpr uint32_t kGfxWait = 10000;   // ms
	static constexpr uint32_t kGfxPoll = 20;      // ms
	static constexpr uint32_t kBusyPolls = 20000; // of 10 us: the ring full of draws in flight drains in well under 200 ms
	IOReturn r = gCompute->rtSubmitGfxIb(this, ib, dwords, 0, fence);

	for (uint32_t polls = 0; r == kIOReturnBusy && polls < kBusyPolls; polls++) {
		IODelay(10);
		r = gCompute->rtSubmitGfxIb(this, ib, dwords, 0, fence);
	}

	for (uint32_t waited = 0; r == kIOReturnNotReady && waited < kGfxWait; waited += kGfxPoll) {
		IOSleep(kGfxPoll);
		r = gCompute->rtSubmitGfxIb(this, ib, dwords, 0, fence);
	}

	return r;
}

// The draws in flight waited for: what they read and write is the guest's again, their slots and the pool free once readyDraws runs.
void RDNA4PvHost::waitDraws(uint32_t line)
{
	flushBatch();

	static constexpr uint32_t kBatchWait = 50;    // ms: a draw takes the card 6 ms at most
	uint64_t ns = 0;

	if (!mPending)
		return;

	const uint64_t from = nowNs();
	const IOReturn r = gCompute->rtWaitGfxFence(this, static_cast<uint32_t>(mLastFence), kBatchWait, ns);

	noteWait(line);
	mPhaseNs[2] += nowNs() - from;

	if (r != kIOReturnSuccess && mGfxHangLines++ < 12) {
		pvstat("host: render: %u draws in flight did not finish (fence %llu): 0x%x; the last ones submitted:", mPending,
		       static_cast<unsigned long long>(mLastFence), r);

		for (uint32_t i = 0; i < kRecent; i++)
			if (mRecent[(mRecentAt + i) % kRecent][0])
				pvstat("host: render:   %s", mRecent[(mRecentAt + i) % kRecent]);
	}

	mPending = 0;
	mDirtyCount = 0;
	addTimes();
	completeLate();
}

void RDNA4PvHost::noteWait(uint32_t line)
{
	mWaits++;

	for (WaitSite &w : mWaitSites) {
		if (w.line == line || !w.line) {
			w.line = line;
			w.count++;
			break;
		}
	}
}

// A submit without a wait: in flight from now on.
void RDNA4PvHost::flying(uint64_t fence, const char *what)
{
	mSubmitFence[++mSubmits % kSubmitFences] = fence;
	strlcpy(mSubmitName[mSubmits % kSubmitFences], what, sizeof(mSubmitName[0]));
	mFlight[(mFlightAt + mPending) % kInFlight] = fence;
	mPending++;
	mLastFence = fence;
}

// The submits in flight that are done no longer counted, in order; the targets only they drew into no longer dirty.
void RDNA4PvHost::landed()
{
	while (mPending && gCompute->rtGfxFenceDone(this, static_cast<uint32_t>(mFlight[mFlightAt]))) {
		mFlightAt = (mFlightAt + 1) % kInFlight;
		mPending--;
	}

	addTimes();

	if (!mPending) {
		mDirtyCount = 0;
		return;
	}

	const uint32_t oldest = static_cast<uint32_t>(mFlight[mFlightAt]);
	uint32_t kept = 0;

	for (uint32_t i = 0; i < mDirtyCount; i++)
		if (static_cast<int32_t>(static_cast<uint32_t>(mDirty[i].fence) - oldest) >= 0)
			mDirty[kept++] = mDirty[i];

	mDirtyCount = kept;
}

// Before a submit: below kInFlight in flight, by waiting for the oldest only.
void RDNA4PvHost::roomForDraw(uint32_t line)
{
	if (mPending < kInFlight)
		return;

	landed();
	if (mPending < kInFlight)
		return;

	waitFor(mFlight[mFlightAt], line);
}

// The submits up to the one with `fence` waited for (and those before it: in order), not the ones after it.
void RDNA4PvHost::waitFor(uint64_t fence, uint32_t line)
{
	static constexpr uint32_t kOldestWait = 50;   // ms
	uint64_t ns = 0;
	const uint64_t from = nowNs();
	const IOReturn r = gCompute->rtWaitGfxFence(this, static_cast<uint32_t>(fence), kOldestWait, ns);

	noteWait(line);
	mPhaseNs[2] += nowNs() - from;

	if (r != kIOReturnSuccess) {   // a hang: ended (and told) by waiting for all of them
		waitDraws(line);
		return;
	}

	landed();
}

// Whether submits are timed (rdna4_pvprofile), their slots made.
bool RDNA4PvHost::profiling()
{
	uint64_t cpu = 0;

	if (gProfile && !mTimes.handle && gCompute->rtAllocHost(this, kernel_task, kSubmitFences * 16, 0, mTimes.handle, mTimes.gpu, cpu) == kIOReturnSuccess)
		mTimes.cpu = reinterpret_cast<uint8_t *>(cpu);

	return gProfile && mTimes.cpu;
}

// A RELEASE_MEM of the GPU's clock into the next submit's slot, at its start or end: written when the work before it is done. 8 dwords.
uint32_t RDNA4PvHost::timeStamp(uint32_t *out, bool end)
{
	const uint64_t at = mTimes.gpu + ((mSubmits + 1) % kSubmitFences) * 16 + (end ? 8 : 0);
	const uint32_t n = Pm4::releaseMem(out, at, 0);

	out[1] = 0x28 | Pm4::kReleaseEventIndex5;   // BOTTOM_OF_PIPE_TS, no cache action
	out[2] = 3u << 29;                          // DATA_SEL 3: the 64-bit GPU clock

	return n;
}

// The times of the submits done since added up by name.
void RDNA4PvHost::addTimes()
{
	const uint64_t done = mSubmits - mPending;

	for (uint64_t s = mTimedTo + 1; mTimes.cpu && s <= done; s++) {
		auto *t = reinterpret_cast<volatile uint64_t *>(mTimes.cpu + (s % kSubmitFences) * 16);
		const char *name = mSubmitName[s % kSubmitFences];
		GpuTime *slot = nullptr;

		if (done - s >= kSubmitFences || !t[0] || t[1] < t[0])
			continue;

		for (GpuTime &g : mGpuTimes) {   // its entry, else a free one
			if (!strcmp(g.name, name)) {
				slot = &g;
				break;
			}
			slot = !slot && !g.name[0] ? &g : slot;
		}

		if (slot) {
			if (!slot->name[0])
				strlcpy(slot->name, name, sizeof(slot->name));
			slot->ticks += t[1] - t[0];
			slot->count++;
		}

		t[0] = t[1] = 0;
	}

	mTimedTo = done;
}

// Submit number `submit` (of flying()'s count) done: waited for when it may be in flight, alone (and those before it); everything when it
// is the one being put together.
void RDNA4PvHost::waitSubmit(uint64_t submit, uint32_t line)
{
	if (submit > mSubmits) {
		waitDraws(line);
		return;
	}

	// never more than kInFlight in flight, in order: one that many submits back is done
	if (!mPending || mSubmits - submit >= kInFlight)
		return;

	if (!gCompute->rtGfxFenceDone(this, static_cast<uint32_t>(mSubmitFence[submit % kSubmitFences])))
		waitFor(mSubmitFence[submit % kSubmitFences], line);
}

// Target `id` drawn into by the submit with `fence`: dirty until it is done.
void RDNA4PvHost::dirtied(uint32_t task, uint32_t id, uint64_t fence)
{
	const uint32_t key = task << 16 | (id & 0xffff);

	for (uint32_t i = 0; i < mDirtyCount; i++) {   // dirty already: until this later submit is done
		if (mDirty[i].key == key) {
			mDirty[i].fence = fence;
			return;
		}
	}

	if (mDirtyCount == 32)
		landed();

	if (mDirtyCount == 32) {
		waitDraws();
		return;
	}

	mDirty[mDirtyCount++] = { key, fence };
}

void RDNA4PvHost::stampLater(uint32_t index, uint32_t value)
{
	flushBatch();   // the stamp comes after its command buffer's draws

	if (!mPending && !mLateCount) {
		completeStamp(index, value);
		return;
	}

	if (mLateCount == kLateStamps)
		waitDraws();

	if (!mLateCount)
		mLateSince = nowNs();

	mLate[mLateCount++] = { index, value, mPending ? mLastFence : 0, nowNs() };   // 0: nothing of it in flight
	completeLate();
}

// The queued stamps whose draws are done, in order; all of them after waitDraws (nothing in flight then).
void RDNA4PvHost::completeLate()
{
	static constexpr uint64_t kLateWait = 50000000;   // ns: then waited for, and a hang ended
	uint32_t done = 0;

	landed();

	while (done < mLateCount) {
		const LateStamp &l = mLate[done];

		if (mPending && l.fence && !gCompute->rtGfxFenceDone(this, static_cast<uint32_t>(l.fence)))
			break;

		const uint64_t waited = nowNs() - l.at;

		mLateNs += waited;
		mLateMaxNs = waited > mLateMaxNs ? waited : mLateMaxNs;
		mLateDone++;

		completeStamp(l.index, l.value);
		done++;
	}

	for (uint32_t i = done; i < mLateCount; i++)
		mLate[i - done] = mLate[i];

	mLateCount -= done;

	if (done && mLateCount)
		mLateSince = nowNs();

	if (mLateCount && nowNs() - mLateSince > kLateWait)
		waitDraws();
}

// Before a draw or dispatch is put together: the slots from the start again at their end.
void RDNA4PvHost::readyDraws()
{
	if (mSlot >= kSlots)   // a ring: takeSlot waits for the submit that used a slot last
		mSlot = 0;
}

// Whether a draw in flight draws into texture `id`: the CPU must not read it before they are done.
bool RDNA4PvHost::drawing(uint32_t task, uint32_t id) const
{
	const uint32_t key = task << 16 | (id & 0xffff);

	for (uint32_t i = 0; i < mDirtyCount; i++)
		if (mDirty[i].key == key)
			return true;

	for (uint32_t i = 0; i < mBatchDraws; i++)   // put together, not submitted yet: waitDraws submits them first
		if (mBatchDirty[i] == key)
			return true;

	return false;
}

// A mapping the draw being put together uses.
void RDNA4PvHost::noteUsed(Mapping *m)
{
	if (mBuilding && mUsedMapCount < kUsedMax)
		mUsedMaps[mUsedMapCount++] = m;
}

// What the draw put together used, noted with the submit and serial it went out with (only ever later than noted while it was built).
void RDNA4PvHost::markUsed(uint64_t submit, uint64_t serial)
{
	for (uint32_t i = 0; i < mUsedMapCount; i++)
		mUsedMaps[i]->submit = mUsedMaps[i]->submit > submit ? mUsedMaps[i]->submit : submit;

	for (uint32_t i = 0; i < mUsedShadowCount; i++)
		if (mUsedShadows[i]->serial != ~0ull && mUsedShadows[i]->serial < serial)
			mUsedShadows[i]->serial = serial;

	if (mBuildSlot < kSlots && mSlotSubmit[mBuildSlot] < submit)
		mSlotSubmit[mBuildSlot] = submit;

	for (uint32_t i = 0; i < kPoolChunks; i++)
		if ((mUsedChunks & 1u << i) && mPoolChunkSubmit[i] < submit)
			mPoolChunkSubmit[i] = submit;

	mBuilding = false;
}

// A misc and IB slot for a submit: in turn, and once the submit that used it last is done.
uint32_t RDNA4PvHost::takeSlot()
{
	readyDraws();

	const uint32_t slot = mSlot++;

	waitSubmit(mSlotSubmit[slot]);
	mSlotSubmit[slot] = nextSubmit();

	return slot;
}

// A draw's, clear's or copy's stream added to the batch (false: submit it alone). Its target is dirty from now on (drawing()); a copy
// (task and target 0) has none.
bool RDNA4PvHost::batchDraw(const uint32_t *ib, uint32_t dwords, uint32_t task, uint32_t target)
{
	uint64_t cpu = 0;

	if ((gInPlace & 512) || profiling() || dwords * 4 > kBatchBytes)
		return false;

	if (!mBatchIb.handle && gCompute->rtAllocHost(this, kernel_task, kBatchBytes * kBatchSlots, RDNA4_HOST_EXECUTABLE, mBatchIb.handle, mBatchIb.gpu,
	                                              cpu) == kIOReturnSuccess)
		mBatchIb.cpu = reinterpret_cast<uint8_t *>(cpu);

	if (!mBatchIb.cpu)
		return false;

	if (mBatchDraws == kBatchDraws || (mBatchDwords + dwords) * 4 > kBatchBytes)
		flushBatch();

	const uint32_t slot = mBatchSlot % kBatchSlots;

	if (!mBatchDraws && mBatchFence[slot] && !gCompute->rtGfxFenceDone(this, static_cast<uint32_t>(mBatchFence[slot])))
		waitFor(mBatchFence[slot]);   // the slot's IB still read by the card

	memcpy(mBatchIb.cpu + static_cast<uint64_t>(slot) * kBatchBytes + mBatchDwords * 4, ib, dwords * 4);
	mBatchDwords += dwords;
	mBatchDirty[mBatchDraws++] = task << 16 | (target & 0xffff);

	return true;
}

// The draws put together submitted as one IB, left in flight; their targets dirty until it is done.
void RDNA4PvHost::flushBatch()
{
	const uint32_t slot = mBatchSlot % kBatchSlots, draws = mBatchDraws, dwords = mBatchDwords;
	uint32_t dirty[kBatchDraws];
	uint64_t fence = 0;

	if (!draws || mFlushing)
		return;

	// closed before anything below can wait (and so flush again)
	memcpy(dirty, mBatchDirty, draws * 4);
	mBatchSlot++;
	mBatchDwords = 0;
	mBatchDraws = 0;
	mFlushing = true;
	__sync_synchronize();   // seen by the card (snooped) before the submit
	roomForDraw();

	const IOReturn r = submitGfx(mBatchIb.gpu + static_cast<uint64_t>(slot) * kBatchBytes, dwords, fence);

	mFlushing = false;

	if (r != kIOReturnSuccess) {
		if (mGfxHangLines++ < 12)
			pvstat("host: render: a batch of %u draws not submitted (0x%x)", draws, r);
		return;
	}

	mDrawSerial++;
	flying(fence, "batch");
	mBatchFence[slot] = fence;

	for (uint32_t i = 0; i < draws; i++)
		if (dirty[i])   // not a copy's
			dirtied(dirty[i] >> 16, dirty[i] & 0xffff, fence);
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
// top [va >> 32], middle [(va >> 22) & 1023], leaf [(va >> 12) & 1023]. A leaf entry is the data page's number, | 0x80000000 if the GPU
// may write the page: WindowServer's read-only textures (resource type 3) have entries without it (2026-11-04); the upper two levels hold
// bare page numbers. 0 = not mapped. With `write` only writable pages.
uint32_t RDNA4PvHost::guestPfn(uint32_t task, uint64_t va, bool write) {
	const unsigned index[4] = {0, static_cast<unsigned>(va >> 32) & 1023, static_cast<unsigned>(va >> 22) & 1023, static_cast<unsigned>(va >> 12) & 1023};
	uint32_t pfn = mTaskRoot[task & 63];
	for (unsigned level = 0; level < 4 && pfn; level++)
		pfn = tableEntry(pfn, index[level]);
	return !write || (pfn & 0x80000000u) ? pfn & 0x0fffffffu : 0;
}

// The physical addresses of `count` pages of a task's GPU space from `va` (page-aligned) as guestPfn finds them for reading, each leaf table
// walked to once for all of its pages (a page each would map 4 table pages: 32768 mappings for a 4K target); false if one is not mapped.
bool RDNA4PvHost::guestPages(uint32_t task, uint64_t va, uint64_t count, uint64_t *pages) {
	for (uint64_t i = 0; i < count;) {
		const uint64_t at = va + (i << 12);
		const unsigned index[3] = {0, static_cast<unsigned>(at >> 32) & 1023, static_cast<unsigned>(at >> 22) & 1023};
		uint32_t pfn = mTaskRoot[task & 63];

		for (unsigned level = 0; level < 3 && pfn; level++)
			pfn = tableEntry(pfn, index[level]);

		if (!pfn || pfn >= (1u << 28))
			return false;

		IOMemoryMap *map;
		const uint8_t *leaf = physBytes(static_cast<uint64_t>(pfn) << 12, 0x1000, false, map);
		const volatile uint32_t *entry = reinterpret_cast<const volatile uint32_t *>(leaf);
		bool ok = leaf != nullptr;

		for (uint32_t e = static_cast<uint32_t>(at >> 12) & 1023; e < 1024 && i < count && ok; e++, i++) {
			pages[i] = static_cast<uint64_t>(entry[e] & 0x0fffffffu) << 12;
			ok = pages[i] != 0;
		}

		OSSafeReleaseNULL(map);

		if (!ok)
			return false;
	}

	return true;
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
		const uint32_t off = static_cast<uint32_t>(va & 0xfff);
		const uint32_t take = len < 0x1000 - off ? len : 0x1000 - off;
		const uint32_t pfn = guestPfn(task, va, toGuest);
		IOMemoryMap *map = nullptr;
		uint8_t *at = pfn ? physBytes((static_cast<uint64_t>(pfn) << 12) + off, take, toGuest, map) : nullptr;

		if (!at)
			return false;

		if (toGuest)
			memcpy(at, buf, take);
		else
			memcpy(buf, at, take);

		OSSafeReleaseNULL(map);
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
	bool listing;                       // every command logged (the first command buffers of a task)
	RDNA4PvHost *host;
	uint32_t task;
	uint32_t pipeline;                  // Compute.setComputePipelineState
	uint32_t target, renderPipeline;    // Render: colour attachment 0's texture (writeDescriptor), setRenderPipelineState
	RDNA4PvHost::Bindings stage[2];     // Render.setVertexBuffers, setFragmentBuffers
	uint32_t blendColor[4];             // Render.setBlendColorRed: four floats' bits
	uint64_t viewport[4];               // Render.setViewport: originX, originY, width, height as doubles' bits (znear, zfar not used)
	bool viewportSet;                   // since the encoder began (writeDescriptor); else the whole target
	uint64_t scissor[4];                // Render.setScissorRect: x, y, width, height in pixels
	bool scissorSet;                    // as viewportSet
	RDNA4PvHost::Bindings compute;      // Compute.setBuffers
};
// The bytes of the draws execSink runs, by command (0: not a draw it runs).
uint32_t drawBytes(uint32_t id) {
	switch (id) {
	case 0x000: return 20;
	case 0x001: case 0x003: return 8;
	case 0x005: case 0x007: return 12;
	case 0x009: return 16;
	case 0x006: return 24;
	default: return 0;
	}
}

void execSink(void *user, uint32_t id, const char *name, const uint8_t *payload, uint32_t bytes, size_t offset) {
	auto *t = static_cast<ExecTally *>(user);
	t->commands++;
	if (!name)
		t->unknown++;
	if (t->listing) {
		uint32_t w[4] = {};
		memcpy(w, payload, bytes < sizeof(w) ? bytes : sizeof(w));
		pvlog("host:   stream: task %u %s cmd 0x%03x %s, %u bytes: %08x %08x %08x %08x", t->task, t->segment, id, name ? name : "?", bytes, w[0], w[1], w[2], w[3]);
	}
	if (id == 0x132 || id == 0x12c || id == 0x12d || id == 0x12e)
		t->host->runBlit(t->task, id, payload, bytes);
	else if (id == 0x01a) {
		t->target = t->host->runRenderPass(t->task, payload, bytes);
		t->viewportSet = false;
		t->scissorSet = false;
	}
	else if (id == 0x074 && bytes >= 4) {   // Render.setRenderPipelineState {u32 pipeline id}
		memcpy(&t->renderPipeline, payload, 4);
		t->host->describeRenderPipeline(t->task, t->renderPipeline);
	} else if (drawBytes(id) && bytes >= drawBytes(id)) {
		// The draws (measured, docs/m1-stream.md s.5b; rdna4-mtlindexed 2026-11-05): 0x001 drawPrimitives {u32 type, u16 start, u16 count}; 0x000
		// {u32 type, u64 start, u64 count}; 0x003 {u16 start, u16 count, u16 instances, u16 type}; 0x005 {u16 start, u16 count, u16 instances,
		// u16 base instance, u32 type}; drawIndexedPrimitives {u16 type, u16 index type (1: 32-bit), u32 index buffer, ...}: 0x007 {..., u16 count,
		// u16 offset}, 0x009 {..., u16 count, u16 offset, u32 instances}, 0x006 {..., u64 count, u64 offset}.
		uint32_t w[6] = {};
		memcpy(w, payload, drawBytes(id));
		const uint32_t type = w[0] & 0xffff, indexBytes = w[0] >> 16 ? 4 : 2;
		RDNA4PvHost::Draw d = {w[0], w[1] & 0xffff, w[1] >> 16, 1, 0, 0, 0, 0, {}};
		if (id == 0x000)
			d = {w[0], w[1], w[3], 1, 0, 0, 0, 0, {}};
		else if (id == 0x003)
			d = {w[1] >> 16, w[0] & 0xffff, w[0] >> 16, w[1] & 0xffff, 0, 0, 0, 0, {}};
		else if (id == 0x005)
			d = {w[2], w[0] & 0xffff, w[0] >> 16, w[1] & 0xffff, w[1] >> 16, 0, 0, 0, {}};
		else if (id == 0x007 || id == 0x009)
			d = {type, 0, w[2] & 0xffff, id == 0x009 ? w[3] : 1, 0, w[1], w[2] >> 16, indexBytes, {}};
		else if (id == 0x006)
			d = {type, 0, w[2], 1, 0, w[1], w[4], indexBytes, {}};
		if ((id == 0x000 && (w[2] || w[4])) || (id == 0x006 && (w[3] || w[5])))
			d.type = ~0u;   // a start, count or offset beyond 32 bits: refused as an unknown primitive
		memcpy(d.blendColor, t->blendColor, sizeof(d.blendColor));
		memcpy(d.viewport, t->viewport, sizeof(d.viewport));
		d.viewportSet = t->viewportSet;
		memcpy(d.scissor, t->scissor, sizeof(d.scissor));
		d.scissorSet = t->scissorSet;
		const uint64_t start = nowNs();
		t->host->runDraw(t->task, t->target, t->renderPipeline, d, t->stage);
		t->host->noteDraw(nowNs() - start);
	} else if ((id == 0x0cb || id == 0x07d || id == 0x06e) && bytes >= 8) {
		// Compute.setBuffers, Render.setVertexBuffers, setFragmentBuffers {u32 first index, u32 count, count x {u32 resource id, u64 offset}}
		// (measured, docs/m1-stream.md s.5b; setBytes is setBuffers on a driver buffer)
		RDNA4PvHost::Bindings &g = id == 0x0cb ? t->compute : t->stage[id == 0x06e];
		uint32_t first, count;
		memcpy(&first, payload, 4);
		memcpy(&count, payload + 4, 4);
		for (uint32_t k = 0; k < count && first + k < RDNA4_MTLC_MAX_BUFFERS && 8 + 12 * (k + 1) <= bytes; k++) {
			memcpy(&g.buffer[first + k], payload + 8 + 12 * k, 4);
			memcpy(&g.offset[first + k], payload + 12 + 12 * k, 8);
		}
	} else if (id == 0x082 && bytes >= 32) {   // Render.setViewport {double originX, originY, width, height, znear, zfar}
		memcpy(t->viewport, payload, sizeof(t->viewport));
		t->viewportSet = true;
	} else if (id == 0x075 && bytes >= 32) {   // Render.setScissorRect {u64 x, y, width, height}
		memcpy(t->scissor, payload, sizeof(t->scissor));
		t->scissorSet = true;
	} else if (id == 0x065 && bytes >= 16) {   // Render.setBlendColorRed {float red, green, blue, alpha}
		memcpy(t->blendColor, payload, 16);
	} else if ((id == 0x072 || id == 0x070 || id == 0x081 || id == 0x07f || id == 0x0ce || id == 0x0cc) && bytes >= 8) {
		// Render.setFragmentTextures / setFragmentSamplerStates {u32 first index, u32 count, count x u32 resource id} (measured), and
		// setVertexTextures / setVertexSamplerStates (the same layout assumed, not measured: the rig is down, 2026-10-26), and
		// Compute.setTextures / setSamplerStates (the same layout, seen in the rig's log on 2026-10-22)
		uint32_t first, count;
		memcpy(&first, payload, 4);
		memcpy(&count, payload + 4, 4);
		const bool compute = id == 0x0ce || id == 0x0cc;
		RDNA4PvHost::Bindings &g = compute ? t->compute : t->stage[id == 0x072 || id == 0x070];
		uint32_t *to = id == 0x072 || id == 0x081 || id == 0x0ce ? g.texture : g.sampler;
		for (uint32_t k = 0; k < count && first + k < 16 && 8 + 4 * (k + 1) <= bytes; k++)
			memcpy(&to[first + k], payload + 8 + 4 * k, 4);
	} else if ((id == 0x0cf || id == 0x07e || id == 0x06f) && bytes >= 12) {   // the set...BufferOffset of each: {u32 index, u64 offset}
		RDNA4PvHost::Bindings &g = id == 0x0cf ? t->compute : t->stage[id == 0x06f];
		uint32_t index;
		memcpy(&index, payload, 4);
		if (index < RDNA4_MTLC_MAX_BUFFERS)
			memcpy(&g.offset[index], payload + 4, 8);
	} else if (id == 0x0d0 && bytes >= 4) {
		memcpy(&t->pipeline, payload, 4);
	} else if ((id == 0x0ca || id == 0x0c8) && bytes >= 48) {
		// Compute.dispatchThreads {u64 threads[3], u64 per group[3]}; dispatchThreadgroups {u64 groups[3], u64 per group[3]}: whole groups
		uint64_t size[6];
		memcpy(size, payload, sizeof(size));
		for (unsigned d = 0; d < 3 && id == 0x0c8; d++)
			size[d] *= size[d + 3];
		t->host->runCompute(t->task, t->pipeline, t->compute, size);
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
	static constexpr uint32_t kMaxRead = 1u << 20;   // 64 KiB dropped one of Maps' command buffers (72 KiB) whole
	const uint32_t n = len < kMaxRead ? static_cast<uint32_t>(len) : kMaxRead;
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
	t.listing = mListed[task & 63] < 2 && ++mListed[task & 63];
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
	flushHeld();   // the guest reads what the command buffer drew once its stamp completes
	pvlog("host: stream: task %u command buffer at 0x%llx, %llu bytes: %u segment(s), %u command(s), %u unknown, %u byte(s) undecoded%s", task, va, len,
	      segments, t.commands, t.unknown, undecoded, len > n ? " (only the first 1 MiB read)" : "");
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
bool RDNA4PvHost::textureRange(uint32_t task, uint32_t id, uint64_t *va, uint64_t *len, uint32_t *bytesPerRow, uint32_t *width, uint32_t *height,
                               uint32_t *format, uint32_t *storage) {
	uint32_t rec[3] = {}, desc[29];
	if (!id || id > 0x10000 || !readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(rec), sizeof(rec)) ||
	    ((rec[0] & 0xff) != 2 && (rec[0] & 0xff) != 3) || (rec[0] >> 8) < sizeof(desc) || !readGuest(task, rec[1], reinterpret_cast<uint8_t *>(desc), sizeof(desc))) {
		if (mRenderLines++ < 24)
			pvlog("host: task %u resource %u: no texture (heap record %08x)", task, id, rec[0]);
		return false;
	}
	if (mRenderLines++ < 24)
		pvlog("host: task %u texture %u: descriptor %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", task, id, desc[0],
	      desc[1], desc[2], desc[3], desc[4], desc[5], desc[6], desc[7], desc[8], desc[9], desc[10], desc[11], desc[12], desc[13], desc[14], desc[15]);
	*len = desc[0] | static_cast<uint64_t>(desc[1]) << 32;
	*va = static_cast<uint64_t>(desc[2]) << 12;

	// where in its allocation the texture starts (dwords 4-5; 0x100 for CoreAnimation's text images, measured 2026-11-05)
	const uint64_t start = desc[4] | static_cast<uint64_t>(desc[5]) << 32;

	if (start >= *len)
		return false;

	*va += start;
	*len -= start;

	if (bytesPerRow)
		*bytesPerRow = desc[13];
	if (width)
		*width = desc[15];
	if (height)
		*height = desc[16];
	if (storage)   // Metal's storage mode: 0 shared, 1 managed, 2 private (rdna4/probes/storage.swift, 2026-11-06)
		*storage = desc[25] >> 20 & 3;
	if (format) {   // {..., 0x2c, its own id, (Metal pixel format << 16) | usage} (0: not found)
		*format = 0;
		for (uint32_t j = 19; j + 1 < 29; j++)
			if (desc[j - 1] == 0x2c && desc[j] == id)
				*format = desc[j + 1] >> 16;
	}
	return *len && *len <= (1ull << 30) && guestPfn(task, *va);
}

// M4: an IOSurface-backed texture (resource type 5, 56 bytes: dword 0 the IOSurface id, dword 5 the Metal pixel format << 16 | ?, dwords 6
// and 7 width and height; apps' drawables, iTerm2's measured 2026-10-22). The host is the guest's own kernel, so the surface is found by id:
// IOSurfaceRoot::lookupSurface(root, id, task) returns it retained (IOSurface::tryRetain), but only for the surface's owner task (find_surface),
// which is read from the root's id table first. ponytail: that table is Tahoe 25G83's private layout (root +0xd0 table, +0xd8 count;
// surface +0x10 id, +0x170 owner task), read without IOSurfaceRoot's lock, and checked against the id twice; an OS update can move it (the
// draw is then refused).
struct RDNA4PvHost::Surface {
	OSObject *surface = nullptr;
	IOMemoryDescriptor *memory = nullptr;   // prepared while held
	uint64_t base = 0;
	uint32_t bytesPerRow = 0, width = 0, height = 0, format = 0;   // format: Metal's
	~Surface() {
		if (memory)
			memory->complete();
		if (surface)
			surface->release();
	}
};

// The IOSurface with id `sid`, retained, or nullptr (the root's table; the layout above, the same on Sequoia 24H232: its find_surface reads the
// owner at +0x170 too).
static void *retainSurface(uint32_t sid) {
	static IOService *root;   // ponytail: kept for the boot
	if (!gSurfaceFns.lookup || !sid)
		return nullptr;
	if (!root) {
		OSDictionary *match = IOService::serviceMatching("IOSurfaceRoot");
		root = match ? IOService::copyMatchingService(match) : nullptr;
		OSSafeReleaseNULL(match);
	}
	const auto *r = reinterpret_cast<const uint8_t *>(root);
	const void *entry = r && sid < *reinterpret_cast<const uint32_t *>(r + 0xd8) ? (*reinterpret_cast<void *const *const *>(r + 0xd0))[sid] : nullptr;
	if (!entry || *reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(entry) + 0x10) != sid)
		return nullptr;
	return gSurfaceFns.lookup(root, sid, *reinterpret_cast<task_t const *>(static_cast<const uint8_t *>(entry) + 0x170));
}

// The paravirtual displays' frames (pvscanout.hpp): at most height rows of at most width pixels of IOSurface `sid` into `dst`, as much as both have.
bool PvScanout::copySurface(uint32_t sid, uint8_t *dst, uint32_t pitch, uint32_t width, uint32_t height) {
	void *s = retainSurface(sid);
	IOMemoryDescriptor *md = s ? gSurfaceFns.memory(s) : nullptr;
	bool ok = false;
	if (md) {
		const uint64_t base = gSurfaceFns.baseOffset(s), bpr = gSurfaceFns.bytesPerRow(s);
		if (bpr) {
			width = width < bpr / 4 ? width : static_cast<uint32_t>(bpr / 4);
			height = height < gSurfaceFns.height(s) ? height : gSurfaceFns.height(s);
		}
		if (bpr && width && height && base + bpr * height <= md->getLength() && md->prepare() == kIOReturnSuccess) {
			ok = true;
			for (uint32_t y = 0; y < height && ok; y++)
				ok = md->readBytes(base + bpr * y, dst + static_cast<uint64_t>(pitch) * y, 4ull * width) == 4ull * width;
			md->complete();
		}
	}
	if (s)
		static_cast<OSObject *>(s)->release();
	return ok;
}

// Metal pixel format -> gfx12 image format, its DST_SEL x, y, z, w (4 X .. 7 W, 0 zero, 1 one), bytes per pixel
struct TextureFormat { uint32_t metal, gfx, sel, bpp; };

static const TextureFormat *textureFormat(uint32_t metal) {
	static const TextureFormat fmts[] = {{70, 42, 4 | 5 << 3 | 6 << 6 | 7 << 9, 4}, {71, 66, 4 | 5 << 3 | 6 << 6 | 7 << 9, 4},    // RGBA8 unorm, sRGB
	                                     {80, 42, 6 | 5 << 3 | 4 << 6 | 7 << 9, 4}, {81, 66, 6 | 5 << 3 | 4 << 6 | 7 << 9, 4},    // BGRA8 unorm, sRGB
	                                     {55, 22, 4 | 0 << 3 | 0 << 6 | 1 << 9, 4}, {115, 57, 4 | 5 << 3 | 6 << 6 | 7 << 9, 8},   // R32Float, RGBA16Float
	                                     {125, 63, 4 | 5 << 3 | 6 << 6 | 7 << 9, 16},                                             // RGBA32Float
	                                     {1, 1, 4 << 9, 1}, {10, 1, 4 | 1 << 9, 1}, {11, 64, 4 | 1 << 9, 1},         // A8, R8 unorm, sRGB
	                                     {30, 14, 4 | 5 << 3 | 1 << 9, 2}, {31, 65, 4 | 5 << 3 | 1 << 9, 2},                      // RG8 unorm, sRGB
	                                     {20, 7, 4 | 1 << 9, 2}, {25, 13, 4 | 1 << 9, 2},                                          // R16 unorm, float
	                                     {60, 23, 4 | 5 << 3 | 1 << 9, 4}, {65, 29, 4 | 5 << 3 | 1 << 9, 4},                       // RG16 unorm, float
	                                     {90, 36, 4 | 5 << 3 | 6 << 6 | 7 << 9, 4}, {94, 36, 6 | 5 << 3 | 4 << 6 | 7 << 9, 4},    // RGB10A2, BGR10A2
	                                     {92, 30, 4 | 5 << 3 | 6 << 6 | 1 << 9, 4}, {110, 51, 4 | 5 << 3 | 6 << 6 | 7 << 9, 8},   // RG11B10Float, RGBA16
	                                     // integer formats (VideoToolbox's scaler writes RGBA32Uint and reads HEIC tiles' chroma as RG8Uint):
	                                     // gfx11/12 IMG_FORMAT 8_UINT 5, 16_UINT 11, 8_8_UINT 18, 32_UINT 20, 16_16_UINT 27, 8_8_8_8_UINT 46,
	                                     // 32_32_UINT 48, 16_16_16_16_UINT 55, 32_32_32_32_UINT 61
	                                     {13, 5, 4 | 1 << 9, 1}, {23, 11, 4 | 1 << 9, 2}, {53, 20, 4 | 1 << 9, 4},                 // R8, R16, R32 uint
	                                     {33, 18, 4 | 5 << 3 | 1 << 9, 2},                                                         // RG8 uint
	                                     {63, 27, 4 | 5 << 3 | 1 << 9, 4}, {73, 46, 4 | 5 << 3 | 6 << 6 | 7 << 9, 4},              // RG16, RGBA8 uint
	                                     {103, 48, 4 | 5 << 3 | 1 << 9, 8}, {113, 55, 4 | 5 << 3 | 6 << 6 | 7 << 9, 8},            // RG32, RGBA16 uint
	                                     {123, 61, 4 | 5 << 3 | 6 << 6 | 7 << 9, 16}};                                           // RGBA32 uint
	for (const TextureFormat &e : fmts)
		if (e.metal == metal)
			return &e;

	return nullptr;
}

// A texture's IOSurface: any format of textureFormat() for sampling (CoreAnimation's layers come as RGBA16Float too), the colour targets
// check for BGRA8 themselves. Dword 13 of the descriptor is the plane: a texture of a biplanar surface's second plane (CoreAnimation's RG8
// masks beside their colour, behind vibrancy) has that plane's offset, rows and size; taken as the whole surface's it was refused, and
// what it masked never drawn (2026-11-05).
bool RDNA4PvHost::surfaceRange(uint32_t task, uint32_t id, Surface &out) {
	uint32_t rec[3], d[14];
	static uint32_t refused;

	if (!gSurfaceFns.lookup || !id || id > 0x10000 || !readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(rec), sizeof(rec)) ||
	    (rec[0] & 0xff) != 5 || (rec[0] >> 8) < sizeof(d) || !readGuest(task, rec[1], reinterpret_cast<uint8_t *>(d), sizeof(d)))
		return false;   // not a type-5 texture (or no IOSurface functions)

	const TextureFormat *fm = textureFormat(d[5] >> 16);

	if (!fm) {
		if (refused++ < 16)
			pvlog("host: render: IOSurface %u (resource %u): Metal format %u, %ux%u: not supported yet", d[0], id, d[5] >> 16, d[6], d[7]);
		return false;
	}

	const uint32_t sid = d[0];
	void *s = retainSurface(sid);
	if (!s) {
		if (refused++ < 16)
			pvlog("host: render: IOSurface %u (resource %u, %ux%u): not found", sid, id, d[6], d[7]);
		return false;
	}
	const uint32_t plane = d[13];
	const bool planar = gSurfaceFns.planeCount(s) > 1;

	out.surface = static_cast<OSObject *>(s);
	out.memory = gSurfaceFns.memory(s);
	out.base = gSurfaceFns.baseOffset(s);
	out.bytesPerRow = gSurfaceFns.bytesPerRow(s);
	out.width = gSurfaceFns.width(s);
	out.height = gSurfaceFns.height(s);
	out.format = d[5] >> 16;

	if (planar && plane < gSurfaceFns.planeCount(s)) {
		out.base += gSurfaceFns.planeOffset(s, plane);
		out.bytesPerRow = gSurfaceFns.planeBytesPerRow(s, plane);
		out.width = gSurfaceFns.planeWidth(s, plane);
		out.height = gSurfaceFns.planeHeight(s, plane);
	}

	// The texture may cover only the top left of its surface: CoreImage renders into surfaces from a pool, larger than the image
	// (582x1174 in 640x1216, RGhA). Its size is the texture's then.
	const bool bgra = out.format != 80 || planar || gSurfaceFns.pixelFormat(s) == 0x42475241;   // 'BGRA'
	const bool same = gSurfaceFns.surfaceId(s) == sid && bgra && d[6] && d[7] && d[6] <= out.width && d[7] <= out.height;

	out.width = same ? d[6] : out.width;
	out.height = same ? d[7] : out.height;

	const uint64_t used = out.base + static_cast<uint64_t>(out.bytesPerRow) * out.height;
	const bool fits = out.memory && out.bytesPerRow >= out.width * fm->bpp && used <= out.memory->getLength();
	const bool ok = same && fits && out.memory->prepare() == kIOReturnSuccess;

	if (!ok) {
		if (refused++ < 16)
			pvlog("host: render: IOSurface %u (resource %u): id %u, format 0x%08x, %ux%u (Metal's %ux%u), %u bytes per row: not as expected", sid, id,
			      gSurfaceFns.surfaceId(s), gSurfaceFns.pixelFormat(s), out.width, out.height, d[6], d[7], out.bytesPerRow);
		out.memory = nullptr;   // not prepared: nothing to complete
	}

	return ok;
}

// M2: a render pass with nothing drawn: Render.writeDescriptor (584 bytes) carries colour attachment 0 as {u32 texture at +0x4c, u32 (store action
// << 16) | load action at +0x60, double clear colour[4] at +0x68} (mtlprobe: texture 1, 0x00010002 = clear and store, 0.25 0.5 0.75 1; measured
// 2026-10-21). A clear is a draw over the texture, or a fill of its memory with the colour as a pixel of its format (BGRA8 when the descriptor
// names none). Nothing else of a pass is carried out here (its draws come as their own commands).
// A render pass descriptor's non-zero dwords, as offset:value (with debug.rdna4_pvtalk). Its colour attachments are 0x3c apart from
// 0x4c, eight of them, each {u32 texture, ..., u32 (store << 16) | load at +0x14, double clear colour[4] at +0x1c} (WindowServer's passes,
// 2026-11-05).
static void logPassDescriptor(const uint8_t *p, uint32_t bytes)
{
	char line[200];
	size_t at = 0;

	for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
		uint32_t v;

		memcpy(&v, p + i, 4);
		if (!v)
			continue;

		at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, " %03x:%08x", i, v));
		if (at > sizeof(line) - 16) {
			pvlog("host: render: pass descriptor%s", line);
			at = 0;
		}
	}

	if (at)
		pvlog("host: render: pass descriptor%s", line);
}

uint32_t RDNA4PvHost::runRenderPass(uint32_t task, const uint8_t *p, uint32_t bytes) {
	if (!gCompute || bytes < 0x88)
		return 0;
	uint32_t tex, actions;
	uint64_t rgba[4];   // the clear colour as IEEE-754 doubles, converted with integers only (no floating point in the kernel)
	memcpy(&tex, p + 0x4c, 4);
	memcpy(&actions, p + 0x60, 4);
	memcpy(rgba, p + 0x68, sizeof(rgba));

	if (gTalk)
		logPassDescriptor(p, bytes);

	if ((actions & 0xffff) != 2 || !tex)   // MTLLoadActionClear
		return tex;

	flushHeld();
	const uint32_t r = unorm8(rgba[0]), g = unorm8(rgba[1]), b = unorm8(rgba[2]), a = unorm8(rgba[3]);
	uint64_t va, len;
	uint32_t bpr = 0, w = 0, h = 0, format = 0;
	const bool found = textureRange(task, tex, &va, &len, &bpr, &w, &h, &format) && !(len & 3);
	// R, G, B, A in memory for RGBA8 (70, 71), else B, G, R, A
	const uint32_t pixel = format == 70 || format == 71 ? r | g << 8 | b << 16 | a << 24 : b | g << 8 | r << 16 | a << 24;
	// RGBA16Float: only to zero so far (a 32-bit pattern cannot hold four halves; transparent black is what CoreAnimation clears to)
	const bool half = format == 115;
	const bool zero = !(rgba[0] | rgba[1] | rgba[2] | rgba[3]);
	bool ok = found && (!half || zero);
	uint64_t handle = 0, gpu = 0;
	Surface surface;

	mMapFloor = mMapClock;

	// On the card in the graphics ring, after the draws before: the host does not wait for them (the clears of WindowServer's passes were
	// most of its waits with two displays). By a draw when the colour block can write the texture (its rows 128 bytes apart: any colour of
	// RGBA16Float too), else by CP DMA.
	const uint32_t tp = half ? 8 : 4;
	const bool drawable = found && bpr && !(bpr % tp) && h && static_cast<uint64_t>(bpr) * h <= len &&
	                      (format == 70 || format == 71 || format == 80 || format == 81 || half || (!format && w * 4 == bpr));

	if (drawable && mapGuest(task, va, len, handle, gpu) && clearByDraw(task, tex, gpu, bpr / tp, w, h, half, format != 70 && format != 71, rgba)) {
		dropShadows(nullptr, task, va);

		return tex;
	}

	if (drawable && (bpr % 128) && clearHeld(task, tex, va, w, h, bpr, half, format != 70 && format != 71, rgba))
		return tex;

	if (ok && mapGuest(task, va, len, handle, gpu) && clearOnCard(task, tex, gpu, len, half ? 0 : pixel)) {
		clearShadows(nullptr, task, va, tex, half ? 0 : pixel);

		return tex;
	}

	if (!found && surfaceRange(task, tex, surface) && surface.format == 80 && surface.width <= 16384) {
		const uint64_t bytes = static_cast<uint64_t>(surface.bytesPerRow) * surface.height;

		const bool mapped = onCard(surface, bytes, gpu) || mapMemory(surface.memory, surface.base, bytes, handle, gpu);

		if (mapped && !(surface.bytesPerRow % 4) &&
		    clearByDraw(task, tex, gpu, surface.bytesPerRow / 4, surface.width, surface.height, false, true, rgba)) {
			dropShadows(surface.surface, task, surface.base);

			return tex;
		}

		if (mapped && clearOnCard(task, tex, gpu, bytes, pixel)) {
			clearShadows(surface.surface, task, surface.base, tex, pixel);

			return tex;
		}
	}

	waitDraws();   // cleared with SDMA or the CPU, after the draws before
	ok = ok && dmaGuest(task, va, 0, half ? 0 : pixel, len);
	if (found)
		dropShadows(nullptr, task, va);

	if (!ok && (surface.memory || surfaceRange(task, tex, surface)) && surface.format == 80 && surface.width <= 16384) {   // a BGRA8 IOSurface: row by row
		cardToMemory(surface.surface);
		dropShadows(surface.surface, task, surface.base);
		auto *line = static_cast<uint32_t *>(IOMalloc(surface.width * 4));
		ok = line;
		for (uint32_t x = 0; line && x < surface.width; x++)
			line[x] = pixel;
		for (uint32_t y = 0; ok && y < surface.height; y++)
			ok = surface.memory->writeBytes(surface.base + static_cast<uint64_t>(y) * surface.bytesPerRow, line, surface.width * 4) == surface.width * 4;
		if (line)
			IOFree(line, surface.width * 4);
	}
	if (mDrawLines[task & 63] < 40 && ++mDrawLines[task & 63])
		pvlog("host: render: clear texture %u to RGBA8 (%u, %u, %u, %u) = BGRA8 pixel 0x%08x on the card: %s", tex, r, g, b, a, pixel, ok ? "done" : "NOT done");
	return tex;
}

// PKT3 DMA_DATA filling `bytes` at `dst` with the dword `pattern` (SRC_SEL DATA), through the L2.
static uint32_t dmaFill(uint32_t *out, uint32_t pattern, uint64_t dst, uint32_t bytes, bool sync)
{
	out[0] = 0xc0055000u;                                   // PKT3(DMA_DATA, 5)
	out[1] = 3u << 20 | 2u << 29 | (sync ? 1u << 31 : 0);    // DST_SEL DST_ADDR_TC_L2, SRC_SEL DATA; CP_SYNC
	out[2] = pattern;
	out[3] = 0;
	out[4] = static_cast<uint32_t>(dst);
	out[5] = static_cast<uint32_t>(dst >> 32);
	out[6] = bytes & 0x3ffffff;                             // BYTE_COUNT
	return 7;
}

// Packets that hold the CP until the work before them is done and its caches written back: a RELEASE_MEM (end of pipe, caches flushed)
// to a dword of ours, then WAIT_REG_MEM for it. CP DMA runs beside the draws otherwise: a copy could read a frame, or a fill write a
// texture, while draws before it in the ring were still at it. 0 dwords when the dword is not made.
uint32_t RDNA4PvHost::idleBarrier(uint32_t *out)
{
	uint64_t cpu = 0;

	if (!mIdle.handle && gCompute->rtAllocHost(this, kernel_task, 0x1000, 0, mIdle.handle, mIdle.gpu, cpu) == kIOReturnSuccess)
		mIdle.cpu = reinterpret_cast<uint8_t *>(cpu);

	if (!mIdle.cpu)
		return 0;

	const uint32_t seq = ++mIdleSeq ? mIdleSeq : ++mIdleSeq;
	uint32_t n = Pm4::releaseMem(out, mIdle.gpu, seq);

	out[n++] = 0xc0053c00u;                       // PKT3(WAIT_REG_MEM, 5)
	out[n++] = 3u | 1u << 4 | 1u << 8;              // FUNCTION equal, MEM_SPACE memory, ENGINE_SEL PFP
	out[n++] = static_cast<uint32_t>(mIdle.gpu);
	out[n++] = static_cast<uint32_t>(mIdle.gpu >> 32);
	out[n++] = seq;
	out[n++] = 0xffffffffu;
	out[n++] = 4;                                 // poll interval

	return n;
}

// A texture's `bytes` at `gpu` filled with `pattern` by the card, left in flight like a draw into it (task << 16 | tex among the draws
// in flight: a CPU copy of it waits). False if the IB does not hold it or the buffers are not made yet.
bool RDNA4PvHost::clearOnCard(uint32_t task, uint32_t tex, uint64_t gpu, uint64_t bytes, uint32_t pattern)
{
	static constexpr uint32_t kChunk = 1u << 20;
	static uint32_t ib[RDNA4_TRIRUN_PAGE / 4];   // ponytail: one host, draws run one at a time on its work loop
	uint32_t n = 0;
	uint64_t at = 0;

	if (!mGfxIb.cpu || !bytes || (bytes & 3) || !(n = idleBarrier(ib)))
		return false;

	const bool timed = profiling();

	n += timed ? timeStamp(ib + n, false) : 0;

	while (at < bytes && n + 7 + 8 <= RDNA4_TRIRUN_PAGE / 4) {
		const uint32_t len = static_cast<uint32_t>(bytes - at < kChunk ? bytes - at : kChunk);

		at += len;
		n += dmaFill(ib + n, pattern, gpu + at - len, len, at >= bytes);
	}

	if (at < bytes)
		return false;

	n += timed ? timeStamp(ib + n, true) : 0;

	const uint64_t slotAt = static_cast<uint64_t>(takeSlot()) * RDNA4_TRIRUN_PAGE;
	uint64_t fence = 0;

	memcpy(mGfxIb.cpu + slotAt, ib, n * 4);
	__sync_synchronize();   // seen by the card (snooped) before the submit

	roomForDraw();

	if (submitGfx(mGfxIb.gpu + slotAt, n, fence) != kIOReturnSuccess)
		return false;

	mDrawSerial++;
	mClearBytes += bytes;
	flying(fence, "clear");
	dirtied(task, tex, fence);

	return true;
}

static uint32_t dmaData(uint32_t *out, uint64_t src, uint64_t dst, uint32_t bytes, bool sync);
static uint32_t *streamReg(uint32_t *ib, uint32_t n, uint32_t opcode, uint32_t reg);

// A clear by the 3D pipeline: one triangle over the whole target (`width` x `height`, rows `pitch` pixels apart, at `gpu`) in the stream of
// the host's draws, drawn by nggfill.s and psfill.s with the colour (Metal's doubles, as floats) in the pixel shader's user SGPRs, the colour
// block converting it to the target's format. CP DMA filled 23 GB/s, and WindowServer clears each frame's 4K target and its intermediate
// textures: 9.8 GB in five seconds of a window drag. Left in flight like a draw. False when the target is not one the colour block writes.
bool RDNA4PvHost::clearByDraw(uint32_t task, uint32_t tex, uint64_t gpu, uint32_t pitch, uint32_t width, uint32_t height, bool half,
                              bool bgra, const uint64_t rgba[4])
{
	static uint32_t ib[RDNA4_TRIRUN_PAGE / 4];   // ponytail: one host, draws run one at a time on its work loop
	const uint32_t bpp = half ? 8 : 4;

	if ((gInPlace & 256) || !mGfxIb.cpu || !mGfxMisc.cpu || !mGfxRings.handle || !width || !height || width > 16384 || height > 16384 ||
	    pitch < width || (gpu & 0xff) || (pitch * bpp) % 128)
		return false;

	if (!mFillCode.handle) {   // the pair, each padded with s_code_end for the SQ's prefetch
		static uint32_t code[RDNA4_TRI_SHADER_PAD];
		IOReturn r = gCompute->rtAlloc(this, RDNA4_TRI_CODE_BYTES, mFillCode.handle, mFillCode.gpu);

		for (unsigned k = 0; k < 2 && r == kIOReturnSuccess; k++) {
			const uint32_t *kernel = k ? kPsfillKernel : kNggfillKernel;
			const uint32_t dwords = k ? sizeof(kPsfillKernel) / 4 : sizeof(kNggfillKernel) / 4;

			for (uint32_t i = 0; i < RDNA4_TRI_SHADER_PAD; i++)
				code[i] = i < dwords ? kernel[i] : Isa::kSCodeEnd;

			r = gCompute->rtCopy(this, mFillCode.handle, k ? RDNA4_TRI_PS_OFFSET : 0, kernel_task, reinterpret_cast<mach_vm_address_t>(code), sizeof(code), true);
		}

		if (r != kIOReturnSuccess) {
			if (mFillCode.handle)
				gCompute->rtFree(this, mFillCode.handle);
			mFillCode = {};
			return false;
		}
	}

	const uint64_t slotAt = static_cast<uint64_t>(takeSlot()) * RDNA4_TRIRUN_PAGE;
	const rdna4_tri_va where = {mFillCode.gpu, gpu, rdna4_trirun_rings_va(mGfxRings.gpu), mGfxMisc.gpu + slotAt};
	uint32_t n = rdna4_tri_record(ib, &where);
	uint64_t fence = 0;

	struct Patch { uint32_t opcode, reg, value; } patch[] = {
		{0x69, 0x31e, (pitch - 1) << 16 | (height - 1)},                     // CB_COLOR0_ATTRIB2: MIP0_WIDTH (the pitch), MIP0_HEIGHT
		{0x69, 0x3b0, half ? 0x0002070cu : bgra ? 0x0002880au : 0x0002800au},   // CB_COLOR0_INFO: 16_16_16_16 float, 8_8_8_8 unorm (COMP_SWAP ALT: BGRA8)
		{0x69, 0x82, (width - 1) | (height - 1) << 16},                     // PA_SC_WINDOW_SCISSOR_BR: inclusive on gfx12
		{0x69, 0x94, 0},                                                    // PA_SC_VPORT_SCISSOR_0_TL, _BR: the whole target
		{0x69, 0x95, (width - 1) | (height - 1) << 16},
		{0x69, 0x10f, halfAsFloat(width, false)},                           // PA_CL_VPORT_XSCALE, XOFFSET, YSCALE, YOFFSET: the whole target
		{0x69, 0x110, halfAsFloat(width, false)},
		{0x69, 0x111, halfAsFloat(height, true)},
		{0x69, 0x112, halfAsFloat(height, false)},
		{0x69, 0x1e0, 0},                                                   // CB_BLEND0_CONTROL: off
		{0x69, 0x1d8, 0x06000600u},                                         // SX_MRT0_BLEND_OPT: BLEND_DISABLED
		{0x69, 0x214, 0xf},                                                 // CB_TARGET_MASK
		{0x69, 0x195, 9u},                                                  // SPI_SHADER_COL_FORMAT: 32_ABGR
	};
	for (const Patch &pt : patch) {
		uint32_t *d = n ? streamReg(ib, n, pt.opcode, pt.reg) : nullptr;

		if (!d)
			return false;

		*d = pt.value;
	}

	uint32_t *rsrc2 = streamReg(ib, n, 0x76, 0x0b);   // SPI_SHADER_PGM_RSRC2_PS: USER_SGPR [5:1] = 4, the colour

	if (!rsrc2)
		return false;

	*rsrc2 = (*rsrc2 & ~0x3eu) | 4u << 1;

	// SPI_SHADER_USER_DATA_PS_0..3: the colour, before the draw
	bool coloured = false;

	for (uint32_t i = 0; i + 3 <= n && !coloured; i++) {
		if (ib[i] != 0xc0012d00u)   // DRAW_INDEX_AUTO
			continue;

		if (n + 6 + 16 > sizeof(ib) / 4)
			return false;

		memmove(&ib[i + 6], &ib[i], (n - i) * 4);
		ib[i] = 0xc0047600u;
		ib[i + 1] = 0x0c;
		for (uint32_t c = 0; c < 4; c++)
			ib[i + 2 + c] = doubleAsFloat(rgba[c]);
		n += 6;
		coloured = true;
	}

	if (!coloured)
		return false;

	if (profiling()) {   // timed: its start and end
		memmove(ib + 8, ib, n * 4);
		timeStamp(ib, false);
		n += 8;
		n += timeStamp(ib + n, true);
	}

	mClearBytes += static_cast<uint64_t>(pitch) * bpp * height;

	// With the draws put together: alone, each of WindowServer's clears closed a batch (some 400 a second). Its slot and the target's
	// mapping are noted with a submit no earlier than the batch's.
	if (batchDraw(ib, n, task, tex))
		return true;

	memcpy(mGfxIb.cpu + slotAt, ib, n * 4);
	__sync_synchronize();   // seen by the card (snooped) before the submit

	roomForDraw();

	if (submitGfx(mGfxIb.gpu + slotAt, n, fence) != kIOReturnSuccess)
		return false;

	mDrawSerial++;
	flying(fence, "clear by draw");
	dirtied(task, tex, fence);

	return true;
}


// `rows` rows of `rowLen` bytes from `src`, `srcPitch` apart, to `dst`, `dstPitch` apart, copied by the card in the graphics ring after
// the work before them, left in flight; in chunks of 1 MiB when both pitches are the row's. False when the IB does not hold them.
bool RDNA4PvHost::rowsOnCard(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t rowLen, uint32_t rows)
{
	static constexpr uint32_t kChunk = 1u << 20;
	const bool whole = srcPitch == rowLen && dstPitch == rowLen;
	const uint64_t total = whole ? static_cast<uint64_t>(rowLen) * rows : rows;
	uint64_t cpu = 0, fence = 0;

	if (!rowLen || !rows || (!whole && rows > kDmaRows))
		return false;

	if (!mDmaIb.handle && gCompute->rtAllocHost(this, kernel_task, kDmaIbBytes * kDmaSlots, RDNA4_HOST_EXECUTABLE, mDmaIb.handle, mDmaIb.gpu,
	                                            cpu) == kIOReturnSuccess)
		mDmaIb.cpu = reinterpret_cast<uint8_t *>(cpu);

	if (!mDmaIb.cpu)
		return false;

	const uint32_t slot = mDmaSlot++ % kDmaSlots;

	if (mDmaFence[slot] && !gCompute->rtGfxFenceDone(this, static_cast<uint32_t>(mDmaFence[slot])))
		waitFor(mDmaFence[slot]);   // its IB still read by the card

	auto *ib = reinterpret_cast<uint32_t *>(mDmaIb.cpu + static_cast<uint64_t>(slot) * kDmaIbBytes);
	uint32_t n = idleBarrier(ib);
	const bool timed = n && profiling();

	n += timed ? timeStamp(ib + n, false) : 0;

	for (uint64_t i = 0; n && i < total && n + 7 + 8 <= kDmaIbBytes / 4;) {
		const uint64_t from = whole ? i : i * srcPitch, to = whole ? i : i * dstPitch;
		const uint32_t len = whole ? static_cast<uint32_t>(total - i < kChunk ? total - i : kChunk) : rowLen;

		i += whole ? len : 1;
		n += dmaData(ib + n, src + from, dst + to, len, i >= total);
	}

	if (!n || n + 7 + 8 > kDmaIbBytes / 4)
		return false;

	n += timed ? timeStamp(ib + n, true) : 0;

	// With the draws put together, as clears are: alone, each shadow's fill and write-back closed a batch.
	if (batchDraw(ib, n, 0, 0))
		return true;

	__sync_synchronize();   // seen by the card (snooped) before the submit
	roomForDraw();

	if (submitGfx(mDmaIb.gpu + static_cast<uint64_t>(slot) * kDmaIbBytes, n, fence) != kIOReturnSuccess)
		return false;

	mDrawSerial++;
	flying(fence, "copy");
	mDmaFence[slot] = fence;

	return true;
}

RDNA4PvHost::CardSurface *RDNA4PvHost::cardSurface(const void *surface)
{
	for (CardSurface &c : mCardSurface)
		if (surface && c.surface == surface)
			return &c;

	return nullptr;
}

// A surface just presented kept on the card from its next use on: a frame of a display, drawn and presented again and again.
void RDNA4PvHost::keepOnCard(void *surface)
{
	CardSurface *slot = nullptr;
	IOMemoryDescriptor *md = surface ? gSurfaceFns.memory(surface) : nullptr;
	const uint64_t bpr = md ? gSurfaceFns.bytesPerRow(surface) : 0, bytes = bpr * (md ? gSurfaceFns.height(surface) : 0);

	if ((gInPlace & 32) || !md || cardSurface(surface) || gSurfaceFns.planeCount(surface) > 1 || (bpr % 128) || (gSurfaceFns.baseOffset(surface) & 0xff) || !bytes ||
	    gSurfaceFns.baseOffset(surface) + bytes > md->getLength())
		return;

	dropReleasedSurfaces();

	for (CardSurface &c : mCardSurface)   // a free one, else the least recently used
		slot = !c.surface ? &c : slot && (!slot->surface || slot->used <= c.used) ? slot : &c;

	if (slot->surface) {
		cardToMemory(slot->surface);
		dropCardSurface(*slot);
	}

	static_cast<OSObject *>(surface)->retain();
	*slot = {static_cast<OSObject *>(surface), md, gSurfaceFns.baseOffset(surface), bytes, 0, 0, ++mCardClock, false};
}

// Surface `s` on the card (its VRAM copy at `gpu`) when it is kept there; made and loaded with what the guest's memory has on first use.
// Not again when its lock seeds move: a frame's do with every one WindowServer draws, and the CPU does not write frames.
bool RDNA4PvHost::onCard(const Surface &s, uint64_t bytes, uint64_t &gpu)
{
	CardSurface *c = cardSurface(s.surface);
	uint64_t handle = 0, src = 0;

	if (c && (gInPlace & 32))   // switched off: back to the guest's memory
		cardToMemory(c->surface);

	if (!c || (gInPlace & 32) || s.base != c->base || bytes > c->bytes)
		return false;

	if (!c->handle && gCompute->rtAlloc(this, c->bytes, c->handle, c->gpu) != kIOReturnSuccess) {
		c->handle = 0;
		return false;
	}

	if (!c->loaded) {
		if (c->memory->prepare() != kIOReturnSuccess)
			return false;

		const bool ok = mapMemory(c->memory, c->base, c->bytes, handle, src) && rowsOnCard(src, static_cast<uint32_t>(c->bytes), c->gpu,
		                                                                                    static_cast<uint32_t>(c->bytes), static_cast<uint32_t>(c->bytes), 1);

		c->memory->complete();

		if (!ok)
			return false;

		c->loaded = true;
		mCardLoads++;
	}

	c->used = ++mCardClock;
	gpu = c->gpu;

	return true;
}

// What the card drew into a surface it keeps written back into the guest's memory, before the CPU reads or writes it there; the surface
// is then the guest's memory's again until it is presented anew.
void RDNA4PvHost::cardToMemory(const void *surface)
{
	CardSurface *c = cardSurface(surface);
	uint64_t handle = 0, dst = 0;

	if (!c)
		return;

	if (c->loaded && c->memory->prepare() == kIOReturnSuccess) {
		const uint32_t bytes = static_cast<uint32_t>(c->bytes);

		if (mapMemory(c->memory, c->base, c->bytes, handle, dst) && rowsOnCard(c->gpu, bytes, dst, bytes, bytes, 1))
			mCardBacks++;

		c->memory->complete();
	}

	waitDraws();
	dropCardSurface(*c);
}

// A kept surface given up: its VRAM freed (after the draws that use it), the surface released.
void RDNA4PvHost::dropCardSurface(CardSurface &c)
{
	if (c.handle) {
		waitDraws();
		gCompute->rtFree(this, c.handle);
	}

	if (c.surface)
		c.surface->release();

	c = {};
}

// The kept surfaces only the host still holds (the guest freed them) given up, without writing them back.
void RDNA4PvHost::dropReleasedSurfaces()
{
	for (CardSurface &c : mCardSurface)
		if (c.surface && c.surface->getRetainCount() <= 1)
			dropCardSurface(c);
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


// A texture a shader samples or reads, as the guest has it: a Metal texture's memory (resource type 2, or 3 read-only, with the same
// descriptor: 29 dwords with one mip level, its format record {0x2c, its own id, (Metal pixel format << 16) | usage} from dword 18), or an
// IOSurface (type 5: surfaceRange, BGRA8 only).
struct RDNA4PvHost::TextureSource {
	uint32_t type = 0, format = 0, usage = 0, width = 0, height = 0, depth = 1, rowBytes = 0;
	uint64_t va = 0, length = 0, imageBytes = 0;   // imageBytes: a 3D texture's slices apart
	uint32_t slices = 1;                            // an array's, sliceBytes apart
	uint64_t sliceBytes = 0;
	Surface surface;
};

bool RDNA4PvHost::textureSource(uint32_t task, uint32_t id, TextureSource &out)
{
	uint32_t rec[3], td[160] = {};   // a mipmapped texture's levels come before its format (6 levels: 29 dwords more)

	if (!readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(rec), sizeof(rec)))
		return false;

	out.type = rec[0] & 0xff;
	if (out.type == 5) {
		if (!surfaceRange(task, id, out.surface))
			return false;

		out.format = out.surface.format;
		out.width = out.surface.width;
		out.height = out.surface.height;
		out.rowBytes = out.surface.bytesPerRow;
		out.length = static_cast<uint64_t>(out.rowBytes) * out.height;

		return true;
	}

	const uint32_t bytes = (rec[0] >> 8) < sizeof(td) ? rec[0] >> 8 : sizeof(td);
	const bool read = readGuest(task, rec[1], reinterpret_cast<uint8_t *>(td), bytes);
	static uint32_t dumped[16];

	if (read && out.type != 2 && dumped[out.type & 15]++ < 4)
		pvlog("host: render: texture %u of type %u, %u bytes: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
		      "%08x %08x %08x %08x %08x %08x %08x", id, out.type, rec[0] >> 8, td[0], td[1], td[2], td[3], td[4], td[5], td[6], td[7], td[8], td[9],
		      td[10], td[11], td[12], td[13], td[14], td[15], td[16], td[17], td[18], td[19], td[20], td[21], td[22], td[23]);

	if ((out.type != 2 && out.type != 3) || bytes < 4 * 29 || !read)
		return false;

	for (uint32_t j = 19; j + 1 < bytes / 4; j++)
		if (td[j - 1] == 0x2c && td[j] == id) {
			out.format = td[j + 1] >> 16;
			out.usage = td[j + 1] & 0xffff;
		}

	out.width = td[15];
	out.height = td[16];
	out.depth = td[17] > 1 ? td[17] : 1;
	out.rowBytes = td[13];
	out.va = static_cast<uint64_t>(td[2]) << 12;
	out.length = td[0] | static_cast<uint64_t>(td[1]) << 32;
	out.imageBytes = (td[11] | static_cast<uint64_t>(td[12]) << 32) / out.depth;   // level 0's bytes

	// Where in its allocation the texture starts: dwords 4-5 (CoreAnimation's text images start 0x100 into theirs). Read from the
	// allocation's start, text came out shifted by a row and a bit, its end at its start (rdna4_pvtex, 2026-11-05).
	const uint64_t start = td[4] | static_cast<uint64_t>(td[5]) << 32;

	if (start >= out.length)
		return false;

	out.va += start;
	out.length -= start;
	// an array's slices (WindowServer's 16384x1x3 R32Float: dword 3 0x00030001, dwords 6-7 0x10000; measured 2026-11-05)
	out.slices = td[3] >> 16 ? td[3] >> 16 : 1;
	out.sliceBytes = td[6] | static_cast<uint64_t>(td[7]) << 32;

	static uint32_t volumes;

	if (out.depth > 1 && volumes++ < 8)
		pvlog("host: render: 3D texture %u: %ux%ux%u, level 0 at +%llu, %llu bytes, %u bytes per row, of %llu bytes", id, out.width, out.height,
		      out.depth, td[9] | static_cast<unsigned long long>(td[10]) << 32, td[11] | static_cast<unsigned long long>(td[12]) << 32, out.rowBytes,
		      static_cast<unsigned long long>(out.length));

	return true;
}

// Row y's first `bytes` bytes of a texture textureSource found; of a 3D texture, row y % height of slice y / height.
bool RDNA4PvHost::readTextureRow(uint32_t task, const TextureSource &src, uint32_t y, uint8_t *dst, uint32_t bytes)
{
	const uint64_t at = static_cast<uint64_t>(y / src.height) * src.imageBytes + static_cast<uint64_t>(y % src.height) * src.rowBytes;

	if (src.surface.memory) {
		cardToMemory(src.surface.surface);

		return src.surface.memory->readBytes(src.surface.base + at, dst, bytes) == bytes;
	}

	return readGuest(task, src.va + at, dst, bytes);
}

// M4: a stage's (or a kernel's) textures 0-15 and samplers 0-15: each texture's mip level 0 staged into the runtime at a pitch of a multiple
// of 128 bytes (gfx12's linear images), and the T#s and S#s written into `table` (its layout in include/rdna4mtlc.h, shared with
// air2amdgpu.cpp). A texture resource (measured, docs/m1-stream.md s.5b):
// {u64 bytes, u32 start page, u32 (array << 16) | mips, ..., from dword 9 per level {u64 offset, u64 bytes, u64 bytes per row, u32 width,
// height, depth}, then ..., 0x2c, its own id, (Metal pixel format << 16) | usage}. A sampler resource (type 7, tag 3, 36 bytes) packs at
// dword 3: mag [1:0], min [3:2], address s [10:8], t [14:12], r [18:16], mip [25:24], anisotropy [30:26], normalized [31].
// The bytes of a small texture's rows that are not 0 (-1: too large or not read): an image the guest has not filled yet reads all 0.
int RDNA4PvHost::inkedBytes(uint32_t task, const TextureSource &src, uint32_t bpp)
{
	static uint8_t row[1024];
	const uint32_t bytes = src.width * bpp;
	int inked = 0;

	if (src.height > 256 || !bytes || bytes > sizeof(row))
		return -1;

	for (uint32_t y = 0; y < src.height; y++) {
		if (!readTextureRow(task, src, y, row, bytes))
			return -1;

		for (uint32_t i = 0; i < bytes; i++)
			inked += row[i] != 0;
	}

	return inked;
}

// `writes`: the shader may write its textures (a kernel, or a fragment shader without a colour output); those with
// MTLTextureUsageShaderWrite are then copied back after it. CoreAnimation makes its textures with that usage and draws only read them.
bool RDNA4PvHost::stageTextures(uint32_t task, const Bindings &bound, uint32_t *table, StagedTexture (&out)[16], bool talk, const char *stageName,
                                bool writes) {
	bool ok = true;
	for (unsigned k = 0; k < 16 && ok; k++) {
		const uint32_t id = bound.texture[k];
		TextureSource src;
		if (!id) {   // a 1x1 texel of the spare buffer: an image instruction on an all-zero T# can stall the texture unit
			linearImage(table + (RDNA4_MTLC_TABLE_TEXTURES + 32 * k) / 4, mGfxSpare.gpu, 42, 4 | 5 << 3 | 6 << 6 | 7 << 9, 1, 1, 64);
			continue;
		}
		bool earlier = false;   // the same texture in an earlier slot: its T#
		for (unsigned j = 0; j < k && !earlier; j++)
			if ((earlier = bound.texture[j] == id))
				memcpy(table + (RDNA4_MTLC_TABLE_TEXTURES + 32 * k) / 4, table + (RDNA4_MTLC_TABLE_TEXTURES + 32 * j) / 4, 32);
		if (earlier)
			continue;

		ok = textureSource(task, id, src);

		// Not a texture at all: a binding left from an earlier encoder whose id was used again (Xgc's slot 3 named a function), which the
		// shader does not sample. The spare texel as for no binding; refused, it took the whole draw with it.
		if (!ok && src.type != 2 && src.type != 3 && src.type != 5) {
			linearImage(table + (RDNA4_MTLC_TABLE_TEXTURES + 32 * k) / 4, mGfxSpare.gpu, 42, 4 | 5 << 3 | 6 << 6 | 7 << 9, 1, 1, 64);
			ok = true;
			continue;
		}

		// A 1D array (WindowServer's colour lookup tables) staged as a 3D image's slices are, sampled through a 1D-array T#.
		const bool array1d = ok && src.slices > 1 && src.depth == 1 && src.height == 1 && src.sliceBytes;

		if (array1d) {
			src.depth = src.slices;
			src.imageBytes = src.sliceBytes;
		}

		const uint32_t format = src.format, usage = src.usage;
		const TextureFormat *fm = textureFormat(format);
		const uint32_t w = src.width, h = src.height, depth = src.depth, rowBytes = src.rowBytes;
		if (talk)
			pvlog("host: render:   %s texture %u: resource %u (type %u), format %u, %ux%ux%u, %u bytes per row, %d bytes not 0", stageName, k, id,
			      src.type, format, w, h, depth, rowBytes, ok && fm ? inkedBytes(task, src, fm->bpp) : -1);
		ok = ok && fm && w && h && w <= 16384 && h <= 16384 && rowBytes >= w * fm->bpp && static_cast<uint64_t>(rowBytes) * h <= src.length;
		ok = ok && (depth == 1 || (depth <= 2048 && src.imageBytes >= static_cast<uint64_t>(rowBytes) * h && src.imageBytes * depth <= src.length));
		if (!ok) {
			if (talk || mTexLines++ < 2)   // per stats period
				pvstat("host: render: %s texture %u (resource %u, type %u): format %u, %ux%ux%u, %u bytes per row: not supported yet", stageName, k,
				      id, src.type, format, w, h, depth, rowBytes);
			break;
		}
		// In place when the hardware can read the texture as the guest has it: 2D, its rows a multiple of 128 bytes apart (the rig's linear pitch),
		// or a single row, and its start 256-byte aligned (a T#'s base has no lower bits: a texture off it was read shifted, the end of each
		// row at the start of the next; mapMemory refuses an IOSurface's).
		const uint64_t used = static_cast<uint64_t>(rowBytes) * h;
		uint64_t gpu = 0;

		const bool pitched = h == 1 || (!(rowBytes % 128) && !(rowBytes % fm->bpp));
		const bool based = src.surface.memory || !(src.va & 0xff);
		const bool kind = src.surface.memory ? !(gInPlace & 4) : !(gInPlace & 8);
		const bool shaderWrites = writes && (usage & 2) && src.type == 2;
		uint32_t shadowPitch = 0;
		// a large read-only texture through a shadow on the card first (read across the bus each draw in place), in place when it gets none
		const bool large = src.type == 3 && used >= (4u << 20) && used <= kShadowMax && !(gInPlace & 1024);
		const bool shadowed = large && !(gInPlace & 16) && shadowTexture(task, id, src, fm->bpp, gpu, shadowPitch);
		const bool inPlace = !shadowed && (gInPlace & 1) && kind && depth == 1 && pitched && based;

		if (inPlace && (src.surface.memory ? onCard(src.surface, used, gpu) || mapMemory(src.surface.memory, src.surface.base, used, out[k].handle, gpu)
		                                   : mapGuest(task, src.va, used, out[k].handle, gpu))) {
			out[k].va = 0;   // nothing to copy back: the card writes the guest's memory itself
			out[k].writable = false;
			linearImage(table + (RDNA4_MTLC_TABLE_TEXTURES + 32 * k) / 4, gpu, fm->gfx, fm->sel, w, h, h == 1 ? w : rowBytes / fm->bpp);
			mInPlace++;
			continue;
		}

		// Through a shadow; not one the shader writes (copied back after the draw as before).
		if (shadowed || (!shaderWrites && !(gInPlace & 16) && !large && shadowTexture(task, id, src, fm->bpp, gpu, shadowPitch))) {
			uint32_t *t = table + (RDNA4_MTLC_TABLE_TEXTURES + 32 * k) / 4;

			if (array1d)
				linearArray1d(t, gpu, fm->gfx, fm->sel, w, depth);
			else if (depth > 1)
				linearVolume(t, gpu, fm->gfx, fm->sel, w, h, depth);
			else
				linearImage(t, gpu, fm->gfx, fm->sel, w, h, shadowPitch / fm->bpp);

			mCopied++;
			continue;
		}

		mCopied++;
		if (mStagedLines++ < 4)
			pvstat("host: render: %s texture %u (resource %u, type %u): format %u, usage 0x%x, %ux%ux%u, %u bytes per row: copied for the draw%s",
			       stageName, k, id, src.type, format, usage, w, h, depth, rowBytes, shaderWrites ? " (the shader writes it)" : "");

		const uint32_t texPitch = (w * fm->bpp + 127) & ~127u;   // bytes
		// a 3D image's slices: height rows of the row's bytes aligned to 256, not to the pitch's 128 (rdna4-mtltex3d, 2026-11-04)
		const uint64_t slice = depth > 1 ? static_cast<uint64_t>((w * fm->bpp + 255) & ~255u) * h : static_cast<uint64_t>(texPitch) * h;
		const uint64_t bytes = slice * depth;
		uint8_t *pooled = nullptr;
		const bool inPool = poolTake(bytes, gpu, pooled);
		auto *copy = inPool ? pooled : static_cast<uint8_t *>(IOMalloc(bytes));
		const char *step = "staging memory";
		uint32_t y = 0;

		ok = copy != nullptr;
		if (ok && depth > 1)
			memset(copy, 0, bytes);

		if (drawing(task, id))   // copied by the CPU: what draws in flight put into it first
			waitDraws();

		const uint64_t copyFrom = nowNs();

		for (; y < h * depth && ok; y++)
			ok = readTextureRow(task, src, y, copy + (y / h) * slice + static_cast<uint64_t>(y % h) * texPitch, w * fm->bpp);

		mCopyNs += nowNs() - copyFrom;
		mCopiedBytes += static_cast<uint64_t>(w) * fm->bpp * h * depth;

		if (!ok && copy)
			step = "its rows";

		if (ok && !inPool) {   // not in the pool: a runtime buffer of its own
			step = "runtime allocation";
			ok = gCompute->rtAlloc(this, bytes, out[k].handle, gpu) == kIOReturnSuccess;
		}

		if (ok && !inPool) {
			step = "copy into the runtime";
			ok = gCompute->rtCopy(this, out[k].handle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(copy), bytes, true) == kIOReturnSuccess;
		}

		out[k].cpu = inPool ? pooled : nullptr;
		if (copy && !inPool)
			IOFree(copy, bytes);

		if (!ok) {
			if (talk || mTexLines++ < 2)
				pvstat("host: render: %s texture %u (resource %u, type %u): format %u, %ux%ux%u, %llu bytes at 0x%llx of %llu: %s failed (row %u)",
				      stageName, k, id, src.type, format, w, h, depth, static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(src.va),
				      static_cast<unsigned long long>(src.length), step, y ? y - 1 : 0);
			break;
		}
		const uint32_t pitchPx = texPitch / fm->bpp;
		out[k].va = src.va;
		out[k].rowBytes = rowBytes;
		out[k].pitch = texPitch;
		out[k].rowLen = w * fm->bpp;
		out[k].height = h;
		out[k].writable = writes && (usage & 2) && src.type == 2 && depth == 1;   // MTLTextureUsageShaderWrite; not read-only (type 3), IOSurface or 3D
		if (array1d)
			linearArray1d(table + (RDNA4_MTLC_TABLE_TEXTURES + 32 * k) / 4, gpu, fm->gfx, fm->sel, w, depth);
		else if (depth > 1)
			linearVolume(table + (RDNA4_MTLC_TABLE_TEXTURES + 32 * k) / 4, gpu, fm->gfx, fm->sel, w, h, depth);
		else
			linearImage(table + (RDNA4_MTLC_TABLE_TEXTURES + 32 * k) / 4, gpu, fm->gfx, fm->sel, w, h, pitchPx);
	}
	for (unsigned k = 0; k < 16 && ok; k++) {
		const uint32_t id = bound.sampler[k];
		uint32_t rec[3] = {}, sd[9] = {};
		if (!id)
			continue;
		ok = readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(rec), sizeof(rec)) && (rec[0] & 0xff) == 7 && (rec[0] >> 8) >= 16 &&
		     readGuest(task, rec[1], reinterpret_cast<uint8_t *>(sd), 16) && sd[0] == 3;
		if (!ok) {
			if (talk || mTexLines++ < 2)
				pvstat("host: render: %s sampler %u (resource %u): record %08x %08x, descriptor %08x %08x %08x %08x: not supported yet", stageName,
				      k, id, rec[0], rec[1], sd[0], sd[1], sd[2], sd[3]);
			break;
		}

		const uint32_t d = sd[3];
		static const uint32_t clamp[8] = {2, 3, 0, 1, 6, 6, 2, 2};   // Metal: edge, mirror-clamp-to-edge, repeat, mirror, zero, border
		uint32_t *sm = table + (RDNA4_MTLC_TABLE_SAMPLERS + 16 * k) / 4;   // SQ_IMG_SAMP_WORD0-3; ponytail: no anisotropy, LOD unclamped, border transparent black
		sm[0] = clamp[(d >> 8) & 7] | clamp[(d >> 12) & 7] << 3 | clamp[(d >> 16) & 7] << 6 | (d >> 31 ? 0 : 1u << 15);
		sm[1] = 0xfffu << 13;
		sm[2] = (d & 3) << 20 | ((d >> 2) & 3) << 22 | ((d >> 24) & 3) << 26;
		sm[3] = 0;
	}
	return ok;
}

// The shadow of a texture with this key, or nullptr.
RDNA4PvHost::Shadow *RDNA4PvHost::findShadow(const ShadowKey &k)
{
	for (Shadow &s : mShadows) {
		const bool same = s.handle && s.surface == k.surface && (k.surface || s.task == k.task) && s.at == k.at && s.rowBytes == k.rowBytes &&
		                  s.width == k.width && s.height == k.height && s.depth == k.depth && s.bpp == k.bpp && s.imageBytes == k.imageBytes;

		if (same)
			return &s;
	}

	return nullptr;
}

// Whether a shadow still holds what the guest has: not written since by anyone but the card through it, the surface's lock seeds where it
// has them, and not older than kShadowAge (a write the host does not see: a guest texture the CPU wrote, a surface written without a lock).
bool RDNA4PvHost::shadowCurrent(const Shadow &s) const
{
	if (mHeld.active && mHeld.shadow == &s)
		return true;

	const bool locked = s.surface && (gSurfaceFns.lockSeed(s.surface) != s.lockSeed || gSurfaceFns.unlockSeed(s.surface) != s.unlockSeed);
	const int stale = !s.valid ? 0 : nowNs() - s.when >= (s.readOnly ? kShadowAgeReadOnly : kShadowAge) ? 1 : locked ? 2 : -1;

	if (stale >= 0)
		const_cast<uint32_t &>(mStale[stale])++;

	return stale < 0;
}

// A shadow for this key in place of the least recently used one, its buffer `pitch` bytes a row; nothing in it yet (valid false).
RDNA4PvHost::Shadow *RDNA4PvHost::makeShadow(const ShadowKey &k)
{
	// as the card reads them: rows 128 bytes apart; a 3D image's or an array's slices `height` rows of the row's bytes aligned to 256
	// (rdna4-mtltex3d, 2026-11-04)
	const uint32_t pitch = (k.width * k.bpp + 127) & ~127u;
	const uint64_t slice = k.depth > 1 ? static_cast<uint64_t>((k.width * k.bpp + 255) & ~255u) * k.height : static_cast<uint64_t>(pitch) * k.height;
	const uint64_t bytes = slice * k.depth;
	Shadow *s = nullptr;

	if (bytes > kShadowMax)
		return nullptr;

	for (Shadow &e : mShadows) {
		if (shadowBusy(e))
			continue;

		s = !s || e.used < s->used ? &e : s;
	}

	if (!s)
		return nullptr;

	// on the card when the card can fill it and write it back (a 2D texture, a DMA packet a row), else in host memory for the CPU
	const bool contiguous = pitch == k.rowBytes && pitch == k.width * k.bpp;   // copied in 1 MiB pieces, not a packet a row
	const bool onCard = k.depth == 1 && (k.height <= kDmaRows || contiguous) && !(gInPlace & 64);

	if (s->handle && (s->bytes < bytes || onCard != !s->cpu))
		freeShadow(*s);

	// within the budget: the least recently used ones' memory given back
	while (mShadowBytes + (s->handle ? 0 : bytes) > kShadowBudget) {
		Shadow *old = nullptr;

		for (Shadow &e : mShadows)
			if (e.handle && &e != s && !shadowBusy(e))
				old = !old || e.used < old->used ? &e : old;

		if (!old)
			break;

		freeShadow(*old);
	}

	if (s->handle && !shadowIdle(*s))
		waitDraws();   // draws in flight may read it

	if (!s->handle) {
		uint64_t cpu = 0;
		const IOReturn r = onCard ? gCompute->rtAlloc(this, bytes, s->handle, s->gpu)
		                          : gCompute->rtAllocHost(this, kernel_task, bytes, 0, s->handle, s->gpu, cpu);

		if (r != kIOReturnSuccess) {
			s->handle = 0;

			return nullptr;
		}

		s->cpu = reinterpret_cast<uint8_t *>(cpu);
		s->bytes = bytes;
		mShadowBytes += bytes;
	}

	if (k.surface)
		k.surface->retain();

	if (s->surface)
		s->surface->release();

	s->surface = k.surface;
	s->task = k.task;
	s->at = k.at;
	s->rowBytes = k.rowBytes;
	s->width = k.width;
	s->height = k.height;
	s->depth = k.depth;
	s->bpp = k.bpp;
	s->imageBytes = k.imageBytes;
	s->pitch = pitch;
	s->slice = slice;
	s->valid = false;
	s->used = ++mShadowClock;
	mShadowsMade++;

	return s;
}

// The guest's rows copied into the shadow (after the draws in flight: one may write them); its seeds and time noted.
bool RDNA4PvHost::fillShadow(Shadow &s, IOMemoryDescriptor *memory, bool written)
{
	const uint64_t from = nowNs();
	const uint32_t rowLen = s.width * s.bpp;
	bool ok = true;

	cardToMemory(s.surface);

	if (!s.cpu)
		return fillShadowOnCard(s, memory);

	if (written || !shadowIdle(s))
		waitDraws();   // a draw in flight writes the texture, or reads the shadow

	if (s.surface) {
		s.lockSeed = gSurfaceFns.lockSeed(s.surface);
		s.unlockSeed = gSurfaceFns.unlockSeed(s.surface);
	}

	for (uint32_t y = 0; y < s.height * s.depth && ok; y++) {
		uint8_t *row = s.cpu + (y / s.height) * s.slice + static_cast<uint64_t>(y % s.height) * s.pitch;
		const uint64_t at = s.at + (y / s.height) * s.imageBytes + static_cast<uint64_t>(y % s.height) * s.rowBytes;

		ok = memory ? memory->readBytes(at, row, rowLen) == rowLen : readGuest(s.task, at, row, rowLen);
	}

	__sync_synchronize();   // seen by the card (snooped) before the submit
	mCopyNs += nowNs() - from;
	mCopiedBytes += static_cast<uint64_t>(rowLen) * s.height * s.depth;

	CopiedTop *slot = nullptr;
	for (CopiedTop &t : mCopiedTop) {   // the textures copied most: this one's entry, else the one copied least
		if (t.at == s.at && t.task == s.task) {
			slot = &t;
			break;
		}
		slot = !slot || t.bytes < slot->bytes ? &t : slot;
	}
	if (slot->at != s.at || slot->task != s.task)
		*slot = {s.at, 0, s.task, s.width, s.height, s.rowBytes, 0, s.surface != nullptr};
	slot->bytes += static_cast<uint64_t>(rowLen) * s.height * s.depth;
	slot->times++;

	s.valid = ok;
	s.when = nowNs();

	return ok;
}

// A shadow on the card filled by the card: the guest's rows copied into it in the graphics ring, after the draws before (which may write
// the texture, or read the shadow), before the draws after; the CPU neither copies nor waits.
bool RDNA4PvHost::fillShadowOnCard(Shadow &s, IOMemoryDescriptor *memory)
{
	const uint64_t span = static_cast<uint64_t>(s.rowBytes) * (s.height - 1) + s.width * s.bpp;
	uint64_t handle = 0, src = 0;
	bool ok = memory ? memory->prepare() == kIOReturnSuccess : true;

	if (ok) {   // a surface's memory mapped from 256 bytes below (mapMemory's alignment)
		ok = (memory ? mapMemory(memory, s.at & ~0xffull, span + (s.at & 0xff), handle, src) : mapGuest(s.task, s.at, span, handle, src)) &&
		     rowsOnCard(src + (memory ? s.at & 0xff : 0), s.rowBytes, s.gpu, s.pitch, s.width * s.bpp, s.height);

		if (memory)
			memory->complete();
	}

	if (s.surface) {
		s.lockSeed = gSurfaceFns.lockSeed(s.surface);
		s.unlockSeed = gSurfaceFns.unlockSeed(s.surface);
	}

	mCopiedBytes += static_cast<uint64_t>(s.width) * s.bpp * s.height;
	mCardFills++;
	s.valid = ok;
	s.when = nowNs();
	s.serial = mDrawSerial;   // the fill in flight writes it

	return ok;
}

// A texture the card cannot read in place, read from its shadow: the one kept while it holds what the guest has, else filled anew.
bool RDNA4PvHost::shadowTexture(uint32_t task, uint32_t id, const TextureSource &src, uint32_t bpp, uint64_t &gpu, uint32_t &pitch)
{
	const Surface &sf = src.surface;
	const uint32_t depth = src.depth > 1 ? src.depth : 1;
	const ShadowKey key = {sf.surface, task, sf.memory ? sf.base : src.va, src.rowBytes, src.width, src.height, depth, bpp,
	                       depth > 1 ? src.imageBytes : static_cast<uint64_t>(src.rowBytes) * src.height};
	Shadow *s = findShadow(key);

	if (s && shadowCurrent(*s)) {
		mCopiesReused++;
	} else {
		s = s ? s : makeShadow(key);

		if (!s || !fillShadow(*s, sf.memory, drawing(task, id)))
			return false;
	}

	s->used = ++mShadowClock;
	s->serial = nextSerial();
	s->readOnly = src.type == 3;

	if (mBuilding && mUsedShadowCount < kUsedMax)
		mUsedShadows[mUsedShadowCount++] = s;
	gpu = s->gpu;
	pitch = s->pitch;

	return true;
}

// Shadows of what the guest's memory now holds differently: of this surface or task's texture at `at` (any when `all`).
void RDNA4PvHost::dropShadows(const OSObject *surface, uint32_t task, uint64_t at, bool all)
{
	for (Shadow &s : mShadows) {
		if (mHeld.active && mHeld.shadow == &s)
			continue;

		if (all || (s.surface == surface && (surface || s.task == task) && s.at == at))
			s.valid = false;
	}
}

// The shadows on the card of a texture just cleared on the card cleared with it (they hold what the guest has then without being filled
// anew; a view of other than 4 bytes a pixel only when cleared to 0); the others dropped.
void RDNA4PvHost::clearShadows(const OSObject *surface, uint32_t task, uint64_t at, uint32_t tex, uint32_t pattern)
{
	for (Shadow &s : mShadows) {
		if (s.surface != surface || (!surface && s.task != task) || s.at != at || (mHeld.active && mHeld.shadow == &s))
			continue;

		s.valid = s.handle && !s.cpu && (!pattern || s.bpp == 4) && clearOnCard(task, tex, s.gpu, s.bytes, pattern);
		s.when = nowNs();
	}
}

// Guest textures' shadows within [va, va + bytes) of a task's memory, written by a blit or a dispatch.
void RDNA4PvHost::dropShadowRange(uint32_t task, uint64_t va, uint64_t bytes)
{
	for (Shadow &s : mShadows) {
		const uint64_t end = s.at + s.imageBytes * s.depth;

		if (!s.surface && s.task == task && s.at < va + bytes && va < end && !(mHeld.active && mHeld.shadow == &s))
			s.valid = false;
	}
}

// Whether no draw in flight reads the shadow: none since it was last read, or more than kInFlight since (all of those were waited for
// on the way: never more than kInFlight are in flight).
bool RDNA4PvHost::shadowIdle(const Shadow &s) const
{
	return (!mPending && !mBatchDraws) || (s.serial <= mDrawSerial && mDrawSerial - s.serial >= kInFlight);
}

// A shadow's memory given back (after the draws that read it).
void RDNA4PvHost::freeShadow(Shadow &s)
{
	if (!s.handle)
		return;

	if (!shadowIdle(s))
		waitDraws();

	gCompute->rtFree(this, s.handle);
	mShadowBytes -= s.bytes;
	mShadowsFreed++;
	s.handle = 0;
	s.cpu = nullptr;
	s.bytes = 0;
	s.valid = false;
}

// The target held for this draw (true): its shadow, filled when it does not hold what the guest has; whatever was held before written
// back first.
bool RDNA4PvHost::holdTarget(uint32_t task, uint32_t target, uint64_t va, const Surface &surface, uint32_t width, uint32_t height,
                             uint32_t bpr, uint32_t tp)
{
	HeldTarget &h = mHeld;
	const uint64_t at = surface.memory ? surface.base : va;

	const ShadowKey key = {surface.surface, task, at, bpr, width, height, 1, tp, static_cast<uint64_t>(bpr) * height};

	if (h.active && h.task == task && h.target == target && h.shadow == findShadow(key))
		return true;

	flushHeld();
	cardToMemory(surface.surface);

	Shadow *s = findShadow(key);

	if (!s || !shadowCurrent(*s)) {
		s = s ? s : makeShadow(key);

		if (!s || !fillShadow(*s, surface.memory, drawing(task, target)))
			return false;
	}

	if (surface.memory && surface.memory->prepare() != kIOReturnSuccess)
		return false;

	if (surface.memory)
		surface.memory->retain();

	s->used = ++mShadowClock;
	s->serial = ~0ull;   // read and written by draws until written back
	h = {true, task, target, s, surface.memory, height, 0};

	return true;
}

// A guest texture the colour block cannot write in place (rows not 128 bytes apart) cleared in its shadow on the card by a draw, and held: the
// pass's draws go into it after, and its rows are written back once, at the end of the command buffer. Its memory filled across the bus by
// CP DMA and its shadow with it took a 1588x1200 backdrop of WindowServer's twice the time of a frame's draws.
bool RDNA4PvHost::clearHeld(uint32_t task, uint32_t tex, uint64_t va, uint32_t width, uint32_t height, uint32_t bpr, bool half, bool bgra,
                            const uint64_t rgba[4])
{
	const uint32_t tp = half ? 8 : 4;
	const ShadowKey key = {nullptr, task, va, bpr, width, height, 1, tp, static_cast<uint64_t>(bpr) * height};

	if ((gInPlace & (16 | 256)) || !mRtOpen)
		return false;

	flushHeld();

	Shadow *s = findShadow(key);

	s = s ? s : makeShadow(key);

	if (!s || s->cpu || !clearByDraw(task, tex, s->gpu, s->pitch / tp, width, height, half, bgra, rgba))
		return false;

	s->valid = true;
	s->when = nowNs();
	s->used = ++mShadowClock;
	s->serial = ~0ull;   // read and written by draws until written back
	mHeld = {true, task, tex, s, nullptr, 0, height};
	dropShadows(nullptr, task, va);   // other views of it (the held one stays)

	return true;
}

// The held target's drawn rows written back, after the draws into it; its shadow then holds what the guest has, and nothing is held.
void RDNA4PvHost::flushHeld()
{
	HeldTarget &h = mHeld;

	if (!h.active)
		return;

	Shadow &s = *h.shadow;
	const uint32_t rowLen = s.width * s.bpp;

	h.active = false;

	if (h.top < h.bottom && !s.cpu) {   // on the card: written back by it, left in flight before the batch's stamp
		const uint64_t span = static_cast<uint64_t>(s.rowBytes) * (h.bottom - h.top - 1) + rowLen, at = s.at + static_cast<uint64_t>(h.top) * s.rowBytes;
		uint64_t handle = 0, dst = 0;

		if (!(h.memory ? mapMemory(h.memory, at & ~0xffull, span + (at & 0xff), handle, dst) : mapGuest(s.task, at, span, handle, dst)) ||
		    !rowsOnCard(s.gpu + static_cast<uint64_t>(h.top) * s.pitch, s.pitch, dst + (h.memory ? at & 0xff : 0), s.rowBytes, rowLen,
		                h.bottom - h.top))
			pvstat("host: render: a held target of %ux%u not written back", s.width, s.height);

		mCardFlushes++;
		h.top = h.bottom;
	}

	if (h.top < h.bottom)
		waitDraws();

	for (uint32_t y = h.top; y < h.bottom; y++) {
		const uint8_t *row = s.cpu + static_cast<uint64_t>(y) * s.pitch;
		const uint64_t at = s.at + static_cast<uint64_t>(y) * s.rowBytes;

		if (h.memory)
			h.memory->writeBytes(at, row, rowLen);
		else
			copyGuest(s.task, at, const_cast<uint8_t *>(row), rowLen, true);
	}

	if (h.memory) {
		h.memory->complete();
		h.memory->release();
	}

	h.memory = nullptr;
	s.valid = true;
	s.when = nowNs();
	s.serial = mDrawSerial;

	if (s.surface) {   // as they are now: written back by the host, not by a lock
		s.lockSeed = gSurfaceFns.lockSeed(s.surface);
		s.unlockSeed = gSurfaceFns.unlockSeed(s.surface);
	}
}

// `bytes` of a task's GPU address space from `va` at a GPU VA of the host's runtime client, in place (the guest's pages are the card's to read:
// no IOMMU on the rig, bus = physical). The GPU address is `va`'s within its page.
bool RDNA4PvHost::mapGuest(uint32_t task, uint64_t va, uint64_t bytes, uint64_t &handle, uint64_t &gpu) {
	const uint64_t first = va & ~0xfffull, count = (va + bytes + 0xfff - first) >> 12;

	if (const Mapping *m = keyedMapping(nullptr, task, first, count)) {
		handle = 0;
		gpu = m->gpu + (va & 0xfff);

		return true;
	}

	auto *pages = count && count <= (256u << 8) ? static_cast<uint64_t *>(IOMalloc(count * 8)) : nullptr;
	bool ok = pages != nullptr;
	Mapping *entry = nullptr;

	ok = ok && guestPages(task, first, count, pages) && mapPages(pages, count, handle, gpu, &entry);
	if (ok)
		keyMapping(entry, nullptr, task, first);
	if (pages)
		IOFree(pages, count * 8);
	gpu += ok ? va & 0xfff : 0;

	return ok;
}

// The same for `bytes` of a prepared memory descriptor (an IOSurface's) from `offset`, which must be 256-byte aligned (an image's base).
bool RDNA4PvHost::mapMemory(IOMemoryDescriptor *memory, uint64_t offset, uint64_t bytes, uint64_t &handle, uint64_t &gpu) {
	const uint64_t first = offset & ~0xfffull, count = (offset + bytes + 0xfff - first) >> 12;

	if (const Mapping *m = !(offset & 0xff) ? keyedMapping(memory, 0, first, count) : nullptr) {
		handle = 0;
		gpu = m->gpu + (offset & 0xfff);

		return true;
	}

	Mapping *entry = nullptr;
	auto *pages = !(offset & 0xff) && count && count <= (256u << 8) ? static_cast<uint64_t *>(IOMalloc(count * 8)) : nullptr;
	bool ok = pages != nullptr;

	// A segment at a time: a call a page was 3600 calls for a 2560x1440 target, each draw that wrote or read it.
	for (uint64_t i = 0; i < count && ok;) {
		IOByteCount length = 0;
		const addr64_t phys = memory->getPhysicalSegment(first + (i << 12), &length, kIOMemoryMapperNone);

		ok = phys && !(phys & 0xfff) && length;

		// physical memory breaks only at page boundaries: a segment ending inside a page ends where the descriptor does
		for (uint64_t p = 0; ok && p < (length + 0xfff) >> 12 && i < count; p++, i++)
			pages[i] = phys + (p << 12);
	}

	ok = ok && mapPages(pages, count, handle, gpu, &entry);
	if (ok)
		keyMapping(entry, memory, 0, first);
	if (pages)
		IOFree(pages, count * 8);
	gpu += ok ? offset & 0xfff : 0;

	return ok;
}

// `count` pages at a GPU VA, from the cache of mappings when the same pages are mapped already (`handle` then 0: the cache keeps the mapping),
// else mapped anew in place of the least recently used one (`handle` 0 too: the cache owns every mapping it hands out).
bool RDNA4PvHost::mapPages(const uint64_t *pages, uint64_t count, uint64_t &handle, uint64_t &gpu, Mapping **entry) {
	Mapping *lru = &mMap[0];

	handle = 0;
	gpu = 0;
	mMapClock++;
	for (Mapping &m : mMap) {
		if (m.count == count && !memcmp(m.pages, pages, count * 8)) {
			m.used = mMapClock;
			m.submit = nextSubmit();
			noteUsed(&m);
			gpu = m.gpu;
			if (entry)
				*entry = &m;
			return true;
		}
		lru = m.used < lru->used ? &m : lru;
	}

	const uint64_t from = nowNs();
	auto *copy = static_cast<uint64_t *>(IOMalloc(count * 8));
	uint64_t h = 0;
	bool ok = copy && gCompute->rtMapPages(this, pages, count, h, gpu) == kIOReturnSuccess;

	// The cache holds the client's GPU VA (1 GiB) and host quota: make room by dropping the least recently used mappings.
	for (uint32_t tries = 0; copy && !ok && tries < kMappings; tries++) {
		Mapping *oldest = nullptr;

		for (Mapping &m : mMap)
			oldest = m.handle && m.used <= mMapFloor && (!oldest || m.used < oldest->used) ? &m : oldest;
		if (!oldest)
			break;
		waitSubmit(oldest->submit);   // a draw in flight may use it
		dropMapping(*oldest);
		ok = gCompute->rtMapPages(this, pages, count, h, gpu) == kIOReturnSuccess;
	}

	// Then the shadows least recently used, which share the VA: a mapping a draw needs now counts more than a copy kept for later (Mission
	// Control's desktop picture was not drawn: no VA left to sample it in place).
	for (uint32_t tries = 0; copy && !ok && tries < kShadows; tries++) {
		Shadow *oldest = nullptr;

		for (Shadow &e : mShadows)
			oldest = e.handle && !shadowBusy(e) && (!oldest || e.used < oldest->used) ? &e : oldest;
		if (!oldest)
			break;
		freeShadow(*oldest);
		ok = gCompute->rtMapPages(this, pages, count, h, gpu) == kIOReturnSuccess;
	}

	mMapNs += nowNs() - from;
	mMapMisses++;
	mMapPages += count;
	if (!ok) {
		if (copy)
			IOFree(copy, count * 8);
		return false;
	}

	for (Mapping &m : mMap)   // the slot to fill: an empty one (the loop above may have emptied some), else the least recently used
		lru = !m.handle ? &m : lru->handle && m.used < lru->used ? &m : lru;
	if (lru->handle && lru->used > mMapFloor) {   // all in use by this draw: no room
		gCompute->rtFree(this, h);
		IOFree(copy, count * 8);
		gpu = 0;

		return false;
	}
	if (lru->handle)
		waitSubmit(lru->submit);   // a draw in flight may use it
	dropMapping(*lru);
	memcpy(copy, pages, count * 8);
	*lru = {copy, count, h, gpu, mMapClock, nextSubmit()};
	noteUsed(lru);
	if (entry)
		*entry = lru;

	return true;
}

void RDNA4PvHost::dropMapping(Mapping &m) {
	if (m.handle)
		gCompute->rtFree(this, m.handle);
	if (m.pages)
		IOFree(m.pages, m.count * 8);
	if (m.memory)
		m.memory->release();
	m = {};
}

// The cached mapping with this key and current generation, or nullptr. `memory` keys an IOSurface's pages, else `task` with its VA.
RDNA4PvHost::Mapping *RDNA4PvHost::keyedMapping(const void *memory, uint32_t task, uint64_t first, uint64_t count)
{
	const uint64_t gen = memory ? 0 : mPtGen[task & 63];

	for (Mapping &m : mMap) {
		if (!m.keyed || m.memory != memory || m.first != first || m.count != count)
			continue;

		if (memory || (m.task == task && m.gen == gen)) {
			m.used = ++mMapClock;
			m.submit = nextSubmit();
			noteUsed(&m);

			return &m;
		}
	}

	return nullptr;
}

void RDNA4PvHost::keyMapping(Mapping *m, IOMemoryDescriptor *memory, uint32_t task, uint64_t first)
{
	if (!m)
		return;

	if (memory)
		memory->retain();

	if (m->memory)
		m->memory->release();

	m->memory = memory;
	m->task = task;
	m->first = first;
	m->gen = memory ? 0 : mPtGen[task & 63];
	m->keyed = true;
}

// The textures stageTextures put in the runtime freed; with copyBack, the writable ones (MTLTextureUsageShaderWrite) first copied back into the
// guest's texture, row by row. False if a copy back failed.
bool RDNA4PvHost::releaseTextures(uint32_t task, StagedTexture (&staged)[16], bool copyBack) {
	bool all = true;
	for (StagedTexture &st : staged) {
		if (!st.handle && !st.cpu)
			continue;
		if (copyBack && st.writable) {
			const uint64_t bytes = static_cast<uint64_t>(st.pitch) * st.height;
			auto *copy = st.cpu ? st.cpu : static_cast<uint8_t *>(IOMalloc(bytes));
			bool ok = copy != nullptr;

			if (ok && !st.cpu)
				ok = gCompute->rtCopy(this, st.handle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(copy), bytes, false) == kIOReturnSuccess;

			for (uint32_t y = 0; y < st.height && ok; y++)
				ok = copyGuest(task, st.va + static_cast<uint64_t>(y) * st.rowBytes, copy + static_cast<uint64_t>(y) * st.pitch, st.rowLen, true);

			dropShadowRange(task, st.va, static_cast<uint64_t>(st.rowBytes) * st.height);
			if (copy && !st.cpu)
				IOFree(copy, bytes);

			all &= ok;
		}

		if (st.handle)
			gCompute->rtFree(this, st.handle);

		st = {};
	}
	return all;
}

namespace {

// A vertex shader's ring descriptor literals (nggcol.s) with the attribute ring at `rings`, `outs` values a vertex.
uint32_t ringLiteral(uint32_t dw, uint64_t rings, uint32_t outs)
{
	if (dw == 0xc01a0001u)
		return static_cast<uint32_t>(rings);

	if (dw == 0xc01a0002u)
		return static_cast<uint32_t>((rings >> 32) & 0xffff) | 0xc0000000u | 16u * outs << 16;   // swizzle 3, stride 16 bytes per value (RADV)

	if (dw == 0xc01a0003u)
		return static_cast<uint32_t>(Gfx12DrawCol::kColAttrRingBytes);

	return dw;
}

// The misc page's qword for stage `st`'s buffer `k`: the vertex stage's table at +0x400, the fragment stage's at +0x800 (include/rdna4mtlc.h).
uint32_t bufferSlot(unsigned st, unsigned k)
{
	const uint32_t at = 0x400 + 0x400 * st + (k < 8 ? 8 * k : RDNA4_MTLC_TABLE_MORE_BUFFERS + 8 * (k - 8));

	return at / 8;
}

} // namespace

bool RDNA4PvHost::poolTake(uint64_t bytes, uint64_t &gpu, uint8_t *&cpu)
{
	static constexpr uint64_t kChunk = kPoolBytes / kPoolChunks;
	uint64_t at = (mPoolUsed + 0xff) & ~0xffull, user = 0;

	if (!mGfxPool.handle && gCompute->rtAllocHost(this, kernel_task, kPoolBytes, 0, mGfxPool.handle, mGfxPool.gpu, user) == kIOReturnSuccess)
		mGfxPool.cpu = reinterpret_cast<uint8_t *>(user);

	if (!mGfxPool.cpu || !bytes || bytes > kChunk)
		return false;

	if (at >= kPoolBytes || at / kChunk != (at + bytes - 1) / kChunk) {   // into the next piece, from its start
		const uint32_t next = at >= kPoolBytes ? 0 : static_cast<uint32_t>((at / kChunk + 1) % kPoolChunks);

		at = next * kChunk;
		waitSubmit(mPoolChunkSubmit[next]);   // a draw in flight may read it
	}

	mPoolChunkSubmit[at / kChunk] = nextSubmit();

	if (mBuilding)
		mUsedChunks |= 1u << (at / kChunk);

	mPoolUsed = at + bytes;
	gpu = mGfxPool.gpu + at;
	cpu = mGfxPool.cpu + at;

	return true;
}

// The spare buffer the stages' unbound buffer slots point at, zeroed once made: a shader's load through an unbound slot would otherwise
// go to address 0, a page fault that hangs the graphics ring. Stores into it are lost.
bool RDNA4PvHost::makeSpare(GfxBuffers &g)
{
	static constexpr uint64_t kSpareBytes = 64 * 1024;

	if (g.handle)
		return true;

	auto *zeros = static_cast<uint8_t *>(IOMalloc(kSpareBytes));

	if (zeros)
		memset(zeros, 0, kSpareBytes);

	bool ok = zeros && gCompute->rtAlloc(this, kSpareBytes, g.handle, g.gpu) == kIOReturnSuccess &&
	          gCompute->rtCopy(this, g.handle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(zeros), kSpareBytes, true) == kIOReturnSuccess;

	if (zeros)
		IOFree(zeros, kSpareBytes);

	if (!ok && g.handle) {
		gCompute->rtFree(this, g.handle);
		g = {};
	}

	return ok;
}

// A draw whose IB did not finish: its shaders, its addresses and the packets it put in place of the template's draw.
void RDNA4PvHost::logHang(const char (*name)[64], const Draw &draw, const Bindings *bound, uint32_t count, uint64_t indexGpu,
                          const uint64_t table[2], const uint32_t *seq, uint32_t m)
{
	pvstat("host: render: hung: \"%s\" and \"%s\", type %u, %u %s from %u, %u instance(s) from %u; index buffer 0x%llx (+%u), tables 0x%llx "
	      "0x%llx, code 0x%llx, spare 0x%llx", name[1], name[2], draw.type, count, draw.indexBytes ? "indices" : "vertices", draw.start,
	      draw.instances, draw.baseInstance, indexGpu, draw.indexOffset, table[0], table[1], mGfxCode.gpu, mGfxSpare.gpu);

	for (unsigned st = 0; st < 2; st++) {
		const uint32_t *b = bound[st].buffer, *t = bound[st].texture, *sm = bound[st].sampler;

		pvstat("host: render: hung: %s buffers %u %u %u %u %u %u %u %u, textures %u %u %u %u %u %u, samplers %u %u %u %u", st ? "fragment" : "vertex",
		      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], t[0], t[1], t[2], t[3], t[4], t[5], sm[0], sm[1], sm[2], sm[3]);
	}

	for (uint32_t i = 0; i < m; i += 8)
		pvlog("host: render: hung: packets +%u: %08x %08x %08x %08x %08x %08x %08x %08x", i, seq[i], i + 1 < m ? seq[i + 1] : 0,
		      i + 2 < m ? seq[i + 2] : 0, i + 3 < m ? seq[i + 3] : 0, i + 4 < m ? seq[i + 4] : 0, i + 5 < m ? seq[i + 5] : 0,
		      i + 6 < m ? seq[i + 6] : 0, i + 7 < m ? seq[i + 7] : 0);
}

// The shaders' code as the draw stream finds it: VS at +0, PS at +RDNA4_TRI_PS_OFFSET, each slot padded with s_code_end for the SQ's
// prefetch; the vertex shader's ring literals patched when it has values for the fragment shader.
void RDNA4PvHost::placeShaders(uint32_t *code, const Jit *const shader[3], uint64_t rings, uint32_t outs)
{
	for (uint32_t i = 0; i < RDNA4_TRI_CODE_BYTES / 4; i++)
		code[i] = Isa::kSCodeEnd;

	for (unsigned k = 1; k <= 2; k++)
		memcpy(reinterpret_cast<uint8_t *>(code) + (k == 1 ? 0 : RDNA4_TRI_PS_OFFSET), shader[k]->gfx + 32,
		       reinterpret_cast<const uint32_t *>(shader[k]->gfx)[2]);

	for (uint32_t i = 0; outs && i < RDNA4_TRI_PS_OFFSET / 4; i++)
		code[i] = ringLiteral(code[i], rings, outs);
}

// M4: Render.drawPrimitives on the card: the pipeline's vertex and fragment functions translated by the helper (air2amdgpu.cpp
// translateShader), drawn with the stream of src/gfx12_draw.h (the triangle every gfx test of the kext draws, proven on the card) with the
// translated shaders in place of its own and the target, viewport, blending and draw patched; the colour attachment (a BGRA8 or RGBA8
// texture or a BGRA8 IOSurface) staged through a linear RGBA8 target, BGRA8 swapped on the way in and out. Triangle lists and strips, a first vertex, instances
// (NUM_INSTANCES), 16-bit indexed draws. With values from the vertex to the fragment shader the stream is the colour triangle's
// (src/gfx12_draw_col.h: the attribute ring), whose descriptor literals in the vertex shader the host patches as for nggcol.s. Each stage's
// bound buffers are staged into the runtime (read-only: nothing is copied back) and their addresses put in a table per stage in the misc
// page, which the shaders find in their two user SGPRs (SPI_SHADER_USER_DATA_GS_0/1, _PS_0/1, set by a packet added before the draw).
// The slot of a shader pair: the one holding it, else the least recently used one, `fresh` (to be filled; it holds nothing until then).
uint32_t RDNA4PvHost::codeSlot(const uint8_t *vs, const uint8_t *ps, bool &fresh)
{
	uint32_t lru = 0;

	mCodeClock++;

	for (uint32_t i = 0; i < kCodeSlots; i++) {
		CodeSlot &c = mCode[i];

		if (c.valid && !memcmp(c.hash[0], vs, 32) && !memcmp(c.hash[1], ps, 32)) {
			c.used = mCodeClock;
			fresh = false;

			return i;
		}

		if (!c.valid ? mCode[lru].valid : c.used < mCode[lru].used)
			lru = i;
	}

	CodeSlot &c = mCode[lru];

	memcpy(c.hash[0], vs, 32);
	memcpy(c.hash[1], ps, 32);
	c.used = mCodeClock;
	c.valid = false;
	fresh = true;

	return lru;
}

// Whether a draw's staging leaves the CPU work after the card: a texture to copy back, or memory of its own to free.
static bool ownsMemory(const RDNA4PvHost::StagedTexture (&staged)[2][16], const uint64_t (&handle)[2][RDNA4_MTLC_MAX_BUFFERS])
{
	for (unsigned st = 0; st < 2; st++) {
		for (const auto &t : staged[st])
			if (t.handle || t.writable)
				return true;

		for (uint64_t h : handle[st])
			if (h)
				return true;
	}

	return false;
}

const RDNA4PvHost::PipelineInfo &RDNA4PvHost::pipelineInfo(uint32_t task, uint32_t pipeline)
{
	uint32_t rec[3] = {}, fnRec[3][3] = {};
	uint64_t pd[8] = {};
	uint32_t pbytes = 0, fn[3] = {};

	readGuest(task, 0x1000 + 12ull * pipeline, reinterpret_cast<uint8_t *>(rec), sizeof(rec));

	// The pipeline: {16-byte header, u8 n, n x {u8 key, u8 size, value}}: key 1 the vertex function, key 2 the fragment function (measured).
	if (resourceType(task, pipeline, pd, &pbytes) == 7 && pbytes >= 17) {
		const auto *pb = reinterpret_cast<const uint8_t *>(pd);

		for (uint32_t k = 0, at = 17; k < pb[16] && at + 2 <= pbytes; k++, at += 2 + pb[at + 1])
			if (pb[at] >= 1 && pb[at] <= 2 && pb[at + 1] == 4 && at + 6 <= pbytes)
				memcpy(&fn[pb[at]], pb + at + 2, 4);
	}

	for (unsigned k = 1; k <= 2; k++)
		if (fn[k] && fn[k] <= 0x10000)
			readGuest(task, 0x1000 + 12ull * fn[k], reinterpret_cast<uint8_t *>(fnRec[k]), sizeof(fnRec[k]));

	PipelineInfo *lru = &mPipelines[0];

	for (PipelineInfo &e : mPipelines) {
		const bool same = e.valid && e.task == task && e.pipeline == pipeline && !memcmp(e.rec, rec, sizeof(rec)) && !memcmp(e.pd, pd, sizeof(pd)) &&
		                  !memcmp(e.fnRec, fnRec, sizeof(fnRec));

		if (same) {
			e.used = ++mPipelineClock;

			return e;
		}

		lru = e.used < lru->used ? &e : lru;
	}

	PipelineInfo &e = *lru;

	e = {};
	e.task = task;
	e.pipeline = pipeline;
	memcpy(e.rec, rec, sizeof(rec));
	memcpy(e.pd, pd, sizeof(pd));
	memcpy(e.fnRec, fnRec, sizeof(fnRec));
	memcpy(e.fn, fn, sizeof(fn));

	// Colour attachment 0's blend state, from the pipeline's descriptor (readBlendState, pvdraw.hpp), and its vertex descriptor as the
	// vertex stage's attribute records (pvdraw.hpp, include/rdna4mtlc.h).
	static uint8_t pb[1024];   // ponytail: one host, draws run one at a time on its work loop
	const uint32_t len = rec[0] >> 8;
	const uint32_t got = len >= 17 && len <= sizeof(pb) && readGuest(task, rec[1], pb, len) ? len : 0;

	readBlendState(pb, got, e.blend);
	e.attributesKnown = readVertexDescriptor(pb, got, e.attributes, e.skipped);

	for (unsigned k = 1; k <= 2; k++)
		e.named[k] = fn[k] && functionName(task, fn[k], e.name[k], sizeof(e.name[k]), e.hash[k], &e.lib[k]);

	e.valid = true;
	e.used = ++mPipelineClock;

	return e;
}

void RDNA4PvHost::runDraw(uint32_t task, uint32_t target, uint32_t pipeline, const Draw &draw, const Bindings *bound) {
	// A draw takes the card 6 ms at most; any of WindowServer's draws hangs it now and then (about every 8 s, any shader, also with clock
	// gating off; cause open), and each held all drawing until the wait gave up and the ring was reset: 2 s, then 250 ms were visible stalls.
	static constexpr uint32_t kDrawWait = 50;   // ms
	const uint64_t drawFrom = nowNs();

	mMapFloor = mMapClock;
	mBuilding = true;
	mUsedMapCount = mUsedShadowCount = 0;
	mUsedChunks = 0;
	mBuildSlot = ~0u;

	const uint32_t count = draw.count;
	const bool talk = (mDrawLines[task & 63] < 40 && ++mDrawLines[task & 63]) || gTalk;
	const PipelineInfo &pi = pipelineInfo(task, pipeline);
	const uint32_t (&fn)[3] = pi.fn, (&blend)[10] = pi.blend;
	uint32_t attributes[RDNA4_MTLC_MAX_ATTRIBUTES][4];
	const uint32_t (&skippedAttributes)[RDNA4_MTLC_MAX_ATTRIBUTES] = pi.skipped;
	const bool attributesKnown = pi.attributesKnown;

	memcpy(attributes, pi.attributes, sizeof(attributes));

	const char *why = !fn[1] || !fn[2] ? "the pipeline has no vertex or no fragment function"
	                : !attributesKnown ? "the pipeline's vertex descriptor is not understood"
	                : draw.type > 4 ? "an unknown primitive type"
	                : !draw.instances ? "no instances"
	                : draw.indexBytes && draw.start ? "a base vertex in an indexed draw (so far)"
	                : !count || count > 0xffff ? "no vertices" : nullptr;
	const Jit *shader[3] = {};
	char name[3][64] = {};
	for (unsigned k = 1; k <= 2 && !why; k++) {
		memcpy(name[k], pi.name[k], sizeof(name[k]));
		shader[k] = pi.named[k] ? translated(task, pi.lib[k], name[k], pi.hash[k]) : nullptr;
		// The RGFX header: {"RGFX", u32 stage, u32 code bytes, ...}; the code must fit its slot with room for the prefetch.
		const auto *head = shader[k] && shader[k]->gfx && shader[k]->gfxLen >= 32 ? reinterpret_cast<const uint32_t *>(shader[k]->gfx) : nullptr;
		const uint32_t room = (k == 1 ? RDNA4_TRI_PS_OFFSET : RDNA4_TRI_CODE_BYTES - RDNA4_TRI_PS_OFFSET) - 4 * RDNA4_TRI_SHADER_PAD;

		if (!head || head[1] != k || head[2] > room || head[2] + 32 > shader[k]->gfxLen) {
			why = k == 1 ? "no translated vertex shader" : "no translated fragment shader";

			if (mShaderLines++ < 8)
				pvstat("host: render: pipeline %u's %s function \"%s\" (named %u, hash %02x%02x%02x%02x): %s", pipeline, k == 1 ? "vertex" : "fragment",
				       name[k], pi.named[k], pi.hash[k][0], pi.hash[k][1], pi.hash[k][2], pi.hash[k][3],
				       !shader[k] ? "no translation" : !head ? "a kernel, not a shader" : "its RGFX header does not fit");
		}
	}
	// The values between the stages: after each shader's code its table {u32 name hash, u32 flags (bit 0 flat)}, the vertex function's
	// outputs by ring slot, the fragment function's inputs by hardware input; input k reads the slot with its name (SPI_PS_INPUT_CNTL_k).
	// The point's coordinate (flags bit 1) has no name: the SPI makes it (PT_SPRITE_TEX, with SPI_INTERP_CONTROL_0's PNT_SPRITE_ENA).
	const uint32_t *vs = why ? nullptr : reinterpret_cast<const uint32_t *>(shader[1]->gfx), *ps = why ? nullptr : reinterpret_cast<const uint32_t *>(shader[2]->gfx);
	const uint32_t outs = vs ? vs[7] : 0, ins = ps ? ps[7] & 0xffff : 0;
	const bool noColour = ps && ps[7] >> 31;   // the fragment shader writes no colour (air2amdgpu.cpp: textures, say)
	uint32_t inputCntl[16] = {};
	bool sprite = false;
	if (!why && (outs > 16 || ins > 16 || 32 + vs[2] + 8 * outs > shader[1]->gfxLen || 32 + ps[2] + 8 * ins > shader[2]->gfxLen))
		why = "more than 16 values between the stages, or a shader without its table of them";
	for (uint32_t k = 0; k < ins && !why; k++) {
		const uint32_t *out = vs + 8 + vs[2] / 4, *in = ps + 8 + ps[2] / 4 + 2 * k;   // the tables after the code (whole instructions)
		uint32_t slot = 0;

		if (in[1] & 2) {
			inputCntl[k] = 1u << 17;   // PT_SPRITE_TEX
			sprite = true;
			continue;
		}

		while (slot < outs && out[2 * slot] != in[0])
			slot++;
		if (slot == outs)
			why = "a fragment input the vertex function does not write";
		inputCntl[k] = slot | (in[1] & 1) << 10;   // OFFSET, FLAT_SHADE
	}
	const uint64_t targetAt = nowNs();

	mPhaseNs[4] += targetAt - drawFrom;
	// The colour attachment: a linear 32-bit or RGBA16Float texture, or a BGRA8 IOSurface (surfaceRange; its rows width * 4 bytes, bpr apart).
	uint64_t va = 0, len = 0;
	uint32_t bpr = 0, width = 0, height = 0;
	Surface surface;
	// A texture: BGRA8 (its rows may be padded; height from the descriptor), or of unknown format with unpadded 32-bit rows (as before).
	uint32_t targetFormat = 0, texHeight = 0;
	bool bgra = true;   // swapped into and out of the staged RGBA8 target
	// RGBA8 (70, 71) as BGRA8, but already in the staged target's order; RGBA16Float (115: CoreAnimation renders symbols into it) as
	// itself, 8 bytes a pixel.
	const auto known = [](uint32_t f) { return f == 70 || f == 71 || f == 80 || f == 81 || f == 115; };
	uint32_t storage = 0;
	const bool found = !why && textureRange(task, target, &va, &len, &bpr, &width, &texHeight, &targetFormat, &storage) && width;
	const bool fitsKnown = known(targetFormat) && texHeight && static_cast<uint64_t>(bpr) * texHeight <= len;
	const bool fitsPlain = !targetFormat && bpr == width * 4 && bpr && len % bpr == 0;   // of unknown format: unpadded 32-bit rows

	if (found && bpr >= width * (targetFormat == 115 ? 8 : 4) && (fitsKnown || fitsPlain)) {
		height = targetFormat ? texHeight : static_cast<uint32_t>(len / bpr);
		bgra = targetFormat != 70 && targetFormat != 71 && targetFormat != 115;
	}
	else if (!why && surfaceRange(task, target, surface) && surface.format == 80) {
		bpr = surface.bytesPerRow;
		width = surface.width;
		height = surface.height;
	}
	if (!why && !(width && height && width <= 16384 && height <= 16384)) {
		static uint32_t refusedTargets;

		why = "the colour attachment is neither a linear 32-bit or RGBA16Float texture nor a BGRA8 IOSurface";
		if (refusedTargets++ < 8)
			pvlog("host: render: colour attachment %u: format %u, %ux%u, %u bytes per row, %llu bytes: not supported yet", target, targetFormat,
			      width, texHeight, bpr, static_cast<unsigned long long>(len));
	}
	// The viewport: Render.setViewport's, else the whole target; whole pixels only so far (window = origin + (ndc + 1) * size / 2, y down).
	int64_t vx = 0, vy = 0, vw = width, vh = height;
	if (!why && draw.viewportSet &&
	    !(wholeDouble(draw.viewport[0], vx) && wholeDouble(draw.viewport[1], vy) && wholeDouble(draw.viewport[2], vw) && wholeDouble(draw.viewport[3], vh) &&
	      vw > 0 && vh > 0 && vw <= 32768 && vh <= 32768 && vx > -32768 && vx < 32768 && vy > -32768 && vy < 32768))
		why = "a viewport not in whole pixels, or too large (so far)";
	if (why) {
		noteNotRun(why, name[2]);
		if (talk)
			pvlog("host: render: draw of %u vertices (type %u) with pipeline %u: %s: not run", count, draw.type, pipeline, why);
		return;
	}
	// The scissor rectangle within the target (the whole target unless set; the hardware's viewport scissor is enabled in the template).
	const auto clampTo = [](uint64_t v, uint32_t limit) { return static_cast<uint32_t>(v < limit ? v : limit); };
	const uint32_t sx = draw.scissorSet ? clampTo(draw.scissor[0], width) : 0, sy = draw.scissorSet ? clampTo(draw.scissor[1], height) : 0;
	const uint32_t sx2 = draw.scissorSet ? clampTo(draw.scissor[0] + draw.scissor[2], width) : width;
	const uint32_t sy2 = draw.scissorSet ? clampTo(draw.scissor[1] + draw.scissor[3], height) : height;
	// The target in place when the colour block can write it as the guest has it (its rows a multiple of 128 bytes apart, its base 256-byte
	// aligned): no staging, no copies, and BGRA8 swapped by the colour block (COMP_SWAP) instead of the CPU. Else staged at a pitch of its own.
	const bool half = targetFormat == 115;
	const uint32_t tp = half ? 8 : 4;   // bytes a pixel
	const bool inPlace = (gInPlace & 2) && !(bpr % 128) && !((surface.memory ? surface.base : va) & 0xff);
	const uint32_t pitch = inPlace ? bpr / tp : (width * tp + 127) / 128 * 128 / tp;   // pixels; staged: a multiple of 128 bytes

	const uint64_t targetBytes = static_cast<uint64_t>(pitch) * height * tp;
	// Only the scissor rectangle's rows are staged in and out (WindowServer limits most draws to what changed): nothing outside it is drawn.
	const uint64_t regionAt = static_cast<uint64_t>(pitch) * sy * tp, regionBytes = static_cast<uint64_t>(pitch) * (sy2 - sy) * tp;

	if (sx2 <= sx || sy2 <= sy) {
		if (talk)
			pvlog("host: render: draw with pipeline %u: an empty scissor rectangle: nothing to draw", pipeline);
		return;
	}

	const uint32_t kind = surface.memory ? (cardSurface(surface.surface) ? 0 : 1) : 2 + storage;

	mTargetDraws[kind]++;
	mTargetBytes[kind] += static_cast<uint64_t>(sx2 - sx) * (sy2 - sy) * tp;

	// The buffers kept for the boot (made by the first draw), and the target for this one.
	auto make = [&](GfxBuffers &g, uint64_t bytes) { return g.handle || gCompute->rtAlloc(this, bytes, g.handle, g.gpu) == kIOReturnSuccess; };
	// The state page and the IB in host memory the CPU writes in place: an SDMA copy each was two of a draw's waits. Else as the others.
	auto makeHost = [&](GfxBuffers &g, uint64_t bytes) {
		uint64_t cpu = 0;

		if (g.handle)
			return true;

		if (gCompute->rtAllocHost(this, kernel_task, bytes, RDNA4_HOST_EXECUTABLE, g.handle, g.gpu, cpu) != kIOReturnSuccess)
			return make(g, bytes);

		g.cpu = reinterpret_cast<uint8_t *>(cpu);

		return true;
	};
	if (!mRtOpen) {
		gCompute->rtPreferCpuTables(this);
		gCompute->rtFreesWhenIdle(this);
		mRtOpen = gCompute->rtOpen(this) == kIOReturnSuccess;
	}
	GfxBuffers tgt {};
	uint8_t *pooledTarget = nullptr;

	if (inPlace)
		flushHeld();   // another target: what was held is written back for the draws that read it

	const bool held = !inPlace && mRtOpen && !(gInPlace & 16) && holdTarget(task, target, va, surface, width, height, bpr, tp);

	if (!held)   // drawn otherwise: no shadow of it holds what the guest will have
		dropShadows(surface.surface, task, surface.memory ? surface.base : va);

	// this draw's state page and IB
	const uint32_t slot = takeSlot();

	mBuildSlot = slot;
	const uint64_t slotAt = static_cast<uint64_t>(slot) * RDNA4_TRIRUN_PAGE;

	const bool staged = !inPlace && !held;
	const bool pooled = staged && mRtOpen && poolTake(targetBytes, tgt.gpu, pooledTarget);
	uint32_t *stage = nullptr;

	if (pooled)
		stage = reinterpret_cast<uint32_t *>(pooledTarget + regionAt);
	else if (staged)
		stage = static_cast<uint32_t *>(IOMalloc(regionBytes));

	if (held)
		tgt.gpu = mHeld.shadow->gpu;

	// The target for the card: the guest's memory mapped (in place), the pool, or a runtime buffer of its own.
	auto placeTarget = [&]() {
		const uint64_t bytes = static_cast<uint64_t>(bpr) * height;

		if (inPlace && surface.memory)
			return onCard(surface, bytes, tgt.gpu) || mapMemory(surface.memory, surface.base, bytes, tgt.handle, tgt.gpu);

		if (inPlace)
			return mapGuest(task, va, bytes, tgt.handle, tgt.gpu);

		return held || pooled || gCompute->rtAlloc(this, targetBytes, tgt.handle, tgt.gpu) == kIOReturnSuccess;
	};
	const uint64_t ringBytes = rdna4_tri_ring_bytes() + RDNA4_TRIRUN_RING_SLACK;
	bool ok = mRtOpen && (!staged || stage) && make(mGfxCode, RDNA4_TRI_CODE_BYTES * kCodeSlots) && make(mGfxRings, ringBytes);

	ok = ok && makeHost(mGfxMisc, RDNA4_TRIRUN_PAGE * kSlots) && makeHost(mGfxIb, RDNA4_TRIRUN_PAGE * kSlots) && makeSpare(mGfxSpare);
	ok = ok && rdna4_trirun_rings_fit(mGfxRings.gpu, ringBytes, rdna4_tri_ring_bytes()) && placeTarget();
	const bool allocated = ok;

	// The shaders: the pair's slot, uploaded when the pair is not on the card yet.
	static uint32_t code[RDNA4_TRI_CODE_BYTES / 4];   // ponytail: one host, draws run one at a time on its work loop
	const uint64_t rings = rdna4_trirun_rings_va(mGfxRings.gpu);   // attribute ring first
	bool newCode = false;
	const uint32_t pair = ok ? codeSlot(shader[1]->hash, shader[2]->hash, newCode) : 0;
	const uint64_t codeAt = static_cast<uint64_t>(pair) * RDNA4_TRI_CODE_BYTES, codeGpu = mGfxCode.gpu + codeAt;

	if (newCode)
		placeShaders(code, shader, rings, outs);

	// The stream: the triangle's, with this draw's addresses, shaders' registers, target, viewport and vertex count.
	static uint32_t ib[RDNA4_TRIRUN_PAGE / 4];
	const uint64_t miscGpu = mGfxMisc.gpu + slotAt, ibGpu = mGfxIb.gpu + slotAt;
	const rdna4_tri_va where = {codeGpu, tgt.gpu, rings, miscGpu};
	const rdna4_tricol_va whereCol = {codeGpu, tgt.gpu, rings, miscGpu};
	const uint32_t n = !ok ? 0 : outs ? rdna4_tricol_record(ib, &whereCol) : rdna4_tri_record(ib, &where);
	// Blending: the colour pass, and when alpha's equation differs and both are written, an alpha-only pass after it.
	// ponytail: a colour factor reading destination alpha sees the alpha from before the draw, not from earlier triangles of the same
	// draw; exact unless such a draw overlaps itself.
	const uint32_t mask = (blend[9] >> 3 & 1) | (blend[9] >> 1 & 2) | (blend[9] << 1 & 4) | (blend[9] << 3 & 8);   // Metal's A, B, G, R bits
	const BlendPass rgbPass(blend[3], blend[4], blend[5]), alphaPass(blend[6], blend[7], blend[8]);
	const bool split = blend[2] && rgbPass.control != alphaPass.control && (mask & 7) && (mask & 8);
	const BlendPass &first = mask & 7 ? rgbPass : alphaPass;
	// Metal's point, line, line strip, triangle and triangle strip: DI_PT_POINTLIST .. DI_PT_TRISTRIP, and the NGG stage's output points,
	// lines or triangles (the hardware passes the primitive's vertices to the shader, which exports it as given).
	static const uint32_t kPrimitive[5][2] = {{1, 0}, {2, 1}, {3, 1}, {4, 2}, {6, 2}};
	struct Patch { uint32_t opcode, reg, value; } patch[] = {
		{0x76, 0x8a, vs[3]}, {0x76, 0x8b, vs[4]},                          // SPI_SHADER_PGM_RSRC1_GS, RSRC2_GS: as LLVM compiled the vertex shader
		{0x76, 0x0a, ps[3]}, {0x76, 0x0b, ps[4]},                          // SPI_SHADER_PGM_RSRC1_PS, RSRC2_PS
		{0x69, 0x197, ps[5]}, {0x69, 0x198, ps[6]},                        // SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR
		{0x69, 0x31e, (pitch - 1) << 16 | (height - 1)},                    // CB_COLOR0_ATTRIB2: MIP0_WIDTH (the pitch), MIP0_HEIGHT
		{0x69, 0x3b0, half ? 0x0002070cu : (inPlace || held) && bgra ? 0x0002880au : 0x0002800au},   // CB_COLOR0_INFO: 8_8_8_8 unorm (COMP_SWAP ALT: BGRA8), or 16_16_16_16 float
		{0x69, 0x82, (width - 1) | (height - 1) << 16},                    // PA_SC_WINDOW_SCISSOR_BR: inclusive on gfx12
		{0x69, 0x94, sx | sy << 16},                                       // PA_SC_VPORT_SCISSOR_0_TL, _BR: Metal's scissor rectangle, BR inclusive
		{0x69, 0x95, (sx2 - 1) | (sy2 - 1) << 16},
		{0x69, 0x10f, halfAsFloat(static_cast<uint32_t>(vw), false)},     // PA_CL_VPORT_XSCALE: width / 2
		{0x69, 0x110, halfAsFloat(2 * vx + vw)},                            // XOFFSET: originX + width / 2
		{0x69, 0x111, halfAsFloat(static_cast<uint32_t>(vh), true)},      // YSCALE: -height / 2 (Metal's y points up)
		{0x69, 0x112, halfAsFloat(2 * vy + vh)},                            // YOFFSET: originY + height / 2
		{0x79, 0x242, kPrimitive[draw.type][0]},                           // VGT_PRIMITIVE_TYPE
		{0x79, 0x266, kPrimitive[draw.type][1]},                           // VGT_GS_OUT_PRIM_TYPE
		{0x76, 0x31, outs ? (outs - 1) | ins << 11 : 0x400u},              // SPI_SHADER_GS_OUT_CONFIG_PS: VS_EXPORT_COUNT, NUM_INTERP; NO_PC_EXPORT
		{0x69, 0x207, 0x00000240u},                                        // PA_SU_SC_MODE_CNTL: the template's, without PROVOKING_VTX_LAST (Metal's is the first)
		{0x69, 0x1e0, blend[2] ? first.control : 0},                       // CB_BLEND0_CONTROL
		{0x69, 0x1d8, blend[2] ? first.opt : 0x06000600u},                 // SX_MRT0_BLEND_OPT: else BLEND_DISABLED
		{0x69, 0x214, noColour ? 0 : split ? mask & 7 : mask},             // CB_TARGET_MASK
		{0x69, 0x195, noColour ? 0u : 9u},                                 // SPI_SHADER_COL_FORMAT: 32_ABGR, or none
		// DB_SHADER_CONTROL: a shader without a colour writes memory, which runs only with EXEC_ON_HIER_FAIL and EXEC_ON_NOOP, after the depth
		// test (LATE_Z; RADV's for writes_memory, gfx12.json); else the template's EARLY_Z_THEN_LATE_Z
		{0x69, 0x1b, noColour ? 0x600u : 0x10u},
	};
	// Where each patched register is in the stream: the same for every draw of a template (plain or colour), found once (streamReg walks
	// the stream's packets for each: most of the time a draw's stream took).
	static constexpr uint32_t kPatches = sizeof(patch) / sizeof(patch[0]);
	static uint32_t patchAt[2][kPatches];
	static bool patchFound[2];

	if (n && !patchFound[outs != 0]) {
		for (uint32_t i = 0; i < kPatches; i++) {
			const uint32_t *d = streamReg(ib, n, patch[i].opcode, patch[i].reg);

			patchAt[outs != 0][i] = d ? static_cast<uint32_t>(d - ib) : 0;
		}

		patchFound[outs != 0] = true;
	}

	for (uint32_t i = 0; i < kPatches; i++)
		if (n && patchAt[outs != 0][i])
			ib[patchAt[outs != 0][i]] = patch[i].value;
		else
			ok = false;
	// The stages' buffers, in place (each mapped once a draw, whole), and their tables at +0x400 (vertex) and +0x800 (fragment) of the misc page
	// (the fence at +0).
	static constexpr uint64_t kMaxBuffer = 64ull << 20;
	static uint64_t misc[RDNA4_TRIRUN_PAGE / 8];   // a page: too large for the kernel stack
	uint64_t handle[2][RDNA4_MTLC_MAX_BUFFERS] = {}, base[2][RDNA4_MTLC_MAX_BUFFERS] = {};
	memset(misc, 0, sizeof(misc));
	for (unsigned st = 0; st < 2 && ok; st++)
		for (unsigned k = 0; k < RDNA4_MTLC_MAX_BUFFERS && ok; k++) {
			const uint32_t id = bound[st].buffer[k];
			uint64_t bva = 0, blen = 0;

			if (!id) {   // read anyway by shaders whose buffers the host does not know (setVertexBytes, say)
				misc[bufferSlot(st, k)] = mGfxSpare.gpu;
				continue;
			}

			ok = bufferRange(task, id, &bva, &blen) && bound[st].offset[k] < blen && blen <= kMaxBuffer;
			for (unsigned s2 = 0; s2 <= st && ok && !base[st][k]; s2++)   // the same buffer in an earlier slot: its mapping
				for (unsigned k2 = 0; k2 < (s2 == st ? k : RDNA4_MTLC_MAX_BUFFERS) && !base[st][k]; k2++)
					base[st][k] = bound[s2].buffer[k2] == id ? base[s2][k2] : 0;
			ok = ok && (base[st][k] || mapGuest(task, bva, blen, handle[st][k], base[st][k]));
			if (talk && ok) {   // what the draw reads (the first 8 dwords of each buffer)
				uint32_t w[8] = {};
				readGuest(task, bva + bound[st].offset[k], reinterpret_cast<uint8_t *>(w), blen - bound[st].offset[k] < sizeof(w) ?
				          static_cast<uint32_t>(blen - bound[st].offset[k]) : sizeof(w));
				pvlog("host: render:   %s buffer %u: resource %u + %llu, %llu bytes: %08x %08x %08x %08x %08x %08x %08x %08x", st ? "fragment" : "vertex", k,
				      id, bound[st].offset[k], blen - bound[st].offset[k], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
			}
			misc[bufferSlot(st, k)] = ok ? base[st][k] + bound[st].offset[k] : 0;
		}
	memcpy(&misc[(0x400 + RDNA4_MTLC_TABLE_ATTRIBUTES) / 8], attributes, sizeof(attributes));
	mPhaseNs[5] += nowNs() - targetAt;
	StagedTexture stagedTextures[2][16] = {};
	const uint64_t texturesFrom = nowNs();
	for (unsigned st = 0; st < 2 && ok; st++)
		ok = stageTextures(task, bound[st], reinterpret_cast<uint32_t *>(&misc[(0x400 + 0x400 * st) / 8]), stagedTextures[st], talk, st ? "fragment" : "vertex",
		                   st && noColour);
	mPhaseNs[0] += nowNs() - texturesFrom;

	// The colour attachment bound as a texture too (CoreAnimation reads the layer it draws into): its copy holds the attachment from
	// before the draw. Copied back after the attachment, it put the old pixels over the draw's: the Dock's menus lost their text.
	for (auto &stageTextures : stagedTextures)
		for (StagedTexture &st : stageTextures)
			if (st.writable && !surface.memory && st.va >= va && st.va < va + len)
				st.writable = false;

	// Colour attachment 0 as the draw finds it, for fragment shaders that read it (Metal's [[color(0)]]; RDNA4_MTLC_TABLE_FRAMEBUFFER): a T# of
	// the staged RGBA8 target. ponytail: a pixel the same draw covers twice reads the value from before the draw, not its first write.
	linearImage(reinterpret_cast<uint32_t *>(&misc[(0x800 + RDNA4_MTLC_TABLE_FRAMEBUFFER) / 8]), tgt.gpu, half ? 57 : 42,
	            (inPlace || held) && bgra ? 6 | 5 << 3 | 4 << 6 | 7 << 9 : 4 | 5 << 3 | 6 << 6 | 7 << 9, width, height, pitch);   // 8_8_8_8 unorm: RGBA
	if (!ok && talk)
		pvlog("host: render: draw with pipeline %u: a texture or sampler could not be set up: not run", pipeline);

	for (uint32_t i = 0; talk && i < RDNA4_MTLC_MAX_ATTRIBUTES; i++) {
		if (attributes[i][2])
			pvlog("host: render:   attribute %u: offset %u, stride %u, buffer %u, kind %u x %u", i, attributes[i][0], attributes[i][1],
			      attributes[i][2] & 0xff, attributes[i][2] >> 8 & 15, attributes[i][2] >> 12 & 7);

		if (skippedAttributes[i])
			pvlog("host: render:   attribute %u: Metal vertex format %u (buffer %u) not supported: read as 0", i, skippedAttributes[i] & 0xffff,
			      skippedAttributes[i] >> 16);
	}

	// An indexed draw's indices, staged too.
	uint64_t indexHandle = 0, indexGpu = 0;
	const bool texturesOk = ok;
	if (ok && draw.indexBytes) {
		const uint64_t bytes = static_cast<uint64_t>(count) * draw.indexBytes;
		uint64_t iva = 0, ilen = 0;
		uint8_t *pooledIndices = nullptr;

		ok = bufferRange(task, draw.indexBuffer, &iva, &ilen) && draw.indexOffset <= ilen && bytes <= ilen - draw.indexOffset;

		if (ok && poolTake(bytes, indexGpu, pooledIndices)) {
			ok = readGuest(task, iva + draw.indexOffset, pooledIndices, static_cast<uint32_t>(bytes));
		} else if (ok) {   // a runtime buffer of its own
			auto *copy = static_cast<uint8_t *>(IOMalloc(bytes));

			ok = copy && readGuest(task, iva + draw.indexOffset, copy, static_cast<uint32_t>(bytes)) &&
			     gCompute->rtAlloc(this, bytes, indexHandle, indexGpu) == kIOReturnSuccess &&
			     gCompute->rtCopy(this, indexHandle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(copy), bytes, true) == kIOReturnSuccess;
			if (copy)
				IOFree(copy, bytes);
		}
	}

	if (texturesOk && !ok && talk)
		pvlog("host: render: draw with pipeline %u: %u indices of %u bytes could not be staged: not run", pipeline, count, draw.indexBytes);

	const bool indicesOk = ok;
	// The draw, in place of the template's DRAW_INDEX_AUTO (3 dwords): the stages' tables (SPI_SHADER_USER_DATA_GS_0/1, _PS_0/1); for an indexed
	// draw VGT_INDEX_TYPE (SET_UCONFIG_REG_INDEX, as RADV on gfx9+); per instance the first vertex and the instance id (USER_DATA_GS_2/3, the
	// vertex shader's s10-s11) and the draw: DRAW_INDEX_AUTO, or DRAW_INDEX_2 from the staged indices (DI_SRC_SEL_DMA).
	const uint64_t table[2] = {miscGpu + 0x400, miscGpu + 0x800};
	uint32_t seq[8 + 6 + 2 + 16 + 3 + 3 + 9 + 2 * 12], m = 0;
	auto put = [&](uint32_t v) { seq[m++] = v; };
	put(0xc0046900u);   // CB_BLEND_RED .. ALPHA: Render.setBlendColor's floats
	put(0x105);
	for (uint32_t c : draw.blendColor)
		put(c);
	if (ins) {
		put(0xc0006900u | ins << 16);   // SPI_PS_INPUT_CNTL_0 .. ins - 1
		put(0x199);
		for (uint32_t k = 0; k < ins; k++)
			put(inputCntl[k]);
	}
	put(0xc0016900u);   // SPI_INTERP_CONTROL_0: the template's, with PNT_SPRITE_ENA for the point's coordinate (S, T, 0, 1, top left 0, 0)
	put(0x191);
	put(sprite ? 0x86bu : 0x869u);
	for (unsigned st = 0; st < 2; st++) {
		put(0xc0027600u);
		put(st ? 0x0c : 0x8c);
		put(static_cast<uint32_t>(table[st]));
		put(static_cast<uint32_t>(table[st] >> 32));
	}
	if (draw.indexBytes) {
		put(0xc0017a00u);
		put(0x243 | 2u << 28);                // VGT_INDEX_TYPE, index 2
		put(draw.indexBytes == 4 ? 1 : 0);    // 32- or 16-bit
	}
	for (uint32_t pass = 0; pass < (split ? 2u : 1u); pass++) {
		if (pass) {   // the alpha pass
			const uint32_t regs[3][2] = {{0x1e0, alphaPass.control}, {0x1d8, alphaPass.opt}, {0x214, mask & 8}};
			for (const auto &r : regs) {
				put(0xc0016900u);   // SET_CONTEXT_REG
				put(r[0]);
				put(r[1]);
			}
		}
		put(0xc0027600u);
		put(0x8e);                                // SPI_SHADER_USER_DATA_GS_2, _3: the first vertex, the base instance
		put(draw.start);
		put(draw.baseInstance);
		put(0xc0002f00u);                         // NUM_INSTANCES: the shader adds InstanceID (v4) to the base
		put(draw.instances);
		if (draw.indexBytes) {
			put(0xc0042700u);                     // DRAW_INDEX_2 {max indices, address, count, initiator}
			put(count);
			put(static_cast<uint32_t>(indexGpu));
			put(static_cast<uint32_t>(indexGpu >> 32));
			put(count);
			put(0);
		} else {
			put(0xc0012d00u);                     // DRAW_INDEX_AUTO {count, initiator: DI_SRC_SEL_AUTO_INDEX}
			put(count);
			put(2);
		}
	}
	uint32_t total = 0;
	for (uint32_t i = 0; ok && i + 3 <= n; i++)
		if (ib[i] == 0xc0012d00u && n - 3 + m <= sizeof(ib) / 4) {
			memmove(&ib[i + m], &ib[i + 3], (n - i - 3) * 4);
			memcpy(&ib[i], seq, m * 4);
			total = n - 3 + m;
			break;
		}
	ok &= total != 0;
	// SPI_SHADER_PGM_RSRC2_GS, _PS: USER_SGPR [5:1] = 4 and 2 (air2amdgpu.cpp); the GS also ES_VGPR_COMP_CNT [17:16] = 1, so the
	// hardware loads InstanceID into v4 (gfx12, as RADV).
	static const uint32_t rsrc2[2][3] = {{0x8b, 4, 1u << 16}, {0x0b, 2, 0}};
	for (const auto &r : rsrc2)
		if (uint32_t *d = ok ? streamReg(ib, total, 0x76, r[0]) : nullptr)
			*d = (*d & ~0x3003eu) | r[1] << 1 | r[2];
		else
			ok = false;

	if (indicesOk && !ok && talk)
		pvlog("host: render: draw with pipeline %u: the stream does not fit or lacks a register (%u + %u dwords): not run", pipeline, n, m);

	const bool streamOk = ok;
	const char *failedStep = "its colour attachment's rows";   // read before the steps below
	const uint64_t targetFrom = nowNs();
	uint64_t afterFence = 0;
	// The attachment in (BGRA8 rows -> RGBA8 at the target's pitch), the draw, the attachment out; in place only the draw.
	auto swap = [&]() {
		for (uint64_t i = 0; bgra && staged && i < regionBytes / 4; i++)
			stage[i] = (stage[i] & 0xff00ff00u) | (stage[i] >> 16 & 0xff) | (stage[i] & 0xff) << 16;
	};
	auto row = [&](uint32_t y) { return reinterpret_cast<uint8_t *>(stage) + static_cast<uint64_t>(y - sy) * pitch * tp; };
	auto upload = [&](const GfxBuffers &g, const void *from, uint64_t bytes) {
		if (!g.cpu)
			return gCompute->rtCopy(this, g.handle, slotAt, kernel_task, reinterpret_cast<mach_vm_address_t>(from), bytes, true);

		memcpy(g.cpu + slotAt, from, bytes);
		__sync_synchronize();   // seen by the card (snooped) before the submit

		return kIOReturnSuccess;
	};
	if (ok && staged && drawing(task, target))
		waitDraws();

	if (ok && staged)
		cardToMemory(surface.surface);

	for (uint32_t y = sy; y < sy2 && ok && staged; y++)
		ok = surface.memory ? surface.memory->readBytes(surface.base + static_cast<uint64_t>(y) * bpr, row(y), width * 4) == width * 4
		                    : readGuest(task, va + static_cast<uint64_t>(y) * bpr, row(y), width * tp);
	uint64_t fence = 0, ns = 0;

	if (streamOk && !ok && talk)
		pvlog("host: render: draw with pipeline %u: the colour attachment could not be read: not run", pipeline);

	if (ok) {
		swap();
		// each step's result, so that a draw that fails says where
		const char *step = "target in";
		uint64_t t[6] = {nowNs()};
		IOReturn r = !staged || pooled ? kIOReturnSuccess
		                               : gCompute->rtCopy(this, tgt.handle, regionAt, kernel_task, reinterpret_cast<mach_vm_address_t>(stage), regionBytes, true);
		t[1] = nowNs();

		if (r == kIOReturnSuccess && newCode) {
			waitDraws();   // a draw in flight may run the pair this slot held
			step = "code in";
			mCodeIns++;

			// Only what each shader takes, with its pad of s_code_end: the rest of the slot is never run (the shaders end in s_endpgm). It
			// was the whole slot, 640 KiB through the bounce buffer, for a few KiB of code.
			for (unsigned k = 1; k <= 2 && r == kIOReturnSuccess; k++) {
				const uint32_t at = k == 1 ? 0 : RDNA4_TRI_PS_OFFSET;
				const uint32_t bytes = (reinterpret_cast<const uint32_t *>(shader[k]->gfx)[2] + 4 * RDNA4_TRI_SHADER_PAD + 255) & ~255u;

				r = gCompute->rtCopy(this, mGfxCode.handle, codeAt + at, kernel_task, reinterpret_cast<mach_vm_address_t>(code) + at, bytes, true);
			}

			mCode[pair].valid = r == kIOReturnSuccess;
		}

		t[2] = nowNs();

		if (r == kIOReturnSuccess) {
			step = "state in";
			r = upload(mGfxMisc, misc, sizeof(misc));
		}

		if (profiling() && total + 16 <= sizeof(ib) / 4) {   // timed: its start and end
			memmove(ib + 8, ib, total * 4);
			timeStamp(ib, false);
			total += 8;
			total += timeStamp(ib + total, true);
		}

		// Waited for now when the CPU has to finish the draw (a staged target or texture to copy back, a runtime buffer to free), else left
		// in flight with those before it, put together with the next ones when it can be.
		const bool sync = staged || indexHandle || ownsMemory(stagedTextures, handle);
		const bool batched = r == kIOReturnSuccess && !sync && batchDraw(ib, total, task, target);

		if (r == kIOReturnSuccess && !batched) {
			step = "IB in";
			r = upload(mGfxIb, ib, total * 4);
		}

		t[3] = nowNs();

		if (r == kIOReturnSuccess && !batched)
			roomForDraw();

		t[4] = nowNs();

		if (r == kIOReturnSuccess && !batched) {
			step = "submit";
			r = submitGfx(ibGpu, total, fence);
			mDrawSerial += r == kIOReturnSuccess;
		}

		t[5] = nowNs();

		static uint32_t slowLines;
		if (t[5] - targetFrom > 30000000 && slowLines++ < 12)
			pvstat("host: render: slow draw: rows %llu, target in %llu, code %llu, state and IB %llu, room %llu, submit %llu us (%s, staged %d, new code %d)",
			       (t[0] - targetFrom) / 1000, (t[1] - t[0]) / 1000, (t[2] - t[1]) / 1000, (t[3] - t[2]) / 1000, (t[4] - t[3]) / 1000, (t[5] - t[4]) / 1000,
			       name[2], staged, newCode);

		if (r == kIOReturnSuccess) {   // described for a hang
			snprintf(mRecent[mRecentAt], sizeof(mRecent[0]), "fence %llu: task %u, target %u (%ux%u, %s), \"%s\" / \"%s\", %u x %u, "
			         "fragment textures %u %u %u %u", static_cast<unsigned long long>(fence), task, target, width, height,
			         inPlace ? "in place" : held ? "held" : "staged", name[1], name[2], count, draw.instances, bound[1].texture[0],
			         bound[1].texture[1], bound[1].texture[2], bound[1].texture[3]);
			mRecentAt = (mRecentAt + 1) % kRecent;
		}

		if (r == kIOReturnSuccess && !sync) {
			mPhaseNs[1] += nowNs() - targetFrom;

			// in the open batch, or the submit just made: either way the next of flying()'s count; the serial is counted at the submit
			markUsed(mSubmits + 1, batched ? mDrawSerial + 1 : mDrawSerial);

			if (!batched) {
				flying(fence, name[2]);
				dirtied(task, target, fence);
			}
		}

		if (r == kIOReturnSuccess && sync) {
			mPhaseNs[1] += nowNs() - targetFrom;
			step = "fence";
			const uint64_t fenceFrom = nowNs();
			r = gCompute->rtWaitGfxFence(this, static_cast<uint32_t>(fence), kDrawWait, ns);
			afterFence = nowNs();
			mPhaseNs[2] += afterFence - fenceFrom;
			mPending = 0;   // in order: those before it are done too
			mDirtyCount = 0;
			addTimes();
		}

		if (r == kIOReturnSuccess && staged && !pooled) {
			step = "target out";
			r = gCompute->rtCopy(this, tgt.handle, regionAt, kernel_task, reinterpret_cast<mach_vm_address_t>(stage), regionBytes, false);
		}

		ok = r == kIOReturnSuccess;
		if (!ok && (talk || mGfxHangLines < 4))
			pvlog("host: render: draw with pipeline %u: %s failed (0x%x)", pipeline, step, r);

		failedStep = ok ? nullptr : step;

		if (!ok && !strcmp(step, "fence") && mGfxHangLines++ < 12) {
			const uint32_t *ft = reinterpret_cast<const uint32_t *>(&misc[(0x800 + RDNA4_MTLC_TABLE_TEXTURES) / 8]);

			logHang(name, draw, bound, count, indexGpu, table, seq, m);
			pvstat("host: render: hung: target %u (%ux%u, %s at 0x%llx, scissor %u,%u-%u,%u); fragment textures' bases 0x%08x 0x%08x 0x%08x 0x%08x",
			      target, width, height, inPlace ? "in place" : pooled ? "pooled" : "staged", tgt.gpu, sx, sy, sx2, sy2, ft[0], ft[8], ft[16], ft[24]);
		}
	} else if (talk && !allocated) {
		pvlog("host: render: draw with pipeline %u: no runtime client, buffers or target staging", pipeline);
	}
	if (ok) {
		swap();
		if (held) {
			mHeld.top = sy < mHeld.top ? sy : mHeld.top;
			mHeld.bottom = sy2 > mHeld.bottom ? sy2 : mHeld.bottom;
		}

		for (uint32_t y = sy; y < sy2 && ok && staged; y++)
			ok = surface.memory ? surface.memory->writeBytes(surface.base + static_cast<uint64_t>(y) * bpr, row(y), width * 4) == width * 4
			                    : copyGuest(task, va + static_cast<uint64_t>(y) * bpr, row(y), width * tp, true);
	}
	if (tgt.handle)
		gCompute->rtFree(this, tgt.handle);
	for (unsigned st = 0; st < 2; st++)
		for (unsigned k = 0; k < RDNA4_MTLC_MAX_BUFFERS; k++)
			if (handle[st][k])
				gCompute->rtFree(this, handle[st][k]);
	if (indexHandle)
		gCompute->rtFree(this, indexHandle);
	for (auto &stageTextures : stagedTextures)
		ok = releaseTextures(task, stageTextures, ok) && ok;   // textures the shaders may write copied back
	if (stage && !pooled)
		IOFree(stage, regionBytes);
	mPhaseNs[3] += nowNs() - (afterFence ? afterFence : targetFrom);
	if (!ok)
		noteNotRun(!allocated ? "no runtime buffers, or its target not mapped" : !texturesOk ? "a texture or sampler not set up"
		           : !indicesOk ? "its indices not staged" : !streamOk ? "its stream does not fit"
		           : failedStep ? failedStep : "its colour attachment's rows", name[2]);
	if (talk)
		pvlog("host: render: draw of %u %s x %u (type %u) into texture %u (%ux%u) with \"%s\" and \"%s\" on the card: %s (%llu us); blending %u, "
		      "colour %u %u %u, alpha %u %u %u, mask 0x%x", count, draw.indexBytes ? "indices" : "vertices", draw.instances, draw.type, target,
		      width, height, name[1], name[2], ok ? "done" : "NOT done", ns / 1000, blend[2], blend[3], blend[4], blend[5], blend[6], blend[7],
		      blend[8], blend[9]);
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
		const uint32_t dp = guestPfn(task, dstVa, true), sp = srcVa ? guestPfn(task, srcVa) : 1;
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

// A blit's texture region and the buffer's side of it. The texture's part: {u16 slice, u16 level} (rdna4-mtlslices: slice n of an array is
// n, level n of a mipmap n << 16, the dword after it 0; measured 2026-11-05).
struct RDNA4PvHost::BlitRegion {
	uint64_t origin[3], size[3];
	uint64_t offset, bytesPerRow, bytesPerImage;
	uint32_t slice, level;

	void place(const uint8_t *p)
	{
		uint32_t v;

		memcpy(&v, p, 4);
		slice = v & 0xffff;
		level = v >> 16;
	}
};

// M2: Blit.fillBuffer {u32 buffer, u64 offset, u64 length, u32 value} and Blit.copyFromBuffer {u32 source, u32 destination, u64 source offset,
// u64 destination offset, u64 size} (measured against mtlprobe on the rig, 2026-10-21), run on the card.
void RDNA4PvHost::runBlit(uint32_t task, uint32_t id, const uint8_t *p, uint32_t bytes) {
	if (!gCompute)
		return;

	flushHeld();
	waitDraws();   // blits read and write with the CPU and SDMA, after the draws before them
	uint64_t va = 0, len = 0, va2 = 0, len2 = 0;
	if (id == 0x132 && bytes >= 24) {
		uint32_t buf, value;
		uint64_t off, n;
		memcpy(&buf, p, 4);
		memcpy(&off, p + 4, 8);
		memcpy(&n, p + 12, 8);
		memcpy(&value, p + 20, 4);
		const bool ok = bufferRange(task, buf, &va, &len) && off <= len && n <= len - off && !(n & 3) && !((va + off) & 3) &&
		                dmaGuest(task, va + off, 0, (value & 0xff) * 0x01010101u, n);

		if (ok)
			dropShadowRange(task, va + off, n);   // a texture made from the buffer
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

		if (ok)
			dropShadowRange(task, va2 + dof, n);
		pvlog("host: blit: copyFromBuffer %u +0x%llx -> %u +0x%llx, 0x%llx bytes on the card: %s", src, so, dst, dof, n, ok ? "done" : "NOT done");
	} else if (id == 0x12e && bytes >= 88) {
		// Blit.copyFromTexture {u32 texture, u32 buffer, u64 origin[3], u64 size[3], u64 destination offset, u64 bytes per row, u64 bytes per image,
		// u32 slice, u32 level} (mtlprobe: 64x64x1, 0x100 per row, 0x4000 per image; measured 2026-10-21). Only the whole of a linear texture, copied
		// as bytes: what a texture's memory looks like is not measured yet beyond its size (64x64 BGRA8 = exactly 0x4000).
		uint32_t tex, buf;
		BlitRegion r;
		memcpy(&tex, p, 4);
		memcpy(&buf, p + 4, 4);
		memcpy(r.origin, p + 8, 24);
		memcpy(r.size, p + 32, 24);
		memcpy(&r.offset, p + 56, 8);
		memcpy(&r.bytesPerRow, p + 64, 8);
		memcpy(&r.bytesPerImage, p + 72, 8);
		r.place(p + 80);

		// the whole of a linear 32-bit texture as bytes, on the card; else row by row
		const uint64_t bpi = r.bytesPerImage, dof = r.offset;
		const bool origin = !r.origin[0] && !r.origin[1] && !r.origin[2] && !r.slice && !r.level;
		const bool whole = origin && r.size[2] == 1 && r.bytesPerRow == r.size[0] * 4 && bpi == r.bytesPerRow * r.size[1];
		bool onCard = whole && textureRange(task, tex, &va, &len) && bpi == len && bufferRange(task, buf, &va2, &len2);

		onCard = onCard && dof <= len2 && bpi <= len2 - dof && dmaGuest(task, va2 + dof, va, 0, bpi);

		const bool ok = onCard || copyTextureRegion(task, false, tex, buf, r);
		pvlog("host: blit: copyFromTexture %u (%llux%llux%llu at %llu, %llu, %llu, slice %u, level %u, %llu bytes per row) -> buffer %u +0x%llx %s: %s",
		      tex, r.size[0], r.size[1], r.size[2], r.origin[0], r.origin[1], r.origin[2], r.slice, r.level, r.bytesPerRow, buf, dof,
		      onCard ? "on the card" : "by rows", ok ? "done" : "NOT done");
		if (!ok || gTalk)
			dumpTexture(task, tex);
	} else if (id == 0x12c && bytes >= 88) {
		// Blit.copyFromBuffer into a texture {u32 buffer, u32 texture, u64 source offset, u64 source bytes per row, u64 source bytes per image,
		// u64 size[3], u64 destination origin[3], u32 slice, u32 level} (rdna4-mtlbuftex private, 2026-11-05).
		uint32_t buf, tex;
		BlitRegion r;
		memcpy(&buf, p, 4);
		memcpy(&tex, p + 4, 4);
		memcpy(&r.offset, p + 8, 8);
		memcpy(&r.bytesPerRow, p + 16, 8);
		memcpy(&r.bytesPerImage, p + 24, 8);
		memcpy(r.size, p + 32, 24);
		memcpy(r.origin, p + 56, 24);
		r.place(p + 80);
		const bool ok = copyTextureRegion(task, true, tex, buf, r);

		if (textureRange(task, tex, &va2, &len2))
			dropShadowRange(task, va2, len2);

		pvlog("host: blit: copyFromBuffer %u +0x%llx (%llu bytes per row, %llu per image) -> texture %u (%llux%llux%llu at %llu, %llu, %llu, slice %u, "
		      "level %u) by rows: %s", buf, r.offset, r.bytesPerRow, r.bytesPerImage, tex, r.size[0], r.size[1], r.size[2], r.origin[0], r.origin[1],
		      r.origin[2], r.slice, r.level, ok ? "done" : "NOT done");
		if (!ok || gTalk)
			dumpTexture(task, tex);
	}
}

// A texture's whole heap descriptor, for the blits it failed (the first few of a boot).
void RDNA4PvHost::dumpTexture(uint32_t task, uint32_t id)
{
	static uint32_t dumped;
	uint32_t rec[3] = {}, td[32] = {};

	if (dumped >= 16 || !readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(rec), sizeof(rec)))
		return;

	dumped++;
	readGuest(task, rec[1], reinterpret_cast<uint8_t *>(td), (rec[0] >> 8) < sizeof(td) ? rec[0] >> 8 : sizeof(td));
	for (uint32_t i = 0; i < 32; i += 8)
		pvlog("host: blit: texture %u (heap record %08x) [%u]: %08x %08x %08x %08x %08x %08x %08x %08x", id, rec[0], i, td[i], td[i + 1], td[i + 2],
		      td[i + 3], td[i + 4], td[i + 5], td[i + 6], td[i + 7]);
}

// A region of level 0 of a texture from or into a buffer (rows bytesPerRow, images bytesPerImage apart from offset), row by row with the
// CPU: the blits the card does not run yet. A 2D texture, an array's slice or a 3D texture's depth slices; the texture's memory as
// textureSource finds it; not into an IOSurface.
bool RDNA4PvHost::copyTextureRegion(uint32_t task, bool toTexture, uint32_t tex, uint32_t buf, const BlitRegion &r)
{
	TextureSource t;
	uint64_t va, len;

	if (r.level || !textureSource(task, tex, t) || (toTexture && t.surface.memory) || !bufferRange(task, buf, &va, &len))
		return false;

	const TextureFormat *fm = textureFormat(t.format);
	const uint64_t images = r.size[2];
	const uint64_t sliceAt = static_cast<uint64_t>(r.slice) * t.sliceBytes;

	if (!fm || !r.size[0] || !r.size[1] || !images)
		return false;

	// within the texture: its rows, depth slices and array slices (the guest's u64s checked without summing them)
	if (r.size[0] > t.width || r.origin[0] > t.width - r.size[0] || r.size[1] > t.height || r.origin[1] > t.height - r.size[1] ||
	    images > t.depth || r.origin[2] > t.depth - images)
		return false;

	const uint64_t rowBytes = r.size[0] * fm->bpp;   // the width is a u32: no wrap

	if (rowBytes > 0x10000)
		return false;

	if (r.slice >= t.slices || (r.slice && !t.sliceBytes))
		return false;

	// within the buffer: the last image's last row ends inside it
	uint64_t inBuffer, rows, end;

	if (r.bytesPerRow < rowBytes || r.offset > len || __builtin_mul_overflow(images - 1, r.bytesPerImage, &inBuffer) ||
	    __builtin_mul_overflow(r.size[1] - 1, r.bytesPerRow, &rows) || __builtin_add_overflow(inBuffer, rows, &end) ||
	    __builtin_add_overflow(end, rowBytes, &end) || end > len - r.offset)
		return false;

	auto *row = static_cast<uint8_t *>(IOMalloc(rowBytes));
	const uint32_t n = static_cast<uint32_t>(rowBytes);
	bool ok = row != nullptr;

	cardToMemory(t.surface.surface);   // read by the CPU

	for (uint64_t z = 0; z < images && ok; z++) {
		for (uint64_t y = 0; y < r.size[1] && ok; y++) {
			const uint64_t at = sliceAt + (r.origin[2] + z) * t.imageBytes + (r.origin[1] + y) * t.rowBytes + r.origin[0] * fm->bpp;
			const uint64_t other = va + r.offset + z * r.bytesPerImage + y * r.bytesPerRow;

			if (toTexture) {
				ok = readGuest(task, other, row, n) && copyGuest(task, t.va + at, row, n, true);
				continue;
			}

			if (t.surface.memory)
				ok = t.surface.memory->readBytes(t.surface.base + at, row, n) == n;
			else
				ok = readGuest(task, t.va + at, row, n);

			ok = ok && copyGuest(task, other, row, n, true);
		}
	}

	if (row)
		IOFree(row, rowBytes);

	return ok;
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
		// {16-byte header, u8 n, n x {u8 key, u8 size, value}} as a render pipeline's, then the kernel's name: key 0 its function
		// (WindowServer's sum_rgba_columns: {2: 19, 1: 1, 0: 88}, the function made just before the pipeline). Key 2's value was taken for
		// the function, and a task with more than one function never had its kernels run.
		const auto *pb = reinterpret_cast<const uint8_t *>(desc);
		uint32_t named = 0;

		for (uint32_t k = 0, at = 17; k < pb[16] && at + 2 <= bytes; k++, at += 2 + pb[at + 1])
			if (pb[at] == 0 && pb[at + 1] == 4 && at + 6 <= bytes)
				memcpy(&named, pb + at + 2, 4);

		if (named && named <= 0x10000 && resourceType(task, named) == 6) {
			fn = named;
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
		// A name longer than the helper's request holds (CoreImage's stitched kernels: hundreds of characters) is sent empty: the helper
		// then takes the library's only function. The hash tells the functions apart.
		if (!memcmp(head + i, "NAME", 4) && n && i + 6 + n <= sizeof(head) && names++ == 0) {
			const bool fits = n < nameCap;

			if (fits)
				memcpy(name, head + i + 6, n);

			name[fits ? n : 0] = 0;
			i += 5 + n;   // not into the name: it may hold "NAME" or "HASH"
		} else if (!memcmp(head + i, "HASH", 4) && n == 32 && i + 6 + 32 <= sizeof(head) && names == 1) {
			memcpy(hash, head + i + 6, 32);
		}
	}
	if (names != 1) {
		const auto *h = reinterpret_cast<const uint32_t *>(head);
		const auto *d = reinterpret_cast<const uint32_t *>(desc);

		pvstat("host: compute: task %u function %u: the metallib holds %u functions, not one: not run; header %08x %08x %08x %08x %08x %08x %08x "
		       "%08x %08x %08x %08x %08x, function %08x %08x %08x %08x %08x %08x", task, fn, names, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
		       h[8], h[9], h[10], h[11], d[0], d[1], d[2], d[3], d[4], d[5]);
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
		gCompute->rtPreferCpuTables(this);
		gCompute->rtFreesWhenIdle(this);
		mRtOpen = gCompute->rtOpen(this) == kIOReturnSuccess;   // ponytail: never closed, the host lives as long as the boot
	uint64_t out[9] = {};
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
// so a function that cannot be translated is asked for once). Null without a helper, or when it did not answer within kTranslateWait (not
// cached): WindowServer's largest shader (Xgc, 440 KB of code) takes the helper 6 s.
// Whether a translator helper is connected, waiting up to kHelperWait for one: WindowServer draws its first frames before launchd has started
// the helper, and a draw left out is not drawn again until its region changes. Waits once until a helper connects; while the helper
// restarts (it exits when lld cannot run again) the same.
static bool waitForHelper() {
	static constexpr uint32_t kHelperWait = 20;   // seconds
	uint64_t deadline = 0;

	IOLockLock(gTr.lock);
	if (!gTr.helper && !gTr.gaveUp) {
		pvlog("host: waiting up to %u s for the translator helper", kHelperWait);
		clock_interval_to_deadline(kHelperWait, kSecondScale, &deadline);
		while (!gTr.helper && IOLockSleepDeadline(gTr.lock, &gTr.helper, deadline, THREAD_UNINT) == THREAD_AWAKENED) {
		}

		gTr.gaveUp = !gTr.helper;
	}

	const bool helper = gTr.helper;
	IOLockUnlock(gTr.lock);

	return helper;
}

// The entry for a new translation: a free one, else the least recently used. Not the oldest: a draw asks for its vertex shader, then its
// fragment shader, and the fragment shader's translation must not take the vertex shader's entry (the draw then ran freed code, which
// read through a null address and hung the card).
RDNA4PvHost::Jit &RDNA4PvHost::jitSlot()
{
	uint32_t lru = 0;

	if (mJits < kJits)
		return mJit[mJits++];

	for (uint32_t i = 1; i < kJits; i++)
		if (mJit[i].used < mJit[lru].used)
			lru = i;

	return mJit[lru];
}

const RDNA4PvHost::Jit *RDNA4PvHost::translated(uint32_t task, uint64_t lib, const char *name, const uint8_t hash[32]) {
	mJitClock++;

	for (uint32_t i = 0; i < mJits; i++) {
		Jit &c = mJit[i];

		if (memcmp(c.hash, hash, 32) || strcmp(c.name, name) || (!name[0] && c.lib != lib) || !(c.program || c.gfx || c.helper == gTr.helpers))
			continue;

		c.used = mJitClock;

		return c.program || c.gfx ? &c : nullptr;   // a failure stays one until another helper connects
	}

	if (!gTr.lock || !waitForHelper())
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
	mTranslations++;
	const uint64_t askedAt = nowNs();
	IOLockLock(gTr.lock);
	gTr.id = ++gTr.seq ? gTr.seq : ++gTr.seq;   // never 0 (0 = no request)
	gTr.taken = gTr.done = false;
	gTr.lib = bytes;
	gTr.libLen = static_cast<uint32_t>(size);
	strlcpy(gTr.name, name, sizeof(gTr.name));
	IOLockWakeup(gTr.lock, &gTr.id, false);
	static constexpr uint32_t kTranslateWait = 15;   // seconds
	uint64_t deadline = 0;
	clock_interval_to_deadline(kTranslateWait, kSecondScale, &deadline);
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
		pvlog("host: compute: \"%s\": the helper did not translate it within %u s", name, kTranslateWait);
		return nullptr;
	}
	Jit &j = jitSlot();

	if (j.program)
		gCompute->rtUnload(this, j.program);
	if (j.gfx)
		IOFree(j.gfx, j.gfxLen);
	memcpy(j.hash, hash, 32);
	strlcpy(j.name, name, sizeof(j.name));
	j.lib = lib;
	j.helper = gTr.helpers;
	j.used = mJitClock;
	const bool shader = elf && elfLen >= 32 && !memcmp(elf, "RGFX", 4);
	j.program = elf && !shader && buffers <= 8 ? load(elf, elfLen, name) : 0;
	j.buffers = buffers;
	j.written = written;
	j.gfx = shader ? elf : nullptr;   // kept: the host places it into its code buffer for each draw
	j.gfxLen = shader ? elfLen : 0;
	if (elf && !shader)
		IOFree(elf, elfLen);
	pvstat("host: \"%s\" %s by the helper in %llu ms (%llu bytes of metallib, %u bytes of %s)", name, elf ? "translated" : "could NOT be translated",
	       (nowNs() - askedAt) / 1000000, size, elfLen, shader ? "shader" : "code");
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
		uint32_t r[3];
		if (type != 6 && type != 7 && readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(r), sizeof(r)) && (r[0] >> 8) > 32)
			dumpGuest(task, r[1], (r[0] >> 8) < 256 ? r[0] >> 8 : 256);   // M4: textures and samplers in full, to read their formats
	}
}

// M3: Compute.dispatchThreads (size: threads in the grid[3], threads per threadgroup[3]), run on the card with the function translated
// AIR -> gfx1201 (at run time by the helper, or ahead of time into src/mtlkernels.h by tools/metal/m3). Each bound buffer, from its offset
// to its end, is copied into a runtime buffer, the kernel dispatched, and the buffers it writes copied back into the guest's pages.
void RDNA4PvHost::runCompute(uint32_t task, uint32_t pipeline, const Bindings &bound, const uint64_t *size) {
	mMapFloor = mMapClock;

	const uint32_t *buffer = bound.buffer;
	const uint64_t *offset = bound.offset;
	static constexpr uint64_t kMaxBuffer = 64ull << 20;   // ponytail: staged through one kernel allocation; map the guest pages if bigger is needed
	char name[64];
	uint8_t hash[32];
	uint64_t lib = 0;
	if (!functionOf(task, pipeline, name, sizeof(name), hash, &lib))
		return;
	flushHeld();   // the kernel reads and writes the guest's memory
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
	ok = ok && (!most || stage);   // a kernel of textures only has no buffers to stage
	for (unsigned b = 0; b < nb && ok; b++)
		ok = gCompute->rtAlloc(this, bytes[b], handle[b], gpu[b]) == kIOReturnSuccess && readGuest(task, va[b], stage, static_cast<uint32_t>(bytes[b])) &&
		     gCompute->rtCopy(this, handle[b], 0, kernel_task, reinterpret_cast<mach_vm_address_t>(stage), bytes[b], true) == kIOReturnSuccess;
	// The kernel's textures and samplers in a table of their own (air2amdgpu.cpp: its address follows the dimensions in the kernargs).
	StagedTexture staged[16] = {};
	uint64_t tableHandle = 0, tableGpu = 0;
	bool textures = false;
	for (unsigned k = 0; k < 16; k++)
		textures |= bound.texture[k] || bound.sampler[k];
	if (ok && textures) {
		static uint32_t table[0x400 / 4];   // ponytail: one host, dispatches run one at a time on its work loop
		memset(table, 0, sizeof(table));
		waitDraws();   // dispatches run alone
		readyDraws();
		ok = stageTextures(task, bound, table, staged, true, "kernel", true) && gCompute->rtAlloc(this, sizeof(table), tableHandle, tableGpu) == kIOReturnSuccess &&
		     gCompute->rtCopy(this, tableHandle, 0, kernel_task, reinterpret_cast<mach_vm_address_t>(table), sizeof(table), true) == kIOReturnSuccess;
	}
	uint64_t micros = 0;

	// A kernel that faulted once is not run again: each fault resets the address space's waves, and the draws after it hung one by one
	// (VideoToolbox's VTMTSComputeFunction for System Settings' thumbnails: the mouse stood still).
	for (uint32_t i = 0; ok && i < mFaultedCount; i++) {
		if (mFaulted[i] != prog)
			continue;

		ok = false;
		pvlog("host: compute: \"%s\" faulted before: not run", name);
	}

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
		memcpy(d.kernargs + 8 * nb + sizeof(dims), &tableGpu, 8);   // 0 without textures (the table's kernels ignore it)
		d.kernargBytes = 8 * nb + sizeof(dims) + 8;
		ok = gCompute->rtDispatch(this, d, micros) == kIOReturnSuccess;

		if (!ok && prog && mFaultedCount < kFaulted) {
			mFaulted[mFaultedCount++] = prog;
			pvstat("host: compute: \"%s\" did not finish: not run again this boot", name);
		}
	}
	for (unsigned b = 0; b < nb && ok; b++) {
		if (!(written & (1u << b)))
			continue;

		ok = gCompute->rtCopy(this, handle[b], 0, kernel_task, reinterpret_cast<mach_vm_address_t>(stage), bytes[b], false) == kIOReturnSuccess &&
		     copyGuest(task, va[b], stage, static_cast<uint32_t>(bytes[b]), true);
		dropShadowRange(task, va[b], bytes[b]);
	}
	for (unsigned b = 0; b < nb; b++)
		if (handle[b])
			gCompute->rtFree(this, handle[b]);
	ok = releaseTextures(task, staged, ok) && ok;   // the textures the kernel may write copied back
	if (tableHandle)
		gCompute->rtFree(this, tableHandle);
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

// SubmitTransaction (FIFO 0x06) {u32 port, u32 surface, u32 task}, or SubmitTransaction2 (0x07) {u32 port, u32 task, u32 surface, ..., the gamma
// table}: the frame into the port's head (pvscanout.hpp), before the batch's stamp completes it. The gamma table is not applied.
void RDNA4PvHost::presentTransaction(const pvstream::FifoCommand &c, const uint8_t *cmd) {
	const uint8_t *pl = cmd + c.payloadOffset;
	uint32_t f[3];

	if (c.length - c.payloadOffset < sizeof(f))
		return;

	memcpy(f, pl, sizeof(f));

	const uint32_t port = f[0], surface = c.id == 0x07 ? f[2] : f[1], task = c.id == 0x07 ? f[1] : f[2];
	bool ok = gPresentOnCard && presentOnCard(port, surface);

	if (!ok) {   // copied by the CPU from the guest's memory
		void *s = retainSurface(surface);

		if (port < kScanoutPorts && mScanout[port].flips)   // into the head's own surface, which the HUBP must read then
			dropScanout(mScanout[port]);

		cardToMemory(s);
		if (s)
			static_cast<OSObject *>(s)->release();

		ok = PvScanout::present(port, surface, task);
	}

	mPresents += ok;

	if (ok && port < 8)
		timePresent(mPortTiming[port]);

	if (gTalk)
		pvlog("host: present: port %u, IOSurface %u (task %u): %s", port, surface, task, ok ? "copied" : "NOT copied");
}

// PKT3 DMA_DATA {header, control, src lo, hi, dst lo, hi, command} as Mesa's si_cp_dma for gfx9 on: source and destination through the L2,
// the CP waiting for the copy (CP_SYNC) on the last one.
static uint32_t dmaData(uint32_t *out, uint64_t src, uint64_t dst, uint32_t bytes, bool sync)
{
	out[0] = 0xc0055000u;                                   // PKT3(DMA_DATA, 5)
	out[1] = 3u << 20 | 3u << 29 | (sync ? 1u << 31 : 0);    // DST_SEL, SRC_SEL: DST_ADDR_TC_L2, SRC_ADDR_TC_L2; CP_SYNC
	out[2] = static_cast<uint32_t>(src);
	out[3] = static_cast<uint32_t>(src >> 32);
	out[4] = static_cast<uint32_t>(dst);
	out[5] = static_cast<uint32_t>(dst >> 32);
	out[6] = bytes & 0x3ffffff;                             // BYTE_COUNT
	return 7;
}

void RDNA4PvHost::timePresent(PortTiming &t)
{
	const uint64_t now = nowNs();
	const uint64_t sinceVbl = now - mVblAt;

	if (t.last && now - t.last > kVblPeriodNs * 3 / 2 && now - t.last < 4 * kVblPeriodNs)   // an animation, one frame or more missed
		t.missed++;

	// How smooth an animation was: the time since the last frame in VBLs (rounded), longer gaps an idle display's
	const uint64_t gap = t.last ? (now - t.last + kVblPeriodNs / 2) / kVblPeriodNs : 0;

	if (gap >= 1 && gap <= 8)
		t.gaps[gap < 4 ? gap - 1 : 3]++;

	t.last = now;
	t.frames++;

	if (mVblAt && sinceVbl < 4 * kVblPeriodNs) {
		t.vblNs += sinceVbl;
		t.vblMaxNs = sinceVbl > t.vblMaxNs ? sinceVbl : t.vblMaxNs;
	}
}

bool RDNA4PvHost::presentOnCard(uint32_t port, uint32_t sid)
{
	static constexpr uint32_t kChunk = 1u << 20;
	PvScanout::Head h;

	if (port >= kScanoutPorts || !mRtOpen || !PvScanout::head(port, h) || !h.pitch || !h.height)
		return false;

	ScanoutMap &sm = mScanout[port];
	void *s = retainSurface(sid);
	IOMemoryDescriptor *md = s ? gSurfaceFns.memory(s) : nullptr;

	// A frame of another size than the head: macOS was told to use another of its modes. The head goes to it (once a size), or for a
	// size no mode has that is larger than the head's largest (a scaled mode's), to the largest.
	const uint32_t frameW = md ? gSurfaceFns.width(s) : 0, frameH = md ? gSurfaceFns.height(s) : 0;
	const uint32_t format = md ? gSurfaceFns.pixelFormat(s) : 0;

	if (frameW && frameH && (frameW != h.width || frameH != h.height) && (frameW != sm.triedW || frameH != sm.triedH)) {
		uint32_t bigW = 0, bigH = 0;
		const bool larger = PvScanout::largest(port, bigW, bigH) && frameW > bigW && frameH > bigH;

		sm.triedW = frameW;
		sm.triedH = frameH;

		if (!larger && PvScanout::setMode(port, frameW, frameH) && PvScanout::head(port, h))
			pvlog("host: present: port %u now %ux%u for frames of that size", port, h.width, h.height);
		else if (larger && (h.width != bigW || h.height != bigH) && PvScanout::setMode(port, bigW, bigH) && PvScanout::head(port, h))
			pvlog("host: present: port %u now %ux%u for frames of %ux%u (format 0x%08x), scaled down", port, h.width, h.height, frameW, frameH,
			      format);
	}

	// the scanout at a VA of ours, mapped again when the head moved
	const uint64_t scanBytes = static_cast<uint64_t>(h.pitch) * h.height;

	if (sm.phys != h.phys || sm.bytes != scanBytes || sm.mode != gPresentOnCard) {   // set up again when rdna4_pvpresent changed too
		const uint32_t triedW = sm.triedW, triedH = sm.triedH;

		dropScanout(sm);
		sm.triedW = triedW;
		sm.triedH = triedH;

		if (gCompute->rtMapVram(this, h.phys, scanBytes, sm.handle, sm.gpu) != kIOReturnSuccess) {
			if (s)
				static_cast<OSObject *>(s)->release();

			return false;
		}

		sm.phys = h.phys;
		sm.bytes = scanBytes;
		sm.mode = static_cast<uint8_t>(gPresentOnCard);
		setUpFlips(sm, port);
	}

	const uint64_t base = md ? gSurfaceFns.baseOffset(s) : 0, bpr = md ? gSurfaceFns.bytesPerRow(s) : 0;
	const uint32_t width = bpr && h.width < bpr / 4 ? h.width : static_cast<uint32_t>(bpr / 4);

	// larger than the head both ways: a scaled mode's frame, scaled down into the scanout (else the head's part of it is copied)
	const bool scaled = frameW > h.width && frameH > h.height && bpr / 4 >= frameW && scaleKernel();
	const uint32_t height = scaled ? frameH : md && h.height < gSurfaceFns.height(s) ? h.height : md ? gSurfaceFns.height(s) : 0;
	uint64_t handle = 0, src = 0;
	const CardSurface *card = cardSurface(s);
	bool ok = md && width && height && base + bpr * height <= md->getLength();

	if (ok && card && card->loaded && card->base == base) {   // the frame on the card already
		src = card->gpu;
	}
	else if (ok && md->prepare() == kIOReturnSuccess) {
		ok = mapMemory(md, base, bpr * height, handle, src);
		md->complete();
	}
	else {
		ok = false;
	}

	if (ok)
		keepOnCard(s);   // its next frame drawn on the card

	if (s)
		static_cast<OSObject *>(s)->release();

	uint64_t cpu = 0;

	if (ok && !mPresentIb.handle && gCompute->rtAllocHost(this, kernel_task, kPresentIbBytes * kScanoutPorts, RDNA4_HOST_EXECUTABLE, mPresentIb.handle,
	                                                        mPresentIb.gpu, cpu) == kIOReturnSuccess)
		mPresentIb.cpu = reinterpret_cast<uint8_t *>(cpu);

	if (!ok || !mPresentIb.cpu)
		return false;

	if (sm.fence && !gCompute->rtGfxFenceDone(this, static_cast<uint32_t>(sm.fence)))
		waitFor(sm.fence);   // the port's IB of the frame before (each port has its own: the other display's frame is not waited for)

	flipDone();   // the frame before asked for, if its copy is done: this one goes into another buffer

	roomForDraw();

	// the rows, or the whole frame in chunks when the surface's rows are the scanout's
	const uint64_t ibAt = static_cast<uint64_t>(port) * kPresentIbBytes;
	auto *ib = reinterpret_cast<uint32_t *>(mPresentIb.cpu + ibAt);
	const uint32_t maxDw = kPresentIbBytes / 4;
	const bool whole = bpr == h.pitch;
	const uint64_t total = whole ? bpr * height : height;
	uint32_t n = idleBarrier(ib);   // the frame's draws done, their colour caches written back
	const bool timed = n && profiling();

	if (!n)
		return false;

	n += timed ? timeStamp(ib + n, false) : 0;

	const uint32_t target = sm.flips ? flipTarget(sm) : 0;
	const uint64_t dst = sm.flips ? sm.va[target] : sm.gpu;

	if (scaled) {
		const uint32_t k = scaleFrame(ib + n, port, src, static_cast<uint32_t>(bpr / 4), frameW, frameH, dst, h, format == 0x6c313072);   // 'l10r'
		const uint32_t idle = k ? idleBarrier(ib + n + k) : 0;   // its writes in memory before the flip

		if (!k || !idle)
			return false;

		n += k + idle;
	}

	for (uint64_t i = 0; !scaled && i < total && n + 7 + 8 <= maxDw;) {
		const uint64_t at = whole ? i : i * bpr, to = whole ? i : i * h.pitch;
		const uint32_t len = whole ? static_cast<uint32_t>(total - i < kChunk ? total - i : kChunk) : width * 4;

		i += whole ? len : 1;
		n += dmaData(ib + n, src + at, dst + to, len, i >= total);
	}

	if (n + 7 + 8 > maxDw)   // too many rows for the IB
		return false;

	// debug.rdna4_pvframe: the frame as it is, into host memory too; a scaled one as it went into the scanout
	const uint64_t grabFrom = scaled ? dst : src, grabPitch = scaled ? h.pitch : bpr;
	const uint32_t grabH = scaled ? h.height : height;
	const bool grab = mGrabWant == port + 1 && !mGrabFlying && !mGrabReady && grabBuffer(grabPitch * grabH);

	if (grab) {
		for (uint64_t i = 0, total2 = grabPitch * grabH; i < total2 && n + 7 + 8 <= maxDw;) {
			const uint32_t len = static_cast<uint32_t>(total2 - i < kChunk ? total2 - i : kChunk);

			n += dmaData(ib + n, grabFrom + i, mGrab.gpu + i, len, i + len >= total2);
			i += len;
		}

		mGrab.width = scaled ? h.width : width;
		mGrab.height = grabH;
		mGrab.pitch = static_cast<uint32_t>(grabPitch);
	}

	if (n + 7 + 8 > maxDw)
		return false;

	n += timed ? timeStamp(ib + n, true) : 0;

	__sync_synchronize();   // seen by the card (snooped) before the submit

	uint64_t fence = 0;

	if (submitGfx(mPresentIb.gpu + ibAt, n, fence) != kIOReturnSuccess)
		return false;

	flying(fence, "present");
	sm.fence = fence;

	if (grab) {
		mGrab.fence = fence;
		mGrabFlying = true;
	}

	if (sm.flips) {   // shown once the copy is done (flipDone)
		sm.queued = static_cast<uint8_t>(target + 1);
		sm.flipFence = fence;
	}

	return true;
}

// scale.cl loaded into the runtime with a page for its kernargs, once: false (and not tried again) if it could not be.
bool RDNA4PvHost::scaleKernel()
{
	if (mScale.code || mScale.tried)
		return mScale.code != 0;

	mScale.tried = true;

	uint64_t out[9] = {}, cpu = 0;

	if (gCompute->rtLoad(this, kernel_task, reinterpret_cast<mach_vm_address_t>(kScaleCodeObject), sizeof(kScaleCodeObject), "scale", out) !=
	    kIOReturnSuccess || gCompute->rtAllocHost(this, kernel_task, kScaleArgs * kScanoutPorts, 0, mScale.argsHandle, mScale.argsGpu, cpu) !=
	    kIOReturnSuccess) {
		pvlog("host: present: the scale kernel did NOT load: frames of scaled modes are cropped");

		return false;
	}

	mScale.argsCpu = reinterpret_cast<uint8_t *>(cpu);
	mScale.rsrc[0] = static_cast<uint32_t>(out[3]);
	mScale.rsrc[1] = static_cast<uint32_t>(out[4]);
	mScale.rsrc[2] = static_cast<uint32_t>(out[5]);
	mScale.wave32 = out[6] & (1u << 10);
	mScale.code = out[8];

	return true;
}

// The dispatch of scale.cl from `src` (`srcW` x `srcH`, `srcPitch` pixels a row) into the head's scanout at `dst`, into `ib`; its
// kernargs in the port's slot (free: the port's IB before waited for). Returns the dwords written.
uint32_t RDNA4PvHost::scaleFrame(uint32_t *ib, uint32_t port, uint64_t src, uint32_t srcPitch, uint32_t srcW, uint32_t srcH, uint64_t dst,
                                 const PvScanout::Head &h, bool tenBit)
{
	struct Args { uint64_t src, dst; uint32_t srcPitch, dstPitch, srcW, srcH, dstW, dstH, tenBit; } args = {
		src, dst, srcPitch, h.pitch / 4, srcW, srcH, h.width, h.height, tenBit };
	const uint64_t at = mScale.argsGpu + port * kScaleArgs;
	const uint32_t zero = 0, all[2] = {0xffffffffu, 0xffffffffu}, none4[4] = {}, start[3] = {}, threads[3] = {64, 1, 1};
	const uint32_t pgm[2] = {static_cast<uint32_t>(mScale.code >> 8), static_cast<uint32_t>(mScale.code >> 40)};
	const uint32_t user[2] = {static_cast<uint32_t>(at), static_cast<uint32_t>(at >> 32)};
	uint32_t n = 0;

	memcpy(mScale.argsCpu + port * kScaleArgs, &args, sizeof(args));
	__sync_synchronize();

	n += Pm4::acquireMem(ib + n, Pm4::kGcrMemSync);   // the kernargs not the last frame's in the scalar cache
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_PGM_LO, pgm, 2);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC1, mScale.rsrc, 2);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_PGM_RSRC3, &mScale.rsrc[2], 1);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_RESOURCE_LIM, &zero, 1);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_TMPRING, &zero, 1);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE0, all, 2);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE2, all, 2);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_THREAD_SE4, none4, 4);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_START_X, start, 3);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_NUM_THREAD_X, threads, 3);
	n += Pm4::setShReg(ib + n, RDNA4_PM4_COMPUTE_USER_DATA0, user, 2);
	n += Pm4::dispatchDirect(ib + n, (h.width + 63) / 64, h.height, 1,
	                         Pm4::kDispatchShaderEn | Pm4::kDispatchForceStart0 | (mScale.wave32 ? Pm4::kDispatchWave32 : 0));

	return n;
}

// The port's two buffers of its own for flipping, and the HUBP that reads its scanout; flipping stays off without them.
void RDNA4PvHost::setUpFlips(ScanoutMap &sm, uint32_t port)
{
	uint8_t hubp = 0;
	uint64_t mc = 0;

	if (gPresentOnCard != 1 || !gCompute->rtScanoutHubp(sm.phys, hubp, mc))
		return;

	sm.hubp = hubp;
	sm.mc[0] = mc;
	sm.va[0] = sm.gpu;

	for (uint32_t k = 0; k < 2; k++) {
		if (gCompute->rtAlloc(this, sm.bytes, sm.back[k], sm.va[k + 1]) != kIOReturnSuccess) {
			sm.back[k] = 0;
			break;
		}

		sm.mc[k + 1] = gCompute->rtBufferMc(this, sm.back[k]);

		// what the head shows now: a frame smaller than the head leaves the rest as it was
		if (sm.mc[k + 1] && !rowsOnCard(sm.gpu, static_cast<uint32_t>(sm.bytes), sm.va[k + 1], static_cast<uint32_t>(sm.bytes),
		                                 static_cast<uint32_t>(sm.bytes), 1))
			sm.mc[k + 1] = 0;
	}

	sm.flips = sm.mc[1] && sm.mc[2];
	pvlog("host: present: port %u: HUBP%u reads its scanout at MC 0x%llx; frames %s", port, hubp, mc,
	      sm.flips ? "flipped at vblank between it and two buffers of ours" : "copied into it (no VRAM for flipping)");
}

// The buffer a frame goes into: the one the HUBP neither reads nor was asked to show; the one queued already if a frame's copy into it
// is not done yet (after it in the ring).
uint32_t RDNA4PvHost::flipTarget(ScanoutMap &sm)
{
	uint64_t requested = 0, inUse = 0;

	if (sm.queued)
		return sm.queued - 1u;

	if (!gCompute->rtScanoutShown(sm.hubp, requested, inUse) || (requested != sm.mc[0] && requested != sm.mc[1] && requested != sm.mc[2])) {
		pvlog("host: present: HUBP%u shows 0x%llx, none of the port's buffers: frames copied into the scanout again", sm.hubp, requested);
		sm.flips = false;

		return 0;
	}

	for (uint32_t k = 0; k < 3; k++)
		if (sm.mc[k] != requested && sm.mc[k] != inUse)
			return k;

	return 0;   // not reached: three buffers, two of them taken at most
}

// Host memory for debug.rdna4_pvframe's copy of a frame of `bytes`.
bool RDNA4PvHost::grabBuffer(uint64_t bytes)
{
	uint64_t cpu = 0;

	if (mGrab.handle && mGrab.bytes >= bytes)
		return true;

	if (mGrab.handle) {
		waitDraws();
		gCompute->rtFree(this, mGrab.handle);
	}

	mGrab = {};

	if (gCompute->rtAllocHost(this, kernel_task, bytes, 0, mGrab.handle, mGrab.gpu, cpu) != kIOReturnSuccess) {
		mGrab = {};
		return false;
	}

	mGrab.cpu = reinterpret_cast<uint8_t *>(cpu);
	mGrab.bytes = bytes;

	return true;
}

// debug.rdna4_pvframe: asks the host thread for port `port`'s next frame and waits for it (2 s at most), then writes it at half size.
// One reader at a time: a second one's request had the host thread allocate the buffer anew while the first still copied out of it.
int RDNA4PvHost::grabFrame(uint32_t port, struct sysctl_req *req)
{
	static uint32_t busy;

	if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
		return EBUSY;

	const int err = grabFrameOnce(port, req);

	__atomic_store_n(&busy, 0, __ATOMIC_RELEASE);

	return err;
}

int RDNA4PvHost::grabFrameOnce(uint32_t port, struct sysctl_req *req)
{
	mGrabReady = false;
	mGrabWant = port + 1;

	for (uint32_t i = 0; i < 2000 && !mGrabReady; i++)
		IOSleep(1);

	mGrabWant = 0;

	if (!mGrabReady)
		return ETIMEDOUT;

	const uint32_t size[2] = {mGrab.width / 2, mGrab.height / 2};
	auto *row = static_cast<uint32_t *>(IOMalloc(size[0] * 4));
	int err = row ? SYSCTL_OUT(req, size, sizeof(size)) : ENOMEM;

	for (uint32_t y = 0; y < size[1] && !err; y++) {
		const auto *line = reinterpret_cast<const uint32_t *>(mGrab.cpu + static_cast<uint64_t>(2 * y) * mGrab.pitch);

		for (uint32_t x = 0; x < size[0]; x++)
			row[x] = line[2 * x];

		err = SYSCTL_OUT(req, row, size[0] * 4);
	}

	if (row)
		IOFree(row, size[0] * 4);

	mGrabReady = false;

	return err;
}

// The frames whose copy is done asked for at the next vblank.
void RDNA4PvHost::flipDone()
{
	if (mGrabFlying && gCompute->rtGfxFenceDone(this, static_cast<uint32_t>(mGrab.fence))) {
		mGrabFlying = false;
		mGrabReady = true;
	}

	for (ScanoutMap &sm : mScanout) {
		if (!sm.flips || !sm.queued || !gCompute->rtGfxFenceDone(this, static_cast<uint32_t>(sm.flipFence)))
			continue;

		gCompute->rtScanoutFlip(sm.hubp, sm.mc[sm.queued - 1]);
		sm.queued = 0;
	}
}

// The HUBP back on surface `mc` (the head's own) before the CPU writes that or our buffers go: waited for until it reads it.
void RDNA4PvHost::scanoutHome(ScanoutMap &sm, uint64_t mc)
{
	uint64_t requested = 0, inUse = 0;

	if (!sm.flips)
		return;

	if (sm.queued) {
		waitDraws();
		sm.queued = 0;
	}

	gCompute->rtScanoutFlip(sm.hubp, mc);

	for (uint32_t i = 0; i < 100 && gCompute->rtScanoutShown(sm.hubp, requested, inUse) && inUse != mc; i++)   // a frame at most
		IOSleep(1);
}

// A port's scanout given up (the head moved, or frames go by the CPU): what the HUBP reads is the head's own surface again.
void RDNA4PvHost::dropScanout(ScanoutMap &sm)
{
	PvScanout::Head h;
	uint8_t hubp = 0;
	uint64_t mc = 0;
	const uint32_t port = static_cast<uint32_t>(&sm - mScanout);

	// where the head is now, if it moved: the HUBP the display code set up reads that
	scanoutHome(sm, PvScanout::head(port, h) && gCompute->rtScanoutHubp(h.phys, hubp, mc) ? mc : sm.mc[0]);

	if (sm.handle || sm.back[0] || sm.back[1])
		waitDraws();

	const uint64_t handles[] = {sm.handle, sm.back[0], sm.back[1]};

	for (uint64_t handle : handles)
		if (handle)
			gCompute->rtFree(this, handle);

	sm = {};
}

// GetDeviceInfo (FIFO 0x3a): the record 0x2d names the 4 KiB buffer the host fills; the layout and the values are docs/m0-pvgpu.md "The GetDeviceInfo reply".
void RDNA4PvHost::answerDeviceInfo(const pvstream::FifoCommand &c, const uint8_t *cmd) {
	const uint8_t *pl = cmd + c.payloadOffset;
	const uint32_t plen = c.length - c.payloadOffset;
	uint32_t rec[3] = {};
	if (plen >= sizeof(rec))
		memcpy(rec, pl, sizeof(rec));
	const bool known = rec[0] == pvstream::kDeviceInfoRecord || rec[0] == pvstream::kDeviceInfoRecordSequoia;
	if (plen < sizeof(rec) || !known) {
		pvlog("host: GetDeviceInfo without a 0x2d or 0x2a record (%u payload bytes, first word 0x%x): not answered", plen, rec[0]);
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
	const size_t pairs = pvstream::buildDeviceInfoReply(reinterpret_cast<uint8_t *>(map->getVirtualAddress()), bytes, gGpuCoreCount);
	__sync_synchronize();
	map->release();
	desc->release();
	mReplies++;
	pvlog("host: GetDeviceInfo answered: reply buffer page 0x%x (%u bytes, %u pair slots), %u pairs written, GpuCoreCount %u", page, bytes, bytes / 8,
	      static_cast<unsigned>(pairs), gGpuCoreCount);
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
		uint32_t width, height;

		info.id = port + 1;

		// a name of its own for each port: with two the same EDID, CoreDisplay took one display's saved mode for the other (mode 0x0 set on
		// the 4K head, "Failed to obtain mode info") and mirrored and scaled them in ways the windows did not follow
		snprintf(info.name, sizeof(info.name), "AMD RDNA4 PV%u", port + 1);
		if (PvScanout::enabled() && PvScanout::mode(port, width, height) && width <= 0xffff && height <= 0xffff) {
			info.width = info.modes[0].width = static_cast<uint16_t>(width);
			info.height = info.modes[0].height = static_cast<uint16_t>(height);
		}

		// the modes of the monitor its head can run, the one it is lit in first: macOS presents in the one chosen (presentOnCard)
		PvScanout::Mode modes[pvstream::disp::kMaxModes];
		const uint32_t count = PvScanout::enabled() ? PvScanout::modes(port, modes, pvstream::disp::kMaxModes) : 0;

		for (uint32_t i = 0; i < count; i++)
			info.modes[i] = { static_cast<uint16_t>(modes[i].width), static_cast<uint16_t>(modes[i].height),
			                  static_cast<uint32_t>(static_cast<uint64_t>(modes[i].milliHz) * 65536 / 1000) };

		if (count) {
			info.modeCount = count;
			info.width = info.modes[0].width;
			info.height = info.modes[0].height;
		}

		// IOPresentment takes only a plane with gamma, which the guest announces when transactions carry the table (FIFO 0x07)
		if (PvScanout::enabled())
			info.txnProtocol = 1;

		// No hardware cursor: the host draws none (the pipe's cursor fields are only logged), and with one WindowServer leaves the pointer
		// out of the frames it presents: invisible once the displays were told apart and no longer mirrored. Without, it draws the pointer.
		if (PvScanout::enabled())
			info.cursorWidth = info.cursorHeight = 0;
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
// The time the host spent on draws (staging, the card, copying back), logged every 5 s while there are any: how much of WindowServer's
// frame the host takes.
void RDNA4PvHost::noteDraw(uint64_t ns) {
	const uint64_t now = nowNs();

	if (!mDrawStatSince)
		mDrawStatSince = now;
	mDrawStatCount++;
	mDrawStatNs += ns;
	mDrawStatMaxNs = ns > mDrawStatMaxNs ? ns : mDrawStatMaxNs;
	if (now - mDrawStatSince < 5000000000ull)
		return;

	// two lines: one is cut off at the log's line length
	pvstat("host: render: %u draws, %u frames in the last %llu ms took %llu ms (the longest %llu ms): textures %llu, target in %llu, card %llu, "
	      "out %llu, pipeline %llu, target and buffers %llu ms; %u waits, %u shader pairs in, %u translations", mDrawStatCount, mPresents,
	      (now - mDrawStatSince) / 1000000, mDrawStatNs / 1000000, mDrawStatMaxNs / 1000000, mPhaseNs[0] / 1000000, mPhaseNs[1] / 1000000,
	      mPhaseNs[2] / 1000000, mPhaseNs[3] / 1000000, mPhaseNs[4] / 1000000, mPhaseNs[5] / 1000000, mWaits, mCodeIns, mTranslations);
	pvstat("host: render:   %u textures in place, %u copied, mapping %llu ms (%u new, %llu pages), rows %llu ms (%llu KiB, %u copies reused); "
	      "shadows: %u made, %u freed, %llu MiB; stale %u dropped, %u old, %u locked", mInPlace, mCopied, mMapNs / 1000000, mMapMisses, mMapPages, mCopyNs / 1000000, mCopiedBytes >> 10,
	      mCopiesReused, mShadowsMade, mShadowsFreed, mShadowBytes >> 20, mStale[0], mStale[1], mStale[2]);

	if (mNotRun)
		pvstat("host: render:   %u draws not run, the first with \"%s\": %s", mNotRun, mNotRunName, mNotRunWhy);

	// where the waits for the card came from (pvgpu.cpp lines), the most first
	char sites[160] = "";
	for (uint32_t k = 0, at = 0; k < 6 && at < sizeof(sites) - 16; k++) {
		WaitSite *top = nullptr;

		for (WaitSite &w : mWaitSites)
			top = w.count && (!top || w.count > top->count) ? &w : top;

		if (!top)
			break;

		at += snprintf(sites + at, sizeof(sites) - at, " %u@%u", top->count, top->line);
		top->count = 0;
	}
	if (sites[0])
		pvstat("host: render:   waits:%s", sites);
	memset(mWaitSites, 0, sizeof(mWaitSites));

	char ports[200] = "", gaps[200] = "";
	for (uint32_t p = 0, at = 0, gat = 0; p < 8 && at < sizeof(ports) - 64 && gat < sizeof(gaps) - 64; p++) {
		const PortTiming &t = mPortTiming[p];

		if (!t.frames)
			continue;

		at += snprintf(ports + at, sizeof(ports) - at, " port %u: %u (%u missed, VBL to present %llu us, most %llu us);", p, t.frames,
		               t.missed, t.vblNs / t.frames / 1000, t.vblMaxNs / 1000);
		gat += snprintf(gaps + gat, sizeof(gaps) - gat, " port %u: %u/%u/%u/%u;", p, t.gaps[0], t.gaps[1], t.gaps[2], t.gaps[3]);
	}
	if (ports[0] || mLateDone)
		pvstat("host: render:   frames:%s stamps behind draws %u, waited %llu us, most %llu us", ports, mLateDone,
		       mLateDone ? mLateNs / mLateDone / 1000 : 0, mLateMaxNs / 1000);
	if (gaps[0])
		pvstat("host: render:   frames after 1, 2, 3 and 4 to 8 VBLs:%s", gaps);
	if (mVbls)
		pvstat("host: render:   VBL events: %u, %llu us late on average, %u by 2 ms or more, the latest %llu us", mVbls,
		       mVblLateNs / mVbls / 1000, mVblsLate, mVblLateMaxNs / 1000);
	mVbls = mVblsLate = 0;
	mVblLateNs = mVblLateMaxNs = 0;

	uint64_t clocks = 0, active = 0;

	if (gCompute->rtGfxClocks(clocks, active) && mClocksAt) {
		const uint64_t us = (now - mClocksAt) / 1000;

		pvstat("host: render:   card: %llu MHz on average (clocks not gated), %llu %% of them busy", us ? clocks / us : 0,
		       clocks ? active * 100 / clocks : 0);
	}

	mClocksAt = now;

	pvstat("host: render:   targets: surfaces on the card %u (%llu MiB), in memory %u (%llu MiB); textures shared %u (%llu MiB), managed %u (%llu MiB), "
	       "private %u (%llu MiB)", mTargetDraws[0], mTargetBytes[0] >> 20, mTargetDraws[1], mTargetBytes[1] >> 20, mTargetDraws[2], mTargetBytes[2] >> 20,
	       mTargetDraws[3], mTargetBytes[3] >> 20, mTargetDraws[4], mTargetBytes[4] >> 20);
	memset(mTargetDraws, 0, sizeof(mTargetDraws));

	for (uint32_t k = 0; k < 4; k++) {
		CopiedTop *top = nullptr;

		for (CopiedTop &t : mCopiedTop)
			top = t.bytes && (!top || t.bytes > top->bytes) ? &t : top;

		if (!top)
			break;

		pvstat("host: render:   copied most: %s of task %u, %ux%u, %u bytes a row: %u times, %llu MiB", top->surface ? "a surface" : "a texture", top->task,
		       top->width, top->height, top->rowBytes, top->times, top->bytes >> 20);
		top->bytes = 0;
	}
	memset(mCopiedTop, 0, sizeof(mCopiedTop));
	memset(mTargetBytes, 0, sizeof(mTargetBytes));

	addTimes();

	char times[200] = "";
	for (uint32_t k = 0, at = 0; k < 8 && at < sizeof(times) - 48; k++) {
		GpuTime *top = nullptr;

		for (GpuTime &g : mGpuTimes)
			top = g.ticks && (!top || g.ticks > top->ticks) ? &g : top;

		if (!top)
			break;

		at += snprintf(times + at, sizeof(times) - at, " %s %u: %llu ms;", top->name, top->count, top->ticks / 100000);
		top->ticks = 0;
	}
	if (times[0])
		pvstat("host: render:   on the card:%s clears %llu MiB", times, mClearBytes >> 20);
	mClearBytes = 0;
	memset(mGpuTimes, 0, sizeof(mGpuTimes));

	uint32_t kept = 0;
	for (const CardSurface &c : mCardSurface)
		kept += c.handle != 0;
	pvstat("host: render:   surfaces on the card: %u, %u loaded into it, %u written back; shadows filled and written back by the card: %u, %u", kept,
	       mCardLoads, mCardBacks, mCardFills, mCardFlushes);
	mCardLoads = mCardBacks = mCardFills = mCardFlushes = 0;

	for (PortTiming &t : mPortTiming)
		t = { t.last, 0, 0, 0, 0 };
	mLateNs = mLateMaxNs = 0;
	mLateDone = 0;

	mNotRun = 0;
	mTexLines = 0;
	mShadowsMade = mShadowsFreed = 0;
	mStagedLines = 0;
	memset(mStale, 0, sizeof(mStale));
	mInPlace = mCopied = 0;
	mWaits = mCodeIns = mTranslations = mPresents = 0;
	mMapNs = 0;
	mMapMisses = 0;
	mMapPages = mCopiedBytes = mCopyNs = 0;
	mCopiesReused = 0;
	memset(mPhaseNs, 0, sizeof(mPhaseNs));
	mDrawStatSince = now;
	mDrawStatCount = 0;
	mDrawStatNs = 0;
	mDrawStatMaxNs = 0;
}

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
			const uint64_t late = now - mNextVblNs;

			frame = true;
			mVblAt = now;
			mVbls++;
			mVblLateNs += late < 4 * kVblPeriodNs ? late : 0;
			mVblLateMaxNs = late < 4 * kVblPeriodNs && late > mVblLateMaxNs ? late : mVblLateMaxNs;
			mVblsLate += late >= 2000000 && late < 4 * kVblPeriodNs;
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
		mNub->raiseInterrupt(0, true);   // with the line off as well, as the stamps (completeStamp)
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
	// Delivered with the line off too: IOAccelEventMachine2::waitForStamp has it on only while a thread waits synchronously for a stamp
	// (on at entry, off again at exit), so a completed command buffer nobody waited for was noticed only when the driver next polled, and
	// Metal's completion handlers came 50-100 ms late. Its interrupt action reads the status and finishes the stamps that are reached.
	const bool delivered = mNub->raiseInterrupt(0, true);
	static uint32_t logged = 0;
	if (logged++ < 12 || !delivered)
		pvlog("host: stamp[%u] = %u written (FIFO page offset 0x%x), INTR_STATUS_GPU bit %u set, interrupt 0 %s", index, value, index * 4, index,
		      delivered ? "raised" : "pending (the line is disabled or has no handler yet)");
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
		// the guest compares the read counter with the write counter to see that the ring is empty and how much room is left
		rec[1] = read + consume(CmdSource { tag, reinterpret_cast<const uint8_t *>(ch.map->getVirtualAddress()), ch.ringBytes, ch.kind, n }, read, pending);
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
		pvstream::buildDeviceInfoReply(expect, sizeof(expect), gGpuCoreCount);
		const uint8_t *reply = static_cast<const uint8_t *>(mTestReply->getBytesNoCopy());
		testCheck(memcmp(reply, expect, sizeof(expect)) == 0, "GetDeviceInfo reply buffer holds the documented key/value pairs");
		uint8_t info[pvstream::kDeviceInfoBytes] = {};
		pvstream::parseDeviceInfoReference(reply, 0x1000 / 8, info);
		uint32_t msaa, gpuCores, shaderVersion;
		memcpy(&msaa, info + 0x00, 4);
		memcpy(&gpuCores, info + 0x84, 4);
		memcpy(&shaderVersion, info + 0x44, 4);
		testCheck(msaa == 4 && info[0xac] == 1 && gpuCores == gGpuCoreCount && info[0xcd] == 1 && info[0xb5] == 0 && shaderVersion == 0x20002,
		          "parseDeviceInfo (mirror): MSAASamples 4, GpuCoreCount of the card, DeserializerVersion undefined, shader version defaulted to 2.2");
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

	// one display a head when the heads are the paravirtual displays (pvscanout.hpp)
	if (PvScanout::enabled()) {
		const uint32_t displays = PvScanout::count();

		memcpy(static_cast<uint8_t *>(mBar0) + kCtrl + kRegNumDisplays, &displays, sizeof(displays));
	}

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
	copy[kSlotPowerStateWillChangeToGated] = reinterpret_cast<void *>(&tPowerGated);
	*reinterpret_cast<void ***>(this) = copy;   // a published nub is never released, so the copy is never orphaned; publish() frees it if attach fails
	return copy;
}

bool RDNA4PvNub::publish(IOService *parent, uint32_t level) {
	if (!parent || !level)
		return false;
	sysctl_register_oid(&sysctl__debug_rdna4_pvtalk);
	sysctl_register_oid(&sysctl__debug_rdna4_pvinplace);
	sysctl_register_oid(&sysctl__debug_rdna4_pvpresent);
	sysctl_register_oid(&sysctl__debug_rdna4_pvprofile);
	sysctl_register_oid(&sysctl__debug_rdna4_pvtexid);
	sysctl_register_oid(&sysctl__debug_rdna4_pvtex);
	sysctl_register_oid(&sysctl__debug_rdna4_pvframeport);
	sysctl_register_oid(&sysctl__debug_rdna4_pvframe);
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

	IOPCIDevice *gpu = OSDynamicCast(IOPCIDevice, parent);
	if (gpu && gpu->configRead16(kIOPCIConfigVendorID) == 0x1002) {
		const uint16_t device = gpu->configRead16(kIOPCIConfigDeviceID);
		const uint8_t revision = gpu->configRead8(kIOPCIConfigRevisionID);

		gGpuCoreCount = Sku::computeUnits(device, revision);
	}

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
		// Nor one whose work cannot run: Metal would see a device whose every command buffer "completes" without the card (rig, 2026-10-22:
		// stage 2 failed, all mtlprobe checks wrong).
		if (!gCompute || !gCompute->runtimeReady()) {
			pvlog("not publishing after the delay: the compute runtime is not up");
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
void RDNA4PvNub::setSurfaceFunctions(const mach_vm_address_t fn[kSurfaceFunctions]) { memcpy(&gSurfaceFns, fn, sizeof(gSurfaceFns)); }

int RDNA4PvHost::dumpTexture(uint32_t task, uint32_t id, struct sysctl_req *req)
{
	TextureSource t;
	const bool found = textureSource(task, id, t);   // not found: the header, the record and the descriptor only, to see why

	const TextureFormat *fm = textureFormat(t.format);
	const uint32_t bpp = fm ? fm->bpp : 4;
	const uint32_t head[4] = { t.width, t.height, t.format, bpp };
	const uint32_t rowBytes = found ? t.width * bpp : 0;
	uint32_t rec[3] = {}, desc[29] = {};

	readGuest(task, 0x1000 + 12ull * id, reinterpret_cast<uint8_t *>(rec), sizeof(rec));
	readGuest(task, rec[1], reinterpret_cast<uint8_t *>(desc), sizeof(desc));

	if (rowBytes > 0x10000)
		return EINVAL;

	auto *row = static_cast<uint8_t *>(IOMalloc(rowBytes ? rowBytes : 1));

	if (!row)
		return ENOMEM;

	int err = SYSCTL_OUT(req, head, sizeof(head));

	err = err ? err : SYSCTL_OUT(req, rec, sizeof(rec));
	err = err ? err : SYSCTL_OUT(req, desc, sizeof(desc));

	for (uint32_t y = 0; rowBytes && y < t.height && !err; y++) {
		if (!readTextureRow(task, t, y, row, rowBytes))
			memset(row, 0, rowBytes);

		err = SYSCTL_OUT(req, row, rowBytes);
	}

	IOFree(row, rowBytes ? rowBytes : 1);

	return err;
}

void RDNA4PvNub::setPhysmap(uint8_t *base, uint8_t *max)
{
	gPhysmap = base;
	gPhysmapEnd = max;
}


bool RDNA4PvNub::raiseInterrupt(int source, bool offToo) {
	if (source < 0 || source >= kMaxInterrupts)
		return false;
	IOInterruptAction h;
	OSObject *t;
	void *r;
	IOLockLock(mLock);
	const bool ok = mInt[source].registered && (mInt[source].enabled || offToo);
	// Like a level interrupt: the status bits stay set until the driver clears
	// them, so a raise while its event source has the line disabled (it does
	// while its work loop runs) is not lost. Dropping it stalled Apple's driver
	// after 16 command buffers (card, Sequoia, 2026-11-03).
	if (mInt[source].registered && !mInt[source].enabled)
		mInt[source].pending = true;
	h = mInt[source].handler;
	t = mInt[source].target;
	r = mInt[source].refCon;
	IOLockUnlock(mLock);
	if (ok && h)
		h(t, r, this, source);
	return ok;
}

bool RDNA4PvNub::interruptBusy(int source) {
	bool busy;

	if (source < 0 || source >= kMaxInterrupts)
		return false;

	IOLockLock(mLock);
	busy = mInt[source].registered && (!mInt[source].enabled || mInt[source].pending);
	IOLockUnlock(mLock);

	return busy;
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
	const bool pending = mInt[source].pending;
	mInt[source].pending = false;
	IOLockUnlock(mLock);
	if (pending)
		raiseInterrupt(source);
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
	gTr.gaveUp = false;
	gTr.helpers += mHelper;
	IOLockWakeup(gTr.lock, &gTr.helper, false);
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
