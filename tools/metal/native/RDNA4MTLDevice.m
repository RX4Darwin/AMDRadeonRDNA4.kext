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
 * Gate 1: the device exists. It has a name and a registry ID. Metal then sends it MTLIOAccelDevice's private messages
 * (_setAcceleratorService: right after the init, _removeRequested on safe eject): those are answered here, and anything else neither
 * class answers is logged once ("RDNA4MTLDevice: not implemented: <selector>") and returns zero, so the spike lists what a device needs.
 * ponytail: zero for every unknown message, struct returns included; real methods replace the log lines one by one.
 *
 * The device exists only in processes started with RDNA4_NATIVE_MTL=1 (the init returns nil elsewhere, which Metal skips): published
 * for every process, it was made in ControlCenter and the like, which crashed on the first missing message (2026-11-19).
 */

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

@interface _MTLDevice : NSObject   // Metal's own base class (exported by Metal.framework)
@end

@interface RDNA4MTLDevice : _MTLDevice <MTLDevice>   // MTLAddDevice asserts conformsToProtocol:@protocol(MTLDevice)
- (instancetype)initWithAcceleratorPort:(io_service_t)service;
@end

@implementation RDNA4MTLDevice {
	io_service_t _service;
	uint64_t _registryID;
	__weak id _accelService;   // Metal's MTLIOAccelService for our service
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
	NSLog(@"RDNA4MTLDevice: made for service 0x%x (registry ID 0x%llx)", service, _registryID);
	return self;
}

- (void)dealloc
{
	IOObjectRelease(_service);
}

- (void)_setAcceleratorService:(id)service
{
	_accelService = service;
}

- (void)_removeRequested
{
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	return [super methodSignatureForSelector:sel] ?: [NSMethodSignature signatureWithObjCTypes:"@@:"];
}

- (void)forwardInvocation:(NSInvocation *)inv
{
	static NSMutableSet *logged;
	NSString *name = NSStringFromSelector(inv.selector);
	@synchronized ([RDNA4MTLDevice class]) {
		if (!logged)
			logged = [NSMutableSet set];
		if ([logged containsObject:name])
			name = nil;
		else
			[logged addObject:name];
	}
	if (name)
		NSLog(@"RDNA4MTLDevice: not implemented: %@", name);
	NSUInteger len = inv.methodSignature.methodReturnLength;
	if (len) {
		void *zero = calloc(1, len);
		[inv setReturnValue:zero];
		free(zero);
	}
}

- (NSString *)name
{
	return @"AMD Radeon RDNA4 (native Metal, spike)";
}

- (uint64_t)registryID
{
	return _registryID;
}

@end
