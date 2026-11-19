// mtlprobe: known Metal work for M1 (docs/m1-stream.md), so that what the fake paravirtual GPU's host decodes can be matched to what was asked.
// Each mode is one small, separately committed piece of work on the system default device; it waits at most 5 s for completion, since the host
// does not execute anything yet (it acknowledges every command), and prints what came back.
//
//   mtlprobe info      the device and what it reports
//   mtlprobe blit      fill a 4 KiB buffer with 0x5a, copy it into a second buffer
//   mtlprobe compute   c[i] = a[i] + b[i] over 64 floats (the kernel compiled from source here)
//   mtlprobe grid      out[y * 50 + x] = x + 1000 * y over 50 x 20 threads in groups of 16 x 4 (2D, partial groups at the edges)
//   mtlprobe mix       out[i] = 3 * in[i] + 100000 * group + 1000 * thread in group + group size over 100 threads in groups of 32 (the
//                      last group has 4, and Metal reports 4 as its size); a kernel that is not in the kext's table: it runs only if
//                      translated at run time
//   mtlprobe dualsrc   dual-source blending: every pixel of a quad whose colour and second source depend on its parity, checked
//   mtlprobe devices   every Metal device MTLCopyAllDevices returns, its class and registry ID (docs/native-metal-spike.md)
//   mtlprobe native <probe.metallib>   the native-Metal spike on our own device (RDNA4MTLDevice; as root, RDNA4_NATIVE_MTL=1): gate 2,
//                      c = a + b over 1000 floats in groups of 64 (dispatchThreads: the last group is partial), then c *= 3 with setBytes;
//                      gate 3, a render pass: the kext's colour triangle into a 256x256 RGBA8 texture filled with 0x55 and cleared to 0,
//                      checked as rdna4-run tricol checks its own (userspace/rdna4-run/gfx12tricol.h); gate 5, ray queries: 1024 rays
//                      against 300 random triangles in an acceleration structure, each hit checked against the CPU's nearest triangle
//   mtlprobe two       two kernels of one library in one command buffer: out[i] = in[i] + 1, then out[i] *= 3 (64 threads)
//   mtlprobe twolib    the same with two libraries of one kernel each: out[i] = in[i] + 7, then out[i] *= out[i]
//   mtlprobe feat      one kernel per Metal language feature (constant buffers, math, threadgroup memory and barriers, atomics, local
//                      arrays, integer functions, half), each checked against the CPU
//   mtlprobe bytes     o[i] *= k with k from setBytes and dispatchThreadgroups (2 x 32), then k = 10 again over o[32..63] by
//                      setBufferOffset: o[i] = 3 i below 32, 30 i above
//   mtlprobe simd      SIMD-group functions over 40 threads in one group (a full SIMD group of 32 and one of 8), against a CPU model of
//                      what Apple's GPU does
//   mtlprobe tri       one triangle (vertices from vertex_id, a constant colour) drawn into a 64x64 BGRA texture cleared to black
//   mtlprobe tricol    the triangle with a colour per vertex (red, green, blue) interpolated across it, each pixel checked against the CPU
//   mtlprobe vbuf      a triangle from a vertex buffer (corners) scaled by setVertexBytes, coloured by setFragmentBytes
//   mtlprobe drawmodes a triangle strip from vertex 3 (red), 3 instances (green), base instance 3 (yellow) and an indexed quad (blue):
//                      pixels per colour, to compare with Apple's GPU
//   mtlprobe texinfo   four textures (RGBA8 4x4, BGRA8 8x2, R32Float 3x5, RGBA16Float 16x16 with mips) and three samplers bound to one
//                      draw, nothing checked (for decoding texture and sampler resources)
//   mtlprobe texdraw   a 4x4 RGBA8 texture sampled (nearest, clamp) over a full-screen quad into 64x64: each 16x16 block one texel, checked
//   mtlprobe blend     four quadrants over a cleared target: source-over, additive, blend-constant source, write mask red+alpha; checked
//   mtlprobe blendx    the same with separate-alpha cases and a blend colour whose alpha (0.25) differs from its RGB; checked
//   mtlprobe blendinfo six pipelines differing only in blending and write mask, one draw each, nothing checked (for decoding blend state)
//   mtlprobe drawargs  draws with distinct arguments, nothing checked (for decoding the draw commands)
//   mtlprobe render    one render pass that clears a 64x64 BGRA texture to (0.25, 0.5, 0.75, 1)
//   mtlprobe texsize   a 4x2 texture through a constexpr sampler over 64x64, its width and height (get_width, get_height) in alpha, the right
//                      quarter discarded; checked
//   mtlprobe surfdraw  an IOSurface-backed render target (64x64 BGRA8, rows 320 bytes apart): cleared, a quad drawn over its left half; every
//                      pixel and the row padding (must stay untouched) checked through IOSurfaceLock
//   mtlprobe manyinst  1024 instances of a 2x2-pixel square into 64x64, each placed and coloured by its instance id; checked
//   mtlprobe viewport  iTerm2's kind of viewport (origin -64, 0, size 128x128) on 64x64: a quad over NDC [0, 1]^2 with a colour gradient fills
//                      the target; checked
//   mtlprobe texlinear a 4x4 texture sampled linearly (constexpr sampler, clamp to edge) at texel centres and halfway between them, written to
//                      a 7x7 grid of pixels (of a 64x64 target); checked against the CPU's bilinear values
//   mtlprobe fragpos   the fragment shader's [[position]] as its colour (x / 64, y / 64) over 64x64; checked
//   mtlprobe glyphs    iTerm2's text in miniature: 64 instances of an 8x8-pixel quad (two triangles from a vertex buffer, offsets and
//                      colours from an instance buffer) with three values to the fragment shader (float2 uv, float4 colour, float4 mask)
//                      over 64x64; every pixel checked
//   mtlprobe glyphs2 F iTerm2's text draw copied closely (its vertex shader's branches and uint2 viewport, a 4-channel glyph atlas sampled
//                      linearly, source-over blending, its 2x viewport) over 64x64; the raw BGRA8 pixels go to file F, to compare with
//                      another GPU's (not checked here)
//   mtlprobe coverage2 what Core Animation's shaders needed (2026-10-26): ushort vertex and instance ids, a half4 colour, saturate,
//                      sign, dfdx, fwidth, a sample with a texel offset from fragment texture 12, and vertex texture 9, over 64x64; checked
//   mtlprobe ctex      a compute kernel with textures (2026-10-26): reads texture 0, samples texture 3 through sampler 2, writes texture 5,
//                      and stores threadgroups_per_grid; 8x8 threads in 4x4 groups; checked
//   mtlprobe fbfetch   (2026-10-26) a fragment shader reading colour attachment 0 ([[color(0)]]) over a cleared 64x64 target, then one with no
//                      colour that writes a texture at its pixel; both checked
//   mtlprobe varyings  five values from the vertex to the fragment function over a 64x64 quad: float3 perspective (red), float without
//                      perspective (alpha), half2 centroid (green), uint and bool flat (blue: 0 in the first triangle, 0.65 in the second,
//                      from its first vertex); checked away from the diagonal
//   mtlprobe cube      3D: a cube in perspective (a matrix by setVertexBytes, positions from a buffer, an indexed draw, its hidden faces
//                      drawn last) with a Depth32Float buffer (less, written) into 64x64; each pixel's face checked against the CPU's
//                      rasterisation of the same triangles (a hidden face's colour must not appear)
//   mtlprobe spin [S]  3D in a window for S seconds (10): nine lit, textured cubes spinning with a depth buffer through a CAMetalLayer, the
//                      frames a second in the title and printed every second; not checked
//   mtlprobe all       all of them, each in its own child process (a crash or hang in one does not hide the others)
//
// Build: clang -arch x86_64 -mmacosx-version-min=11.0 -fobjc-arc tools/metal/m1/mtlprobe.m -framework Foundation -framework Metal -framework IOSurface \
//        -framework AppKit -framework QuartzCore -o mtlprobe
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <simd/simd.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../../userspace/rdna4-run/gfx12tricol.h"   // rdna4_tricol_check: mtlprobe native

#define OUT(fmt, ...) do { printf("mtlprobe: " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

// Commit, then wait for the completion handler for at most 5 s.
static void run(id<MTLCommandBuffer> cb, const char *what) {
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	[cb addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(done); }];
	NSDate *t0 = [NSDate date];
	[cb commit];
	long timedOut = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
	OUT("%s: %s after %.0f ms, status %ld, error %s", what, timedOut ? "NOT COMPLETED" : "completed", -[t0 timeIntervalSinceNow] * 1000,
	    (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none");
}

static void info(id<MTLDevice> d) {
	OUT("info: name \"%s\", registryID 0x%llx, low power %d, headless %d, unified memory %d", d.name.UTF8String, d.registryID, d.lowPower, d.headless,
	    d.hasUnifiedMemory);
	OUT("info: maxThreadsPerThreadgroup %lux%lux%lu, max buffer %lu MiB, recommended working set %llu MiB", (unsigned long)d.maxThreadsPerThreadgroup.width,
	    (unsigned long)d.maxThreadsPerThreadgroup.height, (unsigned long)d.maxThreadsPerThreadgroup.depth, (unsigned long)(d.maxBufferLength >> 20),
	    d.recommendedMaxWorkingSetSize >> 20);
	const struct { MTLGPUFamily f; const char *name; } families[] = {
		{MTLGPUFamilyCommon1, "Common1"}, {MTLGPUFamilyCommon2, "Common2"}, {MTLGPUFamilyCommon3, "Common3"}, {MTLGPUFamilyMac2, "Mac2"},
		{(MTLGPUFamily)5001, "Metal3"}, {MTLGPUFamilyApple5, "Apple5"}, {MTLGPUFamilyApple7, "Apple7"}};
	char line[256] = "";
	for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++)
		snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s %d", families[i].name, [d supportsFamily:families[i].f]);
	OUT("info: families:%s; argument buffers tier %ld, BC formats %d", line, (long)d.argumentBuffersSupport, d.supportsBCTextureCompression);
}

static void blit(id<MTLDevice> d, id<MTLCommandQueue> q) {
	id<MTLBuffer> a = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
	id<MTLBuffer> b = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
	memset(b.contents, 0, 4096);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe blit";
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e fillBuffer:a range:NSMakeRange(0, 4096) value:0x5a];
	[e copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:4096];
	[e endEncoding];
	run(cb, "blit");
	const uint8_t *p = b.contents;
	OUT("blit: destination bytes 0x%02x 0x%02x .. 0x%02x (0x5a = executed)", p[0], p[1], p[4095]);
}

static void compute(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"kernel void vadd(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],"
	                                              "                 device float *c [[buffer(2)]], uint i [[thread_position_in_grid]]) { c[i] = a[i] + b[i]; }"
	                                     options:nil
	                                       error:&err];
	OUT("compute: library %s%s", lib ? "compiled" : "NOT compiled: ", lib ? "" : err.localizedDescription.UTF8String);
	if (!lib)
		return;
	id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"vadd"] error:&err];
	OUT("compute: pipeline %s%s", ps ? "created" : "NOT created: ", ps ? "" : err.localizedDescription.UTF8String);
	if (!ps)
		return;
	id<MTLBuffer> a = [d newBufferWithLength:256 options:MTLResourceStorageModeShared], b = [d newBufferWithLength:256 options:MTLResourceStorageModeShared],
	              c = [d newBufferWithLength:256 options:MTLResourceStorageModeShared];
	float *pa = a.contents, *pb = b.contents, *pc = c.contents;
	for (int i = 0; i < 64; i++) {
		pa[i] = i;
		pb[i] = 1000;
		pc[i] = -1;
	}
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe compute";
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:ps];
	[e setBuffer:a offset:0 atIndex:0];
	[e setBuffer:b offset:0 atIndex:1];
	[e setBuffer:c offset:0 atIndex:2];
	[e dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
	[e endEncoding];
	run(cb, "compute");
	OUT("compute: c[0] %.0f c[1] %.0f c[63] %.0f (1000 1001 1063 = executed)", pc[0], pc[1], pc[63]);
}

static void grid(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"kernel void grid2d(device uint *out [[buffer(0)]], uint2 p [[thread_position_in_grid]], uint2 n [[threads_per_grid]]) {"
	                                              "\tout[p.y * n.x + p.x] = p.x + 1000 * p.y;\n}"
	                                     options:nil
	                                       error:&err];
	id<MTLComputePipelineState> ps = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"grid2d"] error:&err] : nil;
	OUT("grid: pipeline %s%s", ps ? "created" : "NOT created: ", ps ? "" : err.localizedDescription.UTF8String);
	if (!ps)
		return;
	enum { W = 50, H = 20 };
	id<MTLBuffer> out = [d newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
	uint32_t *p = out.contents;
	memset(p, 0xff, W * H * 4);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe grid";
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:ps];
	[e setBuffer:out offset:0 atIndex:0];
	[e dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)];
	[e endEncoding];
	run(cb, "grid");
	int wrong = 0;
	for (int i = 0; i < W * H; i++)
		wrong += p[i] != (uint32_t)(i % W + 1000 * (i / W));
	OUT("grid: out[0] %u out[50] %u out[999] %u, %d of %d wrong (0 1000 19049, 0 wrong = executed)", p[0], p[50], p[999], wrong, W * H);
}

static void mix(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"kernel void mix(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]], uint i [[thread_position_in_grid]],"
	                                              " uint g [[threadgroup_position_in_grid]], uint t [[thread_position_in_threadgroup]], uint n [[threads_per_threadgroup]]) {"
	                                              " out[i] = 3 * in[i] + 100000 * g + 1000 * t + n; }"
	                                     options:nil
	                                       error:&err];
	id<MTLComputePipelineState> ps = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"mix"] error:&err] : nil;
	OUT("mix: pipeline %s%s", ps ? "created" : "NOT created: ", ps ? "" : err.localizedDescription.UTF8String);
	if (!ps)
		return;
	enum { N = 100, G = 32 };
	id<MTLBuffer> in = [d newBufferWithLength:N * 4 options:MTLResourceStorageModeShared], out = [d newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
	uint32_t *pi = in.contents, *po = out.contents;
	for (int i = 0; i < N; i++) {
		pi[i] = i;
		po[i] = 0xffffffff;
	}
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe mix";
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:ps];
	[e setBuffer:in offset:0 atIndex:0];
	[e setBuffer:out offset:0 atIndex:1];
	[e dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(G, 1, 1)];
	[e endEncoding];
	run(cb, "mix");
	int wrong = 0;
	for (int i = 0; i < N; i++)
		wrong += po[i] != (uint32_t)(3 * i + 100000 * (i / G) + 1000 * (i % G) + (N - i / G * G < G ? N - i / G * G : G));
	OUT("mix: out[0] %u out[33] %u out[99] %u, %d of %d wrong (32 101131 303301, 0 wrong = executed)", po[0], po[33], po[99], wrong, N);
}

