// cgmodes — the displays CoreGraphics sees and every mode of each, as the diagnostic script prints them. It replaces a
// Python snippet that needed the Quartz module, which /usr/bin/python had only up to macOS 12.2. Read-only.
//
//   cgmodes      one line per display, then one per mode ("*" = the current one)
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>

int main(void) {
	CGDirectDisplayID ids[16];
	uint32_t n = 0;
	if (CGGetOnlineDisplayList(16, ids, &n) != kCGErrorSuccess) {
		printf("CGGetOnlineDisplayList failed\n");
		return 1;
	}
	const void *key = kCGDisplayShowDuplicateLowResolutionModes;
	CFDictionaryRef opts = CFDictionaryCreate(NULL, &key, (const void **)&kCFBooleanTrue, 1,
	                                          &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	for (uint32_t i = 0; i < n; i++) {
		CGDirectDisplayID d = ids[i];
		CGRect b = CGDisplayBounds(d);
		printf("display 0x%x vendor %u model %u at %d,%d%s%s\n", d, CGDisplayVendorNumber(d), CGDisplayModelNumber(d),
		       (int)b.origin.x, (int)b.origin.y, CGDisplayIsMain(d) ? " main" : "", CGDisplayIsInMirrorSet(d) ? " mirrored" : "");
		CGDisplayModeRef cur = CGDisplayCopyDisplayMode(d);
		CFArrayRef modes = CGDisplayCopyAllDisplayModes(d, opts);
		for (CFIndex k = 0; modes && k < CFArrayGetCount(modes); k++) {
			CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, k);
			// The same mode as the current one: same IO mode, same size in points and in pixels.
			int isCur = cur && CGDisplayModeGetIODisplayModeID(m) == CGDisplayModeGetIODisplayModeID(cur) &&
			            CGDisplayModeGetWidth(m) == CGDisplayModeGetWidth(cur) &&
			            CGDisplayModeGetPixelWidth(m) == CGDisplayModeGetPixelWidth(cur);
			printf(" %s id %d %zux%zu (%zux%zu pixels) %.3f Hz ioflags 0x%x%s\n", isCur ? "*" : " ",
			       CGDisplayModeGetIODisplayModeID(m), CGDisplayModeGetWidth(m), CGDisplayModeGetHeight(m),
			       CGDisplayModeGetPixelWidth(m), CGDisplayModeGetPixelHeight(m), CGDisplayModeGetRefreshRate(m),
			       CGDisplayModeGetIOFlags(m), CGDisplayModeIsUsableForDesktopGUI(m) ? "" : " not for the desktop");
		}
		if (modes) CFRelease(modes);
		if (cur) CGDisplayModeRelease(cur);
	}
	CFRelease(opts);
	return 0;
}
