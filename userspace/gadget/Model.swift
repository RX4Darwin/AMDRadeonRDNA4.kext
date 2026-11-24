// Model.swift: the state the menu bar item and its popover show, refreshed every two seconds; the release check every six hours.

import Foundation
import SwiftUI
import UserNotifications
import ServiceManagement

@MainActor
final class GadgetModel: ObservableObject {
    @Published var gpus: [GPUReading] = []
    @Published var history: [Double] = []           // the first GPU's temperature, the last two minutes
    @Published var powerHistory: [Double] = []
    @Published var installed: String?
    @Published var build: String?
    @Published var releases: [Release] = []
    @Published var checkError: String?
    @Published var tuning: Tuning?                  // nil: the driver's settings are not there (not running, or older than 0.5.0)
    @Published var edited = Tuning()
    @Published var busy: String?                    // what is running (download, install, ...)
    @Published var log = ""
    @AppStorage("showPower") var showPower = true
    @AppStorage("includePrereleases") var includePrereleases = false
    @AppStorage("notifiedTag") var notifiedTag = ""

    private var timer: Timer?
    private var lastCheck = Date.distantPast

    init() {
        refreshDriver()
        sample()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
        Task { await checkReleases() }
    }

    private func tick() {
        sample()
        if Date().timeIntervalSince(lastCheck) > 6 * 3600 {
            Task { await checkReleases() }
        }
    }

    func sample() {
        gpus = Sensors.read()
        if let t = gpus.first?.temperature {
            history = Array((history + [t]).suffix(60))
        }
        if let p = gpus.first?.powerW {
            powerHistory = Array((powerHistory + [p]).suffix(60))
        }
    }

    func refreshDriver() {
        installed = Driver.installedVersion()
        build = Driver.build()
        tuning = Tuning.current()
        if let t = tuning { edited = t }
    }

    /// The newest release (pre-releases if asked for), and whether it is newer than what is installed.
    var latest: Release? { releases.first { includePrereleases || !$0.prerelease } }
    var updateAvailable: Bool {
        guard let l = latest else { return false }
        guard let i = installed else { return true }
        return Driver.older(i, than: l.version)
    }

    func checkReleases() async {
        lastCheck = Date()
        do {
            releases = try await Driver.releases()
            checkError = nil
        } catch {
            checkError = error.localizedDescription
            return
        }
        if updateAvailable, let l = latest, l.tag_name != notifiedTag {
            notifiedTag = l.tag_name
            let c = UNMutableNotificationContent()
            c.title = "AMDRadeonRDNA4 \(l.version) is out"
            c.body = installed.map { "You have \($0). Open RDNA4 Gadget to update." } ?? "Open RDNA4 Gadget to install it."
            try? await UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: l.tag_name, content: c, trigger: nil))
        }
    }

    func install(_ release: Release) {
        guard busy == nil else { return }
        busy = "Downloading \(release.tag_name)…"
        Task {
            do {
                let dir = try await Driver.fetch(release)
                busy = "Installing \(release.tag_name)…"
                let dry = ProcessInfo.processInfo.environment["RDNA4GADGET_DRYRUN"] == "1"
                log = try Driver.install(from: dir, dryRun: dry) + (dry ? "\n(dry run: nothing written)" : "\nRestart to use \(release.version).")
            } catch {
                log = "Not installed: \(error.localizedDescription)"
            }
            busy = nil
            refreshDriver()
        }
    }

    func uninstall() {
        guard busy == nil else { return }
        busy = "Removing the driver…"
        do {
            log = try Driver.uninstall() + "\nRestart to finish."
        } catch {
            log = "Not removed: \(error.localizedDescription)"
        }
        busy = nil
        refreshDriver()
    }

    func applyTuning() {
        do {
            try edited.apply()
            log = "Settings saved: they apply at the next restart."
        } catch {
            log = "Settings not saved: \(error.localizedDescription)"
        }
        refreshDriver()
    }

    func restart() {
        NSAppleScript(source: "tell application \"System Events\" to restart")?.executeAndReturnError(nil)
    }

    var launchAtLogin: Bool {
        get { SMAppService.mainApp.status == .enabled }
        set {
            try? newValue ? SMAppService.mainApp.register() : SMAppService.mainApp.unregister()
            objectWillChange.send()
        }
    }
}
