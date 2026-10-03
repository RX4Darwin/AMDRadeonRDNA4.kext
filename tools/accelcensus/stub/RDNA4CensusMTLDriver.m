// RDNA4CensusMTLDriver: the E1c stub of docs/metal-spike.md (hub-task-427). A Metal device bundle with no GPU behind it: its principal class
// RDNA4CensusMtlDevice subclasses Apple's MTLIOAccelDevice and logs, to stderr, everything Metal does with it:
//
//   - where and by whom the bundle image was loaded (path of the image, calling process, the backtrace into Metal's loader),
//   - every instance and class method of MTLIOAccelDevice (and its superclasses below NSObject) that Metal calls on the subclass: selector,
//     decoded arguments, return value. The methods are re-routed through forwardInvocation: (class_addMethod with _objc_msgForward), the logger
//     then calls the superclass implementation: the stub changes nothing about what Metal's own code does, it only watches.
//
// Every line starts with "RDNA4STUB|<pid>"; tools/emu-linux.sh --census carries stderr back over the serial console. Nothing here touches a GPU or
// any IOKit service itself (the base class does what Apple's code does: with the census kext at level 1 or 2 that is the measurement).
// Build: `make census-stub` (osxcross); MTLIOAccelDevice is private, so the link uses -undefined dynamic_lookup (the symbol is exported by Metal).
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#define LOG(fmt, ...) fprintf(stderr, "RDNA4STUB|%d " fmt "\n", getpid(), ##__VA_ARGS__)

// Private base class, resolved by dyld from Metal.framework when the bundle loads.
@interface MTLIOAccelDevice : NSObject
@end

@interface NSInvocation (RDNA4Stub)
- (void)invokeUsingIMP:(IMP)imp;
@end

enum { kMaxSel = 2048, kFullLogs = 12 };
static struct {
	SEL sel;
	BOOL meta;
	atomic_uint count;
} gSeen[kMaxSel];
static atomic_uint gSeenCount;
static atomic_uint gHooked, gSkippedStret;

static int seenIndex(SEL sel, BOOL meta) {
	unsigned n = atomic_load(&gSeenCount);
	for (unsigned i = 0; i < n && i < kMaxSel; i++)
		if (gSeen[i].sel == sel && gSeen[i].meta == meta)
			return (int)i;
	return -1;
}

static void describeValue(char *out, size_t cap, const char *type, const void *p) {
	while (*type == 'r' || *type == 'n' || *type == 'N' || *type == 'o' || *type == 'O' || *type == 'R' || *type == 'V')
		type++;
	switch (*type) {
	case 'c': snprintf(out, cap, "%d", *(const signed char *)p); break;
	case 'C': snprintf(out, cap, "%u", *(const unsigned char *)p); break;
	case 'B': snprintf(out, cap, "%s", *(const _Bool *)p ? "YES" : "NO"); break;
	case 's': snprintf(out, cap, "%d", *(const short *)p); break;
	case 'S': snprintf(out, cap, "%u", *(const unsigned short *)p); break;
	case 'i': snprintf(out, cap, "%d", *(const int *)p); break;
	case 'I': snprintf(out, cap, "0x%x", *(const unsigned *)p); break;
	case 'l': case 'q': snprintf(out, cap, "%lld", (long long)*(const long long *)p); break;
	case 'L': case 'Q': snprintf(out, cap, "0x%llx", *(const unsigned long long *)p); break;
	case 'f': snprintf(out, cap, "%g", *(const float *)p); break;
	case 'd': snprintf(out, cap, "%g", *(const double *)p); break;
	case ':': snprintf(out, cap, "@selector(%s)", *(SEL const *)p ? sel_getName(*(SEL const *)p) : "nil"); break;
	case '*': snprintf(out, cap, "%p", *(void *const *)p); break;
	case '#': snprintf(out, cap, "class %s", *(Class const *)p ? class_getName(*(Class const *)p) : "nil"); break;
	case '@': {
		const void *o = *(void *const *)p;
		// Only the class name: no message is sent to an argument (it may be a half-built object).
		if (!o) snprintf(out, cap, "nil");
		else if ((uintptr_t)o & 1) snprintf(out, cap, "%p(tagged)", o);
		else snprintf(out, cap, "%p<%s>", o, object_getClassName((__bridge id)o));
		break;
	}
	case '^': snprintf(out, cap, "%p", *(void *const *)p); break;
	case '{': case '(': {
		NSUInteger size = 0, align = 0;
		@try { NSGetSizeAndAlignment(type, &size, &align); } @catch (id e) { size = 0; }
		size_t n = size < 16 ? size : 16, at = (size_t)snprintf(out, cap, "struct[%lu]=", (unsigned long)size);
		for (size_t i = 0; i < n && at + 3 < cap; i++)
			at += (size_t)snprintf(out + at, cap - at, "%02x", ((const unsigned char *)p)[i]);
		break;
	}
	case 'v': snprintf(out, cap, "void"); break;
	default: snprintf(out, cap, "?(%s)", type); break;
	}
}

