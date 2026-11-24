// Driver.swift: what of AMDRadeonRDNA4 is installed and running, the releases on GitHub, and installing, updating and removing it.

import Foundation
import IOKit
import IOKit.kext

struct Release: Decodable {
    struct Asset: Decodable {
        let name: String
        let browser_download_url: URL
    }
    let tag_name: String
    let name: String?
    let html_url: URL
    let prerelease: Bool
    let assets: [Asset]

    var version: String { tag_name.hasPrefix("v") ? String(tag_name.dropFirst()) : tag_name }
    var releaseZip: Asset? { assets.first { $0.name.hasSuffix("-RELEASE.zip") } }
}

enum Driver {
    static let repository = "RX4Darwin/AMDRadeonRDNA4.kext"
    static let kextID = "com.hackintosh.AMDRadeonRDNA4"
    static let bundlePath = "/Library/GPUBundles/AMDRadeonRDNA4MTLDriver.bundle"
    static let lilu = "4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102"

    /// The loaded kext's version, nil if it is not loaded.
    static func kextVersion() -> String? {
        guard let info = KextManagerCopyLoadedKextInfo([kextID] as CFArray, nil)?.takeRetainedValue() as? [String: Any],
              let mine = info[kextID] as? [String: Any] else { return nil }
        return mine["CFBundleVersion"] as? String
    }

    /// The running driver's build ("0.5.0 (v0.5.0-...)"), as it publishes it in the registry.
    static func build() -> String? {
        var iter: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOPCIDevice"), &iter) == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(iter) }
        while case let e = IOIteratorNext(iter), e != 0 {
            defer { IOObjectRelease(e) }
            let opts = IOOptionBits(kIORegistryIterateRecursively)
            if let v = IORegistryEntrySearchCFProperty(e, kIOServicePlane, "AMDRadeonRDNA4,Build" as CFString, kCFAllocatorDefault, opts) as? String {
                return v
            }
        }
        return nil
    }

    /// The Metal driver bundle's version, nil if it is not installed.
    static func bundleVersion() -> String? {
        (NSDictionary(contentsOfFile: bundlePath + "/Contents/Info.plist")?["CFBundleShortVersionString"] as? String)
    }

    /// The installed version: the running kext's, else the bundle's.
    static func installedVersion() -> String? { kextVersion() ?? bundleVersion() }

