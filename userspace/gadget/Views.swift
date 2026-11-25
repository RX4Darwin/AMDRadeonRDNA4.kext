// Views.swift: the popover: the card's readings, the driver (versions, update, removal), its tweaks (Super Resolution).

import SwiftUI

struct GadgetView: View {
    static let size = NSSize(width: 360, height: 600)
    @ObservedObject var model: GadgetModel
    @State private var tab = 0
    @State private var confirmUninstall = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            readings
            Picker("", selection: $tab) {
                Text("Driver").tag(0)
                Text("Tweaks").tag(1)
                Text("Gadget").tag(2)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            switch tab {
            case 0: driver
            case 1: TweaksView()
            default: gadget
            }
            if let busy = model.busy {
                HStack { ProgressView().controlSize(.small); Text(busy).font(.caption) }
            }
            if !model.log.isEmpty {
                ScrollView {
                    Text(model.log).font(.system(size: 10, design: .monospaced)).textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 60)
                .padding(6)
                .background(RoundedRectangle(cornerRadius: 6).fill(Color.primary.opacity(0.06)))
            }
        }
        .padding(14)
        .frame(width: GadgetView.size.width, height: GadgetView.size.height, alignment: .top)
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            Image(systemName: "cpu").foregroundStyle(.red)
            VStack(alignment: .leading, spacing: 1) {
                Text(model.gpus.first?.name ?? "No GPU readings").font(.headline)
                Text(model.build.map { "AMDRadeonRDNA4 \($0)" } ?? (model.installed.map { "AMDRadeonRDNA4 \($0) (not running)" } ?? "AMDRadeonRDNA4 not installed"))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private var readings: some View {
        if let g = model.gpus.first {
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 4) {
                GridRow {
                    Reading(label: "Temperature", value: g.temperature.map { String(format: "%.0f °C", $0) })
                    Reading(label: "Hotspot", value: g.hotspotC.map { String(format: "%.0f °C", $0) })
                    Reading(label: "Power", value: g.powerW.map { String(format: "%.0f W", $0) })
                }
                GridRow {
                    Reading(label: "GPU clock", value: g.coreMHz.map { String(format: "%.0f MHz", $0) })
                    Reading(label: "Memory", value: g.memoryMHz.map { String(format: "%.0f MHz", $0) })
                    Reading(label: "Load", value: g.utilization.map { String(format: "%.0f %%", $0) })
                }
                GridRow {
                    Reading(label: "Fan", value: g.fanRPM.map { $0 == 0 ? "stopped" : String(format: "%.0f RPM", $0) })
                    Reading(label: "VRAM", value: g.vramUsed.map { used in   // in GiB, as macOS and AMD state a card's memory
                        g.vramTotal.map { String(format: "%.1f / %.0f GB", used / 0x1p30, $0 / 0x1p30) } ?? String(format: "%.1f GB", used / 0x1p30)
                    })
                    Reading(label: "Driver", value: g.driver.isEmpty ? nil : g.driver.replacingOccurrences(of: "Accelerator", with: ""))
                }
            }
            HStack(spacing: 10) {
                Sparkline(values: model.history, color: .orange, label: "°C, 2 min")
                Sparkline(values: model.powerHistory, color: .red, label: "W, 2 min")
            }
            .frame(height: 38)
            if !g.smc.isEmpty {
                Text("SMC: " + g.smc.sorted { $0.key < $1.key }.map { String(format: "%@ %.0f", $0.key, $0.value) }.joined(separator: "  "))
                    .font(.caption2).foregroundStyle(.secondary)
            }
        } else {
            Text("No GPU readings: neither AMDRadeonRDNA4 nor a VirtualSMC GPU plug-in reports any.").font(.caption).foregroundStyle(.secondary)
        }
    }

    private var driver: some View {
        VStack(alignment: .leading, spacing: 6) {
            LabeledContent("Installed", value: model.installed ?? "none")
            if let l = model.latest {
                LabeledContent("Latest") {
                    Link(l.version + (l.prerelease ? " (test build)" : ""), destination: l.html_url)
                }
            } else if let e = model.checkError {
                Text("Could not check for releases: \(e)").font(.caption).foregroundStyle(.secondary)
            }
            Toggle("Include test builds", isOn: $model.includePrereleases).font(.caption)
            HStack {
                if let l = model.latest, model.installed == nil || model.updateAvailable || model.installed == l.version {
                    Button(model.installed == nil ? "Install \(l.version)" : model.updateAvailable ? "Update to \(l.version)" : "Reinstall \(l.version)") {
                        model.install(l)
                    }
                    .disabled(model.busy != nil || l.releaseZip == nil)
                } else if model.latest != nil {
                    Text("Up to date").font(.caption).foregroundStyle(.secondary)
                }
                Button("Check now") { Task { await model.checkReleases() } }
                Spacer()
                Button("Uninstall…", role: .destructive) { confirmUninstall = true }
                    .disabled(model.busy != nil || model.installed == nil)
            }
            Text("Installing asks for an administrator's password, puts the kext into the EFI OpenCore booted from (the old config.plist kept as the way back) and needs a restart.")
                .font(.caption2).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .confirmationDialog("Remove AMDRadeonRDNA4?", isPresented: $confirmUninstall) {
            Button("Remove and keep the EFI backup", role: .destructive) { model.uninstall() }
        } message: {
            Text("The Metal driver, its helpers and the kext are removed, and config.plist goes back to what it was before the last install. The displays run without acceleration after the restart.")
        }
    }

    private var gadget: some View {
        VStack(alignment: .leading, spacing: 6) {
            Toggle("Show power in the menu bar", isOn: $model.showPower)
            Toggle("Open at login", isOn: Binding(get: { model.launchAtLogin }, set: { model.launchAtLogin = $0 }))
            HStack {
                Spacer()
                Button("Quit RDNA4 Gadget") { NSApp.terminate(nil) }
            }
        }
        .toggleStyle(.checkbox)
    }
}

private struct Reading: View {
    let label: String
    let value: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(label.uppercased()).font(.system(size: 8, weight: .semibold, design: .monospaced)).foregroundStyle(.secondary)
            Text(value ?? "–").font(.system(size: 13, weight: .medium).monospacedDigit())
        }
        .frame(minWidth: 96, alignment: .leading)
    }
}