static void logLoaded(const char *how) {
	Dl_info info = {};
	dladdr((const void *)&logLoaded, &info);
	LOG("%s: image %s, process %s", how, info.dli_fname ? info.dli_fname : "?", getprogname());
	void *frames[14];
	int n = backtrace(frames, 14);
	for (int i = 0; i < n; i++) {
		Dl_info fi = {};
		if (dladdr(frames[i], &fi))
			LOG("  frame %d: %s + 0x%lx (%s)", i, fi.dli_sname ? fi.dli_sname : "?", (unsigned long)((char *)frames[i] - (char *)fi.dli_saddr),
			    fi.dli_fname ? fi.dli_fname : "?");
		else
			LOG("  frame %d: %p", i, frames[i]);
	}
}

static void summary(void) {
	unsigned n = atomic_load(&gSeenCount);
	LOG("summary: %u selectors hooked, %u skipped (struct return), %u called:", atomic_load(&gHooked), atomic_load(&gSkippedStret), n);
	for (unsigned i = 0; i < n && i < kMaxSel; i++)
		LOG("  %c[%s] x%u", gSeen[i].meta ? '+' : '-', sel_getName(gSeen[i].sel), atomic_load(&gSeen[i].count));
}

__attribute__((constructor)) static void stubConstructor(void) {
	logLoaded("constructor (bundle image mapped)");
	atexit(summary);
}

@interface RDNA4CensusMtlDevice : MTLIOAccelDevice
@end

static void hookClass(Class target, Class sup, BOOL meta) {
	Class root = meta ? object_getClass([NSObject class]) : [NSObject class];
	for (Class c = sup; c && c != root; c = class_getSuperclass(c)) {
		unsigned n = 0;
		Method *list = class_copyMethodList(c, &n);
		for (unsigned i = 0; i < n; i++) {
			SEL sel = method_getName(list[i]);
			const char *name = sel_getName(sel), *types = method_getTypeEncoding(list[i]);
			if (!types || name[0] == '.' || !strcmp(name, "dealloc") || !strcmp(name, "forwardInvocation:") ||
			    !strcmp(name, "methodSignatureForSelector:"))
				continue;
			if (meta ? [NSObject respondsToSelector:sel] : [NSObject instancesRespondToSelector:sel])
				continue;
			if (class_getInstanceMethod(target, sel) != class_getInstanceMethod(sup, sel) && class_getInstanceMethod(target, sel))
				continue;   // the stub's own, or already hooked
			BOOL stret = NO;
			@try {
				NSMethodSignature *sig = [NSMethodSignature signatureWithObjCTypes:types];
				stret = sig.methodReturnType[0] == '{' && sig.methodReturnLength > 16;
			} @catch (id e) {
				continue;
			}
			if (stret) {
				atomic_fetch_add(&gSkippedStret, 1);
				continue;
			}
			if (class_addMethod(target, sel, _objc_msgForward, types))
				atomic_fetch_add(&gHooked, 1);
		}
		free(list);
	}
}

