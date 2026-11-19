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
 *   - command buffers run their dispatches on commit, one rdna4_dispatch each, in order, on the committing thread.
 * Gate 3: a render pass. Textures are linear 2D RGBA8 / BGRA8 in shared memory; a render pipeline's vertex and fragment functions are
 * translated into RGFX blobs (air2amdgpu.cpp translateShader); each draw is the PV host's (src/pvgpu.cpp runDraw), cut down: the proven
 * triangle stream (userspace/rdna4-run/gfx12tri.h, gfx12tricol.h with values between the stages) with the shaders' registers, target,
 * viewport and draw patched in, the stages' buffer tables in a state page, submitted as the client's own gfx IB (rdna4_submit_gfx_ib, a
 * boot with rdna4-gfx=2). A clear load action is done by the CPU (shared memory).
 * ponytail: synchronous commit, one draw at a time, no blending, depth, textures in shaders, MSAA or indexed draws; one host allocation
 * per set*Bytes. A real queue (IBs batched, fences, completion on a thread) when a gate needs overlap.
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

#include "librdna4.h"
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
		NSLog(@"RDNA4MTL: %@", name);
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
@end

@interface RDNA4MTLDevice : _MTLDevice <MTLDevice>   // MTLAddDevice asserts conformsToProtocol:@protocol(MTLDevice)
- (instancetype)initWithAcceleratorPort:(io_service_t)service;
- (rdna4_t *)gpu;   // nil until the kext's runtime client is open
@end

static NSError *rdna4Error(NSString *what, kern_return_t kr)
{
	NSString *text = kr ? [NSString stringWithFormat:@"%@: %s (0x%x)", what, rdna4_error(kr), kr] : what;
	NSLog(@"RDNA4MTL: %@", text);
	return [NSError errorWithDomain:@"RDNA4MTL" code:kr userInfo:@{ NSLocalizedDescriptionKey : text }];
}

// ---- buffers

@interface RDNA4MTLBuffer : NSObject <MTLBuffer>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device length:(NSUInteger)length options:(MTLResourceOptions)options;
// The caller's own page-aligned memory (newBufferWithBytesNoCopy:, an IOSurface's), mapped into the GPU (rdna4_import_host), not copied.
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device bytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options
                   deallocator:(void (^)(void *, NSUInteger))deallocator;
@end

@implementation RDNA4MTLBuffer {
	RDNA4MTLDevice *_device;
	rdna4_buffer_t _buf;
	void *_cpu;
	NSUInteger _length;
	MTLResourceOptions _options;
	void (^_deallocator)(void *, NSUInteger);   // an imported buffer's owner's
}
@synthesize label;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device bytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options
                   deallocator:(void (^)(void *, NSUInteger))deallocator
{
	if (!(self = [super init]) || !device.gpu)
		return nil;
	const uint64_t bytes = ((length ?: 1) + 4095) & ~4095ull;
	const kern_return_t kr = ((uintptr_t)pointer & 4095) ? kIOReturnBadArgument : rdna4_import_host(device.gpu, pointer, bytes, 0, &_buf);
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

static BOOL isColourFormat(MTLPixelFormat f)   // what a target may be so far: RGBA8 and BGRA8, unorm or sRGB
{
	return f == MTLPixelFormatRGBA8Unorm || f == MTLPixelFormatRGBA8Unorm_sRGB || f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB;
}

static BOOL isBgra(MTLPixelFormat f) { return f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB; }

// A linear 2D image's T# (src/pvdraw.hpp linearImage, as ac_build_gfx12_texture_descriptor: one mip level, a custom pitch): the gfx12
// format and DST_SEL of an RGBA8 / BGRA8 texture (src/pvgpu.cpp textureFormat), width, height, pitch in pixels.
static void linearImage(uint32_t t[8], uint64_t gpu, MTLPixelFormat f, uint32_t w, uint32_t h, uint32_t pitchPx)
{
	const BOOL srgb = f == MTLPixelFormatRGBA8Unorm_sRGB || f == MTLPixelFormatBGRA8Unorm_sRGB;
	const uint32_t format = srgb ? 66 : 42, sel = isBgra(f) ? 6 | 5 << 3 | 4 << 6 | 7 << 9 : 4 | 5 << 3 | 6 << 6 | 7 << 9;
	t[0] = (uint32_t)(gpu >> 8);
	t[1] = (uint32_t)((gpu >> 40) & 0xff) | format << 17 | ((w - 1) & 3) << 30;   // MAX_MIP 0, FORMAT, BASE_LEVEL 0, WIDTH_LO
	t[2] = (w - 1) >> 2 | (h - 1) << 14;                                          // WIDTH_HI, HEIGHT
	t[3] = sel | 9u << 28;                                                        // DST_SEL, LAST_LEVEL 0, SW_MODE linear, TYPE 2D
	t[4] = ((pitchPx - 1) & 0x3fff) | ((pitchPx - 1) >> 14 & 3) << 14;             // DEPTH = pitch - 1 low, PITCH_MSB
	t[5] = 4u << 20;                                                              // PERF_MOD 4
	t[6] = 1u << 15;                                                              // MAX_UNCOMPRESSED_BLOCK_SIZE 256 B
	t[7] = 0;
}

@interface RDNA4MTLTexture : NSObject <MTLTexture>
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor;
// Over an IOSurface (CAMetalLayer's drawables): its memory mapped into the GPU, its rows as the surface has them.
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)descriptor iosurface:(IOSurfaceRef)surface
                         plane:(NSUInteger)plane;
