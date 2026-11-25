// RDNA4FXTemporal.mm: MetalFX's temporal scaler on our device, as AMD FSR 3.1's upscaler (docs/fsr3-metalfx-plan.md, step 3).
//
// -[MTLFXTemporalScalerDescriptor newTemporalScalerWithDevice:] with our device (hooked in RDNA4MTLDevice.mm) gives an
// RDNA4FXTemporalScaler: FSR 3.1's passes from lib/FidelityFX-SDK (v1.1.4), built into the bundle's fsr3.metallib by
// tools/gen/build-fsr3.sh, run as compute dispatches on the app's command buffer. The host side follows the SDK's
// ffx_fsr3upscaler.cpp (fsr3upscalerCreate, fsr3upscalerDispatch): its resources, the constants of each frame, the clears, the order of
// the passes. Only Metal's public API on our device: the passes are translated at first use like any app's kernels.
//
// What MetalFX does not say: the camera's near/far planes and field of view (FSR uses them for its depth-to-view-space terms in the
// disocclusion test): fixed below. No sharpening (MetalFX's temporal scaler has none). Environment: RDNA4_FSR3_JITTER="sx,sy" scales
// the jitter given to FSR (default "1,1"; MetalFX's and FSR's are the same convention, +x right and +y down in input pixels).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>   // (its types only; MetalFX is not linked)
#include <cmath>
#include <cstdlib>
#include <cstring>

#import <os/log.h>
#define RLOG(...) os_log(OS_LOG_DEFAULT, "%{public}s", [NSString stringWithFormat:__VA_ARGS__].UTF8String)   // (as RDNA4MTLDevice.mm's)

// FSR 3.1's constants (ffx_fsr3upscaler_private.h Fsr3UpscalerConstants = the passes' cbFSR3UPSCALER_t), padded as the MSL struct
struct Fsr3Constants {
	int32_t renderSize[2], previousFrameRenderSize[2], upscaleSize[2], previousFrameUpscaleSize[2], maxRenderSize[2], maxUpscaleSize[2];
	float deviceToViewDepth[4];
	float jitterOffset[2], previousFrameJitterOffset[2], motionVectorScale[2], downscaleFactor[2], motionVectorJitterCancellation[2];
	float tanHalfFOV, jitterPhaseCount, deltaTime, deltaPreExposure, viewSpaceToMetersFactor, frameIndex, velocityFactor,
	      reactivenessScale, shadingChangeScale, accumulationAddedPerFrame, minDisocclusionAccumulation;
	float pad[3];
};
static_assert(sizeof(Fsr3Constants) == 160, "cbFSR3UPSCALER_t");

struct Fsr3Spd {   // cbSPD_t
	uint32_t mips, numWorkGroups, workGroupOffset[2], renderSize[2];
};

enum { kPrepareInputs, kLumaPyramid, kShadingChangePyramid, kShadingChange, kPrepareReactivity, kLumaInstability, kAccumulate, kPasses };
static NSString *const kPassNames[kPasses] = {@"prepare_inputs", @"luma_pyramid", @"shading_change_pyramid", @"shading_change",
                                               @"prepare_reactivity", @"luma_instability", @"accumulate"};
static const float kNear = 0.1f, kFar = 1000.0f, kFovY = 1.2f;   // ponytail: MetalFX gives no camera; ~69 degrees, a game's usual

@interface RDNA4FXTemporalScaler : NSObject <MTLFXTemporalScaler>
- (instancetype)initWithDescriptor:(MTLFXTemporalScalerDescriptor *)d device:(id<MTLDevice>)device library:(id<MTLLibrary>)library;
@end

