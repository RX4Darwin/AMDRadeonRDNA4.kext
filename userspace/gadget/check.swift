// check.swift: build.sh runs this before it builds the app: version order and the settings text.
import Foundation

assert(Driver.older("0.4.0", than: "0.5.0") && !Driver.older("0.5.0", than: "0.4.0") && !Driver.older("0.5.0", than: "0.5.0-test1"))
assert(Tuning(text: "on rdna4-pvcursor=0 rdna4-gfxcap=2000").text == "on rdna4-pvcursor=0 rdna4-gfxcap=2000")
var lifted = Tuning(text: "on rdna4-gfxcap=2000")
lifted.clockLimitMHz = 0
assert(lifted.text == "on rdna4-gfxcap=0")   // a limit the card keeps across warm reboots is lifted, not dropped
assert(Tuning(text: "off rdna4-hotplug=0").turnedOff && !Tuning(text: "off rdna4-hotplug=0").hotPlug)
assert(Tuning().text == "")
print("gadget checks passed")
