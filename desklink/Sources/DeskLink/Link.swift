import AppKit
import Foundation
import OSLog
import ServiceManagement

private let log = Logger(subsystem: "nl.w-tb.desklink", category: "panel")

/// What this Mac shares with the desk panel, and the panel's commands back,
/// every message sealed with the key the two share.
@MainActor
final class Link: ObservableObject {
    enum Reach { case unknown, answering, refused, unreachable }

    @Published private(set) var track: Track?
    @Published private(set) var cover: NSImage?
    @Published private(set) var heard: Date?
    @Published private(set) var reach = Reach.unknown
    @Published private(set) var reachesBack: Bool?  // the panel's commands get here, as it last said
    @Published private(set) var firmware = ""

    /// The output takes no level, an app would take the volume keys, and
    /// Desk Link may not press them yet: the menu offers to ask.
    var asksForKeys: Bool { output.percent == nil && VolumeKeys.taken && !VolumeKeys.allowed }
    @Published var sharesClaude: Bool {
        didSet { defaults.set(sharesClaude, forKey: "claude") }
    }
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
        didSet {
            Keychain.write(key)
            seal = Seal(hex: key)
        }
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
    private static let browsers: Set<String> = [
        "com.apple.Safari", "com.google.Chrome", "org.mozilla.firefox", "company.thebrowser.Browser",
        "com.microsoft.edgemac", "com.brave.Browser", "com.operasoftware.Opera", "com.vivaldi.Vivaldi",
    ]
    // The panel forgets a laptop it has not heard from in 30 seconds. It is
    // told how this one is this often whatever plays, which is the ping.
    private static let heartbeat: TimeInterval = 10

    private let defaults = UserDefaults.standard
    private let source: NowPlayingSource
    private let output = SystemVolume()
    private var server: CommandServer!
    private var artwork: Artwork?
    private var beat: Timer?
    private var awake: NSObjectProtocol?
    private var pendingReport: DispatchWorkItem?
    private var seal: Seal?
    private let machine = Host.current().localizedName ?? ProcessInfo.processInfo.hostName

    init() {
        defaults.register(defaults: ["sharing": true, "panel": "smart-flexispot", "claude": true])
        sharing = defaults.bool(forKey: "sharing")
        sharesClaude = defaults.bool(forKey: "claude")
        panel = defaults.string(forKey: "panel") ?? "smart-flexispot"
        // A key handed over with `defaults write nl.w-tb.desklink key ...` is
        // moved into the keychain at once.
        if let given = defaults.string(forKey: "key"), !given.isEmpty {
            Keychain.write(given)
            defaults.removeObject(forKey: "key")
        }
        let stored = Keychain.read()
        key = stored
        seal = Seal(hex: stored)
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

    /// The menu's own button, as the panel's is.
    func playPause() {
        guard let track else { return }
        source.send(track.playing ? .pause : .play)
    }

    private func begin() {
        // App Nap holds a menu bar app's timers back by minutes, past the
        // panel's patience, which then takes the Mac for gone.
        awake = ProcessInfo.processInfo.beginActivity(options: .userInitiatedAllowingIdleSystemSleep,
                                                      reason: "Telling the desk panel what plays")
        source.start()
        server.start()
        beat = Timer.scheduledTimer(withTimeInterval: Self.heartbeat, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.report() }
        }
        report()
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
        cover = nil
        report()  // nothing playing, so the panel lets go at once
        reach = .unknown
        reachesBack = nil
    }

