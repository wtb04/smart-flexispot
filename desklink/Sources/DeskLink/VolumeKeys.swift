import AppKit
import ApplicationServices

/// The volume keys, pressed for the panel when the output takes no level from
/// macOS, as a display does: an app such as MonitorControl takes the keys and
/// sets the display's own volume. Without one running, they would do nothing.
enum VolumeKeys {
    private static let takers: Set<String> = [
        "app.monitorcontrol.MonitorControl", "pro.betterdisplay.BetterDisplay", "fyi.lunar.Lunar",
    ]

    /// An app that turns the keys into a display's volume is running.
    static var taken: Bool {
        NSWorkspace.shared.runningApplications.contains { takers.contains($0.bundleIdentifier ?? "") }
    }

    /// Pressing keys for the user takes Accessibility, which macOS asks for once.
    static var allowed: Bool { AXIsProcessTrusted() }

    static func askToAllow() {
        let prompt = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        AXIsProcessTrustedWithOptions([prompt: true] as CFDictionary)
    }

    static func press(up: Bool) {
        let key: Int32 = up ? 0 : 1  // NX_KEYTYPE_SOUND_UP, NX_KEYTYPE_SOUND_DOWN
        for down in [true, false] {
            let event = NSEvent.otherEvent(
                with: .systemDefined, location: .zero,
                modifierFlags: NSEvent.ModifierFlags(rawValue: down ? 0xa00 : 0xb00),
                timestamp: 0, windowNumber: 0, context: nil, subtype: 8,
                data1: Int((key << 16) | ((down ? 0xa : 0xb) << 8)), data2: -1)
            event?.cgEvent?.post(tap: .cghidEventTap)
        }
    }
}
