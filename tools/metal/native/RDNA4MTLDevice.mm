/*
 * RDNA4MTLDevice.m
 * AMDRadeonRDNA4
 *
 * The native-Metal spike's driver bundle (docs/native-metal-spike.md): the class Metal makes for our IOAccelerator service
 * (src/accelcensus.cpp, level 3: MetalPluginName "AMDRadeonRDNA4MTLDriver", MetalPluginClassName "RDNA4MTLDevice").
 *
 * Metal's loader (Tahoe 26.6.2, x86_64, read from the shared cache): getMetalPluginClassForService reads MetalPluginName,
 * gpu_bundle_find_trusted (libsystem_sandbox) finds <name>.bundle in /Library/GPUBundles (trusted: SIP filesystem protection off) or
 * /System/Library/Extensions, the class is MetalPluginClassName's (else the principal class) and must be a subclass of _MTLDevice; then
 * -[MTLIOAccelService initWithAcceleratorPort:deviceClass:] makes the device with [[class alloc] initWithAcceleratorPort:service].
 *
 * Gate 1: the device exists. Metal sends it MTLIOAccelDevice's private messages (_setAcceleratorService: right after the init,
 * _removeRequested on safe eject): those are answered here. Every class here logs a message it does not answer once
 * ("RDNA4MTL: <class> does not implement <selector>") and returns zeros of the protocol's return type, so the spike lists what is missing.
 *
 * Gate 2: buffers and compute. The device drives the card through the kext's runtime client (librdna4, RDNA4ComputeService: root only,
 * a GPU address space of the client's own):
 *   - buffers: rdna4_alloc_host, snooped system memory mapped into the task and the GPU (shared storage);
 *   - libraries: a metallib (file, URL or data), kept as it is; functions are names in it;
 *   - compute pipelines: the function translated from AIR by the translator in file mode (/usr/local/libexec/rdna4-air2amdgpu, or
 *     $RDNA4_AIR2AMDGPU) into a code object, loaded with rdna4_load; kernargs are the translator's ABI (tools/metal/m3/air2amdgpu.cpp);
 *   - command buffers run their dispatches in order, one rdna4_dispatch each (gate 8: on the device's work queue).
 * Gate 3: a render pass. Textures are linear 2D RGBA8 / BGRA8 in shared memory; a render pipeline's vertex and fragment functions are
 * translated into RGFX blobs (air2amdgpu.cpp translateShader); each draw is the PV host's (src/pvgpu.cpp runDraw), cut down: the proven
 * triangle stream (userspace/rdna4-run/gfx12tri.h, gfx12tricol.h with values between the stages) with the shaders' registers, target,
 * viewport and draw patched in, the stages' buffer tables in a state page, submitted as the client's own gfx IB (rdna4_submit_gfx_ib, a
 * boot with rdna4-gfx=2). A clear load action is done by the CPU (shared memory).
 * ponytail: one host allocation per set*Bytes.
 *
 * Gate 8: a queue. commit returns at once; one serial work queue per device runs the committed command buffers in commit order, calls
 * their scheduled and completed handlers and presents their drawables. Draws go out behind each other without waiting, each with its own
 * IB page and state page out of kSlots; anything else (a dispatch, a CPU clear, blit or build) first waits for the draws before it
 * (drainGfx), and so does the end of a command buffer. Blits are the CPU's (shared memory).
 * ponytail: one IB per draw (two rdna4_write calls and a submit each, ~64 us); one IB for a command buffer's draws when that matters.
 *
 * Gate 7: libraries from source. Metal's own compiler makes them for this device (_MTLDevice's newLibraryWithSource: and the rest, not
 * overridden): their functions (_MTLFunctionInternal) give their AIR as -bitcodeData, which the translator takes as it takes a metallib.
 *
 * Gate 9: on the screen. IOSurfaces (CAMetalLayer's drawables) and newBufferWithBytesNoCopy: memory are the app's own pages, mapped into
 * its GPU address space by the kext (rdna4_import_host); a command buffer presents its drawables once its work has run.
 *
 * Gate 10: 3D. Depth32Float textures in VRAM (64KB_2D, cleared by a copy of the value), the depth test and write; textures (linear RGBA8 /
 * BGRA8) and samplers in shaders, as T#s and S#s in the stages' tables; indexed and instanced draws.
 *
 * Gate 5: ray queries. Acceleration structures of triangle geometry are the device's own format (a stackless BVH, built by the CPU
 * as a command buffer's step, below); a kernel's intersect is translated into tools/metal/m3/rtlib.c's traversal of it.
 * ponytail: primitive structures only (no instances), no refit, compaction, masks, cull modes or intersection functions.
 *
 * The device exists only in processes started with RDNA4_NATIVE_MTL=1 (the init returns nil elsewhere, which Metal skips): published
 * for every process, it was made in ControlCenter and the like, which crashed on the first missing message (2026-11-19).
 */

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
@interface NSObject (RDNA4MetalPrivate)
- (id)newSerializedDescriptor;   // MTLVertexDescriptorInternal's (Metal.framework)
@end
#include <malloc/malloc.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <dlfcn.h>
#include <unordered_map>
#include <map>
#include <spawn.h>
#include <sys/wait.h>
#include <crt_externs.h>
#include <execinfo.h>
#import <os/log.h>
#include <atomic>
#include <mutex>
#include <sys/sysctl.h>
#include <vector>

// NSLog's %@ arguments are <private> in the unified log (WindowServer's messages were unreadable): the text, formatted, as public
#define RLOG(...) os_log(OS_LOG_DEFAULT, "%{public}s", [NSString stringWithFormat:__VA_ARGS__].UTF8String)
// The first `n` of a kind, logged ("RDNA4MTL trace: ..."), to see what a process (WindowServer) does with the device.
#define RTRACE(n, ...) do { static int seen_; if (__atomic_fetch_add(&seen_, 1, __ATOMIC_RELAXED) < (n)) RLOG(@"RDNA4MTL trace: " __VA_ARGS__); } while (0)

extern "C" {
#include "librdna4.h"
}
// Where WindowServer's time goes (window drags were slow, 2026-11-20): totals, logged every 5 s while there is work ("RDNA4MTL stats: ...").
enum RDNA4Stat { kStatBuffers, kStatDraws, kStatDrawUs, kStatDrainUs, kStatShadowBytes, kStatShadowUs, kStatSelfReads, kStatSelfReadBytes,
	             kStatDispatches, kStatDispatchUs, kStatSlotUs, kStatBuildUs, kStatWriteUs, kStatSubmitUs, kStatBarriers, kStatWaitUs, kStatRunUs, kStatEncodeUs, kStatRecordUs, kStatAllocs, kStatAllocUs, kStatFreeUs, kStatImports, kStatImportBytes, kStatAllocBytes, kStatSteps, kStatStepUs, kStatDrawnSurface, kStatDrawnOwn, kStatHeldIn, kStatHeldOut, kStatFramed, kStatVblMade, kStatVblCommitted, kStatVblDone, kStatMade0, kStatMade1, kStatMade2, kStatMade3, kStatMade4, kStatMade5, kStatCount };
static std::atomic<uint64_t> gStats[kStatCount], gLatencyMaxUs, gEncodeMaxUs, gStepMaxUs;   // gLatencyMaxUs: the longest commit to done since the last report
static std::atomic<int64_t> gHostLive, gHostCount, gOrderWaiting, gShadowLive, gShadowCount, gTexturesLive, gBuffersLive;   // gauges: host memory allocated (bytes, buffers), command buffers held back
static uint64_t nowUs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000; }
static void statAdd(RDNA4Stat s, uint64_t v) { gStats[s] += v; }
static void statMax(std::atomic<uint64_t> &m, uint64_t v)
{
	for (uint64_t was = m; v > was && !m.compare_exchange_weak(was, v);)
		;
}
struct StatTime {   // the time until the end of the scope, added to a stat
	RDNA4Stat stat;
	uint64_t t0 = nowUs();
	~StatTime() { statAdd(stat, nowUs() - t0); }
};
static void statReport(void)
{
	static std::atomic<uint64_t> last;
	const uint64_t now = nowUs();
	uint64_t then = last;
	if (!then) {
		last = now;
		return;
	}
	if (now - then < 5000000 || !last.compare_exchange_strong(then, now))
		return;
	uint64_t v[kStatCount];
	for (int i = 0; i < kStatCount; i++)
		v[i] = gStats[i].exchange(0);
	const uint64_t worst = gLatencyMaxUs.exchange(0), worstEncode = gEncodeMaxUs.exchange(0), worstStep = gStepMaxUs.exchange(0), n = v[kStatBuffers] ?: 1;
	const double sec = (now - then) / 1e6;
	RLOG(@"RDNA4MTL stats: %.1f s: %llu command buffers, %llu draws (%.1f ms submitting), %.1f ms waiting for draws, shadows %.1f MB (%.1f ms), "
	     @"self-reads %llu %.1f MB copied on the card, %llu barriers on the card, %llu dispatches (%.1f ms); a draw's slot wait %.1f ms, build %.1f, writes %.1f, submit %.1f",
	     sec, v[kStatBuffers], v[kStatDraws], v[kStatDrawUs] / 1e3, v[kStatDrainUs] / 1e3, v[kStatShadowBytes] / 1e6, v[kStatShadowUs] / 1e3,
	     v[kStatSelfReads], v[kStatSelfReadBytes] / 1e6, v[kStatBarriers], v[kStatDispatches], v[kStatDispatchUs] / 1e3, v[kStatSlotUs] / 1e3,
	     v[kStatBuildUs] / 1e3, v[kStatWriteUs] / 1e3, v[kStatSubmitUs] / 1e3);
	RLOG(@"RDNA4MTL stats: a command buffer from commit to done %.2f ms (%.2f waiting for the queue, %.2f running), at most %.2f ms; "
	     @"made to committed %.2f ms (%.2f in its draw calls), at most %.2f", (v[kStatWaitUs] + v[kStatRunUs]) / 1e3 / n, v[kStatWaitUs] / 1e3 / n,
	     v[kStatRunUs] / 1e3 / n, worst / 1e3, v[kStatEncodeUs] / 1e3 / n, v[kStatRecordUs] / 1e3 / n, worstEncode / 1e3);
	RLOG(@"RDNA4MTL stats: drawn (covered) %.1f MB into IOSurfaces, %.1f MB into the device's own textures; surfaces held in VRAM: %.1f MB "
	     @"filled, %.1f MB written back", v[kStatDrawnSurface] / 1e6, v[kStatDrawnOwn] / 1e6, v[kStatHeldIn] / 1e6, v[kStatHeldOut] / 1e6);
	if (v[kStatFramed])
		RLOG(@"RDNA4MTL stats: from display 0's VBL event (%llu command buffers within 20 ms of one): made %.2f ms, committed %.2f, done %.2f; "
		     @"made under 1, 2, 4, 6, 8 ms and later: %llu/%llu/%llu/%llu/%llu/%llu", v[kStatFramed], v[kStatVblMade] / 1e3 / v[kStatFramed],
		     v[kStatVblCommitted] / 1e3 / v[kStatFramed], v[kStatVblDone] / 1e3 / v[kStatFramed], v[kStatMade0], v[kStatMade1], v[kStatMade2],
		     v[kStatMade3], v[kStatMade4], v[kStatMade5]);
	RLOG(@"RDNA4MTL stats: %llu steps other than draws (%.1f ms, the longest %.2f ms)", v[kStatSteps], v[kStatStepUs] / 1e3, worstStep / 1e3);
	RLOG(@"RDNA4MTL stats: %llu buffers allocated (%.1f MB), %llu imported (%.1f MB): %.1f ms, freeing %.1f ms", v[kStatAllocs] - v[kStatImports],
	     v[kStatAllocBytes] / 1e6, v[kStatImports], v[kStatImportBytes] / 1e6, v[kStatAllocUs] / 1e3, v[kStatFreeUs] / 1e3);
	RLOG(@"RDNA4MTL stats: host memory %.1f MB in %lld buffers (shadows %.1f MB in %lld), %lld command buffers held back for one enqueued before them; "
	     @"%lld textures, %lld command buffers alive", gHostLive / 1e6, (long long)gHostCount, gShadowLive / 1e6, (long long)gShadowCount,
	     (long long)gOrderWaiting, (long long)gTexturesLive, (long long)gBuffersLive);
}

#include "pvdraw.hpp"
#include "pm4.hpp"
#include "gfx12tri.h"
#include "gfx12tricol.h"
#include "gfx12trirun.h"
#include "rdna4mtlc.h"
#include "mtlkernels.h"

// The return-type-correct signature of a protocol method the class adopts (protocols it adopts included), else nil.
static const char *protocolTypes(Protocol *p, SEL sel)
{
	for (int required = 1; required >= 0; required--) {
		struct objc_method_description d = protocol_getMethodDescription(p, sel, required, YES);
		if (d.types)
			return d.types;
	}
	unsigned n = 0;
	Protocol *__unsafe_unretained *inner = protocol_copyProtocolList(p, &n);
	const char *types = NULL;
	for (unsigned i = 0; i < n && !types; i++)
		types = protocolTypes(inner[i], sel);
	free(inner);
	return types;
}

static NSMethodSignature *missingSignature(Class cls, SEL sel)
{
	const char *types = NULL;
	for (Class c = cls; c && !types; c = class_getSuperclass(c)) {
		unsigned n = 0;
		Protocol *__unsafe_unretained *list = class_copyProtocolList(c, &n);
		for (unsigned i = 0; i < n && !types; i++)
			types = protocolTypes(list[i], sel);
		free(list);
	}
	return [NSMethodSignature signatureWithObjCTypes:types ?: "@@:"];
}

static void missingCall(id obj, NSInvocation *inv)
{
	static NSMutableSet *logged;
	NSString *name = [NSString stringWithFormat:@"%@ does not implement %@", [obj class], NSStringFromSelector(inv.selector)];
	BOOL first;
	@synchronized ([NSObject class]) {
		if (!logged)
			logged = [NSMutableSet set];
		first = ![logged containsObject:name];
		[logged addObject:name];
	}
	if (first)
		RLOG(@"RDNA4MTL: %@", name);
	NSUInteger len = inv.methodSignature.methodReturnLength;
	if (len) {
		void *zero = calloc(1, len);
		[inv setReturnValue:zero];
		free(zero);
	}
}

#define RDNA4_ANSWER_MISSING                                                                                                               \
	-(NSMethodSignature *)methodSignatureForSelector : (SEL)sel                                                                         \
	{                                                                                                                                  \
		return [super methodSignatureForSelector:sel] ?: missingSignature([self class], sel);                                         \
	}                                                                                                                                  \
	-(void)forwardInvocation : (NSInvocation *)inv                                                                                     \
	{                                                                                                                                  \
		missingCall(self, inv);                                                                                                        \
	}

@interface _MTLDevice : NSObject   // Metal's own base class (exported by Metal.framework)
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error;
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error;
@end

@interface RDNA4MTLDevice : _MTLDevice <MTLDevice>   // MTLAddDevice asserts conformsToProtocol:@protocol(MTLDevice)
- (instancetype)initWithAcceleratorPort:(io_service_t)service;         // nil unless RDNA4_NATIVE_MTL is set
- (instancetype)initNativeWithAcceleratorPort:(io_service_t)service;   // whatever the environment (the router's choice)
- (rdna4_t *)gpu;   // nil until the kext's runtime client is open
- (NSError *)drainGfx;   // waits for the draws submitted so far (the work queue)
// A resource's gpuResourceID (argument buffers, MoltenVK's descriptor sets): the address of a 32-byte slot holding its T# or S#, which the
// translated shader loads as it loads a bound one's (air2amdgpu.cpp). handle 0: a new slot (0 back: none left); `words` written into it.
- (uint64_t)handle:(uint64_t)handle words:(const uint32_t *)words count:(unsigned)count;
- (void)freeHandle:(uint64_t)handle;
- (void)spareImage:(uint32_t[8])t;   // the T# of an unbound texture slot: one texel of the zeroed spare buffer
- (uint64_t)spareHandle;              // a T# slot holding that T# (an unbound texture handle)
@end

static NSError *rdna4Error(NSString *what, kern_return_t kr)
{
	NSString *text = kr ? [NSString stringWithFormat:@"%@: %s (0x%x)", what, rdna4_error(kr), kr] : what;
	RLOG(@"RDNA4MTL: %@", text);
	return [NSError errorWithDomain:@"RDNA4MTL" code:kr userInfo:@{ NSLocalizedDescriptionKey : text }];
}

// ---- buffers

@interface RDNA4MTLBuffer : NSObject <MTLBuffer>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device length:(NSUInteger)length options:(MTLResourceOptions)options;
// The caller's own page-aligned memory (newBufferWithBytesNoCopy:, an IOSurface's), mapped into the GPU (rdna4_import_host), not copied.
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device bytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options
                   deallocator:(void (^)(void *, NSUInteger))deallocator;
- (void)keepOwner:(void (^)(void))release;   // called when the buffer goes
- (void)countAsShadow;                         // (the stats' gauge)
@property (readonly) BOOL imported, readOnly;   // the caller's memory; mapped for the GPU to read only
@end

// Host memory freed, kept 2 s for an allocation of the same size (at most kPoolBytes): Core Animation makes and drops a few textures and
// buffers a frame, each allocation and free a kext call of about 1 ms (2026-11-20). Handed out zeroed, as new memory is.
namespace {
struct Pooled { rdna4_t *gpu; rdna4_buffer_t buf; void *cpu; uint64_t bytes; CFAbsoluteTime at; };
constexpr uint64_t kPoolBytes = 128ull << 20;
std::mutex gPoolLock;
std::vector<Pooled> gPool;
uint64_t gPooledBytes;

// The entries older than 2 s out of the pool, freed by the caller (outside the lock).
void poolExpired(std::vector<Pooled> &out, CFAbsoluteTime now)
{
	for (size_t i = 0; i < gPool.size();)
		if (now - gPool[i].at > 2) {
			gPooledBytes -= gPool[i].bytes;
			out.push_back(gPool[i]);
			gPool[i] = gPool.back();
			gPool.pop_back();
		} else {
			i++;
		}
}

void poolFree(std::vector<Pooled> &freed)
{
	for (Pooled &p : freed)
		rdna4_free(p.gpu, &p.buf);
}

// (diagnostic, RDNA4_ALLOC_TRACE=n: every nth host buffer's maker logged, with the live buffers by length; RDNA4_ALLOC_LEN=bytes: only
// those of that length. Mixxx's leak, 2026-11-21)
void allocTrace(NSUInteger length, int change)
{
	static const int every = getenv("RDNA4_ALLOC_TRACE") ? atoi(getenv("RDNA4_ALLOC_TRACE")) : 0;
	static const unsigned long only = getenv("RDNA4_ALLOC_LEN") ? strtoul(getenv("RDNA4_ALLOC_LEN"), nullptr, 0) : 0;   // (or those of one length)
	static std::mutex lock;
	static std::map<NSUInteger, int64_t> live;
	static int made;
	if (every <= 0)
		return;
	std::lock_guard<std::mutex> hold(lock);
	live[length] += change;
	if (change < 0 || (only && length != only) || ++made % every)
		return;
	void *frames[16];
	const int n = backtrace(frames, 16);
	char **names = backtrace_symbols(frames, n);
	NSMutableString *stack = [NSMutableString string], *lengths = [NSMutableString string];
	for (int i = 2; i < n; i++)
		[stack appendFormat:@" | %s", names[i] + 4];
	free(names);
	for (auto &e : live)
		if (e.second > 20)
			[lengths appendFormat:@" %lu:%lld", (unsigned long)e.first, (long long)e.second];
	RLOG(@"RDNA4MTL: buffer %d of %lu bytes (live by length%@):%@", made, (unsigned long)length, lengths, stack);
}

bool poolTake(rdna4_t *gpu, uint64_t bytes, rdna4_buffer_t *buf, void **cpu)
{
	std::vector<Pooled> freed;
	bool found = false;
	{
		std::lock_guard<std::mutex> hold(gPoolLock);
		poolExpired(freed, CFAbsoluteTimeGetCurrent());
		for (size_t i = 0; i < gPool.size() && !found; i++)
			if (gPool[i].gpu == gpu && gPool[i].bytes == bytes) {
				*buf = gPool[i].buf, *cpu = gPool[i].cpu;
				gPooledBytes -= bytes;
				gPool[i] = gPool.back();
				gPool.pop_back();
				found = true;
			}
	}
	poolFree(freed);
	if (found)
		memset(*cpu, 0, bytes);
	return found;
}

void poolPut(rdna4_t *gpu, rdna4_buffer_t buf, void *cpu, uint64_t bytes)
{
	const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	std::vector<Pooled> freed;
	{
		std::lock_guard<std::mutex> hold(gPoolLock);
		poolExpired(freed, now);
		if (gPooledBytes + bytes <= kPoolBytes) {
			gPool.push_back({ gpu, buf, cpu, bytes, now });
			gPooledBytes += bytes;
		} else {
			freed.push_back({ gpu, buf, cpu, bytes, now });
		}
	}
	poolFree(freed);
}
}   // namespace

static void deviceIdle(RDNA4MTLDevice *device);   // (below: the device's work queue declared first)

@implementation RDNA4MTLBuffer {
	RDNA4MTLDevice *_device;
	rdna4_buffer_t _buf;
	void *_cpu;
	NSUInteger _length;
	MTLResourceOptions _options;
	void (^_deallocator)(void *, NSUInteger);   // an imported buffer's owner's
	void (^_owner)(void);
	uint64_t _allocated;   // host memory it allocated (the gauge)
	BOOL _shadow;
}
- (BOOL)isComplete { deviceIdle(_device); return YES; }
- (void)waitUntilComplete { deviceIdle(_device); }   // (Sublime Text's GL)

- (void)keepOwner:(void (^)(void))release { _owner = release; }
- (void)countAsShadow
{
	_shadow = YES;
	gShadowLive += _allocated, gShadowCount += 1;
}
@synthesize label;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device bytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options
                   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	if (!(self = [super init]) || !device.gpu)
		return nil;
	const uint64_t bytes = ((length ?: 1) + 4095) & ~4095ull;
	const StatTime timed { kStatAllocUs };
	statAdd(kStatAllocs, 1), statAdd(kStatImports, 1), statAdd(kStatImportBytes, bytes);
	kern_return_t kr = ((uintptr_t)pointer & 4095) ? kIOReturnBadArgument : rdna4_import_host(device.gpu, pointer, bytes, 0, &_buf);
	// memory this process may only read (Core Animation's shared with WindowServer): for the GPU to read, so its updates are seen (copied,
	// the menu bar's app menus never changed after the first)
	if (kr == KERN_PROTECTION_FAILURE && !(kr = rdna4_import_host(device.gpu, pointer, bytes, RDNA4_HOST_READ_ONLY, &_buf)))
		_readOnly = YES;
	_imported = !kr;
	if (kr) {
		rdna4Error([NSString stringWithFormat:@"%lu bytes at %p into the GPU", (unsigned long)length, pointer], kr);
		return nil;
	}
	_cpu = pointer;
	_device = device;
	_length = length;
	_options = options;
	_deallocator = deallocator;
	return self;
}

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device length:(NSUInteger)length options:(MTLResourceOptions)options
{
	if (!(self = [super init]) || !device.gpu)
		return nil;
	const uint64_t bytes = ((length ?: 1) + 4095) & ~4095ull;
	const StatTime timed { kStatAllocUs };
	statAdd(kStatAllocs, 1), statAdd(kStatAllocBytes, bytes);
	const kern_return_t kr = poolTake(device.gpu, bytes, &_buf, &_cpu) ? KERN_SUCCESS : rdna4_alloc_host(device.gpu, bytes, &_buf, &_cpu);
	if (kr) {
		rdna4Error([NSString stringWithFormat:@"a buffer of %lu bytes", (unsigned long)length], kr);
		return nil;
	}
	gHostLive += bytes, gHostCount += 1;
	allocTrace(length, +1);
	_allocated = bytes;
	_device = device;
	_length = length;
	_options = options;
	return self;
}

- (void)dealloc
{
	if (_cpu) {
		const StatTime timed { kStatFreeUs };
		if (_allocated)
			poolPut(_device.gpu, _buf, _cpu, _allocated);
		else
			rdna4_free(_device.gpu, &_buf);   // an imported range unmapped and unwired, not freed
	}
	gHostLive -= _allocated, gHostCount -= _allocated ? 1 : 0;
	if (_allocated)
		allocTrace(_length, -1);
	if (_shadow)
		gShadowLive -= _allocated, gShadowCount -= 1;
	if (_deallocator)
		_deallocator(_cpu, _length);
	if (_owner)
		_owner();
}


- (id<MTLDevice>)device { return _device; }
- (NSUInteger)length { return _length; }
- (void *)contents { return _cpu; }
- (uint64_t)gpuAddress { return _buf.gpu; }
- (MTLResourceOptions)resourceOptions { return _options; }
// the mode it was made with (its memory is host memory the card reads either way): Apple's GL-on-Metal renderer returns a buffer to the
// pool of its storageMode, and answered Shared, its Private index buffers went to the Shared pool, never taken again: Mixxx made a new
// one every frame until the client's buffer quota ran out (2026-11-21)
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift); }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (NSUInteger)allocatedSize { return (NSUInteger)_buf.bytes; }
- (void)didModifyRange:(NSRange)range { }   // shared and snooped: nothing to flush
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }

RDNA4_ANSWER_MISSING
@end

// ---- textures

static BOOL isBgra(MTLPixelFormat f) { return f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB; }

// What a colour attachment may be: the formats src/pvdraw.hpp colourInfo knows (as the PV host's targets).
static BOOL isTargetFormat(MTLPixelFormat f)
{
	switch (f) {
	case 70: case 71: case 80: case 81: case 115: case 110: case 65: case 10: case 25: case 90: case 94: case 103: case 104: case 63: case 53:
	case 55: case 105: case 125: case 92: case 30:
		return YES;
	default: return NO;
	}
}

// An IOSurface that WindowServer draws into (a display's frame: textures made as render targets), held in VRAM (rdna4-nativeheld=1, off
// by default): drawn and read there, and presented from there by the kext (rdna4_present_source). The rows drawn are written back into the
// surface when WindowServer goes idle, at least each second, and before a step of the CPU; the copy is filled from the surface again when
// its seed moved (the CPU wrote it) or the CPU touched it here. Drawn into in host memory, a blended 4K quad took 0.92 ms on the card, in
// VRAM 0.17 ms (2026-11-20); written back after every command buffer instead of presented from VRAM, the write-backs (3 GB/s) made it
// slower. One per surface, the same layout as the surface, decided when a texture is made (all its draws agree).
@interface RDNA4Held : NSObject
@property (readonly) uint64_t gpu;   // the surface's first byte, in VRAM
- (NSError *)sync;                   // (work queue) the VRAM copy current before a use on the card
- (void)dirty:(uint64_t)lo to:(uint64_t)hi;   // (work queue) bytes of the surface drawn in VRAM
- (BOOL)writeBack:(NSError **)why;    // (work queue) the drawn bytes into the surface (on the card, not waited for): whether any were
- (void)present:(BOOL)on;             // the kext presents the surface's frames from the VRAM copy (or from its memory again)
@property (readonly) uint32_t surfaceID;
- (void)cpuTouched;                   // the CPU may have written the surface: filled again before the next use
@end
static RDNA4Held *heldFor(IOSurfaceRef surface, BOOL make, RDNA4MTLDevice *device, RDNA4MTLBuffer *host);

// MTLTextureSwizzleChannels as RDNA4MTLTexture's _swizzle (pvdraw.hpp viewSel): red .. alpha a byte each
static uint32_t packSwizzle(MTLTextureSwizzleChannels s) { return s.red | s.green << 8 | s.blue << 16 | (uint32_t)s.alpha << 24; }

// BC1-BC3 (DXT1-5: Unity's textures, Daggerfall Unity's title and sky) kept as RGBA8 (sRGB for the sRGB ones: Metal's odd formats 131,
// 133, 135) and decoded on the CPU as their blocks come in (replaceRegion, the buffer-to-texture blits through it). The bytes of a block,
// 0: not one of them. ponytail: 4 bytes a texel in memory where the card could sample the blocks (IMG_FORMAT BC1-3) at 0.5-1.
static uint32_t bcBlockBytes(MTLPixelFormat f) { return f == 130 || f == 131 ? 8 : f >= 132 && f <= 135 ? 16 : 0; }

// A BC1 colour block's 4x4 texels as RGBA8 (`fourColour`: BC2's and BC3's, never the 3-colour mode with transparent black).
static void bcColour(const uint8_t *b, bool fourColour, uint8_t out[16][4])
{
	const uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
	uint8_t pal[4][4];
	for (int k = 0; k < 2; k++) {
		const uint32_t c = k ? c1 : c0;
		pal[k][0] = (uint8_t)((c >> 11) * 255 / 31), pal[k][1] = (uint8_t)((c >> 5 & 63) * 255 / 63), pal[k][2] = (uint8_t)((c & 31) * 255 / 31);
		pal[k][3] = 255;
	}
	for (int ch = 0; ch < 3; ch++) {
		if (fourColour || c0 > c1) {
			pal[2][ch] = (uint8_t)((2 * pal[0][ch] + pal[1][ch] + 1) / 3);
			pal[3][ch] = (uint8_t)((pal[0][ch] + 2 * pal[1][ch] + 1) / 3);
		} else {
			pal[2][ch] = (uint8_t)((pal[0][ch] + pal[1][ch] + 1) / 2);
			pal[3][ch] = 0;
		}
	}
	pal[2][3] = 255, pal[3][3] = fourColour || c0 > c1 ? 255 : 0;
	const uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
	for (int i = 0; i < 16; i++)
		memcpy(out[i], pal[idx >> (2 * i) & 3], 4);
}

// One block (`bytes` 8: BC1, 16: BC2 if `explicitAlpha`, else BC3) as 4x4 RGBA8 texels, row by row.
static void bcBlock(const uint8_t *b, uint32_t bytes, bool explicitAlpha, uint8_t out[16][4])
{
	if (bytes == 8) {
		bcColour(b, false, out);
		return;
	}
	bcColour(b + 8, true, out);
	if (explicitAlpha) {   // BC2: 4 bits a texel
		for (int i = 0; i < 16; i++)
			out[i][3] = (uint8_t)((b[i / 2] >> (4 * (i & 1)) & 15) * 17);
		return;
	}
	uint8_t a[8] = { b[0], b[1] };   // BC3: two end points, 3-bit indices
	for (int k = 2; k < 8; k++)
		a[k] = b[0] > b[1] ? (uint8_t)(((8 - k) * b[0] + (k - 1) * b[1] + 3) / 7)
		       : k < 6 ? (uint8_t)(((6 - k) * b[0] + (k - 1) * b[1] + 2) / 5) : k == 6 ? 0 : 255;
	uint64_t bits = 0;
	for (int k = 0; k < 6; k++)
		bits |= (uint64_t)b[2 + k] << (8 * k);
	for (int i = 0; i < 16; i++)
		out[i][3] = a[bits >> (3 * i) & 7];
}

@interface RDNA4MTLTexture : NSObject <MTLTexture>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor;
// Over an IOSurface (CAMetalLayer's drawables): its memory mapped into the GPU, its rows as the surface has them.
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)surface
                         plane:(NSUInteger)plane;
- (void)clearTo:(MTLClearColor)colour;
// The clear as a 32-bit pattern over `bytes` from gpuBase (a 2D colour texture whose pixel is that pattern repeated), for the card to fill
- (BOOL)fillFor:(MTLClearColor)colour pattern:(uint32_t *)pattern bytes:(uint64_t *)bytes;
// A view (newTextureViewWithPixelFormat:...): the same memory from level `levels.location` on, read as `format` (of the same size a pixel).
- (instancetype)initViewOf:(RDNA4MTLTexture *)texture format:(MTLPixelFormat)format type:(MTLTextureType)type levels:(NSRange)levels
                    slices:(NSRange)slices;   // a Depth32Float texture: every pixel (the tiling does not matter for one value)
- (void)descriptor:(uint32_t[8])t;          // its T# for shaders (level 0)
- (void)descriptor:(uint32_t[8])t level:(NSUInteger)level;   // level `level` as an image of its own (explicit-LOD sampling)
- (void)refresh;                            // an IOSurface's copy for the GPU made current (rows not 128-byte multiples)
// The GPU drew into its copy (a shadowed render target): refresh leaves the copy alone until writeBack puts it into the surface (after the
// command buffer's draws). Before, the draws stayed in the copy and the next refresh put the surface's old pixels over them: WindowServer's
// menu titles strip on the 4K display (3752x60, 15040 bytes a row) read all zeros, its app menus never showed (2026-11-20).
- (void)drewIntoShadow;
- (void)writeBack;
- (void)generateMipmaps;                    // levels 1 .. from level 0 (the CPU, 2x2 boxes)
@property (readonly) RDNA4MTLTexture *readCopy;   // a texture of its size and pitch, kept: what a draw reading its own target samples
@property (readonly) uint64_t spanBytes;          // from its first row's start to its last row's end
@property (readonly) uint64_t identity;           // where its first row is, as wrote() counts: in its IOSurface, else its GPU address
- (void)wroteAll;                                // all of its memory written (wrote())
- (void)written:(uint64_t)lo to:(uint64_t)hi;    // identity addresses written (wrote()): where its copy is behind
enum { kWrittenRanges = 8 };   // ranges kept apart (more: the two nearest merged); the menu bar's rows and the Dock's are two, not all between
- (uint32_t)takeWritten:(uint64_t[kWrittenRanges][2])out from:(uint64_t)lo to:(uint64_t)hi;   // those ranges' parts in [lo, hi) (their count),
                                                                                           // forgotten: the copy is to be made of them now
- (uint32_t)sampleOr;                       // the OR of some of its pixels' first 4 bytes (the trace)
- (NSString *)origin;                       // where its pixels are (the trace)
- (RDNA4MTLTexture *)level:(NSUInteger)level;   // level `level` as a texture of its own (kept): a render pass's attachment level
@property (readonly) BOOL shadowed;         // the GPU reads a copy of an IOSurface's pixels (refresh)
@property (readonly) BOOL onSurface;        // an IOSurface's
@property (readonly) RDNA4Held *held;        // its IOSurface's pixels the GPU uses are in VRAM (rdna4-nativeheld=1)
- (void)heldRows:(uint32_t)y0 to:(uint32_t)y1 width:(uint32_t)x1;   // (work queue) rows drawn into its held copy (columns up to x1)
// A view of a buffer's memory (newLinearTextureWithDescriptor:): rows `bpr` apart from `offset`.
- (instancetype)initWithBuffer:(RDNA4MTLBuffer *)buffer descriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset
                   bytesPerRow:(NSUInteger)bpr;
@property (readonly) BOOL isDepth;        // Depth32Float, Stencil8 or both in VRAM, 64KB_2D (as the PV host's buffers), for the depth block only
@property (readonly) uint64_t depthBytes;    // a depth texture's depth (Depth32Float) from gpuBase, 0: none
@property (readonly) uint64_t stencilBytes;  // its stencil (Stencil8) after the depth, 0: none
@property (readonly) uint32_t samples;       // 2 or 4: a multisampled texture's samples, in VRAM, 64KB_2D (as the PV host's msaaSurface); else 1
@property (readonly) uint64_t gpuBase;    // the first row's GPU address (page aligned)
@property (readonly) NSUInteger rowBytes; // a multiple of 256
@property (readonly) uint32_t bpp;        // bytes a pixel
@property (readonly) uint32_t gfxFormat;  // the image format of its T#s
@property (readonly) uint8_t *bytes;      // a colour texture's first pixel (the CPU's view)
@property (readonly) RDNA4MTLBuffer *memory;   // a colour texture's pixels
@end

// The targets whose copies draws read (readCopy), with the range written since each was last copied: the card copies that only. All of a
// 4K target a pass, the copies were 4.4 GB/s on the bus while windows moved (2026-11-20). Kept by GPU address: views and textures of the
// same memory see each other's writes, by the IOSurface's ID where there is one (WindowServer makes several textures of a surface, each its
// own import at its own GPU address: by GPU address, their writes missed each other's copies, stale blocks in Safari's toolbar, 2026-11-20).
// Draws mark their scissor's rows; clears, CPU writes, compute and blits all of what they touch.
// ponytail: a write the device does not see (another process's, the CPU's through a buffer's contents) leaves its copy behind there.
// [lo, hi) into the sorted ranges r (n of them, kept apart: those that touch merged, the two nearest made one past kWrittenRanges)
static void rangesAdd(uint64_t r[kWrittenRanges][2], uint32_t *count, uint64_t lo, uint64_t hi)
{
	uint64_t t[kWrittenRanges + 1][2];
	uint32_t n = 0, k = 0;
	for (; k < *count && r[k][0] < lo; k++)
		memcpy(t[n++], r[k], sizeof t[0]);
	t[n][0] = lo, t[n++][1] = hi;
	for (; k < *count; k++)
		memcpy(t[n++], r[k], sizeof t[0]);
	uint32_t m = 0;
	for (uint32_t i = 0; i < n; i++)
		if (m && t[i][0] <= t[m - 1][1])
			t[m - 1][1] = MAX(t[m - 1][1], t[i][1]);
		else
			memcpy(t[m++], t[i], sizeof t[0]);
	if (m > kWrittenRanges) {
		uint32_t best = 0;
		for (uint32_t i = 1; i + 1 < m; i++)
			if (t[i + 1][0] - t[i][1] < t[best + 1][0] - t[best][1])
				best = i;
		t[best][1] = t[best + 1][1];
		memmove(t[best + 1], t[best + 2], (m - best - 2) * sizeof t[0]);
		m--;
	}
	memcpy(r, t, m * sizeof t[0]);
	*count = m;
}

static NSHashTable<RDNA4MTLTexture *> *gCopied;
static void wrote(uint64_t lo, uint64_t hi)
{
	if (lo >= hi)
		return;
	@synchronized([RDNA4MTLTexture class]) {
		for (RDNA4MTLTexture *t in gCopied)
			[t written:lo to:hi];
	}
}

// An IOSurface's memory mapped into the GPU, shared by the textures made of it and kept 2 s after the last was made: WindowServer makes
// textures of the same surfaces (the displays', the windows') every frame, and each import and its free were kext calls of about 1 ms,
// 2 s of every 5 while a window moved (2026-11-20). The import holds the surface.
static RDNA4MTLBuffer *importedSurface(RDNA4MTLDevice *device, IOSurfaceRef surface, void *base, size_t bytes)
{
	static NSMutableDictionary<NSNumber *, NSArray *> *kept;   // ID -> {import, last use (s)}
	static CFAbsoluteTime swept;
	const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	NSNumber *key = @(IOSurfaceGetID(surface));
	@synchronized([RDNA4MTLBuffer class]) {
		kept = kept ?: [NSMutableDictionary dictionary];
		if (now - swept > 1) {
			swept = now;
			for (NSNumber *k in kept.allKeys)
				if (now - [kept[k][1] doubleValue] > 2)
					[kept removeObjectForKey:k];
		}
		RDNA4MTLBuffer *b = kept[key][0];
		if (b && b.contents == base && b.length == bytes) {
			kept[key] = @[ b, @(now) ];
			return b;
		}
	}
	CFRetain(surface);
	RDNA4MTLBuffer *b = [[RDNA4MTLBuffer alloc] initWithDevice:device bytesNoCopy:base length:bytes options:MTLResourceStorageModeShared
	                                               deallocator:^(void *p, NSUInteger n) { CFRelease(surface); }];
	if (!b) {
		CFRelease(surface);
		return nil;
	}
	@synchronized([RDNA4MTLBuffer class]) {
		kept[key] = @[ b, @(now) ];
	}
	return b;
}

@implementation RDNA4MTLTexture {
	id<MTLBuffer> _overBuffer;   // made over a buffer (newTextureWithDescriptor:offset:bytesPerRow:): it, the offset and the row pitch given
	NSUInteger _overOffset, _overRowBytes;
	uint32_t _swizzle;   // its descriptor's or view's MTLTextureSwizzleChannels, red .. alpha a byte each; 0: none (pvdraw.hpp viewSel)
	uint64_t _written[kWrittenRanges][2];   // (gCopied) identity addresses [lo, hi), sorted, apart
	uint32_t _writtenCount;
	BOOL _shadowAhead;   // (drewIntoShadow) the copy holds newer pixels than the surface: on the root texture, not a view
	RDNA4MTLDevice *_device;
	RDNA4MTLBuffer *_memory;
	NSUInteger _width, _height;
	MTLPixelFormat _format;
	MTLTextureUsage _usage;
	IOSurfaceRef _surface;
	NSUInteger _plane, _offset;   // an IOSurface plane's, from the surface's base
	const TextureFormat *_fm;
	MTLTextureType _type;   // 2D, or 1D (one row), or a 1D array (a row a layer), or 3D
	NSUInteger _layers, _depth, _slicePitch;   // a 1D array's layers; a 3D texture's slices and the bytes from one to the next
	NSUInteger _slices;   // a 2D array's, a cube's (6) or a cube array's 2D images, laid out as a 3D texture's slices (_slicePitch)
	NSUInteger _levels, _levelOffset[16], _levelRow[16];   // a mipmapped 2D texture's levels: each an image of its own in _memory
	const uint8_t *_surfaceRows;   // shadowed: the IOSurface's own pixels, _surfaceRowBytes apart; _memory is the GPU's copy
	NSUInteger _surfaceRowBytes;
	uint32_t _shadowSeed;   // a planar surface's seed when last refreshed (_shadowSeeded)
	BOOL _shadowSeeded;
	RDNA4MTLTexture *_parent;   // a view's
	RDNA4Held *_held;
	uint64_t _handle;       // (gpuResourceID) its T#'s slot
	rdna4_buffer_t _vram;   // a depth or multisampled texture's
	uint64_t _depthBytes, _stencilBytes;
	uint32_t _samples;
}
@synthesize label;

- (instancetype)initDepthWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)d
{
	if (!(self = [super init]) || !device.gpu)
		return nil;
	// whole 256x256-pixel blocks, as the PV host's (src/pvgpu.cpp stencilFor): the depth block's 64KB_2D layout fits in them; the stencil
	// after the depth (a multiple of 64 KiB from it)
	_samples = (uint32_t)MAX(d.sampleCount, (NSUInteger)1);
	const uint64_t pixels = (uint64_t)((d.width + 255) & ~(NSUInteger)255) * ((d.height + 255) & ~(NSUInteger)255) * _samples;
	_depthBytes = d.pixelFormat == MTLPixelFormatStencil8 ? 0 : pixels * 4;
	// Depth16Unorm (Dawn's, Chrome's compositor) stored as Depth32Float: the same values in [0, 1], finer; ponytail: a CPU or blit copy would see
	// 4 bytes a pixel, none so far (depth textures have no CPU copy)
	_stencilBytes = d.pixelFormat == MTLPixelFormatDepth32Float || d.pixelFormat == MTLPixelFormatDepth16Unorm ? 0 : pixels;
	const kern_return_t kr = rdna4_alloc(device.gpu, _depthBytes + _stencilBytes, &_vram);
	if (kr) {
		rdna4Error([NSString stringWithFormat:@"a %lux%lu depth texture", (unsigned long)d.width, (unsigned long)d.height], kr);
		return nil;
	}
	_isDepth = YES;
	_type = d.textureType;
	_device = device;
	_width = d.width;
	_height = d.height;
	_format = d.pixelFormat;
	_usage = d.usage;
	_swizzle = packSwizzle(d.swizzle);
	return self;
}

- (void)descriptor:(uint32_t[8])t level:(NSUInteger)level
{
	level = MIN(level, MAX(_levels, (NSUInteger)1) - 1);
	if (!level) {
		[self descriptor:t];
		return;
	}
	const uint32_t w = (uint32_t)MAX(_width >> level, (NSUInteger)1), h = (uint32_t)MAX(_height >> level, (NSUInteger)1);
	linearImage(t, self.gpuBase + _levelOffset[level], _fm->gfx, viewSel(_fm->sel, _swizzle), w, h, (uint32_t)(_levelRow[level] / _fm->bpp));
}

- (BOOL)shadowed { return _surfaceRows != NULL; }
- (BOOL)onSurface { return self.root->_surface != NULL; }

- (NSString *)origin
{
	return [NSString stringWithFormat:@"%@%@%@ rows %lu", _surface ? [NSString stringWithFormat:@"iosurface %u plane %lu", IOSurfaceGetID(_surface), (unsigned long)_plane]
	                                                       : _parent ? @"view" : _memory.imported ? @"buffer" : @"own",
	                                 _surfaceRows ? @" shadowed" : @"", _memory.readOnly ? @" read-only" : @"", (unsigned long)(_surfaceRows ? _surfaceRowBytes : _rowBytes)];
}

- (uint32_t)sampleOr
{
	uint32_t v = 0;
	if (!_memory)
		return 0;   // (the card's memory only)
	for (NSUInteger y = 0; !_isDepth && y < _height; y += MAX(_height / 37, (NSUInteger)1))
		for (NSUInteger x = 0; x < _width; x += MAX(_width / 41, (NSUInteger)1)) {
			uint32_t w;
			memcpy(&w, self.bytes + y * _rowBytes + x * self.bpp, MIN(self.bpp, 4u));
			v |= w;
		}
	return v;
}

static char gCopyKey;
// The texture a draw reading its own target samples instead (the card copies the target into it, gpuCopyFrom:): over a buffer of the
// target's own row pitch, so that the copy is one span.
- (RDNA4MTLTexture *)readCopy
{
	RDNA4MTLTexture *copy = objc_getAssociatedObject(self, &gCopyKey);
	if (!copy) {
		// in the card's memory, not the CPU's: written and read there, not across the bus (a 4K target a pass)
		copy = [[RDNA4MTLTexture alloc] init];
		if (!_fm || rdna4_alloc(_device.gpu, _rowBytes * _height, &copy->_vram))
			return nil;
		copy->_device = _device;
		copy->_fm = _fm;
		copy->_format = _format;
		copy->_width = _width;
		copy->_height = _height;
		copy->_rowBytes = _rowBytes;
		copy->_type = MTLTextureType2D;
		objc_setAssociatedObject(self, &gCopyKey, copy, OBJC_ASSOCIATION_RETAIN);
		@synchronized([RDNA4MTLTexture class]) {
			gCopied = gCopied ?: [NSHashTable weakObjectsHashTable];
			[gCopied addObject:self];
			_written[0][0] = self.identity;   // all of it, the first time
			_written[0][1] = self.identity + self.spanBytes;
			_writtenCount = 1;
		}
	}
	return copy;
}
- (uint64_t)spanBytes { return _rowBytes * (_height - 1) + _width * self.bpp; }
- (uint64_t)identity { return _surface && !_surfaceRows ? (1ull << 63 | (uint64_t)IOSurfaceGetID(_surface) << 32) + _offset : self.gpuBase; }
- (void)wroteAll { wrote(self.identity - _offset, self.identity - _offset + (_memory ? _memory.length : _vram.bytes)); }
- (void)written:(uint64_t)lo to:(uint64_t)hi
{
	const uint64_t base = self.identity, end = base + self.spanBytes;
	lo = MAX(lo, base), hi = MIN(hi, end);
	if (lo >= hi)
		return;
	rangesAdd(_written, &_writtenCount, lo, hi);
}
- (uint32_t)takeWritten:(uint64_t[kWrittenRanges][2])out from:(uint64_t)lo to:(uint64_t)hi
{
	@synchronized([RDNA4MTLTexture class]) {
		uint64_t keep[2 * kWrittenRanges][2];   // the parts outside [lo, hi): still to be copied, by a later draw that reads them
		uint32_t n = 0, k = 0;
		for (uint32_t i = 0; i < _writtenCount; i++) {
			const uint64_t a = _written[i][0], b = _written[i][1];
			if (MAX(a, lo) < MIN(b, hi))
				out[n][0] = MAX(a, lo), out[n++][1] = MIN(b, hi);
			if (a < lo)
				keep[k][0] = a, keep[k++][1] = MIN(b, lo);
			if (b > hi)
				keep[k][0] = MAX(a, hi), keep[k++][1] = b;
		}
		_writtenCount = 0;
		for (uint32_t i = 0; i < k; i++)
			[self written:keep[i][0] to:keep[i][1]];
		return n;
	}
}

- (RDNA4MTLTexture *)root
{
	RDNA4MTLTexture *t = self;
	while (t->_parent)
		t = t->_parent;
	return t;
}
- (void)drewIntoShadow
{
	if (_surfaceRows)
		self.root->_shadowAhead = YES;
}
- (void)writeBack
{
	RDNA4MTLTexture *r = self.root;
	if (!r->_shadowAhead || !r->_surfaceRows)
		return;
	for (NSUInteger y = 0; y < r->_height; y++)
		memcpy((uint8_t *)r->_surfaceRows + y * r->_surfaceRowBytes, r.bytes + y * r->_rowBytes, r->_width * r.bpp);
	r->_shadowAhead = NO;
}

// A CPU write into a shadowed surface's copy (replaceRegion, and the blits through it) put into the surface at once. Left to the next draw's
// write-back, it never reached the surface without one, and a blit read a copy no draw had refreshed: Core Animation's partial repaints
// of Safari's tiles (2543 pixels wide, 10176 bytes a row) blitted the old tile as zeros, the page black but for what changed (2026-11-22).
// _shadowAhead stays as it was: draws later in the command buffer are still written back.
- (void)shadowToSurface
{
	RDNA4MTLTexture *r = self.root;
	for (NSUInteger y = 0; r->_surfaceRows && y < r->_height; y++)
		memcpy((uint8_t *)r->_surfaceRows + y * r->_surfaceRowBytes, r.bytes + y * r->_rowBytes, r->_width * r.bpp);
}

- (void)refresh
{
	if (!_surfaceRows || self.root->_shadowAhead)
		return;
	// A planar surface (video: the wallpaper's 4K 10-bit frame, 25 MB, copied for every command buffer while nothing changed it,
	// 2026-11-20) again only when its seed moved: its writers are decoders, through the CPU (lock and unlock move the seed).
	// ponytail: planar only; a surface drawn by the GPU (an app's, through the paravirtual GPU) need not move its seed.
	if (_surface && IOSurfaceGetPlaneCount(_surface) > 0) {
		const uint32_t seed = IOSurfaceGetSeed(_surface);
		if (_shadowSeeded && seed == _shadowSeed)
			return;
		_shadowSeed = seed, _shadowSeeded = YES;
	}
	const uint64_t t0 = nowUs();
	for (NSUInteger y = 0; y < _height; y++)
		memcpy(self.bytes + y * _rowBytes, _surfaceRows + y * _surfaceRowBytes, _width * self.bpp);
	statAdd(kStatShadowBytes, _height * _width * self.bpp);
	statAdd(kStatShadowUs, nowUs() - t0);
}

- (void)descriptor:(uint32_t[8])t
{
	if (_samples > 1) {   // its samples (64KB_2D, as the resolve's source)
		msaaImage(t, self.gpuBase, _fm->gfx, viewSel(_fm->sel, _swizzle), (uint32_t)_width, (uint32_t)_height, _samples == 4 ? 2 : 1);
		return;
	}
	if (_slices) {   // a 2D array's T#, a cube's typed CUBE (11): the hardware's own pitches as a 3D image's
		linearArray2d(t, self.gpuBase, _fm->gfx, viewSel(_fm->sel, _swizzle), (uint32_t)_width, (uint32_t)_height, (uint32_t)_slices);
		if (_type == MTLTextureTypeCube || _type == MTLTextureTypeCubeArray)
			t[3] = (t[3] & 0x0fffffffu) | 11u << 28;
		return;
	}
	if (_type == MTLTextureType3D) {   // the hardware's own pitches (pvdraw.hpp linearVolume; the memory laid out so in the init)
		linearVolume(t, self.gpuBase, _fm->gfx, viewSel(_fm->sel, _swizzle), (uint32_t)_width, (uint32_t)_height, (uint32_t)_depth);
		return;
	}
	if (_height == 1 && _width > 16384) {   // a texture buffer's wide row (initWithBuffer:): rows of 16384 texels
		linearImage(t, self.gpuBase, _fm->gfx, viewSel(_fm->sel, _swizzle), 16384, (uint32_t)((_width + 16383) / 16384), 16384);
		return;
	}
	linearImage(t, self.gpuBase, _fm->gfx, viewSel(_fm->sel, _swizzle), (uint32_t)_width, (uint32_t)_height, (uint32_t)(_rowBytes / _fm->bpp));
	if (_type == MTLTextureType1D)
		t[3] = (t[3] & 0x0fffffffu) | 8u << 28;   // TYPE 1D
	else if (_type == MTLTextureType1DArray)
		linearArray1d(t, self.gpuBase, _fm->gfx, viewSel(_fm->sel, _swizzle), (uint32_t)_width, (uint32_t)_layers);
}

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane
{
	if (!(self = [super init]))
		return nil;
	const BOOL planar = IOSurfaceGetPlaneCount(surface) > 0;
	const size_t bpr = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, plane) : IOSurfaceGetBytesPerRow(surface), bytes = IOSurfaceGetAllocSize(surface);
	const size_t width = planar ? IOSurfaceGetWidthOfPlane(surface, plane) : IOSurfaceGetWidth(surface);
	const size_t height = planar ? IOSurfaceGetHeightOfPlane(surface, plane) : IOSurfaceGetHeight(surface);
	void *base = IOSurfaceGetBaseAddress(surface);
	const NSUInteger offset = planar ? (uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface, plane) - (uint8_t *)base : 0;
	_fm = textureFormat((uint32_t)d.pixelFormat);
	if ((plane && plane >= IOSurfaceGetPlaneCount(surface)) || !_fm || !d.width || !d.height || d.width > width || d.height > height ||
	    d.width * _fm->bpp > bpr) {
		rdna4Error([NSString stringWithFormat:@"an IOSurface texture of format %lu, %lux%lu, %zu bytes a row (plane %lu at %lu): only the formats "
		                                      @"of pvdraw.hpp textureFormat, so far",
		                                      (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, bpr,
		                                      (unsigned long)plane, (unsigned long)offset], 0);
		return nil;
	}
	_plane = plane;
	_offset = offset;
	_type = MTLTextureType2D;   // (0 is MTLTextureType1D: its T# read row 0 only, the wallpaper came out flat)
	RTRACE(40, @"IOSurface texture %u: format %lu %lux%lu of %zux%zu, plane %lu at %lu, %zu bytes a row, usage 0x%lx", IOSurfaceGetID(surface),
	       (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, width, height, (unsigned long)plane,
	       (unsigned long)offset, bpr, (unsigned long)d.usage);
	CFRetain(surface);
	_surface = surface;
	if ((bpr & 127) || (offset & 255)) {   // rows the hardware cannot walk in place (WindowServer's small surfaces, 704 bytes a row): a copy
		_surfaceRows = (const uint8_t *)base + offset;
		_surfaceRowBytes = bpr;
		_offset = 0;
		_rowBytes = (d.width * _fm->bpp + 255) & ~(NSUInteger)255;
		_memory = [[RDNA4MTLBuffer alloc] initWithDevice:device length:_rowBytes * d.height options:MTLResourceStorageModeShared];
		[_memory keepOwner:^{ CFRelease(surface); }];
		[_memory countAsShadow];
	} else {
		_memory = importedSurface(device, surface, base, bytes);
		CFRelease(surface);   // the import holds it
		_rowBytes = bpr;
		// a target of WindowServer's (its displays' frames, the menu bars: made as render targets; the apps' windows it only reads)
		if (_memory && !planar && _fm->bpp == 4 && (d.usage & MTLTextureUsageRenderTarget) && bytes >= (1u << 20))
			_held = heldFor(surface, YES, device, _memory);
	}
	if (!_memory)
		return nil;
	_device = device;
	_width = d.width;
	_height = d.height;
	_format = d.pixelFormat;
	_usage = d.usage;
	_swizzle = packSwizzle(d.swizzle);
	return self;
}

- (instancetype)initViewOf:(RDNA4MTLTexture *)t format:(MTLPixelFormat)format type:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices
{
	if (!(self = [super init]))
		return nil;
	const NSUInteger l = levels.location, have = MAX(t->_levels, (NSUInteger)1);
	_fm = textureFormat((uint32_t)format);
	if (t->_isDepth || !t->_fm || !_fm || _fm->bpp != t->_fm->bpp || l >= have || slices.location || t->_type == MTLTextureType3D) {
		rdna4Error([NSString stringWithFormat:@"a view of format %lu, type %lu, levels %lu+%lu, slices %lu+%lu of a texture of format %lu: only colour "
		                                      @"textures, the same size a pixel, from slice 0, not 3D, so far", (unsigned long)format,
		                                      (unsigned long)type, (unsigned long)l, (unsigned long)levels.length,
		                                      (unsigned long)slices.location, (unsigned long)slices.length, (unsigned long)t->_format], 0);
		return nil;
	}
	_device = t->_device;
	_memory = t->_memory;
	_held = t->_held;
	_surface = t->_surface;
	_plane = t->_plane;
	_surfaceRows = l ? NULL : t->_surfaceRows;   // a level of a shadowed texture: ponytail, level 0 only is kept current
	_surfaceRowBytes = t->_surfaceRowBytes;
	_offset = t->_offset + t->_levelOffset[l];
	_rowBytes = l ? t->_levelRow[l] : t->_rowBytes;
	_width = MAX(t->_width >> l, (NSUInteger)1);
	_height = MAX(t->_height >> l, (NSUInteger)1);
	_format = format;
	_usage = t->_usage;
	_type = type == MTLTextureType1DArray || type == MTLTextureType1D ? type : MTLTextureType2D;
	_layers = t->_layers;
	_depth = 1;
	_levels = MIN(levels.length ?: 1, have - l);
	for (NSUInteger k = 0; k < _levels; k++) {
		_levelOffset[k] = t->_levelOffset[l + k] - t->_levelOffset[l];
		_levelRow[k] = t->_levelRow[l + k] ?: t->_rowBytes;
	}
	_parent = t;
	return self;
}

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices
{
	return [[RDNA4MTLTexture alloc] initViewOf:self format:format type:type levels:levels slices:slices];
}

// A swizzled view (Apple's GL-on-Metal renderer makes one for each texture it binds; without swizzles it bound none, 2026-11-21)
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices
                                        swizzle:(MTLTextureSwizzleChannels)swizzle
{
	RDNA4MTLTexture *v = [[RDNA4MTLTexture alloc] initViewOf:self format:format type:type levels:levels slices:slices];
	if (v)
		v->_swizzle = packSwizzle(swizzle);
	return v;
}

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
{
	return [self newTextureViewWithPixelFormat:format textureType:self.textureType levels:NSMakeRange(0, self.mipmapLevelCount)
	                                    slices:NSMakeRange(0, self.arrayLength)];
}

- (id<MTLTexture>)parentTexture { return _parent; }

- (RDNA4MTLTexture *)level:(NSUInteger)level
{
	if (!level)
		return self;
	static char key;
	@synchronized(self) {
		// held weakly: a view keeps its parent (_parent), so a strong cache here kept both forever (every texture with mip levels Core
		// Animation drew into, its blurs': WindowServer passed 7000 textures and 3 GB and aborted, 2026-11-20)
		NSMapTable *views = objc_getAssociatedObject(self, &key);
		if (!views)
			objc_setAssociatedObject(self, &key, views = [NSMapTable strongToWeakObjectsMapTable], OBJC_ASSOCIATION_RETAIN);
		RDNA4MTLTexture *v = [views objectForKey:@(level)];
		if (!v && (v = [[RDNA4MTLTexture alloc] initViewOf:self format:_format type:MTLTextureType2D levels:NSMakeRange(level, 1)
		                                            slices:NSMakeRange(0, 1)]))
			[views setObject:v forKey:@(level)];
		return v;
	}
}

- (IOSurfaceRef)iosurface { return _surface; }
- (NSUInteger)iosurfacePlane { return _plane; }
- (uint32_t)bpp { return _fm ? _fm->bpp : 4; }
- (uint32_t)gfxFormat { return _fm ? _fm->gfx : 0; }   // the image format of its T#s
- (MTLTextureSwizzleChannels)swizzle
{
	return _swizzle ? MTLTextureSwizzleChannelsMake((MTLTextureSwizzle)(_swizzle & 0xff), (MTLTextureSwizzle)(_swizzle >> 8 & 0xff),
	                                                (MTLTextureSwizzle)(_swizzle >> 16 & 0xff), (MTLTextureSwizzle)(_swizzle >> 24))
	                : MTLTextureSwizzleChannelsDefault;
}
- (uint32_t)swizzleKey
{
	return _swizzle ?: MTLTextureSwizzleRed | MTLTextureSwizzleGreen << 8 | MTLTextureSwizzleBlue << 16 | (uint32_t)MTLTextureSwizzleAlpha << 24;
}
- (BOOL)isWriteComplete { deviceIdle(_device); return YES; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }   // (as buffers: never purged)
- (id<MTLBuffer>)buffer { return _overBuffer; }   // (MoltenVK asks)
- (NSUInteger)bufferOffset { return _overOffset; }
- (NSUInteger)bufferBytesPerRow { return _overRowBytes; }
- (void)waitUntilComplete { deviceIdle(_device); }
- (MTLTextureCompressionType)compressionType { return MTLTextureCompressionTypeLossless; }
- (BOOL)allowGPUOptimizedContents { return NO; }
// ponytail: the T# as it is when asked (MoltenVK: when it writes the descriptor), not refreshed if the texture's memory moves later.
- (MTLResourceID)gpuResourceID
{
	uint32_t t[8];
	if (self.isDepth || !self.gpuBase)   // as an unbound slot (the draw's and dispatch's tables): a depth T# or one at 0 could stall the unit
		[_device spareImage:t];
	else
		[self descriptor:t];
	_handle = [_device handle:_handle words:t count:8];
	return (MTLResourceID){ ._impl = _handle };
}

- (instancetype)initWithBuffer:(RDNA4MTLBuffer *)buffer descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bpr
{
	if (!(self = [super init]))
		return nil;
	_fm = textureFormat((uint32_t)d.pixelFormat);
	// 2D, or a texture buffer (GL's buffer textures through Apple's GL-on-Metal renderer: Sublime Text's) as a 2D row; a row wider than a
	// T# holds (16384 texels, Sublime's 24576 RGBA32Float) as rows of 16384, as the translator reads it (air2amdgpu read_texture_buffer_1d)
	const BOOL row = d.textureType == MTLTextureTypeTextureBuffer && d.height == 1;
	if ((d.textureType != MTLTextureType2D && !row) || !_fm || !d.width || !d.height || bpr < d.width * _fm->bpp ||
	    (row && d.width > 16384u * 16384u) || offset + bpr * (d.height - 1) + d.width * _fm->bpp > buffer.length) {
		rdna4Error([NSString stringWithFormat:@"a texture of type %lu, format %lu, %lux%lu over a buffer at %lu, %lu bytes a row: only 2D and "
		                                      @"texture buffers of pvdraw.hpp's formats inside the buffer, so far", (unsigned long)d.textureType,
		                                      (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)offset,
		                                      (unsigned long)bpr], 0);
		return nil;
	}
	_type = MTLTextureType2D;
	_overBuffer = buffer, _overOffset = offset, _overRowBytes = bpr;
	if ((offset & 255) || (bpr & 127) || bpr % _fm->bpp) {   // where the hardware cannot read it in place (Core Animation's at offset 32): a copy
		_surfaceRows = (const uint8_t *)buffer.contents + offset;
		_surfaceRowBytes = bpr;
		_rowBytes = (d.width * _fm->bpp + 255) & ~(NSUInteger)255;
		_memory = [[RDNA4MTLBuffer alloc] initWithDevice:(RDNA4MTLDevice *)buffer.device length:_rowBytes * d.height options:MTLResourceStorageModeShared];
		RDNA4MTLBuffer *source = buffer;
		[_memory keepOwner:^{ (void)source; }];   // the memory the copy is of stays while the texture does
		[_memory countAsShadow];
	} else {
		_memory = buffer;
		_offset = offset;
		_rowBytes = bpr;
	}
	_device = (RDNA4MTLDevice *)buffer.device;
	_width = d.width;
	_height = d.height;
	_format = d.pixelFormat;
	_usage = d.usage;
	_swizzle = packSwizzle(d.swizzle);
	return self;
}

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)d
{
	if (!(self = [super init]))
		return nil;
	_fm = textureFormat(bcBlockBytes(d.pixelFormat) ? (d.pixelFormat & 1 ? 71u : 70u) : (uint32_t)d.pixelFormat);   // (BC: as RGBA8)
	// rows: a 1D texture's one, a 1D array's one a layer (CoreDisplay's combined gamma LUT: R32Float, 8192 wide), their T#s typed so
	const BOOL oneD = (d.textureType == MTLTextureType1D || (d.textureType == MTLTextureType1DArray && d.arrayLength >= 1 && d.arrayLength <= 2048)) &&
	                  d.height == 1;
	const BOOL threeD = d.textureType == MTLTextureType3D && d.mipmapLevelCount == 1 && d.depth >= 1 && d.depth <= 2048;
	const BOOL twoD = d.textureType == MTLTextureType2D && d.mipmapLevelCount >= 1 && d.mipmapLevelCount <= 15;
	// 2D arrays and cubes (VectorKit's: Maps aborted on a nil 8x8 cube, 2026-11-20), one level: their images as a 3D texture's slices
	const BOOL cube = d.textureType == MTLTextureTypeCube || d.textureType == MTLTextureTypeCubeArray;
	NSUInteger slices = 0;
	if (d.mipmapLevelCount == 1 && cube)
		slices = 6 * MAX(d.arrayLength, (NSUInteger)1);
	else if (d.mipmapLevelCount == 1 && d.textureType == MTLTextureType2DArray)
		slices = d.arrayLength;
	// 2x and 4x (Maps' 600x600 BGRA8 sRGB, 2026-11-20), 1, 2, 4 or 8 bytes a pixel (vkQuake's R8 through MoltenVK, 2026-11-21): the samples
	// in the card's memory, as the PV host's msaaSurface
	const BOOL msaa = d.textureType == MTLTextureType2DMultisample && (d.sampleCount == 2 || d.sampleCount == 4) && d.mipmapLevelCount == 1 &&
	                  _fm && (_fm->bpp == 1 || _fm->bpp == 2 || _fm->bpp == 4 || _fm->bpp == 8) && device.gpu;
	if ((!twoD && !threeD && !msaa && !(oneD && d.mipmapLevelCount == 1) && !(slices && slices <= 2048)) || (d.sampleCount != 1 && !msaa) || !_fm || !d.width || !d.height ||
	    d.width > 16384 || d.height > 16384) {
		rdna4Error([NSString stringWithFormat:@"a texture of type %lu, format %lu, %lux%lu, %lu layers, %lu levels, %lu samples: only 2D and 1D of "
		                                      @"pvdraw.hpp's formats with one level and one sample so far (arrays and cubes too)", (unsigned long)d.textureType,
		                                      (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height,
		                                      (unsigned long)d.arrayLength, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount], 0);
		return nil;
	}
	_type = d.textureType;
	_layers = _type == MTLTextureType1DArray || slices ? MAX(d.arrayLength, (NSUInteger)1) : 1;
	_slices = slices;
	_depth = threeD ? d.depth : 1;
	_rowBytes = (d.width * _fm->bpp + 255) & ~(NSUInteger)255;   // the colour block's pitch in whole 256-byte rows (the PV host: a multiple of 128)
	NSUInteger bytes = _rowBytes * d.height * _layers;
	if (threeD || slices) {   // the hardware's pitches for a linear 3D image (pvdraw.hpp linearVolume): rows at 128 bytes, slices at 256
		_rowBytes = (d.width * _fm->bpp + 127) & ~(NSUInteger)127;
		_slicePitch = (_rowBytes * d.height + 255) & ~(NSUInteger)255;
		bytes = _slicePitch * (slices ?: _depth);
	}
	_levels = twoD ? d.mipmapLevelCount : 1;
	_levelRow[0] = _rowBytes;
	for (NSUInteger l = 1; l < _levels; l++) {   // each level an image of its own after level 0, 256-byte aligned
		const NSUInteger w = MAX(d.width >> l, (NSUInteger)1), h = MAX(d.height >> l, (NSUInteger)1);
		_levelOffset[l] = (bytes + 255) & ~(NSUInteger)255;
		_levelRow[l] = (w * _fm->bpp + 255) & ~(NSUInteger)255;
		bytes = _levelOffset[l] + _levelRow[l] * h;
	}
	if (msaa) {   // 64KB_2D in whole 128x128 blocks (msaaSurface's), the colour block's pitch its width; drawn, cleared and resolved on the card
		_samples = (uint32_t)d.sampleCount;
		_rowBytes = d.width * _fm->bpp;
		// (fragments of under 4 bytes: a 64 KiB block is wider than 128 pixels, up to 256)
		const NSUInteger align = _fm->bpp * _samples < 4 ? 255 : 127;
		if (rdna4_alloc(device.gpu, (uint64_t)((d.width + align) & ~align) * ((d.height + align) & ~align) * _fm->bpp * _samples, &_vram))
			return nil;
	} else if (!(_memory = [[RDNA4MTLBuffer alloc] initWithDevice:device length:bytes options:MTLResourceStorageModeShared]))
		return nil;
	_device = device;
	_width = d.width;
	_height = d.height;
	_format = d.pixelFormat;
	_usage = d.usage;
	_swizzle = packSwizzle(d.swizzle);
	return self;
}

- (id<MTLDevice>)device { return _device; }
- (MTLTextureType)textureType { return _type == MTLTextureType1D || _type == MTLTextureType1DArray || _type == MTLTextureType3D || _slices || _samples > 1 ? _type : MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return _format; }
- (NSUInteger)width { return _width; }
- (NSUInteger)height { return _height; }
- (NSUInteger)depth { return _depth ?: 1; }
- (NSUInteger)mipmapLevelCount { return _levels ?: 1; }
- (NSUInteger)sampleCount { return MAX(_samples, 1u); }
- (uint32_t)samples { return MAX(_samples, 1u); }
- (NSUInteger)arrayLength { return _layers ?: 1; }
- (MTLTextureUsage)usage { return _usage; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (NSUInteger)allocatedSize { return _memory.allocatedSize; }
- (BOOL)isFramebufferOnly { return NO; }
- (uint64_t)gpuBase { return _held ? _held.gpu + _offset : _vram.handle ? _vram.gpu + _offset : _memory.gpuAddress + _offset; }
- (uint8_t *)bytes
{
	[_held cpuTouched];
	return (uint8_t *)_memory.contents + _offset;
}
- (RDNA4Held *)held { return _held; }
- (void)heldRows:(uint32_t)y0 to:(uint32_t)y1 width:(uint32_t)x1
{
	if (_held && y0 < y1)
		[_held dirty:_offset + (uint64_t)y0 * _rowBytes to:_offset + (uint64_t)(y1 - 1) * _rowBytes + (uint64_t)x1 * self.bpp];
}

+ (instancetype)allocWithZone:(NSZone *)zone
{
	gTexturesLive++;
	return [super allocWithZone:zone];
}
- (void)dealloc
{
	gTexturesLive--;
	[_device freeHandle:_handle];
	if (_vram.handle)
		rdna4_free(_device.gpu, &_vram);
}

- (BOOL)fits:(MTLRegion)r level:(NSUInteger)level
{
	const NSUInteger levels = MAX(_levels, (NSUInteger)1);
	return level < levels && r.origin.x + r.size.width <= MAX(_width >> level, (NSUInteger)1) &&
	       r.origin.y + r.size.height <= MAX(_height >> level, (NSUInteger)1) && r.origin.z + MAX(r.size.depth, (NSUInteger)1) <= MAX(_depth, (NSUInteger)1);
}

// Where pixel (x, y, z) of `level` is for the CPU.
- (uint8_t *)at:(NSUInteger)level x:(NSUInteger)x y:(NSUInteger)y z:(NSUInteger)z
{
	return self.bytes + _levelOffset[level] + z * _slicePitch + y * (level ? _levelRow[level] : _rowBytes) + x * self.bpp;
}

// In VRAM only (depth, multisample): no CPU copy, CPU reads and writes are ignored. ponytail: ANGLE's zero fill of a new renderbuffer, which
// its passes clear anyway; a GPU blit if contents matter.
- (BOOL)vramOnly
{
	if (_memory.contents)
		return NO;
	RTRACE(8, @"CPU access to a %lux%lu texture of format %lu, %u samples, in VRAM only: ignored", (unsigned long)_width, (unsigned long)_height,
	       (unsigned long)_format, _samples);
	return YES;
}

- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr
{
	if (self.vramOnly)
		return;
	[self refresh];   // (a shadowed surface's copy current: all of it goes back below)
	[self wroteAll];
	if (const uint32_t bc = bcBlockBytes(_format)) {   // 4x4 blocks from r.origin (a multiple of 4), texels past the level's edge left out
		const NSUInteger lw = MAX(_width >> level, (NSUInteger)1), lh = MAX(_height >> level, (NSUInteger)1);
		for (NSUInteger by = 0; by < (r.size.height + 3) / 4; by++)
			for (NSUInteger bx = 0; bx < (r.size.width + 3) / 4; bx++) {
				uint8_t texels[16][4];
				bcBlock((const uint8_t *)bytes + by * bpr + bx * bc, bc, _format == 132 || _format == 133, texels);
				for (NSUInteger i = 0; i < 16; i++) {
					const NSUInteger x = r.origin.x + bx * 4 + i % 4, y = r.origin.y + by * 4 + i / 4;
					if (x < lw && y < lh)
						memcpy([self at:level x:x y:y z:r.origin.z], texels[i], 4);
				}
			}
		return;
	}
	for (NSUInteger y = 0; [self fits:r level:level] && y < r.size.height; y++)
		memcpy([self at:level x:r.origin.x y:r.origin.y + y z:r.origin.z], (const uint8_t *)bytes + y * bpr, r.size.width * self.bpp);
	[self shadowToSurface];
}

- (void)generateMipmaps
{
	[self wroteAll];
	const uint32_t bpp = self.bpp;
	for (NSUInteger l = 1; l < _levels; l++) {
		const NSUInteger w = MAX(_width >> l, (NSUInteger)1), h = MAX(_height >> l, (NSUInteger)1);
		const NSUInteger pw = MAX(_width >> (l - 1), (NSUInteger)1), ph = MAX(_height >> (l - 1), (NSUInteger)1);
		for (NSUInteger y = 0; y < h; y++)
			for (NSUInteger x = 0; x < w; x++) {
				uint8_t *out = [self at:l x:x y:y z:0];
				if (bpp != 4) {   // ponytail: nearest for formats of other than four 8-bit channels
					memcpy(out, [self at:l - 1 x:MIN(2 * x, pw - 1) y:MIN(2 * y, ph - 1) z:0], bpp);
					continue;
				}
				for (int c = 0; c < 4; c++) {
					unsigned sum = 0;
					for (int k = 0; k < 4; k++)
						sum += [self at:l - 1 x:MIN(2 * x + (k & 1), pw - 1) y:MIN(2 * y + (k >> 1), ph - 1) z:0][c];
					out[c] = (uint8_t)((sum + 2) / 4);
				}
			}
	}
}

- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
{
	if (self.vramOnly)
		return;
	if (_type == MTLTextureType1DArray && slice < _layers && !level && r.origin.x + r.size.width <= _width)   // a layer is a row
		memcpy(self.bytes + slice * _rowBytes + r.origin.x * self.bpp, bytes, r.size.width * self.bpp);
	else if (_slices && slice < _slices && !level && [self fits:r level:0])   // an array's or cube's image: a slice
		for (NSUInteger y = 0; y < r.size.height; y++)
			memcpy([self at:0 x:r.origin.x y:r.origin.y + y z:slice], (const uint8_t *)bytes + y * bpr, r.size.width * self.bpp);
	else if (_type == MTLTextureType3D && [self fits:r level:level])
		for (NSUInteger z = 0; z < MAX(r.size.depth, (NSUInteger)1); z++)
			for (NSUInteger y = 0; y < r.size.height; y++)
				memcpy([self at:0 x:r.origin.x y:r.origin.y + y z:r.origin.z + z], (const uint8_t *)bytes + z * bpi + y * bpr, r.size.width * self.bpp);
	else if (!slice)
		[self replaceRegion:r mipmapLevel:level withBytes:bytes bytesPerRow:bpr];
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
           slice:(NSUInteger)slice
{
	if (self.vramOnly)
		return;
	if (_slices && slice < _slices && !level && [self fits:r level:0])
		for (NSUInteger y = 0; y < r.size.height; y++)
			memcpy((uint8_t *)bytes + y * bpr, [self at:0 x:r.origin.x y:r.origin.y + y z:slice], r.size.width * self.bpp);
	else if (!slice)
		[self getBytes:bytes bytesPerRow:bpr fromRegion:r mipmapLevel:level];
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
	if (self.vramOnly)
		return;
	[self refresh];   // (a shadowed surface: its copy current, unless draws not yet written back made it newer)
	for (NSUInteger y = 0; [self fits:r level:level] && y < r.size.height; y++)
		memcpy((uint8_t *)bytes + y * bpr, [self at:level x:r.origin.x y:r.origin.y + y z:r.origin.z], r.size.width * self.bpp);
}

// A clear load action by the CPU: the colour packed as the format stores it (pvdraw.hpp clearPattern; RGBA16Float as four halves).
// ponytail: a CPU fill; a GPU clear when it shows in a profile.
// A clear colour as the texture's pixel (8 bytes: a 4-byte pixel twice).
- (uint64_t)pixelFor:(MTLClearColor)c
{
	const double ch[4] = { c.red, c.green, c.blue, c.alpha };
	uint64_t bits[4];
	memcpy(bits, ch, sizeof bits);
	uint64_t px = 0;
	if (_format == MTLPixelFormatRGBA16Float)
		px = doubleAsHalf(bits[0]) | doubleAsHalf(bits[1]) << 16 | (uint64_t)doubleAsHalf(bits[2]) << 32 | (uint64_t)doubleAsHalf(bits[3]) << 48;
	else {
		uint32_t pattern = 0;
		if (!clearPattern((uint32_t)_format, bits, pattern))
			RLOG(@"RDNA4MTL: a clear of a format %lu target to (%g %g %g %g): cleared to zero", (unsigned long)_format, c.red, c.green, c.blue, c.alpha);
		px = pattern | (uint64_t)pattern << 32;
	}
	return px;
}

- (BOOL)fillFor:(MTLClearColor)c pattern:(uint32_t *)pattern bytes:(uint64_t *)bytes
{
	const uint32_t bpp = self.bpp;
	const uint64_t px = [self pixelFor:c];
	// (1- and 2-byte pixels: clearPattern repeats the pixel through the 32 bits)
	if ((_type != MTLTextureType2D && _samples < 2) || !_fm || (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) || (uint32_t)px != (uint32_t)(px >> 32) ||
	    (self.gpuBase & 3))
		return NO;
	*pattern = (uint32_t)px;
	*bytes = _samples > 1 ? _vram.bytes : (_rowBytes * (_height - 1) + _width * bpp + 3) & ~3ull;   // the rows and what lies between them (row padding)
	return YES;
}

- (void)clearTo:(MTLClearColor)c
{
	if (!_memory) {   // ponytail: the card's memory only takes fills of one 32-bit pattern (fillFor)
		RLOG(@"RDNA4MTL: a clear of a format %lu texture in the card's memory to (%g %g %g %g): not done", (unsigned long)_format, c.red, c.green,
		     c.blue, c.alpha);
		return;
	}
	[self wroteAll];
	const uint64_t px = [self pixelFor:c];
	const uint32_t bpp = self.bpp;
	uint8_t one[16];
	for (uint32_t i = 0; i < bpp; i++)
		one[i] = (uint8_t)(px >> (8 * (i % 8)));
	for (NSUInteger y = 0; y < _height; y++) {
		uint8_t *row = self.bytes + y * _rowBytes;
		if (bpp == 4)
			memset_pattern4(row, one, _width * 4);
		else if (bpp == 8)
			memset_pattern8(row, one, _width * 8);
		else
			for (NSUInteger x = 0; x < _width; x++)
				memcpy(row + x * bpp, one, bpp);
	}
}

RDNA4_ANSWER_MISSING
@end

@implementation RDNA4MTLBuffer (Textures)
- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bpr
                                    bytesPerImage:(NSUInteger)bpi
{
	return [[RDNA4MTLTexture alloc] initWithBuffer:self descriptor:d offset:offset bytesPerRow:bpr];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bpr
{
	return [[RDNA4MTLTexture alloc] initWithBuffer:self descriptor:d offset:offset bytesPerRow:bpr];
}

// Metal's "tiled" textures over memory (Core Animation's, WindowServer): rows `bpr` apart all the same, so linear here.
- (id<MTLTexture>)newTiledTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bpr
{
	return [[RDNA4MTLTexture alloc] initWithBuffer:self descriptor:d offset:offset bytesPerRow:bpr];
}
@end

// ---- acceleration structures (gate 5)

// The device's own format, which tools/metal/m3/rtlib.c traverses: a header {'RAS1', nodes, triangles, 0}, the nodes depth first (32 bytes:
// lo[3], escape, hi[3], tris: 0 for an inner node, else first << 4 | count) and the triangles (48 bytes: v0, v1, v2, primitive id,
// geometry index, 0). Built by the CPU, as a command buffer's step, from shared vertex (and index) buffers.
typedef struct { float lo[3]; uint32_t escape; float hi[3]; uint32_t tris; } RtNode;
typedef struct { float v[9]; uint32_t prim, geom, zero; } RtTri;
enum { kRtHeader = 16, kRtLeaf = 4 };

static uint64_t rtBytes(uint64_t triangles) { return kRtHeader + sizeof(RtNode) * (2 * triangles + 1) + sizeof(RtTri) * triangles; }

static int gRtAxis;   // ponytail: qsort's axis; builds run one at a time (synchronous commit)
static int rtCompare(const void *a, const void *b)
{
	const RtTri *x = (const RtTri *)a, *y = (const RtTri *)b;
	const float cx = x->v[gRtAxis] + x->v[3 + gRtAxis] + x->v[6 + gRtAxis], cy = y->v[gRtAxis] + y->v[3 + gRtAxis] + y->v[6 + gRtAxis];
	return cx < cy ? -1 : cx > cy;
}

// The subtree of tris[first, first + count) from nodes[*used] on, depth first; leaves of at most kRtLeaf triangles, split at the median
// of the widest axis of their centres.
static void rtBuild(RtTri *tris, uint32_t first, uint32_t count, RtNode *nodes, uint32_t *used)
{
	RtNode *n = &nodes[(*used)++];
	float clo[3] = { INFINITY, INFINITY, INFINITY }, chi[3] = { -INFINITY, -INFINITY, -INFINITY };
	for (int a = 0; a < 3; a++) {
		n->lo[a] = INFINITY;
		n->hi[a] = -INFINITY;
	}
	for (uint32_t i = first; i < first + count; i++)
		for (int a = 0; a < 3; a++) {
			const float c = tris[i].v[a] + tris[i].v[3 + a] + tris[i].v[6 + a];
			clo[a] = fminf(clo[a], c);
			chi[a] = fmaxf(chi[a], c);
			for (int k = 0; k < 3; k++) {
				n->lo[a] = fminf(n->lo[a], tris[i].v[3 * k + a]);
				n->hi[a] = fmaxf(n->hi[a], tris[i].v[3 * k + a]);
			}
		}
	if (count <= kRtLeaf) {
		n->tris = first << 4 | count;
		n->escape = *used;
		return;
	}
	gRtAxis = chi[1] - clo[1] > chi[0] - clo[0] ? 1 : 0;
	gRtAxis = chi[2] - clo[2] > chi[gRtAxis] - clo[gRtAxis] ? 2 : gRtAxis;
	qsort(tris + first, count, sizeof(RtTri), rtCompare);
	rtBuild(tris, first, count / 2, nodes, used);
	rtBuild(tris, first + count / 2, count - count / 2, nodes, used);
	n->tris = 0;
	n->escape = *used;
}

// The triangles a descriptor describes, or an error: triangle geometry of float3 vertices, with or without 16- or 32-bit indices.
static NSError *rtTriangles(MTLPrimitiveAccelerationStructureDescriptor *d, NSMutableData *out)
{
	uint32_t geom = 0;
	for (MTLAccelerationStructureGeometryDescriptor *g in d.geometryDescriptors) {
		if (![g isKindOfClass:[MTLAccelerationStructureTriangleGeometryDescriptor class]])
			return rdna4Error(@"only triangle geometry so far", 0);
		MTLAccelerationStructureTriangleGeometryDescriptor *t = (MTLAccelerationStructureTriangleGeometryDescriptor *)g;
		if (t.vertexFormat != MTLAttributeFormatFloat3 || t.transformationMatrixBuffer || ![(id)t.vertexBuffer isKindOfClass:[RDNA4MTLBuffer class]] ||
		    (t.indexBuffer && ![(id)t.indexBuffer isKindOfClass:[RDNA4MTLBuffer class]]))
			return rdna4Error(@"a triangle geometry of other than float3 vertices in buffers of this device, or with a transform: not yet", 0);
		const NSUInteger stride = t.vertexStride ?: 12;
		const uint8_t *vb = (const uint8_t *)t.vertexBuffer.contents + t.vertexBufferOffset;
		const uint8_t *ib = t.indexBuffer ? (const uint8_t *)t.indexBuffer.contents + t.indexBufferOffset : NULL;
		const BOOL wide = t.indexType == MTLIndexTypeUInt32;
		const NSUInteger vertices = (t.vertexBuffer.length - t.vertexBufferOffset) / stride;
		for (NSUInteger i = 0; i < t.triangleCount; i++) {
			RtTri tri = { .prim = (uint32_t)i, .geom = geom };
			for (int k = 0; k < 3; k++) {
				const NSUInteger j = 3 * i + k;
				if (ib && (wide ? 4 : 2) * (j + 1) > t.indexBuffer.length - t.indexBufferOffset)
					return rdna4Error([NSString stringWithFormat:@"triangle %lu of geometry %u reads past its index buffer", (unsigned long)i, geom], 0);
				const NSUInteger v = !ib ? j : wide ? ((const uint32_t *)ib)[j] : ((const uint16_t *)ib)[j];
				if (v >= vertices)
					return rdna4Error([NSString stringWithFormat:@"triangle %lu of geometry %u reads past its vertex buffer", (unsigned long)i, geom], 0);
				memcpy(&tri.v[3 * k], vb + v * stride, 12);
			}
			[out appendBytes:&tri length:sizeof tri];
		}
		geom++;
	}
	return nil;
}

@interface RDNA4MTLAccelerationStructure : NSObject <MTLAccelerationStructure>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device size:(NSUInteger)size;
- (NSError *)build:(MTLAccelerationStructureDescriptor *)descriptor;
@property (readonly) RDNA4MTLBuffer *memory;
@end

// A structure's memory, bound as a buffer (the translator's acceleration structure arguments: their address); nil for another device's
static RDNA4MTLBuffer *structureMemory(id<MTLAccelerationStructure> as);

@implementation RDNA4MTLAccelerationStructure {
	RDNA4MTLDevice *_device;
}
@synthesize label;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device size:(NSUInteger)size
{
	if (!(self = [super init]))
		return nil;
	_memory = [[RDNA4MTLBuffer alloc] initWithDevice:device length:MAX(size, (NSUInteger)kRtHeader) options:MTLResourceStorageModeShared];
	_device = device;
	return _memory ? self : nil;
}

// An instance structure ('RAS2', rtlib.c): per instance its world-to-object transform (3 rows of 4 floats), the address of its primitive
// structure and its mask; the instances' structures are read when a ray reaches them, so they may be built after this one. Instance
// descriptors of the default and the user-ID types (the user ID is not kept: the instance's index stands for it).
- (NSError *)buildInstances:(MTLInstanceAccelerationStructureDescriptor *)d
{
	const MTLAccelerationStructureInstanceDescriptorType type = d.instanceDescriptorType;
	if (type != MTLAccelerationStructureInstanceDescriptorTypeDefault && type != MTLAccelerationStructureInstanceDescriptorTypeUserID)
		return rdna4Error([NSString stringWithFormat:@"instance descriptors of type %lu: the default and user-ID ones only so far", (unsigned long)type], 0);
	const NSUInteger size = type == MTLAccelerationStructureInstanceDescriptorTypeDefault ? sizeof(MTLAccelerationStructureInstanceDescriptor)
	                                                                                      : sizeof(MTLAccelerationStructureUserIDInstanceDescriptor);
	const NSUInteger stride = d.instanceDescriptorStride ?: size, count = d.instanceCount;
	id<MTLBuffer> in = d.instanceDescriptorBuffer;
	if (![(id)in isKindOfClass:[RDNA4MTLBuffer class]] || (count && d.instanceDescriptorBufferOffset + (count - 1) * stride + size > in.length))
		return rdna4Error(@"instance descriptors in a buffer of another device, or past its end", 0);
	if (16 + 64 * count > _memory.length)
		return rdna4Error([NSString stringWithFormat:@"%lu instances do not fit an acceleration structure of %lu bytes", (unsigned long)count,
		                                             (unsigned long)_memory.length], 0);
	uint32_t *head = (uint32_t *)_memory.contents;
	for (NSUInteger i = 0; i < count; i++) {
		const MTLAccelerationStructureInstanceDescriptor *inst =
			(const MTLAccelerationStructureInstanceDescriptor *)((const uint8_t *)in.contents + d.instanceDescriptorBufferOffset + i * stride);
		float *rec = (float *)(head + 4 + 16 * i);
		memset(rec, 0, 64);
		id blas = inst->accelerationStructureIndex < d.instancedAccelerationStructures.count ? d.instancedAccelerationStructures[inst->accelerationStructureIndex] : nil;
		// world = A obj + t (A's columns: the matrix's first three); object = A^-1 (world - t)
		const MTLPackedFloat3 *c = inst->transformationMatrix.columns;
		const float a[3][3] = { { c[0].x, c[1].x, c[2].x }, { c[0].y, c[1].y, c[2].y }, { c[0].z, c[1].z, c[2].z } };
		const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
		                  a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
		if (![blas isKindOfClass:[RDNA4MTLAccelerationStructure class]] || fabsf(det) < 1e-20f)
			continue;   // no structure, or a degenerate transform: an instance no ray meets (address 0)
		float inv[3][3];
		for (int r = 0; r < 3; r++)
			for (int k = 0; k < 3; k++) {   // the adjugate's (r, k): the cofactor of (k, r)
				const int r1 = (k + 1) % 3, r2 = (k + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
				inv[r][k] = (a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]) / det;
			}
		const float t[3] = { c[3].x, c[3].y, c[3].z };
		for (int r = 0; r < 3; r++) {
			for (int k = 0; k < 3; k++)
				rec[4 * r + k] = inv[r][k];
			rec[4 * r + 3] = -(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2]);
		}
		const uint64_t address = ((RDNA4MTLAccelerationStructure *)blas).memory.gpuAddress;
		memcpy(rec + 12, &address, 8);
		((uint32_t *)rec)[14] = inst->mask;
	}
	head[0] = 0x32534152u;   // 'RAS2'
	head[1] = (uint32_t)count;
	head[2] = head[3] = 0;
	return nil;
}

- (NSError *)build:(MTLAccelerationStructureDescriptor *)descriptor
{
	if ([descriptor isKindOfClass:[MTLInstanceAccelerationStructureDescriptor class]])
		return [self buildInstances:(MTLInstanceAccelerationStructureDescriptor *)descriptor];
	if (![descriptor isKindOfClass:[MTLPrimitiveAccelerationStructureDescriptor class]])
		return rdna4Error(@"only primitive and instance acceleration structures so far", 0);
	MTLPrimitiveAccelerationStructureDescriptor *d = (MTLPrimitiveAccelerationStructureDescriptor *)descriptor;
	NSMutableData *tris = [NSMutableData data];
	NSError *why = rtTriangles(d, tris);
	const uint32_t count = (uint32_t)(tris.length / sizeof(RtTri));
	if (why)
		return why;
	if (rtBytes(count) > _memory.length || count >= (1u << 27))
		return rdna4Error([NSString stringWithFormat:@"%u triangles do not fit an acceleration structure of %lu bytes", count,
		                                             (unsigned long)_memory.length], 0);
	uint32_t *head = (uint32_t *)_memory.contents;
	RtNode *nodes = (RtNode *)(head + 4);
	uint32_t used = 0;
	if (count)
		rtBuild((RtTri *)tris.mutableBytes, 0, count, nodes, &used);
	memcpy(nodes + used, tris.bytes, tris.length);
	head[0] = 0x31534152u;   // 'RAS1'
	head[1] = used;
	head[2] = count;
	head[3] = 0;
	return nil;
}

- (id<MTLDevice>)device { return _device; }
- (NSUInteger)size { return _memory.length; }
- (NSUInteger)allocatedSize { return _memory.allocatedSize; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLResourceID)gpuResourceID { return (MTLResourceID){ ._impl = _memory.gpuAddress }; }
- (uint64_t)gpuAddress { return _memory.gpuAddress; }

RDNA4_ANSWER_MISSING
@end

static RDNA4MTLBuffer *structureMemory(id<MTLAccelerationStructure> as)
{
	return [(id)as isKindOfClass:[RDNA4MTLAccelerationStructure class]] ? ((RDNA4MTLAccelerationStructure *)as).memory : nil;
}

// ---- libraries and functions

@interface RDNA4MTLLibrary : NSObject <MTLLibrary>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device path:(NSString *)path;
@property (readonly) NSString *path;   // the metallib
@end

@interface RDNA4MTLFunction : NSObject <MTLFunction>
@property (readonly) RDNA4MTLLibrary *library;
@end

@implementation RDNA4MTLFunction {
	NSString *_name;
}
@synthesize label;

- (instancetype)initWithLibrary:(RDNA4MTLLibrary *)library name:(NSString *)name
{
	if ((self = [super init])) {
		_library = library;
		_name = [name copy];
	}
	return self;
}

- (NSString *)name { return _name; }
- (id<MTLDevice>)device { return _library.device; }
- (MTLFunctionType)functionType { return MTLFunctionTypeKernel; }

RDNA4_ANSWER_MISSING
@end

@implementation RDNA4MTLLibrary {
	RDNA4MTLDevice *_device;
}
@synthesize label, installName;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device path:(NSString *)path
{
	if ((self = [super init])) {
		_device = device;
		_path = [path copy];
	}
	return self;
}

- (id<MTLDevice>)device { return _device; }

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
	return [[RDNA4MTLFunction alloc] initWithLibrary:self name:name];
}

RDNA4_ANSWER_MISSING
@end

// ---- translation

// `function` translated by the translator in file mode ($RDNA4_AIR2AMDGPU, the bundle's Contents/Helpers/air2amdgpu, or /usr/local/libexec/rdna4-air2amdgpu): a kernel's code
// object, or a vertex or fragment function's RGFX blob (tools/metal/m3/air2amdgpu.cpp), and a kernel's count of buffer addresses.
// A process that may not start programs (WindowServer's sandbox): the kext's translator helper does it (rdna4_translate).
static NSError *translateByKext(rdna4_t *gpu, NSData *air, NSString *name, NSData **blob, uint32_t *buffers)
{
	NSMutableData *out = [NSMutableData dataWithLength:RDNA4_MTLC_MAX_ELF];
	uint64_t bytes = 0;
	const kern_return_t kr = air && gpu ? rdna4_translate(gpu, air.bytes, air.length, name.UTF8String, out.mutableBytes, out.length, &bytes, buffers)
	                                    : kIOReturnNotOpen;
	if (kr)
		return rdna4Error([NSString stringWithFormat:@"the kext's helper could not translate %@ (%lu bytes of AIR)", name, (unsigned long)air.length], kr);
	out.length = (NSUInteger)bytes;
	*blob = out;
	return nil;
}

// `function`'s AIR: our library's metallib (the translator picks the function by name), or the bitcode of a function Metal's own compiler
// made; nil without either.
// An OpenGL program (Apple's GL-on-Metal renderer: GL Core IR, bitcodeType not 0, with its GL state as pluginData) as a metallib of AIR.
// Only Metal's compiler service turns GL Core IR into AIR (libGLDPlugin's OGLCodeGenService, with the state), on its way to a device's
// compiler plugin; Apple's paravirtual plugin then packs that AIR as a metallib for its host, so it is asked here: an MTLCompiler with
// that plugin (as -[AppleParavirtDevice setupCompiler] makes it: target {shader version 2, 2}, flags 0x20), one request per function,
// the state as the request's frameworkData. ponytail: the reply is found by its content (the dispatch_data that is a metallib, an
// NSString error) in the struct the completion handler gets: its layout is Tahoe 26.6.2's MTLCompiler private.
static __weak id<MTLDevice> gGlDevice;   // the process's device (initNativeWithAcceleratorPort:): a GL function's own -device is nil
static char kGlAir, kGlReflection;       // associated-object keys: a GL function's metallib and its reflection

static NSData *glProgramAir(id<MTLFunction> function)
{
	id f = function;
	if (![f respondsToSelector:@selector(pluginData)] || ![f respondsToSelector:@selector(bitcodeType)])
		return nil;
	NSData *state = [f performSelector:@selector(pluginData)];
	const unsigned char type = ((unsigned char (*)(id, SEL))objc_msgSend)(f, @selector(bitcodeType));
	if (!type || ![state isKindOfClass:[NSData class]])
		return nil;
	if (NSData *done = objc_getAssociatedObject(f, &kGlAir))
		return done;
	static id compiler;   // ponytail: one for the process (one device)
	static std::once_flag once;
	std::call_once(once, ^{
		const uint32_t target[2] = {2, 2};   // Metal 2.2, the shader version our paravirtual device reports (docs/m0-pvgpu.md)
		dispatch_data_t targetData = dispatch_data_create(target, sizeof target, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
		uint8_t uuid[32] = "AMDRadeonRDNA4 GL programs";
		NSString *plugin = @"/System/Library/Extensions/AppleParavirtGPUMetal.bundle/Contents/Resources/libAppleParavirtCompilerPlugin.dylib";
		Class c = objc_getClass("MTLCompiler");
		SEL init = sel_registerName("initWithTargetData:cacheUUID:pluginPath:device:compilerFlags:");
		if (c && [c instancesRespondToSelector:init])   // (the plugin lives in the dyld shared cache: no file to check)
			compiler = ((id (*)(id, SEL, id, const void *, id, id, unsigned long long))objc_msgSend)([c alloc], init, (id)targetData, uuid, plugin,
			                                                                                           function.device ?: gGlDevice, 0x20ull);
		RLOG(@"RDNA4MTL: OpenGL programs through %@: %@", plugin.lastPathComponent, compiler ? @"ready" : @"no MTLCompiler (not Tahoe 26.6's Metal?)");
	});
	Class requestClass = objc_getClass("MTLCompileFunctionRequestData");
	if (!compiler || !requestClass)
		return nil;
	id request = [requestClass new];
	((void (*)(id, SEL, id))objc_msgSend)(request, sel_registerName("setFunction:"), f);
	((void (*)(id, SEL, signed char))objc_msgSend)(request, sel_registerName("setSync:"), 1);
	// with the argument information in its reflection (what Apple's GL-on-Metal renderer asks its pipelines for, 0x40003: argument info,
	// buffer type info, binding info): without it the reflection named no uniform buffer and GL bound none
	((void (*)(id, SEL, unsigned long long))objc_msgSend)(request, sel_registerName("setPipelineOptions:"), 0x40003ull);
	((void (*)(id, SEL, id))objc_msgSend)(request, sel_registerName("setFrameworkData:"),
	                                      (id)dispatch_data_create(state.bytes, state.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT));
	__block NSData *air = nil, *reflection = nil;
	__block NSString *why = nil;
	void (^done)(const void *, uintptr_t) = ^(const void *reply, uintptr_t) {
		const uintptr_t *w = (const uintptr_t *)reply;
		for (int i = 0; reply && i < 16; i++) {
			void *p = (void *)w[i];
			if ((uintptr_t)p < 0x100000 || !malloc_zone_from_ptr(p) || malloc_size(p) < 16)
				continue;
			id o = (__bridge id)p;
			if ([o isKindOfClass:[NSData class]] && [(NSData *)o length] >= 4 && !memcmp([(NSData *)o bytes], "MTLB", 4))
				air = [(NSData *)o copy];
			else if (!reflection && [o isKindOfClass:[NSData class]] && [(NSData *)o length])   // the function's reflection, the first data
				reflection = [(NSData *)o copy];                                                  // after the metallib (later: the plugin's output)
			else if ([o isKindOfClass:[NSString class]])
				why = [o copy];
		}
	};
	((void (*)(id, SEL, id, id))objc_msgSend)(compiler, sel_registerName("compileFunctionRequest:completionHandler:"), request, done);
	if (!air) {
		RLOG(@"RDNA4MTL: OpenGL program %@ not compiled: %@", function.name, why ?: @"no metallib in the reply");
		return nil;
	}
	objc_setAssociatedObject(f, &kGlAir, air, OBJC_ASSOCIATION_RETAIN);
	if (reflection)
		objc_setAssociatedObject(f, &kGlReflection, reflection, OBJC_ASSOCIATION_RETAIN);
	return air;
}

// A GL program's reflection, as the compiler service gave it with its AIR (glProgramAir), else nil
static NSData *glReflection(id<MTLFunction> function)
{
	if (!function)
		return nil;
	(void)glProgramAir(function);   // (compiled already when the pipeline was: from the function's own cache then)
	return objc_getAssociatedObject(function, &kGlReflection);
}

static NSData *functionAir(id<MTLFunction> function)
{
	if ([(id)function isKindOfClass:[RDNA4MTLFunction class]])
		return [NSData dataWithContentsOfFile:((RDNA4MTLFunction *)function).library.path];
	if (NSData *gl = glProgramAir(function))
		return gl;
	id bitcode = [(id)function respondsToSelector:@selector(bitcodeData)] ? [(id)function performSelector:@selector(bitcodeData)] : nil;
	return [bitcode isKindOfClass:[NSData class]] ? bitcode : nil;
}

// One entry of a container of linked functions (air2amdgpu.cpp linkEntries): {u32 name bytes, name, u32 data bytes, data}
static BOOL appendLinkEntry(NSMutableData *box, NSString *name, NSData *data)
{
	NSData *n = [name dataUsingEncoding:NSUTF8StringEncoding];
	const uint32_t nameBytes = (uint32_t)n.length, dataBytes = (uint32_t)data.length;
	if (!n || !data)
		return NO;
	[box appendBytes:&nameBytes length:4];
	[box appendData:n];
	[box appendBytes:&dataBytes length:4];
	[box appendData:data];
	return YES;
}

// `linked`: the functions `function` calls (a render stage's private linked functions, X.MTL_VISIBLE_FN_REF: RenderBox's custom shaders),
// sent with it in one container that the translator links before translating
static NSError *translateFunctionNow(id<MTLFunction> function, NSArray<id<MTLFunction>> *linked, NSData **blob, uint32_t *buffers);

// A function's translation, once a process for the same AIR: VideoToolbox makes its transfer pipeline anew for each frame (System Settings'
// Displays pane started the translator 14 times a second, 2026-11-21). ponytail: kept for the process's life, unbounded; by the library's
// path or the bitcode's SHA-256 and the name (linked functions: not kept).
static NSError *translateFunction(id<MTLFunction> function, NSArray<id<MTLFunction>> *linked, NSData **blob, uint32_t *buffers)
{
	static NSMutableDictionary<NSString *, NSArray *> *done;
	NSString *key = nil;
	if (!linked.count) {
		if ([(id)function isKindOfClass:[RDNA4MTLFunction class]]) {
			key = [NSString stringWithFormat:@"%@\n%@", ((RDNA4MTLFunction *)function).library.path, function.name];
		} else if (NSData *air = functionAir(function)) {
			uint8_t digest[CC_SHA256_DIGEST_LENGTH];
			CC_SHA256(air.bytes, (CC_LONG)air.length, digest);
			key = [NSString stringWithFormat:@"%@\n%@", [[NSData dataWithBytes:digest length:sizeof digest] base64EncodedStringWithOptions:0],
			                                function.name];
		}
	}
	@synchronized([RDNA4MTLFunction class]) {
		if (NSArray *hit = key ? done[key] : nil) {
			*blob = hit[0];
			*buffers = [hit[1] unsignedIntValue];
			return nil;
		}
	}
	NSError *why = translateFunctionNow(function, linked, blob, buffers);
	if (!why && key && *blob)
		@synchronized([RDNA4MTLFunction class]) {
			done = done ?: [NSMutableDictionary dictionary];
			done[key] = @[ *blob, @(*buffers) ];
		}
	return why;
}

static NSError *translateFunctionNow(id<MTLFunction> function, NSArray<id<MTLFunction>> *linked, NSData **blob, uint32_t *buffers)
{
	// the bundle's own (Contents/Helpers: the translator these functions need, ray queries say), else the paravirtual GPU's helper
	static NSString *const helper = [[NSBundle bundleForClass:[RDNA4MTLFunction class]].bundlePath stringByAppendingPathComponent:@"Contents/Helpers/air2amdgpu"];
	NSString *own = [NSFileManager.defaultManager isExecutableFileAtPath:helper] ? helper : nil;
	NSString *tool = NSProcessInfo.processInfo.environment[@"RDNA4_AIR2AMDGPU"] ?: own ?: @"/usr/local/libexec/rdna4-air2amdgpu";
	// (a short stand-in for long names in file names: Core Image's stitched kernels' run to hundreds of characters, past a file name's 255)
	NSString *stem = function.name.length < 100 ? function.name : [NSString stringWithFormat:@"h%lx", (unsigned long)function.name.hash];
	// numbered: threads translating the same function at once removed each other's files (VideoToolbox's transfer kernel in System
	// Settings' Displays pane: "could not translate" each frame, 2026-11-21)
	static std::atomic<unsigned> calls;
	NSString *fileName = [NSString stringWithFormat:@"%@-%u", stem, calls++];
	NSString *out = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"rdna4mtl-%d-%@.bin", getpid(), fileName]];
	// The input: our library's metallib, or the AIR bitcode of a function Metal's own compiler made (newLibraryWithSource: and the other
	// library calls _MTLDevice answers: an _MTLFunctionInternal, its -bitcodeData)
	// A sandbox (WebKit's and Chrome's GPU processes) may refuse the temporary files and the helper: then the AIR goes through the kext.
	RDNA4MTLDevice *device = (RDNA4MTLDevice *)(function.device ?: gGlDevice);
	NSString *input = nil, *scratch = nil;
	NSData *air = nil;
	if ([(id)function isKindOfClass:[RDNA4MTLFunction class]]) {
		input = ((RDNA4MTLFunction *)function).library.path;
	} else if ((air = functionAir(function))) {
		scratch = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"rdna4mtl-%d-%@.bc", getpid(), fileName]];
		if ([air writeToFile:scratch atomically:NO])
			input = scratch;
		else if (!linked.count)
			return translateByKext(device.gpu, air, function.name, blob, buffers);
	}
	if (!input && !air) {
		// (diagnostic) what such a function has instead: Core Image's (2026-11-21, icons without their layers on our accelerator)
		id f = function;
		auto has = ^NSString *(NSString *sel) {
			if (![f respondsToSelector:NSSelectorFromString(sel)])
				return @"-";
			id v = [f valueForKey:sel];
			return !v ? @"nil" : [v isKindOfClass:[NSData class]] ? [NSString stringWithFormat:@"%lu bytes", (unsigned long)[v length]]
			                                                      : [NSString stringWithFormat:@"%@", [v class]];
		};
		RTRACE(24, @"no AIR: %@ (%@): bitcodeDataInternal %@, filePath %@, airScript %@, stitchingAirScript %@, specializationAirScript %@, "
		       "functionConstantsDictionary %@, precompiledOutput %@, pluginData %@", function.name, [f class], has(@"bitcodeDataInternal"),
		       has(@"filePath"), has(@"airScript"), has(@"stitchingAirScript"), has(@"specializationAirScript"),
		       has(@"functionConstantsDictionary"), has(@"precompiledOutput"), has(@"pluginData"));
		return rdna4Error([NSString stringWithFormat:@"%@: a function of class %@ with no AIR to translate", function.name, [(id)function class]], 0);
	}
	if (linked.count) {
		NSMutableData *box = [NSMutableData dataWithBytes:"RDLK" length:4];
		const uint32_t count = (uint32_t)linked.count + 1;
		[box appendBytes:&count length:4];
		BOOL ok = appendLinkEntry(box, function.name, air ?: [NSData dataWithContentsOfFile:input]);
		for (id<MTLFunction> f in linked)
			ok = ok && appendLinkEntry(box, f.name, functionAir(f));
		NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"rdna4mtl-%d-%@.rdlk", getpid(), fileName]];
		if (scratch)
			[NSFileManager.defaultManager removeItemAtPath:scratch error:nil];
		if (!ok)
			return rdna4Error([NSString stringWithFormat:@"%@: a linked function (%@) without AIR", function.name, [linked valueForKey:@"name"]], 0);
		if (![box writeToFile:path atomically:NO])
			return translateByKext(device.gpu, box, function.name, blob, buffers);
		input = scratch = path;
		air = box;
	}
	// posix_spawn and waitpid, not NSTask: -waitUntilExit runs the calling thread's run loop while it waits, and on an app's main thread
	// that ran the app's own sources, which came back into OpenGL under the context's lock (kitty hung for good, 2026-11-21)
	int fds[2];
	pid_t pid = 0;
	posix_spawn_file_actions_t actions;
	const char *argv[] = { tool.fileSystemRepresentation, input.fileSystemRepresentation, function.name.UTF8String, out.fileSystemRepresentation, NULL };
	bool started = !pipe(fds);
	if (started) {
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
		posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
		posix_spawn_file_actions_addclose(&actions, fds[0]);
		started = !posix_spawn(&pid, argv[0], &actions, NULL, (char *const *)argv, *_NSGetEnviron());
		posix_spawn_file_actions_destroy(&actions);
		close(fds[1]);
		if (!started)
			close(fds[0]);
	}
	if (!started) {   // not allowed to (a sandbox): through the kext
		NSError *why = translateByKext(device.gpu, air ?: [NSData dataWithContentsOfFile:input], function.name, blob, buffers);
		if (scratch)
			[NSFileManager.defaultManager removeItemAtPath:scratch error:nil];
		return why;
	}
	NSMutableData *said = [NSMutableData data];
	char chunk[4096];
	for (ssize_t got; (got = read(fds[0], chunk, sizeof chunk)) > 0 || (got < 0 && errno == EINTR);)
		if (got > 0)
			[said appendBytes:chunk length:(NSUInteger)got];
	close(fds[0]);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	static std::atomic<int> kept;
	if (scratch && getenv("RDNA4_KEEP_AIR"))   // (diagnostic: the AIR translated, kept in the app's temporary directory, numbered)
		[NSFileManager.defaultManager moveItemAtPath:scratch toPath:[scratch stringByAppendingFormat:@".%d", kept++] error:nil];
	else if (scratch)
		[NSFileManager.defaultManager removeItemAtPath:scratch error:nil];
	NSString *text = [[NSString alloc] initWithData:said encoding:NSUTF8StringEncoding];
	NSArray *words = [text componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];   // "<name> <buffers> <written>"
	*blob = [NSData dataWithContentsOfFile:out];
	[NSFileManager.defaultManager removeItemAtPath:out error:nil];
	if (!WIFEXITED(status) || WEXITSTATUS(status) || words.count < 2 || !*blob)
		return rdna4Error([NSString stringWithFormat:@"%@ could not translate %@: %@", tool, function.name, text], 0);
	*buffers = (uint32_t)[words[1] intValue];
	return nil;
}

// ---- compute pipelines

@interface RDNA4MTLComputePipeline : NSObject <MTLComputePipelineState>
@property (readonly) rdna4_program_t program;
@property (readonly) uint32_t buffers;   // the kernargs' buffer addresses: indices 0 .. buffers - 1
@end

@implementation RDNA4MTLComputePipeline {
	RDNA4MTLDevice *_device;
}

// Translate `function` and load its code object; nil and *error on failure.
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device function:(id<MTLFunction>)function error:(NSError **)error
{
	if (!(self = [super init]))
		return nil;
	_device = device;
	NSError *why = [self loadFunction:function];
	if (why && error)
		*error = why;
	return why ? nil : self;
}

- (NSError *)loadFunction:(id<MTLFunction>)function
{
	if (!_device.gpu)
		return rdna4Error(@"the kext's runtime client is not open (root only)", 0);
	NSData *elf = nil;
	NSError *why = translateFunction(function, nil, &elf, &_buffers);
	if (why)
		return why;
	// the kernel's symbol: its name cut at 63 bytes (air2amdgpu.cpp; RDNA4_MAX_NAME)
	NSString *symbol = function.name.length > 63 ? [function.name substringToIndex:63] : function.name;
	const kern_return_t kr = rdna4_load(_device.gpu, elf.bytes, elf.length, symbol.UTF8String, &_program);
	return kr ? rdna4Error([NSString stringWithFormat:@"loading %@", function.name], kr) : nil;
}

- (void)dealloc
{
	if (_program.handle)
		rdna4_unload(_device.gpu, &_program);
}

- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return nil; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (NSUInteger)threadExecutionWidth { return 32; }   // a wave of 32
- (NSUInteger)staticThreadgroupMemoryLength { return (NSUInteger)_program.ldsBytes; }

RDNA4_ANSWER_MISSING
@end

// ---- render pipelines

enum { kMaxBufferIndex = 31, kTableBuffers = 31 };   // a stage's table holds buffers 0-30 (28-30 at RDNA4_MTLC_HIGH_BUFFERS)

// The RGFX header (air2amdgpu.cpp): {"RGFX", stage, code bytes, RSRC1, RSRC2, SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR, attributes}, the code,
// then per value between the stages {u32 name hash, u32 flags: bit 0 flat, bit 1 the point's coordinate}, then {"TXRD", u32 bit n: texture n
// is used} (older translations have none: all used), and {"POSM", u32 attribute, u32 buffer} for a vertex shader whose position is that buffer's
// float4x4 times that attribute.
enum { kRgfxMagic = 0x58464752u, kHead = 8, kTxrd = 0x44525854u, kPosm = 0x4d534f50u };

@interface RDNA4MTLRenderPipeline : NSObject <MTLRenderPipelineState>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error;
@property (readonly) const uint32_t *vs, *ps;   // the RGFX blobs
@property (readonly) BOOL broken;                // a shader did not translate: draws with it are left out
- (uint32_t)texturesOf:(int)stage;               // bit n: the stage's shader uses texture n (its blob's TXRD record)
// its vertex shader's position is the float4x4 in vertex buffer `buffer` times attribute `attribute` (its blob's POSM record)
- (BOOL)positionFrom:(uint32_t *)attribute matrix:(uint32_t *)buffer;
@property (readonly) NSString *names;            // "vertex / fragment" (the trace)
@property BOOL described;                         // its first draw was logged
@property (readonly) uint32_t outs, ins;        // values the vertex function writes, the fragment function reads
@property (readonly) const uint32_t *inputCntl; // SPI_PS_INPUT_CNTL_0 .. ins - 1
@property (readonly) BOOL sprite;               // a fragment input is the point's coordinate
@property (readonly) BOOL exportsNothing;       // the fragment code has no export instruction (drawn as a shader without a colour)
@property (readonly) uint64_t code;             // GPU address: VS at +0, PS at +RDNA4_TRI_PS_OFFSET
@property (readonly) MTLPixelFormat format;     // colour attachment 0's
- (const uint32_t *)blendOf:(NSUInteger)attachment;   // its blend state as the PV host reads it (pvdraw.hpp readBlendStates): [2] on,
                                                    // [3-5] colour's source, destination factor, operation, [6-8] alpha's, [9] write mask
- (MTLPixelFormat)formatOf:(NSUInteger)attachment;
@property (readonly) const uint32_t *attributes;    // the vertex descriptor as the vertex stage's records (RDNA4_MTLC_TABLE_ATTRIBUTES), 16 x 4
@end

// One draw as recorded: everything the device needs to run it.
enum { kStageTextures = 16 };   // a stage's table holds samplers 0-15 and the textures that have levels (include/rdna4mtlc.h)
enum { kTextures = RDNA4_MTLC_TEXTURES };   // and textures 0-30 as T#s
enum { kMaxTextures = RDNA4_MTLC_MAX_TEXTURES, kHandleTextures = kMaxTextures - kTextures };   // 31-127 through handles (include/rdna4mtlc.h)

typedef struct {
	uint64_t table[2][kTableBuffers];   // per stage, its buffers' GPU addresses by index (0: unbound)
	uint32_t textures[2][kTextures][8], samplers[2][kStageTextures][4];   // per stage, T#s and S#s (0: unbound)
	uint32_t type, start, count, instances, baseInstance;
	float viewport[4];                  // x, y, width, height in pixels
	uint32_t scissor[4];                // x, y, width, height in pixels (the whole target when none was set)
	uint32_t covered[4];                // what of it the draw can write: its vertices' bounds where those are known (bound:), else the scissor
	uint32_t attachment;                // the colour attachment the draw goes into (RDNA4_MTLC_TABLE_ATTACHMENT)
	const uint8_t *levelT;              // for shaders that sample explicit levels: per stage, levels 1-7, textures 0-15, a T# each (else NULL)
	const uint64_t *handles;            // for shaders that use texture handles: per stage, textures 31-127's T# slots (else NULL)
	float blendColour[4];
	uint64_t indexGpu;                  // an indexed draw's indices (0: not indexed), 16- or 32-bit
	uint32_t indexBytes;
	uint64_t depthGpu;                  // the pass's Depth32Float attachment (0: none), its size, the state's test and write
	uint32_t depthWidth, depthHeight, depthCompare, depthWrite;
	uint64_t stencilGpu;                // the pass's Stencil8 attachment (0: none; the size the depth's), the state's words, the references
	uint32_t stencilDepthControl, stencilControl, stencilReadMask, stencilWriteMask, stencilRef;
	uint32_t depthOnly;                 // the pass has no colour attachment: drawn into a stand-in (standInFor:), colour not written
} RDNA4Draw;

typedef struct { uint64_t src, dst, bytes; uint32_t pattern; } RDNA4Copy;   // GPU addresses; src 0: a fill with the pattern

@interface RDNA4MTLDevice ()
- (BOOL)gfx;   // the client may submit gfx IBs and the device's gfx buffers exist
- (uint64_t)gfxRings;
- (NSError *)draw:(const RDNA4Draw *)draw pipeline:(RDNA4MTLRenderPipeline *)pipeline target:(RDNA4MTLTexture *)target;
- (RDNA4MTLTexture *)standInFor:(RDNA4MTLTexture *)depth;   // a BGRA8 target of the depth's size and samples, kept, never written
- (NSError *)drainGfx;
- (NSError *)flushBatch;   // the draws and barriers written so far onto the gfx ring (one submit)
- (dispatch_queue_t)work;
- (const rdna4_program_t *)resolveKernel:(BOOL)srgb;   // rdna4_resolve or rdna4_resolve_srgb, loaded on first use (NULL: not loaded)
- (uint64_t)spareBuffer;             // what an unbound buffer slot points at: the zeroed spare buffer
- (BOOL)drawnInFlight:(RDNA4MTLTexture *)t;   // a draw not yet waited for draws into it, a view of it or the texture it views
// On the card, before the next draw: a wait for the draws in flight, then the copies (none: the wait alone)
- (NSError *)gpuBarrier:(const RDNA4Copy *)copies count:(uint32_t)count;
// A kernel on the gfx ring behind the draws before it, not waited for (RDNA4_RINGDISPATCH=1); NO: not one it takes (scratch, other user data)
- (BOOL)canDispatchOnRing:(const rdna4_program_t *)k;
- (NSError *)dispatchOnRing:(const rdna4_program_t *)k groups:(const uint32_t[3])groups size:(const uint32_t[3])size args:(NSData *)args;
@end

// Whether the card is done with a resource, as Apple's GL-on-Metal renderer asks (-isComplete, -isWriteComplete, -waitUntilComplete,
// 2026-11-21): command buffers run in order on the device's work queue, each until its work is done, so the work queue drained is every
// earlier use done. ponytail: waits for all of the device's work, not this resource's.
static void deviceIdle(RDNA4MTLDevice *device)
{
	if (dispatch_queue_t work = [device work])
		dispatch_sync(work, ^{});
}


@implementation RDNA4Held {
	RDNA4MTLDevice *_device;
	RDNA4MTLBuffer *_host;   // the surface's import (its GPU address: what is copied from and into)
	IOSurfaceRef _surface;   // (the import holds it)
	rdna4_buffer_t _vram;
	uint64_t _bytes, _dirty[kWrittenRanges][2];   // bytes drawn in VRAM since the last write-back
	uint32_t _dirtyCount;
	uint32_t _seed;
	std::atomic<bool> _valid;
	BOOL _presented;   // the kext presents the surface's frames from _vram (rdna4_present_source)
}

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device surface:(IOSurfaceRef)surface host:(RDNA4MTLBuffer *)host
{
	if (!(self = [super init]))
		return nil;
	_bytes = (host.length + 255) & ~255ull;
	if (rdna4_alloc(device.gpu, _bytes, &_vram)) {
		rdna4Error([NSString stringWithFormat:@"%llu bytes of VRAM for IOSurface %u", _bytes, IOSurfaceGetID(surface)], 0);
		return nil;
	}
	_device = device, _host = host, _surface = surface;
	return self;
}

- (void)dealloc
{
	[self present:NO];
	rdna4_free(_device.gpu, &_vram);
}

- (uint64_t)gpu { return _vram.gpu; }
- (uint32_t)surfaceID { return IOSurfaceGetID(_surface); }

// The kext presents the surface's frames from the VRAM copy, or from the surface's memory again.
- (void)present:(BOOL)on
{
	@synchronized(self) {
		if (on == _presented)
			return;
		const kern_return_t kr = rdna4_present_source(_device.gpu, IOSurfaceGetID(_surface), on ? &_vram : NULL);
		_presented = on && !kr;
		if (kr && on)
			RTRACE(4, @"IOSurface %u not presented from VRAM (0x%x)", IOSurfaceGetID(_surface), kr);
	}
}

// ponytail: what the card drew and is not written back yet is lost when the CPU writes the surface outside a step of the device's
// (a step writes it back first); WindowServer does not write its frames with the CPU.
- (void)cpuTouched
{
	_valid = false;
	[self present:NO];
}

- (NSError *)sync
{
	const uint32_t seed = IOSurfaceGetSeed(_surface);
	if (_valid && seed == _seed)
		return nil;
	_dirtyCount = 0;   // (the surface is what counts now)
	const RDNA4Copy in = { _host.gpuAddress, _vram.gpu, _host.length & ~3ull };
	NSError *why = [_device gpuBarrier:&in count:1];
	if (!why) {
		_valid = true, _seed = seed;
		statAdd(kStatHeldIn, in.bytes);
		[self present:YES];
	}
	return why;
}

- (void)dirty:(uint64_t)lo to:(uint64_t)hi
{
	hi = MIN((hi + 3) & ~3ull, _host.length & ~3ull);
	if ((lo & ~3ull) < hi)
		rangesAdd(_dirty, &_dirtyCount, lo & ~3ull, hi);
}

- (BOOL)writeBack:(NSError **)why
{
	const uint32_t n = _valid ? _dirtyCount : 0;   // nothing drawn, or the CPU wrote the surface since: it counts
	_dirtyCount = 0;
	if (!n)
		return NO;
	RDNA4Copy out[kWrittenRanges];
	for (uint32_t i = 0; i < n; i++) {
		out[i] = (RDNA4Copy){ _vram.gpu + _dirty[i][0], _host.gpuAddress + _dirty[i][0], _dirty[i][1] - _dirty[i][0] };
		statAdd(kStatHeldOut, out[i].bytes);
	}
	*why = [_device gpuBarrier:out count:n];
	return YES;
}
@end

// rdna4-nativeheld: on unless the boot arguments say rdna4-nativeheld=0 (the driver's default since 2026-11-21, src/trial.cpp), and in
// WindowServer only: the kext presents its frames from VRAM, while an app's drawables are read by WindowServer from the surfaces'
// memory. Held in iTerm2, its frames reached the surface only at the idle or once-a-second write-back: the end of the text vanished
// and came back as it typed (2026-11-21).
static BOOL heldOn(void)
{
	static int on = -1;
	if (on < 0) {
		char args[1024] = {};
		size_t n = sizeof args - 1;
		on = !strcmp(getprogname(), "WindowServer") &&
		     (sysctlbyname("kern.bootargs", args, &n, NULL, 0) || strstr(args, "rdna4-nativeheld=0") == NULL);
	}
	return on;
}

// The surface's held copy (`make`: made if there is none). Kept 5 s after it was last asked for, and
// as long as a texture uses it: one copy a surface.
static NSMapTable<NSNumber *, RDNA4Held *> *gHeldAlive;   // weak: while a texture or `kept` holds it (@synchronized RDNA4Held)

static RDNA4Held *heldFor(IOSurfaceRef surface, BOOL make, RDNA4MTLDevice *device, RDNA4MTLBuffer *host)
{
	if (!heldOn())
		return nil;
	static NSMutableDictionary<NSNumber *, NSArray *> *kept;   // ID -> {held, last asked for (s)}
	static CFAbsoluteTime swept;
	const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	NSNumber *key = @(IOSurfaceGetID(surface));
	@synchronized([RDNA4Held class]) {
		gHeldAlive = gHeldAlive ?: [NSMapTable strongToWeakObjectsMapTable];
		kept = kept ?: [NSMutableDictionary dictionary];
		if (now - swept > 1) {
			swept = now;
			for (NSNumber *k in kept.allKeys)
				if (now - [kept[k][1] doubleValue] > 5)
					[kept removeObjectForKey:k];
		}
		RDNA4Held *h = [gHeldAlive objectForKey:key];
		if (!h && make && host && (h = [[RDNA4Held alloc] initWithDevice:device surface:surface host:host]))
			[gHeldAlive setObject:h forKey:key];
		if (h)
			kept[key] = @[ h, @(now) ];
		return h;
	}
}

// (work queue) What the card drew into every held surface written back into it, waited for: the frames are presented from VRAM, and the
// surfaces' memory (what other processes and the CPU read) kept current when WindowServer goes idle and at least each second.
// `shownToo` NO: only those no display shows from VRAM (debug.rdna4_pvscanoutsids): their memory is all there is of them outside
// WindowServer (a screenshot's surface).
static NSError *heldWriteBackAll(RDNA4MTLDevice *device, BOOL shownToo = YES)
{
	NSArray<RDNA4Held *> *all;
	@synchronized([RDNA4Held class]) {
		all = gHeldAlive.objectEnumerator.allObjects;
	}
	uint32_t shown[32];
	size_t len = sizeof shown;
	if (shownToo || sysctlbyname("debug.rdna4_pvscanoutsids", shown, &len, NULL, 0))
		len = 0;   // (an older kext: none known shown, all written back)
	NSError *why = nil;
	BOOL any = NO;
	for (RDNA4Held *h in all) {
		BOOL skip = NO;
		for (size_t i = 0; !shownToo && i < len / 4; i++)
			skip |= shown[i] == h.surfaceID;
		if (!why && !skip)
			any |= [h writeBack:&why];
	}
	if (any && !why)
		why = [device gpuBarrier:NULL count:0];   // the L2 written back (RELEASE_MEM) after the copies
	return why ?: any ? [device drainGfx] : nil;
}

// (work queue, after each command buffer) The held surfaces written back 100 ms after the last command buffer, or now if a second went
// by without (an animation that never stops).
static void heldIdle(RDNA4MTLDevice *device)
{
	static uint64_t generation, lastBack;
	if (!heldOn())
		return;
	const uint64_t now = nowUs(), mine = ++generation;
	if (now - lastBack > 1000000) {
		lastBack = now;
		(void)heldWriteBackAll(device);
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), device.work, ^{
		if (generation != mine)
			return;
		lastBack = nowUs();
		(void)heldWriteBackAll(device);
	});
}

@implementation RDNA4MTLRenderPipeline {
	RDNA4MTLDevice *_device;
	NSData *_blob[2];
	uint32_t _cntl[16];
	rdna4_buffer_t _codeBuffer;
	uint32_t _blends[8][10], _attributeRecords[16][4];
	BOOL _broken;   // a shader did not translate: its draws are left out
	MTLPixelFormat _formats[8];
	uint32_t _textures[2], _posm[2];   // _posm: 0 or 1 + the attribute, the buffer
}

- (uint32_t)texturesOf:(int)stage { return _textures[stage & 1]; }
- (BOOL)positionFrom:(uint32_t *)attribute matrix:(uint32_t *)buffer
{
	*attribute = _posm[0] - 1, *buffer = _posm[1];
	return _posm[0] != 0;
}

- (const uint32_t *)blendOf:(NSUInteger)k { return _blends[k & 7]; }
- (MTLPixelFormat)formatOf:(NSUInteger)k { return _formats[k & 7]; }
- (const uint32_t *)attributes { return &_attributeRecords[0][0]; }

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error
{
	if (!(self = [super init]))
		return nil;
	_device = device;
	NSError *why = [self build:descriptor];
	if (why && error)
		*error = why;
	return why ? nil : self;
}

// The bundle's rdna4_no_fragment (rdna4.metal), once a process.
static id<MTLFunction> noFragmentFunction(RDNA4MTLDevice *device)
{
	static id<MTLFunction> function;
	static std::once_flag once;
	std::call_once(once, ^{
		NSString *path = [[NSBundle bundleForClass:[RDNA4MTLFunction class]] pathForResource:@"rdna4" ofType:@"metallib"];
		function = path ? [[device newLibraryWithURL:[NSURL fileURLWithPath:path] error:nil] newFunctionWithName:@"rdna4_no_fragment"] : nil;
	});
	return function;
}

- (NSError *)build:(MTLRenderPipelineDescriptor *)desc
{
	if (!_device.gfx)
		return rdna4Error(@"no gfx IBs: the kext's runtime client is not open (root only), or the boot has no client gfx IBs (rdna4-gfx=2)", 0);
	// no fragment function (a depth-only pass; MoltenVK's in vkQuake, refused before: the game stopped): one that writes nothing
	id<MTLFunction> fn[2] = { desc.vertexFunction, desc.fragmentFunction ?: noFragmentFunction(_device) };
	if (!fn[0] || !fn[1] || fn[0].device != _device || fn[1].device != _device)
		return rdna4Error([NSString stringWithFormat:@"a render pipeline needs a vertex and a fragment function of this device so far: %@ (%@, device %@) / %@ (%@, "
		                                             @"device %@)", fn[0].name, [(id)fn[0] class], [(id)fn[0].device class], fn[1].name,
		                                             [(id)fn[1] class], [(id)fn[1].device class]], 0);
	MTLRenderPipelineColorAttachmentDescriptor *a = desc.colorAttachments[0];
	_format = a.pixelFormat;
	const MTLPixelFormat zf = desc.depthAttachmentPixelFormat, sf = desc.stencilAttachmentPixelFormat;
	if ((zf != MTLPixelFormatInvalid && zf != MTLPixelFormatDepth32Float && zf != MTLPixelFormatDepth16Unorm && zf != MTLPixelFormatDepth32Float_Stencil8) ||
	    (sf != MTLPixelFormatInvalid && sf != MTLPixelFormatStencil8 && sf != MTLPixelFormatDepth32Float_Stencil8))
		return rdna4Error([NSString stringWithFormat:@"depth format %lu, stencil format %lu: Depth32Float, Depth16Unorm, Stencil8 and "
		                                             @"Depth32Float_Stencil8 only so far", (unsigned long)zf, (unsigned long)sf], 0);
	for (NSUInteger i = 0; i < 8; i++) {   // each attachment's format and blend state (a draw goes into one of them: the PV host's choice)
		MTLRenderPipelineColorAttachmentDescriptor *c = desc.colorAttachments[i];
		_formats[i] = c.pixelFormat;
		if (c.pixelFormat != MTLPixelFormatInvalid && !isTargetFormat(c.pixelFormat))
			return rdna4Error([NSString stringWithFormat:@"colour attachment %lu of format %lu: not a target format yet", (unsigned long)i,
			                                             (unsigned long)c.pixelFormat], 0);
		const uint32_t blend[10] = { (uint32_t)i, 0, (uint32_t)c.blendingEnabled, (uint32_t)c.sourceRGBBlendFactor,
			                         (uint32_t)c.destinationRGBBlendFactor, (uint32_t)c.rgbBlendOperation, (uint32_t)c.sourceAlphaBlendFactor,
			                         (uint32_t)c.destinationAlphaBlendFactor, (uint32_t)c.alphaBlendOperation, (uint32_t)c.writeMask };
		memcpy(_blends[i], blend, sizeof blend);
	}
	if (desc.rasterSampleCount != 1 && desc.rasterSampleCount != 2 && desc.rasterSampleCount != 4)
		return rdna4Error([NSString stringWithFormat:@"%lu samples: 1, 2 and 4 only so far", (unsigned long)desc.rasterSampleCount], 0);
	// the vertex descriptor as pvdraw.hpp readVertexDescriptor makes it from the PV wire: {offset, stride, buffer | kind << 8 | components << 12
	// | BGRA << 15 | step function << 16, step rate}; an attribute of a format not handled stays zero (the shader reads (0, 0, 0, 1))
	MTLVertexDescriptor *vd = desc.vertexDescriptor;
	for (NSUInteger i = 0; vd && i < 16; i++) {
		MTLVertexAttributeDescriptor *at = vd.attributes[i];
		if (at.format == MTLVertexFormatInvalid)
			continue;
		const uint32_t f = vertexFormat((uint32_t)at.format), buffer = (uint32_t)at.bufferIndex;
		if (!f || buffer >= 31) {
			RLOG(@"RDNA4MTL: vertex attribute %lu: format %lu of buffer %u not supported: read as 0", (unsigned long)i, (unsigned long)at.format, buffer);
			continue;
		}
		MTLVertexBufferLayoutDescriptor *l = vd.layouts[buffer];
		// as Metal has them: step function constant (0: every vertex reads the first element; GL-on-Metal's constant attributes, a
		// colour of one float4 that was read per vertex past its end and shaded kitty's window to black in one corner, 2026-11-21),
		// per vertex (1, the default) or per instance (2, rate 0: always the first)
		const uint32_t step = (uint32_t)l.stepFunction, rate = (uint32_t)l.stepRate;
		const uint32_t rec[4] = { (uint32_t)at.offset, (uint32_t)l.stride, buffer | (f & 15) << 8 | (f >> 4 & 7) << 12 | (f >> 7 & 1) << 15 | step << 16, rate };
		memcpy(_attributeRecords[i], rec, sizeof rec);
	}
	for (int k = 0; k < 2; k++) {
		uint32_t unused;
		NSData *blob = nil;
		_names = [NSString stringWithFormat:@"%@ / %@", fn[0].name, fn[1].name];
		NSArray *linked = (k ? desc.fragmentLinkedFunctions : desc.vertexLinkedFunctions).privateFunctions;
		NSError *why = translateFunction(fn[k], linked, &blob, &unused);
		if (why)   // once more: the helper may have been out of memory or busy
			why = translateFunction(fn[k], linked, &blob, &unused);
		if (why) {
			// A pipeline whose draws are left out, not none: Core Animation aborts WindowServer when one fails to build (the session
			// logged out when TkfhBvcmXc_Iclr did not translate, 2026-11-20). Stand-in shaders (s_endpgm) keep the rest of the setup whole.
			RLOG(@"RDNA4MTL: %@: its draws are left out", _names);
			_broken = YES;
			const uint32_t stub[9] = { kRgfxMagic, (uint32_t)k + 1, 4, 0, 0, 0, 0, k ? 0x80000000u : 0, 0xbfb00000u };
			blob = [NSData dataWithBytes:stub length:sizeof stub];
		}
		_blob[k] = blob;
	}
	const uint32_t *vs = (const uint32_t *)_blob[0].bytes, *ps = (const uint32_t *)_blob[1].bytes;
	const uint32_t room[2] = { RDNA4_TRI_PS_OFFSET - 4 * RDNA4_TRI_SHADER_PAD, RDNA4_TRI_CODE_BYTES - RDNA4_TRI_PS_OFFSET - 4 * RDNA4_TRI_SHADER_PAD };
	for (int k = 0; k < 2; k++) {
		const uint32_t *h = (const uint32_t *)_blob[k].bytes;
		if (_blob[k].length < 4 * kHead || h[0] != kRgfxMagic || h[1] != (uint32_t)k + 1 || h[2] > room[k] || (h[7] & 0xffff) > 16 ||
		    4ull * kHead + h[2] + 8ull * (h[7] & 0xffff) > _blob[k].length)
			return rdna4Error([NSString stringWithFormat:@"%@: not a %s shader that fits its slot", fn[k].name, k ? "fragment" : "vertex"], 0);
		if (h[7] & 0x7fff0000u & ~(k ? 0x7ffd0000u : 0x00010000u))   // (several attachments: the PV host's choice; bit 18 dual source, 19 depth)
			return rdna4Error([NSString stringWithFormat:@"%@: attributes 0x%x not supported yet", fn[k].name, h[7]], 0);
	}
	for (int k = 0; k < 2; k++) {   // the records after the values (TXRD, POSM)
		const uint32_t *h = (const uint32_t *)_blob[k].bytes, *rec = h + kHead + h[2] / 4 + 2 * (h[7] & 0xffff);
		const uint8_t *end = (const uint8_t *)h + _blob[k].length;
		const BOOL txrd = (const uint8_t *)(rec + 2) <= end && rec[0] == kTxrd;
		_textures[k] = txrd ? rec[1] : 0xffff;
		if (!k && txrd && (const uint8_t *)(rec + 5) <= end && rec[2] == kPosm && rec[3] < 16 && rec[4] < kTableBuffers)
			_posm[0] = rec[3] + 1, _posm[1] = rec[4];
	}
	// A fragment function whose colour LLVM left out entirely (Core Animation's TkfhBvcmA2Xh_IsrcM1 is s_endpgm alone) has no export, while its
	// header says it has a colour: drawn with a colour format the hardware waits for an export forever (WindowServer's gfx ring hung,
	// 2026-11-20). An export is a dword with bits 31-26 111110 (gfx12 VEXPORT); none in the code: drawn as a shader without a colour.
	_exportsNothing = YES;
	for (uint32_t i = 0; i < ps[2] / 4 && _exportsNothing; i++)
		_exportsNothing = (ps[kHead + i] >> 26) != 0x3e;
	_outs = vs[7] & 0xffff;
	_ins = ps[7] & 0xffff;
	// input k reads the vertex output with its name (SPI_PS_INPUT_CNTL_k OFFSET, FLAT_SHADE), as the PV host's runDraw
	const uint32_t *outTable = vs + kHead + vs[2] / 4, *inTable = ps + kHead + ps[2] / 4;
	for (uint32_t k = 0; k < _ins; k++) {
		const uint32_t *in = inTable + 2 * k;
		uint32_t slot = 0;
		if (in[1] & 2) {
			_cntl[k] = 1u << 17;   // PT_SPRITE_TEX
			_sprite = YES;
			continue;
		}
		while (slot < _outs && outTable[2 * slot] != in[0])
			slot++;
		if (slot == _outs)
			return rdna4Error([NSString stringWithFormat:@"fragment input %u is not written by %@", k, fn[0].name], 0);
		_cntl[k] = slot | (in[1] & 1) << 10;
	}
	// The code: VS at +0, PS at +RDNA4_TRI_PS_OFFSET, s_code_end around them (the SQ's prefetch); the VS's attribute ring literals patched
	// (nggcol.s: ring VA low, VA high | swizzle | stride 16 bytes a value, ring bytes), as the PV host's ringLiteral.
	NSMutableData *image = [NSMutableData dataWithLength:RDNA4_TRI_CODE_BYTES];
	uint32_t *code = (uint32_t *)image.mutableBytes;
	for (uint32_t i = 0; i < RDNA4_TRI_CODE_BYTES / 4; i++)
		code[i] = 0xbf9f0000u;
	memcpy(code, vs + kHead, vs[2]);
	memcpy((uint8_t *)code + RDNA4_TRI_PS_OFFSET, ps + kHead, ps[2]);
	const uint64_t rings = _device.gfxRings;
	for (uint32_t i = 0; _outs && i < RDNA4_TRI_PS_OFFSET / 4; i++)
		code[i] = code[i] == 0xc01a0001u ? (uint32_t)rings
		        : code[i] == 0xc01a0002u ? (uint32_t)((rings >> 32) & 0xffff) | 0xc0000000u | 16u * _outs << 16
		        : code[i] == 0xc01a0003u ? (uint32_t)RDNA4_TRICOL(kColAttrRingBytes)
		                                 : code[i];
	kern_return_t kr = rdna4_alloc(_device.gpu, RDNA4_TRI_CODE_BYTES, &_codeBuffer);
	if (!kr)
		kr = rdna4_write(_device.gpu, &_codeBuffer, 0, code, RDNA4_TRI_CODE_BYTES);
	return kr ? rdna4Error(@"the shaders' code buffer", kr) : nil;
}

- (void)dealloc
{
	if (_codeBuffer.handle)
		rdna4_free(_device.gpu, &_codeBuffer);
}

- (BOOL)broken { return _broken; }
- (const uint32_t *)vs { return (const uint32_t *)_blob[0].bytes; }
- (const uint32_t *)ps { return (const uint32_t *)_blob[1].bytes; }
- (const uint32_t *)inputCntl { return _cntl; }
- (uint64_t)code { return _codeBuffer.gpu; }
- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return nil; }

RDNA4_ANSWER_MISSING
@end

// ---- sampler and depth-stencil states

@interface RDNA4MTLSamplerState : NSObject <MTLSamplerState>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLSamplerDescriptor *)d;
@property (readonly) const uint32_t *words;   // its S# (SQ_IMG_SAMP_WORD0-3)
@end

@implementation RDNA4MTLSamplerState {
	RDNA4MTLDevice *_device;
	uint32_t _s[4];
	uint64_t _handle;   // (gpuResourceID) its S#'s slot
}
@synthesize label;

// As the PV host's (src/pvgpu.cpp stageTextures): Metal's address modes as the hardware's CLAMP_*, the filters, unnormalised coordinates.
// ponytail: no anisotropy, LOD unclamped, border transparent black.
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLSamplerDescriptor *)d
{
	if (!(self = [super init]))
		return nil;
	static const uint32_t clamp[8] = { 2, 3, 0, 1, 6, 6, 2, 2 };   // edge, mirror-clamp-to-edge, repeat, mirror, zero, border
	_device = device;
	_s[0] = clamp[d.sAddressMode & 7] | clamp[d.tAddressMode & 7] << 3 | clamp[d.rAddressMode & 7] << 6 | (d.normalizedCoordinates ? 0 : 1u << 15);
	_s[1] = 0xfffu << 13;
	_s[2] = (uint32_t)(d.magFilter & 3) << 20 | (uint32_t)(d.minFilter & 3) << 22 | (uint32_t)(d.mipFilter & 3) << 26;
	_s[3] = 0;
	return self;
}

- (const uint32_t *)words { return _s; }
- (id<MTLDevice>)device { return _device; }
- (MTLResourceID)gpuResourceID
{
	@synchronized(self) {
		if (!_handle)
			_handle = [_device handle:0 words:_s count:4];
	}
	return (MTLResourceID){ ._impl = _handle };
}
- (void)dealloc { [_device freeHandle:_handle]; }

RDNA4_ANSWER_MISSING
@end

// An argument buffer's encoder (MoltenVK's descriptor sets: it makes them whatever tier we answer). Its layout is the shader's struct as Apple's
// compiler lays it out: the arguments in index order, a buffer, texture or sampler an 8-byte handle (gpuAddress, gpuResourceID: air2amdgpu reads
// them as such), constant data at its own size and alignment, arrays element after element.
@interface RDNA4MTLArgumentEncoder : NSObject <MTLArgumentEncoder>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device arguments:(NSArray<MTLArgumentDescriptor *> *)arguments;
@end

@implementation RDNA4MTLArgumentEncoder {
	RDNA4MTLDevice *_device;
	std::vector<std::array<NSUInteger, 4>> _slots;   // index, count, offset, element size
	NSUInteger _length, _alignment;
	uint8_t *_at;   // the argument buffer's bytes at the encoded offset; nil: none set
}
@synthesize label;

static NSUInteger argumentSize(MTLDataType t)
{
	switch (t) {
	case MTLDataTypeChar: case MTLDataTypeUChar: case MTLDataTypeBool: return 1;
	case MTLDataTypeShort: case MTLDataTypeUShort: case MTLDataTypeHalf: case MTLDataTypeChar2: case MTLDataTypeUChar2: return 2;
	case MTLDataTypeFloat: case MTLDataTypeInt: case MTLDataTypeUInt: case MTLDataTypeShort2: case MTLDataTypeUShort2: case MTLDataTypeHalf2:
	case MTLDataTypeChar4: case MTLDataTypeUChar4: return 4;
	case MTLDataTypeFloat2: case MTLDataTypeInt2: case MTLDataTypeUInt2: case MTLDataTypeShort4: case MTLDataTypeUShort4: case MTLDataTypeHalf4: return 8;
	case MTLDataTypeFloat3: case MTLDataTypeFloat4: case MTLDataTypeInt3: case MTLDataTypeInt4: case MTLDataTypeUInt3: case MTLDataTypeUInt4: return 16;
	default: return 8;   // a buffer, texture, sampler or other object: its handle
	}
}

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device arguments:(NSArray<MTLArgumentDescriptor *> *)arguments
{
	if (!(self = [super init]))
		return nil;
	_device = device;
	_alignment = 8;
	NSArray *sorted = [arguments sortedArrayUsingComparator:^NSComparisonResult(MTLArgumentDescriptor *a, MTLArgumentDescriptor *b) {
		return [@(a.index) compare:@(b.index)];
	}];
	for (MTLArgumentDescriptor *a in sorted) {
		const NSUInteger size = argumentSize(a.dataType), align = MAX(size, a.constantBlockAlignment ?: 1), count = MAX(a.arrayLength, (NSUInteger)1);
		_length = (_length + align - 1) / align * align;
		_slots.push_back({ a.index, count, _length, size });
		_length += size * count;
		_alignment = MAX(_alignment, align);
	}
	_length = (_length + _alignment - 1) / _alignment * _alignment;
	return self;
}

- (id<MTLDevice>)device { return _device; }
- (NSUInteger)encodedLength { return _length; }
- (NSUInteger)alignment { return _alignment; }
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset { _at = buffer ? (uint8_t *)buffer.contents + offset : nil; }
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer startOffset:(NSUInteger)start arrayElement:(NSUInteger)element
{
	[self setArgumentBuffer:buffer offset:start + element * _length];
}

// Where argument `index` is in the buffer (an array's element by its own index), or nil.
- (uint8_t *)at:(NSUInteger)index
{
	for (const auto &s : _slots)
		if (_at && index >= s[0] && index < s[0] + s[1])
			return _at + s[2] + (index - s[0]) * s[3];
	return nil;
}
- (void)put:(uint64_t)handle at:(NSUInteger)index
{
	if (uint8_t *p = [self at:index])
		memcpy(p, &handle, 8);
}
- (void *)constantDataAtIndex:(NSUInteger)index { return [self at:index]; }
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index { [self put:buffer ? buffer.gpuAddress + offset : 0 at:index]; }
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])buffers offsets:(const NSUInteger[])offsets withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++)
		[self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index { [self put:texture ? texture.gpuResourceID._impl : 0 at:index]; }
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])textures withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++)
		[self setTexture:textures[i] atIndex:range.location + i];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index { [self put:sampler ? sampler.gpuResourceID._impl : 0 at:index]; }
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])samplers withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++)
		[self setSamplerState:samplers[i] atIndex:range.location + i];
}
- (id<MTLArgumentEncoder>)newArgumentEncoderForBufferAtIndex:(NSUInteger)index { return nil; }   // nested argument buffers: none so far

RDNA4_ANSWER_MISSING
@end

@interface RDNA4MTLDepthStencilState : NSObject <MTLDepthStencilState>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLDepthStencilDescriptor *)d;
@property (readonly) uint32_t compare;   // Metal's compare function: the hardware's ZFUNC
@property (readonly) BOOL write;
// The stencil test as the PV host's runDraw: DB_DEPTH_CONTROL's stencil part (STENCIL_ENABLE, BACKFACE_ENABLE, the front and back
// functions), DB_STENCIL_CONTROL (the operations), the read and write masks (front in 0-7, back in 8-15)
@property (readonly) uint32_t stencilDepthControl, stencilControl, readMask, writeMask;
@end

@implementation RDNA4MTLDepthStencilState {
	RDNA4MTLDevice *_device;
	BOOL _stencil;   // a face has a stencil descriptor
}
@synthesize label;
// (Apple's GL-on-Metal renderer asks these, 2026-11-21)
- (BOOL)readsDepth { return _compare != MTLCompareFunctionAlways || _write; }
- (BOOL)writesDepth { return _write; }
- (BOOL)readsStencil { return _stencil; }
- (BOOL)writesStencil { return _stencil && (_writeMask & 0xffff) != 0; }

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLDepthStencilDescriptor *)d
{
	if ((self = [super init])) {
		_device = device;
		_compare = (uint32_t)d.depthCompareFunction & 7;
		_write = d.depthWriteEnabled;
		// Metal's keep, zero, replace, increment clamp, decrement clamp, invert, increment wrap, decrement wrap: STENCIL_KEEP, ZERO,
		// REPLACE_TEST, ADD_CLAMP, SUB_CLAMP, INVERT, ADD_WRAP, SUB_WRAP; its compare functions the hardware's. A face without a
		// descriptor: always passes, keeps.
		static const uint8_t kOp[8] = { 0, 1, 3, 5, 6, 7, 8, 9 };
		MTLStencilDescriptor *face[2] = { d.frontFaceStencil, d.backFaceStencil };
		_stencil = face[0] || face[1];
		uint32_t fn[2], ops[2], rm[2], wm[2];
		for (int i = 0; i < 2; i++) {
			fn[i] = face[i] ? (uint32_t)face[i].stencilCompareFunction & 7 : 7;
			ops[i] = face[i] ? kOp[face[i].stencilFailureOperation & 7] | kOp[face[i].depthStencilPassOperation & 7] << 4 |
			                       kOp[face[i].depthFailureOperation & 7] << 8 : 0;   // FAIL, ZPASS, ZFAIL
			rm[i] = face[i] ? face[i].readMask & 0xff : 0xff;
			wm[i] = face[i] ? face[i].writeMask & 0xff : 0xff;
		}
		_stencilDepthControl = 1 | 1u << 7 | fn[0] << 8 | fn[1] << 20;
		_stencilControl = ops[0] | ops[1] << 12;
		_readMask = rm[0] | rm[1] << 8;
		_writeMask = wm[0] | wm[1] << 8;
	}
	return self;
}

- (id<MTLDevice>)device { return _device; }

RDNA4_ANSWER_MISSING
@end

// ---- command queues, buffers and encoders

typedef NSError * (^RDNA4Op)(void);   // a recorded step: the work queue runs them in order and stops at the first error

@interface RDNA4MTLCommandQueue : NSObject <MTLCommandQueue>
@property (readonly) dispatch_queue_t handlers;   // where its command buffers' handlers run: the app's (setCompletionQueue:) or its own
@property (readonly) dispatch_queue_t gate;       // its command buffers on their way to the work queue, in order (events waited for there)
- (void)reserve:(id)commandBuffer;     // enqueue: its place in the queue's order
- (void)committed:(id)commandBuffer;   // commit: it and the committed ones after it go to the work queue, in enqueue order
@end

@interface RDNA4MTLCommandBuffer : NSObject <MTLCommandBuffer>
- (instancetype)initWithQueue:(RDNA4MTLCommandQueue *)queue;
- (void)submit;                   // its steps onto the device's work queue
- (void)record:(RDNA4Op)op;       // runs once the draws before it have finished
// set*Bytes data: copied into a 256 KiB chunk of the command buffer's (256-byte aligned), so that thousands of them are not a buffer each
// (WindowServer ran the kext's runtime out of buffers); returns the chunk, the data at *offset in it
- (id<MTLBuffer>)stage:(const void *)bytes length:(NSUInteger)length offset:(NSUInteger *)offset;
- (void)recordDraw:(RDNA4Op)op;   // a draw: submitted behind the draws before it, not waited for
// Shadowed textures over buffers made current now, once a command buffer (a window's many draws: one copy): Core Animation unmaps that
// memory once the frame is committed (the GPU's wired pages stay; a CPU copy read from the unmapped range afterwards crashed WindowServer,
// 2026-11-20). Returns those over IOSurfaces, to be made current when the step runs, as before: the surface is retained, and refreshed at
// encoding, a client's pixels came out transparent (Safari's page showed the desktop through, 2026-11-20).
- (NSArray<RDNA4MTLTexture *> *)refreshShadows:(NSArray<RDNA4MTLTexture *> *)shadowed;
- (void)refreshLate:(NSArray<RDNA4MTLTexture *> *)late;   // those, when the step runs (work queue), once a command buffer each
- (void)drewInto:(RDNA4MTLTexture *)target;   // (work queue) a shadowed IOSurface's copy drawn: written back to it at the end
@end

@interface RDNA4MTLComputeEncoder : NSObject <MTLComputeCommandEncoder>
- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer;
@end

@implementation RDNA4MTLComputeEncoder {
	RDNA4MTLCommandBuffer *_commandBuffer;
	RDNA4MTLComputePipeline *_pipeline;
	uint64_t _addresses[kMaxBufferIndex];
	id<MTLBuffer> _bound[kMaxBufferIndex];
	id<MTLTexture> _textures[16];
	id<MTLSamplerState> _samplers[16];
	NSMutableArray<RDNA4MTLTexture *> *_used;   // textures in argument buffers (useResource:usage:): kept and written back as bound ones
}
@synthesize label;

- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)usage
{
	if (![(id)r isKindOfClass:[RDNA4MTLTexture class]] || ((RDNA4MTLTexture *)r).isDepth || [_used containsObject:(RDNA4MTLTexture *)r])
		return;
	if (!_used)
		_used = [NSMutableArray array];
	[_used addObject:(RDNA4MTLTexture *)r];
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained[])r count:(NSUInteger)count usage:(MTLResourceUsage)usage
{
	for (NSUInteger i = 0; i < count; i++)
		[self useResource:r[i] usage:usage];
}
- (void)useHeap:(id<MTLHeap>)heap { }   // (its buffers are memory of the process, as any)
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained[])heaps count:(NSUInteger)count { }

- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { if (i < 16) _textures[i] = t; }
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { if (i < 16) _samplers[i] = s; }
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++)
		[self setTexture:t[i] atIndex:r.location + i];
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++)
		[self setSamplerState:s[i] atIndex:r.location + i];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index { }   // the translator gives such arguments LDS of their own
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained[])resources count:(NSUInteger)count { }

- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer
{
	if ((self = [super init]))
		_commandBuffer = commandBuffer;
	return self;
}

- (id<MTLDevice>)device { return _commandBuffer.device; }
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }

- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
	_pipeline = (RDNA4MTLComputePipeline *)state;
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index >= kMaxBufferIndex)
		return;
	_bound[index] = buffer;
	_addresses[index] = buffer ? buffer.gpuAddress + offset : 0;
}

- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (index < kMaxBufferIndex && _bound[index])
		_addresses[index] = _bound[index].gpuAddress + offset;
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])buffers offsets:(const NSUInteger[])offsets withRange:(NSRange)range
{
	for (NSUInteger i = 0; i < range.length; i++)
		[self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}

- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	NSUInteger at = 0;
	id<MTLBuffer> chunk = [_commandBuffer stage:bytes length:length offset:&at];
	[self setBuffer:chunk offset:at atIndex:index];
}

- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)as atBufferIndex:(NSUInteger)index
{
	[self setBuffer:structureMemory(as) offset:0 atIndex:index];
}

- (void)record:(MTLSize)groups groupSize:(MTLSize)groupSize grid:(MTLSize)grid
{
	if (!_pipeline) {
		RLOG(@"RDNA4MTL: a dispatch without a compute pipeline, left out");
		return;
	}
	// The translator's kernargs: a buffer address per index, u32 threads per group x y z, u32 threads in the grid x y z, the texture table.
	const uint32_t n = _pipeline.buffers;
	NSMutableData *args = [NSMutableData dataWithLength:8 * n + 24 + 8];
	uint8_t *p = (uint8_t *)args.mutableBytes;
	memcpy(p, _addresses, 8 * MIN(n, (uint32_t)kMaxBufferIndex));
	// unbound buffer slots: the zeroed spare buffer, as a draw's (a kernel's read through address 0 faulted: Core Animation's
	// calculate_average_luma, 2026-11-20)
	const uint64_t spare = [(RDNA4MTLDevice *)self.device spareBuffer];
	for (uint32_t i = 0; i < n; i++)
		if (!((uint64_t *)p)[i])
			((uint64_t *)p)[i] = spare;
	const uint32_t sizes[6] = { (uint32_t)groupSize.width, (uint32_t)groupSize.height, (uint32_t)groupSize.depth,
		                        (uint32_t)grid.width,      (uint32_t)grid.height,      (uint32_t)grid.depth };
	memcpy(p + 8 * n, sizes, sizeof sizes);
	RDNA4MTLComputePipeline *pipeline = _pipeline;
	NSMutableArray *held = [NSMutableArray array];   // the bound buffers and textures, kept until the dispatch ran
	for (int i = 0; i < kMaxBufferIndex; i++)
		if (_bound[i])
			[held addObject:_bound[i]];
	// the textures and samplers: a table of their own (T# n at RDNA4_MTLC_TABLE_TEXTURES + 32 n, S# n at RDNA4_MTLC_TABLE_SAMPLERS + 16 n), its
	// address after the sizes in the kernargs (air2amdgpu.cpp); shadowed IOSurfaces copied in before the dispatch
	NSMutableArray<RDNA4MTLTexture *> *shadowed = [NSMutableArray array], *kernelTextures = [NSMutableArray array];
	uint8_t table[0x400] = {};
	BOOL anyTexture = NO;
	for (int i = 0; i < 16; i++) {
		if ([(id)_textures[i] isKindOfClass:[RDNA4MTLTexture class]] && !((RDNA4MTLTexture *)_textures[i]).isDepth) {
			RDNA4MTLTexture *t = (RDNA4MTLTexture *)_textures[i];
			[t descriptor:(uint32_t *)(table + RDNA4_MTLC_TABLE_TEXTURES + 32 * i)];
			[held addObject:t];
			[kernelTextures addObject:t];
			if (t.shadowed)
				[shadowed addObject:t];
			anyTexture = YES;
		}
		if ([(id)_samplers[i] isKindOfClass:[RDNA4MTLSamplerState class]]) {
			memcpy(table + RDNA4_MTLC_TABLE_SAMPLERS + 16 * i, ((RDNA4MTLSamplerState *)_samplers[i]).words, 16);
			anyTexture = YES;
		}
	}
	for (RDNA4MTLTexture *t in _used) {
		if ([kernelTextures containsObject:t])
			continue;
		[held addObject:t];
		[kernelTextures addObject:t];
		if (t.shadowed)
			[shadowed addObject:t];
	}
	{   // always: a kernel's texture read through a table at 0 faulted
		(void)anyTexture;
		uint32_t spare[8];
		[(RDNA4MTLDevice *)self.device spareImage:spare];
		for (int i = 0; i < 16; i++)   // unbound slots: the spare texel (an all-zero T# can stall the texture unit)
			if (![(id)_textures[i] isKindOfClass:[RDNA4MTLTexture class]] || ((RDNA4MTLTexture *)_textures[i]).isDepth)
				memcpy(table + RDNA4_MTLC_TABLE_TEXTURES + 32 * i, spare, 32);
		id<MTLBuffer> t = [self.device newBufferWithBytes:table length:sizeof table options:MTLResourceStorageModeShared];
		const uint64_t at = t.gpuAddress;
		memcpy(p + 8 * n + 24, &at, 8);
		if (t)
			[held addObject:t];
	}
	RDNA4MTLDevice *device = (RDNA4MTLDevice *)self.device;
	NSArray<RDNA4MTLTexture *> *late = [_commandBuffer refreshShadows:shadowed];   // (see the draw's)
	RDNA4MTLCommandBuffer *cb = _commandBuffer;
	BOOL plain = !shadowed.count && !late.count;   // no surface copies or held surfaces to bring in first: on the gfx ring when the kernel can
	for (RDNA4MTLTexture *t in kernelTextures)
		plain &= !t.held;
	rdna4_program_t ringProg = pipeline.program;
	if (plain && [device canDispatchOnRing:&ringProg]) {
		const std::array<uint32_t, 3> g = { (uint32_t)groups.width, (uint32_t)groups.height, (uint32_t)groups.depth };
		const std::array<uint32_t, 3> s = { (uint32_t)groupSize.width, (uint32_t)groupSize.height, (uint32_t)groupSize.depth };
		[_commandBuffer recordDraw:^NSError * {
			(void)held;
			const uint64_t t0 = nowUs();
			rdna4_program_t prog = pipeline.program;
			NSError *why = [device dispatchOnRing:&prog groups:g.data() size:s.data() args:args];
			for (RDNA4MTLTexture *t in kernelTextures) {   // what the kernel may have written (as below)
				[t wroteAll];
				[t heldRows:0 to:(uint32_t)t.height width:(uint32_t)t.width];
				if (t.usage & MTLTextureUsageShaderWrite)
					[cb drewInto:t];
			}
			statAdd(kStatDispatches, 1);
			statAdd(kStatDispatchUs, nowUs() - t0);
			return why;
		}];
		return;
	}
	[_commandBuffer record:^NSError * {
		rdna4_program_t prog = pipeline.program;
		const uint32_t g[3] = { (uint32_t)groups.width, (uint32_t)groups.height, (uint32_t)groups.depth };
		const uint32_t s[3] = { (uint32_t)groupSize.width, (uint32_t)groupSize.height, (uint32_t)groupSize.depth };
		(void)held;
		[cb refreshLate:late];
		BOOL synced = NO;
		for (RDNA4MTLTexture *t in kernelTextures) {
			NSError *why = t.held ? [t.held sync] : nil;
			if (why)
				return why;
			synced |= t.held != nil;
		}
		NSError *drained = synced ? [device drainGfx] : nil;   // (the fills went to the gfx ring, the dispatch to a compute queue)
		if (drained)
			return drained;
		const uint64_t t0 = nowUs();
		const kern_return_t kr = device.gpu ? rdna4_dispatch(device.gpu, &prog, g, s, args.bytes, (uint32_t)args.length, 1000, NULL) : kIOReturnNotOpen;
		for (RDNA4MTLTexture *t in kernelTextures) {   // what the kernel may have written
			[t wroteAll];
			[t heldRows:0 to:(uint32_t)t.height width:(uint32_t)t.width];
			// a shadowed IOSurface it may write: its copy into the surface at the end, as a draw's (VideoToolbox's 420v -> BGRA into
			// System Settings' display thumbnails, 2752 bytes a row: written in the copy only, they stayed black, 2026-11-21)
			if (t.usage & MTLTextureUsageShaderWrite)
				[cb drewInto:t];
		}
		statAdd(kStatDispatches, 1);
		statAdd(kStatDispatchUs, nowUs() - t0);
		return kr ? rdna4Error(@"a dispatch", kr) : nil;
	}];
}

- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)size
{
	[self record:groups groupSize:size
	        grid:MTLSizeMake(groups.width * size.width, groups.height * size.height, groups.depth * size.depth)];
}

- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)size
{
	MTLSize groups = MTLSizeMake((grid.width + size.width - 1) / size.width, (grid.height + size.height - 1) / size.height,
	                             (grid.depth + size.depth - 1) / size.depth);
	[self record:groups groupSize:size grid:grid];
}

- (void)endEncoding { }
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope { }   // dispatches run one after another

RDNA4_ANSWER_MISSING
@end

@interface RDNA4MTLRenderEncoder : NSObject <MTLRenderCommandEncoder>
- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer pass:(MTLRenderPassDescriptor *)pass;
@end

@implementation RDNA4MTLRenderEncoder {
	RDNA4MTLCommandBuffer *_commandBuffer;
	RDNA4MTLTexture *_targets[8];   // the pass's colour attachments of this device
	RDNA4MTLRenderPipeline *_pipeline;
	uint64_t _addresses[2][kTableBuffers];
	id<MTLBuffer> _bound[2][kTableBuffers];
	id<MTLBuffer> _indirect;   // (record:, while an indirect draw is recorded) where its arguments are, at _indirectOffset
	NSUInteger _indirectOffset;
	id<MTLTexture> _textures[2][kMaxTextures];
	id<MTLSamplerState> _samplers[2][kStageTextures];
	RDNA4MTLTexture *_depth, *_stencil;   // the pass's depth and stencil attachments of this device (the same texture for Depth32Float_Stencil8)
	RDNA4MTLDepthStencilState *_depthState;
	uint32_t _stencilRef;   // front in 0-7, back in 8-15
	RDNA4MTLTexture *_resolveTo[8];   // a multisampled colour attachment's resolve texture (its level), resolved at the end if _store says so
	MTLStoreAction _store[8];
	MTLViewport _viewport;
	MTLScissorRect _scissor;
	float _blendColour[4];
}
@synthesize label;

// Resources in argument buffers: memory of the process, as any, and kept alive by their owner (MoltenVK's descriptor sets).
// ponytail: a texture a draw writes through one is not written back to a shadowed IOSurface (the compute encoder's are).
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages { }
- (void)useResources:(const id<MTLResource> __unsafe_unretained[])r count:(NSUInteger)count usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages { }
- (void)useHeap:(id<MTLHeap>)heap stages:(MTLRenderStages)stages { }
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained[])heaps count:(NSUInteger)count stages:(MTLRenderStages)stages { }

- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer pass:(MTLRenderPassDescriptor *)pass
{
	if (!(self = [super init]))
		return nil;
	_commandBuffer = commandBuffer;
	RDNA4MTLTexture *first = nil;
	for (NSUInteger i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[i];
		if (!a.texture)
			continue;
		if (![(id)a.texture isKindOfClass:[RDNA4MTLTexture class]] || ((RDNA4MTLTexture *)a.texture).isDepth) {
			RLOG(@"RDNA4MTL: a render pass whose colour attachment %lu is not a colour texture of this device: its draws into it fail",
			      (unsigned long)i);
			continue;
		}
		// a level of it (Core Animation's blurs draw each mip level from the one before): that level as a texture of its own
		RDNA4MTLTexture *t = _targets[i] = [(RDNA4MTLTexture *)a.texture level:a.level];
		if (!t)
			continue;
		first = first ?: t;
		_store[i] = a.storeAction;
		if (t.samples > 1 && [(id)a.resolveTexture isKindOfClass:[RDNA4MTLTexture class]])
			_resolveTo[i] = [(RDNA4MTLTexture *)a.resolveTexture level:a.resolveLevel];
		const MTLClearColor c = a.clearColor;
		// on the card, behind the draws before it (a step of its own drained them all and the CPU filled up to 33 MB: Core Animation clears
		// about 9 targets a frame, 2026-11-20); the CPU's for what a 32-bit pattern cannot fill
		uint32_t pattern = 0;
		uint64_t bytes = 0;
		RDNA4MTLDevice *device = (RDNA4MTLDevice *)commandBuffer.device;
		if (a.loadAction != MTLLoadActionClear)
			continue;
		if ([t fillFor:c pattern:&pattern bytes:&bytes])
			[commandBuffer recordDraw:^NSError * {
				NSError *why = [t.held sync];
				if (why)
					return why;
				[t wroteAll];
				[commandBuffer drewInto:t];
				[t heldRows:0 to:(uint32_t)t.height width:(uint32_t)t.width];
				const RDNA4Copy fill = { 0, t.gpuBase, bytes, pattern };
				return [device gpuBarrier:&fill count:1];
			}];
		else
			[commandBuffer record:^NSError * {
				[t clearTo:c];
				[commandBuffer drewInto:t];
				return nil;
			}];
	}
	RTRACE(40, @"render pass: attachment 0 %@ format %lu %lux%lu load %lu, attachment 1 %@, depth %@", pass.colorAttachments[0].texture ? NSStringFromClass([(id)pass.colorAttachments[0].texture class]) : @"-",
	       (unsigned long)pass.colorAttachments[0].texture.pixelFormat, (unsigned long)pass.colorAttachments[0].texture.width,
	       (unsigned long)pass.colorAttachments[0].texture.height, (unsigned long)pass.colorAttachments[0].loadAction,
	       pass.colorAttachments[1].texture ? @"yes" : @"-", pass.depthAttachment.texture ? @"yes" : @"-");
	MTLRenderPassDepthAttachmentDescriptor *z = pass.depthAttachment;
	MTLRenderPassStencilAttachmentDescriptor *sa = pass.stencilAttachment;
	RDNA4MTLTexture *zt = [(id)z.texture isKindOfClass:[RDNA4MTLTexture class]] ? (RDNA4MTLTexture *)z.texture : nil;
	RDNA4MTLTexture *st = [(id)sa.texture isKindOfClass:[RDNA4MTLTexture class]] ? (RDNA4MTLTexture *)sa.texture : nil;
	first = first ?: zt ?: st;   // a pass of depth (and stencil) only: its size theirs (empty before: every draw outside the scissor)
	_viewport = (MTLViewport){ 0, 0, (double)first.width, (double)first.height, 0, 1 };
	_scissor = (MTLScissorRect){ 0, 0, first.width, first.height };
	_depth = zt.isDepth && zt.depthBytes ? zt : nil;
	_stencil = st.isDepth && st.stencilBytes ? st : nil;
	// their clears on the card behind the draws before them, as the colour's (the depth's was the CPU's, a step that drained the draws)
	const float zf = (float)z.clearDepth;
	uint32_t zbits;
	memcpy(&zbits, &zf, 4);
	const RDNA4Copy zfill = { 0, _depth.gpuBase, _depth && z.loadAction == MTLLoadActionClear ? _depth.depthBytes : 0, zbits };
	const RDNA4Copy sfill = { 0, _stencil.gpuBase + _stencil.depthBytes, _stencil && sa.loadAction == MTLLoadActionClear ? _stencil.stencilBytes : 0,
		                      (sa.clearStencil & 0xff) * 0x01010101u };
	if (zfill.bytes || sfill.bytes) {
		RDNA4MTLDevice *device = (RDNA4MTLDevice *)commandBuffer.device;
		[commandBuffer recordDraw:^NSError * {
			RDNA4Copy fills[2];
			uint32_t n = 0;
			if (zfill.bytes)
				fills[n++] = zfill;
			if (sfill.bytes)
				fills[n++] = sfill;
			return [device gpuBarrier:fills count:n];
		}];
	}
	return self;
}

- (void)setStencilReferenceValue:(uint32_t)ref { _stencilRef = (ref & 0xff) * 0x101u; }
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back { _stencilRef = (front & 0xff) | (back & 0xff) << 8; }
- (void)setColorStoreAction:(MTLStoreAction)action atIndex:(NSUInteger)i { if (i < 8) _store[i] = action; }

- (void)setDepthStencilState:(id<MTLDepthStencilState>)state
{
	_depthState = [(id)state isKindOfClass:[RDNA4MTLDepthStencilState class]] ? (RDNA4MTLDepthStencilState *)state : nil;
}

- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { if (i < kMaxTextures) _textures[0][i] = t; }
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { if (i < kMaxTextures) _textures[1][i] = t; }
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { if (i < kStageTextures) _samplers[0][i] = s; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { if (i < kStageTextures) _samplers[1][i] = s; }
// several at once (MetalQuake's status bar: answered as missing, its textures were never bound, 2026-11-21)
- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++)
		[self setVertexTexture:t[i] atIndex:r.location + i];
}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++)
		[self setFragmentTexture:t[i] atIndex:r.location + i];
}
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++)
		[self setVertexSamplerState:s[i] atIndex:r.location + i];
}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++)
		[self setFragmentSamplerState:s[i] atIndex:r.location + i];
}
// ponytail: the LOD clamps are the sampler's own, not these (Apple's GL-on-Metal renderer sets samplers so, 2026-11-21)
- (void)setVertexSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i { [self setVertexSamplerState:s atIndex:i]; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i { [self setFragmentSamplerState:s atIndex:i]; }

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances
{
	[self record:type start:0 count:count instances:instances baseInstance:0 indices:indexBuffer offset:offset type:indexType];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)offset
{
	[self record:type start:0 count:count instances:1 baseInstance:0 indices:indexBuffer offset:offset type:indexType];
}

// The base vertex as the first vertex: the vertex stage adds it (USER_DATA_GS_2) to the vertex id of indexed draws as of others. Apple's
// GL-on-Metal renderer draws so: answered as a missing selector, every GL draw was dropped (2026-11-21).
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer indexBufferOffset:(NSUInteger)offset instanceCount:(NSUInteger)instances
                   baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance
{
	[self record:type start:(NSUInteger)(uint32_t)baseVertex count:count instances:instances baseInstance:baseInstance indices:indexBuffer
	      offset:offset type:indexType];
}
// Indirect: the arguments read when the draw runs (record:'s block)
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)offset indirectBuffer:(id<MTLBuffer>)indirect indirectBufferOffset:(NSUInteger)indirectOffset
{
	_indirect = indirect, _indirectOffset = indirectOffset;
	[self record:type start:0 count:0 instances:1 baseInstance:0 indices:indexBuffer offset:offset type:indexType];
	_indirect = nil;
}
- (void)drawPrimitives:(MTLPrimitiveType)type indirectBuffer:(id<MTLBuffer>)indirect indirectBufferOffset:(NSUInteger)indirectOffset
{
	_indirect = indirect, _indirectOffset = indirectOffset;
	[self record:type start:0 count:0 instances:1 baseInstance:0 indices:nil offset:0 type:MTLIndexTypeUInt16];
	_indirect = nil;
}
- (void)setDepthStoreAction:(MTLStoreAction)action {}   // ponytail: depth is kept on the card for the texture's life either way
- (void)setStencilStoreAction:(MTLStoreAction)action {}   // (and stencil)
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope afterStages:(MTLRenderStages)after beforeStages:(MTLRenderStages)before { }   // draws in order
- (void)setLineWidth:(float)width {}                    // ponytail: lines are one pixel wide

- (id<MTLDevice>)device { return _commandBuffer.device; }

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
	_pipeline = (RDNA4MTLRenderPipeline *)state;
}

- (void)bind:(unsigned)stage buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index
{
	if (index >= kTableBuffers) {
		RLOG(@"RDNA4MTL: buffer index %lu: only 0-%d so far", (unsigned long)index, kTableBuffers - 1);
		return;
	}
	_bound[stage][index] = buffer;
	_addresses[stage][index] = buffer ? buffer.gpuAddress + offset : 0;
}

// What of the target a draw can write (d->covered): for a vertex shader whose position is a buffer's matrix times an attribute (POSM), its
// vertices so transformed, bounded, a pixel around them, within the scissor. Left the scissor: more than 256 vertices, a vertex at or behind
// the eye (w <= 0), an attribute not of floats or not per vertex, data the CPU does not see. ponytail: read when the draw is encoded, so
// vertices written after it and before the commit are not seen (Core Animation writes them first).
- (void)bound:(RDNA4Draw *)d pipeline:(RDNA4MTLRenderPipeline *)p indices:(id<MTLBuffer>)ib offset:(NSUInteger)io type:(MTLIndexType)it
{
	uint32_t a, m;
	if (![p positionFrom:&a matrix:&m] || !d->count || d->count > 256)
		return;
	const uint32_t *rec = p.attributes + 4 * a, bi = rec[2] & 0xff, kind = rec[2] >> 8 & 15, comps = rec[2] >> 12 & 7;
	if (kind || comps < 2 || comps > 4 || (rec[2] >> 16 & 0xff) != MTLVertexStepFunctionPerVertex || bi >= kTableBuffers)
		return;
	id<MTLBuffer> vb = _bound[0][bi], mb = _bound[0][m];
	const uint8_t *vp = (const uint8_t *)vb.contents, *mp = (const uint8_t *)mb.contents, *ix = (const uint8_t *)ib.contents;
	const NSUInteger vo = (NSUInteger)(_addresses[0][bi] - vb.gpuAddress), mo = (NSUInteger)(_addresses[0][m] - mb.gpuAddress);
	const NSUInteger ixBytes = it == MTLIndexTypeUInt32 ? 4 : 2;
	if (!vp || !mp || mo + 64 > mb.length || (ib && (!ix || io + d->count * ixBytes > ib.length)))
		return;
	float M[16], lo[2] = { INFINITY, INFINITY }, hi[2] = { -INFINITY, -INFINITY };   // M: column-major
	memcpy(M, mp + mo, sizeof M);
	for (uint32_t i = 0; i < d->count; i++) {
		const uint32_t raw = !ib ? i : ixBytes == 4 ? ((const uint32_t *)(ix + io))[i] : ((const uint16_t *)(ix + io))[i];
		if (ib && raw == (ixBytes == 4 ? 0xffffffffu : 0xffffu))   // a strip's restart
			continue;
		const uint32_t v = d->start + raw;
		const NSUInteger at = vo + (NSUInteger)v * rec[1] + rec[0];
		if (at + 4 * comps > vb.length)
			return;
		float x[4] = { 0, 0, 0, 1 }, c[4];
		memcpy(x, vp + at, 4 * comps);
		for (int r = 0; r < 4; r++)
			c[r] = M[r] * x[0] + M[4 + r] * x[1] + M[8 + r] * x[2] + M[12 + r] * x[3];
		if (!(c[3] > 1e-6f))
			return;
		const float px = d->viewport[0] + (c[0] / c[3] + 1) * 0.5f * d->viewport[2], py = d->viewport[1] + (1 - c[1] / c[3]) * 0.5f * d->viewport[3];
		if (!isfinite(px) || !isfinite(py))
			return;
		lo[0] = fminf(lo[0], px), lo[1] = fminf(lo[1], py), hi[0] = fmaxf(hi[0], px), hi[1] = fmaxf(hi[1], py);
	}
	const int64_t sx = d->scissor[0], sy = d->scissor[1], sx2 = sx + d->scissor[2], sy2 = sy + d->scissor[3];
	const int64_t x0 = MAX((int64_t)floorf(lo[0]) - 1, sx), y0 = MAX((int64_t)floorf(lo[1]) - 1, sy);
	const int64_t x1 = MIN((int64_t)ceilf(hi[0]) + 1, sx2), y1 = MIN((int64_t)ceilf(hi[1]) + 1, sy2);
	if (!(lo[0] <= hi[0]) || x0 >= x1 || y0 >= y1) {   // nothing (every index a restart, or all outside the scissor)
		d->covered[2] = d->covered[3] = 0;
		return;
	}
	d->covered[0] = (uint32_t)x0, d->covered[1] = (uint32_t)y0, d->covered[2] = (uint32_t)(x1 - x0), d->covered[3] = (uint32_t)(y1 - y0);
}

- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self bind:0 buffer:b offset:o index:i]; }
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self bind:1 buffer:b offset:o index:i]; }
- (void)setVertexAccelerationStructure:(id<MTLAccelerationStructure>)as atBufferIndex:(NSUInteger)i
{
	[self bind:0 buffer:structureMemory(as) offset:0 index:i];
}
- (void)setFragmentAccelerationStructure:(id<MTLAccelerationStructure>)as atBufferIndex:(NSUInteger)i
{
	[self bind:1 buffer:structureMemory(as) offset:0 index:i];
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	NSUInteger at = 0;
	id<MTLBuffer> chunk = [_commandBuffer stage:bytes length:length offset:&at];
	[self bind:0 buffer:chunk offset:at index:index];
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	NSUInteger at = 0;
	id<MTLBuffer> chunk = [_commandBuffer stage:bytes length:length offset:&at];
	[self bind:1 buffer:chunk offset:at index:index];
}

- (void)setViewport:(MTLViewport)viewport { _viewport = viewport; }
- (void)setScissorRect:(MTLScissorRect)rect { _scissor = rect; }
// ponytail: one viewport and scissor (the first of several: Maps passes one); no depth bias, depth clipped (Metal's default)
- (void)setViewports:(const MTLViewport[])v count:(NSUInteger)n { if (n) _viewport = v[0]; }
- (void)setScissorRects:(const MTLScissorRect[])r count:(NSUInteger)n { if (n) _scissor = r[0]; }
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp { }
- (void)setDepthClipMode:(MTLDepthClipMode)mode { }
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
	_blendColour[0] = r, _blendColour[1] = g, _blendColour[2] = b, _blendColour[3] = a;
}
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < kTableBuffers) [self bind:0 buffer:_bound[0][i] offset:o index:i]; }
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < kTableBuffers) [self bind:1 buffer:_bound[1][i] offset:o index:i]; }
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++)
		[self bind:0 buffer:b[i] offset:o[i] index:r.location + i];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
	for (NSUInteger i = 0; i < r.length; i++)
		[self bind:1 buffer:b[i] offset:o[i] index:r.location + i];
}
- (void)setCullMode:(MTLCullMode)mode { }                    // ponytail: nothing culled (CA's quads face the viewer)
- (void)setFrontFacingWinding:(MTLWinding)winding { }
- (void)setTriangleFillMode:(MTLTriangleFillMode)mode { }

- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)baseInstance
{
	[self record:type start:start count:count instances:instances baseInstance:baseInstance indices:nil offset:0 type:MTLIndexTypeUInt16];
}

// One draw with everything bound now: buffers, textures and samplers (as T#s and S#s), the viewport, the depth attachment and state, indices.
- (void)record:(MTLPrimitiveType)type start:(NSUInteger)start count:(NSUInteger)count instances:(NSUInteger)instances
    baseInstance:(NSUInteger)baseInstance indices:(id<MTLBuffer>)indexBuffer offset:(NSUInteger)indexOffset type:(MTLIndexType)indexType
{
	const StatTime timed { kStatRecordUs };
	id<MTLBuffer> indirect = _indirect;   // an indirect draw's arguments: read when it runs (written on the card by then, below)
	const NSUInteger indirectOffset = _indirectOffset;
	RDNA4Draw d = { .type = (uint32_t)type, .start = (uint32_t)start, .count = (uint32_t)count, .instances = (uint32_t)instances,
		            .baseInstance = (uint32_t)baseInstance,
		            .viewport = { (float)_viewport.originX, (float)_viewport.originY, (float)_viewport.width, (float)_viewport.height },
		            .scissor = { (uint32_t)_scissor.x, (uint32_t)_scissor.y, (uint32_t)_scissor.width, (uint32_t)_scissor.height } };
	memcpy(d.blendColour, _blendColour, sizeof d.blendColour);
	memcpy(d.table, _addresses, sizeof d.table);
	NSMutableArray *held = [NSMutableArray array];   // what the draw uses, kept until it ran
	NSMutableArray<RDNA4MTLTexture *> *shadowed = [NSMutableArray array];   // read through copies made current before the draw
	NSMutableArray *reads = [NSMutableArray array];   // {stage, slot, texture}: what the draw samples
	const BOOL wantsLevels = _pipeline && ((_pipeline.vs[7] | _pipeline.ps[7]) >> 16 & 1);
	NSMutableData *levelT = wantsLevels ? [NSMutableData dataWithLength:2 * 7 * 16 * 32] : nil;
	const BOOL wantsHandles = _pipeline && (([_pipeline texturesOf:0] | [_pipeline texturesOf:1]) & RDNA4_MTLC_TXRD_HANDLES);
	NSMutableData *handles = wantsHandles ? [NSMutableData dataWithLength:2 * kHandleTextures * 8] : nil;
	for (int st = 0; st < 2; st++) {
		for (int i = 0; i < kTableBuffers; i++)
			if (_bound[st][i])
				[held addObject:_bound[st][i]];
		// the textures its shaders use only: one left bound by an earlier draw is not read, made current, waited for or copied (Core
		// Animation's target, left bound, was a copy of it for every draw of Vfx / Xc, 2026-11-20)
		const uint32_t uses = _pipeline ? [_pipeline texturesOf:st] : 0xffff;
		for (int i = 0; i < kTextures; i++) {
			if ((uses >> i & 1) && [(id)_textures[st][i] isKindOfClass:[RDNA4MTLTexture class]] && !((RDNA4MTLTexture *)_textures[st][i]).isDepth) {
				RDNA4MTLTexture *t = (RDNA4MTLTexture *)_textures[st][i];
				[t descriptor:d.textures[st][i]];
				for (NSUInteger l = 1; levelT && i < kStageTextures && l < 8; l++)   // (the helper refuses levels of textures 16-30)
					[t descriptor:(uint32_t *)((uint8_t *)levelT.mutableBytes + ((st * 7 + (l - 1)) * 16 + i) * 32) level:l];
				if (t.shadowed)
					[shadowed addObject:t];
				[held addObject:t];
				[reads addObject:@[ @(st), @(i), t ]];
			}
			if (i < kStageTextures && [(id)_samplers[st][i] isKindOfClass:[RDNA4MTLSamplerState class]])
				memcpy(d.samplers[st][i], ((RDNA4MTLSamplerState *)_samplers[st][i]).words, 16);
		}
		// textures 31-127: their T# slots (gpuResourceID writes the T# as it is now), unbound ones the spare texel's
		for (int i = kTextures; handles && (uses & RDNA4_MTLC_TXRD_HANDLES) && i < kMaxTextures; i++) {
			RDNA4MTLTexture *t = [(id)_textures[st][i] isKindOfClass:[RDNA4MTLTexture class]] ? (RDNA4MTLTexture *)_textures[st][i] : nil;
			const uint64_t slot = t ? t.gpuResourceID._impl : 0;
			((uint64_t *)handles.mutableBytes)[st * kHandleTextures + (i - kTextures)] = slot ?: [(RDNA4MTLDevice *)self.device spareHandle];
			if (!t || t.isDepth)
				continue;
			if (t.shadowed)
				[shadowed addObject:t];
			[held addObject:t];
			[reads addObject:@[ @(st), @(i), t ]];
		}
	}
	if (handles)
		[held addObject:handles];
	if (indexBuffer) {
		d.indexGpu = indexBuffer.gpuAddress + indexOffset;
		d.indexBytes = indexType == MTLIndexTypeUInt32 ? 4 : 2;
		[held addObject:indexBuffer];
	}
	if (_depthState && (_depth || _stencil)) {
		RDNA4MTLTexture *z = _depth ?: _stencil;
		d.depthWidth = (uint32_t)z.width;
		d.depthHeight = (uint32_t)z.height;
		[held addObject:z];
	}
	// Z on only when it tests or writes something (the PV host's rule): with Z on, compare always and no write, a 4x pass's stencil writes
	// were lost in a pattern of 8x16-pixel tiles (mtlprobe nativestencil, 2026-11-20)
	if (_depth && _depthState && (_depthState.compare != MTLCompareFunctionAlways || _depthState.write)) {
		d.depthGpu = _depth.gpuBase;
		d.depthCompare = _depthState.compare;
		d.depthWrite = _depthState.write;
	}
	if (_stencil && _depthState) {
		d.stencilGpu = _stencil.gpuBase + _stencil.depthBytes;
		d.stencilDepthControl = _depthState.stencilDepthControl;
		d.stencilControl = _depthState.stencilControl;
		d.stencilReadMask = _depthState.readMask;
		d.stencilWriteMask = _depthState.writeMask;
		d.stencilRef = _stencilRef;
		[held addObject:_stencil];
	}
	// The attachment it goes into, as the PV host's runDraw: the one other than 0 the fragment shader writes (its header's bits 28-30), or of
	// several it writes (bits 20-27) the first the pipeline writes (its mask) and the pass has a texture for.
	RDNA4MTLRenderPipeline *pipeline = _pipeline;
	const uint32_t attr = pipeline ? pipeline.ps[7] : 0, writes = attr >> 20 & 0xff;
	d.attachment = attr >> 28 & 7;
	for (uint32_t k = 0; k < 8; k++)
		if ((writes >> k & 1) && [pipeline blendOf:k][9] && _targets[k]) {
			d.attachment = k;
			break;
		}
	RDNA4MTLTexture *target = _targets[d.attachment];
	RDNA4MTLDevice *device = (RDNA4MTLDevice *)self.device;
	// A pass of depth (and stencil) only (Unity's shadow maps): as if into a stand-in of the depth's size, colour not written. Refused
	// before: "a draw without ... a colour attachment", and the command buffer's later work with it (Daggerfall Unity, 2026-11-22).
	if (!target && (_depth || _stencil) && (target = [device standInFor:_depth ?: _stencil]))
		d.depthOnly = 1;
	memcpy(d.covered, d.scissor, sizeof d.covered);
	if (pipeline)
		[self bound:&d pipeline:pipeline indices:indexBuffer offset:indexOffset type:indexType];
	d.levelT = (const uint8_t *)levelT.bytes;
	d.handles = (const uint64_t *)handles.bytes;
	if (pipeline && !pipeline.described) {   // what each pipeline's first draw binds: a hang's draw named (drainGfx), this says what it read
		pipeline.described = YES;
		NSMutableString *what = [NSMutableString stringWithFormat:@"pipeline %@: vs 0x%08x ps 0x%08x, vs rsrc %08x %08x, ps rsrc %08x %08x ena %08x addr %08x, blend",
		                                                          pipeline.names, pipeline.vs[7], pipeline.ps[7], pipeline.vs[3], pipeline.vs[4],
		                                                          pipeline.ps[3], pipeline.ps[4], pipeline.ps[5], pipeline.ps[6]];
		for (int k = 2; k < 10; k++)
			[what appendFormat:@" %u", [pipeline blendOf:d.attachment][k]];
		for (int st = 0; st < 2; st++)
			for (int i = 0; i < kStageTextures; i++) {
				id<MTLTexture> t = _textures[st][i];
				if (t)
					[what appendFormat:@"; %s tex %d: %@ type %lu fmt %lu %lux%lux%lu lv %lu%s", st ? "fs" : "vs", i, NSStringFromClass([(id)t class]),
					                   (unsigned long)t.textureType, (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height,
					                   (unsigned long)t.depth, (unsigned long)t.mipmapLevelCount,
					                   [(id)t isKindOfClass:[RDNA4MTLTexture class]] && ((RDNA4MTLTexture *)t).shadowed ? " shadowed" : ""];
				if (_samplers[st][i]) {
					const uint32_t *w = (const uint32_t *)d.samplers[st][i];
					[what appendFormat:@"; %s smp %d: %08x %08x %08x %08x", st ? "fs" : "vs", i, w[0], w[1], w[2], w[3]];
				}
			}
		for (int i = 0; i < 16; i++)
			if (pipeline.attributes[4 * i + 2])
				[what appendFormat:@"; attr %d: %u %u 0x%x %u", i, pipeline.attributes[4 * i], pipeline.attributes[4 * i + 1],
				                   pipeline.attributes[4 * i + 2], pipeline.attributes[4 * i + 3]];
		RLOG(@"RDNA4MTL trace: %@", what);
	}
	NSArray<RDNA4MTLTexture *> *late = [_commandBuffer refreshShadows:shadowed];
	RDNA4MTLCommandBuffer *cb = _commandBuffer;
	[_commandBuffer recordDraw:^NSError * {
		(void)held;
		(void)levelT;
		[cb refreshLate:late];
		// As the PV host: a texture an earlier draw still in flight draws into is read after those draws (a barrier on the card), and the
		// draw's own target, read as a texture (Core Animation's backdrops), is read from a copy the card makes (the PV host's staged copy);
		// read in place while drawn, WindowServer's first native frames hung the gfx ring (2026-11-20).
		RDNA4Draw dd = d;
		// An indirect draw's arguments, as the PV host's: what wrote them (MoltenVK's culling kernels in vkQuake) ran before this step,
		// dispatches being waited for. MTLDraw(Indexed)PrimitivesIndirectArguments.
		if (indirect) {
			const uint32_t *a = (const uint32_t *)((const uint8_t *)indirect.contents + indirectOffset);
			if (!indirect.contents || indirectOffset + 20 > indirect.length)
				return rdna4Error(@"an indirect draw's arguments outside its buffer", 0);
			dd.count = a[0], dd.instances = a[1];
			if (dd.indexGpu) {
				dd.indexGpu += (uint64_t)a[2] * dd.indexBytes;
				dd.start = a[3], dd.baseInstance = a[4];   // (the base vertex as the first vertex, as drawIndexed...baseVertex:)
			} else {
				dd.start = a[2], dd.baseInstance = a[3];
			}
			if (!dd.count || !dd.instances)
				return nil;
		}
		NSError *why = [target.held sync];
		for (NSUInteger i = 0; i < reads.count && !why; i++)
			why = [((RDNA4MTLTexture *)reads[i][2]).held sync];
		if (why)
			return why;
		BOOL barrier = NO;
		RDNA4MTLTexture *fresh = nil;   // the target, copied before this draw
		for (NSArray *r in reads) {
			RDNA4MTLTexture *t = r[2];
			if (t != target && [device drawnInFlight:t])
				barrier = YES;
			if (t == target) {
				// Brought up to date before each such draw: Core Animation's draws read what the draws just before them drew, with or
				// without a barrier (copied once a pass, its glass and Safari's toolbar showed the desktop under the windows, 2026-11-20).
				// What was written since the last copy only, on the card: copied whole by the CPU (4K, 2000 in 5 s), it took WindowServer's
				// work queue 3.8 s of each 5 while windows moved.
				RDNA4MTLTexture *copy = target.readCopy;
				if (!copy)
					return rdna4Error(@"a draw reading its own target: no copy of it", 0);
				fresh = target;
				[copy descriptor:dd.textures[[r[0] intValue]][[r[1] intValue]]];
			}
		}
		uint64_t ranges[kWrittenRanges][2];
		// The rows it covers only, and a few around them (a sample a little outside them): it writes there only, and Core Animation's
		// backdrops are a window's or a menu's rectangle of a 4K target (all of it copied, 10 GB/s on the card while windows moved,
		// 2026-11-20). ponytail: a draw sampling further outside what it covers reads those rows as last copied.
		enum { kReadMargin = 16 };
		const uint64_t row = fresh.rowBytes, y0 = dd.covered[1] > kReadMargin ? dd.covered[1] - kReadMargin : 0,
		               y1 = MIN((uint64_t)dd.covered[1] + dd.covered[3] + kReadMargin, (uint64_t)fresh.height);
		const uint32_t n = fresh ? [fresh takeWritten:ranges from:fresh.identity + y0 * row to:fresh.identity + y1 * row] : 0;   // 0: copied
		RDNA4Copy copies[kWrittenRanges];
		for (uint32_t i = 0; i < n; i++)
			copies[i] = (RDNA4Copy){ fresh.gpuBase + (ranges[i][0] - fresh.identity), fresh.readCopy.gpuBase + (ranges[i][0] - fresh.identity),
				                     ranges[i][1] - ranges[i][0] };
		if ((barrier || n) && (why = [device gpuBarrier:copies count:n]))
			return why;
		if (!pipeline || !target)
			return rdna4Error(@"a draw without a render pipeline or a colour attachment of this device", 0);
		if (pipeline.broken)
			return nil;

		const uint64_t t0 = nowUs();
		why = [device draw:&dd pipeline:pipeline target:target];
		if (!why)
			[cb drewInto:target];
		statAdd(kStatDraws, 1);
		statAdd(kStatDrawUs, nowUs() - t0);
		return why;
	}];
}

- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count instanceCount:(NSUInteger)instances
{
	[self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:0];
}

- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
{
	[self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:1 baseInstance:0];
}

// A multisampled attachment's resolve (MultisampleResolve, StoreAndMultisampleResolve), as the PV host's resolveMsaa: rdna4_resolve
// (tools/metal/m3/kernels.metal, in src/mtlkernels.h) averages its samples into the resolve texture once the draws are done; an sRGB
// texture's (RGBA8, BGRA8 sRGB) rdna4_resolve_srgb, in linear light. Both read and write the pixels as stored: 8_8_8_8 or 16_16_16_16
// unorm, 10_10_10_2 for RGB10A2 and BGR10A2.
- (void)endEncoding
{
	RDNA4MTLCommandBuffer *cb = _commandBuffer;
	RDNA4MTLDevice *device = (RDNA4MTLDevice *)cb.device;
	for (NSUInteger i = 0; i < 8; i++) {
		RDNA4MTLTexture *ms = _targets[i], *dst = _resolveTo[i];
		if (!dst || (_store[i] != MTLStoreActionMultisampleResolve && _store[i] != MTLStoreActionStoreAndMultisampleResolve))
			continue;
		if (dst.isDepth || dst.samples > 1 || dst.width != ms.width || dst.height != ms.height || dst.bpp != ms.bpp) {
			RLOG(@"RDNA4MTL: a resolve of a %lux%lu format %lu texture into a %lux%lu format %lu one: not done", (unsigned long)ms.width,
			     (unsigned long)ms.height, (unsigned long)ms.pixelFormat, (unsigned long)dst.width, (unsigned long)dst.height,
			     (unsigned long)dst.pixelFormat);
			continue;
		}
		const MTLPixelFormat f = ms.pixelFormat;
		const BOOL srgb = f == MTLPixelFormatRGBA8Unorm_sRGB || f == MTLPixelFormatBGRA8Unorm_sRGB;
		const uint32_t raw = ms.bpp < 4 ? ms.gfxFormat : f == MTLPixelFormatRGB10A2Unorm || f == MTLPixelFormatBGR10A2Unorm ? 36 : ms.bpp == 8 ? 57 : 42;
		const uint32_t sel = 4 | 5 << 3 | 6 << 6 | 7 << 9, w = (uint32_t)ms.width, h = (uint32_t)ms.height;
		[cb record:^NSError * {
			const rdna4_program_t *prog = [device resolveKernel:srgb];
			if (!prog)
				return rdna4Error(@"the multisample resolve kernel", 0);
			NSError *why = [dst.held sync];
			if (!why && dst.held)
				why = [device drainGfx];   // (the held copy's fill went to the gfx ring, the dispatch to a compute queue)
			if (why)
				return why;
			uint32_t table[0x400 / 4] = {};
			msaaImage(table + RDNA4_MTLC_TABLE_TEXTURES / 4, ms.gpuBase, raw, sel, w, h, ms.samples == 4 ? 2 : 1);
			linearImage(table + (RDNA4_MTLC_TABLE_TEXTURES + 32) / 4, dst.gpuBase, raw, sel, w, h, (uint32_t)(dst.rowBytes / dst.bpp));
			id<MTLBuffer> tb = [device newBufferWithBytes:table length:sizeof table options:MTLResourceStorageModeShared];
			if (!tb)
				return rdna4Error(@"a multisample resolve's table", 0);
			struct __attribute__((packed)) { uint32_t dims[6]; uint64_t table; } args = { { 8, 8, 1, w, h, 1 }, tb.gpuAddress };
			const uint32_t groups[3] = { (w + 7) / 8, (h + 7) / 8, 1 }, size[3] = { 8, 8, 1 };
			const kern_return_t kr = rdna4_dispatch(device.gpu, prog, groups, size, &args, sizeof args, 1000, NULL);
			[dst wroteAll];
			[dst heldRows:0 to:h width:w];
			[cb drewInto:dst];
			return kr ? rdna4Error(@"a multisample resolve", kr) : nil;
		}];
	}
}

RDNA4_ANSWER_MISSING
@end

@interface RDNA4MTLAccelerationStructureEncoder : NSObject <MTLAccelerationStructureCommandEncoder>
- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer;
@end

@implementation RDNA4MTLAccelerationStructureEncoder {
	RDNA4MTLCommandBuffer *_commandBuffer;
}
@synthesize label;

- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer
{
	if ((self = [super init]))
		_commandBuffer = commandBuffer;
	return self;
}

- (id<MTLDevice>)device { return _commandBuffer.device; }

- (void)buildAccelerationStructure:(id<MTLAccelerationStructure>)as descriptor:(MTLAccelerationStructureDescriptor *)descriptor
                     scratchBuffer:(id<MTLBuffer>)scratch scratchBufferOffset:(NSUInteger)offset
{
	MTLAccelerationStructureDescriptor *d = [descriptor copy];   // as it is now: the app may change it before the commit
	[_commandBuffer record:^NSError * {
		if (![(id)as isKindOfClass:[RDNA4MTLAccelerationStructure class]])
			return rdna4Error(@"an acceleration structure of another device", 0);
		return [(RDNA4MTLAccelerationStructure *)as build:d];
	}];
}

- (void)endEncoding { }

RDNA4_ANSWER_MISSING
@end

@interface RDNA4MTLBlitEncoder : NSObject <MTLBlitCommandEncoder>
- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer;
@end

// Blits by the CPU, in order with the rest: every buffer and colour texture of this device is shared memory. Depth textures (VRAM) are not
// blitted. ponytail: CPU copies; a DMA or compute copy when they show up in a profile.
@implementation RDNA4MTLBlitEncoder {
	RDNA4MTLCommandBuffer *_commandBuffer;
}
@synthesize label;

- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer
{
	if ((self = [super init]))
		_commandBuffer = commandBuffer;
	return self;
}

- (id<MTLDevice>)device { return _commandBuffer.device; }

static NSError *colourOnly(id<MTLTexture> t)
{
	// (the CPU's: not one in the card's memory only, a multisampled one)
	return [(id)t isKindOfClass:[RDNA4MTLTexture class]] && !((RDNA4MTLTexture *)t).isDepth && ((RDNA4MTLTexture *)t).memory ? nil
	       : rdna4Error(@"a blit of a texture that is not a colour texture of this device in the CPU's memory", 0);
}

- (void)copyFromBuffer:(id<MTLBuffer>)from sourceOffset:(NSUInteger)fromOffset toBuffer:(id<MTLBuffer>)to
     destinationOffset:(NSUInteger)toOffset size:(NSUInteger)size
{
	[_commandBuffer record:^NSError * {
		if (fromOffset + size > from.length || toOffset + size > to.length)
			return rdna4Error(@"a buffer copy past a buffer's end", 0);
		memmove((uint8_t *)to.contents + toOffset, (const uint8_t *)from.contents + fromOffset, size);
		wrote(to.gpuAddress + toOffset, to.gpuAddress + toOffset + size);
		return nil;
	}];
}

- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value
{
	[_commandBuffer record:^NSError * {
		if (NSMaxRange(range) > buffer.length)
			return rdna4Error(@"a buffer fill past its end", 0);
		memset((uint8_t *)buffer.contents + range.location, value, range.length);
		wrote(buffer.gpuAddress + range.location, buffer.gpuAddress + NSMaxRange(range));
		return nil;
	}];
}

- (void)copyFromBuffer:(id<MTLBuffer>)from sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)bpr
   sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)to destinationSlice:(NSUInteger)slice
      destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin
{
	[_commandBuffer record:^NSError * {
		NSError *why = colourOnly(to);
		if (!why && offset + bpr * (size.height - 1) + ((RDNA4MTLTexture *)to).bpp * size.width > from.length)   // (Maps' glyphs: R8)
			why = rdna4Error(@"a buffer-to-texture copy past the buffer's end", 0);
		if (!why)
			[to replaceRegion:MTLRegionMake2D(origin.x, origin.y, size.width, size.height) mipmapLevel:level slice:slice
			        withBytes:(const uint8_t *)from.contents + offset bytesPerRow:bpr bytesPerImage:bpi];
		return why;
	}];
}

- (void)copyFromBuffer:(id<MTLBuffer>)from sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)bpr
   sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)to destinationSlice:(NSUInteger)slice
      destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin options:(MTLBlitOption)options
{
	[self copyFromBuffer:from sourceOffset:offset sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:size toTexture:to
	    destinationSlice:slice destinationLevel:level destinationOrigin:origin];
}

- (void)copyFromTexture:(id<MTLTexture>)from sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin
             sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)to destinationOffset:(NSUInteger)offset
 destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi
{
	[_commandBuffer record:^NSError * {
		NSError *why = colourOnly(from);
		if (!why && offset + bpr * (size.height - 1) + ((RDNA4MTLTexture *)from).bpp * size.width > to.length)
			why = rdna4Error(@"a texture-to-buffer copy past the buffer's end", 0);
		if (!why)
			[from getBytes:(uint8_t *)to.contents + offset bytesPerRow:bpr
			    fromRegion:MTLRegionMake2D(origin.x, origin.y, size.width, size.height) mipmapLevel:level];
		return why;
	}];
}

- (void)copyFromTexture:(id<MTLTexture>)from sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin
             sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)to destinationOffset:(NSUInteger)offset
 destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(MTLBlitOption)options
{
	[self copyFromTexture:from sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size toBuffer:to destinationOffset:offset
	    destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}

- (void)copyFromTexture:(id<MTLTexture>)from sourceSlice:(NSUInteger)fromSlice sourceLevel:(NSUInteger)fromLevel
           sourceOrigin:(MTLOrigin)fromOrigin sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)to
       destinationSlice:(NSUInteger)toSlice destinationLevel:(NSUInteger)toLevel destinationOrigin:(MTLOrigin)toOrigin
{
	[_commandBuffer record:^NSError * {
		NSError *why = colourOnly(from) ?: colourOnly(to);
		if (why)
			return why;
		RDNA4MTLTexture *f = [(RDNA4MTLTexture *)from level:fromLevel];
		[f refresh];   // (a shadowed surface's copy current)
		[to replaceRegion:MTLRegionMake2D(toOrigin.x, toOrigin.y, size.width, size.height) mipmapLevel:toLevel
		        withBytes:f.bytes + fromOrigin.y * f.rowBytes + f.bpp * fromOrigin.x bytesPerRow:f.rowBytes];
		return nil;
	}];
}

- (void)copyFromTexture:(id<MTLTexture>)from toTexture:(id<MTLTexture>)to
{
	[self copyFromTexture:from sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
	           sourceSize:MTLSizeMake(MIN(from.width, to.width), MIN(from.height, to.height), 1) toTexture:to destinationSlice:0
	     destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
}

- (void)generateMipmapsForTexture:(id<MTLTexture>)texture
{
	[_commandBuffer record:^NSError * {
		NSError *why = colourOnly(texture);
		if (!why)
			[(RDNA4MTLTexture *)texture generateMipmaps];
		return why;
	}];
}

- (void)synchronizeResource:(id<MTLResource>)resource { }   // shared memory: nothing to do
- (void)synchronizeTexture:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level { }
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)texture { }
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)texture { }
- (void)endEncoding { }

RDNA4_ANSWER_MISSING
@end

static bool gAbortStack;   // WindowServer on our accelerator: kept as SIGABRT's handler (commit puts it back over WindowServer's own)
static void abortStack(int sig);

// An event's value: a shared event's own (signaledValue), else one kept with the event (a plain MTLEvent has none it shows).
static const void *const kEventValue = &kEventValue;
static void eventSignal(id event, uint64_t value)
{
	if ([event respondsToSelector:@selector(setSignaledValue:)])
		[event setSignaledValue:value];
	else
		objc_setAssociatedObject(event, kEventValue, @(value), OBJC_ASSOCIATION_RETAIN);
}
static void eventWait(id event, uint64_t value)
{
	enum { kTimeoutMs = 1000 };   // ponytail: a signal that never comes costs a second, not the queue (another process's, say)
	BOOL reached;
	if ([event respondsToSelector:@selector(waitUntilSignaledValue:timeoutMS:)])
		reached = [event waitUntilSignaledValue:value timeoutMS:kTimeoutMs];
	else {
		uint64_t waited = 0;
		while (!(reached = [objc_getAssociatedObject(event, kEventValue) unsignedLongLongValue] >= value) && waited < kTimeoutMs * 1000)
			usleep(200), waited += 200;
	}
	if (!reached)
		RTRACE(20, @"event %p: value %llu not reached within %d ms: the work after the wait goes on", event, value, kTimeoutMs);
}

@implementation RDNA4MTLCommandBuffer {
	RDNA4MTLCommandQueue *_queue;
	NSMutableArray<RDNA4Op> *_ops;
	NSMutableIndexSet *_draws;   // which of _ops are draws
	NSMutableArray<MTLCommandBufferHandler> *_completed, *_scheduled;
	NSMutableSet *_refreshed, *_refreshedLate, *_drawnShadows;
	NSMutableArray<id<MTLDrawable>> *_drawables;   // presented once the work has run
	NSMutableArray *_signals, *_waits;   // encodeSignalEvent: {event, value}: signalled once the work has run; encodeWaitForEvent: waited
	                                     // for before it runs (its queue's gate)
	dispatch_group_t _scheduledGroup, _done;
	NSMutableDictionary *_userDictionary;
	id<MTLBuffer> _chunk;   // set*Bytes data (stage:)
	NSUInteger _chunkUsed;
	_Atomic MTLCommandBufferStatus _status;
	uint64_t _createdUs, _committedUs;   // (the stats)
	NSError *_error;
}
+ (instancetype)allocWithZone:(NSZone *)zone
{
	gBuffersLive++;
	return [super allocWithZone:zone];
}
- (void)dealloc { gBuffersLive--; }
@synthesize label;

- (instancetype)initWithQueue:(RDNA4MTLCommandQueue *)queue
{
	if ((self = [super init])) {
		_queue = queue;
		_ops = [NSMutableArray array];
		_draws = [NSMutableIndexSet indexSet];
		_completed = [NSMutableArray array];
		_scheduled = [NSMutableArray array];
		_drawables = [NSMutableArray array];
		_signals = [NSMutableArray array];
		_waits = [NSMutableArray array];
		_scheduledGroup = dispatch_group_create();
		_createdUs = nowUs();
		_done = dispatch_group_create();
	}
	return self;
}

- (id<MTLDevice>)device { return _queue.device; }
- (void)presentDrawable:(id<MTLDrawable>)drawable { [_drawables addObject:drawable]; }
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time { [_drawables addObject:drawable]; }
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration { [_drawables addObject:drawable]; }
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (MTLCommandBufferStatus)status { return _status; }
- (NSError *)error { return _error; }
- (BOOL)retainedReferences { return YES; }
// Metal's per-command-buffer dictionary (MPS keeps its state in it; nil, it freed what it put there and crashed WindowServer)
- (NSMutableDictionary *)userDictionary
{
	@synchronized(self) {
		return _userDictionary ?: (_userDictionary = [NSMutableDictionary dictionary]);
	}
}
- (uint64_t)globalTraceObjectID { return (uint64_t)(uintptr_t)self; }
- (void)record:(RDNA4Op)op { [_ops addObject:op]; }

- (id<MTLBuffer>)stage:(const void *)bytes length:(NSUInteger)length offset:(NSUInteger *)offset
{
	enum { kChunk = 256 << 10, kAlone = 64 << 10 };
	if (length > kAlone) {
		*offset = 0;
		return [self.device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];
	}
	if (!_chunk || _chunkUsed + length > kChunk) {
		_chunk = [self.device newBufferWithLength:kChunk options:MTLResourceStorageModeShared];
		_chunkUsed = 0;
	}
	*offset = _chunkUsed;
	if (length)
		memcpy((uint8_t *)_chunk.contents + _chunkUsed, bytes, length);
	_chunkUsed += (length + 255) & ~(NSUInteger)255;
	return _chunk;
}
- (void)drewInto:(RDNA4MTLTexture *)target
{
	// a texture of a held surface that is not held itself drew into the surface: the held copy filled again before its next use
	if (heldOn() && !target.held && target.onSurface)
		[heldFor(target.root.iosurface, NO, nil, nil) cpuTouched];
	if (!target.shadowed || !target.iosurface)   // (one over a buffer: Core Animation's memory may be gone by the end)
		return;
	[target drewIntoShadow];
	_drawnShadows = _drawnShadows ?: [NSMutableSet set];
	[_drawnShadows addObject:target];
}
- (void)refreshLate:(NSArray<RDNA4MTLTexture *> *)late
{
	for (RDNA4MTLTexture *t in late) {
		if ([_refreshedLate containsObject:t])
			continue;
		_refreshedLate = _refreshedLate ?: [NSMutableSet set];
		[_refreshedLate addObject:t];
		[t refresh];
	}
}
- (NSArray<RDNA4MTLTexture *> *)refreshShadows:(NSArray<RDNA4MTLTexture *> *)shadowed
{
	NSMutableArray<RDNA4MTLTexture *> *late = [NSMutableArray array];
	for (RDNA4MTLTexture *t in shadowed) {
		if (t.iosurface) {
			[late addObject:t];
			continue;
		}
		if (!_refreshed)
			_refreshed = [NSMutableSet set];
		if (![_refreshed containsObject:t]) {
			[_refreshed addObject:t];
			[t refresh];
		}
	}
	return late;
}
- (void)recordDraw:(RDNA4Op)op
{
	[_draws addIndex:_ops.count];
	[_ops addObject:op];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block { [_completed addObject:block]; }
// Events: what Core Animation synchronises WindowServer's frames with its clients' drawing by (IOSurfaceSharedEvent-backed MTLSharedEvents,
// _MTLDevice's own). Answered as no-ops before, no client's signal ever came: WindowServer's "synchronize timed out" on our accelerator,
// then Core Animation's transient surface assert (2026-11-20). A signal after all of the buffer's work (its shadows written back), a wait
// before all of it, on the queue's gate (submit): on the device's one work queue it would hold up the signal another queue of the process
// owes it.
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
	if (event)
		[_signals addObject:@[ event, @(value) ]];
}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
	if (event)
		[_waits addObject:@[ event, @(value) ]];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)block { [_scheduled addObject:block]; }
// Metal runs a queue's command buffers in the order they were enqueued (commit enqueues if enqueue was not called): Core Animation enqueues
// one, then another, and may commit the second first. Run in commit order, a frame's draws could land under the ones meant to be below them.
- (void)enqueue
{
	if (_status >= MTLCommandBufferStatusEnqueued)
		return;
	_status = MTLCommandBufferStatusEnqueued;
	[_queue reserve:self];
}
- (void)waitUntilScheduled { if (_status >= MTLCommandBufferStatusCommitted) dispatch_group_wait(_scheduledGroup, DISPATCH_TIME_FOREVER); }
- (void)commitAndWaitUntilSubmitted   // WindowServer's commit (a no-op here before: its frames never ran, the screen stayed black)
{
	[self commit];
	[self waitUntilScheduled];
}
- (void)waitUntilCompleted { if (_status >= MTLCommandBufferStatusCommitted) dispatch_group_wait(_done, DISPATCH_TIME_FOREVER); }
- (CFTimeInterval)GPUStartTime { return 0; }
- (CFTimeInterval)GPUEndTime { return 0; }
- (CFTimeInterval)kernelStartTime { return 0; }
- (CFTimeInterval)kernelEndTime { return 0; }

- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
	return [[RDNA4MTLComputeEncoder alloc] initWithCommandBuffer:self];
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
	return [self computeCommandEncoder];   // serial either way
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)pass
{
	return [[RDNA4MTLRenderEncoder alloc] initWithCommandBuffer:self pass:pass];
}

- (id<MTLAccelerationStructureCommandEncoder>)accelerationStructureCommandEncoder
{
	return [[RDNA4MTLAccelerationStructureEncoder alloc] initWithCommandBuffer:self];
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
	return [[RDNA4MTLBlitEncoder alloc] initWithCommandBuffer:self];
}

// The work queue runs the steps in order: a draw is submitted behind the draws before it; anything else (a dispatch, a CPU clear, copy or
// build) first waits for them. The command buffer completes once its last draw has, then presents its drawables.
- (void)commitAndHold { [self commit]; }   // (Apple's GL-on-Metal renderer: committed as any other)
- (void)commit
{
	if (_status >= MTLCommandBufferStatusCommitted)
		return;
	struct sigaction now;
	if (gAbortStack && !sigaction(SIGABRT, NULL, &now) && now.sa_handler != abortStack)
		signal(SIGABRT, abortStack);
	[self enqueue];
	_committedUs = nowUs();
	statAdd(kStatEncodeUs, _committedUs - _createdUs);
	statMax(gEncodeMaxUs, _committedUs - _createdUs);
	_status = MTLCommandBufferStatusCommitted;
	dispatch_group_enter(_scheduledGroup);
	dispatch_group_enter(_done);
	[_queue committed:self];
}

// Its steps on the device's work queue (the queue calls it in enqueue order).
- (void)submit
{
	RDNA4MTLDevice *device = (RDNA4MTLDevice *)self.device;
	NSArray *waits = [_waits copy];   // (a command buffer is submitted once)
	dispatch_block_t run = ^{
		const uint64_t started = nowUs();
		statAdd(kStatWaitUs, started - self->_committedUs);
		dispatch_queue_t handlers = self->_queue.handlers;
		NSError *why = nil;
		uint64_t slowest = 0, slowestAt = 0;   // its longest step (a command buffer of 200 ms or more is logged: the pointer stood at times)
		for (NSUInteger i = 0; i < self->_ops.count && !why; i++) {
			const BOOL draw = [self->_draws containsIndex:i];
			const uint64_t t0 = nowUs();
			if (!draw)
				why = [device drainGfx] ?: heldWriteBackAll(device);   // (a CPU step may read a held surface's memory)
			const uint64_t t1 = nowUs();
			if (!why)
				why = self->_ops[i]();
			const uint64_t t2 = nowUs();
			if (t2 - t0 > slowest)
				slowest = t2 - t0, slowestAt = i;
			if (!draw)
				statAdd(kStatSteps, 1), statAdd(kStatStepUs, t2 - t1), statMax(gStepMaxUs, t2 - t1);
		}
		// Scheduled once every draw is on the gfx ring, before they are waited for: WindowServer hands its frame to the display then
		// (commitAndWaitUntilSubmitted), and the kext's present copies it behind them on the same ring, after an idle barrier. Scheduled
		// when the command buffer only started, the display showed frames half drawn (a moving window torn between two places); scheduled
		// once its draws were done, WindowServer's main thread waited for the card in 23 % of a window drag (spindump, 2026-11-20).
		NSError *flushed = [device flushBatch];   // (the last batch of draws onto the ring first)
		why = why ?: flushed;
		void (^scheduledNow)(void) = ^{
			self->_status = MTLCommandBufferStatusScheduled;
			dispatch_group_leave(self->_scheduledGroup);
			NSArray<MTLCommandBufferHandler> *scheduled = [self->_scheduled copy];
			// given up once run, as the completed ones: a handler holding its command buffer (iTerm2's) kept it, its set*Bytes buffers and
			// its ops alive for good, 52 small buffers a frame, until the client's buffer quota ran out and iTerm2 crashed (2026-11-21)
			[self->_scheduled removeAllObjects];
			if (scheduled.count)
				dispatch_async(handlers, ^{
					for (MTLCommandBufferHandler h in scheduled)
						h(self);
				});
		};
		// A buffer that drew into shadows (surfaces the card cannot draw in place) is scheduled only once their pixels are back in the
		// surfaces: iTerm2 presents its drawable when waitUntilScheduled returns (presentsWithTransaction), and WindowServer composed the
		// surface as the frame before left it: the end of the text vanished now and then and came back (2026-11-21).
		const BOOL shadowsDrawn = self->_drawnShadows.count > 0;
		if (!shadowsDrawn)
			scheduledNow();
		NSError *drained = [device drainGfx];
		self->_error = why ?: drained;
		for (RDNA4MTLTexture *t in self->_drawnShadows)   // the draws done: their pixels into the surfaces
			[t writeBack];
		[self->_drawnShadows removeAllObjects];
		// held surfaces the kext does not present (a screenshot's, read by screencaptureui as soon as this completes): their pixels into
		// their memory now, not at the idle write-back (screenshots came out all transparent, 2026-11-22)
		if (!self->_error && heldOn())
			self->_error = heldWriteBackAll(device, NO);
		if (shadowsDrawn)
			scheduledNow();
		const uint64_t done = nowUs();
		statAdd(kStatRunUs, done - started);
		uint64_t vblNs = 0;   // the kext's last VBL event on display 0 (the same clock): where in the frame this one was made, committed, done
		size_t vblLen = sizeof vblNs;
		if (!sysctlbyname("debug.rdna4_pvvblat", &vblNs, &vblLen, NULL, 0) && vblNs / 1000 <= self->_createdUs && done - vblNs / 1000 < 20000) {
			const uint64_t vbl = vblNs / 1000;
			statAdd(kStatFramed, 1), statAdd(kStatVblMade, self->_createdUs - vbl), statAdd(kStatVblCommitted, self->_committedUs - vbl),
				statAdd(kStatVblDone, done - vbl);
			static const uint64_t edges[] = { 1000, 2000, 4000, 6000, 8000 };   // us: the made histogram's buckets
			int bucket = 0;
			while (bucket < 5 && self->_createdUs - vbl >= edges[bucket])
				bucket++;
			statAdd(static_cast<RDNA4Stat>(kStatMade0 + bucket), 1);
		}
		if (done - started >= 200000)
			RLOG(@"RDNA4MTL: a command buffer (%@) ran %.0f ms: %lu steps, %lu draws, the longest step %lu (%s) %.0f ms", self.label ?: @"-",
			     (done - started) / 1e3, (unsigned long)self->_ops.count, (unsigned long)self->_draws.count, (unsigned long)slowestAt,
			     [self->_draws containsIndex:slowestAt] ? "a draw" : "not a draw", slowest / 1e3);
		statMax(gLatencyMaxUs, done - self->_committedUs);
		RTRACE(60, @"command buffer %@: %lu steps, %lu draws, %lu drawables%@%@", self.label ?: @"-", (unsigned long)self->_ops.count,
		       (unsigned long)self->_draws.count, (unsigned long)self->_drawables.count, self->_error ? @": " : @"",
		       self->_error.localizedDescription ?: @"");
		[self->_ops removeAllObjects];
		for (id<MTLDrawable> d in self->_drawables)
			[d present];
		[self->_drawables removeAllObjects];
		for (NSArray *s in self->_signals)
			eventSignal(s[0], [s[1] unsignedLongLongValue]);
		[self->_signals removeAllObjects];
		self->_status = self->_error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
		statAdd(kStatBuffers, 1);
		statReport();
		// the handlers on the queue's handler queue, in order (one that waits for the app's main thread must not stop the GPU work behind
		// it), then waitUntilCompleted returns, as Metal's does
		NSArray<MTLCommandBufferHandler> *completed = [self->_completed copy];
		[self->_completed removeAllObjects];
		dispatch_async(handlers, ^{
			for (MTLCommandBufferHandler h in completed)
				h(self);
			dispatch_group_leave(self->_done);
		});
		heldIdle(device);
	};
	// through the queue's gate, in order: the events it waits for first, there (its queue's later buffers wait behind it, others do not)
	dispatch_async(_queue.gate, ^{
		for (NSArray *w in waits)
			eventWait(w[0], [w[1] unsignedLongLongValue]);
		dispatch_async(device.work, run);
	});
}

RDNA4_ANSWER_MISSING
@end

@implementation RDNA4MTLCommandQueue {
	RDNA4MTLDevice *_device;
	dispatch_queue_t _own, _app, _gate;
	NSMutableArray *_order;   // enqueued, not yet submitted, in enqueue order
}

- (void)reserve:(id)commandBuffer
{
	@synchronized(self) {
		if (!_order)
			_order = [NSMutableArray array];
		[_order addObject:commandBuffer];
	}
}

- (void)committed:(id)commandBuffer
{
	@synchronized(self) {
		while (_order.count && [(id<MTLCommandBuffer>)_order[0] status] >= MTLCommandBufferStatusCommitted) {
			[(RDNA4MTLCommandBuffer *)_order[0] submit];
			[_order removeObjectAtIndex:0];
		}
		int64_t held = 0;
		for (id<MTLCommandBuffer> b in _order)
			held += b.status >= MTLCommandBufferStatusCommitted;
		gOrderWaiting = held;
	}
}
@synthesize label;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device
{
	if ((self = [super init])) {
		_device = device;
		_own = dispatch_queue_create("RDNA4MTL handlers", DISPATCH_QUEUE_SERIAL);
		_gate = dispatch_queue_create("RDNA4MTL gate", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
	}
	return self;
}

- (dispatch_queue_t)handlers { return _app ?: _own; }
- (dispatch_queue_t)gate { return _gate; }
- (void)setCompletionQueue:(dispatch_queue_t)queue { _app = queue; }
- (void)setSubmissionQueue:(dispatch_queue_t)queue { }
- (void)setGPUPriority:(NSUInteger)priority { }

- (id<MTLDevice>)device { return _device; }
- (id<MTLCommandBuffer>)commandBuffer { return [[RDNA4MTLCommandBuffer alloc] initWithQueue:self]; }
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d { return [self commandBuffer]; }   // (its options: none apply)

RDNA4_ANSWER_MISSING
@end

// ---- the device

// A PM4 stream's register value: the SET_*_REG packet `opcode` that sets `reg` (the PV host's streamReg), else NULL.
static uint32_t *streamReg(uint32_t *ib, uint32_t n, uint32_t opcode, uint32_t reg)
{
	for (uint32_t i = 0; i + 1 < n;) {
		const uint32_t h = ib[i], count = ((h >> 16) & 0x3fff) + 1;   // PM4 type 3: dwords after the header
		if ((h >> 30) != 3)
			return NULL;
		if (((h >> 8) & 0xff) == opcode && reg >= ib[i + 1] && reg - ib[i + 1] < count - 1 && i + 2 + (reg - ib[i + 1]) < n)
			return &ib[i + 2 + (reg - ib[i + 1])];
		i += 1 + count;
	}
	return NULL;
}

static uint32_t floatBits(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

// the state page: the stream's fence at +0, the vertex stage's table at +0x400, the fragment stage's at +0x800, the levels' T#s from +0x1000
// (RDNA4_MTLC_LEVEL_FIRST, written only for a stage that samples at explicit LODs)
enum { kMiscBytes = 0x5800, kMiscTables = 0x1000 };   // (0x5800: the fragment stage's texture handles end at 0x5800)
// draws in flight: each has its own IB page and state page until its fence has come. 16 kept the submit loop waiting for the card for
// most of a frame of 80 draws, and WindowServer's main thread with it (commitAndWaitUntilSubmitted: 25 % of a window drag, 2026-11-20)
enum { kSlots = 128, kBatchSlots = 32 };   // a batch is submitted at kBatchSlots: up to 96 slots in flight while the next is written

// A fence: nothing to wait for, a command buffer's steps run in order on the device's one work queue (VectorKit makes one: Maps,
// 2026-11-20). Encoders' updateFence: / waitForFence: are answered by RDNA4_ANSWER_MISSING (nothing).
@interface RDNA4MTLFence : NSObject <MTLFence>
- (instancetype)initWithDevice:(id<MTLDevice>)device;
@end

@implementation RDNA4MTLFence {
	id<MTLDevice> __weak _device;
}
@synthesize label;
- (instancetype)initWithDevice:(id<MTLDevice>)device
{
	if ((self = [super init]))
		_device = device;
	return self;
}
- (id<MTLDevice>)device { return _device; }
@end

// GL windows without the kernel's accelerator surfaces (2026-11-21). On Tahoe a GL window's surface lives in the app's own SkyLight
// (CAWindowContent): SLSBindSurface makes an accelerator surface for it (IOAccelCreateSurface, client type 0 on our accelerator), sets its
// shape, and each flush asks it for the buffer to show (IOAccelSurfaceControl -> an IOSurface's mach port), which becomes the window's
// content; Apple's GL drawable (client type 9, never run on our accelerator) fills those buffers in the kernel. Here, in a process with
// Apple's GL-on-Metal renderer only: SkyLight's surface calls are redirected (its import pointers) to surfaces of this file, and our
// drawable gives GL their IOSurfaces. Other surfaces (another accelerator's) go to the real calls.
struct GLSurface {
	uint32_t id, options, width = 0, height = 0;
	IOSurfaceRef buffers[2] = {};
};
static std::mutex gGLSurfacesLock;
static std::unordered_map<uint64_t, GLSurface> gGLSurfaces;   // by our connect (kGLConnect + n)
static constexpr uint64_t kGLConnect = 0x7d000000;
static kern_return_t (*gRealCreateSurface)(io_service_t, uint32_t, uint32_t, uint64_t *);
static kern_return_t (*gRealDestroySurface)(uint64_t);
static kern_return_t (*gRealSurfaceControl)(uint64_t, uint32_t, uint32_t, uint32_t *);
static kern_return_t (*gRealSetShape)(uint64_t, const void *, uint32_t, uint32_t);
static kern_return_t (*gRealSetScale)(uint64_t, uint32_t, const void *);
static kern_return_t (*gRealWaitForSurface)(uint64_t);
static bool glConnect(uint64_t c) { return (c & ~0xffffffull) == kGLConnect; }
// (the caller holds gGLSurfacesLock) the surface with SkyLight's id, or null
static GLSurface *glSurfaceWithId(uint32_t sid)
{
	for (auto &g : gGLSurfaces)
		if (g.second.id == sid)
			return &g.second;
	return nullptr;
}

// (the caller holds gGLSurfacesLock) the surface's buffers at its size, BGRA8
static void glSurfaceBuffers(GLSurface &g)
{
	for (IOSurfaceRef &b : g.buffers) {
		if (b && IOSurfaceGetWidth(b) == g.width && IOSurfaceGetHeight(b) == g.height)
			continue;
		if (b)
			CFRelease(b);
		b = g.width && g.height ? IOSurfaceCreate((__bridge CFDictionaryRef)@{ (id)kIOSurfaceWidth : @(g.width), (id)kIOSurfaceHeight : @(g.height),
		                                                                         (id)kIOSurfaceBytesPerElement : @4, (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA') })
		                        : nullptr;
	}
}

static kern_return_t glCreateSurface(io_service_t accel, uint32_t sid, uint32_t options, uint64_t *out)
{
	io_name_t cls = "";
	IOObjectGetClass(accel, cls);
	if (strcmp(cls, "RDNA4Accelerator"))
		return gRealCreateSurface(accel, sid, options, out);
	static uint32_t next;
	std::lock_guard<std::mutex> hold(gGLSurfacesLock);
	*out = kGLConnect + (++next & 0xffffff);
	gGLSurfaces[*out] = GLSurface{ sid, options };
	RTRACE(40, @"GL surface 0x%x (options 0x%x): connect 0x%llx", sid, options, *out);
	return KERN_SUCCESS;
}
static kern_return_t glDestroySurface(uint64_t c)
{
	if (!glConnect(c))
		return gRealDestroySurface(c);
	std::lock_guard<std::mutex> hold(gGLSurfacesLock);
	auto it = gGLSurfaces.find(c);
	if (it != gGLSurfaces.end()) {
		for (IOSurfaceRef b : it->second.buffers)
			if (b)
				CFRelease(b);
		gGLSurfaces.erase(it);
	}
	return KERN_SUCCESS;
}
// IOAccelDeviceRegion {u32 rects, IOAccelBounds bounds, IOAccelBounds rect[]}, IOAccelBounds {s16 x, y, w, h}: the bounds' size
static kern_return_t glSetShape(uint64_t c, const void *region, uint32_t options, uint32_t index)
{
	if (!glConnect(c))
		return gRealSetShape(c, region, options, index);
	const int16_t *bounds = (const int16_t *)((const uint8_t *)region + 4);
	std::lock_guard<std::mutex> hold(gGLSurfacesLock);
	auto it = gGLSurfaces.find(c);
	if (it != gGLSurfaces.end() && region && bounds[2] > 0 && bounds[3] > 0) {
		it->second.width = (uint32_t)bounds[2], it->second.height = (uint32_t)bounds[3];
		RTRACE(40, @"GL surface 0x%x: shape %d,%d %dx%d (options 0x%x, index %u)", it->second.id, bounds[0], bounds[1], bounds[2], bounds[3], options, index);
	}
	return KERN_SUCCESS;
}
static kern_return_t glSurfaceControl(uint64_t c, uint32_t cmd, uint32_t arg, uint32_t *out)
{
	if (!glConnect(c))
		return gRealSurfaceControl(c, cmd, arg, out);
	if (out)
		*out = 0;
	if (cmd == 0x14) {   // the buffer to show (SLSFlushSurfaceWithOptionsAndIndex): a send right for its IOSurface, which SkyLight gives up
		std::lock_guard<std::mutex> hold(gGLSurfacesLock);
		auto it = gGLSurfaces.find(c);
		IOSurfaceRef b = it != gGLSurfaces.end() && arg < 2 ? it->second.buffers[arg] : nullptr;
		if (b && out)
			*out = IOSurfaceCreateMachPort(b);
		RTRACE(40, @"GL surface 0x%x: buffer %u shown (%@)", it != gGLSurfaces.end() ? it->second.id : 0, arg, b ? @"ours" : @"none");
		return KERN_SUCCESS;
	}
	RTRACE(80, @"GL surface control 0x%llx: command 0x%x, argument 0x%x", c, cmd, arg);
	return KERN_SUCCESS;
}
static kern_return_t glSetScale(uint64_t c, uint32_t options, const void *scale)
{
	return glConnect(c) ? KERN_SUCCESS : gRealSetScale(c, options, scale);
}
static kern_return_t glWaitForSurface(uint64_t c) { return glConnect(c) ? KERN_SUCCESS : gRealWaitForSurface(c); }

// `image`'s import of `name` (its stub's pointer: jmpq *slot(%rip)) to `to`, the real one into *real. ponytail: x86_64 stubs only.
static bool rebindImport(const struct mach_header_64 *image, const char *name, void *to, void **real)
{
	void *target = dlsym(RTLD_DEFAULT, name);
	unsigned long size = 0;
	uint8_t *stubs = getsectiondata(image, "__TEXT", "__stubs", &size);
	bool done = false;
	for (unsigned long i = 0; target && stubs && i + 6 <= size; i += 6) {
		if (stubs[i] != 0xff || stubs[i + 1] != 0x25)
			continue;
		int32_t disp;
		memcpy(&disp, stubs + i + 2, 4);
		void **slot = (void **)(stubs + i + 6 + disp);
		if (*slot != target)
			continue;
		const vm_address_t page = (vm_address_t)slot & ~(vm_address_t)(vm_page_size - 1);
		if (vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
			continue;
		*real = target;
		*slot = to;
		vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ);
		done = true;
	}
	return done;
}

// SkyLight's surface calls to ours, once, in a GL process (never WindowServer)
static void glSurfacesInstall(void)
{
	static std::mutex lock;
	static bool tried;   // set once the renderer was there to decide (the device is made before it is loaded)
	std::lock_guard<std::mutex> hold(lock);
	if (tried || !strcmp(getprogname(), "WindowServer"))
		return;
	const struct mach_header_64 *sky = nullptr, *gl = nullptr;
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *n = _dyld_get_image_name(i);
		if (strstr(n, "/SkyLight.framework/"))
			sky = (const struct mach_header_64 *)_dyld_get_image_header(i);
		// a GL process: its renderer, or OpenGL.framework (our device is made while its GL context is, before a window's surface is
		// bound, and the renderer may not be loaded yet then)
		if (strstr(n, "AppleMetalOpenGLRenderer") || strstr(n, "/OpenGL.framework/Versions/A/OpenGL"))
			gl = (const struct mach_header_64 *)_dyld_get_image_header(i);
	}
	if (!sky || !gl)   // not a GL process (again at the next GL drawable)
		return;
	tried = true;
	const bool ok = rebindImport(sky, "IOAccelCreateSurface", (void *)glCreateSurface, (void **)&gRealCreateSurface) &&
	                rebindImport(sky, "IOAccelDestroySurface", (void *)glDestroySurface, (void **)&gRealDestroySurface) &&
	                rebindImport(sky, "IOAccelSurfaceControl", (void *)glSurfaceControl, (void **)&gRealSurfaceControl) &&
	                rebindImport(sky, "IOAccelSetSurfaceFramebufferShape", (void *)glSetShape, (void **)&gRealSetShape) &&
	                rebindImport(sky, "IOAccelSetSurfaceScale", (void *)glSetScale, (void **)&gRealSetScale) &&
	                rebindImport(sky, "IOAccelWaitForSurface", (void *)glWaitForSurface, (void **)&gRealWaitForSurface);
	RLOG(@"RDNA4MTL: GL window surfaces in this process: %@", ok ? @"ours" : @"NOT redirected (SkyLight's imports not found)");
}

// A GL window's drawable: the IOSurfaces of the surface with its id (setDrawableSurface)
@interface RDNA4GLDrawable : NSObject
@end
@implementation RDNA4GLDrawable {
	uint32_t _sid;
}
- (GLSurface *)surface { return glSurfaceWithId(_sid); }   // (the caller holds gGLSurfacesLock)
- (int)setDrawableSurface:(unsigned)surface mode:(unsigned long long)mode colorDepthMode:(unsigned)depth face:(unsigned)face level:(unsigned)level
                 volatile:(unsigned)vol fixedSource:(unsigned)fixed scaleOptions:(unsigned)scale scaledWidth:(unsigned)w scaledHeight:(unsigned)h
{
	_sid = surface;
	RTRACE(40, @"GL drawable: surface 0x%x mode 0x%llx depth %u scale 0x%x %ux%u", surface, mode, depth, scale, w, h);
	return 0;
}
// +1: the renderer CFReleases what it gets (GLRDrawable, after making its textures of it); given +0, every lookup over-released the
// window's buffer, and Mixxx crashed releasing one that was gone (2026-11-21)
- (IOSurfaceRef)lookupIOSurfaceAtIndex:(unsigned long long)i
{
	std::lock_guard<std::mutex> hold(gGLSurfacesLock);
	GLSurface *g = [self surface];
	if (!g || i >= 2)
		return nullptr;
	glSurfaceBuffers(*g);
	return g->buffers[i] ? (IOSurfaceRef)CFRetain(g->buffers[i]) : nullptr;
}
- (unsigned)width { std::lock_guard<std::mutex> hold(gGLSurfacesLock); GLSurface *g = [self surface]; return g ? g->width : 0; }
- (unsigned)height { std::lock_guard<std::mutex> hold(gGLSurfacesLock); GLSurface *g = [self surface]; return g ? g->height : 0; }
- (unsigned)surfaceWidth { return [self width]; }
- (unsigned)surfaceHeight { return [self height]; }
- (int)clearDrawable { _sid = 0; return 0; }
- (int)setSwapInterval:(int)interval limit:(int)limit { return 0; }
- (int)setSwapRectX:(unsigned)x y:(unsigned)y w:(unsigned)w h:(unsigned)h { return 0; }
- (unsigned long long)windowMode { return 0; }
- (int)signalSharedEvent:(id)e value:(unsigned long long)v operation:(unsigned long long)op { return 0; }
RDNA4_ANSWER_MISSING
@end

@implementation RDNA4MTLDevice {
	id<MTLBuffer> _handles;           // (handle:words:count:) the slots, the spare T# in each free one: an explicit-LOD sample reads past its
	NSMutableIndexSet *_freeHandles;  // handle's slot (RDNA4_MTLC_LEVEL_FIRST), and an all-zero T# can stall the texture unit
	uint64_t _spareHandle;            // (spareHandle)
	io_service_t _service;
	uint64_t _registryID;
	__weak id _accelService;   // Metal's MTLIOAccelService for our service
	rdna4_t _gpu;
	BOOL _open;
	dispatch_queue_t _work;   // runs committed command buffers, in commit order (every queue's)
	// gfx: made by the first render pipeline; _misc and _ib hold kSlots state pages and IB pages, used in turn by the draws
	BOOL _gfxReady;
	rdna4_buffer_t _rings, _spare, _misc, _ib;
	uint8_t *_miscCpu, *_ibCpu;
	rdna4_buffer_t _idle;   // gpuBarrierCopyFrom:'s word: the card's end-of-pipe write of _idleSeq, waited for
	uint32_t _idleSeq;   // _misc and _ib in this process: snooped host memory, written in place (through the kext: 55 us a draw)
	uint64_t _slotFence[kSlots], _lastFence;   // a slot's draw's fence, while it may still run; the last draw's
	RDNA4MTLRenderPipeline *_slotPipe[kSlots];   // what each slot's draw is (pipeline, vertices, instances, target size and format): a hang
	uint32_t _slotWhat[kSlots][5];               // names the draws in flight (described then: formatted for each draw, 4000 a second, it cost)
	NSHashTable *_drawnInFlight;               // the targets of the draws since the last drainGfx (work queue only)
	NSMutableDictionary<NSString *, RDNA4MTLTexture *> *_standIns;   // standInFor:, by size and samples
	BOOL _drawsInFlight;                       // a draw since the last drainGfx: drainGfx checks _lastSlot's stream fence word
	uint32_t _slot, _lastSlot;
	uint32_t _batchFirst, _batchCount;   // the slots written and not yet submitted (queueSlot, flushBatch): consecutive, from _batchFirst
	rdna4_program_t _resolve[2];
}

static NSString *serviceString(io_service_t service, CFStringRef key);

// Our own accelerator: by its class, which a sandbox lets a process see where it refuses the properties (WebKit's GPU process,
// iokit-get-properties), else by its property
static bool isOurAccelerator(io_service_t service)
{
	io_name_t cls = "";
	return (!IOObjectGetClass(service, cls) && !strcmp(cls, "RDNA4Accelerator")) || serviceString(service, CFSTR("RDNA4Accelerator"));
}

+ (void)load
{
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");   // Metal's private protocol, also asserted by MTLAddDevice
	if (spi)
		class_addProtocol(self, spi);
}

// WindowServer on our own accelerator: an abort's stack into the unified log (ReportCrash writes few reports for a process that keeps
// crashing). ponytail: backtrace_symbols and os_log are not async-signal-safe; this process is dying anyway.
static void abortStack(int sig)
{
	void *frames[64];
	const int n = backtrace(frames, 64);
	char **names = backtrace_symbols(frames, n);
	for (int i = 0; names && i < n; i++)
		os_log(OS_LOG_DEFAULT, "RDNA4MTL abort: %{public}s", names[i]);
	signal(sig, SIG_DFL);
	raise(sig);
}

- (instancetype)initWithAcceleratorPort:(io_service_t)service
{
	if (isOurAccelerator(service) && !strcmp(getprogname(), "WindowServer") && !gAbortStack) {
		gAbortStack = true;
		// kept as SIGABRT's handler over WindowServer's own from a timer too, for 2 minutes (it may abort before its first commit)
		static dispatch_source_t keep;
		keep = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
		__block int ticks = 0;
		dispatch_source_set_timer(keep, DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
		dispatch_source_set_event_handler(keep, ^{
			struct sigaction now;
			if (!sigaction(SIGABRT, NULL, &now) && now.sa_handler != abortStack)
				signal(SIGABRT, abortStack);
			if (++ticks >= 2400)
				dispatch_source_cancel(keep);
		});
		dispatch_resume(keep);
	}
	// always on our own accelerator (rdna4accel.cpp: no other device there), else only with RDNA4_NATIVE_MTL
	if (!getenv("RDNA4_NATIVE_MTL") && !isOurAccelerator(service)) {
		self = [super init];   // before the nil return: _MTLDevice's dealloc expects its init to have run
		return nil;
	}
	return [self initNativeWithAcceleratorPort:service];
}

- (instancetype)initNativeWithAcceleratorPort:(io_service_t)service
{
	self = [super init];
	if (!self)
		return nil;
	gGlDevice = self;
	glSurfacesInstall();   // (a GL process: before SkyLight binds its window surfaces)
	IOObjectRetain(service);
	_service = service;
	IORegistryEntryGetRegistryEntryID(service, &_registryID);
	// through the accelerator first (a sandboxed app, WebKit's GPU process, may open only an IOAccelerator's clients), else our service
	// (the native spike's own service, debug.rdna4_nativemtl=1, makes a client of type RDNA4_NATIVE_UC_TYPE that is not the runtime's:
	// mtlprobe's native checks then had no gfx, 2026-11-20)
	rdna4_info_t info;
	kern_return_t viaAccel = rdna4_open_accelerator(&_gpu, service);
	if (!viaAccel && (viaAccel = rdna4_info(&_gpu, &info)))
		rdna4_close(&_gpu);
	const kern_return_t kr = viaAccel ? rdna4_open(&_gpu) : KERN_SUCCESS;
	_open = kr == KERN_SUCCESS;
	if (viaAccel)
		RTRACE(4, @"RDNA4MTLDevice: the runtime client through the accelerator: %s", rdna4_error(viaAccel));
	// user-interactive: WindowServer's main thread waits for its submits (commitAndWaitUntilSubmitted); at the default QoS it ran at 31
	_work = dispatch_queue_create("RDNA4MTL work", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
	RLOG(@"RDNA4MTLDevice: for service 0x%x (registry ID 0x%llx): the kext's runtime client: %s", service, _registryID,
	      _open ? "open" : rdna4_error(kr));
	return _open ? self : nil;   // no device without the GPU (the router then makes Apple's)
}

- (void)dealloc
{
	rdna4_buffer_t *gfx[5] = { &_rings, &_spare, &_misc, &_ib, &_idle };
	for (int i = 0; i < 4; i++)
		if (gfx[i]->handle)
			rdna4_free(&_gpu, gfx[i]);
	if (_open)
		rdna4_close(&_gpu);
	IOObjectRelease(_service);
}

- (rdna4_t *)gpu { return _open ? &_gpu : NULL; }

- (BOOL)gfx
{
	if (_gfxReady || !_open)
		return _gfxReady;
	rdna4_info_t info;
	kern_return_t kr = rdna4_info(&_gpu, &info);
	// WindowServer makes its device about a second before the kext's gfx ring is up (rig, 2026-11-19): wait for it, up to 15 s
	for (int i = 0; i < 150 && !kr && (info.flags & RDNA4_FLAG_VM) && !(info.flags & RDNA4_FLAG_GFX); i++) {
		usleep(100000);
		kr = rdna4_info(&_gpu, &info);
	}
	if (kr || !(info.flags & RDNA4_FLAG_VM) || !(info.flags & RDNA4_FLAG_GFX)) {
		rdna4Error([NSString stringWithFormat:@"no client gfx IBs (flags 0x%llx: a private GPU VM and rdna4-gfx=2 needed)", info.flags], kr);
		return NO;
	}
	const uint64_t ringBytes = rdna4_tri_ring_bytes() + RDNA4_TRIRUN_RING_SLACK;
	static const uint8_t zeros[65536] = {};
	if ((kr = rdna4_alloc(&_gpu, ringBytes, &_rings)) || (!_spare.handle && (kr = rdna4_alloc(&_gpu, sizeof zeros, &_spare))) ||
	    (kr = rdna4_write(&_gpu, &_spare, 0, zeros, sizeof zeros)) || (kr = rdna4_alloc_host(&_gpu, kSlots * kMiscBytes, &_misc, (void **)&_miscCpu)) ||
	    (kr = rdna4_alloc_host_flags(&_gpu, kSlots * RDNA4_TRIRUN_PAGE, RDNA4_HOST_EXECUTABLE, &_ib, (void **)&_ibCpu)) ||
	    (kr = rdna4_alloc_host(&_gpu, 4096, &_idle, NULL))) {
		rdna4Error(@"the gfx buffers", kr);
		return NO;
	}
	if (!rdna4_trirun_rings_fit(_rings.gpu, _rings.bytes, rdna4_tri_ring_bytes())) {
		rdna4Error(@"the 2 MiB-aligned ring block does not fit its allocation", 0);
		return NO;
	}
	return _gfxReady = YES;
}

- (uint64_t)gfxRings { return rdna4_trirun_rings_va(_rings.gpu); }

// By the root texture: a pass draws into a level's view (level:) or a view, the next draw samples the texture; by the object, that draw
// missed the barrier and read texels not yet written: squares in every menu's glass blur, popping in and out (2026-11-22)
- (BOOL)drawnInFlight:(RDNA4MTLTexture *)t { return [_drawnInFlight containsObject:t.root]; }

// ponytail: a whole BGRA8 texture per depth size that is never written (CB_TARGET_MASK 0); the colour block's registers only need
// something of that size. Its memory could be skipped with a target of no format.
- (RDNA4MTLTexture *)standInFor:(RDNA4MTLTexture *)depth
{
	const NSUInteger samples = MAX(depth.samples, 1u);
	NSString *key = [NSString stringWithFormat:@"%lux%lux%lu", (unsigned long)depth.width, (unsigned long)depth.height, (unsigned long)samples];
	@synchronized(self) {
		RDNA4MTLTexture *t = _standIns[key];
		if (t)
			return t;
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:depth.width
		                                                                             height:depth.height mipmapped:NO];
		d.usage = MTLTextureUsageRenderTarget;
		d.storageMode = MTLStorageModePrivate;
		if (samples > 1)
			d.textureType = MTLTextureType2DMultisample, d.sampleCount = samples;
		t = (RDNA4MTLTexture *)[self newTextureWithDescriptor:d];
		if ([t isKindOfClass:[RDNA4MTLTexture class]]) {
			_standIns = _standIns ?: [NSMutableDictionary dictionary];
			_standIns[key] = t;
			return t;
		}
		return nil;
	}
}

// A slot's IB onto the gfx ring; again while the ring is full (kIOReturnBusy: every client's submissions in it at once, at most 128;
// kIOReturnNoResources: no room for the packets), for up to 2 s.
// The next slot for a draw or a barrier, its earlier use waited for. Its IB page goes onto the ring with the pages written before it in
// one submit (queueSlot, flushBatch): a batch is submitted before the slots wrap (its pages must be one range) and at 32 slots (a slot is
// never waited for while its own batch is unsubmitted: 128 slots).
- (NSError *)takeSlot:(uint32_t *)slot
{
	*slot = _slot++ % kSlots;
	NSError *why = _batchCount && (!*slot || _batchCount >= kBatchSlots) ? [self flushBatch] : nil;
	if (why)
		return why;
	const uint64_t t0 = nowUs();
	const kern_return_t kr = _slotFence[*slot] ? rdna4_wait_gfx_fence(&_gpu, _slotFence[*slot], 2000, NULL) : KERN_SUCCESS;
	statAdd(kStatSlotUs, nowUs() - t0);
	if (kr)
		return rdna4Error([NSString stringWithFormat:@"waiting for slot %u's earlier draw", *slot], kr);
	_slotFence[*slot] = 0;
	return nil;
}

// Slot `slot`'s IB (n dwords in its page) padded to the page's end with a NOP and added to the batch: the pages of consecutive slots are
// then one IB. One submit (a kext call, 8 us) a draw was a third of WindowServer's wait for its frames' submits (2026-11-20).
- (void)queueSlot:(uint32_t)slot dwords:(uint32_t)n
{
	uint32_t *page = (uint32_t *)(_ibCpu + (uint64_t)slot * RDNA4_TRIRUN_PAGE);
	const uint32_t room = RDNA4_TRIRUN_PAGE / 4 - n;
	if (room == 1)
		page[n] = 0xffff1000u;   // the one-dword NOP (PACKET3(NOP, 0x3FFF))
	else if (room)
		page[n] = 0xc0001000u | (room - 2) << 16;   // NOP: a header and room - 1 dwords skipped
	if (!_batchCount)
		_batchFirst = slot;
	_batchCount++;
}

- (NSError *)flushBatch
{
	if (!_batchCount)
		return nil;
	__sync_synchronize();   // the pages in memory before the card reads them
	const uint64_t t0 = nowUs();
	uint64_t fence = 0;
	const uint32_t first = _batchFirst, count = _batchCount;
	_batchCount = 0;
	const kern_return_t kr = [self submitIb:(uint64_t)first * RDNA4_TRIRUN_PAGE dwords:count * (RDNA4_TRIRUN_PAGE / 4) fence:&fence];
	statAdd(kStatSubmitUs, nowUs() - t0);
	if (kr)
		return rdna4Error([NSString stringWithFormat:@"%u draws and barriers onto the gfx ring", count], kr);
	for (uint32_t i = 0; i < count; i++)
		_slotFence[first + i] = fence;
	_lastFence = fence;
	return nil;
}

- (kern_return_t)submitIb:(uint64_t)offset dwords:(uint32_t)n fence:(uint64_t *)fence
{
	kern_return_t kr;
	for (int tries = 0; ((kr = rdna4_submit_gfx_ib(&_gpu, &_ib, offset, n, fence)) == kIOReturnBusy || kr == kIOReturnNoResources) && tries < 20000;
	     tries++)
		usleep(100);
	return kr;
}

// An IB of its own in the next slot, as the PV host's rowsOnCard: its idle barrier (RELEASE_MEM of a new sequence number with the colour
// caches flushed and the L2 written back, end of pipe: after every draw before; WAIT_REG_MEM for it), then DMA_DATA through the L2 (CP_SYNC
// on the last: the CP goes on once it is copied); the next draw's ACQUIRE_MEM invalidates the shader caches. Waiting instead for the last
// draw's own stream fence word hung the ring within seconds (2026-11-20, cause not found). On the CPU (a drain, then 33 MB a 4K target), it
// was most of WindowServer's frame time.
- (NSError *)gpuBarrier:(const RDNA4Copy *)copies count:(uint32_t)count
{
	// the gfx buffers made first, as a draw does (a pass whose first work is a clear, GL's into its window's buffer, reached them unmade:
	// a null IB page, 2026-11-21)
	if (!self.gfx)
		return rdna4Error(@"a copy or fill on the card without the device's gfx buffers", 0);
	static uint32_t ib[RDNA4_TRIRUN_PAGE / 4];
	uint32_t n = _lastFence || _batchCount ? [self idleBarrier:ib] : 0;
	uint64_t total = 0;
	for (uint32_t c = 0; c < count; c++)
	for (uint64_t at = 0, bytes = copies[c].bytes; at < bytes;) {
		if (n + 7 > sizeof ib / 4 - 1)   // (a dword left for the page's NOP)
			return rdna4Error([NSString stringWithFormat:@"%u copies on the card: more than their IB holds", count], 0);
		// 32 MiB a packet (BYTE_COUNT is 26 bits from gfx9 on, Mesa's S_415_BYTE_COUNT_GFX9): 1 MiB ones overflowed the page for vkQuake's
		// 4K multisampled depth clear (device lost, 2026-11-21)
		const uint32_t len = (uint32_t)MIN(bytes - at, (uint64_t)1 << 25);
		const uint64_t from = copies[c].src + at, to = copies[c].dst + at;
		const bool fill = !copies[c].src;   // SRC_SEL DATA: the pattern (in SRC_ADDR_LO) over the bytes
		total += fill ? 0 : len;
		const uint32_t dma[7] = { 0xc0055000u, 3u << 20 | (fill ? 2u : 3u) << 29 | (c + 1 == count && at + len >= bytes ? 1u << 31 : 0),
			                      fill ? copies[c].pattern : (uint32_t)from, fill ? 0 : (uint32_t)(from >> 32), (uint32_t)to, (uint32_t)(to >> 32),
			                      len };   // DST_SEL, SRC_SEL: the L2; CP_SYNC on the last
		memcpy(ib + n, dma, sizeof dma);
		n += 7;
		at += len;
	}
	if (!n)
		return nil;   // nothing in flight to wait for, nothing to copy
	uint32_t slot = 0;
	NSError *why = [self takeSlot:&slot];
	if (why)
		return why;
	memcpy(_ibCpu + (uint64_t)slot * RDNA4_TRIRUN_PAGE, ib, n * 4);
	[self queueSlot:slot dwords:n];   // (_lastSlot stays the last draw's: its stream fence is what drainGfx checks)
	_slotPipe[slot] = nil;
	[_drawnInFlight removeAllObjects];
	statAdd(kStatSelfReads, total ? 1 : 0);
	statAdd(kStatSelfReadBytes, total);
	statAdd(kStatBarriers, 1);
	return nil;
}

// As the PV host's stageTextures: an image instruction on an all-zero T# can stall the texture unit (an unbound slot hung WindowServer's
// first native draws and with them the gfx ring, 2026-11-20).
- (const rdna4_program_t *)resolveKernel:(BOOL)srgb
{
	const char *name = srgb ? "rdna4_resolve_srgb" : "rdna4_resolve";
	@synchronized(self) {
		rdna4_program_t *p = &_resolve[srgb ? 1 : 0];
		for (unsigned i = 0; !p->handle && i < kMtlKernelCount; i++)
			if (!strcmp(kMtlKernels[i].name, name) && rdna4_load(&_gpu, kMtlKernels[i].elf, kMtlKernels[i].size, name, p))
				return NULL;
		return p->handle ? p : NULL;
	}
}

- (void)spareImage:(uint32_t[8])t
{
	@synchronized(self) {
		if (!_spare.handle) {
			static const uint8_t zeros[65536] = {};
			if (rdna4_alloc(&_gpu, sizeof zeros, &_spare) || rdna4_write(&_gpu, &_spare, 0, zeros, sizeof zeros))
				_spare.handle = 0;
		}
	}
	linearImage(t, _spare.gpu, 42, 4 | 5 << 3 | 6 << 6 | 7 << 9, 1, 1, 64);
}

- (uint64_t)spareHandle
{
	@synchronized(self) {
		if (!_spareHandle) {
			uint32_t spare[8];
			[self spareImage:spare];
			_spareHandle = [self handle:0 words:spare count:8];
		}
		return _spareHandle;
	}
}

// ponytail: 32768 slots (1 MiB) for the process, its textures and samplers that were asked their gpuResourceID; a second slab if that runs out.
- (uint64_t)handle:(uint64_t)handle words:(const uint32_t *)words count:(unsigned)count
{
	static const NSUInteger kHandles = 32768;
	@synchronized(self) {
		if (!_handles) {
			uint32_t spare[8];
			[self spareImage:spare];
			_handles = [self newBufferWithLength:kHandles * 32 options:MTLResourceStorageModeShared];
			for (NSUInteger i = 0; _handles && i < kHandles; i++)
				memcpy((uint8_t *)_handles.contents + 32 * i, spare, 32);
			_freeHandles = [NSMutableIndexSet indexSetWithIndexesInRange:NSMakeRange(0, kHandles)];
		}
		if (!handle && _handles && _freeHandles.count) {
			const NSUInteger i = _freeHandles.firstIndex;
			[_freeHandles removeIndex:i];
			handle = _handles.gpuAddress + 32 * i;
		}
		if (handle)
			memcpy((uint8_t *)_handles.contents + (handle - _handles.gpuAddress), words, 4 * count);
		return handle;
	}
}

- (void)freeHandle:(uint64_t)handle
{
	if (!handle)
		return;
	uint32_t spare[8];
	[self spareImage:spare];
	@synchronized(self) {
		memcpy((uint8_t *)_handles.contents + (handle - _handles.gpuAddress), spare, 32);
		[_freeHandles addIndex:(NSUInteger)(handle - _handles.gpuAddress) / 32];
	}
}

- (uint64_t)spareBuffer
{
	uint32_t t[8];
	[self spareImage:t];   // makes it
	return _spare.gpu;
}
- (dispatch_queue_t)work { return _work; }

// Waits for the draws submitted so far (on the work queue: before the CPU or the compute queue touches what they wrote, and at the end of
// a command buffer). The gfx fences are ordered, so the last one is enough; its stream's own fence word says the stream ran to its end.
// An idle barrier: the draws before it done, the caches flushed and the L2 written back (gpuBarrier's opening, a ring dispatch's ends).
- (uint32_t)idleBarrier:(uint32_t *)ib
{
	const uint64_t word = _idle.gpu;
	const uint32_t seq = ++_idleSeq ?: ++_idleSeq;
	const uint32_t barrier[15] = {
		Pm4::header(Pm4::OpReleaseMem, 6), Pm4::kReleaseGcrSeq | Pm4::kReleaseGcrGl2Wb | Pm4::kReleaseCachePolicy3 | Pm4::kEventCacheFlushTs |
		Pm4::kReleaseEventIndex5, Pm4::kReleaseData32, (uint32_t)word, (uint32_t)(word >> 32), seq, 0, 0,
		0xc0053c00u, 3u | 1u << 4 | 1u << 8, (uint32_t)word, (uint32_t)(word >> 32), seq, 0xffffffffu, 4 };   // WAIT_REG_MEM: equal, memory, PFP
	memcpy(ib, barrier, sizeof barrier);
	return 15;
}

// Scratch and user data other than the kernarg pointer stay with the kext's dispatch (rdna4_dispatch).
- (BOOL)canDispatchOnRing:(const rdna4_program_t *)k
{
	static const BOOL on = getenv("RDNA4_RINGDISPATCH") && atoi(getenv("RDNA4_RINGDISPATCH")) == 1;
	const uint32_t user = (k->rsrc2 >> 1) & 0x1f;
	return on && self.gfx && !(k->rsrc2 & 1) && user == ((k->properties >> 3 & 1) ? 2u : 0u) && !(k->properties & 1u << 1);
}

// As the kext's launch (src/compute.cpp), in a slot of the gfx ring: the draws before it waited for on the card (idle barrier), the
// caches invalidated (the draw stream's opening ACQUIRE_MEM), the program, its resources, the kernarg pointer in USER_DATA_0/1 (the
// kernargs in the slot's state page, past its stream fence word), DISPATCH_DIRECT as a compute packet (SHADER_TYPE 1), the idle barrier
// again for what reads its results. Register offsets: GC seg0 (src/gfxregs.hpp) + 0x1260 - SET_SH_REG's 0x2c00.
- (NSError *)dispatchOnRing:(const rdna4_program_t *)k groups:(const uint32_t[3])groups size:(const uint32_t[3])size args:(NSData *)args
{
	const uint64_t kernargBytes = MAX((uint64_t)args.length, k->kernargBytes);
	if (kernargBytes > kMiscBytes - 0x100)
		return rdna4Error([NSString stringWithFormat:@"a dispatch's %llu bytes of kernargs", kernargBytes], 0);
	uint32_t slot = 0;
	NSError *taken = [self takeSlot:&slot];
	if (taken)
		return taken;
	uint8_t *kernarg = _miscCpu + (uint64_t)slot * kMiscBytes + 0x100;
	memset(kernarg, 0, kernargBytes);
	memcpy(kernarg, args.bytes, args.length);
	const uint64_t kernargGpu = _misc.gpu + (uint64_t)slot * kMiscBytes + 0x100;
	const uint32_t lds = ((uint32_t)k->ldsBytes + 1023) / 1024 * 1024 / 512;   // ldsSizeField: 1 KiB granules in 512-byte units
	uint32_t ib[96], n = [self idleBarrier:ib];
	const uint32_t acquire[8] = { 0xc0065800u, 0, 0xffffffffu, 0x00ffffffu, 0, 0, 0x0000000au, 0x0000c3b1u };
	memcpy(ib + n, acquire, sizeof acquire), n += 8;
	auto sh = [&](uint32_t reg, std::initializer_list<uint32_t> v) {
		ib[n++] = 0xc0007600u | (uint32_t)v.size() << 16;
		ib[n++] = reg;
		for (uint32_t x : v)
			ib[n++] = x;
	};
	sh(0x20c, { (uint32_t)(k->gpu >> 8), (uint32_t)(k->gpu >> 40) });                               // COMPUTE_PGM_LO, HI
	sh(0x212, { (uint32_t)k->rsrc1, ((uint32_t)k->rsrc2 & ~(0x1ffu << 15)) | lds << 15 });             // RSRC1, RSRC2 (LDS_SIZE)
	sh(0x228, { (uint32_t)k->rsrc3 });                                                                // RSRC3
	sh(0x215, { 0 });                                                                                 // COMPUTE_RESOURCE_LIMITS
	sh(0x218, { 0 });                                                                                 // COMPUTE_TMPRING_SIZE
	sh(0x216, { 0xffffffffu, 0xffffffffu });                                                         // STATIC_THREAD_MGMT_SE0, SE1
	sh(0x219, { 0xffffffffu, 0xffffffffu });                                                         // SE2, SE3
	sh(0x22b, { 0, 0, 0, 0 });                                                                        // SE4..SE7: none on Navi 48
	sh(0x204, { 0, 0, 0 });                                                                           // COMPUTE_START_X, Y, Z
	sh(0x207, { size[0], size[1], size[2] });                                                         // COMPUTE_NUM_THREAD_X, Y, Z
	if ((k->rsrc2 >> 1) & 0x1f)
		sh(0x240, { (uint32_t)kernargGpu, (uint32_t)(kernargGpu >> 32) });                           // COMPUTE_USER_DATA_0, 1
	ib[n++] = 0xc0031502u;   // DISPATCH_DIRECT, SHADER_TYPE compute
	ib[n++] = groups[0], ib[n++] = groups[1], ib[n++] = groups[2];
	ib[n++] = 1u | 1u << 2 | (k->properties & 1u << 10 ? 1u << 15 : 0u);   // COMPUTE_SHADER_EN, FORCE_START_AT_000, CS_W32_EN
	n += [self idleBarrier:ib + n];
	memcpy(_ibCpu + (uint64_t)slot * RDNA4_TRIRUN_PAGE, ib, n * 4);
	__sync_synchronize();
	[self queueSlot:slot dwords:n];   // (_lastSlot stays the last draw's: its stream fence is what drainGfx checks)
	_slotPipe[slot] = nil;   // (no stream fence of its own: the batch's fence is what drainGfx waits for)
	return nil;
}

- (NSError *)drainGfx
{
	NSError *flushed = [self flushBatch];
	if (flushed)
		return flushed;
	if (!_lastFence)
		return nil;
	[_drawnInFlight removeAllObjects];
	const uint64_t fence = _lastFence;
	const BOOL drew = _drawsInFlight;
	_drawsInFlight = NO;
	uint32_t streamFence = 0;
	uint64_t fences[kSlots];
	memcpy(fences, _slotFence, sizeof fences);
	_lastFence = 0;
	memset(_slotFence, 0, sizeof _slotFence);
	const uint64_t t0 = nowUs();
	kern_return_t kr = rdna4_wait_gfx_fence(&_gpu, fence, 2000, NULL);
	statAdd(kStatDrainUs, nowUs() - t0);
	if (!kr)
		streamFence = *(volatile uint32_t *)(_miscCpu + (uint64_t)_lastSlot * kMiscBytes);
	if (kr) {
		NSMutableArray *inFlight = [NSMutableArray array];
		for (uint32_t i = 0; i < kSlots; i++)
			if (fences[i] && _slotPipe[i])
				[inFlight addObject:[NSString stringWithFormat:@"%llu %@ (%u vertices x %u, %ux%u format %u)", fences[i], _slotPipe[i].names,
				                                               _slotWhat[i][0], _slotWhat[i][1], _slotWhat[i][2], _slotWhat[i][3], _slotWhat[i][4]]];
		return rdna4Error([NSString stringWithFormat:@"waiting for the draws (fence %llu; in flight: %@)", fence,
		                                             [inFlight componentsJoinedByString:@"; "]], kr);
	}
	if (drew && streamFence != 1)
		return rdna4Error([NSString stringWithFormat:@"the draws finished but the last stream's fence reads %u", streamFence], 0);
	return nil;
}

// One draw, submitted (not waited for: drainGfx): the PV host's runDraw for a single-sampled target of a colourInfo format, blended or not.
- (NSError *)draw:(const RDNA4Draw *)d pipeline:(RDNA4MTLRenderPipeline *)p target:(RDNA4MTLTexture *)t
{
	if (d->type > 4 || !d->count || !d->instances || d->count > (1u << 24))
		return rdna4Error([NSString stringWithFormat:@"a draw of %u vertices, type %u, %u instances: not run", d->count, d->type, d->instances], 0);
	if (!d->depthOnly && [p formatOf:d->attachment] != t.pixelFormat)
		return rdna4Error([NSString stringWithFormat:@"the pipeline's colour attachment %u format %lu is not the target's %lu", d->attachment,
		                                             (unsigned long)[p formatOf:d->attachment], (unsigned long)t.pixelFormat], 0);
	static uint32_t ib[RDNA4_TRIRUN_PAGE / 4];   // built here and copied into the slot's pages: draws are made on the work queue only
	static uint64_t misc[kMiscBytes / 8];
	uint32_t slot = 0;
	NSError *taken = [self takeSlot:&slot];
	if (taken)
		return taken;
	// A slot taken and left out (a return below before queueSlot) is queued as NOPs: a batch is its slots' pages one after the other, and
	// the card ran the page left in the run (zeros) and hung the gfx ring for good (a depth-only pass's empty scissor, 2026-11-22).
	struct Pad {
		__unsafe_unretained RDNA4MTLDevice *device;
		uint32_t slot;
		bool queued;
		~Pad() { if (!queued) [device queueSlot:slot dwords:0]; }
	} pad = { self, slot, false };
	uint64_t t0 = nowUs();
	kern_return_t kr = KERN_SUCCESS;
	const uint64_t miscGpu = _misc.gpu + (uint64_t)slot * kMiscBytes, ibOffset = (uint64_t)slot * RDNA4_TRIRUN_PAGE;
	const uint32_t *vs = p.vs, *ps = p.ps, outs = p.outs, ins = p.ins;
	const BOOL noColour = (ps[7] >> 31) || p.exportsNothing || d->depthOnly;
	// dual-source blending (the fragment header's bit 18: the colour and its [[color(0), index(1)]]; Sublime Text's subpixel text through
	// Apple's GL-on-Metal renderer), as the PV host: both exports in the colour's format and in CB_SHADER_MASK, DISABLE_DUAL_QUAD, no SX
	// blend optimisation
	const BOOL dualSource = (ps[7] >> 18 & 1) && !noColour;
	const BOOL depthOut = ps[7] >> 19 & 1;   // the fragment shader exports its depth (MRTZ)
	const uint32_t dbShader = noColour ? 0x600u : 0x10u;
	// Blending as the PV host's runDraw: the colour pass and, when alpha's equation differs and both are written, an alpha-only pass after it
	// (the card ignores SEPARATE_ALPHA_BLEND). ponytail: a colour factor reading destination alpha sees the alpha from before the draw.
	const uint32_t *blend = [p blendOf:d->attachment];
	const uint32_t mask = (blend[9] >> 3 & 1) | (blend[9] >> 1 & 2) | (blend[9] << 1 & 4) | (blend[9] << 3 & 8);   // Metal's A, B, G, R bits
	const BlendPass rgbPass(blend[3], blend[4], blend[5]), alphaPass(blend[6], blend[7], blend[8]);
	const bool split = blend[2] && rgbPass.control != alphaPass.control && (mask & 7) && (mask & 8);
	const BlendPass &first = mask & 7 ? rgbPass : alphaPass;
	const uint32_t sx = d->scissor[0], sy = d->scissor[1], sx2 = MIN(sx + d->scissor[2], (uint32_t)t.width), sy2 = MIN(sy + d->scissor[3], (uint32_t)t.height);
	if (sx >= sx2 || sy >= sy2)
		return nil;   // nothing inside the scissor rectangle
	const uint64_t rings = self.gfxRings;
	const rdna4_tri_va where = { p.code, t.gpuBase, rings, miscGpu };
	const rdna4_tricol_va whereCol = { p.code, t.gpuBase, rings, miscGpu };
	const uint32_t n = outs ? rdna4_tricol_record(ib, &whereCol) : rdna4_tri_record(ib, &where);
	if (!n)
		return rdna4Error(@"the stream builder refused the draw's addresses", 0);
	const uint32_t width = (uint32_t)t.width, height = (uint32_t)t.height, pitch = (uint32_t)(t.rowBytes / t.bpp);
	const float *v = d->viewport;
	// Metal's point, line, line strip, triangle, triangle strip: DI_PT_* and the NGG stage's output primitive
	static const uint32_t kPrimitive[5][2] = { { 1, 0 }, { 2, 1 }, { 3, 1 }, { 4, 2 }, { 6, 2 } };
	// As the PV host's patch table. Multisampling (a 2x or 4x target; else one sample): CB_COLOR0_ATTRIB NUM_FRAGMENTS, ATTRIB3 COLOR_SW_MODE
	// 64KB_2D, PA_SC_AA_CONFIG MSAA_NUM_SAMPLES and MSAA_EXPOSED_SAMPLES, PA_SC_MODE_CNTL_0 MSAA_ENABLE, DB_EQAA, the centroid order and the
	// sample positions of a quad's four pixels (radeonsi's si_state_msaa.c), PA_SC_SAMPLE_PROPERTIES.MAX_SAMPLE_DIST (RADV's).
	static const uint32_t kLocs[3] = { 0, 0x0000cc44u, 0x622ae6aeu }, kCentroid[3] = { 0, 0x10101010u, 0x32103210u }, kMaxDist[3] = { 0, 4, 6 };
	const uint32_t logSamples = t.samples == 4 ? 2 : t.samples == 2 ? 1 : 0;
	const struct { uint32_t opcode, reg, value; } patch[] = {
		{ 0x76, 0x8a, vs[3] }, { 0x76, 0x8b, vs[4] },                     // SPI_SHADER_PGM_RSRC1_GS, RSRC2_GS
		{ 0x76, 0x0a, ps[3] }, { 0x76, 0x0b, ps[4] },                     // SPI_SHADER_PGM_RSRC1_PS, RSRC2_PS
		{ 0x69, 0x197, ps[5] }, { 0x69, 0x198, ps[6] },                   // SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR
		{ 0x69, 0x31e, (pitch - 1) << 16 | (height - 1) },               // CB_COLOR0_ATTRIB2: MIP0_WIDTH (the pitch), MIP0_HEIGHT
		{ 0x69, 0x3b0, colourInfo((uint32_t)t.pixelFormat, isBgra(t.pixelFormat)) },   // CB_COLOR0_INFO
		{ 0x69, 0x82, (width - 1) | (height - 1) << 16 },                // PA_SC_WINDOW_SCISSOR_BR (inclusive)
		{ 0x69, 0x94, sx | sy << 16 }, { 0x69, 0x95, (sx2 - 1) | (sy2 - 1) << 16 },   // PA_SC_VPORT_SCISSOR_0_TL, _BR: the scissor, BR inclusive
		{ 0x69, 0x10f, floatBits(v[2] / 2) }, { 0x69, 0x110, floatBits(v[0] + v[2] / 2) },    // PA_CL_VPORT_XSCALE, XOFFSET
		{ 0x69, 0x111, floatBits(-v[3] / 2) }, { 0x69, 0x112, floatBits(v[1] + v[3] / 2) },   // YSCALE (Metal's y up), YOFFSET
		{ 0x79, 0x242, kPrimitive[d->type][0] }, { 0x79, 0x266, kPrimitive[d->type][1] },     // VGT_PRIMITIVE_TYPE, VGT_GS_OUT_PRIM_TYPE
		{ 0x76, 0x31, outs ? (outs - 1) | ins << 11 : 0x400u },          // SPI_SHADER_GS_OUT_CONFIG_PS
		{ 0x69, 0x207, 0x00000244u },                                    // PA_SU_SC_MODE_CNTL: provoking vertex first, clockwise front
		{ 0x69, 0x1e0, blend[2] ? first.control : 0 },                   // CB_BLEND0_CONTROL
		{ 0x69, 0x1d8, dualSource ? 0u : blend[2] ? first.opt : 0x06000600u },   // SX_MRT0_BLEND_OPT: else BLEND_DISABLED; NONE for dual source
		{ 0x69, 0x216, dualSource ? 0x00cc0011u : 0x00cc0010u },         // CB_COLOR_CONTROL: ROP3 copy, MODE normal; DISABLE_DUAL_QUAD
		{ 0x69, 0x214, noColour ? 0u : split ? mask & 7 : mask },         // CB_TARGET_MASK
		{ 0x69, 0x195, noColour ? 0u : dualSource ? 0x99u : 9u },        // SPI_SHADER_COL_FORMAT: 32_ABGR (both sources)
		{ 0x69, 0x215, dualSource ? 0xffu : 0xfu },                      // CB_SHADER_MASK: the exports' channels
		// DB_SHADER_CONTROL; with the shader's depth Z_EXPORT_ENABLE and LATE_Z (Z_ORDER 0); SPI_SHADER_Z_FORMAT 32_R for it, else ZERO
		{ 0x69, 0x1b, depthOut ? (dbShader & ~0x30u) | 1u : dbShader },
		{ 0x69, 0x194, depthOut ? 1u : 0u },
		{ 0x69, 0x31b, logSamples }, { 0x69, 0x31f, 0x01000000u | (logSamples ? 3u << 15 : 0u) }, { 0x69, 0x2f8, logSamples | logSamples << 20 },
		{ 0x69, 0x292, 0x22u | (logSamples ? 1u : 0u) }, { 0x69, 0x1e, 0x00110000u | logSamples << 8 | logSamples << 12 },
		{ 0x69, 0x2fc, kCentroid[logSamples] }, { 0x69, 0x2fd, kCentroid[logSamples] }, { 0x69, 0x2fe, kLocs[logSamples] },
		{ 0x69, 0x302, kLocs[logSamples] }, { 0x69, 0x306, kLocs[logSamples] }, { 0x69, 0x30a, kLocs[logSamples] }, { 0x69, 0x317, kMaxDist[logSamples] },
	};
	for (size_t i = 0; i < sizeof patch / sizeof patch[0]; i++) {
		uint32_t *at = streamReg(ib, n, patch[i].opcode, patch[i].reg);
		if (!at)
			return rdna4Error([NSString stringWithFormat:@"the stream lacks register 0x%x", patch[i].reg], 0);
		*at = patch[i].value;
	}
	// The stages' tables in the state page (unbound buffers: a zeroed spare, as the PV host), the fence word at +0 zeroed.
	const BOOL levels = (vs[7] | ps[7]) >> 16 & 1;   // a stage samples at explicit LODs: levels 1-7 of each texture (one level: level 0's T#)
	// the whole page for those and for textures past 15 (+0x4200) or texture handles (+0x4c00), else the tables
	const BOOL highTextures = (([p texturesOf:0] | [p texturesOf:1]) >> 16) != 0;
	const size_t miscBytes = levels || highTextures ? kMiscBytes : kMiscTables;
	memset(misc, 0, miscBytes);
	for (unsigned st = 0; st < 2; st++)
		for (unsigned k = 0; k < kTableBuffers; k++) {
			// 28-30 past both tables, as the PV host's (MoltenVK binds vertex buffers from 30 down: left at 0, vkQuake's world draws
			// read address 0 and hung the gfx ring, 2026-11-21)
			const uint32_t at = k >= 28 ? RDNA4_MTLC_HIGH_BUFFERS + 0x20 * st + 8 * (k - 28)
			                            : 0x400 + 0x400 * st + (k < 8 ? 8 * k : RDNA4_MTLC_TABLE_MORE_BUFFERS + 8 * (k - 8));
			misc[at / 8] = d->table[st][k] ?: _spare.gpu;
		}
	memcpy((uint8_t *)misc + 0x400 + RDNA4_MTLC_TABLE_ATTRIBUTES, p.attributes, 16 * 16);   // the vertex descriptor
	((uint32_t *)misc)[(0x800 + RDNA4_MTLC_TABLE_ATTACHMENT) / 4] = d->attachment;
	// the target as an image, for shaders that read it (framebuffer fetch: RDNA4_MTLC_TABLE_FRAMEBUFFER). ponytail: a pixel the same draw
	// covers twice reads its first write, not the value from before the draw (the PV host's reads a staged copy).
	[t descriptor:(uint32_t *)((uint8_t *)misc + 0x800 + RDNA4_MTLC_TABLE_FRAMEBUFFER)];
	uint32_t spare[8];
	[self spareImage:spare];
	static const uint32_t none[8] = {};
	static_assert(0x800 + RDNA4_MTLC_TEXTURE_AT(kTextures) <= kMiscBytes, "textures 16-30 fit the state page");
	static_assert(0x800 + RDNA4_MTLC_TABLE_TEXTURE_HANDLES + 8 * kMaxTextures <= kMiscBytes, "the texture handles fit the state page");
	static_assert(0x400 + RDNA4_MTLC_TABLE_TEXTURE_HANDLES >= 0x800 + RDNA4_MTLC_TEXTURE_AT(kTextures) &&
	              0x400 + RDNA4_MTLC_TABLE_TEXTURE_HANDLES + 8 * kMaxTextures <= 0x800 + RDNA4_MTLC_TABLE_TEXTURE_HANDLES,
	              "the vertex stage's handles between the fragment stage's textures 16-30 and its handles");
	for (unsigned st = 0; st < 2; st++) {   // the T#s at RDNA4_MTLC_TEXTURE_AT(n), the S#s at +576 + 16 n of the stage's table (include/rdna4mtlc.h)
		uint8_t *table = (uint8_t *)misc + 0x400 + 0x400 * st;
		for (unsigned k = 0; k < kTextures; k++)   // unbound: the spare texel (here and at every level)
			memcpy(table + RDNA4_MTLC_TEXTURE_AT(k), memcmp(d->textures[st][k], none, 32) ? d->textures[st][k] : spare, 32);
		for (unsigned k = 0; k < kStageTextures; k++)
			memcpy(table + RDNA4_MTLC_TABLE_SAMPLERS + 16 * k, d->samplers[st][k], 16);
		uint64_t *handle = (uint64_t *)(table + RDNA4_MTLC_TABLE_TEXTURE_HANDLES);   // 0-30: their T#s just written, 31-127: recorded
		for (unsigned k = 0; d->handles && k < kMaxTextures; k++)
			handle[k] = k < kTextures ? miscGpu + 0x400 + 0x400 * st + RDNA4_MTLC_TEXTURE_AT(k) : d->handles[st * kHandleTextures + (k - kTextures)];
	}
	// The draw in place of the template's DRAW_INDEX_AUTO: blend colour, the fragment inputs, the stages' tables (USER_DATA_GS_0/1, _PS_0/1),
	// the first vertex and base instance (USER_DATA_GS_2/3), NUM_INSTANCES, DRAW_INDEX_AUTO.
	uint32_t seq[256], m = 0;
#define PUT(x) (seq[m++] = (x))
	if (d->depthGpu || d->stencilGpu) {   // the depth and stencil tests, as the PV host's (src/pvgpu.cpp runDraw)
		PUT(0xc0026900u), PUT(0x001), PUT(0), PUT(0);   // DB_DEPTH_VIEW, DB_DEPTH_VIEW1: slice 0, mip 0
		PUT(0xc00b6900u), PUT(0x005);                   // DB_DEPTH_SIZE_XY, DB_Z_INFO, DB_STENCIL_INFO, Z read/write and stencil bases
		PUT((d->depthWidth - 1) | (d->depthHeight - 1) << 16);
		PUT((d->depthGpu ? 3 | 3u << 4 : 0) | logSamples << 2);   // Z_32_FLOAT, SW_64KB_2D (else Z_INVALID), NUM_SAMPLES as the colour's
		PUT(d->stencilGpu ? 1 | 3u << 4 | 1u << 29 : 0);         // STENCIL_8, SW_64KB_2D, TILE_STENCIL_DISABLE (else STENCIL_INVALID)
		const uint64_t bases[4] = { d->depthGpu, d->depthGpu, d->stencilGpu, d->stencilGpu };
		for (int i = 0; i < 4; i++)
			PUT((uint32_t)(bases[i] >> 8)), PUT((uint32_t)(bases[i] >> 40));
		PUT(0xc0026900u), PUT(0x01c);   // DB_DEPTH_CONTROL: the stencil's part, Z_ENABLE, Z_WRITE_ENABLE, ZFUNC; DB_STENCIL_CONTROL
		PUT((d->stencilGpu ? d->stencilDepthControl : 0) | (d->depthGpu ? 2u | (d->depthWrite ? 4u : 0u) | d->depthCompare << 4 : 0));
		PUT(d->stencilGpu ? d->stencilControl : 0);
		if (d->stencilGpu)   // DB_STENCIL_REF, DB_STENCIL_OPVAL (1: the increments' step), DB_STENCIL_READ_MASK, DB_STENCIL_WRITE_MASK
			PUT(0xc0046900u), PUT(0x022), PUT(d->stencilRef), PUT(0x101), PUT(d->stencilReadMask), PUT(d->stencilWriteMask);
	}
	if (d->depthGpu) {   // Metal's 0..1 depth range
		PUT(0xc0026900u), PUT(0x113), PUT(0x3f800000u), PUT(0);   // PA_CL_VPORT_ZSCALE 1, ZOFFSET 0: z = ndc z (Metal's 0..1)
		PUT(0xc0026900u), PUT(0x115), PUT(0), PUT(0x3f800000u);   // PA_SC_VPORT_ZMIN_0, ZMAX_0
		PUT(0xc0016900u), PUT(0x204), PUT(1u << 24 | 1u << 19);   // PA_CL_CLIP_CNTL: DX_LINEAR_ATTR_CLIP_ENA, DX_CLIP_SPACE_DEF
	}
	PUT(0xc0046900u), PUT(0x105);   // CB_BLEND_RED .. ALPHA
	for (int i = 0; i < 4; i++)
		PUT(floatBits(d->blendColour[i]));
	if (ins) {
		PUT(0xc0006900u | ins << 16), PUT(0x199);
		for (uint32_t k = 0; k < ins; k++)
			PUT(p.inputCntl[k]);
	}
	PUT(0xc0016900u), PUT(0x191), PUT(p.sprite ? 0x86bu : 0x869u);   // SPI_INTERP_CONTROL_0
	for (unsigned st = 0; st < 2; st++) {
		const uint64_t table = miscGpu + 0x400 + 0x400 * st;
		PUT(0xc0027600u), PUT(st ? 0x0c : 0x8c), PUT((uint32_t)table), PUT((uint32_t)(table >> 32));
	}
	if (d->indexGpu) {   // VGT_INDEX_TYPE (SET_UCONFIG_REG_INDEX, as RADV)
		PUT(0xc0017a00u), PUT(0x243 | 2u << 28), PUT(d->indexBytes == 4 ? 1 : 0);
		if (d->type == 2 || d->type == 4) {   // Metal's primitive restart for indexed strips (the PV host's): GE_MULTI_PRIM_IB_RESET_EN, the index
			PUT(0xc0017900u), PUT(0x24b), PUT(5);
			PUT(0xc0016900u), PUT(0x103), PUT(d->indexBytes == 4 ? 0xffffffffu : 0xffffu);
		}
	}
	for (uint32_t pass = 0; pass < (split ? 2u : 1u); pass++) {
		if (pass) {   // the alpha pass
			const uint32_t regs[3][2] = { { 0x1e0, alphaPass.control }, { 0x1d8, alphaPass.opt }, { 0x214, mask & 8 } };
			for (int r = 0; r < 3; r++)
				PUT(0xc0016900u), PUT(regs[r][0]), PUT(regs[r][1]);
		}
		PUT(0xc0027600u), PUT(0x8e), PUT(d->start), PUT(d->baseInstance);
		PUT(0xc0002f00u), PUT(d->instances);
		if (d->indexGpu)   // DRAW_INDEX_2 {max indices, address, count, initiator}
			PUT(0xc0042700u), PUT(d->count), PUT((uint32_t)d->indexGpu), PUT((uint32_t)(d->indexGpu >> 32)), PUT(d->count), PUT(0);
		else
			PUT(0xc0012d00u), PUT(d->count), PUT(2);
	}
#undef PUT
	uint32_t total = 0;
	for (uint32_t i = 0; i + 3 <= n; i++)
		if (ib[i] == 0xc0012d00u && n - 3 + m <= sizeof ib / 4) {
			memmove(&ib[i + m], &ib[i + 3], (n - i - 3) * 4);
			memcpy(&ib[i], seq, m * 4);
			total = n - 3 + m;
			break;
		}
	if (!total)
		return rdna4Error(@"the stream has no draw to replace, or the draw does not fit its page", 0);
	// USER_SGPR: 4 for the vertex stage (and ES_VGPR_COMP_CNT 1: InstanceID in v4), 2 for the fragment stage (air2amdgpu.cpp)
	static const uint32_t rsrc2[2][3] = { { 0x8b, 4, 1u << 16 }, { 0x0b, 2, 0 } };
	for (int i = 0; i < 2; i++) {
		uint32_t *at = streamReg(ib, total, 0x76, rsrc2[i][0]);
		if (!at)
			return rdna4Error(@"the stream lacks a shader's RSRC2", 0);
		*at = (*at & ~0x3003eu) | rsrc2[i][1] << 1 | rsrc2[i][2];
	}
	for (unsigned st = 0; levels && st < 2; st++)   // each level's own image (a texture of fewer levels: its last), unbound ones the spare texel
		for (uint32_t level = 1; level < RDNA4_MTLC_LEVELS; level++) {
			uint8_t *at = (uint8_t *)misc + kMiscTables + RDNA4_MTLC_LEVEL_STRIDE * (level - 1) + 0x400 * st;
			memcpy(at, d->levelT ? (const void *)(d->levelT + (st * 7 + (level - 1)) * 16 * 32) : (const void *)d->textures[st], 32 * kStageTextures);
			for (unsigned k = 0; k < kStageTextures; k++)
				if (!memcmp(at + 32 * k, none, 32))
					memcpy(at + 32 * k, spare, 32);
		}
	statAdd(kStatBuildUs, nowUs() - t0);
	t0 = nowUs();
	memcpy(_miscCpu + (uint64_t)slot * kMiscBytes, misc, miscBytes);
	memcpy(_ibCpu + ibOffset, ib, total * 4);
	__sync_synchronize();   // in memory before the card reads them
	statAdd(kStatWriteUs, nowUs() - t0);
	if (kr)
		return rdna4Error([NSString stringWithFormat:@"a draw of %u vertices (%u dwords)", d->count, total], kr);
	[self queueSlot:slot dwords:total];
	pad.queued = true;
	_lastSlot = slot;
	_drawsInFlight = YES;
	const uint32_t cx = d->covered[0], cy = d->covered[1], cx2 = MIN(cx + d->covered[2], (uint32_t)t.width), cy2 = MIN(cy + d->covered[3], (uint32_t)t.height);
	if (cx < cx2 && cy < cy2)
		statAdd(t.onSurface ? kStatDrawnSurface : kStatDrawnOwn, (uint64_t)(cx2 - cx) * (cy2 - cy) * t.bpp);
	[t heldRows:cy to:cy2 width:cx2];
	if (cx < cx2 && cy < cy2)
		wrote(t.identity + cy * t.rowBytes, t.identity + (uint64_t)(cy2 - 1) * t.rowBytes + cx2 * t.bpp);
	if (!_drawnInFlight)
		_drawnInFlight = [NSHashTable weakObjectsHashTable];
	[_drawnInFlight addObject:t.root];
	_slotPipe[slot] = p;
	const uint32_t what[5] = { d->count, d->instances, (uint32_t)t.width, (uint32_t)t.height, (uint32_t)t.pixelFormat };
	memcpy(_slotWhat[slot], what, sizeof what);
	return nil;
}

- (void)_setAcceleratorService:(id)service
{
	_accelService = service;
}

- (void)_removeRequested
{
}

// What MTLCopyDeviceForRegistryID hands out (Metal's _MTLIOAccelCopyDeviceForRegistryID: [[service weakDevice] _deviceWrapper]; CoreDisplay
// finds the display's device so): the outermost object of the device, a validation layer's when there is one, else the device itself.
- (id)_deviceWrapper { return self; }
// SkyLight's CompositorMetal::new_compositor takes the device whose acceleratorPort is the display's accelerator (else no compositor:
// "Unable to composite display due to null compositor", a black desktop)
- (io_service_t)acceleratorPort { return _service; }
- (BOOL)isLowPower { return NO; }
- (BOOL)isRemovable { return NO; }
- (BOOL)isHeadless { return NO; }
- (MTLDeviceLocation)location { return MTLDeviceLocationSlot; }   // a PCIe card (System Settings' Displays pane asks)
- (NSUInteger)locationNumber { return 0; }
- (uint64_t)peerGroupID { return 0; }   // not in a peer group (MoltenVK asks)
- (uint32_t)peerIndex { return 0; }
- (uint32_t)peerCount { return 0; }

- (NSString *)name { return @"AMD Radeon RDNA4 (native Metal, spike)"; }
// Apple's GL-on-Metal renderer takes its renderer string and its per-vendor limits from this ("AMD" or "ATI" for AMD's)
- (NSString *)vendorName { return @"AMD"; }
- (uint64_t)registryID { return _registryID; }
- (BOOL)hasUnifiedMemory { return NO; }
// (0 by the catch-all before: Core Image's icon kernels asked) 256 bytes, the sampler's linear and buffer texture base alignment
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format { return 256; }
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format { return 256; }
- (NSUInteger)maxBufferLength { return 64ull << 20; }   // ponytail: the kext's host-memory BO limit; VRAM buffers when a gate needs more
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 1024); }

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	return [[RDNA4MTLBuffer alloc] initWithDevice:self length:length options:options];
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options
{
	RDNA4MTLBuffer *b = [[RDNA4MTLBuffer alloc] initWithDevice:self length:length options:options];
	if (b && length)
		memcpy(b.contents, pointer, length);
	return b;
}

- (BOOL)supportsRaytracing { return YES; }
- (BOOL)supportsTextureSwizzle { return YES; }   // (views with a swizzle: newTextureViewWithPixelFormat:...swizzle:)
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count { return count == 1; }
// 1, 2 and 4 samples (2x/4x MSAA textures and resolves). Apple's GL-on-Metal renderer asks the private supportsSampleCount: (answered NO
// as a missing selector, it made no context: "invalid memory allocation", 2026-11-21).
- (BOOL)supportsTextureSampleCount:(NSUInteger)count { return count == 1 || count == 2 || count == 4; }
- (BOOL)supportsSampleCount:(NSUInteger)count { return [self supportsTextureSampleCount:count]; }

// A texture over the caller's memory (Core Animation's tiled textures), as a buffer of it and a linear texture over that.
- (id<MTLTexture>)newTiledTextureWithBytesNoCopy:(void *)bytes length:(NSUInteger)length descriptor:(MTLTextureDescriptor *)d
                                          offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bpr
{
	RDNA4MTLBuffer *b = (RDNA4MTLBuffer *)[self newBufferWithBytesNoCopy:bytes length:length options:MTLResourceStorageModeShared deallocator:nil];
	return b ? [[RDNA4MTLTexture alloc] initWithBuffer:b descriptor:d offset:offset bytesPerRow:bpr] : nil;
}

// What Apple's paravirtual device answers (mtlprobe features, Tahoe 26.6.2, 2026-11-20): _MTLDevice's own defaults describe an Apple GPU
// (Apple family 1, programmable blending, memoryless targets, ...), and Core Animation chose techniques of those for our device that
// the paravirtual one never sees (an empty mask shader hung WindowServer). The same draws and the same translator serve both, so the same
// answers; ray tracing and the buffer size limit are our own.
// Tier 2 and Metal3: MoltenVK then writes its descriptor sets' argument buffers itself (gpuAddress, gpuResourceID), no encoder (which we lack)
- (MTLArgumentBuffersTier)argumentBuffersSupport { return MTLArgumentBuffersTier2; }
// Mac2 as well, as every Metal 2 Mac GPU answers: ANGLE (Chrome's and WebKit's WebGL) has no Metal renderer without it
- (BOOL)supportsFamily:(MTLGPUFamily)family { return family == 2001 || family == 2002 || family == 3001 || family == 3002 || family == MTLGPUFamilyMetal3; }   // Mac1, Mac2, Common1, Common2
- (uint64_t)dedicatedMemorySize { return 1073741824ull; }
- (uint64_t)deviceLinearReadOnlyTextureAlignmentBytes { return 256ull; }
- (uint64_t)deviceLinearTextureAlignmentBytes { return 256ull; }
- (uint64_t)doubleFPConfig { return 0ull; }
- (uint64_t)featureProfile { return 10000ull; }
- (uint64_t)halfFPConfig { return 0ull; }
- (uint64_t)iosurfaceReadOnlyTextureAlignmentBytes { return 16ull; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }
- (BOOL)isFixedLinePointFillDepthGradientSupported { return YES; }
- (BOOL)isPlacementHeapSupported { return NO; }
- (uint64_t)linearTextureAlignmentBytes { return 256ull; }
- (uint64_t)llvmVersion { return 32023ull; }
- (NSUInteger)maxArgumentBufferSamplerCount { return 1024ull; }
- (uint64_t)maxColorAttachments { return 8ull; }
- (uint64_t)maxComputeTextures { return 128ull; }
- (uint64_t)maxComputeThreadgroupMemory { return 32768ull; }
- (uint64_t)maxCustomSamplePositions { return 8ull; }
- (uint64_t)maxFragmentTextures { return 128ull; }
- (uint64_t)maxFramebufferStorageBits { return 1024ull; }
- (uint64_t)maxIOCommandsInFlight { return 8ull; }
- (uint64_t)maxIndirectBuffers { return 64ull; }
- (uint64_t)maxIndirectSamplersPerDevice { return 1024ull; }
- (uint64_t)maxIndirectTextures { return 128ull; }
- (uint64_t)maxInterpolants { return 32ull; }
- (uint64_t)maxInterpolatedComponents { return 124ull; }
- (uint64_t)maxTessellationFactor { return 64ull; }
- (uint64_t)maxTextureBufferWidth { return 134217728ull; }
- (uint64_t)maxTextureDimensionCube { return 16384ull; }
- (uint64_t)maxTextureHeight2D { return 16384ull; }
- (uint64_t)maxTextureWidth1D { return 16384ull; }
- (uint64_t)maxTextureWidth2D { return 16384ull; }
- (uint64_t)maxTotalComputeThreadsPerThreadgroup { return 1024ull; }
- (uint64_t)maxTransferRate { return 63015384608ull; }
- (uint64_t)maxVertexTextures { return 128ull; }
- (uint64_t)maxViewportCount { return 16ull; }
- (uint64_t)maxVisibilityQueryOffset { return 262136ull; }
- (uint64_t)metalAssertionsEnabled { return 0ull; }
- (uint64_t)minConstantBufferAlignmentBytes { return 256ull; }
- (MTLReadWriteTextureTier)readWriteTextureSupport { return MTLReadWriteTextureTier1; }
- (uint64_t)sharedMemorySize { return 5726625792ull; }
- (uint64_t)singleFPConfig { return 0ull; }
- (uint64_t)supportPriorityBand { return 0ull; }
- (BOOL)supportsArrayOfSamplers { return YES; }
- (BOOL)supportsArrayOfTextures { return YES; }
- (BOOL)supportsBaseVertexInstanceDrawing { return YES; }
// Attributes at any byte offset and stride: the translator reads each component from its dword (SH_MEM_CONFIG allows unaligned loads).
// Without it Apple's GL-on-Metal renderer refuses kitty's cell arrays and draws each of its ~3000 instances alone (2026-11-21).
- (BOOL)supportsUnalignedVertexFetch { return YES; }
- (BOOL)supportsBlackOrWhiteSamplerBorderColors { return YES; }
- (BOOL)supportsBufferPrefetchStatistics { return NO; }
- (BOOL)supportsBufferWithIOSurface { return NO; }
- (BOOL)supportsBufferlessClientStorageTexture { return YES; }
- (BOOL)supportsCombinedMSAAStoreAndResolveAction { return YES; }
- (BOOL)supportsCompressedTextureViewSPI { return NO; }
- (BOOL)supportsCountingOcclusionQuery { return YES; }
- (BOOL)supportsDeadlineProfile { return NO; }
- (BOOL)supportsDepthClipMode { return YES; }
- (BOOL)supportsDynamicControlPointCount { return NO; }
- (BOOL)supportsFastMathInfNaNPropagation { return NO; }
- (BOOL)supportsFillTexture { return NO; }
- (BOOL)supportsFixedLinePointFillDepthGradient { return YES; }
- (BOOL)supportsFloat16InfNanFiltering { return YES; }
- (BOOL)supportsForceSeamsOnCubemaps { return YES; }
- (BOOL)supportsFragmentBufferWrites { return YES; }
- (BOOL)supportsFragmentOnlyEncoders { return NO; }
- (BOOL)supportsIABHashForTools { return NO; }
- (BOOL)supportsIndirectDrawAndDispatch { return YES; }
- (BOOL)supportsIndirectTessellation { return YES; }
- (BOOL)supportsIndirectTextures { return YES; }
- (BOOL)supportsIndirectWritableTextures { return YES; }
- (BOOL)supportsInvariantVertexPosition { return NO; }
- (BOOL)supportsLayeredRendering { return YES; }
- (BOOL)supportsLimitedYUVFormats { return NO; }
- (BOOL)supportsMSAADepthResolve { return YES; }
- (BOOL)supportsMemorylessRenderTargets { return NO; }
- (BOOL)supportsMipLevelsSmallerThanBlockSize { return YES; }
- (BOOL)supportsMirrorClampToEdgeSamplerMode { return YES; }
- (BOOL)supportsMutableTier1ArgumentBuffers { return YES; }
- (BOOL)supportsNonPrivateDepthStencilTextures { return NO; }
- (BOOL)supportsNonPrivateMSAATextures { return NO; }
- (BOOL)supportsNonUniformThreadgroupSize { return YES; }
- (BOOL)supportsNonZeroTextureWriteLOD { return NO; }
- (BOOL)supportsPacked32TextureBufferWrites { return YES; }
- (BOOL)supportsPartialRenderMemoryBarrier { return YES; }
- (BOOL)supportsPlacementHeaps { return NO; }
- (BOOL)supportsPrimitiveRestartOverride { return NO; }
- (BOOL)supportsProgrammableBlending { return NO; }
- (BOOL)supportsRayTracingTraversalMetrics { return NO; }
- (BOOL)supportsReadWriteBufferArguments { return YES; }
- (BOOL)supportsReadWriteTextureArguments { return YES; }
- (BOOL)supportsReadWriteTextureCubeArguments { return YES; }
- (BOOL)supportsRelaxedTextureViewRequirements { return NO; }
- (BOOL)supportsRenderMemoryBarrier { return YES; }
- (BOOL)supportsRenderTargetTextureRotation { return NO; }
- (BOOL)supportsRenderTextureWrites { return YES; }
- (BOOL)supportsRenderToLinearTextures { return NO; }
- (BOOL)supportsSIMDGroup { return YES; }
- (BOOL)supportsSamplerCompareFunction { return YES; }
- (BOOL)supportsSeparateDepthStencil { return NO; }
- (BOOL)supportsShaderLODAverage { return NO; }
- (BOOL)supportsSharedStorageHeapResources { return NO; }
- (BOOL)supportsSharedStorageTextures { return NO; }
- (BOOL)supportsSharedTextureHandles { return NO; }
- (BOOL)supportsTessellation { return YES; }
- (BOOL)supportsTextureCubeArray { return YES; }
- (BOOL)supportsTextureOutOfBoundsReads { return YES; }
- (BOOL)supportsViewportAndScissorArray { return YES; }

- (uint64_t)recommendedMaxWorkingSetSize { return 1ull << 30; }
// What it has allocated (host memory: the kext's per-client limit is 3 GiB): answered 0, Core Animation kept its surfaces until WindowServer
// passed the limit and aborted (2.7 GB after a login with 11 apps, 2026-11-20); measured against the working set above, it gives them up.
- (NSUInteger)currentAllocatedSize { return (NSUInteger)MAX(gHostLive.load(), (int64_t)0); }
- (NSUInteger)maxThreadgroupMemoryLength { return 32768; }

- (MTLAccelerationStructureSizes)accelerationStructureSizesWithDescriptor:(MTLAccelerationStructureDescriptor *)d
{
	if ([d isKindOfClass:[MTLInstanceAccelerationStructureDescriptor class]])   // 'RAS2': a header and 64 bytes an instance
		return (MTLAccelerationStructureSizes){ .accelerationStructureSize = 16 + 64 * ((MTLInstanceAccelerationStructureDescriptor *)d).instanceCount,
		                                        .buildScratchBufferSize = 16 };
	NSUInteger triangles = 0;
	if ([d isKindOfClass:[MTLPrimitiveAccelerationStructureDescriptor class]])
		for (MTLAccelerationStructureGeometryDescriptor *g in ((MTLPrimitiveAccelerationStructureDescriptor *)d).geometryDescriptors)
			if ([g isKindOfClass:[MTLAccelerationStructureTriangleGeometryDescriptor class]])
				triangles += ((MTLAccelerationStructureTriangleGeometryDescriptor *)g).triangleCount;
	return (MTLAccelerationStructureSizes){ .accelerationStructureSize = (NSUInteger)rtBytes(triangles), .buildScratchBufferSize = 16 };
}

- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size
{
	return [[RDNA4MTLAccelerationStructure alloc] initWithDevice:self size:size];
}

- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d
{
	return [self newAccelerationStructureWithSize:[self accelerationStructureSizesWithDescriptor:d].accelerationStructureSize];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane
{
	RDNA4MTLTexture *t = [[RDNA4MTLTexture alloc] initWithDevice:self descriptor:descriptor iosurface:surface plane:plane];
	if (!t) {   // Core Animation then has a surface it does not track (push_surface asserts "found != slist->end ()")
		const OSType f = surface ? IOSurfaceGetPixelFormat(surface) : 0;
		RTRACE(20, @"no texture of IOSurface %u ('%c%c%c%c' %zux%zu plane %lu) as format %lu type %lu %lux%lu usage 0x%lx", surface ? IOSurfaceGetID(surface) : 0,
		       (char)(f >> 24), (char)(f >> 16), (char)(f >> 8), (char)f, surface ? IOSurfaceGetWidth(surface) : 0, surface ? IOSurfaceGetHeight(surface) : 0,
		       (unsigned long)plane, (unsigned long)descriptor.pixelFormat, (unsigned long)descriptor.textureType, (unsigned long)descriptor.width,
		       (unsigned long)descriptor.height, (unsigned long)descriptor.usage);
	}
	return t;
}

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options
                              deallocator:(void (^)(void *, NSUInteger))deallocator
{
	RDNA4MTLBuffer *b = [[RDNA4MTLBuffer alloc] initWithDevice:self bytesNoCopy:pointer length:length options:options deallocator:deallocator];
	if (b)
		return b;
	// Read-only memory (a CGImage's pixels, say) cannot be wired for the GPU to write (the kext's import): a copy, which nobody can have
	// changed, and the owner's deallocator kept until it goes.
	b = [self newBufferWithBytes:pointer length:length options:options];
	if (b && deallocator)
		[b keepOwner:^{ deallocator(pointer, length); }];
	return b;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	const MTLPixelFormat f = descriptor.pixelFormat;
	const NSUInteger n = descriptor.sampleCount;
	if ((f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth32Float_Stencil8) &&
	    ((descriptor.textureType == MTLTextureType2D && n == 1) || (descriptor.textureType == MTLTextureType2DMultisample && (n == 2 || n == 4))) &&
	    descriptor.width && descriptor.height && descriptor.width <= 16384 && descriptor.height <= 16384)
		return [[RDNA4MTLTexture alloc] initDepthWithDevice:self descriptor:descriptor];
	RDNA4MTLTexture *t = [[RDNA4MTLTexture alloc] initWithDevice:self descriptor:descriptor];
	if (!t)
		RTRACE(20, @"no texture of format %lu type %lu %lux%lux%lu, %lu levels, %lu samples, storage %lu", (unsigned long)f,
		       (unsigned long)descriptor.textureType, (unsigned long)descriptor.width, (unsigned long)descriptor.height,
		       (unsigned long)descriptor.depth, (unsigned long)descriptor.mipmapLevelCount, (unsigned long)n, (unsigned long)descriptor.storageMode);
	return t;
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
	if (!url.isFileURL || ![NSFileManager.defaultManager isReadableFileAtPath:url.path]) {
		if (error)
			*error = rdna4Error([NSString stringWithFormat:@"no metallib at %@", url], 0);
		return nil;
	}
	id<MTLLibrary> lib = [super newLibraryWithURL:url error:error];   // Metal's own (function constants, descriptors); ours if it refuses
	if (lib)
		return lib;
	return [[RDNA4MTLLibrary alloc] initWithDevice:self path:url.path];
}

- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
	return [self newLibraryWithURL:[NSURL fileURLWithPath:path] error:error];
}

// Metal's own parser first: its functions carry their AIR, no file needed (a sandboxed GPU process, ANGLE's library, cannot write one)
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
	id<MTLLibrary> lib = [super newLibraryWithData:data error:error];
	if (lib)
		return lib;
	NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:
	                                             [NSString stringWithFormat:@"rdna4mtl-%d-%@.metallib", getpid(), NSUUID.UUID.UUIDString]];
	if (![(NSData *)data writeToFile:path atomically:NO]) {
		if (error)
			*error = rdna4Error([NSString stringWithFormat:@"cannot write %@", path], 0);
		return nil;
	}
	if (error)
		*error = nil;
	return [[RDNA4MTLLibrary alloc] initWithDevice:self path:path];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error
{
	if (function.device != self) {
		if (error)
			*error = rdna4Error(@"a function of another device", 0);
		return nil;
	}
	return [[RDNA4MTLComputePipeline alloc] initWithDevice:self function:function error:error];
}

// Core Animation's compute pipelines (calculate_average_luma, which picks the menu bar's text colour) come with a descriptor; _MTLDevice's own
// answer failed and Core Animation aborted, logging the user out (2026-11-20).
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor error:(NSError **)error
{
	return [self newComputePipelineStateWithFunction:descriptor.computeFunction error:error];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options
                                                         reflection:(MTLAutoreleasedComputePipelineReflection *)reflection
                                                              error:(NSError **)error
{
	if (reflection)
		*reflection = nil;
	return [self newComputePipelineStateWithFunction:descriptor.computeFunction error:error];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                                            options:(MTLPipelineOption)options
                                                         reflection:(MTLAutoreleasedComputePipelineReflection *)reflection
                                                              error:(NSError **)error
{
	if (reflection)
		*reflection = nil;
	return [self newComputePipelineStateWithFunction:function error:error];
}

- (id<MTLFence>)newFence { return [[RDNA4MTLFence alloc] initWithDevice:self]; }

- (id<MTLArgumentEncoder>)newArgumentEncoderWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments
{
	return [[RDNA4MTLArgumentEncoder alloc] initWithDevice:self arguments:arguments];
}

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
	return [[RDNA4MTLSamplerState alloc] initWithDevice:self descriptor:descriptor];
}

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)descriptor
{
	return [[RDNA4MTLDepthStencilState alloc] initWithDevice:self descriptor:descriptor];
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error
{
	return [[RDNA4MTLRenderPipeline alloc] initWithDevice:self descriptor:descriptor error:error];
}

// The asynchronous forms: made as the synchronous ones, the handler called on another thread as Metal's (MoltenVK waits for it holding a
// lock the handler takes). Answered by _MTLDevice's own before, the handler never came and vkQuake waited for good (2026-11-21).
static dispatch_queue_t pipelineQueue(void) { return dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0); }
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                           completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
	dispatch_async(pipelineQueue(), ^{
		NSError *error = nil;
		id<MTLRenderPipelineState> state = [self newRenderPipelineStateWithDescriptor:descriptor error:&error];
		handler(state, error);
	});
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor options:(MTLPipelineOption)options
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
	dispatch_async(pipelineQueue(), ^{
		NSError *error = nil;
		MTLAutoreleasedRenderPipelineReflection reflection = nil;
		id<MTLRenderPipelineState> state = [self newRenderPipelineStateWithDescriptor:descriptor options:options reflection:&reflection
		                                                                         error:&error];
		handler(state, reflection, error);
	});
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler
{
	dispatch_async(pipelineQueue(), ^{
		NSError *error = nil;
		id<MTLComputePipelineState> state = [self newComputePipelineStateWithFunction:function error:&error];
		handler(state, error);
	});
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
                          completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler
{
	dispatch_async(pipelineQueue(), ^{
		NSError *error = nil;
		id<MTLComputePipelineState> state = [self newComputePipelineStateWithFunction:function error:&error];
		handler(state, nil, error);
	});
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor options:(MTLPipelineOption)options
                            completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler
{
	[self newComputePipelineStateWithFunction:descriptor.computeFunction options:options completionHandler:handler];
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                                                           options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
	id<MTLRenderPipelineState> state = [self newRenderPipelineStateWithDescriptor:descriptor error:error];
	if (reflection) {
		// GL's programs: Metal's own reflection object from the compiler service's data, which names their arguments (Apple's GL-on-Metal
		// renderer binds the uniform buffers it lists, and none without it: every uniform read 0, 2026-11-21). Others: none, as before.
		NSData *v = glReflection(descriptor.vertexFunction), *f = glReflection(descriptor.fragmentFunction);
		Class c = objc_getClass("MTLRenderPipelineReflectionInternal");
		SEL init = sel_registerName("initWithVertexData:fragmentData:serializedVertexDescriptor:device:options:flags:");
		id r = nil;
		// the vertex stage's input reflection needs the pipeline's vertex descriptor, serialized as Metal does (nil: Metal aborted)
		id vd = [descriptor.vertexDescriptor respondsToSelector:@selector(newSerializedDescriptor)]
		            ? ((id (*)(id, SEL))objc_msgSend)(descriptor.vertexDescriptor, @selector(newSerializedDescriptor)) : nil;
		if (state && (v || f) && c && [c instancesRespondToSelector:init] && (!v || vd))
			r = ((id (*)(id, SEL, id, id, id, id, unsigned long long, unsigned long long))objc_msgSend)([c alloc], init,
				v ? (id)dispatch_data_create(v.bytes, v.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT) : nil,
				f ? (id)dispatch_data_create(f.bytes, f.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT) : nil, vd, self, options, 0ull);
		*reflection = r;
		if ((v || f) && !r)
			RLOG(@"RDNA4MTL: pipeline %@ / %@: no reflection from its GL programs' data", descriptor.vertexFunction.name, descriptor.fragmentFunction.name);
	}
	return state;
}

// GL windows: our own drawable, and their surfaces' sizes (GLSurface above)
- (id)newGLDrawable { glSurfacesInstall(); return [RDNA4GLDrawable new]; }
- (int)getSurfaceMode:(unsigned long long *)mode width:(unsigned *)width height:(unsigned *)height forId:(unsigned)sid
{
	std::lock_guard<std::mutex> hold(gGLSurfacesLock);
	const GLSurface *g = glSurfaceWithId(sid);
	if (!g)
		return -1;
	*mode = g->options, *width = g->width, *height = g->height;
	return 0;
}

- (id<MTLCommandQueue>)newCommandQueue
{
	return [[RDNA4MTLCommandQueue alloc] initWithDevice:self];
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count
{
	return [self newCommandQueue];
}
// (Apple's GL-on-Metal renderer makes its queue so: _MTLDevice's answer was nil and the GL context failed, 2026-11-21)
- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:(id)descriptor   // (MTLCommandQueueDescriptor: macOS 15)
{
	return [self newCommandQueue];
}

RDNA4_ANSWER_MISSING
@end

// ---- heaps

// A heap whose resources are allocated each on its own from the device (no placement, no aliasing): what Core Animation asks of one, so
// that it does not go without (newHeapWithDescriptor: answered nil before). ponytail: size is a budget only, used size counted roughly.
@interface RDNA4MTLHeap : NSObject <MTLHeap>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLHeapDescriptor *)descriptor;
@end

@implementation RDNA4MTLHeap {
	RDNA4MTLDevice *_device;
	NSUInteger _size, _used;
	MTLStorageMode _storage;
}
@synthesize label;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLHeapDescriptor *)d
{
	if ((self = [super init])) {
		_device = device;
		_size = d.size;
		_storage = d.storageMode;
	}
	return self;
}

- (id<MTLDevice>)device { return _device; }
- (MTLStorageMode)storageMode { return _storage; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (MTLResourceOptions)resourceOptions { return (MTLResourceOptions)_storage << MTLResourceStorageModeShift; }
- (MTLHeapType)type { return MTLHeapTypeAutomatic; }
- (NSUInteger)size { return _size; }
- (NSUInteger)usedSize { return MIN(_used, _size); }
- (NSUInteger)currentAllocatedSize { return _size; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment { return _size > _used ? _size - _used : 0; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	_used += length;
	return [_device newBufferWithLength:length options:options];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
	_used += descriptor.width * descriptor.height * 4;
	return [_device newTextureWithDescriptor:descriptor];
}

RDNA4_ANSWER_MISSING
@end

@implementation RDNA4MTLDevice (Heaps)

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)descriptor
{
	return [[RDNA4MTLHeap alloc] initWithDevice:self descriptor:descriptor];
}

- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d
{
	const TextureFormat *fm = textureFormat((uint32_t)d.pixelFormat);
	const NSUInteger row = (d.width * (fm ? fm->bpp : 4) + 255) & ~(NSUInteger)255;
	return (MTLSizeAndAlign){ .size = row * d.height * MAX(d.depth, (NSUInteger)1) * MAX(d.arrayLength, (NSUInteger)1) * 2, .align = 256 };
}

- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	return (MTLSizeAndAlign){ .size = (length + 4095) & ~(NSUInteger)4095, .align = 4096 };
}

@end

// ---- the router (the native desktop)

// The paravirtual accelerator's Metal plug-in class while the kext routes it here (src/accelcensus.cpp routePv: debug.rdna4_nativemtl=2 or 3,
// rdna4-nativews): a process the accelerator's RDNA4NativeProcesses names (WindowServer), or one started with RDNA4_NATIVE_MTL=router, gets our
// device for it; every other process Apple's paravirtual device, from Apple's bundle (RDNA4AppleMetalPluginName, ...ClassName), as before.
@interface RDNA4MTLRouter : _MTLDevice
@end

@implementation RDNA4MTLRouter

// A named process (WindowServer) that keeps starting: its third start within two minutes gets Apple's device, so that a crash on ours
// ends with the desktop back (a file of start times in the process's own temporary directory, which WindowServer's sandbox lets it write).
static BOOL crashLooping(NSString *me)
{
	NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"rdna4-native-starts"];
	const double now = NSDate.date.timeIntervalSince1970;
	NSMutableArray<NSNumber *> *starts = [NSMutableArray array];
	for (NSString *line in [[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] componentsSeparatedByString:@"\n"])
		if (line.doubleValue > now - 120)
			[starts addObject:@(line.doubleValue)];
	const BOOL looping = starts.count >= 2;
	if (!looping)
		[starts addObject:@(now)];
	[[starts componentsJoinedByString:@"\n"] writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
	if (looping)
		RLOG(@"RDNA4MTLRouter: %@: started %lu times in two minutes on the native device: Apple's this time", me, (unsigned long)starts.count);
	return looping;
}

static NSString *serviceString(io_service_t service, CFStringRef key)
{
	CFTypeRef v = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
	NSString *s = v && CFGetTypeID(v) == CFStringGetTypeID() ? (__bridge NSString *)v : nil;
	if (v && !s)
		CFRelease(v);
	return s ? CFBridgingRelease(v) : nil;
}

- (instancetype)initWithAcceleratorPort:(io_service_t)service
{
	self = [super init];   // _MTLDevice's dealloc expects its init to have run; this object goes, the one returned stays
	NSString *me = @(getprogname());
	NSArray *native = [serviceString(service, CFSTR("RDNA4NativeProcesses")) componentsSeparatedByString:@","];
	// RDNA4_NATIVE_MTL=router: this process on our device here too (=1 alone makes our device for its own service, RDNA4NativeMetal, and
	// leaves Apple's here: the checks compare the two)
	const char *env = getenv("RDNA4_NATIVE_MTL");
	if ((env && !strcmp(env, "router")) || ([native containsObject:me] && !crashLooping(me))) {
		RDNA4MTLDevice *d = [[RDNA4MTLDevice alloc] initNativeWithAcceleratorPort:service];
		RLOG(@"RDNA4MTLRouter: %@: the native device%s", me, d ? "" : " could not be made: Apple's paravirtual device");
		if (d)
			return (id)d;
	}
	NSString *name = serviceString(service, CFSTR("RDNA4AppleMetalPluginName")) ?: @"AppleParavirtGPUMetal";
	NSString *cls = serviceString(service, CFSTR("RDNA4AppleMetalPluginClassName")) ?: @"AppleParavirtDevice";
	NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithFormat:@"/System/Library/Extensions/%@.bundle", name]];
	NSError *why = nil;
	Class apple = [bundle loadAndReturnError:&why] ? NSClassFromString(cls) : nil;
	if (!apple) {
		RLOG(@"RDNA4MTLRouter: %@: Apple's %@ (%@) could not be loaded: %@", me, cls, name, why);
		return nil;
	}
	return [(id)[apple alloc] initWithAcceleratorPort:service];
}

@end