// Two pipelines in one command buffer: first(in -> out), then second(out in place); 64 threads; check(i) is out[i]'s expected value.
static void pair(id<MTLDevice> d, id<MTLCommandQueue> q, const char *what, NSArray<NSString *> *sources, NSString *first, NSString *second,
                 uint32_t (^check)(uint32_t)) {
	NSError *err = nil;
	id<MTLComputePipelineState> ps[2] = {nil, nil};
	NSString *names[2] = {first, second};
	for (int k = 0; k < 2; k++) {
		id<MTLLibrary> lib = [d newLibraryWithSource:sources[sources.count == 2 ? k : 0] options:nil error:&err];
		ps[k] = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:names[k]] error:&err] : nil;
		if (!ps[k]) {
			OUT("%s: pipeline %s NOT created: %s", what, names[k].UTF8String, err.localizedDescription.UTF8String);
			return;
		}
	}
	enum { N = 64 };
	id<MTLBuffer> in = [d newBufferWithLength:N * 4 options:MTLResourceStorageModeShared], out = [d newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
	uint32_t *pi = in.contents, *po = out.contents;
	for (uint32_t i = 0; i < N; i++) {
		pi[i] = i;
		po[i] = 0xffffffff;
	}
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = [NSString stringWithFormat:@"mtlprobe %s", what];
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:ps[0]];
	[e setBuffer:in offset:0 atIndex:0];
	[e setBuffer:out offset:0 atIndex:1];
	[e dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(N, 1, 1)];
	[e setComputePipelineState:ps[1]];
	[e setBuffer:out offset:0 atIndex:0];
	[e dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(N, 1, 1)];
	[e endEncoding];
	run(cb, what);
	int wrong = 0;
	for (uint32_t i = 0; i < N; i++)
		wrong += po[i] != check(i);
	OUT("%s: out[0] %u out[63] %u, %d of %d wrong (%u %u, 0 wrong = executed)", what, po[0], po[63], wrong, N, check(0), check(63));
}

// One kernel `name` from `src` over n threads in groups of g, with the given buffers at indices 0, 1, ...; false if it did not run.
static bool runKernel(id<MTLDevice> d, id<MTLCommandQueue> q, NSString *src, NSString *name, NSArray<id<MTLBuffer>> *buffers, NSUInteger n, NSUInteger g) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:[@"#include <metal_stdlib>\nusing namespace metal;\n" stringByAppendingString:src] options:nil error:&err];
	id<MTLComputePipelineState> ps = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&err] : nil;
	if (!ps) {
		OUT("feat: %s: NOT compiled: %s", name.UTF8String, err.localizedDescription.UTF8String);
		return false;
	}
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:ps];
	for (NSUInteger k = 0; k < buffers.count; k++)
		[e setBuffer:buffers[k] offset:0 atIndex:k];
	[e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(g, 1, 1)];
	[e endEncoding];
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	[cb addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(done); }];
	[cb commit];
	return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
}

static void feat(id<MTLDevice> d, id<MTLCommandQueue> q) {
	id<MTLBuffer> (^buf)(NSUInteger) = ^(NSUInteger bytes) { return [d newBufferWithLength:bytes options:MTLResourceStorageModeShared]; };
	int failed = 0;
	// Each check: the kernel's output against the CPU's, exactly or (fast math) within a relative 1e-3.
	void (^report)(const char *, BOOL, int, double, double) = ^(const char *what, BOOL ran, int bad, double got, double want) {
		OUT("feat: %-12s %s (first wrong: got %g, want %g)", what, !ran ? "NOT RUN" : bad ? "WRONG" : "ok", got, want);
	};
	{   // constant reference
		id<MTLBuffer> o = buf(256), k = buf(4);
		float *po = o.contents;
		for (int i = 0; i < 64; i++) po[i] = i;
		*(float *)k.contents = 2.5f;
		BOOL ran = runKernel(d, q, @"kernel void constref(device float *o [[buffer(0)]], constant float &k [[buffer(1)]], uint i [[thread_position_in_grid]]) { o[i] *= k; }", @"constref", @[o, k], 64, 64);
		int bad = 0; double got = 0, want = 0;
		for (int i = 0; i < 64; i++) if (po[i] != i * 2.5f && !bad++) { got = po[i]; want = i * 2.5f; }
		report("constref", ran, bad, got, want); failed += !ran || bad;
	}
	{   // constant struct
		id<MTLBuffer> o = buf(256), p = buf(8);
		float *po = o.contents;
		for (int i = 0; i < 64; i++) po[i] = i;
		((float *)p.contents)[0] = 0.5f; ((uint32_t *)p.contents)[1] = 40;
		BOOL ran = runKernel(d, q, @"struct P { float a; uint n; }; kernel void conststruct(device float *o [[buffer(0)]], constant P &p [[buffer(1)]], uint i [[thread_position_in_grid]]) { if (i < p.n) o[i] += p.a; }", @"conststruct", @[o, p], 64, 64);
		int bad = 0; double got = 0, want = 0;
		for (int i = 0; i < 64; i++) { float w = i < 40 ? i + 0.5f : i; if (po[i] != w && !bad++) { got = po[i]; want = w; } }
		report("conststruct", ran, bad, got, want); failed += !ran || bad;
	}
	{   // constant array
		id<MTLBuffer> o = buf(256), t = buf(32);
		float *po = o.contents, *pt = t.contents;
		for (int i = 0; i < 8; i++) pt[i] = 10 + i;
		BOOL ran = runKernel(d, q, @"kernel void constptr(device float *o [[buffer(0)]], constant float *t [[buffer(1)]], uint i [[thread_position_in_grid]]) { o[i] = t[i & 7]; }", @"constptr", @[o, t], 64, 64);
		int bad = 0; double got = 0, want = 0;
		for (int i = 0; i < 64; i++) if (po[i] != 10 + (i & 7) && !bad++) { got = po[i]; want = 10 + (i & 7); }
		report("constptr", ran, bad, got, want); failed += !ran || bad;
	}
	{   // math (fast math, Metal's default)
		id<MTLBuffer> o = buf(256);
		float *po = o.contents, x[64];
		for (int i = 0; i < 64; i++) po[i] = x[i] = 0.5f + i / 16.0f;
		BOOL ran = runKernel(d, q, @"kernel void math(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { float x = o[i]; o[i] = sqrt(x) + exp(x) + sin(x) + pow(x, 1.5f) + fabs(x) + max(x, 2.0f) + floor(x) + rsqrt(x) + log2(x); }", @"math", @[o], 64, 64);
		int bad = 0; double got = 0, want = 0;
		for (int i = 0; i < 64; i++) {
			double v = x[i], w = sqrt(v) + exp(v) + sin(v) + pow(v, 1.5) + fabs(v) + fmax(v, 2.0) + floor(v) + 1 / sqrt(v) + log2(v);
			if (fabs(po[i] - w) > 1e-3 * fabs(w) && !bad++) { got = po[i]; want = w; }
		}
		report("math", ran, bad, got, want); failed += !ran || bad;
	}
	{   // threadgroup memory and barriers: sums of 64 in a tree
		id<MTLBuffer> in = buf(1024), o = buf(16);
		float *pi = in.contents, *po = o.contents;
		for (int i = 0; i < 256; i++) pi[i] = i;
		BOOL ran = runKernel(d, q, @"kernel void tgreduce(device const float *in [[buffer(0)]], device float *o [[buffer(1)]], uint t [[thread_position_in_threadgroup]], uint g [[threadgroup_position_in_grid]], uint i [[thread_position_in_grid]]) {"
		                           " threadgroup float s[64]; s[t] = in[i]; threadgroup_barrier(mem_flags::mem_threadgroup);"
		                           " for (uint k = 32; k > 0; k >>= 1) { if (t < k) s[t] += s[t + k]; threadgroup_barrier(mem_flags::mem_threadgroup); }"
		                           " if (t == 0) o[g] = s[0]; }", @"tgreduce", @[in, o], 256, 64);
		int bad = 0; double got = 0, want = 0;
		for (int g = 0; g < 4; g++) { double w = 64 * 64 * g + 63 * 32; if (po[g] != w && !bad++) { got = po[g]; want = w; } }
		report("tgreduce", ran, bad, got, want); failed += !ran || bad;
	}
	{   // device atomics
		id<MTLBuffer> c = buf(4);
		*(uint32_t *)c.contents = 0;
		BOOL ran = runKernel(d, q, @"kernel void atomics(device atomic_uint *c [[buffer(0)]], uint i [[thread_position_in_grid]]) { atomic_fetch_add_explicit(c, i, memory_order_relaxed); }", @"atomics", @[c], 256, 64);
		uint32_t got = *(uint32_t *)c.contents;
		report("atomics", ran, got != 32640, got, 32640); failed += !ran || got != 32640;
	}
	{   // local array, indexed by data
		id<MTLBuffer> in = buf(256), o = buf(256);
		uint32_t *pi = in.contents, *po = o.contents;
		for (int i = 0; i < 64; i++) pi[i] = i * 3;
		BOOL ran = runKernel(d, q, @"kernel void localarr(device const uint *in [[buffer(0)]], device uint *o [[buffer(1)]], uint i [[thread_position_in_grid]]) { uint a[16]; for (int k = 0; k < 16; k++) a[k] = in[(i + k) & 63]; o[i] = a[in[i] & 15]; }", @"localarr", @[in, o], 64, 64);
		int bad = 0; double got = 0, want = 0;
		for (int i = 0; i < 64; i++) { uint32_t w = pi[(i + (pi[i] & 15)) & 63]; if (po[i] != w && !bad++) { got = po[i]; want = w; } }
		report("localarr", ran, bad, got, want); failed += !ran || bad;
	}
	{   // integer functions
		id<MTLBuffer> o = buf(256);
		int32_t *po = o.contents, x[64];
		for (int i = 0; i < 64; i++) po[i] = x[i] = i * 37 - 1000;
		BOOL ran = runKernel(d, q, @"kernel void ints(device int *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { int x = o[i]; o[i] = x / 7 + x % 5 + popcount(x) + clz(x) + (int)((uint)x >> 3) + min(x, 9) + abs(x); }", @"ints", @[o], 64, 64);
		int bad = 0; double got = 0, want = 0;
		for (int i = 0; i < 64; i++) {
			int32_t v = x[i], w = v / 7 + v % 5 + __builtin_popcount((uint32_t)v) + __builtin_clz((uint32_t)v) + (int32_t)((uint32_t)v >> 3) + (v < 9 ? v : 9) + (v < 0 ? -v : v);
			if (po[i] != w && !bad++) { got = po[i]; want = w; }
		}
		report("ints", ran, bad, got, want); failed += !ran || bad;
	}
	{   // half
		id<MTLBuffer> o = buf(128);
		_Float16 *po = o.contents;
		for (int i = 0; i < 64; i++) po[i] = (_Float16)(i * 0.25f);
		BOOL ran = runKernel(d, q, @"kernel void halfs(device half *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = o[i] * 2.0h + 1.0h; }", @"halfs", @[o], 64, 64);
		int bad = 0; double got = 0, want = 0;
		for (int i = 0; i < 64; i++) { float w = i * 0.5f + 1; if ((float)po[i] != w && !bad++) { got = po[i]; want = w; } }
		report("halfs", ran, bad, got, want); failed += !ran || bad;
	}
	{   // vector and common functions (fast math)
		id<MTLBuffer> o = buf(256);
		float *po = o.contents, x[64];
		for (int i = 0; i < 64; i++) po[i] = x[i] = 0.5f + i / 16.0f;
		BOOL ran = runKernel(d, q, @"kernel void vecmath(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { float x = o[i];"
		                           " o[i] = fmod(x, 0.7f) + clamp(float2(x, -x), 1.0f, 3.0f).x + clamp(float2(x, -x), -2.0f, 3.0f).y + dot(float4(x, 1, 2, 3), float4(2, x, 1, 0.5f))"
		                           " + mix(float4(x), float4(10.0f), 0.25f).z + float(clamp(int(x * 4), 3, 9)); }", @"vecmath", @[o], 64, 64);
		int bad = 0; double got = 0, want = 0;
		for (int i = 0; i < 64; i++) {
			double v = x[i], w = fmod(v, 0.7) + fmin(fmax(v, 1), 3) + fmin(fmax(-v, -2), 3) + (2 * v + v + 2 + 1.5) + (v + (10 - v) * 0.25);
			int n = (int)(v * 4);
			w += n < 3 ? 3 : n > 9 ? 9 : n;
			if (fabs(po[i] - w) > 1e-3 * fabs(w) + 1e-3 && !bad++) { got = po[i]; want = w; }
		}
		report("vecmath", ran, bad, got, want); failed += !ran || bad;
	}
	OUT("feat: %d of 10 wrong or not run (0 = executed)", failed);
}