- (void)clearTo:(MTLClearColor)colour;
- (NSError *)clearDepthTo:(double)depth;   // a Depth32Float texture: every pixel (the tiling does not matter for one value)
- (void)descriptor:(uint32_t[8])t;          // its T# for shaders
@property (readonly) BOOL isDepth;        // Depth32Float in VRAM, 64KB_2D (as the PV host's depth buffers), for the depth block only
@property (readonly) uint64_t gpuBase;    // the first row's GPU address (page aligned)
@property (readonly) NSUInteger rowBytes; // a multiple of 256
@end

@implementation RDNA4MTLTexture {
	RDNA4MTLDevice *_device;
	RDNA4MTLBuffer *_memory;
	NSUInteger _width, _height;
	MTLPixelFormat _format;
	MTLTextureUsage _usage;
	IOSurfaceRef _surface;
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
		float *p = pattern.mutableBytes;
		for (NSUInteger i = 0; i < _vram.bytes / 4; i++)
			p[i] = value;
		patternValue = value;
	}
	const kern_return_t kr = rdna4_write(_device.gpu, &_vram, 0, pattern.bytes, _vram.bytes);
	return kr ? rdna4Error(@"a depth clear", kr) : nil;
}

- (void)descriptor:(uint32_t[8])t
{
	linearImage(t, self.gpuBase, _format, (uint32_t)_width, (uint32_t)_height, (uint32_t)(_rowBytes / 4));
}

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)surface plane:(NSUInteger)plane
{
	if (!(self = [super init]))
		return nil;
	const size_t bpr = IOSurfaceGetBytesPerRow(surface), bytes = IOSurfaceGetAllocSize(surface);
	void *base = IOSurfaceGetBaseAddress(surface);
	if (plane || IOSurfaceGetPlaneCount(surface) > 1 || !isColourFormat(d.pixelFormat) || IOSurfaceGetBytesPerElement(surface) != 4 ||
	    !d.width || !d.height || d.width > IOSurfaceGetWidth(surface) || d.height > IOSurfaceGetHeight(surface) || (bpr & 255)) {
		rdna4Error([NSString stringWithFormat:@"an IOSurface texture of format %lu, %lux%lu, %zu bytes a row (plane %lu): only one plane of "
		                                      @"RGBA8 / BGRA8, rows a multiple of 256 bytes, so far", (unsigned long)d.pixelFormat,
		                                      (unsigned long)d.width, (unsigned long)d.height, bpr, (unsigned long)plane], 0);
		return nil;
	}
	CFRetain(surface);
	_memory = [[RDNA4MTLBuffer alloc] initWithDevice:device bytesNoCopy:base length:bytes options:MTLResourceStorageModeShared
	                                     deallocator:^(void *p, NSUInteger n) { CFRelease(surface); }];
	if (!_memory)
		return nil;
	_surface = surface;
	_rowBytes = bpr;
	_device = device;
	_width = d.width;
	_height = d.height;
	_format = d.pixelFormat;
	_usage = d.usage;
	return self;
}

