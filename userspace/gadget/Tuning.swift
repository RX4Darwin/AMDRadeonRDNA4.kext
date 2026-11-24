// Tuning.swift: the driver's persistent settings (AMDRadeonRDNA4's rdna4-settings NVRAM variable, through debug.rdna4_settings), taken at the
// next boot. Only the options the driver accepts there (src/trial.hpp settingAllowed); a boot-arg of the same name still wins.

import Foundation

struct Tuning: Equatable {
    var hardwareCursor = true        // rdna4-pvcursor
    var openGL = true                // rdna4-accelgl: OpenGL apps on the card
    var secondDisplayPort = true     // rdna4-head2dp
    var fastModes = true             // rdna4-pvfast: DisplayPort modes above the boot's clock
    var hotPlug = true               // rdna4-hotplug
    var clockLimitMHz = 0            // rdna4-gfxcap, 0: none
    var clockLimitWasSet = false     // a limit was in force: lifting it needs rdna4-gfxcap=0 (the card keeps it across warm reboots)
    var turnedOff = false            // the driver turned the settings off after a boot that did not last

    static let sysctl = "debug.rdna4_settings"

    /// The settings the driver has; nil when the driver (0.5.0 or later) is not running.
    static func current() -> Tuning? {
        var size = 0
        guard sysctlbyname(sysctl, nil, &size, nil, 0) == 0 else { return nil }
        var buf = [CChar](repeating: 0, count: size + 1)
        guard sysctlbyname(sysctl, &buf, &size, nil, 0) == 0 else { return nil }
        return Tuning(text: String(cString: buf))
    }

    init() {}

    init(text: String) {
        let words = text.split(separator: " ").map(String.init)
        turnedOff = words.first == "off"
        var values: [String: Int] = [:]
        for w in words.dropFirst() {
            let kv = w.split(separator: "=", maxSplits: 1)
            if kv.count == 2, let v = Int(kv[1]) { values[String(kv[0])] = v }
        }
        hardwareCursor = values["rdna4-pvcursor"].map { $0 != 0 } ?? true
        openGL = values["rdna4-accelgl"].map { $0 != 0 } ?? true
        secondDisplayPort = values["rdna4-head2dp"].map { $0 != 0 } ?? true
        fastModes = values["rdna4-pvfast"].map { $0 != 0 } ?? true
        hotPlug = values["rdna4-hotplug"].map { $0 != 0 } ?? true
        clockLimitMHz = values["rdna4-gfxcap"] ?? 0
        clockLimitWasSet = values["rdna4-gfxcap"] != nil
    }

    /// The variable's text: only what differs from the driver's defaults.
    var text: String {
        var words: [String] = []
        if !hardwareCursor { words.append("rdna4-pvcursor=0") }
        if !openGL { words.append("rdna4-accelgl=0") }
        if !secondDisplayPort { words.append("rdna4-head2dp=0") }
        if !fastModes { words.append("rdna4-pvfast=0") }
        if !hotPlug { words.append("rdna4-hotplug=0") }
        if clockLimitMHz > 0 { words.append("rdna4-gfxcap=\(clockLimitMHz)") } else if clockLimitWasSet { words.append("rdna4-gfxcap=0") }
        return words.isEmpty ? "" : "on " + words.joined(separator: " ")
    }

    /// Writes them for the next boot (an administrator's password).
    func apply() throws {
        _ = try Admin.run("/usr/sbin/sysctl \(Tuning.sysctl)=\(Driver.q(text))")
    }
}
