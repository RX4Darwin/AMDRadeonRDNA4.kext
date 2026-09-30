// rdna4-census: the Recovery-side trigger of E1 (docs/metal-spike.md, hub-task-354). Read-only apart from opening connections to the census
// service; it does not call anything that could touch the GPU.
//
//   rdna4-census registry   list every IOAccelerator service, its class and its registry properties
//   rdna4-census metal      MTLCopyAllDevices() / MTLCreateSystemDefaultDevice(): what Metal's loader does with the census service
//   rdna4-census open       IOServiceOpen on the IOAccelerator service with the user-client types Apple's stack uses, then selector 0
//   rdna4-census all        the three, each in its own child process (a crash in one does not hide the others)
//
// Every line is prefixed "RDNA4CENSUS|" so the emulator loop can pick it from the serial console. Metal's own complaints (loader path,
// missing plug-in) go to the unified log: tools/emu-linux.sh --census collects `log show` for them.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#include <unistd.h>
#include <sys/wait.h>

#define OUT(fmt, ...) do { printf("RDNA4CENSUS|" fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

static void registry(void) {
	io_iterator_t it = 0;
	kern_return_t kr = IOServiceGetMatchingServices(0, IOServiceMatching("IOAccelerator"), &it);
	OUT("registry: IOServiceGetMatchingServices(IOAccelerator) = 0x%x", kr);
	int n = 0;
	for (io_service_t s; (s = IOIteratorNext(it)); n++) {
		io_name_t name = "", cls = "";
		IORegistryEntryGetName(s, name);
		IOObjectGetClass(s, cls);
		uint64_t id = 0;
		IORegistryEntryGetRegistryEntryID(s, &id);
		OUT("registry: service %d: name %s class %s registry-id 0x%llx", n, name, cls, id);
		CFMutableDictionaryRef props = NULL;
		if (IORegistryEntryCreateCFProperties(s, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
			NSDictionary *d = (__bridge_transfer NSDictionary *)props;
			for (NSString *k in [[d allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
				if ([k isEqualToString:@"CensusLog"])
					continue;
				OUT("registry:   %s = %s", k.UTF8String, [[d[k] description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String);
			}
		}
		IOObjectRelease(s);
	}
	OUT("registry: %d IOAccelerator service(s)", n);
	IOObjectRelease(it);
}

static void metal(void) {
	OUT("metal: calling MTLCopyAllDevices()");
	NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
	OUT("metal: MTLCopyAllDevices() returned %lu device(s)", (unsigned long)all.count);
	for (id<MTLDevice> d in all)
		OUT("metal:   device '%s' registryID 0x%llx lowPower %d headless %d removable %d", d.name.UTF8String, d.registryID, d.isLowPower, d.isHeadless, d.isRemovable);
	OUT("metal: calling MTLCreateSystemDefaultDevice()");
	id<MTLDevice> def = MTLCreateSystemDefaultDevice();
	OUT("metal: MTLCreateSystemDefaultDevice() = %s", def ? def.name.UTF8String : "nil");
	if (def) {
		OUT("metal: creating a command queue");
		id<MTLCommandQueue> q = [def newCommandQueue];
		OUT("metal: newCommandQueue = %s", q ? "ok" : "nil");
	}
}

static void openTypes(void) {
	io_iterator_t it = 0;
	IOServiceGetMatchingServices(0, IOServiceMatching("IOAccelerator"), &it);
	io_service_t s = IOIteratorNext(it);
	if (!s) {
		OUT("open: no IOAccelerator service");
		return;
	}
	const uint32_t types[] = { 0, 1, 2, 4, 5, 6, 7, 8, 9, 0x100 };
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		io_connect_t c = 0;
		kern_return_t kr = IOServiceOpen(s, mach_task_self(), types[i], &c);
		OUT("open: IOServiceOpen(type %u) = 0x%x", types[i], kr);
		if (kr != KERN_SUCCESS)
			continue;
		for (uint32_t sel = 0; sel < 3; sel++) {
			uint64_t out[4] = {};
			uint32_t outCnt = 4;
			kr = IOConnectCallScalarMethod(c, sel, NULL, 0, out, &outCnt);
			OUT("open:   type %u selector %u scalar call = 0x%x", types[i], sel, kr);
		}
		IOServiceClose(c);
	}
	IOObjectRelease(s);
	IOObjectRelease(it);
}

static int child(const char *self, const char *mode) {
	pid_t p = fork();
	if (p == 0) {
		execl(self, self, mode, (char *)NULL);
		_exit(127);
	}
	int st = 0;
	waitpid(p, &st, 0);
	if (WIFSIGNALED(st))
		OUT("%s: child killed by signal %d", mode, WTERMSIG(st));
	else
		OUT("%s: child exited %d", mode, WEXITSTATUS(st));
	return st;
}

int main(int argc, char **argv) {
	@autoreleasepool {
		const char *mode = argc > 1 ? argv[1] : "all";
		if (!strcmp(mode, "registry")) registry();
		else if (!strcmp(mode, "metal")) metal();
		else if (!strcmp(mode, "open")) openTypes();
		else if (!strcmp(mode, "all")) {
			child(argv[0], "registry");
			child(argv[0], "metal");
			child(argv[0], "open");
		} else {
			fprintf(stderr, "usage: %s registry|metal|open|all\n", argv[0]);
			return 2;
		}
	}
	return 0;
}