static void bytes(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"kernel void scalebytes(device float *o [[buffer(0)]], constant float &k [[buffer(1)]], uint i [[thread_position_in_grid]]) { o[i] *= k; }"
	                                     options:nil
	                                       error:&err];
	id<MTLComputePipelineState> ps = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"scalebytes"] error:&err] : nil;
	if (!ps) {
		OUT("bytes: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	id<MTLBuffer> o = [d newBufferWithLength:256 options:MTLResourceStorageModeShared];
	float *po = o.contents;
	for (int i = 0; i < 64; i++)
		po[i] = i;
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe bytes";
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:ps];
	[e setBuffer:o offset:0 atIndex:0];
	const float three = 3, ten = 10;
	[e setBytes:&three length:4 atIndex:1];
	[e dispatchThreadgroups:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
	[e setBufferOffset:128 atIndex:0];
	[e setBytes:&ten length:4 atIndex:1];
	[e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
	[e endEncoding];
	run(cb, "bytes");
	int wrong = 0;
	for (int i = 0; i < 64; i++)
		wrong += po[i] != (i < 32 ? 3 * i : 30 * i);
	OUT("bytes: o[1] %.0f o[33] %.0f o[63] %.0f, %d of 64 wrong (3 990 1890, 0 wrong = executed)", po[1], po[33], po[63], wrong);
}

static void simd(id<MTLDevice> d, id<MTLCommandQueue> q) {
	enum { N = 40, K = 11 };
	id<MTLBuffer> o = [d newBufferWithLength:N * K * 4 options:MTLResourceStorageModeShared];
	memset(o.contents, 0xff, N * K * 4);
	BOOL ran = runKernel(d, q,
	    @"kernel void simdprobe(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]], uint lane [[thread_index_in_simdgroup]],"
	     " uint sg [[simdgroup_index_in_threadgroup]], uint w [[threads_per_simdgroup]], uint n [[simdgroups_per_threadgroup]]) {"
	     " uint x = 100 + i; float f = i * 0.5f; device uint *r = o + i * 11;"
	     " r[0] = simd_sum(x); r[1] = (uint)(simd_max(f) * 2); r[2] = simd_prefix_exclusive_sum(x);"
	     " r[3] = (uint)(simd_prefix_inclusive_sum(f) * 2); r[4] = simd_shuffle_xor(x, 1); r[5] = simd_shuffle_up(x, 3);"
	     " r[6] = simd_broadcast(x, 2); r[7] = (uint)(uint64_t)simd_ballot(x % 3 == 0);"
	     " r[8] = lane + 100 * sg + 10000 * w + 1000000 * n; r[9] = (simd_any(x == 105) ? 1 : 0) + (simd_all(x >= 100) ? 2 : 0);"
	     " r[10] = simd_shuffle_down(x, 3); }",
	    @"simdprobe", @[o], N, N);
	const uint32_t *p = o.contents;
	int bad = 0, first = -1;
	for (int i = 0; i < N; i++) {
		const int g0 = i / 32 * 32, gn = i < 32 ? 32 : N - 32, lane = i % 32;   // this lane's SIMD group: its first thread and its active count
		uint32_t want[11] = {0}, sum = 0, ex = 0, ball = 0;
		float mx = 0, inc = 0;
		for (int k = 0; k < gn; k++) {
			const uint32_t xk = 100 + g0 + k;
			sum += xk;
			mx = fmaxf(mx, (g0 + k) * 0.5f);
			if (k < lane) ex += xk;
			if (k <= lane) inc += (g0 + k) * 0.5f;
			if (xk % 3 == 0) ball |= 1u << k;
		}
		const uint32_t x = 100 + i;
		want[0] = sum; want[1] = (uint32_t)(mx * 2); want[2] = ex; want[3] = (uint32_t)(inc * 2); want[4] = 100 + g0 + (lane ^ 1);
		want[5] = lane >= 3 ? x - 3 : x; want[6] = 100 + g0 + 2; want[7] = ball; want[8] = lane + 100 * (i / 32) + 10000 * 32 + 1000000 * 2;
		bool any = false, all = true;
		for (int k = 0; k < gn; k++) { any |= 100 + g0 + k == 105; all &= 100 + g0 + k >= 100; }
		want[9] = (any ? 1 : 0) + (all ? 2 : 0);
		want[10] = lane + 3 < 32 ? x + 3 : x;
		for (int k = 0; k < 11; k++)
			if ((k != 10 || i < 32) && p[i * 11 + k] != want[k] && bad++ == 0) {
				first = i * 11 + k;
				OUT("simd: thread %d result %d: got %u, want %u", i, k, p[first], want[k]);
			}
	}
	OUT("simd: %s, %d of %d results wrong (0 = executed)", ran ? "ran" : "NOT RUN", bad, N * 11 - 8);
}

static void tri(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct V { float4 pos [[position]]; };\n"
	                                              "vertex V tri_v(uint vid [[vertex_id]]) { V v; v.pos = float4(vid == 0 ? -0.5 : vid == 1 ? 0.5 : 0.0, vid == 2 ? 0.5 : -0.5, 0, 1); return v; }\n"
	                                              "fragment float4 tri_f() { return float4(1, 0.5, 0.25, 1); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"tri_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"tri_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	OUT("tri: pipeline %s%s", ps ? "created" : "NOT created: ", ps ? "" : err.localizedDescription.UTF8String);
	if (!ps)
		return;
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	memset(out.contents, 0x11, 64 * 64 * 4);
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe tri";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "tri");
	const uint8_t *c = (const uint8_t *)out.contents + (32 * 64 + 32) * 4, *k = out.contents;
	int lit = 0;
	for (int i = 0; i < 64 * 64; i++)
		lit += ((const uint8_t *)out.contents)[i * 4 + 2] == 255;
	const uint8_t *top = (const uint8_t *)out.contents + (20 * 64 + 24) * 4, *bottom = (const uint8_t *)out.contents + (44 * 64 + 24) * 4;
	OUT("tri: centre B %u G %u R %u A %u, corner R %u, near the apex row 20 R %u, near the base row 44 R %u, %d pixels lit "
	    "(64 128 255 255, 0, 0, 255, 512 = executed, upright)", c[0], c[1], c[2], c[3], k[2], top[2], bottom[2], lit);
}

static void tricol(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct VC { float4 pos [[position]]; float4 col; };\n"
	                                              "vertex VC tricol_v(uint vid [[vertex_id]]) { VC v; v.pos = float4(vid == 0 ? -0.5 : vid == 1 ? 0.5 : 0.0, vid == 2 ? 0.5 : -0.5, 0, 1);"
	                                              " v.col = float4(vid == 0, vid == 1, vid == 2, 1); return v; }\n"
	                                              "fragment float4 tricol_f(VC in [[stage_in]]) { return in.col; }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"tricol_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"tricol_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("tricol: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe tricol";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "tricol");
	// The vertices in pixels: red (16, 48), green (48, 48), blue (32, 16); each lit pixel's colour = 255 x its barycentric weights at its centre.
	const uint8_t *p = out.contents;
	int lit = 0, wrong = 0, worst = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const uint8_t *px = p + (y * 64 + x) * 4;   // B, G, R, A
			if (!px[3])
				continue;
			lit++;
			const double cx = x + 0.5, cy = y + 0.5, area = (48.0 - 16) * (16 - 48.0) - (32 - 16.0) * (48 - 48.0);
			const double w0 = ((48 - cx) * (16 - cy) - (32 - cx) * (48 - cy)) / area, w1 = ((32 - cx) * (48 - cy) - (16 - cx) * (16 - cy)) / area;
			const double w[3] = {w0, w1, 1 - w0 - w1};
			const int got[3] = {px[2], px[1], px[0]};
			int err = px[3] == 255 ? 0 : 255;
			for (int k = 0; k < 3; k++) {
				const int e = abs(got[k] - (int)lround(255 * w[k]));
				err = e > err ? e : err;
			}
			worst = err > worst ? err : worst;
			wrong += err > 2;
		}
	const uint8_t *c = p + (40 * 64 + 32) * 4;
	OUT("tricol: %d pixels lit, %d off by more than 2, worst %d; pixel (32, 40) B %u G %u R %u A %u, (24, 20) A %u (512, 0, <= 2; A 0 = upright)",
	    lit, wrong, worst, c[0], c[1], c[2], c[3], p[(20 * 64 + 24) * 4 + 3]);
}

static void vbuf(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct V { float4 pos [[position]]; };\n"
	                                              "vertex V vb_v(uint vid [[vertex_id]], device const float2 *p [[buffer(0)]], constant float &scale [[buffer(1)]]) {"
	                                              " V v; v.pos = float4(p[vid] * scale, 0, 1); return v; }\n"
	                                              "fragment float4 vb_f(constant float4 &c [[buffer(0)]]) { return c; }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"vb_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"vb_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("vbuf: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	const float corners[6] = {-1, -1, 1, -1, -1, 1}, scale = 0.5f, colour[4] = {0.2f, 0.4f, 0.6f, 1};   // bottom-left, bottom-right, top-left
	id<MTLBuffer> p = [d newBufferWithBytes:corners length:sizeof(corners) options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe vbuf";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re setVertexBuffer:p offset:0 atIndex:0];
	[re setVertexBytes:&scale length:sizeof(scale) atIndex:1];
	[re setFragmentBytes:colour length:sizeof(colour) atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "vbuf");
	const uint8_t *px = out.contents;
	int lit = 0, wrongColour = 0;
	for (int i = 0; i < 64 * 64; i++)
		if (px[i * 4 + 3]) {
			lit++;
			wrongColour += abs(px[i * 4] - 153) > 1 || abs(px[i * 4 + 1] - 102) > 1 || abs(px[i * 4 + 2] - 51) > 1 || px[i * 4 + 3] != 255;
		}
	OUT("vbuf: %d pixels lit, %d of another colour; (20, 44) A %u, (44, 20) A %u (496, 0, 255, 0 = executed, as Apple's GPU)", lit, wrongColour,
	    px[(44 * 64 + 20) * 4 + 3], px[(20 * 64 + 44) * 4 + 3]);
}

static void drawmodes(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct V { float4 pos [[position]]; };\n"
	                                              "vertex V dm_v(uint vid [[vertex_id]], uint iid [[instance_id]], device const float2 *p [[buffer(0)]]) {"
	                                              " V v; v.pos = float4(p[vid] + float2(iid * 0.5, 0), 0, 1); return v; }\n"
	                                              "fragment float4 dm_f(constant float4 &c [[buffer(0)]]) { return c; }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"dm_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"dm_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("drawmodes: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	const float p[22] = {-0.9f, -0.9f, -0.6f, -0.9f, -0.9f, -0.6f,                              // 0-2: a small triangle, bottom left
	                     -0.9f, 0.3f, -0.3f, 0.3f, -0.9f, 0.9f, -0.3f, 0.9f,                     // 3-6: a quad as a strip, top left
	                     0.3f, 0.3f, 0.9f, 0.3f, 0.3f, 0.9f, 0.9f, 0.9f};                        // 7-10: a quad by indices, top right
	const uint16_t idx[8] = {0, 0, 7, 8, 9, 9, 8, 10};                                          // two dummies, then the quad from offset 4
	const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, yellow[4] = {1, 1, 0, 1};
	id<MTLBuffer> pb = [d newBufferWithBytes:p length:sizeof(p) options:MTLResourceStorageModeShared];
	id<MTLBuffer> ib = [d newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe drawmodes";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re setVertexBuffer:pb offset:0 atIndex:0];
	[re setFragmentBytes:red length:16 atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:3 vertexCount:4];
	[re setFragmentBytes:green length:16 atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:3];
	[re setFragmentBytes:yellow length:16 atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:1 baseInstance:3];
	[re setFragmentBytes:blue length:16 atIndex:0];
	[re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:4];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "drawmodes");
	const uint32_t *px = out.contents;
	int n[5] = {0};   // red, green, blue, yellow, other lit
	for (int i = 0; i < 64 * 64; i++)
		switch (px[i]) {   // BGRA in memory, read little-endian as 0xAARRGGBB
		case 0xffff0000u: n[0]++; break;
		case 0xff00ff00u: n[1]++; break;
		case 0xff0000ffu: n[2]++; break;
		case 0xffffff00u: n[3]++; break;
		default: n[4] += px[i] != 0;
		}
	OUT("drawmodes: red %d, green %d, yellow %d, blue %d, other %d (361, 135, 45, 361, 0 = executed, as Apple's GPU)", n[0], n[1], n[3], n[2], n[4]);
}

static void texinfo(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct V { float4 pos [[position]]; };\n"
	                                              "vertex V ti_v(uint vid [[vertex_id]]) { V v; v.pos = float4(vid == 1 ? 1.0 : -1.0, vid == 2 ? 1.0 : -1.0, 0, 1); return v; }\n"
	                                              "fragment float4 ti_f(texture2d<float> a [[texture(0)]], texture2d<float> b [[texture(1)]], texture2d<float> c [[texture(2)]],"
	                                              " texture2d<float> e [[texture(3)]], sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]], sampler s2 [[sampler(2)]]) {"
	                                              " float2 uv = float2(0.5); return a.sample(s0, uv) + b.sample(s1, uv) + c.sample(s2, uv) + e.sample(s0, uv); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"ti_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"ti_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("texinfo: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	const MTLPixelFormat fmt[4] = {MTLPixelFormatRGBA8Unorm, MTLPixelFormatBGRA8Unorm, MTLPixelFormatR32Float, MTLPixelFormatRGBA16Float};
	const NSUInteger w[4] = {4, 8, 3, 16}, h[4] = {4, 2, 5, 16};
	id<MTLTexture> tex[4];
	for (int k = 0; k < 4; k++) {
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt[k] width:w[k] height:h[k] mipmapped:k == 3];
		td.storageMode = MTLStorageModeManaged;
		tex[k] = [d newTextureWithDescriptor:td];
		uint8_t bytes[16 * 16 * 8];
		for (unsigned i = 0; i < sizeof(bytes); i++)
			bytes[i] = (uint8_t)(i * 7 + k);
		[tex[k] replaceRegion:MTLRegionMake2D(0, 0, w[k], h[k]) mipmapLevel:0 withBytes:bytes bytesPerRow:w[k] * (k == 3 ? 8 : 4)];
		OUT("texinfo: texture %d: %lux%lu format %lu, mips %lu", k, (unsigned long)w[k], (unsigned long)h[k], (unsigned long)fmt[k], (unsigned long)tex[k].mipmapLevelCount);
	}
	MTLSamplerDescriptor *sd[3] = {[MTLSamplerDescriptor new], [MTLSamplerDescriptor new], [MTLSamplerDescriptor new]};
	sd[0].minFilter = sd[0].magFilter = MTLSamplerMinMagFilterNearest;   // clamp to edge (default)
	sd[1].minFilter = sd[1].magFilter = MTLSamplerMinMagFilterLinear;
	sd[1].mipFilter = MTLSamplerMipFilterLinear;
	sd[1].sAddressMode = sd[1].tAddressMode = MTLSamplerAddressModeRepeat;
	sd[2].sAddressMode = MTLSamplerAddressModeMirrorRepeat;
	sd[2].tAddressMode = MTLSamplerAddressModeClampToZero;
	sd[2].maxAnisotropy = 4;
	id<MTLSamplerState> ss[3];
	for (int k = 0; k < 3; k++)
		ss[k] = [d newSamplerStateWithDescriptor:sd[k]];
	MTLTextureDescriptor *rtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	rtd.usage = MTLTextureUsageRenderTarget;
	rtd.storageMode = MTLStorageModePrivate;
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = [d newTextureWithDescriptor:rtd];
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe texinfo";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	for (int k = 0; k < 4; k++)
		[re setFragmentTexture:tex[k] atIndex:k];
	for (int k = 0; k < 3; k++)
		[re setFragmentSamplerState:ss[k] atIndex:k];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[re endEncoding];
	run(cb, "texinfo");
}

static void texdraw(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct VT { float4 pos [[position]]; float2 uv; };\n"
	                                              "vertex VT td_v(uint vid [[vertex_id]]) { float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : -1.0;"
	                                              " float y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : -1.0; VT v; v.pos = float4(x, y, 0, 1);"
	                                              " v.uv = float2(x * 0.5 + 0.5, 0.5 - y * 0.5); return v; }\n"
	                                              "fragment float4 td_f(VT in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {"
	                                              " return t.sample(s, in.uv); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"td_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"td_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("texdraw: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *xd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
	xd.storageMode = MTLStorageModeManaged;
	id<MTLTexture> tex = [d newTextureWithDescriptor:xd];
	uint8_t texels[4 * 4 * 4];
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++) {
			uint8_t *e = texels + (y * 4 + x) * 4;
			e[0] = (uint8_t)(x * 60 + 20);   // R
			e[1] = (uint8_t)(y * 60 + 20);   // G
			e[2] = 200;                      // B
			e[3] = 255;
		}
	[tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:16];
	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];   // nearest, clamp to edge
	id<MTLSamplerState> ss = [d newSamplerStateWithDescriptor:sd];
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe texdraw";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re setFragmentTexture:tex atIndex:0];
	[re setFragmentSamplerState:ss atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "texdraw");
	const uint8_t *px = out.contents;
	int wrong = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const uint8_t *p = px + (y * 64 + x) * 4, want[4] = {200, (uint8_t)(y / 16 * 60 + 20), (uint8_t)(x / 16 * 60 + 20), 255};   // B, G, R, A
			wrong += memcmp(p, want, 4) != 0;
		}
	const uint8_t *c = px + (40 * 64 + 20) * 4;
	OUT("texdraw: %d of 4096 pixels wrong; pixel (20, 40) B %u G %u R %u A %u (0, 200 140 80 255 = executed)", wrong, c[0], c[1], c[2], c[3]);
}