    static func releases() async throws -> [Release] {
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(repository)/releases?per_page=20")!)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.setValue("RDNA4Gadget", forHTTPHeaderField: "User-Agent")
        let (data, response) = try await URLSession.shared.data(for: req)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw GadgetError("GitHub answered \((response as? HTTPURLResponse)?.statusCode ?? 0)") }
        return try JSONDecoder().decode([Release].self, from: data)
    }

    /// Whether version a is older than b ("0.4.0" < "0.5.0"; a pre-release tag's suffix after '-' is ignored).
    static func older(_ a: String, than b: String) -> Bool {
        func parts(_ s: String) -> [Int] { s.split(separator: "-")[0].split(separator: ".").map { Int($0) ?? 0 } }
        let x = parts(a), y = parts(b)
        for i in 0..<max(x.count, y.count) where (i < x.count ? x[i] : 0) != (i < y.count ? y[i] : 0) {
            return (i < x.count ? x[i] : 0) < (i < y.count ? y[i] : 0)
        }
        return false
    }

    // MARK: install, update, remove

    /// The EFI partition OpenCore booted from (its boot-path: "...HD(n,GPT,<uuid>,...)"), as a device node; nil if it cannot be told.
    static func bootEFI() -> String? {
        guard let path = nvram("\(lilu):boot-path"),
              let r = path.range(of: #"GPT,([0-9A-Fa-f-]{36})"#, options: .regularExpression) else { return nil }
        let uuid = String(path[r].dropFirst(4))
        guard let plist = run("/usr/sbin/diskutil", ["info", "-plist", uuid]),
              let info = try? PropertyListSerialization.propertyList(from: plist, format: nil) as? [String: Any] else { return nil }
        return info["DeviceNode"] as? String
    }

    static func nvram(_ name: String) -> String? {
        guard let out = run("/usr/sbin/nvram", [name]), let s = String(data: out, encoding: .utf8),
              let tab = s.firstIndex(of: "\t") else { return nil }
        return String(s[s.index(after: tab)...]).trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// Downloads the release's RELEASE zip and unpacks it; the folder.
    static func fetch(_ release: Release) async throws -> URL {
        guard let asset = release.releaseZip else { throw GadgetError("\(release.tag_name) has no RELEASE zip") }
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RDNA4Gadget/\(release.tag_name)", isDirectory: true)
        try? FileManager.default.removeItem(at: dir)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let (file, _) = try await URLSession.shared.download(from: asset.browser_download_url)
        let zip = dir.appendingPathComponent(asset.name)
        try FileManager.default.moveItem(at: file, to: zip)
        guard run("/usr/bin/ditto", ["-x", "-k", zip.path, dir.path]) != nil else { throw GadgetError("could not unpack \(asset.name)") }
        return dir
    }

    /// The release's kext, package and efi.sh in an unpacked folder.
    static func parts(in dir: URL) throws -> (kext: String, pkg: String, efi: String) {
        var kext: String?, pkg: String?, efi: String?
        for case let url as URL in FileManager.default.enumerator(at: dir, includingPropertiesForKeys: nil) ?? FileManager.DirectoryEnumerator() {
            switch url.lastPathComponent {
            case "AMDRadeonRDNA4.kext": kext = kext ?? url.path
            case "efi.sh": efi = efi ?? url.path
            case let n where n.hasSuffix(".pkg"): pkg = pkg ?? url.path
            default: break
            }
        }
        guard let k = kext, let p = pkg, let e = efi else { throw GadgetError("the release is missing its kext, package or efi.sh") }
        return (k, p, e)
    }

    /// Installs or updates: the package (Metal driver bundle, helpers) and the kext into the EFI OpenCore booted from, with efi.sh's
    /// backup as the way back. Asks for an administrator's password once.
    static func install(from dir: URL, dryRun: Bool = false) throws -> String {
        let p = try parts(in: dir)
        guard let disk = bootEFI() else { throw GadgetError("the EFI OpenCore booted from was not found (no boot-path in NVRAM)") }
        return try Admin.run(installScript(p, disk: disk, dryRun: dryRun))
    }

    static func installScript(_ p: (kext: String, pkg: String, efi: String), disk: String, dryRun: Bool) -> String {
        let mount = "/Volumes/RDNA4EFI"
        return efiScript(disk: disk, mount: mount, body: [
            dryRun ? "/bin/sh \(q(p.efi)) install --efi \(q(mount)) --kext \(q(p.kext)) --dry-run"
                   : "/bin/sh \(q(p.efi)) install --efi \(q(mount)) --kext \(q(p.kext)) --pkg \(q(p.pkg))",
        ])
    }

    /// Removes it: the user space (the bundle's uninstall.sh) and the kext, the config.plist back as it was before the last install.
    static func uninstall() throws -> String {
        guard let disk = bootEFI() else { throw GadgetError("the EFI OpenCore booted from was not found (no boot-path in NVRAM)") }
        let res = bundlePath + "/Contents/Resources"
        guard FileManager.default.fileExists(atPath: res + "/efi.sh") else {
            throw GadgetError("the installed driver bundle has no efi.sh (installed without its package, or older than 0.4.0)")
        }
        let tmp = NSTemporaryDirectory() + "rdna4-efi.sh"   // (uninstall.sh removes the bundle it is in)
        let mount = "/Volumes/RDNA4EFI"
        let script = "/bin/cp \(q(res + "/efi.sh")) \(q(tmp)) && " + efiScript(disk: disk, mount: mount, body: [
            "/bin/sh \(q(tmp)) remove --efi \(q(mount))",
            "/bin/sh \(q(res + "/uninstall.sh"))",
        ])
        return try Admin.run(script)
    }

    /// Mounts the EFI at `mount` (unless it is mounted already), runs `body`, unmounts what it mounted.
    private static func efiScript(disk: String, mount: String, body: [String]) -> String {
        """
        set -e
        m=$(/usr/sbin/diskutil info -plist \(q(disk)) | /usr/bin/plutil -extract MountPoint raw -o - - 2>/dev/null || true)
        own=0
        if [ -z "$m" ]; then /bin/mkdir -p \(q(mount)); /sbin/mount -t msdos \(q(disk)) \(q(mount)); m=\(q(mount)); own=1; fi
        um=\(q(mount))
        trap '[ $own = 1 ] && /usr/sbin/diskutil unmount "$um" >/dev/null 2>&1' EXIT
        \(body.map { $0.replacingOccurrences(of: q(mount), with: "\"$m\"") }.joined(separator: "\n"))
        """
    }

    static func q(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

    @discardableResult
    static func run(_ tool: String, _ args: [String]) -> Data? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return nil }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return p.terminationStatus == 0 ? data : nil
    }
}

struct GadgetError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

/// A shell script as root, through macOS's administrator password prompt; its output, or the error it printed.
enum Admin {
    static func run(_ script: String) throws -> String {
        let escaped = script.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
        var error: NSDictionary?
        let result = NSAppleScript(source: "do shell script \"\(escaped)\" with administrator privileges")?.executeAndReturnError(&error)
        if let error {
            throw GadgetError((error[NSAppleScript.errorMessage] as? String) ?? "the command failed")
        }
        return result?.stringValue ?? ""
    }
}
