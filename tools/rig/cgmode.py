#!/usr/bin/python
"""cgmode.py [display] WxH[@Hz]   switch a display to one of its modes, for this login session
   cgmode.py                      list the displays and their modes

Displays preferences hides most modes of a monitor it takes for a television. This reaches all of them, to
test a mode switch (docs/todo-dptest.md). WxH is the mode's size in pixels, so "2560x1440" is the plain mode,
not a HiDPI one; without @Hz the 60 Hz one, or the first. `display` is the number in the listing (0 = main).
The change is not saved: logging out or rebooting puts the display back.

Needs the Quartz module: /usr/bin/python has it up to macOS 12.2."""
from __future__ import print_function
import sys
import Quartz as Q

def modes(d):
    return Q.CGDisplayCopyAllDisplayModes(d, {Q.kCGDisplayShowDuplicateLowResolutionModes: True}) or []

def plain(m):   # not HiDPI: as many pixels as points
    return Q.CGDisplayModeGetWidth(m) == Q.CGDisplayModeGetPixelWidth(m)

err, ids, n = Q.CGGetOnlineDisplayList(16, None, None)
ids = sorted(ids or [], key=lambda d: not Q.CGDisplayIsMain(d))
args = sys.argv[1:]
if not args:
    for i, d in enumerate(ids):
        cur = Q.CGDisplayCopyDisplayMode(d)
        print("display %d: vendor %d model %d%s, now %dx%d pixels at %.0f Hz" % (
            i, Q.CGDisplayVendorNumber(d), Q.CGDisplayModelNumber(d), " (main)" if Q.CGDisplayIsMain(d) else "",
            Q.CGDisplayModeGetPixelWidth(cur), Q.CGDisplayModeGetPixelHeight(cur), Q.CGDisplayModeGetRefreshRate(cur)))
        for m in modes(d):
            if plain(m):
                print("   %dx%d@%.0f" % (Q.CGDisplayModeGetPixelWidth(m), Q.CGDisplayModeGetPixelHeight(m),
                                         Q.CGDisplayModeGetRefreshRate(m)))
    sys.exit(0)

index = int(args.pop(0)) if len(args) > 1 else 0
size, _, hz = args[0].partition("@")
w, h = (int(x) for x in size.split("x"))
found = [m for m in modes(ids[index]) if plain(m) and Q.CGDisplayModeGetPixelWidth(m) == w and
         Q.CGDisplayModeGetPixelHeight(m) == h and (not hz or round(Q.CGDisplayModeGetRefreshRate(m)) == int(hz))]
found.sort(key=lambda m: round(Q.CGDisplayModeGetRefreshRate(m)) != 60)
if not found:
    sys.exit("display %d has no plain %s mode; run without arguments for the list" % (index, args[0]))
err, config = Q.CGBeginDisplayConfiguration(None)
Q.CGConfigureDisplayWithDisplayMode(config, ids[index], found[0], None)
err = Q.CGCompleteDisplayConfiguration(config, Q.kCGConfigureForSession)
print("display %d -> %dx%d@%.0f: %s" % (index, w, h, Q.CGDisplayModeGetRefreshRate(found[0]),
                                       "done" if err == 0 else "CoreGraphics error %d" % err))
