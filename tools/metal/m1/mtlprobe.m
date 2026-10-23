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
//   mtlprobe varyings  five values from the vertex to the fragment function over a 64x64 quad: float3 perspective (red), float without
//                      perspective (alpha), half2 centroid (green), uint and bool flat (blue: 0 in the first triangle, 0.65 in the second,
//                      from its first vertex); checked away from the diagonal
//   mtlprobe all       all of them, each in its own child process (a crash or hang in one does not hide the others)
//
// Build: clang -arch x86_64 -mmacosx-version-min=11.0 -fobjc-arc tools/metal/m1/mtlprobe.m -framework Foundation -framework Metal -framework IOSurface -o mtlprobe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <sys/wait.h>
#include <unistd.h>

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
	for (MTLGPUFamily f = MTLGPUFamilyMac2; f <= MTLGPUFamilyMac2; f++)
		OUT("info: supports MTLGPUFamilyMac2: %d", [d supportsFamily:f]);
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

int main(int argc, char **argv) {
	@autoreleasepool {
		const char *mode = argc > 1 ? argv[1] : "info";
		if (!strcmp(mode, "all")) {
			const char *modes[] = {"info", "blit", "compute", "grid", "mix", "two", "twolib", "feat", "bytes", "simd", "render", "tri", "tricol", "vbuf", "drawmodes", "texdraw", "blend", "blendx", "varyings", "texsize", "surfdraw", "manyinst", "viewport", "texlinear", "fragpos", "glyphs"};
			for (int i = 0; i < 26; i++) {
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
		else if (!strcmp(mode, "glyphs2"))
			glyphs2(d, q, argc > 2 ? argv[2] : NULL);
		else if (!strcmp(mode, "blend") || !strcmp(mode, "blendx"))
			blend(d, q, mode[5] == 'x');
		else if (!strcmp(mode, "blendinfo"))
			blendinfo(d, q);
		else if (!strcmp(mode, "drawargs"))
			drawargs(d, q);
		else {
			OUT("unknown mode %s (info, blit, compute, grid, mix, two, twolib, feat, bytes, simd, render, tri, tricol, vbuf, drawmodes, texdraw, blend, blendx, varyings, texsize, surfdraw, manyinst, viewport, texlinear, fragpos, glyphs, glyphs2 F, texinfo, blendinfo, drawargs, all)", mode);
			return 2;
		}
	}
	return 0;
}
