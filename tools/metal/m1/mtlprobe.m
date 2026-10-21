// mtlprobe: known Metal work for M1 (docs/m1-stream.md), so that what the fake paravirtual GPU's host decodes can be matched to what was asked.
// Each mode is one small, separately committed piece of work on the system default device; it waits at most 5 s for completion, since the host
// does not execute anything yet (it acknowledges every command), and prints what came back.
//
//   mtlprobe info      the device and what it reports
//   mtlprobe blit      fill a 4 KiB buffer with 0x5a, copy it into a second buffer
//   mtlprobe compute   c[i] = a[i] + b[i] over 64 floats (the kernel compiled from source here)
//   mtlprobe grid      out[y * 50 + x] = x + 1000 * y over 50 x 20 threads in groups of 16 x 4 (2D, partial groups at the edges)
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
			const char *modes[] = {"info", "blit", "compute", "grid", "render"};
			for (int i = 0; i < 5; i++) {
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
		else if (!strcmp(mode, "render"))
			render(d, q);
		else {
			OUT("unknown mode %s (info, blit, compute, grid, render, all)", mode);
			return 2;
		}
	}
	return 0;
}