    private func take(_ new: Track?) {
        guard sharing else { return }
        if new?.artwork != track?.artwork {
            artwork = new?.artwork.flatMap(Artwork.init)
            cover = new?.artwork.flatMap { NSImage(data: $0) }
        }
        let paused = new?.playing != track?.playing && new?.title == track?.title
        track = new
        if paused {
            pendingReport?.cancel()
            report()  // a pause is felt at once; it never comes in a burst
        } else {
            reportSoon()
        }
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
        var state: [String: Any] = ["type": "state", "machine": machine, "port": Int(Self.port), "title": ""]
        if let track {
            var takes = ["pause"]
            if track.duration > 0 { takes.append("seek") }
            if track.skips { takes += ["next", "previous"] }
            if let percent = output.percent {
                takes.append("volume")
                state["volume"] = percent
                state["muted"] = output.muted
            } else if VolumeKeys.taken && VolumeKeys.allowed {
                takes.append("volume_step")
            }
            state["app"] = track.app
            state["title"] = track.title
            state["artist"] = track.artist
            state["playing"] = track.playing
            state["position"] = track.position()
            state["duration"] = track.duration
            state["art"] = artwork?.version ?? ""
            // Now Playing does not say; a clip's cover is a wide frame, and
            // before one comes a browser is most likely playing a video.
            state["video"] = artwork.map(\.wide) ?? Self.browsers.contains(track.bundle)
            state["takes"] = takes
        }
        log.info("state \(self.track?.title ?? "nothing", privacy: .public), art \(self.artwork?.version ?? "none", privacy: .public)")
        send(state) { [weak self] reach, pong in
            guard let self, self.sharing else { return }
            self.reach = reach
            if let pong {
                self.heard = Date()
                self.firmware = pong["firmware"] as? String ?? ""
                self.reachesBack = pong["reaches"] as? Bool
            }
        }
    }

    /// To the panel's /link, sealed; `done` hears how it went and, when the
    /// panel answered as the panel, what it said.
    private func send(_ message: [String: Any], done: ((Reach, [String: Any]?) -> Void)? = nil) {
        guard !panel.isEmpty, let seal, let url = URL(string: "http://\(panel)/link"),
              let body = seal.seal(message, to: "/link") else { return }
        var request = URLRequest(url: url, timeoutInterval: 3)
        request.httpMethod = "POST"
        request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        request.httpBody = body
        URLSession.shared.dataTask(with: request) { data, response, _ in
            let status = (response as? HTTPURLResponse)?.statusCode
            Task { @MainActor in
                guard let done else { return }
                guard status != nil else { return done(.unreachable, nil) }
                var opener = seal
                guard status == 200, let data, let answer = opener.message(data, from: "/link reply") else {
                    return done(.refused, nil)  // the wrong key, or whatever answers is not the panel
                }
                done(.answering, answer["type"] as? String == "pong" ? answer : nil)
            }
        }.resume()
    }

    /// An event from tools/claude-hook on this Mac, passed on sealed.
    private func relay(_ event: Data) -> Response {
        guard sharesClaude, let object = try? JSONSerialization.jsonObject(with: event) as? [String: Any] else {
            return .noContent
        }
        send(["type": "claude", "event": object])
        return .noContent
    }

    private func handle(_ request: Request) -> Response {
        if request.method == "POST", request.path == "/claude" {
            return request.local ? relay(request.body) : Response(status: 403)
        }
        guard request.method == "POST", request.path == "/link" || request.path == "/cover",
              seal != nil, let message = seal?.message(request.body, from: request.path),
              let type = message["type"] as? String else { return Response(status: 403) }
        let reply = request.path + " reply"
        switch type {
        case "cover":
            guard let artwork, message["art"] as? String == artwork.version,
                  let sealed = seal?.seal(artwork.jpeg, to: reply) else { return .notFound }
            log.info("cover fetched, \(artwork.jpeg.count) bytes")
            return Response(status: 200, type: "application/octet-stream", body: sealed)
        case "ping":
            return sealedAnswer(["type": "pong"], to: reply)
        case "command":
            guard command(message) else { return Response(status: 400) }
            return sealedAnswer(["type": "ok"], to: reply)
        default:
            return Response(status: 400)
        }
    }

    private func sealedAnswer(_ message: [String: Any], to path: String) -> Response {
        guard let body = seal?.seal(message, to: path) else { return Response(status: 500) }
        return Response(status: 200, type: "application/octet-stream", body: body)
    }

    private func command(_ command: [String: Any]) -> Bool {
        switch command["command"] as? String {
        case "play": source.send(.play)
        case "pause": source.send(.pause)
        case "next": source.send(.next)
        case "previous": source.send(.previous)
        case "seek":
            guard let position = (command["position"] as? NSNumber)?.doubleValue else { return false }
            source.seek(to: position)
        case "volume":
            guard let level = (command["level"] as? NSNumber)?.intValue else { return false }
            output.set(percent: level)
        case "mute":
            guard let muted = command["muted"] as? Bool else { return false }
            output.set(muted: muted)
        case "volume_up": VolumeKeys.press(up: true)
        case "volume_down": VolumeKeys.press(up: false)
        default:
            return false
        }
        return true
    }
}