@implementation RDNA4CensusMtlDevice

+ (void)load {
	logLoaded("+load (class registered)");
	Class sup = class_getSuperclass(self);
	hookClass(self, sup, NO);
	hookClass(object_getClass(self), object_getClass(sup), YES);
	LOG("hooked %u selectors (%u skipped: struct return), superclass %s", atomic_load(&gHooked), atomic_load(&gSkippedStret), class_getName(sup));
}

static NSMethodSignature *signatureFor(Class from, SEL sel, BOOL meta) {
	Method m = meta ? class_getClassMethod(from, sel) : class_getInstanceMethod(from, sel);
	return m && method_getTypeEncoding(m) ? [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(m)] : nil;
}

static void route(id target, NSInvocation *inv, BOOL meta) {
	Class mine = [RDNA4CensusMtlDevice class];
	Class sup = class_getSuperclass(mine);
	SEL sel = inv.selector;
	int idx = seenIndex(sel, meta);
	if (idx < 0) {
		unsigned slot = atomic_fetch_add(&gSeenCount, 1);
		if (slot < kMaxSel) {
			gSeen[slot].sel = sel;
			gSeen[slot].meta = meta;
			idx = (int)slot;
		}
	}
	unsigned count = idx >= 0 ? atomic_fetch_add(&gSeen[idx].count, 1) + 1 : 0;
	BOOL full = count <= kFullLogs;
	NSMethodSignature *sig = inv.methodSignature;
	char args[512] = "";
	if (full) {
		size_t at = 0;
		for (NSUInteger i = 2; i < sig.numberOfArguments && at + 40 < sizeof(args); i++) {
			char buf[160], value[64] __attribute__((aligned(16))) = {};
			const char *t = [sig getArgumentTypeAtIndex:i];
			if (sig.numberOfArguments > 2 && (t[0] == '{' || t[0] == '(')) {
				NSUInteger sz = 0;
				NSGetSizeAndAlignment(t, &sz, NULL);
				if (sz > sizeof(value)) { at += (size_t)snprintf(args + at, sizeof(args) - at, " <struct %lu bytes>", (unsigned long)sz); continue; }
			}
			[inv getArgument:value atIndex:(NSInteger)i];
			describeValue(buf, sizeof(buf), t, value);
			at += (size_t)snprintf(args + at, sizeof(args) - at, " %s", buf);
		}
		LOG("%c[%s]%s (call %u)", meta ? '+' : '-', sel_getName(sel), args, count);
	}
	IMP imp = meta ? class_getMethodImplementation(object_getClass(sup), sel) : class_getMethodImplementation(sup, sel);
	[inv invokeUsingIMP:imp];
	if (full && sig.methodReturnType[0] != 'v') {
		char ret[160], value[64] __attribute__((aligned(16))) = {};
		if (sig.methodReturnLength <= sizeof(value)) {
			[inv getReturnValue:value];
			describeValue(ret, sizeof(ret), sig.methodReturnType, value);
			LOG("  -> %s", ret);
		}
	}
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
	NSMethodSignature *s = signatureFor(class_getSuperclass([RDNA4CensusMtlDevice class]), sel, NO);
	return s ? s : [super methodSignatureForSelector:sel];
}
- (void)forwardInvocation:(NSInvocation *)inv { route(self, inv, NO); }

+ (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
	NSMethodSignature *s = signatureFor(class_getSuperclass([RDNA4CensusMtlDevice class]), sel, YES);
	return s ? s : [super methodSignatureForSelector:sel];
}
+ (void)forwardInvocation:(NSInvocation *)inv { route(self, inv, YES); }

@end
