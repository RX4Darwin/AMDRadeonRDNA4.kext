// RDNA4 Gadget: the card's temperature and power in the menu bar, and in its popover the driver: versions, updates and removal.
// The menu bar item is drawn as RadeonGadget and AMD Power Gadget draw theirs: a title, then small stacked labels beside their values.

import AppKit
import SwiftUI
import Combine

final class StatusItem: NSObject {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let popover = NSPopover()
    private let model: GadgetModel
    private var watch: AnyCancellable?

    @MainActor init(model: GadgetModel) {
        self.model = model
        super.init()
        popover.behavior = .transient
        // The size is set here, not taken from SwiftUI: a popover given its size after it is shown grows upward, off the screen.
        let host = NSHostingController(rootView: GadgetView(model: model))
        host.sizingOptions = []
        popover.contentViewController = host
        popover.contentSize = GadgetView.size
        item.button?.target = self
        item.button?.action = #selector(toggle)
        watch = model.objectWillChange.sink { [weak self] _ in
            DispatchQueue.main.async { self?.redraw() }
        }
        redraw()
    }

    @MainActor @objc private func toggle() {
        if popover.isShown {
            popover.performClose(nil)
        } else if let b = item.button {
            model.refreshDriver()
            popover.show(relativeTo: b.bounds, of: b, preferredEdge: .minY)
            popover.contentViewController?.view.window?.makeKey()
        }
    }

    @MainActor private func redraw() {
        let g = model.gpus.first
        var fields: [(String, String, String)] = [("TMP", g?.temperature.map { String(format: "%.0f°", $0) } ?? "–", "188°")]
        if model.showPower {
            fields.append(("PWR", g?.powerW.map { String(format: "%.0fW", $0) } ?? "–", "388W"))
        }
        item.button?.image = StatusItem.image(title: "GPU", fields: fields)
    }

    /// "GPU" in the menu bar's font, then each field: its label in three stacked letters, its value right-aligned in a slot as wide
    /// as the field's widest reading (the third string), so the item, and the popover anchored to it, keep still as values change.
    /// A template image: the menu bar colours it for light and dark.
    static func image(title: String, fields: [(String, String, String)]) -> NSImage {
        let height: CGFloat = 22
        let titleAttr: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 13, weight: .regular), .foregroundColor: NSColor.black]
        let valueAttr: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedDigitSystemFont(ofSize: 13, weight: .medium), .foregroundColor: NSColor.black]
        let letterAttr: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedSystemFont(ofSize: 6.5, weight: .bold), .foregroundColor: NSColor.black]
        let t = NSAttributedString(string: title, attributes: titleAttr)
        let slots = fields.map { ceil(NSAttributedString(string: $0.2, attributes: valueAttr).size().width) }
        let width = ceil(t.size().width) + 6 + slots.reduce(0) { $0 + 9 + $1 + 6 }
        let image = NSImage(size: NSSize(width: width, height: height), flipped: false) { _ in
            t.draw(at: NSPoint(x: 0, y: (height - t.size().height) / 2))
            var x = ceil(t.size().width) + 6
            for (i, field) in fields.enumerated() {
                for (k, ch) in field.0.prefix(3).enumerated() {   // the label's letters top to bottom
                    let s = NSAttributedString(string: String(ch), attributes: letterAttr)
                    s.draw(at: NSPoint(x: x + (6 - s.size().width) / 2, y: height - 8 - CGFloat(k) * 6.2))
                }
                let v = NSAttributedString(string: field.1, attributes: valueAttr)
                v.draw(at: NSPoint(x: x + 9 + slots[i] - v.size().width, y: (height - v.size().height) / 2))
                x += 9 + slots[i] + 6
            }
            return true
        }
        image.isTemplate = true
        return image
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var status: StatusItem?

    @MainActor func applicationDidFinishLaunching(_ notification: Notification) {
        status = StatusItem(model: GadgetModel())
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