- (IOSurfaceRef)iosurface { return _surface; }
- (NSUInteger)iosurfacePlane { return 0; }

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device descriptor:(MTLTextureDescriptor *)d
{
	if (!(self = [super init]))
		return nil;
	if (d.textureType != MTLTextureType2D || d.mipmapLevelCount != 1 || d.sampleCount != 1 || !isColourFormat(d.pixelFormat) || !d.width ||
	    !d.height || d.width > 16384 || d.height > 16384) {
		rdna4Error([NSString stringWithFormat:@"a texture of type %lu, format %lu, %lux%lu, %lu levels, %lu samples: only 2D RGBA8 / BGRA8 with "
		                                      @"one level and one sample so far", (unsigned long)d.textureType, (unsigned long)d.pixelFormat,
		                                      (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.mipmapLevelCount,
		                                      (unsigned long)d.sampleCount], 0);
		return nil;
	}
	_rowBytes = (d.width * 4 + 255) & ~(NSUInteger)255;   // the colour block's pitch in whole 256-byte rows (the PV host: a multiple of 128)
	_memory = [[RDNA4MTLBuffer alloc] initWithDevice:device length:_rowBytes * d.height options:MTLResourceStorageModeShared];
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
- (MTLTextureType)textureType { return MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return _format; }
- (NSUInteger)width { return _width; }
- (NSUInteger)height { return _height; }
- (NSUInteger)depth { return 1; }
- (NSUInteger)mipmapLevelCount { return 1; }
- (NSUInteger)sampleCount { return 1; }
- (NSUInteger)arrayLength { return 1; }
- (MTLTextureUsage)usage { return _usage; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (NSUInteger)allocatedSize { return _memory.allocatedSize; }
- (BOOL)isFramebufferOnly { return NO; }
- (uint64_t)gpuBase { return _isDepth ? _vram.gpu : _memory.gpuAddress; }

- (void)dealloc
{
	if (_vram.handle)
		rdna4_free(_device.gpu, &_vram);
}

- (BOOL)fits:(MTLRegion)r level:(NSUInteger)level
{
	return !level && r.origin.x + r.size.width <= _width && r.origin.y + r.size.height <= _height;
}

- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr
{
	for (NSUInteger y = 0; [self fits:r level:level] && y < r.size.height; y++)
		memcpy((uint8_t *)_memory.contents + (r.origin.y + y) * _rowBytes + r.origin.x * 4, (const uint8_t *)bytes + y * bpr, r.size.width * 4);
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
	for (NSUInteger y = 0; [self fits:r level:level] && y < r.size.height; y++)
		memcpy((uint8_t *)bytes + y * bpr, (const uint8_t *)_memory.contents + (r.origin.y + y) * _rowBytes + r.origin.x * 4, r.size.width * 4);
}

// A clear load action: the colour packed as the format stores it. ponytail: an sRGB target gets the value unencoded.
- (void)clearTo:(MTLClearColor)c
{
	const double ch[4] = { isBgra(_format) ? c.blue : c.red, c.green, isBgra(_format) ? c.red : c.blue, c.alpha };
	uint32_t px = 0;
	for (int i = 0; i < 4; i++)
		px |= (uint32_t)lround(fmin(fmax(ch[i], 0), 1) * 255) << (8 * i);
	for (NSUInteger y = 0; y < _height; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)_memory.contents + y * _rowBytes);
		for (NSUInteger x = 0; x < _width; x++)
			row[x] = px;
	}
}

RDNA4_ANSWER_MISSING
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
	const RtTri *x = a, *y = b;
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
	uint32_t *head = _memory.contents;
	RtNode *nodes = (RtNode *)(head + 4);
	uint32_t used = 0;
	if (count)
		rtBuild(tris.mutableBytes, 0, count, nodes, &used);
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
	if (![task launchAndReturnError:&runError])
		return rdna4Error([NSString stringWithFormat:@"cannot run %@: %@", tool, runError.localizedDescription], 0);
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
@property (readonly) uint32_t outs, ins;        // values the vertex function writes, the fragment function reads
@property (readonly) const uint32_t *inputCntl; // SPI_PS_INPUT_CNTL_0 .. ins - 1
@property (readonly) BOOL sprite;               // a fragment input is the point's coordinate
@property (readonly) uint64_t code;             // GPU address: VS at +0, PS at +RDNA4_TRI_PS_OFFSET
@property (readonly) MTLPixelFormat format;     // colour attachment 0's
@end

// One draw as recorded: everything the device needs to run it.
enum { kStageTextures = 16 };   // a stage's table holds textures and samplers 0-15 (include/rdna4mtlc.h)

typedef struct {
	uint64_t table[2][kTableBuffers];   // per stage, its buffers' GPU addresses by index (0: unbound)
	uint32_t textures[2][kStageTextures][8], samplers[2][kStageTextures][4];   // per stage, T#s and S#s (0: unbound)
	uint32_t type, start, count, instances, baseInstance;
	float viewport[4];                  // x, y, width, height in pixels
	uint64_t indexGpu;                  // an indexed draw's indices (0: not indexed), 16- or 32-bit
	uint32_t indexBytes;
	uint64_t depthGpu;                  // the pass's Depth32Float attachment (0: none), its size, the state's test and write
	uint32_t depthWidth, depthHeight, depthCompare, depthWrite;
} RDNA4Draw;

@interface RDNA4MTLDevice ()
- (BOOL)gfx;   // the client may submit gfx IBs and the device's gfx buffers exist
- (uint64_t)gfxRings;
- (NSError *)draw:(const RDNA4Draw *)draw pipeline:(RDNA4MTLRenderPipeline *)pipeline target:(RDNA4MTLTexture *)target;
@end

@implementation RDNA4MTLRenderPipeline {
	RDNA4MTLDevice *_device;
	NSData *_blob[2];
	uint32_t _cntl[16];
	rdna4_buffer_t _codeBuffer;
}

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
	if (!isColourFormat(_format) || a.blendingEnabled)
		return rdna4Error([NSString stringWithFormat:@"colour attachment 0 of format %lu%s: only RGBA8 / BGRA8 without blending so far",
		                                             (unsigned long)_format, a.blendingEnabled ? ", blended" : ""], 0);
	for (int k = 0; k < 2; k++) {
		uint32_t unused;
		NSData *blob = nil;
		NSError *why = translateFunction(fn[k], &blob, &unused);
		if (why)
			return why;
		_blob[k] = blob;
	}
	const uint32_t *vs = _blob[0].bytes, *ps = _blob[1].bytes;
	const uint32_t room[2] = { RDNA4_TRI_PS_OFFSET - 4 * RDNA4_TRI_SHADER_PAD, RDNA4_TRI_CODE_BYTES - RDNA4_TRI_PS_OFFSET - 4 * RDNA4_TRI_SHADER_PAD };
	for (int k = 0; k < 2; k++) {
		const uint32_t *h = _blob[k].bytes;
		if (_blob[k].length < 4 * kHead || h[0] != kRgfxMagic || h[1] != (uint32_t)k + 1 || h[2] > room[k] || (h[7] & 0xffff) > 16 ||
		    4ull * kHead + h[2] + 8ull * (h[7] & 0xffff) > _blob[k].length)
			return rdna4Error([NSString stringWithFormat:@"%@: not a %s shader that fits its slot", fn[k].name, k ? "fragment" : "vertex"], 0);
		if (h[7] & 0x7fff0000u & ~(k ? 0x0ff00000u : 0u))   // textures' levels, other attachments: not yet
			return rdna4Error([NSString stringWithFormat:@"%@: attributes 0x%x not supported yet", fn[k].name, h[7]], 0);
	}
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
	uint32_t *code = image.mutableBytes;
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

- (const uint32_t *)vs { return _blob[0].bytes; }
- (const uint32_t *)ps { return _blob[1].bytes; }
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

typedef NSError * (^RDNA4Op)(void);   // a recorded step: commit runs them in order and stops at the first error

@interface RDNA4MTLCommandQueue : NSObject <MTLCommandQueue>
@end

@interface RDNA4MTLCommandBuffer : NSObject <MTLCommandBuffer>
- (instancetype)initWithQueue:(RDNA4MTLCommandQueue *)queue;
- (void)record:(RDNA4Op)op;
@end

@interface RDNA4MTLComputeEncoder : NSObject <MTLComputeCommandEncoder>
- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer;
@end

@implementation RDNA4MTLComputeEncoder {
	RDNA4MTLCommandBuffer *_commandBuffer;
	RDNA4MTLComputePipeline *_pipeline;
	uint64_t _addresses[kMaxBufferIndex];
	id<MTLBuffer> _bound[kMaxBufferIndex];
}
@synthesize label;

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
	id<MTLBuffer> copy = [self.device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];
	[self setBuffer:copy offset:0 atIndex:index];
}

- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)as atBufferIndex:(NSUInteger)index
{
	[self setBuffer:[(id)as isKindOfClass:[RDNA4MTLAccelerationStructure class]] ? ((RDNA4MTLAccelerationStructure *)as).memory : nil offset:0
	        atIndex:index];
}

- (void)record:(MTLSize)groups groupSize:(MTLSize)groupSize grid:(MTLSize)grid
{
	if (!_pipeline) {
		NSLog(@"RDNA4MTL: a dispatch without a compute pipeline, left out");
		return;
	}
	// The translator's kernargs: a buffer address per index, u32 threads per group x y z, u32 threads in the grid x y z, the texture table.
	const uint32_t n = _pipeline.buffers;
	NSMutableData *args = [NSMutableData dataWithLength:8 * n + 24 + 8];
	uint8_t *p = args.mutableBytes;
	memcpy(p, _addresses, 8 * MIN(n, (uint32_t)kMaxBufferIndex));
	const uint32_t sizes[6] = { (uint32_t)groupSize.width, (uint32_t)groupSize.height, (uint32_t)groupSize.depth,
		                        (uint32_t)grid.width,      (uint32_t)grid.height,      (uint32_t)grid.depth };
	memcpy(p + 8 * n, sizes, sizeof sizes);   // the texture table's address stays 0
	RDNA4MTLComputePipeline *pipeline = _pipeline;
	NSMutableArray *held = [NSMutableArray array];   // the bound buffers, kept until the dispatch ran
	for (int i = 0; i < kMaxBufferIndex; i++)
		if (_bound[i])
			[held addObject:_bound[i]];
	RDNA4MTLDevice *device = (RDNA4MTLDevice *)self.device;
	[_commandBuffer record:^NSError * {
		rdna4_program_t prog = pipeline.program;
		const uint32_t g[3] = { (uint32_t)groups.width, (uint32_t)groups.height, (uint32_t)groups.depth };
		const uint32_t s[3] = { (uint32_t)groupSize.width, (uint32_t)groupSize.height, (uint32_t)groupSize.depth };
		(void)held;
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
	RDNA4MTLTexture *_target;
	RDNA4MTLRenderPipeline *_pipeline;
	uint64_t _addresses[2][kTableBuffers];
	id<MTLBuffer> _bound[2][kTableBuffers];
	id<MTLTexture> _textures[2][kStageTextures];
	id<MTLSamplerState> _samplers[2][kStageTextures];
	RDNA4MTLTexture *_depth;
	RDNA4MTLDepthStencilState *_depthState;
	MTLViewport _viewport;
}
@synthesize label;

- (instancetype)initWithCommandBuffer:(RDNA4MTLCommandBuffer *)commandBuffer pass:(MTLRenderPassDescriptor *)pass
{
	if (!(self = [super init]))
		return nil;
	_commandBuffer = commandBuffer;
	MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[0];
	if ([(id)a.texture isKindOfClass:[RDNA4MTLTexture class]])
		_target = (RDNA4MTLTexture *)a.texture;
	else
		NSLog(@"RDNA4MTL: a render pass whose colour attachment 0 is not a texture of this device: its draws fail");
	_viewport = (MTLViewport){ 0, 0, _target.width, _target.height, 0, 1 };
	if (_target && a.loadAction == MTLLoadActionClear) {
		RDNA4MTLTexture *t = _target;
		const MTLClearColor c = a.clearColor;
		[commandBuffer record:^NSError * {
			[t clearTo:c];
			return nil;
		}];
	}
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
		NSLog(@"RDNA4MTL: buffer index %lu: only 0-%d so far", (unsigned long)index, kTableBuffers - 1);
		return;
	}
	_bound[stage][index] = buffer;
	_addresses[stage][index] = buffer ? buffer.gpuAddress + offset : 0;
}

- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self bind:0 buffer:b offset:o index:i]; }
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self bind:1 buffer:b offset:o index:i]; }

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	[self bind:0 buffer:[self.device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared] offset:0 index:index];
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	[self bind:1 buffer:[self.device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared] offset:0 index:index];
}

