import AppKit
import Foundation
import ServiceManagement

/// What this Mac shares with the desk panel, and the panel's commands back.
@MainActor
final class Link: ObservableObject {
    enum Reach { case unknown, answering, refused, unreachable }

    @Published private(set) var track: Track?
    @Published private(set) var reach = Reach.unknown
    @Published var sharing: Bool {
        didSet {
            defaults.set(sharing, forKey: "sharing")
            sharing ? begin() : end()
        }
    }
    @Published var panel: String {
        didSet { defaults.set(panel, forKey: "panel") }
    }
    @Published var key: String {
        didSet { Keychain.write(key) }
    }
    @Published var opensAtLogin: Bool {
        didSet {
            do {
                if opensAtLogin { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
            } catch {
                NSLog("Desk Link: open at login: \(error)")
            }
        }
    }

    private static let port: UInt16 = 47801
    // The panel forgets a laptop it has not heard from in 30 seconds.
    private static let heartbeat: TimeInterval = 10

    private let defaults = UserDefaults.standard
    private let source: NowPlayingSource
    private let output = SystemVolume()
    private var server: CommandServer!
    private var artwork: Artwork?
    private var beat: Timer?
    private var awake: NSObjectProtocol?
    private var pendingReport: DispatchWorkItem?
    private let machine = Host.current().localizedName ?? ProcessInfo.processInfo.hostName

    init() {
        defaults.register(defaults: ["sharing": true, "panel": "smart-flexispot"])
        sharing = defaults.bool(forKey: "sharing")
        panel = defaults.string(forKey: "panel") ?? "smart-flexispot"
        // A key handed over with `defaults write nl.w-tb.desklink key ...` is
        // moved into the keychain at once.
        if let given = defaults.string(forKey: "key"), !given.isEmpty {
            Keychain.write(given)
            defaults.removeObject(forKey: "key")
        }
        key = Keychain.read()
        opensAtLogin = SMAppService.mainApp.status == .enabled
        source = NowPlayingSource(resources: Bundle.main.resourceURL!)
        server = CommandServer(port: Self.port) { [weak self] request in
            DispatchQueue.main.sync { MainActor.assumeIsolated { self?.handle(request) ?? .notFound } }
        }
        source.onChange = { [weak self] track in self?.take(track) }
        output.onChange = { [weak self] in
            if self?.track != nil { self?.reportSoon() }
        }
        NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil,
                                               queue: .main) { [source] _ in source.stopNow() }
        source.clearLeftovers()
        if sharing { begin() }
    }

    private func begin() {
        // App Nap holds a menu bar app's timers back by minutes, past the
        // panel's patience, which then takes the Mac for gone.
        awake = ProcessInfo.processInfo.beginActivity(options: .userInitiatedAllowingIdleSystemSleep,
                                                      reason: "Telling the desk panel what plays")
        source.start()
        server.start()
        beat = Timer.scheduledTimer(withTimeInterval: Self.heartbeat, repeats: true) { [weak self] _ in
            Task { @MainActor in
                if self?.track != nil { self?.report() }
            }
        }
    }

    private func end() {
        beat?.invalidate()
        beat = nil
        if let awake { ProcessInfo.processInfo.endActivity(awake) }
        awake = nil
        source.stop()
        server.stop()
        track = nil
        artwork = nil
        report()  // nothing playing, so the panel lets go at once
        reach = .unknown
    }

    private func take(_ new: Track?) {
        guard sharing else { return }
        if new?.artwork != track?.artwork {
            artwork = new?.artwork.flatMap(Artwork.init)
        }
        track = new
        reportSoon()
    }

    // The player's changes come in bursts, as a volume key held down does:
    // one report for the lot.
    private func reportSoon() {
        pendingReport?.cancel()
        let soon = DispatchWorkItem { [weak self] in self?.report() }
        pendingReport = soon
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3, execute: soon)
    }

    private func report() {
        guard !panel.isEmpty, !key.isEmpty, let url = URL(string: "http://\(panel)/laptop") else { return }
        var body: [String: Any] = ["machine": machine, "port": Int(Self.port), "title": ""]
        if let track {
            var takes = ["pause"]
            if track.duration > 0 { takes.append("seek") }
            if track.skips { takes += ["next", "previous"] }
            if let percent = output.percent {
                takes.append("volume")
                body["volume"] = percent
                body["muted"] = output.muted
            }
            body["app"] = track.app
            body["title"] = track.title
            body["artist"] = track.artist
            body["playing"] = track.playing
            body["position"] = track.position()
            body["duration"] = track.duration
            body["art"] = artwork?.version ?? ""
            body["takes"] = takes
        }
        var request = URLRequest(url: url, timeoutInterval: 3)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue(key, forHTTPHeaderField: "X-Laptop-Key")
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)
        URLSession.shared.dataTask(with: request) { [weak self] _, response, _ in
            let status = (response as? HTTPURLResponse)?.statusCode
            Task { @MainActor in
                guard let self, self.sharing else { return }
                self.reach = status == nil ? .unreachable : status == 403 ? .refused : .answering
            }
        }.resume()
    }

    private func handle(_ request: Request) -> Response {
        if request.method == "GET", request.path.hasPrefix("/art.jpg"), let artwork {
            return Response(status: 200, type: "image/jpeg", body: artwork.jpeg)
        }
        guard request.method == "POST", request.path == "/command" else { return .notFound }
        guard !key.isEmpty, request.headers["x-laptop-key"] == key else { return Response(status: 403) }
        guard let command = try? JSONSerialization.jsonObject(with: request.body) as? [String: Any],
              let name = command["command"] as? String else { return Response(status: 400) }
        switch name {
        case "play": source.send(.play)
        case "pause": source.send(.pause)
        case "next": source.send(.next)
        case "previous": source.send(.previous)
        case "seek":
            guard let position = (command["position"] as? NSNumber)?.doubleValue else { return Response(status: 400) }
            source.seek(to: position)
        case "volume":
            guard let level = (command["level"] as? NSNumber)?.intValue else { return Response(status: 400) }
            output.set(percent: level)
        case "mute":
            guard let muted = command["muted"] as? Bool else { return Response(status: 400) }
            output.set(muted: muted)
        default:
            return Response(status: 400)
        }
        return .noContent
    }
}
