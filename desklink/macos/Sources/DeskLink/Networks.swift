import AppKit
import CoreLocation
import CoreWLAN
import Foundation

/// The Wi-Fi networks this Mac is on, by SSID, so Desk Link can share on some only.
/// Since macOS 14 a Wi-Fi network's name is given only to an app allowed Location
/// Services; until it is, `here` is nil, nothing can tell.
@MainActor
final class Networks: NSObject, CLLocationManagerDelegate {
    // CoreWLAN's change events stop coming after a while on some Macs; asked
    // this often instead, a new network is felt well before the next heartbeat.
    private static let lookEvery: TimeInterval = 5

    private(set) var here: Set<String>?
    var onChange: ((Set<String>?) -> Void)?
    private let location = CLLocationManager()
    private var timer: Timer?
    private var emptyOnce = false

    override init() {
        super.init()
        location.delegate = self
        here = read()
        timer = Timer.scheduledTimer(withTimeInterval: Self.lookEvery, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.look() }
        }
    }

    var allowed: Bool {
        switch location.authorizationStatus {
        case .notDetermined, .denied, .restricted: return false
        default: return true
        }
    }

    /// Opens macOS's own question; denied before, it opens where it is changed instead.
    func askToAllow() {
        if location.authorizationStatus == .notDetermined {
            location.requestWhenInUseAuthorization()
        } else if let settings = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_LocationServices") {
            NSWorkspace.shared.open(settings)
        }
    }

    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        Task { @MainActor in self.look() }
    }

    private func look() {
        let now = read()
        // Roaming between access points, the SSID is gone for a moment: only a second look counts.
        let briefly = now?.isEmpty == true && here?.isEmpty == false && !emptyOnce
        emptyOnce = briefly
        guard now != here, !briefly else { return }
        here = now
        onChange?(now)
    }

    private func read() -> Set<String>? {
        guard allowed else { return nil }
        let interfaces = CWWiFiClient.shared().interfaces() ?? []
        return Set(interfaces.compactMap { $0.ssid() }.filter { !$0.isEmpty })
    }
}