static void blend(id<MTLDevice> d, id<MTLCommandQueue> q, bool x) {   // x: the separate-alpha cases, blend colour alpha 0.25
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct V { float4 pos [[position]]; };\n"
	                                              "vertex V bq_v(uint vid [[vertex_id]], constant float2 &o [[buffer(0)]]) {"
	                                              " float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : 0.0, y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : 0.0;"
	                                              " V v; v.pos = float4(o.x + x, o.y + y, 0, 1); return v; }\n"
	                                              "fragment float4 bq_f(constant float4 &c [[buffer(0)]]) { return c; }"
	                                     options:nil
	                                       error:&err];
	// {enabled, rgb src, rgb dst, alpha src, alpha dst, write mask}; operations add
	const NSUInteger cfg[4][6] = {
		{1, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, 0xf},
		{1, MTLBlendFactorOne, MTLBlendFactorOne, MTLBlendFactorOne, MTLBlendFactorOne, 0xf},
		{1, MTLBlendFactorBlendColor, MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorZero, 0xf},
		{0, MTLBlendFactorOne, MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorZero, MTLColorWriteMaskRed | MTLColorWriteMaskAlpha}};
	const NSUInteger cfgx[4][6] = {
		{1, MTLBlendFactorBlendColor, MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorZero, 0xf},
		{1, MTLBlendFactorOne, MTLBlendFactorZero, MTLBlendFactorZero, MTLBlendFactorOne, 0xf},
		{1, MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorOne, MTLBlendFactorZero, 0xf},
		{1, MTLBlendFactorBlendColor, MTLBlendFactorZero, MTLBlendFactorBlendColor, MTLBlendFactorZero, 0xf}};
	const NSUInteger (*set)[6] = x ? cfgx : cfg;
	id<MTLRenderPipelineState> ps[4];
	for (int k = 0; k < 4; k++) {
		MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = [lib newFunctionWithName:@"bq_v"];
		pd.fragmentFunction = [lib newFunctionWithName:@"bq_f"];
		pd.label = @"Pipeline for mtlprobe blend";   // a label sits before the blend state in the descriptor (iTerm2's pipelines have one)
		MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[0];
		a.pixelFormat = MTLPixelFormatBGRA8Unorm;
		a.blendingEnabled = set[k][0];
		a.sourceRGBBlendFactor = set[k][1];
		a.destinationRGBBlendFactor = set[k][2];
		a.sourceAlphaBlendFactor = set[k][3];
		a.destinationAlphaBlendFactor = set[k][4];
		a.writeMask = set[k][5];
		ps[k] = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
		if (!ps[k]) {
			OUT("blend: pipeline %d NOT created: %s", k, err.localizedDescription.UTF8String);
			return;
		}
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 1);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe blend";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setBlendColorRed:0.5 green:0.5 blue:0.5 alpha:x ? 0.25 : 0.5];
	const float colour[4] = {1, 0.5f, 0.25f, 0.5f}, origin[4][2] = {{-1, 0}, {0, 0}, {-1, -1}, {0, -1}};   // TL, TR, BL, BR
	for (int k = 0; k < 4; k++) {
		[re setRenderPipelineState:ps[k]];
		[re setVertexBytes:origin[k] length:8 atIndex:0];
		[re setFragmentBytes:colour length:16 atIndex:0];
		[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	}
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, x ? "blendx" : "blend");
	// Expected RGBA (0..1) per quadrant: d = (0.2, 0.4, 0.6, 1), s = (1, 0.5, 0.25, 0.5), K = 0.5 (x: alpha 0.25)
	const double wantn[4][4] = {{0.6, 0.45, 0.425, 0.75}, {1, 0.9, 0.85, 1}, {0.5, 0.25, 0.125, 0.5}, {1, 0.4, 0.6, 0.5}},
	             wantx[4][4] = {{0.5, 0.25, 0.125, 0.5}, {1, 0.5, 0.25, 1}, {0.2, 0.4, 0.6, 0.5}, {0.5, 0.25, 0.125, 0.125}};
	const double (*want)[4] = x ? wantx : wantn;
	const char *const names[2][4] = {{"over", "add", "const", "mask"}, {"constrgb", "keepalpha", "keeprgb", "constall"}};
	const int at[4][2] = {{16, 16}, {48, 16}, {16, 48}, {48, 48}};
	int wrong = 0;
	char line[256] = "", expect[256] = "";
	for (int k = 0; k < 4; k++) {
		const uint8_t *p = (const uint8_t *)out.contents + (at[k][1] * 64 + at[k][0]) * 4;   // B, G, R, A
		const int got[4] = {p[2], p[1], p[0], p[3]};
		int w[4];
		for (int ch = 0; ch < 4; ch++) {
			w[ch] = (int)lround(want[k][ch] * 255);
			wrong += abs(got[ch] - w[ch]) > 2;
		}
		snprintf(line + strlen(line), sizeof(line) - strlen(line), " %s %d %d %d %d;", names[x][k], got[0], got[1], got[2], got[3]);
		snprintf(expect + strlen(expect), sizeof(expect) - strlen(expect), "%s%s %d %d %d %d", k ? ", " : "", names[x][k], w[0], w[1], w[2], w[3]);
	}
	OUT("%s: RGBA%s %d of 16 channels off by more than 2 (%s)", x ? "blendx" : "blend", line, wrong, expect);
}

// Dual-source blending (Sublime Text's subpixel text): a quad whose colour and second source ([[color(0), index(1)]]) depend on each
// pixel's x and y parity, blended RGB = colour * source1 + destination * (1 - source1) over a known clear; every pixel checked, the
// mismatches counted by parity (gfx11+ exports the two sources swizzled across lane pairs: a wrong swizzle shows on alternate pixels).
static void dualsrc(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct V { float4 pos [[position]]; };\n"
	                                              "struct O { float4 c [[color(0), index(0)]]; float4 k [[color(0), index(1)]]; };\n"
	                                              "vertex V ds_v(uint vid [[vertex_id]]) {"
	                                              " float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : -1.0, y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : -1.0;"
	                                              " V v; v.pos = float4(x, y, 0, 1); return v; }\n"
	                                              "fragment O ds_f(V in [[stage_in]], constant uint &m [[buffer(0)]]) { uint x = uint(in.pos.x), y = uint(in.pos.y); O o;"
	                                              " o.c = float4((x & 1) ? 1.0 : 0.0, (y & 1) ? 1.0 : 0.0, (x & 2) ? 1.0 : 0.5, 1.0);"
	                                              " o.k = float4((x & 1) ? 0.25 : 0.75, (y & 1) ? 0.5 : 1.0, (x & 2) ? 0.0 : 1.0, 1.0);"
	                                              " if (m == 1) o.k = float4(1.0); if (m == 2) o.c = float4(1.0); return o; }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"ds_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"ds_f"];
	MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[0];
	a.pixelFormat = MTLPixelFormatBGRA8Unorm;
	a.blendingEnabled = YES;
	a.sourceRGBBlendFactor = MTLBlendFactorSource1Color;
	a.destinationRGBBlendFactor = MTLBlendFactorOneMinusSource1Color;
	a.sourceAlphaBlendFactor = MTLBlendFactorSource1Color;
	a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSource1Color;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("dualsrc: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	int total = 0;
	// m 0: as designed; 1: second source 1 everywhere (the result is the colour each pixel got); 2: colour 1 over black (the result is
	// the second source each pixel got)
	for (uint32_t m = 0; m < 3; m++) {
		const double dst[4] = {m == 2 ? 0 : 0.2, m == 2 ? 0 : 0.4, m == 2 ? 0 : 0.6, m == 2 ? 0 : 1};
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = t;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(dst[0], dst[1], dst[2], dst[3]);
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
		[re setRenderPipelineState:ps];
		[re setFragmentBytes:&m length:4 atIndex:0];
		[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
		[re endEncoding];
		id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
		[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
		      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
		[e endEncoding];
		run(cb, "dualsrc");
		int wrong[4] = {0};
		for (int y = 0; y < 64; y++)
			for (int x = 0; x < 64; x++) {
				double c[4] = {(x & 1) ? 1.0 : 0.0, (y & 1) ? 1.0 : 0.0, (x & 2) ? 1.0 : 0.5, 1.0};
				double k[4] = {(x & 1) ? 0.25 : 0.75, (y & 1) ? 0.5 : 1.0, (x & 2) ? 0.0 : 1.0, 1.0};
				for (int ch = 0; ch < 4; ch++) {
					k[ch] = m == 1 ? 1 : k[ch];
					c[ch] = m == 2 ? 1 : c[ch];
				}
				const uint8_t *p = (const uint8_t *)out.contents + (y * 64 + x) * 4;   // B, G, R, A
				const int got[4] = {p[2], p[1], p[0], p[3]};
				int bad = 0;
				for (int ch = 0; ch < 4; ch++)
					bad |= abs(got[ch] - (int)lround((c[ch] * k[ch] + dst[ch] * (1 - k[ch])) * 255)) > 2;
				wrong[(x & 1) | (y & 1) << 1] += bad;
			}
		total += wrong[0] + wrong[1] + wrong[2] + wrong[3];
		char row[2][160] = {"", ""};
		for (int y = 0; y < 2; y++)
			for (int x = 0; x < 4; x++) {
				const uint8_t *p = (const uint8_t *)out.contents + (y * 64 + x) * 4;
				snprintf(row[y] + strlen(row[y]), sizeof(row[y]) - strlen(row[y]), " (%3d %3d %3d %3d)", p[2], p[1], p[0], p[3]);
			}
		OUT("dualsrc: m %u (%s): wrong by parity (x even y even, x odd y even, x even y odd, x odd y odd) %d %d %d %d; pixels y0%s; y1%s", m,
		    m == 0 ? "as designed" : m == 1 ? "source1 = 1: shows the colour" : "colour 1 over black: shows source1", wrong[0], wrong[1], wrong[2],
		    wrong[3], row[0], row[1]);
	}
	OUT("dualsrc: %s", total ? "FAIL" : "PASS");
}

static void blendinfo(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct V { float4 pos [[position]]; };\n"
	                                              "vertex V bi_v(uint vid [[vertex_id]]) { V v; v.pos = float4(vid == 1 ? 0.5 : -0.5, vid == 2 ? 0.5 : -0.5, 0, 1); return v; }\n"
	                                              "fragment float4 bi_f() { return float4(1, 0.5, 0.25, 0.5); }"
	                                     options:nil
	                                       error:&err];
	// {enabled, rgb src, rgb dst, rgb op, alpha src, alpha dst, alpha op, write mask}
	const NSUInteger cfg[6][8] = {
		{0, 1, 0, 0, 1, 0, 0, 0xf},
		{1, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendOperationAdd, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendOperationAdd, 0xf},
		{1, MTLBlendFactorOne, MTLBlendFactorOne, MTLBlendOperationSubtract, MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendOperationReverseSubtract, 0xf},
		{1, MTLBlendFactorDestinationColor, MTLBlendFactorZero, MTLBlendOperationMin, MTLBlendFactorBlendAlpha, MTLBlendFactorOneMinusBlendAlpha, MTLBlendOperationMax, 0xf},
		{0, 1, 0, 0, 1, 0, 0, MTLColorWriteMaskRed | MTLColorWriteMaskAlpha},
		{1, MTLBlendFactorSourceAlphaSaturated, MTLBlendFactorOneMinusDestinationAlpha, MTLBlendOperationAdd, MTLBlendFactorOne, MTLBlendFactorZero, MTLBlendOperationAdd, 0xf}};
	id<MTLRenderPipelineState> ps[6];
	for (int k = 0; k < 6; k++) {
		MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = [lib newFunctionWithName:@"bi_v"];
		pd.fragmentFunction = [lib newFunctionWithName:@"bi_f"];
		MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[0];
		a.pixelFormat = MTLPixelFormatBGRA8Unorm;
		a.blendingEnabled = cfg[k][0];
		a.sourceRGBBlendFactor = cfg[k][1];
		a.destinationRGBBlendFactor = cfg[k][2];
		a.rgbBlendOperation = cfg[k][3];
		a.sourceAlphaBlendFactor = cfg[k][4];
		a.destinationAlphaBlendFactor = cfg[k][5];
		a.alphaBlendOperation = cfg[k][6];
		a.writeMask = cfg[k][7];
		ps[k] = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
		if (!ps[k]) {
			OUT("blendinfo: pipeline %d NOT created: %s", k, err.localizedDescription.UTF8String);
			return;
		}
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = [d newTextureWithDescriptor:td];
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe blendinfo";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setBlendColorRed:0.1 green:0.2 blue:0.3 alpha:0.4];
	for (int k = 0; k < 6; k++) {
		[re setRenderPipelineState:ps[k]];
		[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	}
	[re endEncoding];
	run(cb, "blendinfo");
}

static void drawargs(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct V { float4 pos [[position]]; };\n"
	                                              "vertex V args_v(uint vid [[vertex_id]], uint iid [[instance_id]]) { V v; v.pos = float4((vid % 3) * 0.1, iid * 0.1, 0, 1); return v; }\n"
	                                              "fragment float4 args_f() { return float4(0, 1, 0, 1); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"args_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"args_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("drawargs: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = [d newTextureWithDescriptor:td];
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	id<MTLBuffer> idx = [d newBufferWithLength:64 options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe drawargs";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:5 vertexCount:7];
	[re drawPrimitives:MTLPrimitiveTypeLine vertexStart:2 vertexCount:6 instanceCount:3];
	[re drawPrimitives:MTLPrimitiveTypeLineStrip vertexStart:9 vertexCount:4 instanceCount:2 baseInstance:4];
	[re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:idx indexBufferOffset:8];
	[re endEncoding];
	run(cb, "drawargs");
}

static void render(id<MTLDevice> d, id<MTLCommandQueue> q) {
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	memset(out.contents, 0, 64 * 64 * 4);
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe render";
	[[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "render");
	const uint8_t *p = out.contents;
	OUT("render: first pixel B %u G %u R %u A %u (191 128 64 255 = executed)", p[0], p[1], p[2], p[3]);
}

static void texsize(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct VT { float4 pos [[position]]; float2 uv; };\n"
	                                              "vertex VT ts_v(uint vid [[vertex_id]]) { float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : -1.0;"
	                                              " float y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : -1.0; VT v; v.pos = float4(x, y, 0, 1);"
	                                              " v.uv = float2(x * 0.5 + 0.5, 0.5 - y * 0.5); return v; }\n"
	                                              "fragment float4 ts_f(VT in [[stage_in]], texture2d<float> t [[texture(0)]]) {"
	                                              " constexpr sampler s(filter::nearest, address::clamp_to_edge);"
	                                              " if (in.uv.x > 0.75) discard_fragment();"
	                                              " return float4(t.sample(s, in.uv).rgb, float(t.get_width() * 10 + t.get_height()) / 255.0); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"ts_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"ts_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("texsize: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *xd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:2 mipmapped:NO];
	xd.storageMode = MTLStorageModeManaged;
	id<MTLTexture> tex = [d newTextureWithDescriptor:xd];
	uint8_t texels[4 * 2 * 4];
	for (int y = 0; y < 2; y++)
		for (int x = 0; x < 4; x++) {
			uint8_t *e = texels + (y * 4 + x) * 4;
			e[0] = (uint8_t)(x * 60 + 20);
			e[1] = (uint8_t)(y * 60 + 20);
			e[2] = 200;
			e[3] = 255;
		}
	[tex replaceRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0 withBytes:texels bytesPerRow:16];
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe texsize";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re setFragmentTexture:tex atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "texsize");
	const uint8_t *px = out.contents;
	int wrong = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const uint8_t *p = px + (y * 64 + x) * 4, kept[4] = {200, (uint8_t)(y / 32 * 60 + 20), (uint8_t)(x / 16 * 60 + 20), 42}, gone[4] = {0, 0, 0, 0};
			wrong += memcmp(p, x >= 48 ? gone : kept, 4) != 0;   // B, G, R, A
		}
	const uint8_t *a = px + (40 * 64 + 20) * 4, *b = px + (10 * 64 + 56) * 4;
	OUT("texsize: %d of 4096 pixels wrong; (20, 40) B %u G %u R %u A %u, (56, 10) A %u (0, 200 80 80 42, 0 = executed)", wrong, a[0], a[1], a[2], a[3], b[3]);
}

static void surfdraw(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "vertex float4 sd_v(uint vid [[vertex_id]]) { float x = (vid == 1 || vid == 3 || vid == 4) ? 0.0 : -1.0;"
	                                              " float y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : -1.0; return float4(x, y, 0, 1); }\n"
	                                              "fragment float4 sd_f() { return float4(1, 0.5, 0.25, 1); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"sd_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"sd_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("surfdraw: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	const size_t bpr = 320;
	IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef) @{(id)kIOSurfaceWidth: @64, (id)kIOSurfaceHeight: @64, (id)kIOSurfaceBytesPerElement: @4,
	                                                                 (id)kIOSurfaceBytesPerRow: @(bpr), (id)kIOSurfacePixelFormat: @(0x42475241)});
	if (!surf || IOSurfaceGetBytesPerRow(surf) != bpr) {
		OUT("surfdraw: IOSurface NOT created as asked (%zu bytes per row)", surf ? IOSurfaceGetBytesPerRow(surf) : 0);
		return;
	}
	IOSurfaceLock(surf, 0, NULL);
	memset(IOSurfaceGetBaseAddress(surf), 0x5a, bpr * 64);   // the padding keeps this
	IOSurfaceUnlock(surf, 0, NULL);
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModeManaged;
	id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:surf plane:0];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe surfdraw";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	run(cb, "surfdraw");
	IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *base = IOSurfaceGetBaseAddress(surf);
	int wrong = 0, padding = 0;
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			const uint8_t *p = base + y * bpr + x * 4, left[4] = {64, 128, 255, 255}, right[4] = {191, 128, 64, 255};   // B, G, R, A
			wrong += memcmp(p, x < 32 ? left : right, 4) != 0;
		}
		for (size_t k = 256; k < bpr; k++)
			padding += base[y * bpr + k] != 0x5a;
	}
	const uint8_t *a = base + 40 * bpr + 10 * 4, *b = base + 40 * bpr + 50 * 4;
	OUT("surfdraw: %d of 4096 pixels wrong, %d padding bytes changed; (10, 40) B %u G %u R %u A %u, (50, 40) B %u G %u R %u A %u (0, 0 = executed)", wrong,
	    padding, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
	CFRelease(surf);
}