private struct Sparkline: View {
    let values: [Double]
    let color: Color
    let label: String

    var body: some View {
        ZStack(alignment: .topLeading) {
            GeometryReader { geo in
                if values.count > 1, let lo = values.min(), let hi = values.max() {
                    let span = max(hi - lo, 1)
                    Path { p in
                        for (i, v) in values.enumerated() {
                            let pt = CGPoint(x: geo.size.width * CGFloat(i) / CGFloat(max(values.count - 1, 1)),
                                             y: geo.size.height * (1 - CGFloat((v - lo) / span)) * 0.8 + geo.size.height * 0.1)
                            i == 0 ? p.move(to: pt) : p.addLine(to: pt)
                        }
                    }
                    .stroke(color, lineWidth: 1.5)
                }
            }
            Text(label).font(.system(size: 8)).foregroundStyle(.secondary)
        }
        .padding(4)
        .background(RoundedRectangle(cornerRadius: 5).fill(Color.primary.opacity(0.05)))
    }
}

// The driver's tweaks, kept in this app's preferences where the Metal driver reads them when a game sets up its window
// (tools/metal/native/RDNA4MTLDevice.mm, "Super Resolution"): they apply to games started after a change.
struct TweaksView: View {
    @AppStorage("SuperResolution") private var superResolution = false
    @AppStorage("RenderScale") private var renderScale = 100     // percent of the size a game asks for; 100: its own size
    @AppStorage("Sharpness") private var sharpness = 200         // RCAS, thousandths of a stop below the sharpest
    @AppStorage("AntiLag") private var antiLag = false          // the Metal driver keeps the CPU at most a frame ahead
    @AppStorage("AntiLag2") private var antiLag2 = false        // Mesa's Anti-Lag layer loaded into Vulkan games

    // Anti-Lag 2: Mesa's layer is an implicit one loaded while ENABLE_LAYER_MESA_ANTI_LAG=1, set for the login session's apps
    static func applyAntiLag2(_ on: Bool) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        p.arguments = on ? ["setenv", "ENABLE_LAYER_MESA_ANTI_LAG", "1"] : ["unsetenv", "ENABLE_LAYER_MESA_ANTI_LAG"]
        try? p.run()
    }

    // FSR 1's quality modes (AMD's names and scales)
    private static let modes: [(String, Int)] = [("Off: the game's own size", 100), ("Ultra Quality (77 %)", 77), ("Quality (67 %)", 67),
                                                 ("Balanced (59 %)", 59), ("Performance (50 %)", 50)]

    // shown as 0-100 % (Adrenalin's way), kept as stops below the sharpest: 100 % is 0, 0 % is 2 stops
    private var sharpnessPercent: Int { max(0, 2000 - sharpness) / 20 }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Toggle("Super Resolution (AMD FSR 1)", isOn: $superResolution).toggleStyle(.switch)
            Group {
                Picker("Render scale", selection: $renderScale) {
                    ForEach(Self.modes, id: \.1) { Text($0.0).tag($0.1) }
                }
                HStack {
                    Text("Sharpness")
                    Slider(value: Binding(get: { Double(sharpnessPercent) }, set: { sharpness = 2000 - Int($0.rounded()) * 20 }), in: 0...100)
                    Text("\(sharpnessPercent) %").monospacedDigit().frame(width: 40, alignment: .trailing)
                }
            }
            .disabled(!superResolution)
            Text("Games drawing below the screen's resolution are upscaled with FSR 1 instead of stretched; a render scale makes them draw fewer pixels for more frames (turn it off for a game that then draws the wrong size).")
                .font(.caption2).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Divider()
            Toggle("Anti-Lag", isOn: $antiLag).toggleStyle(.switch)
            Toggle("Anti-Lag 2 (Vulkan games that support it)", isOn: Binding(get: { antiLag2 }, set: { antiLag2 = $0; Self.applyAntiLag2($0) }))
                .toggleStyle(.switch)
            Text("Anti-Lag keeps a game's CPU at most one frame ahead of the card: frames show sooner after input. Anti-Lag 2 is for Vulkan games that build it in (VK_AMD_anti_lag).")
                .font(.caption2).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Text("All of these apply to games started after a change.").font(.caption2).foregroundStyle(.secondary)
        }
    }
}