- (void)setViewport:(MTLViewport)viewport { _viewport = viewport; }

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
		            .viewport = { (float)_viewport.originX, (float)_viewport.originY, (float)_viewport.width, (float)_viewport.height } };
	memcpy(d.table, _addresses, sizeof d.table);
	NSMutableArray *held = [NSMutableArray array];   // what the draw uses, kept until it ran
	for (int st = 0; st < 2; st++) {
		for (int i = 0; i < kTableBuffers; i++)
			if (_bound[st][i])
				[held addObject:_bound[st][i]];
		for (int i = 0; i < kStageTextures; i++) {
			if ([(id)_textures[st][i] isKindOfClass:[RDNA4MTLTexture class]] && !((RDNA4MTLTexture *)_textures[st][i]).isDepth) {
				[(RDNA4MTLTexture *)_textures[st][i] descriptor:d.textures[st][i]];
				[held addObject:_textures[st][i]];
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
	RDNA4MTLRenderPipeline *pipeline = _pipeline;
	RDNA4MTLTexture *target = _target;
	RDNA4MTLDevice *device = (RDNA4MTLDevice *)self.device;
	[_commandBuffer record:^NSError * {
		(void)held;
		if (!pipeline || !target)
			return rdna4Error(@"a draw without a render pipeline or a colour attachment of this device", 0);
		return [device draw:&d pipeline:pipeline target:target];
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

@implementation RDNA4MTLCommandBuffer {
	RDNA4MTLCommandQueue *_queue;
	NSMutableArray<RDNA4Op> *_ops;
	NSMutableArray<MTLCommandBufferHandler> *_completed, *_scheduled;
	NSMutableArray<id<MTLDrawable>> *_drawables;   // presented once the commit has run
	MTLCommandBufferStatus _status;
	NSError *_error;
}
@synthesize label;

- (instancetype)initWithQueue:(RDNA4MTLCommandQueue *)queue
{
	if ((self = [super init])) {
		_queue = queue;
		_ops = [NSMutableArray array];
		_completed = [NSMutableArray array];
		_scheduled = [NSMutableArray array];
		_drawables = [NSMutableArray array];
	}
	return self;
}

- (id<MTLDevice>)device { return _queue.device; }
- (void)presentDrawable:(id<MTLDrawable>)drawable { [_drawables addObject:drawable]; }
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time { [_drawables addObject:drawable]; }
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (MTLCommandBufferStatus)status { return _status; }
- (NSError *)error { return _error; }
- (BOOL)retainedReferences { return YES; }
- (void)record:(RDNA4Op)op { [_ops addObject:op]; }
- (void)addCompletedHandler:(MTLCommandBufferHandler)block { [_completed addObject:block]; }
- (void)addScheduledHandler:(MTLCommandBufferHandler)block { [_scheduled addObject:block]; }
- (void)enqueue { _status = MAX(_status, MTLCommandBufferStatusEnqueued); }
- (void)waitUntilScheduled { }
- (void)waitUntilCompleted { }   // commit has run everything

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

- (void)commit
{
	_status = MTLCommandBufferStatusScheduled;
	for (MTLCommandBufferHandler h in _scheduled)
		h(self);
	for (RDNA4Op op in _ops)
		if ((_error = op()))
			break;
	[_ops removeAllObjects];
	for (id<MTLDrawable> d in _drawables)   // the GPU's work for them is done (synchronous commit)
		[d present];
	[_drawables removeAllObjects];
	_status = _error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
	for (MTLCommandBufferHandler h in _completed)
		h(self);
}

RDNA4_ANSWER_MISSING
@end

@implementation RDNA4MTLCommandQueue {
	RDNA4MTLDevice *_device;
}
@synthesize label;

- (instancetype)initWithDevice:(RDNA4MTLDevice *)device
{
	if ((self = [super init]))
		_device = device;
	return self;
}

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

// CB_COLOR0_INFO of an RGBA8 / BGRA8 target (src/pvdraw.hpp colourInfo): 8_8_8_8 unorm, COMP_SWAP ALT for BGRA8, NUMBER_SRGB for sRGB.
static uint32_t colourInfo(MTLPixelFormat f)
{
	return (isBgra(f) ? 0x0002880au : 0x0002800au) |
	       (f == MTLPixelFormatRGBA8Unorm_sRGB || f == MTLPixelFormatBGRA8Unorm_sRGB ? 6u << 8 : 0u);
}

enum { kMiscBytes = 0x2000 };   // the state page: the stream's fence at +0, the vertex stage's table at +0x400, the fragment stage's at +0x800

@implementation RDNA4MTLDevice {
	io_service_t _service;
	uint64_t _registryID;
	__weak id _accelService;   // Metal's MTLIOAccelService for our service
	rdna4_t _gpu;
	BOOL _open;
	// gfx: made by the first render pipeline; draws run one at a time (synchronous commit), so one state page and one IB serve them all
	BOOL _gfxReady;
	rdna4_buffer_t _rings, _spare, _misc, _ib;
}

+ (void)load
{
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");   // Metal's private protocol, also asserted by MTLAddDevice
	if (spi)
		class_addProtocol(self, spi);
}

- (instancetype)initWithAcceleratorPort:(io_service_t)service
{
	self = [super init];   // before any nil return: _MTLDevice's dealloc expects its init to have run
	if (!self || !getenv("RDNA4_NATIVE_MTL"))
		return nil;
	IOObjectRetain(service);
	_service = service;
	IORegistryEntryGetRegistryEntryID(service, &_registryID);
	const kern_return_t kr = rdna4_open(&_gpu);
	_open = kr == KERN_SUCCESS;
	NSLog(@"RDNA4MTLDevice: made for service 0x%x (registry ID 0x%llx); the kext's runtime client: %s", service, _registryID,
	      _open ? "open" : rdna4_error(kr));
	return self;
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
	if (kr || !(info.flags & RDNA4_FLAG_VM) || !(info.flags & RDNA4_FLAG_GFX)) {
		rdna4Error([NSString stringWithFormat:@"no client gfx IBs (flags 0x%llx: a private GPU VM and rdna4-gfx=2 needed)", info.flags], kr);
		return NO;
	}
	const uint64_t ringBytes = rdna4_tri_ring_bytes() + RDNA4_TRIRUN_RING_SLACK;
	static const uint8_t zeros[65536];
	if ((kr = rdna4_alloc(&_gpu, ringBytes, &_rings)) || (kr = rdna4_alloc(&_gpu, sizeof zeros, &_spare)) ||
	    (kr = rdna4_write(&_gpu, &_spare, 0, zeros, sizeof zeros)) || (kr = rdna4_alloc(&_gpu, kMiscBytes, &_misc)) ||
	    (kr = rdna4_alloc(&_gpu, RDNA4_TRIRUN_PAGE, &_ib))) {
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

// One draw: the PV host's runDraw for a single-sampled RGBA8 / BGRA8 target without blending, depth or textures, non-indexed.
- (NSError *)draw:(const RDNA4Draw *)d pipeline:(RDNA4MTLRenderPipeline *)p target:(RDNA4MTLTexture *)t
{
	if (d->type > 4 || !d->count || !d->instances || d->count > (1u << 24))
		return rdna4Error([NSString stringWithFormat:@"a draw of %u vertices, type %u, %u instances: not run", d->count, d->type, d->instances], 0);
	if (p.format != t.pixelFormat)
		return rdna4Error(@"the pipeline's colour format is not the target's", 0);
	static uint32_t ib[RDNA4_TRIRUN_PAGE / 4];   // ponytail: one draw at a time (synchronous commit)
	static uint64_t misc[kMiscBytes / 8];
	const uint32_t *vs = p.vs, *ps = p.ps, outs = p.outs, ins = p.ins;
	const BOOL noColour = ps[7] >> 31;
	const uint64_t rings = self.gfxRings;
	const rdna4_tri_va where = { p.code, t.gpuBase, rings, _misc.gpu };
	const rdna4_tricol_va whereCol = { p.code, t.gpuBase, rings, _misc.gpu };
	const uint32_t n = outs ? rdna4_tricol_record(ib, &whereCol) : rdna4_tri_record(ib, &where);
	if (!n)
		return rdna4Error(@"the stream builder refused the draw's addresses", 0);
	const uint32_t width = (uint32_t)t.width, height = (uint32_t)t.height, pitch = (uint32_t)(t.rowBytes / 4);
	const float *v = d->viewport;
	// Metal's point, line, line strip, triangle, triangle strip: DI_PT_* and the NGG stage's output primitive
	static const uint32_t kPrimitive[5][2] = { { 1, 0 }, { 2, 1 }, { 3, 1 }, { 4, 2 }, { 6, 2 } };
	// As the PV host's patch table, single-sampled, blending off, every channel written.
	const struct { uint32_t opcode, reg, value; } patch[] = {
		{ 0x76, 0x8a, vs[3] }, { 0x76, 0x8b, vs[4] },                     // SPI_SHADER_PGM_RSRC1_GS, RSRC2_GS
		{ 0x76, 0x0a, ps[3] }, { 0x76, 0x0b, ps[4] },                     // SPI_SHADER_PGM_RSRC1_PS, RSRC2_PS
		{ 0x69, 0x197, ps[5] }, { 0x69, 0x198, ps[6] },                   // SPI_PS_INPUT_ENA, SPI_PS_INPUT_ADDR
		{ 0x69, 0x31e, (pitch - 1) << 16 | (height - 1) },               // CB_COLOR0_ATTRIB2: MIP0_WIDTH (the pitch), MIP0_HEIGHT
		{ 0x69, 0x3b0, colourInfo(t.pixelFormat) },                      // CB_COLOR0_INFO
		{ 0x69, 0x82, (width - 1) | (height - 1) << 16 },                // PA_SC_WINDOW_SCISSOR_BR (inclusive)
		{ 0x69, 0x94, 0 }, { 0x69, 0x95, (width - 1) | (height - 1) << 16 },   // PA_SC_VPORT_SCISSOR_0_TL, _BR: the whole target
		{ 0x69, 0x10f, floatBits(v[2] / 2) }, { 0x69, 0x110, floatBits(v[0] + v[2] / 2) },    // PA_CL_VPORT_XSCALE, XOFFSET
		{ 0x69, 0x111, floatBits(-v[3] / 2) }, { 0x69, 0x112, floatBits(v[1] + v[3] / 2) },   // YSCALE (Metal's y up), YOFFSET
		{ 0x79, 0x242, kPrimitive[d->type][0] }, { 0x79, 0x266, kPrimitive[d->type][1] },     // VGT_PRIMITIVE_TYPE, VGT_GS_OUT_PRIM_TYPE
		{ 0x76, 0x31, outs ? (outs - 1) | ins << 11 : 0x400u },          // SPI_SHADER_GS_OUT_CONFIG_PS
		{ 0x69, 0x207, 0x00000244u },                                    // PA_SU_SC_MODE_CNTL: provoking vertex first, clockwise front
		{ 0x69, 0x1e0, 0 }, { 0x69, 0x1d8, 0x06000600u },               // CB_BLEND0_CONTROL, SX_MRT0_BLEND_OPT: no blending
		{ 0x69, 0x214, noColour ? 0 : 0xf },                             // CB_TARGET_MASK
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
	memset(misc, 0, sizeof misc);
	for (unsigned st = 0; st < 2; st++)
		for (unsigned k = 0; k < kTableBuffers; k++) {
			const uint32_t at = 0x400 + 0x400 * st + (k < 8 ? 8 * k : RDNA4_MTLC_TABLE_MORE_BUFFERS + 8 * (k - 8));
			misc[at / 8] = d->table[st][k] ?: _spare.gpu;
		}
	for (unsigned st = 0; st < 2; st++)   // the T#s at +64 + 32 n, the S#s at +576 + 16 n of the stage's table (include/rdna4mtlc.h)
		for (unsigned k = 0; k < kStageTextures; k++) {
			memcpy((uint8_t *)misc + 0x400 + 0x400 * st + RDNA4_MTLC_TABLE_TEXTURES + 32 * k, d->textures[st][k], 32);
			memcpy((uint8_t *)misc + 0x400 + 0x400 * st + RDNA4_MTLC_TABLE_SAMPLERS + 16 * k, d->samplers[st][k], 16);
		}
	// The draw in place of the template's DRAW_INDEX_AUTO: blend colour, the fragment inputs, the stages' tables (USER_DATA_GS_0/1, _PS_0/1),
	// the first vertex and base instance (USER_DATA_GS_2/3), NUM_INSTANCES, DRAW_INDEX_AUTO.
	uint32_t seq[96], m = 0;
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
	PUT(0xc0046900u), PUT(0x105), PUT(0), PUT(0), PUT(0), PUT(0);
	if (ins) {
		PUT(0xc0006900u | ins << 16), PUT(0x199);
		for (uint32_t k = 0; k < ins; k++)
			PUT(p.inputCntl[k]);
	}
	PUT(0xc0016900u), PUT(0x191), PUT(p.sprite ? 0x86bu : 0x869u);   // SPI_INTERP_CONTROL_0
	for (unsigned st = 0; st < 2; st++) {
		const uint64_t table = _misc.gpu + 0x400 + 0x400 * st;
		PUT(0xc0027600u), PUT(st ? 0x0c : 0x8c), PUT((uint32_t)table), PUT((uint32_t)(table >> 32));
	}
	PUT(0xc0027600u), PUT(0x8e), PUT(d->start), PUT(d->baseInstance);
	PUT(0xc0002f00u), PUT(d->instances);
	if (d->indexGpu) {   // VGT_INDEX_TYPE (SET_UCONFIG_REG_INDEX, as RADV), DRAW_INDEX_2 {max indices, address, count, initiator}
		PUT(0xc0017a00u), PUT(0x243 | 2u << 28), PUT(d->indexBytes == 4 ? 1 : 0);
		PUT(0xc0042700u), PUT(d->count), PUT((uint32_t)d->indexGpu), PUT((uint32_t)(d->indexGpu >> 32)), PUT(d->count), PUT(0);
	} else {
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
	uint64_t fence = 0, ns = 0;
	uint32_t streamFence = 0;
	kern_return_t kr = rdna4_write(&_gpu, &_misc, 0, misc, sizeof misc);
	if (!kr)
		kr = rdna4_write(&_gpu, &_ib, 0, ib, total * 4);
	if (!kr)
		kr = rdna4_submit_gfx_ib(&_gpu, &_ib, 0, total, &fence);
	if (!kr)
		kr = rdna4_wait_gfx_fence(&_gpu, fence, 2000, &ns);
	if (!kr)
		kr = rdna4_read(&_gpu, &_misc, 0, &streamFence, 4);
	if (kr)
		return rdna4Error([NSString stringWithFormat:@"a draw of %u vertices (%u dwords, fence %llu)", d->count, total, fence], kr);
	if (streamFence != 1)
		return rdna4Error([NSString stringWithFormat:@"a draw finished but its stream's fence reads %u", streamFence], 0);
	return nil;
}

- (void)_setAcceleratorService:(id)service
{
	_accelService = service;
}

- (void)_removeRequested
{
}

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
	return [[RDNA4MTLBuffer alloc] initWithDevice:self bytesNoCopy:pointer length:length options:options deallocator:deallocator];
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