static void manyinst(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct VI { float4 pos [[position]]; float4 c [[flat]]; };\n"
	                                              "vertex VI mi_v(uint vid [[vertex_id]], uint iid [[instance_id]]) {"
	                                              " float x = float(iid % 32) * 2 + ((vid == 1 || vid == 3 || vid == 4) ? 2.0 : 0.0);"
	                                              " float y = float(iid / 32) * 2 + ((vid == 2 || vid == 4 || vid == 5) ? 2.0 : 0.0);"
	                                              " VI v; v.pos = float4(x / 32 - 1, 1 - y / 32, 0, 1); v.c = float4(float(iid % 32) * 8 / 255, float(iid / 32) * 8 / 255, 1, 1); return v; }\n"
	                                              "fragment float4 mi_f(VI in [[stage_in]]) { return in.c; }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"mi_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"mi_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("manyinst: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe manyinst";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:1024];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "manyinst");
	const uint8_t *px = out.contents;
	int wrong = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const uint8_t *p = px + (y * 64 + x) * 4, want[4] = {255, (uint8_t)(y / 2 * 8), (uint8_t)(x / 2 * 8), 255};   // B, G, R, A
			wrong += memcmp(p, want, 4) != 0;
		}
	int firstBad = -1, badInstances = 0;   // an instance is its 2x2 square
	for (int i = 0; i < 1024; i++) {
		const uint8_t *p = px + ((i / 32) * 2 * 64 + (i % 32) * 2) * 4, want[4] = {255, (uint8_t)(i / 32 * 8), (uint8_t)(i % 32 * 8), 255};
		if (memcmp(p, want, 4)) {
			badInstances++;
			if (firstBad < 0)
				firstBad = i;
		}
	}
	const uint8_t *a = px + (63 * 64 + 63) * 4;
	OUT("manyinst: %d of 4096 pixels wrong, %d of 1024 instances (first %d); (63, 63) B %u G %u R %u A %u (0, 255 248 248 255 = executed)", wrong,
	    badInstances, firstBad, a[0], a[1], a[2], a[3]);
}

static void viewport(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct VP { float4 pos [[position]]; float2 c; };\n"
	                                              "vertex VP vp_v(uint vid [[vertex_id]]) { float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : 0.0;"
	                                              " float y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : 0.0; VP v; v.pos = float4(x, y, 0, 1); v.c = float2(x, y); return v; }\n"
	                                              "fragment float4 vp_f(VP in [[stage_in]]) { return float4(in.c.x, in.c.y, 0.5, 1); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"vp_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"vp_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("viewport: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe viewport";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setViewport:(MTLViewport){-64, 0, 128, 128, 0, 1}];
	[re setRenderPipelineState:ps];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "viewport");
	// Pixel (px, py): NDC x = (px + 0.5) / 64, y = 1 - (py + 0.5) / 64, the gradient the same.
	const uint8_t *px = out.contents;
	int wrong = 0, worst = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const uint8_t *p = px + (y * 64 + x) * 4;
			const int want[4] = {128, (int)lround((1 - (y + 0.5) / 64) * 255), (int)lround((x + 0.5) / 64 * 255), 255};   // B, G, R, A
			int off = 0;
			for (int c = 0; c < 4; c++)
				off = MAX(off, abs(p[c] - want[c]));
			worst = MAX(worst, off);
			wrong += off > 2;
		}
	const uint8_t *a = px + (8 * 64 + 8) * 4;
	OUT("viewport: %d of 4096 pixels off by more than 2, worst %d; (8, 8) B %u G %u R %u A %u (0 = executed)", wrong, worst, a[0], a[1], a[2], a[3]);
}

static void texlinear(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	// Pixel (x, y) of the target (64x64, its first 7x7 checked: small targets have padded rows) samples u = (x + 1) / 8, v = (y + 1) / 8: odd x, y hit texel centres, even ones the midpoints.
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "vertex float4 tl_v(uint vid [[vertex_id]]) { float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : -1.0;"
	                                              " float y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : -1.0; return float4(x, y, 0, 1); }\n"
	                                              "fragment float4 tl_f(float4 p [[position]], texture2d<float> t [[texture(0)]]) {"
	                                              " constexpr sampler s(filter::linear, address::clamp_to_edge);"
	                                              " return t.sample(s, (floor(p.xy) + 1) / 8); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"tl_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"tl_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("texlinear: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *xd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
	xd.storageMode = MTLStorageModeManaged;
	id<MTLTexture> tex = [d newTextureWithDescriptor:xd];
	uint8_t texels[4 * 4 * 4];
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++) {
			uint8_t *e = texels + (y * 4 + x) * 4;
			e[0] = (uint8_t)(x * 80);   // R: 0 80 160 240 across
			e[1] = (uint8_t)(y * 80);   // G: down
			e[2] = (x & 1) ? 255 : 0;   // B: every other column
			e[3] = 255;
		}
	[tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:16];
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe texlinear";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re setFragmentTexture:tex atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "texlinear");
	// u = (x + 1) / 8 -> texel coordinate 4u - 0.5 = (x + 1) / 2 - 0.5: x odd a centre, x even halfway between two texels.
	const uint8_t *px = out.contents;
	int wrong = 0, worst = 0;
	char row[160] = "";
	for (int y = 0; y < 7; y++)
		for (int x = 0; x < 7; x++) {
			const double tx = (x + 1) / 2.0 - 0.5, ty = (y + 1) / 2.0 - 0.5;
			const int x0 = (int)tx, y0 = (int)ty;
			const double fx = tx - x0, fy = ty - y0;
			double want[3];
#define TEXEL(i, j) texels[(MIN((j), 3) * 4 + MIN((i), 3)) * 4 + c]
			for (int c = 0; c < 3; c++)
				want[c] = (TEXEL(x0, y0) * (1 - fx) + TEXEL(x0 + 1, y0) * fx) * (1 - fy) + (TEXEL(x0, y0 + 1) * (1 - fx) + TEXEL(x0 + 1, y0 + 1) * fx) * fy;
#undef TEXEL
			const uint8_t *p = px + (y * 64 + x) * 4;   // B, G, R, A
			const int off = MAX(MAX(abs(p[2] - (int)lround(want[0])), abs(p[1] - (int)lround(want[1]))), abs(p[0] - (int)lround(want[2])));
			worst = MAX(worst, off);
			wrong += off > 3;
			if (y == 3)
				snprintf(row + strlen(row), sizeof(row) - strlen(row), " %u/%u", p[2], p[0]);
		}
	OUT("texlinear: %d of 49 pixels off by more than 3, worst %d; row 3 R/B:%s (0 = executed)", wrong, worst, row);
}

static void fragpos(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "vertex float4 fp_v(uint vid [[vertex_id]]) { float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : -1.0;"
	                                              " float y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : -1.0; return float4(x, y, 0, 1); }\n"
	                                              "fragment float4 fp_f(float4 p [[position]]) { return float4(p.x / 64, p.y / 64, 0.5, 1); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"fp_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"fp_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("fragpos: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe fragpos";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "fragpos");
	const uint8_t *px = out.contents;
	int wrong = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const uint8_t *p = px + (y * 64 + x) * 4;   // B, G, R, A; position = pixel centre
			wrong += abs(p[2] - (int)lround((x + 0.5) / 64 * 255)) > 2 || abs(p[1] - (int)lround((y + 0.5) / 64 * 255)) > 2 || abs(p[0] - 128) > 2 || p[3] != 255;
		}
	const uint8_t *a = px + (40 * 64 + 20) * 4;
	OUT("fragpos: %d of 4096 pixels wrong; (20, 40) B %u G %u R %u A %u (0, 128 162 82 255 = executed)", wrong, a[0], a[1], a[2], a[3]);
}

static void glyphs(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct GV { float2 pos; float2 uv; }; struct GI { float2 offset; float4 colour; };\n"
	                                              "struct GO { float4 pos [[position]]; float2 uv; float4 colour; float4 mask; };\n"
	                                              "vertex GO gl_v(uint vid [[vertex_id]], uint iid [[instance_id]], constant GV *v [[buffer(0)]], constant GI *in [[buffer(1)]]) {"
	                                              " float2 p = v[vid].pos + in[iid].offset; GO o; o.pos = float4(p.x / 32 - 1, 1 - p.y / 32, 0, 1); o.uv = v[vid].uv;"
	                                              " o.colour = in[iid].colour; o.mask = float4(0, 1, 0, 0); return o; }\n"
	                                              "fragment float4 gl_f(GO in [[stage_in]]) { return float4(in.colour.rgb * dot(float4(in.uv.x, in.uv.y, 0.5, 0.25), in.mask), in.uv.x); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"gl_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"gl_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("glyphs: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	// The quad as iTerm2 draws a glyph: (0, 0) (8, 0) (0, 8), (8, 0) (0, 8) (8, 8), uv the same over 8.
	const float quad[6][4] = {{0, 0, 0, 0}, {8, 0, 1, 0}, {0, 8, 0, 1}, {8, 0, 1, 0}, {0, 8, 0, 1}, {8, 8, 1, 1}};
	float inst[64][8];
	for (int i = 0; i < 64; i++) {
		const float o[8] = {(float)(i % 8 * 8), (float)(i / 8 * 8), 0, 0, (i % 8 + 1) / 8.0f, (i / 8 + 1) / 8.0f, 1, 1};   // float2 + pad, float4
		memcpy(inst[i], o, sizeof(o));
	}
	id<MTLBuffer> vb = [d newBufferWithBytes:quad length:sizeof(quad) options:MTLResourceStorageModeShared];
	id<MTLBuffer> ib = [d newBufferWithBytes:inst length:sizeof(inst) options:MTLResourceStorageModeShared];
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe glyphs";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re setVertexBuffer:vb offset:0 atIndex:0];
	[re setVertexBuffer:ib offset:0 atIndex:1];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:64];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "glyphs");
	// Pixel (x, y): cell (x / 8, y / 8), uv = ((x % 8 + 0.5) / 8, (y % 8 + 0.5) / 8); colour = cell colour * uv.y, alpha uv.x.
	const uint8_t *px = out.contents;
	int wrong = 0, worst = 0, upperLeft = 0, lowerRight = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const double u = (x % 8 + 0.5) / 8, v = (y % 8 + 0.5) / 8, r = (x / 8 + 1) / 8.0, g = (y / 8 + 1) / 8.0;
			const int want[4] = {(int)lround(v * 255), (int)lround(g * v * 255), (int)lround(r * v * 255), (int)lround(u * 255)};   // B, G, R, A
			const uint8_t *p = px + (y * 64 + x) * 4;
			int off = 0;
			for (int c = 0; c < 4; c++)
				off = MAX(off, abs(p[c] - want[c]));
			worst = MAX(worst, off);
			if (off > 2) {
				wrong++;
				if (x % 8 + y % 8 < 7)   // the quad's first triangle, or its second
					upperLeft++;
				else
					lowerRight++;
			}
		}
	OUT("glyphs: %d of 4096 pixels off by more than 2 (%d in first triangles, %d in second), worst %d (0 = executed)", wrong, upperLeft, lowerRight, worst);
}

static void glyphs2(id<MTLDevice> d, id<MTLCommandQueue> q, const char *file) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct GV { float2 pos; float2 uv; }; struct PIU { float2 offset; float2 texOffset; float4 colour; int pad; float4 bg; };\n"
	                                              "struct GO { float4 pos [[position]]; float2 uv; float4 colour; float4 mask; };\n"
	                                              "vertex GO g2_v(uint vid [[vertex_id]], uint iid [[instance_id]], constant float2 &shift [[buffer(3)]], constant GV *v [[buffer(0)]],"
	                                              " constant uint2 &size [[buffer(1)]], constant PIU *piu [[buffer(2)]]) {"
	                                              " float4 c = piu[iid].colour; float4 m;"
	                                              " if (c.x + c.y > 1.2) m = float4(0, 0, 0, 1); else if (c.y > 0.6) m = float4(1, 0, 0, 0); else if (c.x > 0.6) m = float4(0, 1, 0, 0); else m = float4(0, 0, 1, 0);"
	                                              " GO o; o.pos = float4((v[vid].pos + piu[iid].offset + shift) / float2(size), 0, 1); o.uv = v[vid].uv + piu[iid].texOffset;"
	                                              " o.colour = c; o.mask = m; return o; }\n"
	                                              "fragment float4 g2_f(GO in [[stage_in]], texture2d<float> atlas [[texture(0)]]) {"
	                                              " constexpr sampler s(filter::linear, address::clamp_to_edge); float a = dot(atlas.sample(s, in.uv), in.mask);"
	                                              " return float4(in.colour.rgb * a, a); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"g2_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"g2_f"];
	pd.label = @"Pipeline for iTermMetalCellRenderer";
	MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
	ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
	ca.blendingEnabled = YES;   // premultiplied source over
	ca.sourceRGBBlendFactor = MTLBlendFactorOne;
	ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
	ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("glyphs2: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	// A 32x32 atlas: 4 "glyphs" of 8x8 per channel (a different pattern per channel: an L shape, a box, a diagonal, a dot grid).
	uint8_t atlas[32 * 32 * 4];
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++) {
			const int cx = x % 8, cy = y % 8;
			uint8_t *e = atlas + (y * 32 + x) * 4;   // B, G, R, A in memory (BGRA8)
			e[2] = (cx < 2 || cy > 5) ? 255 : 0;                 // R: L
			e[1] = (cx == 0 || cx == 7 || cy == 0 || cy == 7) ? 255 : 0;   // G: box
			e[0] = (cx == cy || cx == 7 - cy) ? 255 : 0;         // B: X
			e[3] = ((cx & 1) == 0 && (cy & 1) == 0) ? 255 : 64;  // A: dots
		}
	MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:32 height:32 mipmapped:NO];
	ad.storageMode = MTLStorageModeManaged;
	id<MTLTexture> at = [d newTextureWithDescriptor:ad];
	[at replaceRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0 withBytes:atlas bytesPerRow:32 * 4];
	// The quad as iTerm2's (positions in pixels from the window's centre, with the 2x viewport below), uv over one 8x8 atlas cell.
	const float quad[6][4] = {{8, 0, 8.0f / 32, 0}, {0, 0, 0, 0}, {0, 8, 0, 8.0f / 32}, {8, 0, 8.0f / 32, 0}, {0, 8, 0, 8.0f / 32}, {8, 8, 8.0f / 32, 8.0f / 32}};
	float piu[64][16];
	const float colours[4][4] = {{1, 1, 1, 1}, {0.2f, 0.9f, 0.3f, 1}, {0.9f, 0.2f, 0.3f, 1}, {0.3f, 0.3f, 0.9f, 1}};
	for (int i = 0; i < 64; i++) {
		float *e = piu[i];
		memset(e, 0, sizeof(piu[i]));
		e[0] = i % 8 * 8;
		e[1] = i / 8 * 8;
		e[2] = (i % 4) * 8.0f / 32;
		e[3] = (i / 4 % 4) * 8.0f / 32;
		memcpy(e + 4, colours[i % 4], 16);
	}
	const float shift[2] = {0, 0};
	const uint32_t size[2] = {64, 64};
	id<MTLBuffer> vb = [d newBufferWithBytes:quad length:sizeof(quad) options:MTLResourceStorageModeShared];
	id<MTLBuffer> pb = [d newBufferWithBytes:piu length:sizeof(piu) options:MTLResourceStorageModeShared];
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0.14, 0.14, 0.14, 1);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe glyphs2";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setViewport:(MTLViewport){-64, 0, 128, 128, -1, 1}];
	[re setRenderPipelineState:ps];
	[re setVertexBuffer:vb offset:0 atIndex:0];
	[re setVertexBytes:size length:8 atIndex:1];
	[re setVertexBuffer:pb offset:0 atIndex:2];
	[re setVertexBytes:shift length:8 atIndex:3];
	[re setFragmentTexture:at atIndex:0];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:64];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "glyphs2");
	FILE *f = file ? fopen(file, "wb") : NULL;
	if (f) {
		fwrite(out.contents, 1, 64 * 64 * 4, f);
		fclose(f);
	}
	const uint8_t *a = (const uint8_t *)out.contents + (4 * 64 + 4) * 4;
	OUT("glyphs2: written to %s; (4, 4) B %u G %u R %u A %u", f ? file : "(nowhere)", a[0], a[1], a[2], a[3]);
}

