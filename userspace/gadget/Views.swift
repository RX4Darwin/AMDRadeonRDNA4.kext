// Views.swift: the popover: the card's readings and the driver (versions, update, removal).

import SwiftUI

struct GadgetView: View {
    static let size = NSSize(width: 360, height: 460)
    @ObservedObject var model: GadgetModel
    @State private var tab = 0
    @State private var confirmUninstall = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            readings
            Picker("", selection: $tab) {
                Text("Driver").tag(0)
                Text("Gadget").tag(1)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            switch tab {
            case 0: driver
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
                    Reading(label: "VRAM", value: g.vramUsed.map { used in
                        g.vramTotal.map { String(format: "%.1f / %.0f GB", used / 1e9, $0 / 1e9) } ?? String(format: "%.1f GB", used / 1e9)
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