@implementation RDNA4FXTemporalScaler {
	id<MTLDevice> _device;
	id<MTLLibrary> _library;
	id<MTLComputePipelineState> _passes[4][kPasses];   // [hdr * 2 + inverted depth]
	id<MTLComputePipelineState> _clearTexture, _clearWords;
	id<MTLSamplerState> _linearClamp;
	BOOL _hdr, _autoExposure;
	// FSR's resources (fsr3upscalerCreate), sized for the descriptor's input (render) and output (upscale) sizes
	id<MTLTexture> _accumulation[2], _luma[2], _intermediate, _shadingChange, _newLocks, _upscaled[2], _spdMips, _spdMip[6],
	    _farthestDepthMip1, _lumaHistory[2], _dilatedReactiveMasks, _defaultReactivity, _defaultExposure, _frameInfo, _dilatedDepth,
	    _dilatedMotion, _reconstructedDepth, _spdAtomic;
	id<MTLBuffer> _reconstructedDepthWords, _spdAtomicWords;
	NSUInteger _reconstructedDepthCount;
	Fsr3Constants _c;
	float _previousPreExposure, _jitterScale[2];
	uint32_t _frame;   // resourceFrameIndex
	BOOL _first;
}
@synthesize inputContentWidth = _inputContentWidth, inputContentHeight = _inputContentHeight, colorTexture = _colorTexture,
            depthTexture = _depthTexture, motionTexture = _motionTexture, outputTexture = _outputTexture, exposureTexture = _exposureTexture,
            reactiveMaskTexture = _reactiveMaskTexture, preExposure = _preExposure, jitterOffsetX = _jitterOffsetX,
            jitterOffsetY = _jitterOffsetY, motionVectorScaleX = _motionVectorScaleX, motionVectorScaleY = _motionVectorScaleY,
            reset = _reset, depthReversed = _depthReversed, colorTextureFormat = _colorTextureFormat,
            depthTextureFormat = _depthTextureFormat, motionTextureFormat = _motionTextureFormat,
            reactiveMaskTextureFormat = _reactiveMaskTextureFormat, outputTextureFormat = _outputTextureFormat, inputWidth = _inputWidth,
            inputHeight = _inputHeight, outputWidth = _outputWidth, outputHeight = _outputHeight,
            inputContentMinScale = _inputContentMinScale, inputContentMaxScale = _inputContentMaxScale, fence = _fence;

- (id<MTLTexture>)texture:(MTLPixelFormat)format width:(NSUInteger)w height:(NSUInteger)h levels:(NSUInteger)levels
{
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:MAX(w, 1u) height:MAX(h, 1u)
	                                                                         mipmapped:NO];
	d.mipmapLevelCount = levels;
	d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
	d.storageMode = MTLStorageModePrivate;
	return [_device newTextureWithDescriptor:d];
}

// An R32Uint texture whose words are also a buffer (SPIRV-Cross's texture atomics: rows of spvLinearTextureAlignment = 256 bytes)
- (id<MTLTexture>)atomicTextureWidth:(NSUInteger)w height:(NSUInteger)h words:(id<MTLBuffer> __strong *)words count:(NSUInteger *)count
{
	const NSUInteger row = (w * 4 + 255) & ~(NSUInteger)255;
	*words = [_device newBufferWithLength:row * h options:MTLResourceStorageModePrivate];
	*count = row / 4 * h;
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:w height:h mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	d.storageMode = MTLStorageModePrivate;
	return [*words newTextureWithDescriptor:d offset:0 bytesPerRow:row];
}