static void coverage2(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct C2 { float4 pos [[position]]; float v [[flat]]; };\n"
	                                              "vertex C2 c2_v(ushort vid [[vertex_id]], ushort iid [[instance_id]], texture2d<float> vt [[texture(9)]]) {"
	                                              " constexpr sampler s(filter::nearest); float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : -1.0;"
	                                              " float y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : -1.0; C2 o; o.pos = float4(x, y, float(iid), 1);"
	                                              " o.v = vt.sample(s, float2(0.5, 0.5)).r; return o; }\n"
	                                              "fragment half4 c2_f(C2 in [[stage_in]], texture2d<half> t [[texture(12)]]) { float4 p = in.pos;"
	                                              " constexpr sampler s(filter::nearest, address::clamp_to_edge);"
	                                              " half texel = t.sample(s, float2(0.375, 0.375), int2(1, 0)).r;"   // texel (1, 1) + (1, 0) = (2, 1)
	                                              " return half4(saturate(p.x / 32 - 0.5), sign(p.y - 32) * 0.5 + 0.5, dfdx(p.x) * 0.25 + texel * 0.25 + in.v * 0.25,"
	                                              " fwidth(p.y * 2) * 0.125); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"c2_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"c2_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("coverage2: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	// A 4x4 R channel: 1 (255) at texel (2, 1) only, so the offset sample reads 1 and an unoffset one 0.
	MTLTextureDescriptor *xd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
	xd.storageMode = MTLStorageModeManaged;
	id<MTLTexture> tex = [d newTextureWithDescriptor:xd];
	uint8_t texels[4 * 4 * 4] = {};
	texels[(1 * 4 + 2) * 4] = 255;
	[tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:16];
	MTLTextureDescriptor *wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
	wd.storageMode = MTLStorageModeManaged;
	id<MTLTexture> white = [d newTextureWithDescriptor:wd];   // the vertex stage's texture: one white texel
	const uint8_t one[4] = {255, 255, 255, 255};
	[white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:one bytesPerRow:4];
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe coverage2";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re setVertexTexture:white atIndex:9];
	[re setFragmentTexture:tex atIndex:12];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "coverage2");
	// R = saturate((x + 0.5) / 32 - 0.5), G = 0 above row 32 and 1 below, B = 0.25 * 3 (dfdx of x is 1, the offset texel 1, the vertex
	// stage's texel 1), A = 0.25 (fwidth of 2y is 2).
	const uint8_t *px = out.contents;
	int wrong = 0, worst = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const double r = fmin(fmax((x + 0.5) / 32 - 0.5, 0), 1);
			const int want[4] = {191, y < 32 ? 0 : 255, (int)lround(r * 255), 64};   // B, G, R, A
			const uint8_t *p = px + (y * 64 + x) * 4;
			int off = 0;
			for (int c = 0; c < 4; c++)
				off = MAX(off, abs(p[c] - want[c]));
			worst = MAX(worst, off);
			wrong += off > 2;
		}
	const uint8_t *a = px + (40 * 64 + 40) * 4;
	OUT("coverage2: %d of 4096 pixels off by more than 2, worst %d; (40, 40) B %u G %u R %u A %u (0, 191 255 195 64 = executed)", wrong, worst, a[0], a[1],
	    a[2], a[3]);
}

static void ctex(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "kernel void ct_k(texture2d<float, access::read> a [[texture(0)]], texture2d<float> b [[texture(3)]],"
	                                              " texture2d<float, access::write> c [[texture(5)]], sampler s [[sampler(2)]], device uint *g [[buffer(0)]],"
	                                              " uint2 gid [[thread_position_in_grid]], uint2 groups [[threadgroups_per_grid]]) {"
	                                              " float4 x = a.read(gid); float4 y = b.sample(s, (float2(gid) + 0.5) / 8.0, level(0));"
	                                              " c.write(float4(x.r, y.g, 0.5, 1), gid);"
	                                              " if (gid.x == 0 && gid.y == 0) { g[0] = groups.x; g[1] = groups.y; } }"
	                                     options:nil
	                                       error:&err];
	id<MTLFunction> fn = [lib newFunctionWithName:@"ct_k"];
	id<MTLComputePipelineState> ps = fn ? [d newComputePipelineStateWithFunction:fn error:&err] : nil;
	if (!ps) {
		OUT("ctex: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *xd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
	xd.storageMode = MTLStorageModeManaged;
	xd.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> a = [d newTextureWithDescriptor:xd], b = [d newTextureWithDescriptor:xd];
	xd.usage = MTLTextureUsageShaderWrite;
	id<MTLTexture> c = [d newTextureWithDescriptor:xd];
	uint8_t ta[8 * 8 * 4], tb[8 * 8 * 4], zero[8 * 8 * 4] = {};
	for (int i = 0; i < 64; i++) {
		const uint8_t va[4] = {(uint8_t)(i % 8 * 32), 0, 0, 255}, vb[4] = {0, (uint8_t)(i / 8 * 32), 0, 255};
		memcpy(ta + 4 * i, va, 4);
		memcpy(tb + 4 * i, vb, 4);
	}
	[a replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:ta bytesPerRow:32];
	[b replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:tb bytesPerRow:32];
	[c replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:zero bytesPerRow:32];
	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];   // nearest, clamp to edge
	id<MTLSamplerState> ss = [d newSamplerStateWithDescriptor:sd];
	id<MTLBuffer> g = [d newBufferWithLength:16 options:MTLResourceStorageModeShared];
	memset(g.contents, 0, 16);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe ctex";
	id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
	[ce setComputePipelineState:ps];
	[ce setTexture:a atIndex:0];
	[ce setTexture:b atIndex:3];
	[ce setTexture:c atIndex:5];
	[ce setSamplerState:ss atIndex:2];
	[ce setBuffer:g offset:0 atIndex:0];
	[ce dispatchThreadgroups:MTLSizeMake(2, 2, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
	[ce endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e synchronizeResource:c];
	[e endEncoding];
	run(cb, "ctex");
	uint8_t tc[8 * 8 * 4];
	[c getBytes:tc bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
	int wrong = 0;
	for (int i = 0; i < 64; i++) {
		const uint8_t want[4] = {(uint8_t)(i % 8 * 32), (uint8_t)(i / 8 * 32), 128, 255};   // R, G, B, A
		wrong += memcmp(tc + 4 * i, want, 4) != 0;
	}
	const uint32_t *gs = g.contents;
	OUT("ctex: %d of 64 texels wrong; texel (5, 6) R %u G %u B %u A %u; threadgroups_per_grid %u x %u (0, 160 192 128 255, 2 x 2 = executed)", wrong,
	    tc[4 * 53], tc[4 * 53 + 1], tc[4 * 53 + 2], tc[4 * 53 + 3], gs[0], gs[1]);
}

static void fbfetch(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "vertex float4 ff_v(uint vid [[vertex_id]]) { float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : -1.0;"
	                                              " float y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : -1.0; return float4(x, y, 0, 1); }\n"
	                                              "fragment float4 ff_f(float4 d [[color(0)]]) { return float4(1 - d.r, d.g * 0.5, d.b, 1); }\n"
	                                              "fragment void fw_f(float4 p [[position]], texture2d<float, access::write> t [[texture(1)]]) {"
	                                              " t.write(float4(p.x / 64, p.y / 64, 0.25, 1), uint2(p.xy)); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"ff_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"ff_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	pd.fragmentFunction = [lib newFunctionWithName:@"fw_f"];
	id<MTLRenderPipelineState> pw = ps ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps || !pw) {
		OUT("fbfetch: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	MTLTextureDescriptor *wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
	wd.usage = MTLTextureUsageShaderWrite;
	wd.storageMode = MTLStorageModeManaged;
	id<MTLTexture> w = [d newTextureWithDescriptor:wd];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 1);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe fbfetch";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	rp.colorAttachments[0].loadAction = MTLLoadActionLoad;   // the second pass keeps the first's result: it writes no colour
	re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:pw];
	[re setFragmentTexture:w atIndex:1];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e synchronizeResource:w];
	[e endEncoding];
	run(cb, "fbfetch");
	// The target: (1 - 0.2, 0.4 * 0.5, 0.6, 1) everywhere, the second pass leaving it. The texture: (x / 64, y / 64, 0.25, 1) at pixel centres.
	const uint8_t *px = out.contents;
	uint8_t tw[64 * 64 * 4];
	[w getBytes:tw bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
	int wrong = 0, wrongW = 0;
	for (int i = 0; i < 64 * 64; i++) {
		const uint8_t *p = px + 4 * i, *q2 = tw + 4 * i;
		const int want[4] = {153, 51, 204, 255};   // B, G, R, A
		const int wantW[4] = {(int)lround((i % 64 + 0.5) / 64 * 255), (int)lround((i / 64 + 0.5) / 64 * 255), 64, 255};   // R, G, B, A
		int off = 0, offW = 0;
		for (int c = 0; c < 4; c++) {
			off = MAX(off, abs(p[c] - want[c]));
			offW = MAX(offW, abs(q2[c] - wantW[c]));
		}
		wrong += off > 2;
		wrongW += offW > 2;
	}
	OUT("fbfetch: target %d of 4096 pixels off by more than 2 (B %u G %u R %u A %u, want 153 51 204 255); written texture %d of 4096 off (0, 0 = executed)",
	    wrong, px[0], px[1], px[2], px[3], wrongW);
}

static void varyings(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct VV { float4 pos [[position]]; float3 a; float b [[center_no_perspective]]; half2 h [[centroid_perspective]];"
	                                              " uint t [[flat]]; bool odd [[flat]]; };\n"
	                                              "vertex VV vy_v(uint vid [[vertex_id]]) { float x = (vid == 1 || vid == 3 || vid == 4) ? 1.0 : -1.0;"
	                                              " float y = (vid == 2 || vid == 4 || vid == 5) ? 1.0 : -1.0; VV v; v.pos = float4(x, y, 0, 1);"
	                                              " v.a = float3(x * 0.5 + 0.5, 0.25, 0.75); v.b = 0.5 - x * 0.5; v.h = half2(y * 0.5 + 0.5, 0.5);"
	                                              " v.t = vid; v.odd = (vid & 1) != 0; return v; }\n"
	                                              "fragment float4 vy_f(VV in [[stage_in]]) {"
	                                              " return float4(in.a.x, float(in.h.x), float(in.t) * 0.2 + (in.odd ? 0.05 : 0.0), in.b); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"vy_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"vy_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("varyings: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe varyings";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "varyings");
	// Pixel (px, py): x01 = (px + 0.5) / 64, y01 = 1 - (py + 0.5) / 64; the second triangle where x01 + y01 > 1.
	const uint8_t *px = out.contents;
	int wrong = 0, checked = 0, worst = 0;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			const double x01 = (x + 0.5) / 64, y01 = 1 - (y + 0.5) / 64;
			if (fabs(x01 + y01 - 1) < 0.05)
				continue;
			const uint8_t *p = px + (y * 64 + x) * 4;   // B, G, R, A
			const int want[4] = {x01 + y01 > 1 ? 166 : 0, (int)lround(y01 * 255), (int)lround(x01 * 255), (int)lround((1 - x01) * 255)};
			int off = 0;
			for (int c = 0; c < 4; c++)
				off = MAX(off, abs(p[c] - want[c]));
			worst = MAX(worst, off);
			wrong += off > 2;
			checked++;
		}
	const uint8_t *a = px + (40 * 64 + 20) * 4, *b = px + (20 * 64 + 44) * 4;
	OUT("varyings: %d of %d pixels off by more than 2, worst %d; (20, 40) B %u G %u R %u A %u, (44, 20) B %u G %u R %u A %u (0 = executed)", wrong,
	    checked, worst, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
}

// The cube's 24 corners (4 a face, faces +x -x +y -y +z -z) and the matrix of a cube turned (pitch, yaw) and set 3 units in front of a camera
// with a 60-degree field of view (Metal's clip space: depth 0..1 from near 1 to far 10).
static void cubeCorners(simd_float3 out[24], simd_float3 normals[6]) {
	static const int axes[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};
	static const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	for (int f = 0; f < 6; f++) {
		const float sgn = (f & 1) ? -1 : 1;
		simd_float3 n = 0;
		n[axes[f][0]] = sgn;
		normals[f] = n;
		for (int k = 0; k < 4; k++) {
			simd_float3 v = 0;
			v[axes[f][0]] = sgn * 0.6f;
			v[axes[f][1]] = corner[k][0] * 0.6f;
			v[axes[f][2]] = corner[k][1] * 0.6f * sgn;   // the winding the same seen from outside
			out[f * 4 + k] = v;
		}
	}
}

static simd_float4x4 cubeRotation(float pitch, float yaw) {
	const float cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
	const simd_float4x4 rx = {{{1, 0, 0, 0}, {0, cp, sp, 0}, {0, -sp, cp, 0}, {0, 0, 0, 1}}};
	const simd_float4x4 ry = {{{cy, 0, -sy, 0}, {0, 1, 0, 0}, {sy, 0, cy, 0}, {0, 0, 0, 1}}};
	return simd_mul(rx, ry);
}

static simd_float4x4 cubeMatrix(float pitch, float yaw, float aspect, simd_float3 at) {
	const float f = 1 / tanf(M_PI / 6), n = 1, z = 10;
	const simd_float4x4 tr = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {at.x, at.y, at.z, 1}}};
	const simd_float4x4 pr = {{{f / aspect, 0, 0, 0}, {0, f, 0, 0}, {0, 0, z / (z - n), 1}, {0, 0, -n * z / (z - n), 0}}};
	return simd_mul(pr, simd_mul(tr, cubeRotation(pitch, yaw)));
}

static void cube(id<MTLDevice> d, id<MTLCommandQueue> q) {
	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct CV { float4 pos [[position]]; uint face [[flat]]; };\n"
	                                              "vertex CV cube_v(uint vid [[vertex_id]], const device packed_float3 *p [[buffer(0)]],"
	                                              " constant float4x4 &m [[buffer(1)]]) { CV o; o.pos = m * float4(float3(p[vid]), 1); o.face = vid / 4; return o; }\n"
	                                              "fragment float4 cube_f(CV in [[stage_in]]) { const float4 c[6] = {float4(1, 0, 0, 1), float4(0, 1, 0, 1),"
	                                              " float4(0, 0, 1, 1), float4(1, 1, 0, 1), float4(1, 0, 1, 1), float4(0, 1, 1, 1)}; return c[in.face]; }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"cube_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"cube_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("cube: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
	dd.depthCompareFunction = MTLCompareFunctionLess;
	dd.depthWriteEnabled = YES;
	id<MTLDepthStencilState> ds = [d newDepthStencilStateWithDescriptor:dd];

	simd_float3 corners[24], normals[6];
	cubeCorners(corners, normals);
	float packed[72];
	for (int i = 0; i < 24; i++)
		for (int c = 0; c < 3; c++)
			packed[i * 3 + c] = corners[i][c];
	const simd_float4x4 m = cubeMatrix(0.45f, 0.6f, 1, simd_make_float3(0, 0, 3));

	// The faces turned away from the camera last: without the depth test they would be drawn over the others.
	simd_float4 clip[24];
	for (int i = 0; i < 24; i++)
		clip[i] = simd_mul(m, simd_make_float4(corners[i], 1));
	uint16_t idx[36];
	int ni = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int f = 0; f < 6; f++) {
			const simd_float4 a = clip[f * 4], b = clip[f * 4 + 1], c = clip[f * 4 + 2];
			const float area = (b.x / b.w - a.x / a.w) * (c.y / c.w - a.y / a.w) - (b.y / b.w - a.y / a.w) * (c.x / c.w - a.x / a.w);
			if ((area < 0) == (pass == 0)) {   // y up: a face seen from outside turns clockwise here
				const uint16_t q4[6] = {0, 1, 2, 0, 2, 3};
				for (int k = 0; k < 6; k++)
					idx[ni++] = (uint16_t)(f * 4 + q4[k]);
			}
		}

	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	td.storageMode = MTLStorageModePrivate;
	id<MTLTexture> t = [d newTextureWithDescriptor:td];
	MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:64 height:64 mipmapped:NO];
	zd.usage = MTLTextureUsageRenderTarget;
	zd.storageMode = MTLStorageModePrivate;
	id<MTLTexture> z = [d newTextureWithDescriptor:zd];
	id<MTLBuffer> vb = [d newBufferWithBytes:packed length:sizeof(packed) options:MTLResourceStorageModeShared];
	id<MTLBuffer> ib = [d newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared];
	id<MTLBuffer> out = [d newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = t;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	rp.depthAttachment.texture = z;
	rp.depthAttachment.loadAction = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth = 1;
	id<MTLCommandBuffer> cb = [q commandBuffer];
	cb.label = @"mtlprobe cube";
	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
	[re setRenderPipelineState:ps];
	[re setDepthStencilState:ds];
	[re setVertexBuffer:vb offset:0 atIndex:0];
	[re setVertexBytes:&m length:sizeof(m) atIndex:1];
	[re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:36 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0];
	[re endEncoding];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1) toBuffer:out
	      destinationOffset:0 destinationBytesPerRow:64 * 4 destinationBytesPerImage:64 * 64 * 4];
	[e endEncoding];
	run(cb, "cube");

	// The CPU's rasterisation: pixel centres, the depth (z / w, linear on the screen) tested less, the faces in the same order.
	int want[64 * 64];
	float depth[64 * 64];
	for (int i = 0; i < 64 * 64; i++) {
		want[i] = -1;
		depth[i] = 1;
	}
	for (int tri = 0; tri < 12; tri++) {
		float sx[3], sy[3], sz[3];
		for (int k = 0; k < 3; k++) {
			const simd_float4 c = clip[idx[tri * 3 + k]];
			sx[k] = (c.x / c.w * 0.5f + 0.5f) * 64;
			sy[k] = (0.5f - c.y / c.w * 0.5f) * 64;
			sz[k] = c.z / c.w;
		}
		const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
		if (fabsf(area) < 1e-6f)
			continue;
		for (int y = 0; y < 64; y++)
			for (int x = 0; x < 64; x++) {
				const float px = x + 0.5f, py = y + 0.5f;
				const float w0 = ((sx[1] - px) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px)) / area;
				const float w1 = ((sx[2] - px) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px)) / area;
				const float w2 = 1 - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0)
					continue;
				const float dz = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
				if (dz < depth[y * 64 + x]) {
					depth[y * 64 + x] = dz;
					want[y * 64 + x] = idx[tri * 3] / 4;
				}
			}
	}
	static const uint8_t bgr[6][3] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {0, 255, 255}, {255, 0, 255}, {255, 255, 0}};
	const uint8_t *px = out.contents;
	int wrong = 0, covered = 0, hiddenSeen = 0, perFace[7] = {0};
	bool hidden[6] = {false};
	for (int i = 18; i < 36; i += 6)
		hidden[idx[i] / 4] = true;
	for (int i = 0; i < 64 * 64; i++) {
		const uint8_t *p = px + i * 4;
		int got = -2;
		if (!p[0] && !p[1] && !p[2])
			got = -1;
		for (int f = 0; f < 6 && got == -2; f++)
			if (p[0] == bgr[f][0] && p[1] == bgr[f][1] && p[2] == bgr[f][2])
				got = f;
		covered += want[i] >= 0;
		wrong += got != want[i];
		hiddenSeen += got >= 0 && hidden[got];
		perFace[got < 0 ? 6 : got]++;
	}
	OUT("cube: %d of 4096 pixels not the CPU's face (%d covered; edges may differ by a pixel), %d of a hidden face; pixels a face +x %d -x %d +y %d "
	    "-y %d +z %d -z %d, none or other %d: %s", wrong, covered, hiddenSeen, perFace[0], perFace[1], perFace[2], perFace[3], perFace[4], perFace[5],
	    perFace[6], !hiddenSeen && covered && wrong * 50 <= covered ? "PASS" : "FAIL");
}

