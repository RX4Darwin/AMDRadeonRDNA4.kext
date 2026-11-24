// Sensors.swift: the GPU's readings, from the IOAccelerator's PerformanceStatistics (AMDRadeonRDNA4 since 0.5.0, Apple's AMD drivers) and,
// where a VirtualSMC GPU plug-in publishes them (SMCRadeonSensors and the like), the SMC's GPU keys.

import Foundation
import IOKit

struct GPUReading {
    var name = "GPU"
    var driver = ""                       // the accelerator's class (RDNA4Accelerator, AMDRadeonX6000_..., ...)
    var edgeC: Double?
    var hotspotC: Double?
    var powerW: Double?
    var fanRPM: Double?
    var coreMHz: Double?
    var memoryMHz: Double?
    var utilization: Double?
    var vramUsed: Double?
    var vramTotal: Double?
    var smc: [String: Double] = [:]       // SMC GPU keys that answered

    /// The temperature to show: the driver's edge reading, else the first SMC one.
    var temperature: Double? { edgeC ?? smc.sorted { $0.key < $1.key }.first(where: { $0.key.hasPrefix("T") })?.value }
}

enum Sensors {
    /// The SMC keys GPU plug-ins publish (proximity, die, diode): read when present, never written.
    static let smcKeys = ["TGDD", "TG0D", "TG0P", "TG0T", "TG1D", "TG1P", "PG0R", "PG0C"]

    static func read() -> [GPUReading] {
        var out: [GPUReading] = []
        var iter: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &iter) == KERN_SUCCESS else { return out }
        defer { IOObjectRelease(iter) }
        while case let entry = IOIteratorNext(iter), entry != 0 {
            defer { IOObjectRelease(entry) }
            guard let stats = IORegistryEntryCreateCFProperty(entry, "PerformanceStatistics" as CFString, kCFAllocatorDefault, 0)?
                    .takeRetainedValue() as? [String: Any] else { continue }
            func n(_ key: String) -> Double? { (stats[key] as? NSNumber)?.doubleValue }
            var r = GPUReading()
            var cls = [CChar](repeating: 0, count: 128)
            IOObjectGetClass(entry, &cls)
            r.driver = String(cString: cls)
            r.name = modelName(entry) ?? r.driver
            r.edgeC = n("Temperature(C)")
            r.hotspotC = n("Hotspot Temperature(C)")
            r.powerW = n("Total Power(W)")
            r.fanRPM = n("Fan Speed(RPM)")
            r.coreMHz = n("Core Clock(MHz)")
            r.memoryMHz = n("Memory Clock(MHz)")
            r.utilization = n("Device Utilization %")
            r.vramUsed = n("vramUsedBytes") ?? n("inUseVidMemoryBytes")
            r.vramTotal = n("VRAM,totalBytes")
            out.append(r)
        }
        // SMC readings: with the first GPU (one card is the case these plug-ins serve), or a GPU of their own if no accelerator reported
        let smc = SMC.shared.map { s in Dictionary(uniqueKeysWithValues: smcKeys.compactMap { k in s.read(k).map { (k, $0) } }) } ?? [:]
        if !smc.isEmpty {
            if out.isEmpty { out.append(GPUReading(name: "GPU (SMC)")) }
            out[0].smc = smc
        }
        // the accelerators with readings first (a second, idle accelerator of the same card reports none)
        return out.sorted { ($0.edgeC != nil ? 0 : 1, $0.name) < ($1.edgeC != nil ? 0 : 1, $1.name) }
    }

    /// The PCI device's "model" above the accelerator.
    private static func modelName(_ entry: io_registry_entry_t) -> String? {
        let opts = IOOptionBits(kIORegistryIterateRecursively | kIORegistryIterateParents)
        guard let v = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, "model" as CFString, kCFAllocatorDefault, opts) else { return nil }
        if let d = v as? Data { return String(decoding: d.prefix { $0 != 0 }, as: UTF8.self) }
        return v as? String
    }
}

/// AppleSMC's user client: reads keys by the usual 80-byte SMCParamStruct (selector 2, kSMCHandleYPCEvent).
final class SMC {
    static let shared = SMC()
    private var conn: io_connect_t = 0

    private init?() {
        let svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"))
        guard svc != 0 else { return nil }
        defer { IOObjectRelease(svc) }
        guard IOServiceOpen(svc, mach_task_self_, 0, &conn) == KERN_SUCCESS else { return nil }
    }

    deinit { IOServiceClose(conn) }

    private func call(_ input: [UInt8]) -> [UInt8]? {
        var output = [UInt8](repeating: 0, count: 80)
        var size = 80
        let kr = IOConnectCallStructMethod(conn, 2, input, 80, &output, &size)
        return kr == KERN_SUCCESS && output[40] == 0 ? output : nil   // result byte at +40
    }

    private static func fourCC(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

    /// A key's value as a number (sp78, flt, fpe2, ui8/16/32), nil if the SMC does not have it.
    func read(_ key: String) -> Double? {
        // offsets: key 0, keyInfo.dataSize 28, keyInfo.dataType 32, result 40, data8 42, bytes 48
        var input = [UInt8](repeating: 0, count: 80)
        withUnsafeBytes(of: SMC.fourCC(key)) { input.replaceSubrange(0..<4, with: $0) }
        input[42] = 9   // kSMCGetKeyInfo
        guard let info = call(input) else { return nil }
        let size = info[28..<32].withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
        let type = info[32..<36].withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
        input.replaceSubrange(28..<32, with: info[28..<32])
        input[42] = 5   // kSMCReadKey
        guard size > 0, size <= 32, let data = call(input) else { return nil }
        let b = Array(data[48..<48 + Int(size)])
        switch type {
        case SMC.fourCC("sp78") where b.count >= 2: return Double(Int16(bitPattern: UInt16(b[0]) << 8 | UInt16(b[1]))) / 256
        case SMC.fourCC("fpe2") where b.count >= 2: return Double(UInt16(b[0]) << 8 | UInt16(b[1])) / 4
        case SMC.fourCC("flt ") where b.count >= 4: return Double(b.withUnsafeBytes { $0.loadUnaligned(as: Float32.self) })
        case SMC.fourCC("ui8 "): return Double(b[0])
        case SMC.fourCC("ui16") where b.count >= 2: return Double(UInt16(b[0]) << 8 | UInt16(b[1]))
        case SMC.fourCC("ui32") where b.count >= 4: return Double(b.reduce(UInt32(0)) { $0 << 8 | UInt32($1) })
        default: return nil
        }
    }
}
