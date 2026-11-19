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
 * ponytail: synchronous commit and one host allocation per setBytes; a real queue (submitted IBs, fences, completion on a thread) when a
 * gate needs overlap.
 *
 * The device exists only in processes started with RDNA4_NATIVE_MTL=1 (the init returns nil elsewhere, which Metal skips): published
 * for every process, it was made in ControlCenter and the like, which crashed on the first missing message (2026-11-19).
 */

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include "librdna4.h"

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
@end

@implementation RDNA4MTLBuffer {
	RDNA4MTLDevice *_device;
	rdna4_buffer_t _buf;
	void *_cpu;
	NSUInteger _length;
	MTLResourceOptions _options;
}
@synthesize label;

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
		rdna4_free(_device.gpu, &_buf);
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

// ---- compute pipelines

@interface RDNA4MTLComputePipeline : NSObject <MTLComputePipelineState>
@property (readonly) rdna4_program_t program;
@property (readonly) uint32_t buffers;   // the kernargs' buffer addresses: indices 0 .. buffers - 1
@end

@implementation RDNA4MTLComputePipeline {
	RDNA4MTLDevice *_device;
}

// Translate `function` (the translator in file mode) and load its code object; nil and *error on failure.
- (instancetype)initWithDevice:(RDNA4MTLDevice *)device function:(RDNA4MTLFunction *)function error:(NSError **)error
{
	if (!(self = [super init]))
		return nil;
	_device = device;
	NSError *why = [self loadFunction:function];
	if (why && error)
		*error = why;
	return why ? nil : self;
}

- (NSError *)loadFunction:(RDNA4MTLFunction *)function
{
	if (!_device.gpu)
		return rdna4Error(@"the kext's runtime client is not open (root only)", 0);
	NSString *tool = NSProcessInfo.processInfo.environment[@"RDNA4_AIR2AMDGPU"] ?: @"/usr/local/libexec/rdna4-air2amdgpu";
	NSString *out = [NSTemporaryDirectory() stringByAppendingPathComponent:
	                                            [NSString stringWithFormat:@"rdna4mtl-%d-%@.hsaco", getpid(), function.name]];
	NSTask *task = [NSTask new];
	NSPipe *pipe = [NSPipe pipe];
	task.executableURL = [NSURL fileURLWithPath:tool];
	task.arguments = @[ function.library.path, function.name, out ];
	task.standardOutput = pipe;
	task.standardError = pipe;
	NSError *runError = nil;
	if (![task launchAndReturnError:&runError])
		return rdna4Error([NSString stringWithFormat:@"cannot run %@: %@", tool, runError.localizedDescription], 0);
	NSData *said = [pipe.fileHandleForReading readDataToEndOfFile];
	[task waitUntilExit];
	NSString *text = [[NSString alloc] initWithData:said encoding:NSUTF8StringEncoding];
	NSArray *words = [text componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];   // "<name> <buffers> <written>"
	NSData *elf = [NSData dataWithContentsOfFile:out];
	[NSFileManager.defaultManager removeItemAtPath:out error:nil];
	if (task.terminationStatus || words.count < 2 || !elf)
		return rdna4Error([NSString stringWithFormat:@"%@ could not translate %@: %@", tool, function.name, text], 0);
	_buffers = (uint32_t)[words[1] intValue];
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

// ---- command queues, buffers and compute encoders

enum { kMaxBufferIndex = 31 };

@interface RDNA4MTLDispatch : NSObject   // one recorded dispatch: everything commit needs
@property RDNA4MTLComputePipeline *pipeline;
@property NSData *kernargs;
@property NSArray *held;   // buffers it uses, kept alive until it ran
@property MTLSize groups, groupSize;
@end
@implementation RDNA4MTLDispatch
@end

@interface RDNA4MTLCommandQueue : NSObject <MTLCommandQueue>
@end

@interface RDNA4MTLCommandBuffer : NSObject <MTLCommandBuffer>
- (instancetype)initWithQueue:(RDNA4MTLCommandQueue *)queue;
- (void)record:(RDNA4MTLDispatch *)dispatch;
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
	RDNA4MTLDispatch *d = [RDNA4MTLDispatch new];
	d.pipeline = _pipeline;
	d.kernargs = args;
	d.groups = groups;
	d.groupSize = groupSize;
	NSMutableArray *held = [NSMutableArray array];
	for (int i = 0; i < kMaxBufferIndex; i++)
		if (_bound[i])
			[held addObject:_bound[i]];
	d.held = held;
	[_commandBuffer record:d];
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

@implementation RDNA4MTLCommandBuffer {
	RDNA4MTLCommandQueue *_queue;
	NSMutableArray<RDNA4MTLDispatch *> *_dispatches;
	NSMutableArray<MTLCommandBufferHandler> *_completed, *_scheduled;
	MTLCommandBufferStatus _status;
	NSError *_error;
}
@synthesize label;

- (instancetype)initWithQueue:(RDNA4MTLCommandQueue *)queue
{
	if ((self = [super init])) {
		_queue = queue;
		_dispatches = [NSMutableArray array];
		_completed = [NSMutableArray array];
		_scheduled = [NSMutableArray array];
	}
	return self;
}

- (id<MTLDevice>)device { return _queue.device; }
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (MTLCommandBufferStatus)status { return _status; }
- (NSError *)error { return _error; }
- (BOOL)retainedReferences { return YES; }
- (void)record:(RDNA4MTLDispatch *)dispatch { [_dispatches addObject:dispatch]; }
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

- (void)commit
{
	_status = MTLCommandBufferStatusScheduled;
	for (MTLCommandBufferHandler h in _scheduled)
		h(self);
	rdna4_t *gpu = ((RDNA4MTLDevice *)self.device).gpu;
	for (RDNA4MTLDispatch *d in _dispatches) {
		rdna4_program_t prog = d.pipeline.program;
		const uint32_t groups[3] = { (uint32_t)d.groups.width, (uint32_t)d.groups.height, (uint32_t)d.groups.depth };
		const uint32_t size[3] = { (uint32_t)d.groupSize.width, (uint32_t)d.groupSize.height, (uint32_t)d.groupSize.depth };
		const kern_return_t kr = gpu ? rdna4_dispatch(gpu, &prog, groups, size, d.kernargs.bytes, (uint32_t)d.kernargs.length, 1000, NULL)
		                             : kIOReturnNotOpen;
		if (kr) {
			_error = rdna4Error(@"a dispatch", kr);
			break;
		}
	}
	[_dispatches removeAllObjects];
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

@implementation RDNA4MTLDevice {
	io_service_t _service;
	uint64_t _registryID;
	__weak id _accelService;   // Metal's MTLIOAccelService for our service
	rdna4_t _gpu;
	BOOL _open;
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
	if (_open)
		rdna4_close(&_gpu);
	IOObjectRelease(_service);
}

- (rdna4_t *)gpu { return _open ? &_gpu : NULL; }

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
	if (![(id)function isKindOfClass:[RDNA4MTLFunction class]]) {
		if (error)
			*error = rdna4Error(@"a function of another device", 0);
		return nil;
	}
	return [[RDNA4MTLComputePipeline alloc] initWithDevice:self function:(RDNA4MTLFunction *)function error:error];
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