- (instancetype)initWithDescriptor:(MTLFXTemporalScalerDescriptor *)d device:(id<MTLDevice>)device library:(id<MTLLibrary>)library
{
	if (!(self = [super init]))
		return nil;
	_device = device, _library = library;
	_colorTextureFormat = d.colorTextureFormat, _depthTextureFormat = d.depthTextureFormat, _motionTextureFormat = d.motionTextureFormat;
	_outputTextureFormat = d.outputTextureFormat;
	if (@available(macOS 14.4, *))
		_reactiveMaskTextureFormat = d.reactiveMaskTextureFormat;
	_inputWidth = _inputContentWidth = d.inputWidth, _inputHeight = _inputContentHeight = d.inputHeight;
	_outputWidth = d.outputWidth, _outputHeight = d.outputHeight;
	_inputContentMinScale = d.inputContentPropertiesEnabled ? d.inputContentMinScale : 1.0f;
	_inputContentMaxScale = d.inputContentPropertiesEnabled ? d.inputContentMaxScale : 3.0f;
	_autoExposure = d.autoExposureEnabled;
	_motionVectorScaleX = _motionVectorScaleY = 1.0f;
	_preExposure = 1.0f;
	// HDR (FSR's reversible tonemap inside) for colour that may go above 1: the float formats
	switch (_colorTextureFormat) {
	case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA32Float: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float:
		_hdr = YES;
		break;
	default:
		break;
	}
	_jitterScale[0] = _jitterScale[1] = 1.0f;
	if (const char *j = getenv("RDNA4_FSR3_JITTER"))
		sscanf(j, "%f,%f", &_jitterScale[0], &_jitterScale[1]);

	_clearTexture = [self pipeline:@"fsr3_clear_texture"];
	_clearWords = [self pipeline:@"fsr3_clear_words"];
	MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
	s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear;
	s.mipFilter = MTLSamplerMipFilterLinear;
	s.sAddressMode = s.tAddressMode = MTLSamplerAddressModeClampToEdge;
	_linearClamp = [_device newSamplerStateWithDescriptor:s];

	const NSUInteger rw = _inputWidth, rh = _inputHeight, uw = _outputWidth, uh = _outputHeight;
	for (int i = 0; i < 2; i++) {
		_accumulation[i] = [self texture:MTLPixelFormatR8Unorm width:rw height:rh levels:1];
		_luma[i] = [self texture:MTLPixelFormatR16Float width:rw height:rh levels:1];
		_upscaled[i] = [self texture:MTLPixelFormatRGBA16Float width:uw height:uh levels:1];
		_lumaHistory[i] = [self texture:MTLPixelFormatRGBA16Float width:rw height:rh levels:1];
	}
	_intermediate = [self texture:MTLPixelFormatR16Float width:rw height:rh levels:1];
	_shadingChange = [self texture:MTLPixelFormatR8Unorm width:rw / 2 height:rh / 2 levels:1];
	_newLocks = [self texture:MTLPixelFormatR8Unorm width:uw height:uh levels:1];
	const NSUInteger levels = (NSUInteger)std::floor(std::log2((double)MAX(MAX(rw / 2, rh / 2), 1u))) + 1;
	_spdMips = [self texture:MTLPixelFormatRG16Float width:rw / 2 height:rh / 2 levels:MAX(levels, 6u)];
	for (NSUInteger l = 0; l < 6; l++)
		_spdMip[l] = [_spdMips newTextureViewWithPixelFormat:MTLPixelFormatRG16Float textureType:MTLTextureType2D levels:NSMakeRange(l, 1)
		                                              slices:NSMakeRange(0, 1)];
	_farthestDepthMip1 = [self texture:MTLPixelFormatR16Float width:rw / 2 height:rh / 2 levels:1];
	_dilatedReactiveMasks = [self texture:MTLPixelFormatRGBA8Unorm width:rw height:rh levels:1];
	_defaultReactivity = [self texture:MTLPixelFormatR8Unorm width:1 height:1 levels:1];
	_defaultExposure = [self texture:MTLPixelFormatRG32Float width:1 height:1 levels:1];
	_frameInfo = [self texture:MTLPixelFormatRGBA32Float width:1 height:1 levels:1];
	_dilatedDepth = [self texture:MTLPixelFormatR32Float width:rw height:rh levels:1];
	_dilatedMotion = [self texture:MTLPixelFormatRG16Float width:rw height:rh levels:1];
	NSUInteger one;
	_reconstructedDepth = [self atomicTextureWidth:rw height:rh words:&_reconstructedDepthWords count:&_reconstructedDepthCount];
	_spdAtomic = [self atomicTextureWidth:1 height:1 words:&_spdAtomicWords count:&one];

	_c.maxUpscaleSize[0] = (int32_t)uw, _c.maxUpscaleSize[1] = (int32_t)uh;
	_c.maxRenderSize[0] = (int32_t)rw, _c.maxRenderSize[1] = (int32_t)rh;
	_c.velocityFactor = 1.0f, _c.reactivenessScale = 1.0f, _c.shadingChangeScale = 1.0f;
	_c.accumulationAddedPerFrame = 1.0f / 3.0f, _c.minDisocclusionAccumulation = -1.0f / 3.0f;
	_first = YES;
	if (!_clearTexture || !_clearWords || !_reconstructedDepth || !_spdAtomic || !_spdMip[5])
		return nil;
	return self;
}

