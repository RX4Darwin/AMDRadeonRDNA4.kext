// check.swift: build.sh runs this before it builds the app: version order.
import Foundation

assert(Driver.older("0.4.0", than: "0.5.0") && !Driver.older("0.5.0", than: "0.4.0") && !Driver.older("0.5.0", than: "0.5.0-test1"))
print("gadget checks passed")
