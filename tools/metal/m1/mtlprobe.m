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
//   mtlprobe drawargs  draws with distinct arguments, nothing checked (for decoding the draw commands)
//   mtlprobe render    one render pass that clears a 64x64 BGRA texture to (0.25, 0.5, 0.75, 1)
//   mtlprobe all       all of them, each in its own child process (a crash or hang in one does not hide the others)
//
// Build: clang -arch x86_64 -mmacosx-version-min=11.0 -fobjc-arc tools/metal/m1/mtlprobe.m -framework Foundation -framework Metal -o mtlprobe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
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
	OUT("feat: %d of 9 wrong or not run (0 = executed)", failed);
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

int main(int argc, char **argv) {
	@autoreleasepool {
		const char *mode = argc > 1 ? argv[1] : "info";
		if (!strcmp(mode, "all")) {
			const char *modes[] = {"info", "blit", "compute", "grid", "mix", "two", "twolib", "feat", "bytes", "simd", "render", "tri", "tricol", "vbuf", "drawmodes"};
			for (int i = 0; i < 15; i++) {
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
		else if (!strcmp(mode, "drawargs"))
			drawargs(d, q);
		else {
			OUT("unknown mode %s (info, blit, compute, grid, mix, two, twolib, feat, bytes, simd, render, tri, tricol, vbuf, drawmodes, drawargs, all)", mode);
			return 2;
		}
	}
	return 0;
}
