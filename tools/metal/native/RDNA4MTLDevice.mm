/*
 * RDNA4MTLDevice.m
 * AMDRadeonRDNA4
 *
 * The native-Metal spike's driver bundle (docs/native-metal-spike.md): the class Metal makes for our IOAccelerator service
 * (src/accelcensus.cpp, level 3: MetalPluginName "AMDRadeonRDNA4Metal", MetalPluginClassName "RDNA4MTLDevice").
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
#import <os/log.h>

// NSLog's %@ arguments are <private> in the unified log (WindowServer's messages were unreadable): the text, formatted, as public
#define RLOG(...) os_log(OS_LOG_DEFAULT, "%{public}s", [NSString stringWithFormat:__VA_ARGS__].UTF8String)
// The first `n` of a kind, logged ("RDNA4MTL trace: ..."), to see what a process (WindowServer) does with the device.
#define RTRACE(n, ...) do { static int seen_; if (__atomic_fetch_add(&seen_, 1, __ATOMIC_RELAXED) < (n)) RLOG(@"RDNA4MTL trace: " __VA_ARGS__); } while (0)

extern "C" {
#include "librdna4.h"
}
#include "pvdraw.hpp"
#include "gfx12tri.h"
#include "gfx12tricol.h"
#include "gfx12trirun.h"
#include "rdna4mtlc.h"

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
@end

@implementation RDNA4MTLBuffer {
	RDNA4MTLDevice *_device;
	rdna4_buffer_t _buf;
	void *_cpu;
	NSUInteger _length;
	MTLResourceOptions _options;
	void (^_deallocator)(void *, NSUInteger);   // an imported buffer's owner's
	void (^_owner)(void);
}

- (void)keepOwner:(void (^)(void))release { _owner = release; }
@synthesize label;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device bytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options
                   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	if (!(self = [super init]) || !device.gpu)
		return nil;
	const uint64_t bytes = ((length ?: 1) + 4095) & ~4095ull;
	kern_return_t kr = ((uintptr_t)pointer & 4095) ? kIOReturnBadArgument : rdna4_import_host(device.gpu, pointer, bytes, 0, &_buf);
	// memory this process may only read (Core Animation's shared with WindowServer): for the GPU to read, so its updates are seen (copied,
	// the menu bar's app menus never changed after the first)
	if (kr == KERN_PROTECTION_FAILURE)
		kr = rdna4_import_host(device.gpu, pointer, bytes, RDNA4_HOST_READ_ONLY, &_buf);
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
	const kern_return_t kr = rdna4_alloc_host(device.gpu, bytes, &_buf, &_cpu);
	if (kr) {
		rdna4Error([NSString stringWithFormat:@"a buffer of %lu bytes", (unsigned long)length], kr);
		return nil;
	}
	_device = device;
	_length = length;
	_options = options;
	return self;
}

- (void)dealloc
{
	if (_cpu)
		rdna4_free(_device.gpu, &_buf);   // an imported range unmapped and unwired, not freed
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
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
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
		return YES;
	default: return NO;
	}
}

@interface RDNA4MTLTexture : NSObject <MTLTexture>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor;
// Over an IOSurface (CAMetalLayer's drawables): its memory mapped into the GPU, its rows as the surface has them.
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)surface
                         plane:(NSUInteger)plane;
- (void)clearTo:(MTLClearColor)colour;
- (NSError *)clearDepthTo:(double)depth;
// A view (newTextureViewWithPixelFormat:...): the same memory from level `levels.location` on, read as `format` (of the same size a pixel).
- (instancetype)initViewOf:(RDNA4MTLTexture *)texture format:(MTLPixelFormat)format type:(MTLTextureType)type levels:(NSRange)levels
                    slices:(NSRange)slices;   // a Depth32Float texture: every pixel (the tiling does not matter for one value)
- (void)descriptor:(uint32_t[8])t;          // its T# for shaders (level 0)
- (void)descriptor:(uint32_t[8])t level:(NSUInteger)level;   // level `level` as an image of its own (explicit-LOD sampling)
- (void)refresh;                            // an IOSurface's copy for the GPU made current (rows not 128-byte multiples)
- (void)generateMipmaps;                    // levels 1 .. from level 0 (the CPU, 2x2 boxes)
- (RDNA4MTLTexture *)copyForReading;        // its pixels now, in a texture of its own (kept for the next time); drained first by the caller
- (uint32_t)sampleOr;                       // the OR of some of its pixels' first 4 bytes (the trace)
- (RDNA4MTLTexture *)level:(NSUInteger)level;   // level `level` as a texture of its own (kept): a render pass's attachment level
@property (readonly) BOOL shadowed;         // the GPU reads a copy of an IOSurface's pixels (refresh)
// A view of a buffer's memory (newLinearTextureWithDescriptor:): rows `bpr` apart from `offset`.
- (instancetype)initWithBuffer:(RDNA4MTLBuffer *)buffer descriptor:(MTLTextureDescriptor *)descriptor offset:(NSUInteger)offset
                   bytesPerRow:(NSUInteger)bpr;
@property (readonly) BOOL isDepth;        // Depth32Float in VRAM, 64KB_2D (as the PV host's depth buffers), for the depth block only
@property (readonly) uint64_t gpuBase;    // the first row's GPU address (page aligned)
@property (readonly) NSUInteger rowBytes; // a multiple of 256
@property (readonly) uint32_t bpp;        // bytes a pixel
@property (readonly) uint8_t *bytes;      // a colour texture's first pixel (the CPU's view)
@property (readonly) RDNA4MTLBuffer *memory;   // a colour texture's pixels
@end

@implementation RDNA4MTLTexture {
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
	NSUInteger _levels, _levelOffset[16], _levelRow[16];   // a mipmapped 2D texture's levels: each an image of its own in _memory
	const uint8_t *_surfaceRows;   // shadowed: the IOSurface's own pixels, _surfaceRowBytes apart; _memory is the GPU's copy
	NSUInteger _surfaceRowBytes;
	RDNA4MTLTexture *_parent;   // a view's
	rdna4_buffer_t _vram;   // a depth texture's
}
@synthesize label;

- (instancetype)initDepthWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)d
{
	if (!(self = [super init]) || !device.gpu)
		return nil;
	// whole 256x256-pixel blocks, as the PV host's (src/pvgpu.cpp stencilFor): the depth block's 64KB_2D layout fits in them
	const uint64_t bytes = (uint64_t)((d.width + 255) & ~(NSUInteger)255) * ((d.height + 255) & ~(NSUInteger)255) * 4;
	const kern_return_t kr = rdna4_alloc(device.gpu, bytes, &_vram);
	if (kr) {
		rdna4Error([NSString stringWithFormat:@"a %lux%lu depth texture", (unsigned long)d.width, (unsigned long)d.height], kr);
		return nil;
	}
	_isDepth = YES;
	_device = device;
	_width = d.width;
	_height = d.height;
	_format = d.pixelFormat;
	_usage = d.usage;
	return self;
}

- (NSError *)clearDepthTo:(double)depth
{
	static NSMutableData *pattern;   // ponytail: one buffer of the value, grown to the largest depth texture; a GPU fill when it shows
	static float patternValue = -1;
	const float value = (float)depth;
	if (!pattern || pattern.length < _vram.bytes || patternValue != value) {
		pattern = [NSMutableData dataWithLength:(NSUInteger)_vram.bytes];
		float *p = (float *)pattern.mutableBytes;
		for (NSUInteger i = 0; i < _vram.bytes / 4; i++)
			p[i] = value;
		patternValue = value;
	}
	const kern_return_t kr = rdna4_write(_device.gpu, &_vram, 0, pattern.bytes, _vram.bytes);
	return kr ? rdna4Error(@"a depth clear", kr) : nil;
}

- (void)descriptor:(uint32_t[8])t level:(NSUInteger)level
{
	level = MIN(level, MAX(_levels, (NSUInteger)1) - 1);
	if (!level) {
		[self descriptor:t];
		return;
	}
	const uint32_t w = (uint32_t)MAX(_width >> level, (NSUInteger)1), h = (uint32_t)MAX(_height >> level, (NSUInteger)1);
	linearImage(t, self.gpuBase + _levelOffset[level], _fm->gfx, _fm->sel, w, h, (uint32_t)(_levelRow[level] / _fm->bpp));
}

- (BOOL)shadowed { return _surfaceRows != NULL; }

- (uint32_t)sampleOr
{
	uint32_t v = 0;
	for (NSUInteger y = 0; !_isDepth && y < _height; y += MAX(_height / 37, (NSUInteger)1))
		for (NSUInteger x = 0; x < _width; x += MAX(_width / 41, (NSUInteger)1)) {
			uint32_t w;
			memcpy(&w, self.bytes + y * _rowBytes + x * self.bpp, MIN(self.bpp, 4u));
			v |= w;
		}
	return v;
}

- (RDNA4MTLTexture *)copyForReading
{
	static char key;
	RDNA4MTLTexture *copy = objc_getAssociatedObject(self, &key);
	if (!copy) {
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:_format width:_width height:_height mipmapped:NO];
		copy = [[RDNA4MTLTexture alloc] initWithDevice:_device descriptor:d];
		objc_setAssociatedObject(self, &key, copy, OBJC_ASSOCIATION_RETAIN);
	}
	NSError *drained = [_device drainGfx];   // what earlier draws put into it
	if (drained)
		return nil;
	for (NSUInteger y = 0; copy && y < _height; y++)
		memcpy(copy.bytes + y * copy.rowBytes, self.bytes + y * _rowBytes, _width * self.bpp);
	return copy;
}

- (void)refresh
{
	for (NSUInteger y = 0; _surfaceRows && y < _height; y++)
		memcpy(self.bytes + y * _rowBytes, _surfaceRows + y * _surfaceRowBytes, _width * self.bpp);
}

- (void)descriptor:(uint32_t[8])t
{
	if (_type == MTLTextureType3D) {   // the hardware's own pitches (pvdraw.hpp linearVolume; the memory laid out so in the init)
		linearVolume(t, self.gpuBase, _fm->gfx, _fm->sel, (uint32_t)_width, (uint32_t)_height, (uint32_t)_depth);
		return;
	}
	linearImage(t, self.gpuBase, _fm->gfx, _fm->sel, (uint32_t)_width, (uint32_t)_height, (uint32_t)(_rowBytes / _fm->bpp));
	if (_type == MTLTextureType1D)
		t[3] = (t[3] & 0x0fffffffu) | 8u << 28;   // TYPE 1D
	else if (_type == MTLTextureType1DArray)
		linearArray1d(t, self.gpuBase, _fm->gfx, _fm->sel, (uint32_t)_width, (uint32_t)_layers);
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
	} else {
		_memory = [[RDNA4MTLBuffer alloc] initWithDevice:device bytesNoCopy:base length:bytes options:MTLResourceStorageModeShared
		                                     deallocator:^(void *p, NSUInteger n) { CFRelease(surface); }];
		_rowBytes = bpr;
	}
	if (!_memory)
		return nil;
	_device = device;
	_width = d.width;
	_height = d.height;
	_format = d.pixelFormat;
	_usage = d.usage;
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
		NSMutableDictionary *views = objc_getAssociatedObject(self, &key);
		if (!views)
			objc_setAssociatedObject(self, &key, views = [NSMutableDictionary dictionary], OBJC_ASSOCIATION_RETAIN);
		RDNA4MTLTexture *v = views[@(level)];
		if (!v && (v = [[RDNA4MTLTexture alloc] initViewOf:self format:_format type:MTLTextureType2D levels:NSMakeRange(level, 1)
		                                            slices:NSMakeRange(0, 1)]))
			views[@(level)] = v;
		return v;
	}
}

- (IOSurfaceRef)iosurface { return _surface; }
- (NSUInteger)iosurfacePlane { return _plane; }
- (uint32_t)bpp { return _fm ? _fm->bpp : 4; }
- (MTLTextureSwizzleChannels)swizzle { return MTLTextureSwizzleChannelsDefault; }
- (MTLTextureCompressionType)compressionType { return MTLTextureCompressionTypeLossless; }
- (BOOL)allowGPUOptimizedContents { return NO; }
- (MTLResourceID)gpuResourceID { return (MTLResourceID){ ._impl = self.gpuBase }; }

- (instancetype)initWithBuffer:(RDNA4MTLBuffer *)buffer descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bpr
{
	if (!(self = [super init]))
		return nil;
	_fm = textureFormat((uint32_t)d.pixelFormat);
	if (d.textureType != MTLTextureType2D || !_fm || !d.width || !d.height || bpr < d.width * _fm->bpp ||
	    offset + bpr * (d.height - 1) + d.width * _fm->bpp > buffer.length) {
		rdna4Error([NSString stringWithFormat:@"a texture of format %lu, %lux%lu over a buffer at %lu, %lu bytes a row: only 2D of pvdraw.hpp's "
		                                      @"formats inside the buffer, so far", (unsigned long)d.pixelFormat,
		                                      (unsigned long)d.width, (unsigned long)d.height, (unsigned long)offset, (unsigned long)bpr], 0);
		return nil;
	}
	_type = MTLTextureType2D;
	if ((offset & 255) || (bpr & 127) || bpr % _fm->bpp) {   // where the hardware cannot read it in place (Core Animation's at offset 32): a copy
		_surfaceRows = (const uint8_t *)buffer.contents + offset;
		_surfaceRowBytes = bpr;
		_rowBytes = (d.width * _fm->bpp + 255) & ~(NSUInteger)255;
		_memory = [[RDNA4MTLBuffer alloc] initWithDevice:(RDNA4MTLDevice *)buffer.device length:_rowBytes * d.height options:MTLResourceStorageModeShared];
		RDNA4MTLBuffer *source = buffer;
		[_memory keepOwner:^{ (void)source; }];   // the memory the copy is of stays while the texture does
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
	return self;
}

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)d
{
	if (!(self = [super init]))
		return nil;
	_fm = textureFormat((uint32_t)d.pixelFormat);
	// rows: a 1D texture's one, a 1D array's one a layer (CoreDisplay's combined gamma LUT: R32Float, 8192 wide), their T#s typed so
	const BOOL oneD = (d.textureType == MTLTextureType1D || (d.textureType == MTLTextureType1DArray && d.arrayLength >= 1 && d.arrayLength <= 2048)) &&
	                  d.height == 1;
	const BOOL threeD = d.textureType == MTLTextureType3D && d.mipmapLevelCount == 1 && d.depth >= 1 && d.depth <= 2048;
	const BOOL twoD = d.textureType == MTLTextureType2D && d.mipmapLevelCount >= 1 && d.mipmapLevelCount <= 15;
	if ((!twoD && !threeD && !(oneD && d.mipmapLevelCount == 1)) || d.sampleCount != 1 || !_fm || !d.width || !d.height ||
	    d.width > 16384 || d.height > 16384) {
		rdna4Error([NSString stringWithFormat:@"a texture of type %lu, format %lu, %lux%lu, %lu layers, %lu levels, %lu samples: only 2D and 1D of "
		                                      @"pvdraw.hpp's formats with one level and one sample so far", (unsigned long)d.textureType,
		                                      (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height,
		                                      (unsigned long)d.arrayLength, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount], 0);
		return nil;
	}
	_type = d.textureType;
	_layers = _type == MTLTextureType1DArray ? d.arrayLength : 1;
	_depth = threeD ? d.depth : 1;
	_rowBytes = (d.width * _fm->bpp + 255) & ~(NSUInteger)255;   // the colour block's pitch in whole 256-byte rows (the PV host: a multiple of 128)
	NSUInteger bytes = _rowBytes * d.height * _layers;
	if (threeD) {   // the hardware's pitches for a linear 3D image (pvdraw.hpp linearVolume): rows at 128 bytes, slices at 256
		_rowBytes = (d.width * _fm->bpp + 127) & ~(NSUInteger)127;
		_slicePitch = (_rowBytes * d.height + 255) & ~(NSUInteger)255;
		bytes = _slicePitch * _depth;
	}
	_levels = twoD ? d.mipmapLevelCount : 1;
	_levelRow[0] = _rowBytes;
	for (NSUInteger l = 1; l < _levels; l++) {   // each level an image of its own after level 0, 256-byte aligned
		const NSUInteger w = MAX(d.width >> l, (NSUInteger)1), h = MAX(d.height >> l, (NSUInteger)1);
		_levelOffset[l] = (bytes + 255) & ~(NSUInteger)255;
		_levelRow[l] = (w * _fm->bpp + 255) & ~(NSUInteger)255;
		bytes = _levelOffset[l] + _levelRow[l] * h;
	}
	_memory = [[RDNA4MTLBuffer alloc] initWithDevice:device length:bytes options:MTLResourceStorageModeShared];
	if (!_memory)
		return nil;
	_device = device;
	_width = d.width;
	_height = d.height;
	_format = d.pixelFormat;
	_usage = d.usage;
	return self;
}

- (id<MTLDevice>)device { return _device; }
- (MTLTextureType)textureType { return _type == MTLTextureType1D || _type == MTLTextureType1DArray || _type == MTLTextureType3D ? _type : MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return _format; }
- (NSUInteger)width { return _width; }
- (NSUInteger)height { return _height; }
- (NSUInteger)depth { return _depth ?: 1; }
- (NSUInteger)mipmapLevelCount { return _levels ?: 1; }
- (NSUInteger)sampleCount { return 1; }
- (NSUInteger)arrayLength { return _layers ?: 1; }
- (MTLTextureUsage)usage { return _usage; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (NSUInteger)allocatedSize { return _memory.allocatedSize; }
- (BOOL)isFramebufferOnly { return NO; }
- (uint64_t)gpuBase { return _isDepth ? _vram.gpu : _memory.gpuAddress + _offset; }
- (uint8_t *)bytes { return (uint8_t *)_memory.contents + _offset; }

- (void)dealloc
{
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

- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr
{
	for (NSUInteger y = 0; [self fits:r level:level] && y < r.size.height; y++)
		memcpy([self at:level x:r.origin.x y:r.origin.y + y z:r.origin.z], (const uint8_t *)bytes + y * bpr, r.size.width * self.bpp);
}

- (void)generateMipmaps
{
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
	if (_type == MTLTextureType1DArray && slice < _layers && !level && r.origin.x + r.size.width <= _width)   // a layer is a row
		memcpy(self.bytes + slice * _rowBytes + r.origin.x * self.bpp, bytes, r.size.width * self.bpp);
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
	if (!slice)
		[self getBytes:bytes bytesPerRow:bpr fromRegion:r mipmapLevel:level];
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
	for (NSUInteger y = 0; [self fits:r level:level] && y < r.size.height; y++)
		memcpy((uint8_t *)bytes + y * bpr, [self at:level x:r.origin.x y:r.origin.y + y z:r.origin.z], r.size.width * self.bpp);
}

// A clear load action by the CPU: the colour packed as the format stores it (pvdraw.hpp clearPattern; RGBA16Float as four halves).
// ponytail: a CPU fill; a GPU clear when it shows in a profile.
- (void)clearTo:(MTLClearColor)c
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
- (NSError *)build:(MTLPrimitiveAccelerationStructureDescriptor *)descriptor;
@property (readonly) RDNA4MTLBuffer *memory;
@end

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

- (NSError *)build:(MTLPrimitiveAccelerationStructureDescriptor *)d
{
	if (![d isKindOfClass:[MTLPrimitiveAccelerationStructureDescriptor class]])
		return rdna4Error(@"only primitive acceleration structures so far (no instances)", 0);
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

// `function` translated by the translator in file mode ($RDNA4_AIR2AMDGPU, the bundle's own, or /usr/local/libexec/rdna4-air2amdgpu): a kernel's code
// object, or a vertex or fragment function's RGFX blob (tools/metal/m3/air2amdgpu.cpp), and a kernel's count of buffer addresses.
// A process that may not start programs (WindowServer's sandbox): the kext's translator helper does it (rdna4_translate).
static NSError *translateByKext(rdna4_t *gpu, NSString *input, NSString *name, NSData **blob, uint32_t *buffers)
{
	NSData *air = [NSData dataWithContentsOfFile:input];
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

static NSError *translateFunction(id<MTLFunction> function, NSData **blob, uint32_t *buffers)
{
	// the bundle's own (build.sh: the translator these functions need, ray queries say), else the paravirtual GPU's helper
	NSString *own = [[NSBundle bundleForClass:[RDNA4MTLFunction class]] pathForResource:@"air2amdgpu" ofType:nil];
	NSString *tool = NSProcessInfo.processInfo.environment[@"RDNA4_AIR2AMDGPU"] ?: own ?: @"/usr/local/libexec/rdna4-air2amdgpu";
	NSString *out = [NSTemporaryDirectory() stringByAppendingPathComponent:
	                                            [NSString stringWithFormat:@"rdna4mtl-%d-%@.bin", getpid(), function.name]];
	// The input: our library's metallib, or the AIR bitcode of a function Metal's own compiler made (newLibraryWithSource: and the other
	// library calls _MTLDevice answers: an _MTLFunctionInternal, its -bitcodeData)
	NSString *input = nil, *scratch = nil;
	if ([(id)function isKindOfClass:[RDNA4MTLFunction class]]) {
		input = ((RDNA4MTLFunction *)function).library.path;
	} else if ([(id)function respondsToSelector:@selector(bitcodeData)]) {
		NSData *bitcode = [(id)function performSelector:@selector(bitcodeData)];
		scratch = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"rdna4mtl-%d-%@.bc", getpid(), function.name]];
		if ([bitcode isKindOfClass:[NSData class]] && [bitcode writeToFile:scratch atomically:NO])
			input = scratch;
	}
	if (!input)
		return rdna4Error([NSString stringWithFormat:@"%@: a function of class %@ with no AIR to translate", function.name, [(id)function class]], 0);
	NSTask *task = [NSTask new];
	NSPipe *pipe = [NSPipe pipe];
	task.executableURL = [NSURL fileURLWithPath:tool];
	task.arguments = @[ input, function.name, out ];
	task.standardOutput = pipe;
	task.standardError = pipe;
	NSError *runError = nil;
	if (![task launchAndReturnError:&runError]) {   // not allowed to (a sandbox): through the kext
		NSError *why = translateByKext(((RDNA4MTLDevice *)function.device).gpu, input, function.name, blob, buffers);
		if (scratch)
			[NSFileManager.defaultManager removeItemAtPath:scratch error:nil];
		return why;
	}
	NSData *said = [pipe.fileHandleForReading readDataToEndOfFile];
	[task waitUntilExit];
	if (scratch)
		[NSFileManager.defaultManager removeItemAtPath:scratch error:nil];
	NSString *text = [[NSString alloc] initWithData:said encoding:NSUTF8StringEncoding];
	NSArray *words = [text componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];   // "<name> <buffers> <written>"
	*blob = [NSData dataWithContentsOfFile:out];
	[NSFileManager.defaultManager removeItemAtPath:out error:nil];
	if (task.terminationStatus || words.count < 2 || !*blob)
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
	NSError *why = translateFunction(function, &elf, &_buffers);
	if (why)
		return why;
	const kern_return_t kr = rdna4_load(_device.gpu, elf.bytes, elf.length, function.name.UTF8String, &_program);
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

enum { kMaxBufferIndex = 31, kTableBuffers = 28 };   // a stage's table holds buffers 0-27 (RDNA4_MTLC_HIGH_BUFFERS, 28-30, not yet)

// The RGFX header (air2amdgpu.cpp): {"RGFX", stage, code bytes, RSRC1, RSRC2, SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR, attributes}, the code,
// then per value between the stages {u32 name hash, u32 flags: bit 0 flat, bit 1 the point's coordinate}.
enum { kRgfxMagic = 0x58464752u, kHead = 8 };

@interface RDNA4MTLRenderPipeline : NSObject <MTLRenderPipelineState>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error;
@property (readonly) const uint32_t *vs, *ps;   // the RGFX blobs
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
enum { kStageTextures = 16 };   // a stage's table holds textures and samplers 0-15 (include/rdna4mtlc.h)

typedef struct {
	uint64_t table[2][kTableBuffers];   // per stage, its buffers' GPU addresses by index (0: unbound)
	uint32_t textures[2][kStageTextures][8], samplers[2][kStageTextures][4];   // per stage, T#s and S#s (0: unbound)
	uint32_t type, start, count, instances, baseInstance;
	float viewport[4];                  // x, y, width, height in pixels
	uint32_t scissor[4];                // x, y, width, height in pixels (the whole target when none was set)
	uint32_t attachment;                // the colour attachment the draw goes into (RDNA4_MTLC_TABLE_ATTACHMENT)
	const uint8_t *levelT;              // for shaders that sample explicit levels: per stage, levels 1-7, textures 0-15, a T# each (else NULL)
	float blendColour[4];
	uint64_t indexGpu;                  // an indexed draw's indices (0: not indexed), 16- or 32-bit
	uint32_t indexBytes;
	uint64_t depthGpu;                  // the pass's Depth32Float attachment (0: none), its size, the state's test and write
	uint32_t depthWidth, depthHeight, depthCompare, depthWrite;
} RDNA4Draw;

@interface RDNA4MTLDevice ()
- (BOOL)gfx;   // the client may submit gfx IBs and the device's gfx buffers exist
- (uint64_t)gfxRings;
- (NSError *)draw:(const RDNA4Draw *)draw pipeline:(RDNA4MTLRenderPipeline *)pipeline target:(RDNA4MTLTexture *)target;
- (NSError *)drainGfx;
- (dispatch_queue_t)work;
- (void)spareImage:(uint32_t[8])t;   // the T# of an unbound texture slot: one texel of the zeroed spare buffer
- (uint64_t)spareBuffer;             // what an unbound buffer slot points at: the zeroed spare buffer
- (BOOL)drawnInFlight:(RDNA4MTLTexture *)t;   // a draw not yet waited for draws into it
@end

@implementation RDNA4MTLRenderPipeline {
	RDNA4MTLDevice *_device;
	NSData *_blob[2];
	uint32_t _cntl[16];
	rdna4_buffer_t _codeBuffer;
	uint32_t _blends[8][10], _attributeRecords[16][4];
	MTLPixelFormat _formats[8];
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

- (NSError *)build:(MTLRenderPipelineDescriptor *)desc
{
	if (!_device.gfx)
		return rdna4Error(@"no gfx IBs: the kext's runtime client is not open (root only), or the boot has no client gfx IBs (rdna4-gfx=2)", 0);
	id<MTLFunction> fn[2] = { desc.vertexFunction, desc.fragmentFunction };
	if (!fn[0] || !fn[1] || fn[0].device != _device || fn[1].device != _device)
		return rdna4Error(@"a render pipeline needs a vertex and a fragment function of this device so far", 0);
	MTLRenderPipelineColorAttachmentDescriptor *a = desc.colorAttachments[0];
	_format = a.pixelFormat;
	if (desc.depthAttachmentPixelFormat != MTLPixelFormatInvalid && desc.depthAttachmentPixelFormat != MTLPixelFormatDepth32Float)
		return rdna4Error(@"a depth attachment other than Depth32Float: not yet", 0);
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
	if (desc.rasterSampleCount > 1)
		return rdna4Error([NSString stringWithFormat:@"%lu samples: one only so far", (unsigned long)desc.rasterSampleCount], 0);
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
		const uint32_t step = l.stepFunction ? (uint32_t)l.stepFunction : 1, rate = l.stepRate ? (uint32_t)l.stepRate : 1;
		const uint32_t rec[4] = { (uint32_t)at.offset, (uint32_t)l.stride, buffer | (f & 15) << 8 | (f >> 4 & 7) << 12 | (f >> 7 & 1) << 15 | step << 16, rate };
		memcpy(_attributeRecords[i], rec, sizeof rec);
	}
	for (int k = 0; k < 2; k++) {
		uint32_t unused;
		NSData *blob = nil;
		_names = [NSString stringWithFormat:@"%@ / %@", fn[0].name, fn[1].name];
		NSError *why = translateFunction(fn[k], &blob, &unused);
		if (why)
			return why;
		_blob[k] = blob;
	}
	const uint32_t *vs = (const uint32_t *)_blob[0].bytes, *ps = (const uint32_t *)_blob[1].bytes;
	const uint32_t room[2] = { RDNA4_TRI_PS_OFFSET - 4 * RDNA4_TRI_SHADER_PAD, RDNA4_TRI_CODE_BYTES - RDNA4_TRI_PS_OFFSET - 4 * RDNA4_TRI_SHADER_PAD };
	for (int k = 0; k < 2; k++) {
		const uint32_t *h = (const uint32_t *)_blob[k].bytes;
		if (_blob[k].length < 4 * kHead || h[0] != kRgfxMagic || h[1] != (uint32_t)k + 1 || h[2] > room[k] || (h[7] & 0xffff) > 16 ||
		    4ull * kHead + h[2] + 8ull * (h[7] & 0xffff) > _blob[k].length)
			return rdna4Error([NSString stringWithFormat:@"%@: not a %s shader that fits its slot", fn[k].name, k ? "fragment" : "vertex"], 0);
		if (h[7] & 0x7fff0000u & ~(k ? 0x7ff10000u : 0x00010000u))   // dual source: not yet (several attachments: the PV host's choice)
			return rdna4Error([NSString stringWithFormat:@"%@: attributes 0x%x not supported yet", fn[k].name, h[7]], 0);
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

RDNA4_ANSWER_MISSING
@end

@interface RDNA4MTLDepthStencilState : NSObject <MTLDepthStencilState>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLDepthStencilDescriptor *)d;
@property (readonly) uint32_t compare;   // Metal's compare function: the hardware's ZFUNC
@property (readonly) BOOL write;
@end

@implementation RDNA4MTLDepthStencilState {
	RDNA4MTLDevice *_device;
}
@synthesize label;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLDepthStencilDescriptor *)d
{
	if ((self = [super init])) {
		_device = device;
		_compare = (uint32_t)d.depthCompareFunction & 7;
		_write = d.depthWriteEnabled;
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
@end

@interface RDNA4MTLCommandBuffer : NSObject <MTLCommandBuffer>
- (instancetype)initWithQueue:(RDNA4MTLCommandQueue *)queue;
- (void)record:(RDNA4Op)op;       // runs once the draws before it have finished
// set*Bytes data: copied into a 256 KiB chunk of the command buffer's (256-byte aligned), so that thousands of them are not a buffer each
// (WindowServer ran the kext's runtime out of buffers); returns the chunk, the data at *offset in it
- (id<MTLBuffer>)stage:(const void *)bytes length:(NSUInteger)length offset:(NSUInteger *)offset;
- (void)recordDraw:(RDNA4Op)op;   // a draw: submitted behind the draws before it, not waited for
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
}
@synthesize label;

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
	[self setBuffer:[(id)as isKindOfClass:[RDNA4MTLAccelerationStructure class]] ? ((RDNA4MTLAccelerationStructure *)as).memory : nil offset:0
	        atIndex:index];
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
	NSMutableArray<RDNA4MTLTexture *> *shadowed = [NSMutableArray array];
	uint8_t table[0x400] = {};
	BOOL anyTexture = NO;
	for (int i = 0; i < 16; i++) {
		if ([(id)_textures[i] isKindOfClass:[RDNA4MTLTexture class]] && !((RDNA4MTLTexture *)_textures[i]).isDepth) {
			RDNA4MTLTexture *t = (RDNA4MTLTexture *)_textures[i];
			[t descriptor:(uint32_t *)(table + RDNA4_MTLC_TABLE_TEXTURES + 32 * i)];
			[held addObject:t];
			if (t.shadowed)
				[shadowed addObject:t];
			anyTexture = YES;
		}
		if ([(id)_samplers[i] isKindOfClass:[RDNA4MTLSamplerState class]]) {
			memcpy(table + RDNA4_MTLC_TABLE_SAMPLERS + 16 * i, ((RDNA4MTLSamplerState *)_samplers[i]).words, 16);
			anyTexture = YES;
		}
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
	[_commandBuffer record:^NSError * {
		rdna4_program_t prog = pipeline.program;
		const uint32_t g[3] = { (uint32_t)groups.width, (uint32_t)groups.height, (uint32_t)groups.depth };
		const uint32_t s[3] = { (uint32_t)groupSize.width, (uint32_t)groupSize.height, (uint32_t)groupSize.depth };
		(void)held;
		for (RDNA4MTLTexture *t in shadowed)
			[t refresh];
		const kern_return_t kr = device.gpu ? rdna4_dispatch(device.gpu, &prog, g, s, args.bytes, (uint32_t)args.length, 1000, NULL) : kIOReturnNotOpen;
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
	id<MTLTexture> _textures[2][kStageTextures];
	id<MTLSamplerState> _samplers[2][kStageTextures];
	RDNA4MTLTexture *_depth;
	RDNA4MTLDepthStencilState *_depthState;
	MTLViewport _viewport;
	MTLScissorRect _scissor;
	float _blendColour[4];
}
@synthesize label;

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
		const MTLClearColor c = a.clearColor;
		if (a.loadAction == MTLLoadActionClear)
			[commandBuffer record:^NSError * {
				[t clearTo:c];
				return nil;
			}];
	}
	RTRACE(40, @"render pass: attachment 0 %@ format %lu %lux%lu load %lu, attachment 1 %@, depth %@", pass.colorAttachments[0].texture ? NSStringFromClass([(id)pass.colorAttachments[0].texture class]) : @"-",
	       (unsigned long)pass.colorAttachments[0].texture.pixelFormat, (unsigned long)pass.colorAttachments[0].texture.width,
	       (unsigned long)pass.colorAttachments[0].texture.height, (unsigned long)pass.colorAttachments[0].loadAction,
	       pass.colorAttachments[1].texture ? @"yes" : @"-", pass.depthAttachment.texture ? @"yes" : @"-");
	_viewport = (MTLViewport){ 0, 0, (double)first.width, (double)first.height, 0, 1 };
	_scissor = (MTLScissorRect){ 0, 0, first.width, first.height };
	MTLRenderPassDepthAttachmentDescriptor *z = pass.depthAttachment;
	if ([(id)z.texture isKindOfClass:[RDNA4MTLTexture class]] && ((RDNA4MTLTexture *)z.texture).isDepth) {
		_depth = (RDNA4MTLTexture *)z.texture;
		RDNA4MTLTexture *t = _depth;
		const double clear = z.clearDepth;
		if (z.loadAction == MTLLoadActionClear)
			[commandBuffer record:^NSError * { return [t clearDepthTo:clear]; }];
	}
	return self;
}

- (void)setDepthStencilState:(id<MTLDepthStencilState>)state
{
	_depthState = [(id)state isKindOfClass:[RDNA4MTLDepthStencilState class]] ? (RDNA4MTLDepthStencilState *)state : nil;
}

- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { if (i < kStageTextures) _textures[0][i] = t; }
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { if (i < kStageTextures) _textures[1][i] = t; }
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { if (i < kStageTextures) _samplers[0][i] = s; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { if (i < kStageTextures) _samplers[1][i] = s; }

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

- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self bind:0 buffer:b offset:o index:i]; }
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self bind:1 buffer:b offset:o index:i]; }

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
	for (int st = 0; st < 2; st++) {
		for (int i = 0; i < kTableBuffers; i++)
			if (_bound[st][i])
				[held addObject:_bound[st][i]];
		for (int i = 0; i < kStageTextures; i++) {
			if ([(id)_textures[st][i] isKindOfClass:[RDNA4MTLTexture class]] && !((RDNA4MTLTexture *)_textures[st][i]).isDepth) {
				RDNA4MTLTexture *t = (RDNA4MTLTexture *)_textures[st][i];
				[t descriptor:d.textures[st][i]];
				for (NSUInteger l = 1; levelT && l < 8; l++)
					[t descriptor:(uint32_t *)((uint8_t *)levelT.mutableBytes + ((st * 7 + (l - 1)) * 16 + i) * 32) level:l];
				if (t.shadowed)
					[shadowed addObject:t];
				[held addObject:t];
				[reads addObject:@[ @(st), @(i), t ]];
			}
			if ([(id)_samplers[st][i] isKindOfClass:[RDNA4MTLSamplerState class]])
				memcpy(d.samplers[st][i], ((RDNA4MTLSamplerState *)_samplers[st][i]).words, 16);
		}
	}
	if (indexBuffer) {
		d.indexGpu = indexBuffer.gpuAddress + indexOffset;
		d.indexBytes = indexType == MTLIndexTypeUInt32 ? 4 : 2;
		[held addObject:indexBuffer];
	}
	if (_depth && _depthState) {
		d.depthGpu = _depth.gpuBase;
		d.depthWidth = (uint32_t)_depth.width;
		d.depthHeight = (uint32_t)_depth.height;
		d.depthCompare = _depthState.compare;
		d.depthWrite = _depthState.write;
		[held addObject:_depth];
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
	d.levelT = (const uint8_t *)levelT.bytes;
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
	[_commandBuffer recordDraw:^NSError * {
		(void)held;
		(void)levelT;
		for (RDNA4MTLTexture *t in shadowed)
			[t refresh];
		// As the PV host: a texture an earlier draw still in flight draws into is read after those draws (its barrier), and the draw's own
		// target, read as a texture (Core Animation's backdrops), is read from a copy made now (the PV host's staged copy); read in place
		// while drawn, WindowServer's first native frames hung the gfx ring (2026-11-20).
		RDNA4Draw dd = d;
		NSError *why = nil;
		for (NSArray *r in reads) {
			RDNA4MTLTexture *t = r[2];
			if ([device drawnInFlight:t] && (why = [device drainGfx]))
				return why;
			if (t == target) {
				RDNA4MTLTexture *copy = [target copyForReading];
				if (copy)
					[copy descriptor:dd.textures[[r[0] intValue]][[r[1] intValue]]];
			}
		}
		if (!pipeline || !target)
			return rdna4Error(@"a draw without a render pipeline or a colour attachment of this device", 0);

		why = [device draw:&dd pipeline:pipeline target:target];
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

- (void)endEncoding { }

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
		return [(RDNA4MTLAccelerationStructure *)as build:(MTLPrimitiveAccelerationStructureDescriptor *)d];
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
	return [(id)t isKindOfClass:[RDNA4MTLTexture class]] && !((RDNA4MTLTexture *)t).isDepth ? nil
	                                                                                         : rdna4Error(@"a blit of a texture that is not a colour texture of this device", 0);
}

- (void)copyFromBuffer:(id<MTLBuffer>)from sourceOffset:(NSUInteger)fromOffset toBuffer:(id<MTLBuffer>)to
     destinationOffset:(NSUInteger)toOffset size:(NSUInteger)size
{
	[_commandBuffer record:^NSError * {
		if (fromOffset + size > from.length || toOffset + size > to.length)
			return rdna4Error(@"a buffer copy past a buffer's end", 0);
		memmove((uint8_t *)to.contents + toOffset, (const uint8_t *)from.contents + fromOffset, size);
		return nil;
	}];
}

- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value
{
	[_commandBuffer record:^NSError * {
		if (NSMaxRange(range) > buffer.length)
			return rdna4Error(@"a buffer fill past its end", 0);
		memset((uint8_t *)buffer.contents + range.location, value, range.length);
		return nil;
	}];
}

- (void)copyFromBuffer:(id<MTLBuffer>)from sourceOffset:(NSUInteger)offset sourceBytesPerRow:(NSUInteger)bpr
   sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)to destinationSlice:(NSUInteger)slice
      destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)origin
{
	[_commandBuffer record:^NSError * {
		NSError *why = colourOnly(to);
		if (!why && offset + bpr * (size.height - 1) + 4 * size.width > from.length)
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
		if (!why && offset + bpr * (size.height - 1) + 4 * size.width > to.length)
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

@implementation RDNA4MTLCommandBuffer {
	RDNA4MTLCommandQueue *_queue;
	NSMutableArray<RDNA4Op> *_ops;
	NSMutableIndexSet *_draws;   // which of _ops are draws
	NSMutableArray<MTLCommandBufferHandler> *_completed, *_scheduled;
	NSMutableArray<id<MTLDrawable>> *_drawables;   // presented once the work has run
	dispatch_group_t _scheduledGroup, _done;
	NSMutableDictionary *_userDictionary;
	id<MTLBuffer> _chunk;   // set*Bytes data (stage:)
	NSUInteger _chunkUsed;
	_Atomic MTLCommandBufferStatus _status;
	NSError *_error;
}
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
		_scheduledGroup = dispatch_group_create();
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
- (void)recordDraw:(RDNA4Op)op
{
	[_draws addIndex:_ops.count];
	[_ops addObject:op];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)block { [_completed addObject:block]; }
- (void)addScheduledHandler:(MTLCommandBufferHandler)block { [_scheduled addObject:block]; }
- (void)enqueue { if (_status < MTLCommandBufferStatusEnqueued) _status = MTLCommandBufferStatusEnqueued; }
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
- (void)commit
{
	if (_status >= MTLCommandBufferStatusCommitted)
		return;
	_status = MTLCommandBufferStatusCommitted;
	dispatch_group_enter(_scheduledGroup);
	dispatch_group_enter(_done);
	RDNA4MTLDevice *device = (RDNA4MTLDevice *)self.device;
	dispatch_async(device.work, ^{
		self->_status = MTLCommandBufferStatusScheduled;
		dispatch_group_leave(self->_scheduledGroup);
		dispatch_queue_t handlers = self->_queue.handlers;
		NSArray<MTLCommandBufferHandler> *scheduled = [self->_scheduled copy];
		if (scheduled.count)
			dispatch_async(handlers, ^{
				for (MTLCommandBufferHandler h in scheduled)
					h(self);
			});
		NSError *why = nil;
		for (NSUInteger i = 0; i < self->_ops.count && !why; i++) {
			if (![self->_draws containsIndex:i])
				why = [device drainGfx];
			if (!why)
				why = self->_ops[i]();
		}
		NSError *drained = [device drainGfx];
		self->_error = why ?: drained;
		RTRACE(60, @"command buffer %@: %lu steps, %lu draws, %lu drawables%@%@", self.label ?: @"-", (unsigned long)self->_ops.count,
		       (unsigned long)self->_draws.count, (unsigned long)self->_drawables.count, self->_error ? @": " : @"",
		       self->_error.localizedDescription ?: @"");
		[self->_ops removeAllObjects];
		for (id<MTLDrawable> d in self->_drawables)
			[d present];
		[self->_drawables removeAllObjects];
		self->_status = self->_error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
		// the handlers on the queue's handler queue, in order (one that waits for the app's main thread must not stop the GPU work behind
		// it), then waitUntilCompleted returns, as Metal's does
		NSArray<MTLCommandBufferHandler> *completed = [self->_completed copy];
		[self->_completed removeAllObjects];
		dispatch_async(handlers, ^{
			for (MTLCommandBufferHandler h in completed)
				h(self);
			dispatch_group_leave(self->_done);
		});
	});
}

RDNA4_ANSWER_MISSING
@end

@implementation RDNA4MTLCommandQueue {
	RDNA4MTLDevice *_device;
	dispatch_queue_t _own, _app;
}
@synthesize label;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device
{
	if ((self = [super init])) {
		_device = device;
		_own = dispatch_queue_create("RDNA4MTL handlers", DISPATCH_QUEUE_SERIAL);
	}
	return self;
}

- (dispatch_queue_t)handlers { return _app ?: _own; }
- (void)setCompletionQueue:(dispatch_queue_t)queue { _app = queue; }
- (void)setSubmissionQueue:(dispatch_queue_t)queue { }
- (void)setGPUPriority:(NSUInteger)priority { }

- (id<MTLDevice>)device { return _device; }
- (id<MTLCommandBuffer>)commandBuffer { return [[RDNA4MTLCommandBuffer alloc] initWithQueue:self]; }
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }

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
enum { kMiscBytes = 0x5000, kMiscTables = 0x1000 };
enum { kSlots = 16 };           // draws in flight: each has its own IB page and state page until its fence has come

@implementation RDNA4MTLDevice {
	io_service_t _service;
	uint64_t _registryID;
	__weak id _accelService;   // Metal's MTLIOAccelService for our service
	rdna4_t _gpu;
	BOOL _open;
	dispatch_queue_t _work;   // runs committed command buffers, in commit order (every queue's)
	// gfx: made by the first render pipeline; _misc and _ib hold kSlots state pages and IB pages, used in turn by the draws
	BOOL _gfxReady;
	rdna4_buffer_t _rings, _spare, _misc, _ib;
	uint64_t _slotFence[kSlots], _lastFence;   // a slot's draw's fence, while it may still run; the last draw's
	NSString *_slotDraw[kSlots];               // what each slot's draw is (pipeline, target): a hang names the draws in flight
	NSHashTable *_drawnInFlight;               // the targets of the draws since the last drainGfx (work queue only)
	uint32_t _slot, _lastSlot;
}

+ (void)load
{
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");   // Metal's private protocol, also asserted by MTLAddDevice
	if (spi)
		class_addProtocol(self, spi);
}

- (instancetype)initWithAcceleratorPort:(io_service_t)service
{
	if (!getenv("RDNA4_NATIVE_MTL")) {
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
	IOObjectRetain(service);
	_service = service;
	IORegistryEntryGetRegistryEntryID(service, &_registryID);
	const kern_return_t kr = rdna4_open(&_gpu);
	_open = kr == KERN_SUCCESS;
	_work = dispatch_queue_create("RDNA4MTL work", DISPATCH_QUEUE_SERIAL);
	RLOG(@"RDNA4MTLDevice: for service 0x%x (registry ID 0x%llx): the kext's runtime client: %s", service, _registryID,
	      _open ? "open" : rdna4_error(kr));
	return _open ? self : nil;   // no device without the GPU (the router then makes Apple's)
}

- (void)dealloc
{
	rdna4_buffer_t *gfx[4] = { &_rings, &_spare, &_misc, &_ib };
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
	    (kr = rdna4_write(&_gpu, &_spare, 0, zeros, sizeof zeros)) || (kr = rdna4_alloc(&_gpu, kSlots * kMiscBytes, &_misc)) ||
	    (kr = rdna4_alloc(&_gpu, kSlots * RDNA4_TRIRUN_PAGE, &_ib))) {
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

- (BOOL)drawnInFlight:(RDNA4MTLTexture *)t { return [_drawnInFlight containsObject:t]; }

// As the PV host's stageTextures: an image instruction on an all-zero T# can stall the texture unit (an unbound slot hung WindowServer's
// first native draws and with them the gfx ring, 2026-11-20).
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

- (uint64_t)spareBuffer
{
	uint32_t t[8];
	[self spareImage:t];   // makes it
	return _spare.gpu;
}
- (dispatch_queue_t)work { return _work; }

// Waits for the draws submitted so far (on the work queue: before the CPU or the compute queue touches what they wrote, and at the end of
// a command buffer). The gfx fences are ordered, so the last one is enough; its stream's own fence word says the stream ran to its end.
- (NSError *)drainGfx
{
	if (!_lastFence)
		return nil;
	[_drawnInFlight removeAllObjects];
	const uint64_t fence = _lastFence;
	uint32_t streamFence = 0;
	NSMutableArray *inFlight = [NSMutableArray array];
	for (uint32_t i = 0; i < kSlots; i++)
		if (_slotFence[i] && _slotDraw[i])
			[inFlight addObject:[NSString stringWithFormat:@"%llu %@", _slotFence[i], _slotDraw[i]]];
	_lastFence = 0;
	memset(_slotFence, 0, sizeof _slotFence);
	kern_return_t kr = rdna4_wait_gfx_fence(&_gpu, fence, 2000, NULL);
	if (!kr)
		kr = rdna4_read(&_gpu, &_misc, (uint64_t)_lastSlot * kMiscBytes, &streamFence, 4);
	if (kr)
		return rdna4Error([NSString stringWithFormat:@"waiting for the draws (fence %llu; in flight: %@)", fence,
		                                             [inFlight componentsJoinedByString:@"; "]], kr);
	if (streamFence != 1)
		return rdna4Error([NSString stringWithFormat:@"the draws finished but the last stream's fence reads %u", streamFence], 0);
	return nil;
}

// One draw, submitted (not waited for: drainGfx): the PV host's runDraw for a single-sampled target of a colourInfo format, blended or not.
- (NSError *)draw:(const RDNA4Draw *)d pipeline:(RDNA4MTLRenderPipeline *)p target:(RDNA4MTLTexture *)t
{
	if (d->type > 4 || !d->count || !d->instances || d->count > (1u << 24))
		return rdna4Error([NSString stringWithFormat:@"a draw of %u vertices, type %u, %u instances: not run", d->count, d->type, d->instances], 0);
	if ([p formatOf:d->attachment] != t.pixelFormat)
		return rdna4Error([NSString stringWithFormat:@"the pipeline's colour attachment %u format %lu is not the target's %lu", d->attachment,
		                                             (unsigned long)[p formatOf:d->attachment], (unsigned long)t.pixelFormat], 0);
	static uint32_t ib[RDNA4_TRIRUN_PAGE / 4];   // built here and copied into the slot's pages: draws are made on the work queue only
	static uint64_t misc[kMiscBytes / 8];
	const uint32_t slot = _slot++ % kSlots;
	kern_return_t kr = _slotFence[slot] ? rdna4_wait_gfx_fence(&_gpu, _slotFence[slot], 2000, NULL) : KERN_SUCCESS;
	if (kr)
		return rdna4Error([NSString stringWithFormat:@"waiting for slot %u's earlier draw", slot], kr);
	_slotFence[slot] = 0;
	const uint64_t miscGpu = _misc.gpu + (uint64_t)slot * kMiscBytes, ibOffset = (uint64_t)slot * RDNA4_TRIRUN_PAGE;
	const uint32_t *vs = p.vs, *ps = p.ps, outs = p.outs, ins = p.ins;
	const BOOL noColour = (ps[7] >> 31) || p.exportsNothing;
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
	// As the PV host's patch table, single-sampled.
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
		{ 0x69, 0x1d8, blend[2] ? first.opt : 0x06000600u },             // SX_MRT0_BLEND_OPT: else BLEND_DISABLED
		{ 0x69, 0x214, noColour ? 0u : split ? mask & 7 : mask },         // CB_TARGET_MASK
		{ 0x69, 0x195, noColour ? 0u : 9u },                             // SPI_SHADER_COL_FORMAT: 32_ABGR
		{ 0x69, 0x1b, noColour ? 0x600u : 0x10u },                       // DB_SHADER_CONTROL
		{ 0x69, 0x31b, 0 }, { 0x69, 0x31f, 0x01000000u }, { 0x69, 0x2f8, 0 }, { 0x69, 0x292, 0x22u }, { 0x69, 0x1e, 0x00110000u },   // one sample
		{ 0x69, 0x2fc, 0 }, { 0x69, 0x2fd, 0 }, { 0x69, 0x2fe, 0 }, { 0x69, 0x302, 0 }, { 0x69, 0x306, 0 }, { 0x69, 0x30a, 0 }, { 0x69, 0x317, 0 },
	};
	for (size_t i = 0; i < sizeof patch / sizeof patch[0]; i++) {
		uint32_t *at = streamReg(ib, n, patch[i].opcode, patch[i].reg);
		if (!at)
			return rdna4Error([NSString stringWithFormat:@"the stream lacks register 0x%x", patch[i].reg], 0);
		*at = patch[i].value;
	}
	// The stages' tables in the state page (unbound buffers: a zeroed spare, as the PV host), the fence word at +0 zeroed.
	const BOOL levels = (vs[7] | ps[7]) >> 16 & 1;   // a stage samples at explicit LODs: levels 1-7 of each texture (one level: level 0's T#)
	const size_t miscBytes = levels ? kMiscBytes : kMiscTables;
	memset(misc, 0, miscBytes);
	for (unsigned st = 0; st < 2; st++)
		for (unsigned k = 0; k < kTableBuffers; k++) {
			const uint32_t at = 0x400 + 0x400 * st + (k < 8 ? 8 * k : RDNA4_MTLC_TABLE_MORE_BUFFERS + 8 * (k - 8));
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
	for (unsigned st = 0; st < 2; st++)   // the T#s at +64 + 32 n, the S#s at +576 + 16 n of the stage's table (include/rdna4mtlc.h)
		for (unsigned k = 0; k < kStageTextures; k++) {
			if (!memcmp(d->textures[st][k], none, 32)) {   // unbound: the spare texel (here and at every level)
				memcpy((uint8_t *)misc + 0x400 + 0x400 * st + RDNA4_MTLC_TABLE_TEXTURES + 32 * k, spare, 32);
				memcpy((uint8_t *)misc + 0x400 + 0x400 * st + RDNA4_MTLC_TABLE_SAMPLERS + 16 * k, d->samplers[st][k], 16);
				continue;
			}
			memcpy((uint8_t *)misc + 0x400 + 0x400 * st + RDNA4_MTLC_TABLE_TEXTURES + 32 * k, d->textures[st][k], 32);
			memcpy((uint8_t *)misc + 0x400 + 0x400 * st + RDNA4_MTLC_TABLE_SAMPLERS + 16 * k, d->samplers[st][k], 16);
		}
	// The draw in place of the template's DRAW_INDEX_AUTO: blend colour, the fragment inputs, the stages' tables (USER_DATA_GS_0/1, _PS_0/1),
	// the first vertex and base instance (USER_DATA_GS_2/3), NUM_INSTANCES, DRAW_INDEX_AUTO.
	uint32_t seq[256], m = 0;
#define PUT(x) (seq[m++] = (x))
	if (d->depthGpu) {   // the depth test, as the PV host's (src/pvgpu.cpp runDraw): Z_32_FLOAT, SW_64KB_2D, Metal's 0..1 depth range
		PUT(0xc0026900u), PUT(0x001), PUT(0), PUT(0);   // DB_DEPTH_VIEW, DB_DEPTH_VIEW1: slice 0, mip 0
		PUT(0xc00b6900u), PUT(0x005);                   // DB_DEPTH_SIZE_XY, DB_Z_INFO, DB_STENCIL_INFO, Z read/write and stencil bases
		PUT((d->depthWidth - 1) | (d->depthHeight - 1) << 16);
		PUT(3 | 3u << 4);                              // Z_32_FLOAT, SW_64KB_2D, one sample
		PUT(0);                                         // STENCIL_INVALID
		for (int i = 0; i < 4; i++) {
			const uint64_t base = i < 2 ? d->depthGpu : 0;
			PUT((uint32_t)(base >> 8)), PUT((uint32_t)(base >> 40));
		}
		PUT(0xc0026900u), PUT(0x01c);                   // DB_DEPTH_CONTROL: Z_ENABLE, Z_WRITE_ENABLE, ZFUNC; DB_STENCIL_CONTROL
		PUT(2u | (d->depthWrite ? 4u : 0u) | d->depthCompare << 4), PUT(0);
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
	uint64_t fence = 0;
	for (unsigned st = 0; levels && st < 2; st++)   // each level's own image (a texture of fewer levels: its last), unbound ones the spare texel
		for (uint32_t level = 1; level < RDNA4_MTLC_LEVELS; level++) {
			uint8_t *at = (uint8_t *)misc + kMiscTables + RDNA4_MTLC_LEVEL_STRIDE * (level - 1) + 0x400 * st;
			memcpy(at, d->levelT ? (const void *)(d->levelT + (st * 7 + (level - 1)) * 16 * 32) : (const void *)d->textures[st], sizeof d->textures[st]);
			for (unsigned k = 0; k < kStageTextures; k++)
				if (!memcmp(at + 32 * k, none, 32))
					memcpy(at + 32 * k, spare, 32);
		}
	kr = rdna4_write(&_gpu, &_misc, (uint64_t)slot * kMiscBytes, misc, miscBytes);
	if (!kr)
		kr = rdna4_write(&_gpu, &_ib, ibOffset, ib, total * 4);
	if (!kr)
		kr = rdna4_submit_gfx_ib(&_gpu, &_ib, ibOffset, total, &fence);
	if (kr)
		return rdna4Error([NSString stringWithFormat:@"a draw of %u vertices (%u dwords)", d->count, total], kr);
	_slotFence[slot] = _lastFence = fence;
	_lastSlot = slot;
	if (!_drawnInFlight)
		_drawnInFlight = [NSHashTable weakObjectsHashTable];
	[_drawnInFlight addObject:t];
	_slotDraw[slot] = [NSString stringWithFormat:@"%@ (%u vertices x %u, %lux%lu format %lu)", p.names, d->count, d->instances,
	                                             (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat];
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

- (NSString *)name { return @"AMD Radeon RDNA4 (native Metal, spike)"; }
- (uint64_t)registryID { return _registryID; }
- (BOOL)hasUnifiedMemory { return NO; }
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
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count { return count == 1; }

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
- (BOOL)supportsFamily:(MTLGPUFamily)family { return family == 2001 || family == 3001 || family == 3002; }   // Mac1, Common1, Common2: Apple's device's
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
- (NSUInteger)maxThreadgroupMemoryLength { return 32768; }

- (MTLAccelerationStructureSizes)accelerationStructureSizesWithDescriptor:(MTLAccelerationStructureDescriptor *)d
{
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
	return [[RDNA4MTLTexture alloc] initWithDevice:self descriptor:descriptor iosurface:surface plane:plane];
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
	if (descriptor.pixelFormat == MTLPixelFormatDepth32Float && descriptor.textureType == MTLTextureType2D && descriptor.sampleCount == 1 &&
	    descriptor.width && descriptor.height && descriptor.width <= 16384 && descriptor.height <= 16384)
		return [[RDNA4MTLTexture alloc] initDepthWithDevice:self descriptor:descriptor];
	return [[RDNA4MTLTexture alloc] initWithDevice:self descriptor:descriptor];
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

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
	NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:
	                                             [NSString stringWithFormat:@"rdna4mtl-%d-%@.metallib", getpid(), NSUUID.UUID.UUIDString]];
	if (![(NSData *)data writeToFile:path atomically:NO]) {
		if (error)
			*error = rdna4Error([NSString stringWithFormat:@"cannot write %@", path], 0);
		return nil;
	}
	id<MTLLibrary> lib = [super newLibraryWithData:data error:error];
	if (lib)
		return lib;
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

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                                                           options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
	if (reflection)
		*reflection = nil;
	return [self newRenderPipelineStateWithDescriptor:descriptor error:error];
}

- (id<MTLCommandQueue>)newCommandQueue
{
	return [[RDNA4MTLCommandQueue alloc] initWithDevice:self];
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count
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
