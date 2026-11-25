// Fan.swift: the Fan Control tab. The card's limits and what is applied come from the kext (debug.rdna4_fan_limits, debug.rdna4_fan);
// Apply writes the settings as debug.rdna4_fan commands to fan.conf, which the package's root job org.rdna4.fan applies at once and at
// every boot (docs/fan-control-plan.md).

import Charts
import SwiftUI

enum FanControl {
    static let conf = URL(fileURLWithPath: "/Library/Application Support/AMDRadeonRDNA4/fan.conf")

    static func sysctl(_ name: String) -> String? {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var buf = [CChar](repeating: 0, count: size)
        guard sysctlbyname(name, &buf, &size, nil, 0) == 0 else { return nil }
        return String(cString: buf)
    }

    /// The card's limits: "temp 25 100; pwm 15 100; stop 30 80; ..." as key -> (min, max)
    static func limits() -> [String: ClosedRange<Int>] {
        var out: [String: ClosedRange<Int>] = [:]
        for part in (sysctl("debug.rdna4_fan_limits") ?? "").split(separator: ";") {
            let f = part.split(separator: " ").map(String.init)
            if f.count == 3, let lo = Int(f[1]), let hi = Int(f[2]), lo <= hi { out[f[0]] = lo...hi }
            else if f.count == 2, let v = Int(f[1]) { out[f[0]] = v...v }   // (curve, legacy, zero: allowed 1 or not 0)
        }
        return out
    }

    /// The commands written for the root job: `reset` first, then what differs from the vBIOS's settings
    static func write(_ lines: [String]) -> String? {
        do {
            try (lines.joined(separator: "\n") + "\n").write(to: conf, atomically: false, encoding: .utf8)
            return nil
        } catch {
            return FileManager.default.fileExists(atPath: conf.path)
                ? "Could not write the fan settings: \(error.localizedDescription)"
                : "The driver package's fan helper is not installed (fan.conf is missing): install the package again."
        }
    }
}

struct FanView: View {
    @ObservedObject var model: GadgetModel
    @AppStorage("FanCustom") private var custom = false
    static let defaultCurve = "40:30,55:40,65:50,75:70,85:100"   // temperature : speed %, five points
    @AppStorage("FanCurve") private var curveText = FanView.defaultCurve
    @AppStorage("FanZeroRPM") private var zeroRPM = true
    @AppStorage("FanStopTemp") private var stopTemp = 50
    @State private var limits: [String: ClosedRange<Int>] = [:]
    @State private var applied = ""
    @State private var message: String?

    private var temps: ClosedRange<Int> { limits["temp"] ?? 25...100 }
    private var speeds: ClosedRange<Int> { limits["pwm"] ?? 15...100 }
    private var stops: ClosedRange<Int> { limits["stop"] ?? 30...80 }

    private var points: [(t: Int, p: Int)] {
        func parse(_ text: String) -> [(t: Int, p: Int)] {
            text.split(separator: ",").compactMap { pair -> (t: Int, p: Int)? in
                let v = pair.split(separator: ":").compactMap { Int($0) }
                return v.count == 2 ? (v[0], v[1]) : nil
            }
        }
        let p = parse(curveText)
        return p.count == 5 ? p : parse(Self.defaultCurve)
    }

