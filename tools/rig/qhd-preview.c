// Select one native framebuffer mode through CoreGraphics for this session.
// The caller arms the independent kernel rollback before invoking "set".
#include <ApplicationServices/ApplicationServices.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int native(CGDisplayModeRef m) {
	return CGDisplayModeGetWidth(m) == CGDisplayModeGetPixelWidth(m) &&
	       CGDisplayModeGetHeight(m) == CGDisplayModeGetPixelHeight(m);
}

int main(int argc, char **argv) {
	CGDirectDisplayID display = CGMainDisplayID();
	uint32_t online = 0;
	if (CGGetOnlineDisplayList(0, NULL, &online) || online != 1 ||
	    CGDisplayVendorNumber(display) != 0x10ac || CGDisplayModelNumber(display) != 0xa223 ||
	    CGDisplayIsInMirrorSet(display)) return 2;
	CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
	if (!current) return 3;
	if (argc == 2 && strcmp(argv[1], "current") == 0) {
		printf("%d %zu %zu %.3f %d\n", CGDisplayModeGetIODisplayModeID(current),
		       CGDisplayModeGetPixelWidth(current), CGDisplayModeGetPixelHeight(current),
		       CGDisplayModeGetRefreshRate(current), native(current));
		CGDisplayModeRelease(current);
		return 0;
	}
	if (argc != 3) return 4;
	const int lookup = strcmp(argv[1], "find") == 0;
	if (!lookup && strcmp(argv[1], "set")) return 4;
	char *end = NULL;
	long key = strtol(argv[2], &end, 10);
	if (!*argv[2] || *end || key <= 0 || (lookup && key != 60 && key != 165)) return 4;
	const void *k = kCGDisplayShowDuplicateLowResolutionModes, *v = kCFBooleanTrue;
	CFDictionaryRef opts = CFDictionaryCreate(NULL, &k, &v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, opts);
	if (!modes) return 5;
	int result = 6;
	for (CFIndex i = 0; i < CFArrayGetCount(modes); ++i) {
		CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
		if (!native(m) || !CGDisplayModeIsUsableForDesktopGUI(m)) continue;
		int id = CGDisplayModeGetIODisplayModeID(m);
		if (lookup) {
			if (CGDisplayModeGetWidth(m) != 2560 || CGDisplayModeGetHeight(m) != 1440 ||
			    fabs(CGDisplayModeGetRefreshRate(m) - key) > 0.6) continue;
			printf("%d\n", id);
			result = 0;
		} else {
			if (id != key) continue;
			CGDisplayConfigRef config = NULL;
			if (CGBeginDisplayConfiguration(&config)) break;
			if (CGConfigureDisplayWithDisplayMode(config, display, m, NULL)) {
				CGCancelDisplayConfiguration(config);
				break;
			}
			result = CGCompleteDisplayConfiguration(config, kCGConfigureForSession) ? 7 : 0;
		}
		break;
	}
	CFRelease(modes);
	CFRelease(opts);
	CGDisplayModeRelease(current);
	return result;
}
