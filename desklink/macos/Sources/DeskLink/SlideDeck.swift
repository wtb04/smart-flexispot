import AppKit
import ApplicationServices

/// Which slide the clicker's keys move, for the panel to show: the page of the
/// Okular or Preview window in front, which is the one last touched, read
/// through Accessibility, which the keys take anyway. Preview says it in its
/// window's title, "talk.pdf – Page 4 of 12", which a slideshow goes on
/// keeping. Okular on the Mac is built without D-Bus and says it only in its
/// toolbar's page box; a presentation shows Accessibility only its own window,
/// with no page in it, so the box is held from before it began and goes on
/// following it.
@MainActor
final class SlideDeck {
    struct Slide: Equatable {
        var page: Int
        var pages: Int
        var deck: String
        var presenting: Bool  // in Okular's presentation or Preview's slideshow
    }

    private struct Box {
        let window: AXUIElement
        let page: AXUIElement   // a text field, "4"
        let pages: AXUIElement  // the button after "of", titled "12"
    }

    private static let okular = "org.kde.okular"
    private static let preview = "com.apple.Preview"
    private static let presenting = " – Presentation"
    // "talk.pdf – Page 4 of 12", in whatever language: a name, a dash, two numbers
    private static let titled = #/^(.+?) [–—-] \D*?(\d+)\D+?(\d+)\s*$/#
    private var held: [pid_t: [Box]] = [:]

    /// Nil when neither is in front, or its page cannot be read.
    func read() -> Slide? {
        guard let app = NSWorkspace.shared.frontmostApplication, AXIsProcessTrusted() else { return nil }
        switch app.bundleIdentifier {
        case Self.okular: return okular(app.processIdentifier)
        case Self.preview: return Self.preview(app.processIdentifier)
        default: return nil
        }
    }

    /// The view's full screen in or out: Escape to leave it, the viewer's own
    /// menu item to go in, which needs it in front, as it is when read.
    func show() -> Bool {
        guard let app = NSWorkspace.shared.frontmostApplication, let slide = read() else { return false }
        if slide.presenting {
            SlideKeys.press(0x35)  // kVK_Escape
            return true
        }
        let names = app.bundleIdentifier == Self.okular ? ["Presentation", "Presentatie"] : ["Slideshow", "Diavoorstelling"]
        let root = AXUIElementCreateApplication(app.processIdentifier)
        guard let bar = Self.element(root, kAXMenuBarAttribute),
              let item = Self.find(in: bar, { names.contains(Self.text($0, kAXTitleAttribute) ?? "") }) else { return false }
        return AXUIElementPerformAction(item, kAXPressAction as CFString) == .success
    }

    /// The focused window's title, or in a slideshow, whose own window has
    /// none, the frontmost document's.
    private static func preview(_ pid: pid_t) -> Slide? {
        let root = AXUIElementCreateApplication(pid)
        let focused = element(root, kAXFocusedWindowAttribute)
        var windows = children(root, kAXWindowsAttribute)
        if let focused { windows.insert(focused, at: 0) }
        for window in windows {
            guard let match = (text(window, kAXTitleAttribute) ?? "").firstMatch(of: titled),
                  let page = Int(match.2), let pages = Int(match.3), pages > 0, (1...pages).contains(page)
            else { continue }
            let slideshow = focused.map { (text($0, kAXTitleAttribute) ?? "").isEmpty } ?? false
            return Slide(page: page, pages: pages, deck: String(match.1), presenting: slideshow)
        }
        return nil
    }

    private func okular(_ pid: pid_t) -> Slide? {
        held = held.filter { NSRunningApplication(processIdentifier: $0.key) != nil }
        var boxes = (held[pid] ?? []).filter { Self.text($0.page, kAXValueAttribute) != nil }  // not closed since
        defer { held[pid] = boxes }
        guard let window = Self.element(AXUIElementCreateApplication(pid), kAXFocusedWindowAttribute) else { return nil }
        let title = Self.text(window, kAXTitleAttribute) ?? ""
        var box = boxes.first { CFEqual($0.window, window) }
        if box == nil, let found = Self.box(in: window) {
            boxes.append(found)
            box = found
        }
        if box == nil, title.hasSuffix(Self.presenting) {
            let deck = Self.deck(of: title)
            box = boxes.first { Self.deck(of: Self.text($0.window, kAXTitleAttribute) ?? "") == deck }
                ?? (boxes.count == 1 ? boxes[0] : nil)
        }
        guard let box, let page = Int(Self.text(box.page, kAXValueAttribute) ?? ""),
              let pages = Int(Self.text(box.pages, kAXTitleAttribute) ?? ""), pages > 0, (1...pages).contains(page)
        else { return nil }
        return Slide(page: page, pages: pages, deck: Self.deck(of: Self.text(box.window, kAXTitleAttribute) ?? title),
                     presenting: title.hasSuffix(Self.presenting))
    }

    /// "talk.pdf – Presentation" is "talk.pdf".
    private static func deck(of title: String) -> String {
        var name = title.trimmingCharacters(in: .whitespaces)
        if name.hasSuffix(presenting) { name.removeLast(presenting.count) }
        return name.trimmingCharacters(in: .whitespaces)
    }

    /// The toolbar's page, "of" and count, in whatever language: a text field
    /// holding a number, a label, and a button titled with a number.
    private static func box(in window: AXUIElement) -> Box? {
        var queue = [window]
        while !queue.isEmpty {
            let row = children(queue.removeFirst())
            for i in row.indices.dropLast(2)
            where text(row[i], kAXRoleAttribute) == "AXTextField" && text(row[i + 1], kAXRoleAttribute) == "AXStaticText" {
                if Int(text(row[i], kAXValueAttribute) ?? "") != nil, Int(text(row[i + 2], kAXTitleAttribute) ?? "") != nil {
                    return Box(window: window, page: row[i], pages: row[i + 2])
                }
            }
            queue += row
        }
        return nil
    }

    private static func find(in e: AXUIElement, _ wanted: (AXUIElement) -> Bool) -> AXUIElement? {
        if wanted(e) { return e }
        for child in children(e) {
            if let found = find(in: child, wanted) { return found }
        }
        return nil
    }

    private static func element(_ of: AXUIElement, _ attribute: String) -> AXUIElement? {
        var value: AnyObject?
        guard AXUIElementCopyAttributeValue(of, attribute as CFString, &value) == .success, let value,
              CFGetTypeID(value) == AXUIElementGetTypeID() else { return nil }
        return (value as! AXUIElement)
    }

    private static func text(_ of: AXUIElement, _ attribute: String) -> String? {
        var value: AnyObject?
        guard AXUIElementCopyAttributeValue(of, attribute as CFString, &value) == .success else { return nil }
        return value as? String
    }

    private static func children(_ of: AXUIElement, _ attribute: String = kAXChildrenAttribute) -> [AXUIElement] {
        var value: AnyObject?
        guard AXUIElementCopyAttributeValue(of, attribute as CFString, &value) == .success else { return [] }
        return value as? [AXUIElement] ?? []
    }
}