// Nine lit, textured cubes spinning in a window; the frames a second in the title.
static void spin(id<MTLDevice> d, id<MTLCommandQueue> q, double seconds) {
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 960, 600)
	                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
	                                            backing:NSBackingStoreBuffered
	                                              defer:NO];
	w.title = @"mtlprobe spin";
	CAMetalLayer *layer = [CAMetalLayer layer];
	layer.device = d;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	w.contentView.wantsLayer = YES;
	w.contentView.layer = layer;
	[w makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];

	NSError *err = nil;
	id<MTLLibrary> lib = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
	                                              "struct SV { float4 pos [[position]]; float3 n; float2 uv; float3 tint [[flat]]; };\n"
	                                              "struct SU { float4x4 m; float4x4 rot; float4 tint; };\n"
	                                              "vertex SV spin_v(uint vid [[vertex_id]], uint iid [[instance_id]], const device packed_float3 *p [[buffer(0)]],"
	                                              " const device packed_float3 *n [[buffer(1)]], constant SU *u [[buffer(2)]]) {"
	                                              " SV o; o.pos = u[iid].m * float4(float3(p[vid]), 1); o.n = (u[iid].rot * float4(float3(n[vid / 4]), 0)).xyz;"
	                                              " uint k = vid & 3; o.uv = float2(k == 1 || k == 2, k >= 2); o.tint = u[iid].tint.rgb; return o; }\n"
	                                              "fragment float4 spin_f(SV in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {"
	                                              " float l = 0.25 + 0.75 * max(dot(normalize(in.n), normalize(float3(-0.4, 0.6, -0.7))), 0.0);"
	                                              " return float4(t.sample(s, in.uv).rgb * in.tint * l, 1); }"
	                                     options:nil
	                                       error:&err];
	MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
	pd.vertexFunction = [lib newFunctionWithName:@"spin_v"];
	pd.fragmentFunction = [lib newFunctionWithName:@"spin_f"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	id<MTLRenderPipelineState> ps = lib ? [d newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
	if (!ps) {
		OUT("spin: pipeline NOT created: %s", err.localizedDescription.UTF8String);
		return;
	}
	MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
	dd.depthCompareFunction = MTLCompareFunctionLess;
	dd.depthWriteEnabled = YES;
	id<MTLDepthStencilState> ds = [d newDepthStencilStateWithDescriptor:dd];
	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
	id<MTLSamplerState> ss = [d newSamplerStateWithDescriptor:sd];

	// A 64x64 checkerboard with a border: which way each face's texture runs shows.
	uint32_t tex[64 * 64];
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			tex[y * 64 + x] = x < 3 || y < 3 ? 0xff202020u : ((x / 8 + y / 8) & 1) ? 0xffffffffu : 0xffb0b0b0u;
	MTLTextureDescriptor *xd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
	id<MTLTexture> checker = [d newTextureWithDescriptor:xd];
	[checker replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:tex bytesPerRow:64 * 4];

	simd_float3 corners[24], normals[6];
	cubeCorners(corners, normals);
	float packed[72], packedN[18];
	for (int i = 0; i < 24; i++)
		for (int c = 0; c < 3; c++)
			packed[i * 3 + c] = corners[i][c];
	for (int i = 0; i < 6; i++)
		for (int c = 0; c < 3; c++)
			packedN[i * 3 + c] = normals[i][c];
	uint16_t idx[36];
	for (int f = 0; f < 6; f++) {
		const uint16_t q4[6] = {0, 1, 2, 0, 2, 3};
		for (int k = 0; k < 6; k++)
			idx[f * 6 + k] = (uint16_t)(f * 4 + q4[k]);
	}
	id<MTLBuffer> vb = [d newBufferWithBytes:packed length:sizeof(packed) options:MTLResourceStorageModeShared];
	id<MTLBuffer> nb = [d newBufferWithBytes:packedN length:sizeof(packedN) options:MTLResourceStorageModeShared];
	id<MTLBuffer> ib = [d newBufferWithBytes:idx length:sizeof(idx) options:MTLResourceStorageModeShared];
	struct SU { simd_float4x4 m, rot; simd_float4 tint; };

	id<MTLTexture> zt = nil;
	NSDate *start = [NSDate date], *second = start;
	int frames = 0, total = 0;
	while (-[start timeIntervalSinceNow] < seconds && w.isVisible) {
		@autoreleasepool {
			for (NSEvent *ev; (ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES]);)
				[NSApp sendEvent:ev];
			const CGSize size = CGSizeMake(w.contentView.bounds.size.width * w.backingScaleFactor, w.contentView.bounds.size.height * w.backingScaleFactor);
			if (size.width < 1 || size.height < 1)
				continue;
			layer.drawableSize = size;
			if (!zt || zt.width != (NSUInteger)size.width || zt.height != (NSUInteger)size.height) {
				MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:(NSUInteger)size.width
				                                                                            height:(NSUInteger)size.height mipmapped:NO];
				zd.usage = MTLTextureUsageRenderTarget;
				zd.storageMode = MTLStorageModePrivate;
				zt = [d newTextureWithDescriptor:zd];
			}
			id<CAMetalDrawable> dr = [layer nextDrawable];
			if (!dr)
				continue;
			const float time = (float)-[start timeIntervalSinceNow], aspect = (float)(size.width / size.height);
			struct SU u[9];
			for (int i = 0; i < 9; i++) {
				const float pitch = time * (0.7f + 0.1f * i), yaw = time * (1.1f - 0.07f * i);
				const simd_float3 at = simd_make_float3((i % 3 - 1) * 1.6f, (1 - i / 3) * 1.6f, 5 + (i % 2) * 0.8f);
				u[i].m = cubeMatrix(pitch, yaw, aspect, at);
				u[i].rot = cubeRotation(pitch, yaw);
				u[i].tint = simd_make_float4(0.5f + 0.5f * (i % 3 == 0), 0.5f + 0.5f * (i % 3 == 1), 0.5f + 0.5f * (i % 3 == 2), 1);
			}
			MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
			rp.colorAttachments[0].texture = dr.texture;
			rp.colorAttachments[0].loadAction = MTLLoadActionClear;
			rp.colorAttachments[0].storeAction = MTLStoreActionStore;
			rp.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.1, 0.14, 1);
			rp.depthAttachment.texture = zt;
			rp.depthAttachment.loadAction = MTLLoadActionClear;
			rp.depthAttachment.storeAction = MTLStoreActionDontCare;
			rp.depthAttachment.clearDepth = 1;
			id<MTLCommandBuffer> cb = [q commandBuffer];
			cb.label = @"mtlprobe spin";
			id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
			[re setRenderPipelineState:ps];
			[re setDepthStencilState:ds];
			[re setVertexBuffer:vb offset:0 atIndex:0];
			[re setVertexBuffer:nb offset:0 atIndex:1];
			[re setVertexBytes:u length:sizeof(u) atIndex:2];
			[re setFragmentTexture:checker atIndex:0];
			[re setFragmentSamplerState:ss atIndex:0];
			[re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:36 indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:0
			            instanceCount:9];
			[re endEncoding];
			[cb presentDrawable:dr];
			[cb commit];
			frames++;
			total++;
			if (-[second timeIntervalSinceNow] >= 1) {
				const double fps = frames / -[second timeIntervalSinceNow];
				w.title = [NSString stringWithFormat:@"mtlprobe spin: %.0f frames a second, %.0fx%.0f", fps, size.width, size.height];
				OUT("spin: %.1f frames a second at %.0fx%.0f", fps, size.width, size.height);
				frames = 0;
				second = [NSDate date];
			}
		}
	}
	OUT("spin: %d frames in %.1f s", total, -[start timeIntervalSinceNow]);
}

static float nativeRandom(uint32_t *seed)   // a fixed sequence (LCG), the same on every run
{
	*seed = *seed * 1664525u + 1013904223u;
	return (float)(*seed >> 8) / 16777216.0f;
}

