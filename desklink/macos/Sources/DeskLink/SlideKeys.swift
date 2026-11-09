import CoreGraphics

/// The panel's clicker, as a presenter's clicker sends it: Page Down and Page
/// Up, which Keynote, PowerPoint, Google Slides and a PDF all take for the
/// next slide and the one before. Pressing keys takes Accessibility, as the
/// volume keys do.
enum SlideKeys {
    static func press(next: Bool) {
        press(next ? 0x79 : 0x74)  // kVK_PageDown, kVK_PageUp
    }

    static func press(_ key: CGKeyCode) {
        let source = CGEventSource(stateID: .hidSystemState)
        for down in [true, false] {
            CGEvent(keyboardEventSource: source, virtualKey: key, keyDown: down)?.post(tap: .cghidEventTap)
        }
    }
}