- (id<MTLComputePipelineState>)pipeline:(NSString *)name
{
	id<MTLFunction> f = [_library newFunctionWithName:name];
	NSError *e = nil;
	id<MTLComputePipelineState> p = f ? [_device newComputePipelineStateWithFunction:f error:&e] : nil;
	if (!p)
		RLOG(@"RDNA4MTL: MetalFX temporal scaler: no pipeline for %@ (%@)", name, e ?: @"not in fsr3.metallib");
	return p;
}

- (MTLTextureUsage)colorTextureUsage { return MTLTextureUsageShaderRead; }
- (MTLTextureUsage)depthTextureUsage { return MTLTextureUsageShaderRead; }
- (MTLTextureUsage)motionTextureUsage { return MTLTextureUsageShaderRead; }
- (MTLTextureUsage)reactiveTextureUsage { return MTLTextureUsageShaderRead; }
- (MTLTextureUsage)outputTextureUsage { return MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; }

- (void)clear:(id<MTLComputeCommandEncoder>)e texture:(id<MTLTexture>)t to:(simd_float4)v
{
	[e setComputePipelineState:_clearTexture];
	[e setTexture:t atIndex:0];
	[e setBytes:&v length:sizeof v atIndex:0];
	[e dispatchThreads:MTLSizeMake(t.width, t.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
}

- (void)clear:(id<MTLComputeCommandEncoder>)e words:(id<MTLBuffer>)b count:(NSUInteger)n to:(uint32_t)v
{
	const uint32_t value[2] = {v, (uint32_t)n};
	[e setComputePipelineState:_clearWords];
	[e setBuffer:b offset:0 atIndex:0];
	[e setBytes:value length:sizeof value atIndex:1];
	[e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
}

- (void)run:(id<MTLComputeCommandEncoder>)e pass:(id<MTLComputePipelineState>)p textures:(NSArray<id<MTLTexture>> *)textures
     groups:(MTLSize)groups size:(MTLSize)size
{
	[e setComputePipelineState:p];
	for (NSUInteger i = 0; i < textures.count; i++)
		[e setTexture:textures[i] atIndex:i];
	[e dispatchThreadgroups:groups threadsPerThreadgroup:size];
}

- (void)encodeToCommandBuffer:(id<MTLCommandBuffer>)commandBuffer
{
	if (!_colorTexture || !_depthTexture || !_motionTexture || !_outputTexture)
		return;
	const int variant = (_hdr ? 2 : 0) + (_depthReversed ? 1 : 0);
	id<MTLComputePipelineState> __strong *passes = _passes[variant];
	if (!passes[0])
		for (int i = 0; i < kPasses; i++)
			if (!(passes[i] = [self pipeline:[NSString stringWithFormat:@"fsr3_%@_%d%d", kPassNames[i], _hdr ? 1 : 0, _depthReversed ? 1 : 0]]))
				return;

	// fsr3upscalerDispatch: the resources of this frame
	const BOOL odd = _frame & 1, reset = _reset || _first;
	id<MTLTexture> accumulationIn = _accumulation[odd], accumulationOut = _accumulation[!odd];
	id<MTLTexture> upscaledIn = _upscaled[odd], upscaledOut = _upscaled[!odd];
	id<MTLTexture> historyIn = _lumaHistory[odd], historyOut = _lumaHistory[!odd];
	id<MTLTexture> currentLuma = _luma[odd], previousLuma = _luma[!odd];
	id<MTLTexture> exposure = _autoExposure ? _frameInfo : _exposureTexture ?: _defaultExposure;
	id<MTLTexture> reactive = _reactiveMaskTexture ?: _defaultReactivity;

	// the constants (as the SDK's, with the camera MetalFX does not give fixed)
	Fsr3Constants &c = _c;
	const int32_t rw = (int32_t)MIN(_inputContentWidth ?: _inputWidth, _inputWidth), rh = (int32_t)MIN(_inputContentHeight ?: _inputHeight, _inputHeight);
	const int32_t uw = (int32_t)_outputWidth, uh = (int32_t)_outputHeight;
	c.previousFrameJitterOffset[0] = c.jitterOffset[0], c.previousFrameJitterOffset[1] = c.jitterOffset[1];
	c.jitterOffset[0] = _jitterOffsetX * _jitterScale[0], c.jitterOffset[1] = _jitterOffsetY * _jitterScale[1];
	c.previousFrameRenderSize[0] = c.renderSize[0], c.previousFrameRenderSize[1] = c.renderSize[1];
	c.renderSize[0] = rw, c.renderSize[1] = rh;
	const float aspect = (float)rw / (float)rh;
	c.tanHalfFOV = std::tan(std::atan(std::tan(kFovY / 2) * aspect));
	c.viewSpaceToMetersFactor = 1.0f;
	{   // setupDeviceDepthToViewSpaceDepthParams, not infinite
		float lo = kNear, hi = kFar;
		if (_depthReversed)
			std::swap(lo, hi);
		const float q = hi / (lo - hi), cot = std::cos(0.5f * kFovY) / std::sin(0.5f * kFovY);
		c.deviceToViewDepth[0] = -q, c.deviceToViewDepth[1] = q * lo;
		c.deviceToViewDepth[2] = aspect / cot, c.deviceToViewDepth[3] = 1.0f / cot;
	}
	c.previousFrameUpscaleSize[0] = c.upscaleSize[0], c.previousFrameUpscaleSize[1] = c.upscaleSize[1];
	c.upscaleSize[0] = uw, c.upscaleSize[1] = uh;
	c.downscaleFactor[0] = (float)rw / uw, c.downscaleFactor[1] = (float)rh / uh;
	const float preExposure = _preExposure != 0.0f ? _preExposure : 1.0f;
	c.deltaPreExposure = _previousPreExposure > 0.0f ? preExposure / _previousPreExposure : 1.0f;
	_previousPreExposure = preExposure;
	c.motionVectorScale[0] = _motionVectorScaleX / rw, c.motionVectorScale[1] = _motionVectorScaleY / rh;
	const float phases = (float)(int32_t)(8.0f * std::pow((float)uw / rw, 2.0f));   // ffxFsr3UpscalerGetJitterPhaseCount
	if (reset || c.jitterPhaseCount == 0)
		c.jitterPhaseCount = phases;
	else if (phases != c.jitterPhaseCount)
		c.jitterPhaseCount += phases > c.jitterPhaseCount ? 1 : -1;
	c.deltaTime = 1.0f / 60.0f;   // ponytail: MetalFX gives no frame time; FSR uses it only for its luma history's decay
	c.frameIndex = reset ? 0.0f : c.frameIndex + 1.0f;

	// ffxSpdSetup over the render size
	const uint32_t gx = (uint32_t)(rw - 1) / 64 + 1, gy = (uint32_t)(rh - 1) / 64 + 1;
	const Fsr3Spd spd = {(uint32_t)MIN(std::floor(std::log2((float)MAX(rw, rh))), 12.0f), gx * gy, {0, 0}, {(uint32_t)rw, (uint32_t)rh}};

	id<MTLComputeCommandEncoder> e = [commandBuffer computeCommandEncoder];
	if (_fence)
		[e waitForFence:_fence];
	const simd_float4 zero = {0, 0, 0, 0};
	if (_first) {
		for (int i = 0; i < 2; i++) {
			[self clear:e texture:_accumulation[i] to:zero];
			[self clear:e texture:_luma[i] to:zero];
		}
		[self clear:e texture:_newLocks to:zero];
		[self clear:e texture:_defaultReactivity to:zero];
		[self clear:e texture:_defaultExposure to:zero];
	}
	if (reset) {
		[self clear:e texture:accumulationIn to:zero];
		[self clear:e texture:_frameInfo to:simd_make_float4(-1, 1, 0, 0)];
	}
	for (int l = 0; l < 6; l++)
		[self clear:e texture:_spdMip[l] to:zero];
	const float far = _depthReversed ? 0.0f : 1.0f;
	uint32_t farBits;
	memcpy(&farBits, &far, 4);
	[self clear:e words:_reconstructedDepthWords count:_reconstructedDepthCount to:farBits];
	[self clear:e words:_spdAtomicWords count:1 to:0];
	_first = NO;

	[e setBytes:&c length:sizeof c atIndex:0];
	[e setSamplerState:_linearClamp atIndex:0];
	const MTLSize tile = MTLSizeMake(8, 8, 1), spdSize = MTLSizeMake(256, 1, 1), spdGroups = MTLSizeMake(gx, gy, 1);
	auto groups = [&](int32_t w, int32_t h) { return MTLSizeMake((NSUInteger)(w + 7) / 8, (NSUInteger)(h + 7) / 8, 1); };

	[e setBuffer:_reconstructedDepthWords offset:0 atIndex:1];
	[self run:e pass:passes[kPrepareInputs]
	    textures:@[_depthTexture, _colorTexture, _motionTexture, _intermediate, currentLuma, _reconstructedDepth, _dilatedDepth, _dilatedMotion]
	      groups:groups(rw, rh) size:tile];
	[e setBytes:&spd length:sizeof spd atIndex:1];
	[e setBuffer:_spdAtomicWords offset:0 atIndex:2];
	[self run:e pass:passes[kLumaPyramid]
	    textures:@[_intermediate, _farthestDepthMip1, currentLuma, _frameInfo, _spdMip[0], _spdMip[1], _spdMip[2], _spdMip[3], _spdMip[4],
	               _spdMip[5], _spdAtomic]
	      groups:spdGroups size:spdSize];
	[self run:e pass:passes[kShadingChangePyramid]
	    textures:@[currentLuma, previousLuma, _dilatedMotion, exposure, _spdMip[0], _spdMip[1], _spdMip[2], _spdMip[3], _spdMip[4], _spdMip[5],
	               _spdAtomic]
	      groups:spdGroups size:spdSize];
	[self run:e pass:passes[kShadingChange] textures:@[_spdMips, _shadingChange] groups:groups((int32_t)(rw * 0.5f), (int32_t)(rh * 0.5f))
	     size:tile];
	[self run:e pass:passes[kPrepareReactivity]
	    textures:@[reactive, _defaultReactivity, accumulationIn, accumulationOut, _shadingChange, currentLuma, _newLocks, _reconstructedDepth,
	               _dilatedMotion, _dilatedDepth, exposure, _dilatedReactiveMasks]
	      groups:groups(rw, rh) size:tile];
	[self run:e pass:passes[kLumaInstability]
	    textures:@[historyOut, historyIn, _farthestDepthMip1, currentLuma, _intermediate, _dilatedMotion, exposure, _dilatedReactiveMasks]
	      groups:groups(rw, rh) size:tile];
	[self run:e pass:passes[kAccumulate]
	    textures:@[_colorTexture, upscaledIn, upscaledOut, _outputTexture, _farthestDepthMip1, _intermediate, _newLocks, _dilatedMotion, exposure,
	               _dilatedReactiveMasks]
	      groups:groups(uw, uh) size:tile];
	if (_fence)
		[e updateFence:_fence];
	[e endEncoding];
	_frame = (_frame + 1) % 16;
	_reset = NO;
}

@end

// The descriptor's method on our device (RDNA4MTLDevice.mm's hook): the scaler, or nil when the bundle's FSR library is missing
__attribute__((ns_returns_retained)) id rdna4NewTemporalScaler(MTLFXTemporalScalerDescriptor *d, id<MTLDevice> device)
{
	static id<MTLLibrary> library;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		NSString *path = [[NSBundle bundleForClass:[RDNA4FXTemporalScaler class]] pathForResource:@"fsr3" ofType:@"metallib"];
		library = path ? [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:nil] : nil;
	});
	if (!library || !d.inputWidth || !d.inputHeight || !d.outputWidth || !d.outputHeight) {
		RLOG(@"RDNA4MTL: MetalFX temporal scaler: no fsr3.metallib in the bundle, or a size of 0: none");
		return nil;
	}
	RDNA4FXTemporalScaler *s = [[RDNA4FXTemporalScaler alloc] initWithDescriptor:d device:device library:library];
	RLOG(@"RDNA4MTL: MetalFX temporal scaler: AMD FSR 3.1, %lux%lu to %lux%lu, colour format %lu%s%s", (unsigned long)d.inputWidth,
	     (unsigned long)d.inputHeight, (unsigned long)d.outputWidth, (unsigned long)d.outputHeight, (unsigned long)d.colorTextureFormat,
	     d.autoExposureEnabled ? ", auto exposure" : "", s ? "" : ": FAILED");
	return s;
}