    /// Point i set to (t, p), its neighbours kept in order: temperatures rising, speeds not falling, all within the limits
    private func set(_ i: Int, t: Int? = nil, p: Int? = nil) {
        var pts = points
        if let t { pts[i].t = min(max(t, temps.lowerBound + i), temps.upperBound - (4 - i)) }
        if let p { pts[i].p = min(max(p, speeds.lowerBound), speeds.upperBound) }
        for j in i + 1 ..< 5 { pts[j].t = max(pts[j].t, pts[j - 1].t + 1); pts[j].p = max(pts[j].p, pts[j - 1].p) }
        for j in stride(from: i - 1, through: 0, by: -1) { pts[j].t = min(pts[j].t, pts[j + 1].t - 1); pts[j].p = min(pts[j].p, pts[j + 1].p) }
        curveText = pts.map { "\($0.t):\($0.p)" }.joined(separator: ",")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if limits.isEmpty {
                Text("The driver reports no fan settings (AMDRadeonRDNA4 not running, or a kext older than 0.5).")
                    .font(.caption).foregroundStyle(.secondary)
            } else {
                Picker("Fan", selection: $custom) {
                    Text("Auto (the card's own)").tag(false)
                    Text("Custom curve").tag(true)
                }
                .pickerStyle(.segmented).labelsHidden()
                // a curve with zero RPM on stays stopped below the stop temperature (the card's rule, as in Adrenalin): choosing a
                // curve turns zero RPM off, and it can be turned on again
                .onChange(of: custom) { if $0 { zeroRPM = false } }
                if custom {
                    Chart {
                        ForEach(Array(points.enumerated()), id: \.offset) { _, pt in
                            LineMark(x: .value("°C", pt.t), y: .value("%", pt.p))
                            PointMark(x: .value("°C", pt.t), y: .value("%", pt.p))
                        }
                        if let g = model.gpus.first, let t = g.temperature {
                            RuleMark(x: .value("now", t)).foregroundStyle(.orange.opacity(0.6))
                        }
                    }
                    .chartXScale(domain: temps.lowerBound...temps.upperBound)
                    .chartYScale(domain: 0...100)
                    .frame(height: 90)
                    ForEach(0 ..< 5, id: \.self) { i in
                        HStack(spacing: 6) {
                            Stepper("\(points[i].t) °C", value: Binding(get: { points[i].t }, set: { set(i, t: $0) }), in: temps)
                                .monospacedDigit().frame(width: 92, alignment: .leading)
                            Slider(value: Binding(get: { Double(points[i].p) }, set: { set(i, p: Int($0.rounded())) }),
                                   in: Double(speeds.lowerBound)...Double(speeds.upperBound))
                            Text("\(points[i].p) %").monospacedDigit().frame(width: 40, alignment: .trailing)
                        }
                        .font(.caption)
                    }
                }
                if limits["zero"] == 1...1 {
                    HStack {
                        Toggle("Stop the fan when cool (zero RPM)", isOn: $zeroRPM).toggleStyle(.checkbox)
                        Spacer()
                        Stepper("below \(stopTemp) °C", value: $stopTemp, in: stops).disabled(!zeroRPM).monospacedDigit()
                    }
                    .font(.caption)
                    if custom && zeroRPM {
                        Text("With zero RPM on, the fan stays stopped below \(stopTemp) °C whatever the curve says; the curve applies above it.")
                            .font(.caption2).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
                    }
                }
                HStack {
                    Button("Apply") { apply() }
                    Button("Reset to the card's") { reset() }
                    Spacer()
                    if let g = model.gpus.first {
                        Text(fanStatus(g)).font(.caption).monospacedDigit().foregroundStyle(.secondary)
                    }
                }
                Text(message ?? "Now: \(applied)").font(.caption2).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .onAppear { refresh() }
    }

    /// The fan's speed, and why it stands when zero RPM holds it (on in what is applied, the card below its stop temperature)
    private func fanStatus(_ g: GPUReading) -> String {
        guard let rpm = g.fanRPM else { return "" }
        if rpm > 0 { return String(format: "%.0f RPM", rpm) }
        return applied.contains("zero 1") || applied.hasPrefix("auto (") ? "fan stopped (zero RPM)" : "fan stopped"
    }

    private func refresh() {
        limits = FanControl.limits()
        applied = FanControl.sysctl("debug.rdna4_fan") ?? "unknown"
        stopTemp = min(max(stopTemp, stops.lowerBound), stops.upperBound)
    }

    private func apply() {
        var lines = ["reset"]
        if custom { lines.append("curve " + points.map { "\($0.t):\($0.p)" }.joined(separator: ",")) }
        if limits["zero"] == 1...1 {
            lines.append("zero \(zeroRPM ? 1 : 0)")
            if zeroRPM { lines.append("stop \(stopTemp)") }
        }
        finish(FanControl.write(lines))
    }

    private func reset() {
        custom = false
        zeroRPM = true
        finish(FanControl.write(["reset"]))
    }

    /// (the root job applies the file within a moment; what the kext then runs is shown)
    private func finish(_ error: String?) {
        message = error
        guard error == nil else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { refresh() }
    }
}