// The native-Metal spike's gate 5: `kRays` rays from a grid at z = 0, tilted, against `kTris` random triangles at z 2-6 (a fixed seed), in
// an acceleration structure; each ray's hit (distance, barycentrics, primitive id) against the nearest one by the CPU (Moller-Trumbore,
// no structure). A distance within 1e-4 counts as the same hit (the primitive may then differ: two triangles at that distance).
static BOOL nativeRays(id<MTLDevice> d, id<MTLLibrary> lib)
{
	enum { kTris = 300, kRays = 1024 };
	static float verts[kTris * 9], org[kRays * 4], dir[kRays * 4], cpu[kRays * 4];
	uint32_t seed = 12345;
#define rnd() nativeRandom(&seed)
	for (int t = 0; t < kTris; t++) {
		const float cx = 2.4f * rnd() - 1.2f, cy = 2.4f * rnd() - 1.2f, cz = 2 + 4 * rnd();
		for (int k = 0; k < 3; k++) {
			verts[9 * t + 3 * k] = cx + 0.5f * rnd() - 0.25f;
			verts[9 * t + 3 * k + 1] = cy + 0.5f * rnd() - 0.25f;
			verts[9 * t + 3 * k + 2] = cz + 0.5f * rnd() - 0.25f;
		}
	}
	for (int i = 0; i < kRays; i++) {
		org[4 * i] = 2.4f * (i % 32) / 31 - 1.2f;
		org[4 * i + 1] = 2.4f * (i / 32) / 31 - 1.2f;
		org[4 * i + 2] = org[4 * i + 3] = 0;
		dir[4 * i] = 0.1f * rnd() - 0.05f;
		dir[4 * i + 1] = 0.1f * rnd() - 0.05f;
		dir[4 * i + 2] = 1;
		dir[4 * i + 3] = 0;
		float best = 100;
		cpu[4 * i] = cpu[4 * i + 1] = cpu[4 * i + 2] = cpu[4 * i + 3] = -1;
		const float *o = &org[4 * i], *dd = &dir[4 * i];
		for (int t = 0; t < kTris; t++) {
			const float *v = &verts[9 * t];
			const float e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] }, e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
			const float p[3] = { dd[1] * e2[2] - dd[2] * e2[1], dd[2] * e2[0] - dd[0] * e2[2], dd[0] * e2[1] - dd[1] * e2[0] };
			const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
			if (fabsf(det) < 1e-12f)
				continue;
			const float inv = 1 / det, s[3] = { o[0] - v[0], o[1] - v[1], o[2] - v[2] };
			const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
			const float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
			const float w = (dd[0] * q[0] + dd[1] * q[1] + dd[2] * q[2]) * inv, dist = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
			if (u < 0 || u > 1 || w < 0 || u + w > 1 || dist < 0 || dist > best)
				continue;
			best = dist;
			cpu[4 * i] = dist;
			cpu[4 * i + 1] = u;
			cpu[4 * i + 2] = w;
			cpu[4 * i + 3] = t;
		}
	}
	NSError *err = nil;
	id<MTLComputePipelineState> rays = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rays"] error:&err];
	id<MTLBuffer> vb = [d newBufferWithBytes:verts length:sizeof verts options:MTLResourceStorageModeShared];
	id<MTLBuffer> ob = [d newBufferWithBytes:org length:sizeof org options:MTLResourceStorageModeShared];
	id<MTLBuffer> db = [d newBufferWithBytes:dir length:sizeof dir options:MTLResourceStorageModeShared];
	id<MTLBuffer> out = [d newBufferWithLength:sizeof cpu options:MTLResourceStorageModeShared];
	if (!rays || !vb || !ob || !db || !out) {
		OUT("native: rays FAIL: %s", rays ? "no buffers" : err.localizedDescription.UTF8String);
		return NO;
	}
	MTLAccelerationStructureTriangleGeometryDescriptor *geo = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
	geo.vertexBuffer = vb;
	geo.vertexStride = 12;
	geo.triangleCount = kTris;
	MTLPrimitiveAccelerationStructureDescriptor *desc = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	desc.geometryDescriptors = @[ geo ];
	const MTLAccelerationStructureSizes sizes = [d accelerationStructureSizesWithDescriptor:desc];
	id<MTLAccelerationStructure> as = [d newAccelerationStructureWithSize:sizes.accelerationStructureSize];
	id<MTLBuffer> scratch = [d newBufferWithLength:sizes.buildScratchBufferSize ?: 16 options:MTLResourceStorageModePrivate];
	id<MTLCommandBuffer> cb = [[d newCommandQueue] commandBuffer];
	id<MTLAccelerationStructureCommandEncoder> ae = [cb accelerationStructureCommandEncoder];
	[ae buildAccelerationStructure:as descriptor:desc scratchBuffer:scratch scratchBufferOffset:0];
	[ae endEncoding];
	id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
	[ce setComputePipelineState:rays];
	[ce setAccelerationStructure:as atBufferIndex:0];
	[ce setBuffer:ob offset:0 atIndex:1];
	[ce setBuffer:db offset:0 atIndex:2];
	[ce setBuffer:out offset:0 atIndex:3];
	[ce dispatchThreads:MTLSizeMake(kRays, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
	[ce endEncoding];
	const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	[cb commit];
	[cb waitUntilCompleted];
	const double ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6;
	const float *gpu = out.contents;
	int hits = 0, bad = 0, firstBad = -1;
	for (int i = 0; i < kRays; i++) {
		const BOOL hitCpu = cpu[4 * i] >= 0, hitGpu = gpu[4 * i] >= 0;
		hits += hitCpu;
		const BOOL same = hitCpu == hitGpu && (!hitCpu || (fabsf(cpu[4 * i] - gpu[4 * i]) < 1e-4f &&
		                                                   (cpu[4 * i + 3] != gpu[4 * i + 3] ||
		                                                    (fabsf(cpu[4 * i + 1] - gpu[4 * i + 1]) < 1e-4f && fabsf(cpu[4 * i + 2] - gpu[4 * i + 2]) < 1e-4f))));
		if (!same && !bad++)
			firstBad = i;
	}
	const uint32_t *head = ((id<MTLBuffer>)[(id)as valueForKey:@"memory"]).contents;
	OUT("native: rays %s: %d of %d rays wrong, %d hit (CPU); structure %u nodes, %u triangles, %lu bytes; commit %.1f ms (status %ld%s%s)%s",
	    !bad && cb.status == MTLCommandBufferStatusCompleted && hits > 0 && hits < kRays ? "PASS" : "FAIL", bad, kRays, hits, head ? head[1] : 0,
	    head ? head[2] : 0, (unsigned long)sizes.accelerationStructureSize, ms, (long)cb.status, cb.error ? ", " : "",
	    cb.error ? cb.error.localizedDescription.UTF8String : "",
	    firstBad >= 0 ? [NSString stringWithFormat:@"; first wrong ray %d: CPU %g %g %g %g, GPU %g %g %g %g", firstBad, cpu[4 * firstBad],
	                     cpu[4 * firstBad + 1], cpu[4 * firstBad + 2], cpu[4 * firstBad + 3], gpu[4 * firstBad], gpu[4 * firstBad + 1],
	                     gpu[4 * firstBad + 2], gpu[4 * firstBad + 3]].UTF8String : "");
	return !bad && cb.status == MTLCommandBufferStatusCompleted && hits > 0 && hits < kRays;
#undef rnd
}

int main(int argc, char **argv) {
	@autoreleasepool {
		const char *mode = argc > 1 ? argv[1] : "info";
		if (!strcmp(mode, "all")) {
			const char *modes[] = {"info", "blit", "compute", "grid", "mix", "two", "twolib", "feat", "bytes", "simd", "render", "tri", "tricol", "vbuf", "drawmodes", "texdraw", "blend", "blendx", "varyings", "texsize", "surfdraw", "manyinst", "viewport", "texlinear", "fragpos", "glyphs", "coverage2", "ctex", "fbfetch", "cube", "dualsrc"};
			for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
				pid_t pid = fork();
				if (pid == 0) {
					execl(argv[0], argv[0], modes[i], (char *)NULL);
					_exit(127);
				}
				int st = 0;
				waitpid(pid, &st, 0);
				OUT("%s: child ended, %s %d", modes[i], WIFSIGNALED(st) ? "signal" : "exit", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
			}
			return 0;
		}
		if (!strcmp(mode, "native")) {   // the native-Metal spike's gate 2: buffers and two compute dispatches on RDNA4MTLDevice
			if (argc < 3) {
				OUT("native: usage: mtlprobe native <probe.metallib>");
				return 2;
			}
			id<MTLDevice> d = nil;
			for (id<MTLDevice> dev in MTLCopyAllDevices())
				if ([NSStringFromClass([(id)dev class]) isEqualToString:@"RDNA4MTLDevice"])
					d = dev;
			if (!d) {
				OUT("native: FAIL: no RDNA4MTLDevice (RDNA4_NATIVE_MTL=1? sysctl debug.rdna4_nativemtl=1? the bundle installed?)");
				return 1;
			}
			NSError *err = nil;
			id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&err];
			id<MTLComputePipelineState> vadd = lib ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"vadd"] error:&err] : nil;
			id<MTLComputePipelineState> scale = vadd ? [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"scale"] error:&err] : nil;
			if (!scale) {
				OUT("native: FAIL: %s", err.localizedDescription.UTF8String ?: "no library or pipeline");
				return 1;
			}
			enum { n = 1000 };
			id<MTLBuffer> a = [d newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
			id<MTLBuffer> b = [d newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
			id<MTLBuffer> c = [d newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
			if (!a || !b || !c) {
				OUT("native: FAIL: no buffers");
				return 1;
			}
			float *pa = a.contents, *pb = b.contents, *pc = c.contents;
			for (int i = 0; i < n; i++) {
				pa[i] = i;
				pb[i] = 0.5f * i + 1;
				pc[i] = -1;
			}
			const float k = 3;
			id<MTLCommandBuffer> cb = [[d newCommandQueue] commandBuffer];
			id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
			[enc setComputePipelineState:vadd];
			[enc setBuffer:a offset:0 atIndex:0];
			[enc setBuffer:b offset:0 atIndex:1];
			[enc setBuffer:c offset:0 atIndex:2];
			[enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
			[enc setComputePipelineState:scale];
			[enc setBuffer:c offset:0 atIndex:0];
			[enc setBytes:&k length:sizeof k atIndex:1];
			[enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
			[enc endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			int bad = 0, firstBad = -1;
			for (int i = 0; i < n; i++)
				if (pc[i] != 3 * (pa[i] + pb[i]) && !bad++)
					firstBad = i;
			const BOOL computeOk = !bad && cb.status == MTLCommandBufferStatusCompleted;
			OUT("native: compute %s: %d of %d wrong (status %ld%s%s); c[0] %g, c[999] %g%s", computeOk ? "PASS" : "FAIL",
			    bad, n, (long)cb.status, cb.error ? ", " : "", cb.error ? cb.error.localizedDescription.UTF8String : "", pc[0], pc[n - 1],
			    firstBad >= 0 ? [NSString stringWithFormat:@"; first wrong c[%d] = %g", firstBad, pc[firstBad]].UTF8String : "");

			// Gate 3: the colour triangle in a render pass whose clear replaces the 0x55 the texture was filled with.
			MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:256 mipmapped:NO];
			td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			td.storageMode = MTLStorageModeShared;
			id<MTLTexture> tex = [d newTextureWithDescriptor:td];
			static uint32_t img[256 * 256];
			memset(img, 0x55, sizeof img);
			[tex replaceRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0 withBytes:img bytesPerRow:1024];
			MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
			rd.vertexFunction = [lib newFunctionWithName:@"tri_v"];
			rd.fragmentFunction = [lib newFunctionWithName:@"tri_f"];
			rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			id<MTLRenderPipelineState> rps = tex ? [d newRenderPipelineStateWithDescriptor:rd error:&err] : nil;
			if (!rps) {
				OUT("native: render FAIL: %s", tex ? err.localizedDescription.UTF8String : "no texture");
				return 1;
			}
			const float pos[6] = { -0.5f, 0.5f, 0.5f, 0.5f, 0, -0.5f };   // pixels (64,64), (192,64), (128,192)
			const float col[12] = { 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1 };
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = tex;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLCommandBuffer> rb = [[d newCommandQueue] commandBuffer];
			id<MTLRenderCommandEncoder> re = [rb renderCommandEncoderWithDescriptor:pass];
			[re setRenderPipelineState:rps];
			[re setVertexBytes:pos length:sizeof pos atIndex:0];
			[re setVertexBytes:col length:sizeof col atIndex:1];
			[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[re endEncoding];
			[rb commit];
			[rb waitUntilCompleted];
			[tex getBytes:img bytesPerRow:1024 fromRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0];
			rdna4_tricol_result tr;
			const BOOL renderOk = rdna4_tricol_check(img, &tr) && rb.status == MTLCommandBufferStatusCompleted;
			OUT("native: render %s: %u px (want 8192), bounds x %u..%u y %u..%u, max channel error %.2f, max |R+G+B-255| %u, centroid 0x%08x, "
			    "near the vertices 0x%08x 0x%08x 0x%08x, pixel (0,0) 0x%08x (status %ld%s%s)", renderOk ? "PASS" : "FAIL", tr.covered, tr.minX, tr.maxX,
			    tr.minY, tr.maxY, tr.maxChannelErr256 / 256.0, tr.maxSumErr, tr.centroid, tr.near0, tr.near1, tr.near2, img[0], (long)rb.status,
			    rb.error ? ", " : "", rb.error ? rb.error.localizedDescription.UTF8String : "");
			const BOOL raysOk = nativeRays(d, lib);
			return computeOk && renderOk && raysOk ? 0 : 1;
		}
		if (!strcmp(mode, "devices")) {   // every Metal device, its class and registry ID (the native-Metal spike's gate 1)
			NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
			OUT("devices: %lu", (unsigned long)all.count);
			for (id<MTLDevice> dev in all)
				OUT("devices: \"%s\" (class %s), registry ID 0x%llx", dev.name.UTF8String, NSStringFromClass([(id)dev class]).UTF8String,
				    (unsigned long long)dev.registryID);
			return 0;
		}
		id<MTLDevice> d = MTLCreateSystemDefaultDevice();
		OUT("device: %s", d ? d.name.UTF8String : "none (MTLCreateSystemDefaultDevice returned nil)");
		if (!d)
			return 1;
		id<MTLCommandQueue> q = [d newCommandQueue];
		if (!strcmp(mode, "info"))
			info(d);
		else if (!strcmp(mode, "blit"))
			blit(d, q);
		else if (!strcmp(mode, "compute"))
			compute(d, q);
		else if (!strcmp(mode, "grid"))
			grid(d, q);
		else if (!strcmp(mode, "mix"))
			mix(d, q);
		else if (!strcmp(mode, "two"))
			pair(d, q, "two",
			     @[ (@"kernel void add1(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]], uint i [[thread_position_in_grid]]) { out[i] = in[i] + 1; }\n"
			         "kernel void mul3(device uint *io [[buffer(0)]], uint i [[thread_position_in_grid]]) { io[i] *= 3; }") ],
			     @"add1", @"mul3", ^uint32_t(uint32_t i) { return (i + 1) * 3; });
		else if (!strcmp(mode, "twolib"))
			pair(d, q, "twolib",
			     @[ @"kernel void inc7(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]], uint i [[thread_position_in_grid]]) { out[i] = in[i] + 7; }",
			        @"kernel void square(device uint *io [[buffer(0)]], uint i [[thread_position_in_grid]]) { io[i] *= io[i]; }" ],
			     @"inc7", @"square", ^uint32_t(uint32_t i) { return (i + 7) * (i + 7); });
		else if (!strcmp(mode, "feat"))
			feat(d, q);
		else if (!strcmp(mode, "bytes"))
			bytes(d, q);
		else if (!strcmp(mode, "simd"))
			simd(d, q);
		else if (!strcmp(mode, "render"))
			render(d, q);
		else if (!strcmp(mode, "tri"))
			tri(d, q);
		else if (!strcmp(mode, "tricol"))
			tricol(d, q);
		else if (!strcmp(mode, "vbuf"))
			vbuf(d, q);
		else if (!strcmp(mode, "drawmodes"))
			drawmodes(d, q);
		else if (!strcmp(mode, "texinfo"))
			texinfo(d, q);
		else if (!strcmp(mode, "texdraw"))
			texdraw(d, q);
		else if (!strcmp(mode, "varyings"))
			varyings(d, q);
		else if (!strcmp(mode, "texsize"))
			texsize(d, q);
		else if (!strcmp(mode, "surfdraw"))
			surfdraw(d, q);
		else if (!strcmp(mode, "manyinst"))
			manyinst(d, q);
		else if (!strcmp(mode, "viewport"))
			viewport(d, q);
		else if (!strcmp(mode, "texlinear"))
			texlinear(d, q);
		else if (!strcmp(mode, "fragpos"))
			fragpos(d, q);
		else if (!strcmp(mode, "glyphs"))
			glyphs(d, q);
		else if (!strcmp(mode, "coverage2"))
			coverage2(d, q);
		else if (!strcmp(mode, "ctex"))
			ctex(d, q);
		else if (!strcmp(mode, "fbfetch"))
			fbfetch(d, q);
		else if (!strcmp(mode, "glyphs2"))
			glyphs2(d, q, argc > 2 ? argv[2] : NULL);
		else if (!strcmp(mode, "blend") || !strcmp(mode, "blendx"))
			blend(d, q, mode[5] == 'x');
		else if (!strcmp(mode, "blendinfo"))
			blendinfo(d, q);
		else if (!strcmp(mode, "dualsrc"))
			dualsrc(d, q);
		else if (!strcmp(mode, "drawargs"))
			drawargs(d, q);
		else if (!strcmp(mode, "cube"))
			cube(d, q);
		else if (!strcmp(mode, "spin"))
			spin(d, q, argc > 2 ? atof(argv[2]) : 10);
		else {
			OUT("unknown mode %s (info, blit, compute, grid, mix, two, twolib, feat, bytes, simd, render, tri, tricol, vbuf, drawmodes, texdraw, blend, blendx, varyings, texsize, surfdraw, manyinst, viewport, texlinear, fragpos, glyphs, glyphs2 F, coverage2, ctex, fbfetch, cube, spin [S], texinfo, blendinfo, drawargs, all)", mode);
			return 2;
		}
	}
	return 0;
}
