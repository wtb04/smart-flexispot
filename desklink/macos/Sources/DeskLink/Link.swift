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
    @Published private(set) var desk: Desk?       // where it stands, as the panel last said; nil without a desk
    @Published private(set) var deskGoing: Desk?  // stand or sit while the panel takes it there
    @Published private(set) var deskHeightMM: Int?

    @Published var sharesClaude: Bool {
        didSet { defaults.set(sharesClaude, forKey: "claude") }
    }
    @Published var pings: Bool {  // off, the panel shows the sessions without its chime and notice
        didSet { defaults.set(pings, forKey: "pings") }
    }
    @Published var sharesSlides: Bool {  // the panel's clicker: the keys, and the page read
        didSet {
            defaults.set(sharesSlides, forKey: "slides")
            if !sharesSlides { slide = nil }
            report()
        }
    }
    @Published var sharing: Bool {
        didSet {
            defaults.set(sharing, forKey: "sharing")
            startOrStop()
        }
    }
    /// Empty, it shares on any network.
    @Published var networks: [String] {
        didSet {
            defaults.set(networks, forKey: "networks")
            startOrStop()
        }
    }
    @Published private(set) var networksHere: Set<String>?  // nil while nothing can tell
    @Published var panel: String {
        didSet {
            defaults.set(panel, forKey: "panel")
            if panel != oldValue { forgetReach() }
        }
    }
    @Published var key: String {
        didSet {
            Keychain.write(key)
            seal = Seal(hex: key)
            if key != oldValue { forgetReach() }
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
    private static let follow: TimeInterval = 0.5  // asked this often while it goes, so its height counts along
    // Longer than it takes from one end to the other, should the panel never say it arrived.
    private static let followFor: TimeInterval = 60
    // Okular says nothing as its page turns; looked at this often, a turn is
    // on the panel well within a second.
    private static let leafingEvery: TimeInterval = 0.3

    private let defaults = UserDefaults.standard
    let networkWatch = Networks()
    private var running = false
    private let source: NowPlayingSource
    private let output = SystemVolume()
    private var server: CommandServer!
    private var artwork: Artwork?
    private var beat: Timer?
    private var following: Timer?
    private var followUntil = Date()
    private var watching = false  // the menu open, so the desk is asked after as while it goes
    private var leafing: Timer?  // the slide's page looked at
    private let deck = SlideDeck()
    private var slide: SlideDeck.Slide?
    private var awake: NSObjectProtocol?
    private var pendingReport: DispatchWorkItem?
    private var vanishing: DispatchWorkItem?  // nothing playing, passed on once it has lasted
    private var seal: Seal?
    private var generation = 0  // bumped by a new address or key, so an older reply is not taken for theirs
    private var coverFrom: Data?   // what artwork and cover are made from
    private var lookedUp = ""      // the browser's video a YouTube thumbnail was asked for
    private var thumbnail: Data?   // and the one found
    private var square: Artwork?   // its channel's picture, for where the panel draws a square
    private let machine = Host.current().localizedName ?? ProcessInfo.processInfo.hostName

    // On the charger macOS may keep a sleeping Mac half awake for hours, its
    // screens off, and the panel would show what was paused there all night.
    private static let idleAway: TimeInterval = 120
    private var sleeping = false
    private var stillSince = Date()  // nothing has played since
    private var toldAway = false

    init() {
        defaults.register(defaults: ["sharing": true, "panel": "smart-flexispot", "claude": true, "pings": true,
                                     "slides": true])
        sharing = defaults.bool(forKey: "sharing")
        sharesClaude = defaults.bool(forKey: "claude")
        pings = defaults.bool(forKey: "pings")
        sharesSlides = defaults.bool(forKey: "slides")
        panel = defaults.string(forKey: "panel") ?? "smart-flexispot"
        networks = defaults.stringArray(forKey: "networks") ?? []
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
        let workspace = NSWorkspace.shared.notificationCenter
        workspace.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.sleeping = true; self?.report() }
        }
        workspace.addObserver(forName: NSWorkspace.screensDidSleepNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.report() }
        }
        for woke in [NSWorkspace.didWakeNotification, NSWorkspace.screensDidWakeNotification] {
            workspace.addObserver(forName: woke, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.sleeping = false; self?.report() }
            }
        }
        NotificationCenter.default.addObserver(forName: .openURL, object: nil, queue: .main) { [weak self] opened in
            guard let url = opened.object as? URL else { return }
            MainActor.assumeIsolated { self?.open(url) }
        }
        networksHere = networkWatch.here
        networkWatch.onChange = { [weak self] here in
            self?.networksHere = here
            self?.startOrStop()
        }
        source.clearLeftovers()
        startOrStop()
    }

    var onChosenNetwork: Bool {
        networks.isEmpty || !(networksHere ?? []).isDisjoint(with: networks)
    }

    /// Switched on, and on one of the networks it is to share on, if any are chosen.
    var sharesNow: Bool { sharing && onChosenNetwork }

    private func startOrStop() {
        if sharesNow && !running {
            begin()
        } else if !sharesNow && running {
            end()
        }
    }

    /// The menu's own button, as the panel's is.
    func playPause() {
        guard let track else { return }
        source.send(track.playing ? .pause : .play)
    }

    private func forgetReach() {
        generation += 1
        reach = .unknown
        reachesBack = nil
        deskSaid([:])
    }

    private func begin() {
        running = true
        // App Nap holds a menu bar app's timers back by minutes, past the
        // panel's patience, which then takes the Mac for gone.
        awake = ProcessInfo.processInfo.beginActivity(options: .userInitiatedAllowingIdleSystemSleep,
                                                      reason: "Telling the desk panel what plays")
        source.start()
        server.start()
        beat = Timer.scheduledTimer(withTimeInterval: Self.heartbeat, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.report() }
        }
        leafing = Timer.scheduledTimer(withTimeInterval: Self.leafingEvery, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.lookAtSlide() }
        }
        report()
    }

    private func lookAtSlide() {
        let now = sharesSlides ? deck.read() : nil
        guard now != slide else { return }
        slide = now
        reportSoon()
    }

    private func end() {
        running = false
        vanishing?.cancel()
        vanishing = nil
        beat?.invalidate()
        beat = nil
        leafing?.invalidate()
        leafing = nil
        slide = nil
        if let awake { ProcessInfo.processInfo.endActivity(awake) }
        awake = nil
        source.stop()
        server.stop()
        track = nil
        showCover(nil)
        lookedUp = ""
        thumbnail = nil
        square = nil
        report(lettingGo: true)  // nothing playing, so the panel lets go at once
        reach = .unknown
        reachesBack = nil
        deskSaid([:])
    }

    // Safari lets go of Now Playing for a moment around a pause or a play;
    // passed on, the panel would hand its buttons to the speaker meanwhile.
    private static let goneAfter: TimeInterval = 3

    private func take(_ new: Track?) {
        guard sharesNow else { return }
        vanishing?.cancel()
        vanishing = nil
        if new == nil, track != nil {
            let later = DispatchWorkItem { [weak self] in self?.apply(nil) }
            vanishing = later
            DispatchQueue.main.asyncAfter(deadline: .now() + Self.goneAfter, execute: later)
            return
        }
        apply(new)
    }

    private func apply(_ new: Track?) {
        vanishing = nil
        if let new, Self.browsers.contains(new.bundle) {
            if new.title + "\n" + new.artist != lookedUp { look(for: new) }
        } else {
            lookedUp = ""  // what was found for a browser's video is not this one's
            thumbnail = nil
            square = nil
        }
        showCover(thumbnail ?? new?.artwork)
        let paused = new?.playing != track?.playing && new?.title == track?.title
        track = new
        if paused {
            pendingReport?.cancel()
            report()  // a pause is felt at once; it never comes in a burst
        } else {
            reportSoon()
        }
    }

    private func showCover(_ from: Data?) {
        guard from != coverFrom else { return }
        coverFrom = from
        artwork = from.flatMap(Artwork.init)
        cover = from.flatMap { NSImage(data: $0) }
    }

    /// The video's own thumbnail, sharper than Safari's cover, or one where it gave none.
    private func look(for track: Track) {
        let wanted = track.title + "\n" + track.artist
        lookedUp = wanted
        thumbnail = nil
        square = nil
        Task { [weak self] in
            let found = await YouTubeCover.find(title: track.title, channel: track.artist, small: track.artwork)
            guard let self, self.lookedUp == wanted, let found else { return }
            self.thumbnail = found.thumbnail
            self.square = found.channel.flatMap(Artwork.init)
            self.showCover(found.thumbnail)
            self.reportSoon()
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

    /// Nobody at this Mac: asleep, or its screens off and nothing played for a while.
    /// The screens are asked each time: a wake from a dark wake can come without
    /// its notice, and a Mac kept for away then stayed silent all day.
    private var away: Bool {
        if track?.playing == true { stillSince = Date() }
        guard CGDisplayIsAsleep(CGMainDisplayID()) != 0 else { return false }
        return sleeping || (track?.playing != true && Date().timeIntervalSince(stillSince) > Self.idleAway)
    }

    private func report(lettingGo: Bool = false) {
        // Off, or on another network, nothing goes to the panel but the last word.
        guard running || lettingGo else { return }
        // Told once that nothing plays, the panel lets go; then it hears
        // nothing, so it also takes the Mac for gone.
        let away = self.away
        if away && toldAway { return }
        toldAway = away
        let track = away ? nil : self.track
        var state: [String: Any] = ["type": "state", "machine": machine, "port": Int(Self.port), "title": ""]
        var takes: [String] = []
        if let track {
            takes = ["pause"]
            if track.duration > 0 { takes.append("seek") }
            // Now Playing does not say what a player takes, and a browser's
            // video, YouTube's, takes no next or previous of its own.
            if track.skips && !Self.browsers.contains(track.bundle) { takes += ["next", "previous"] }
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
            state["square"] = square?.version ?? ""
            // Now Playing does not say; a clip's cover is a wide frame, and
            // before one comes a browser is most likely playing a video.
            state["video"] = artwork.map(\.wide) ?? Self.browsers.contains(track.bundle)
        }
        // The clicker's, whatever plays: slides seldom play as Now Playing has it.
        if !away && sharesSlides && VolumeKeys.allowed { takes.append("slides") }
        if !away, let slide {
            takes.append("slide_show")
            state["slide"] = ["page": slide.page, "pages": slide.pages, "deck": slide.deck, "presenting": slide.presenting]
        }
        if !takes.isEmpty { state["takes"] = takes }
        log.info("state \(track?.title ?? (away ? "nothing, away" : "nothing"), privacy: .public), slide \(self.slide.map { "\($0.page) of \($0.pages)" } ?? "none", privacy: .public), art \(self.artwork?.version ?? "none", privacy: .public), square \(self.square?.version ?? "none", privacy: .public)")
        let generation = self.generation
        send(state) { [weak self] reach, answer in
            guard let self, self.sharesNow, self.generation == generation else { return }
            self.reach = reach
            if let pong = answer, pong["type"] as? String == "pong" {
                self.heard = Date()
                self.firmware = pong["firmware"] as? String ?? ""
                self.reachesBack = pong["reaches"] as? Bool
                self.deskSaid(pong)
            }
            if reach != .answering { self.deskSaid([:]) }
        }
    }

    private func deskSaid(_ answer: [String: Any]) {
        let wasGoing = deskGoing
        (desk, deskGoing, deskHeightMM) = Desk.said(answer)
        if deskGoing != nil && wasGoing == nil { followUntil = Date() + Self.followFor }
        keepFollowing()
    }

    /// The menu open or shut: open, where the desk is and its height are asked every half second.
    func watchDesk(_ open: Bool) {
        watching = open
        keepFollowing()
    }

    private func keepFollowing() {
        if deskGoing == nil && !(watching && desk != nil) {
            stopFollowing()
        } else if following == nil {
            following = Timer.scheduledTimer(withTimeInterval: Self.follow, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.followDesk() }
            }
        }
    }

    /// Taps the panel screen's own button for `preset`, so a tap while the desk
    /// moves stops it. Sent before the panel says of a desk, so desklink://stand
    /// works as Desk Link starts.
    @discardableResult
    func go(to preset: Desk) -> Bool {
        guard Desk.presets.contains(preset), sharesNow, seal != nil else { return false }
        askDesk(["type": "desk", "preset": preset.rawValue])
        return true
    }

    private func askDesk(_ message: [String: Any]) {
        let generation = self.generation
        send(message) { [weak self] reach, answer in
            guard let self, self.sharesNow, self.generation == generation else { return }
            guard let answer, answer["type"] as? String == "desk" else {
                return log.warning("the panel did not answer for the desk: \(String(describing: reach), privacy: .public)")
            }
            self.deskSaid(answer)
        }
    }

    private func followDesk() {
        if deskGoing != nil && Date() >= followUntil { deskGoing = nil }
        keepFollowing()
        if following != nil { askDesk(["type": "desk"]) }
    }

    private func stopFollowing() {
        following?.invalidate()
        following = nil
    }

    /// desklink://stand and desklink://sit, for a Shortcut or a key binding.
    private func open(_ url: URL) {
        guard let preset = url.host().flatMap(Desk.init(rawValue:)), Desk.presets.contains(preset) else {
            return log.warning("not a desk link: \(url.absoluteString, privacy: .public)")
        }
        if !go(to: preset) { log.warning("the desk is not sent anywhere: \(self.status, privacy: .public)") }
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
                done(.answering, answer)
            }
        }.resume()
    }

    /// An event from tools/claude-hook on this Mac, passed on sealed.
    private func relay(_ event: Data) -> Response {
        guard sharesClaude, onChosenNetwork, let object = try? JSONSerialization.jsonObject(with: event) as? [String: Any] else {
            return .noContent
        }
        var message: [String: Any] = ["type": "claude", "event": object]
        if !pings { message["quiet"] = true }
        send(message)
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
            let wanted = message["art"] as? String
            guard let picture = [artwork, square].compactMap({ $0 }).first(where: { $0.version == wanted }),
                  let sealed = seal?.seal(picture.jpeg, to: reply) else { return .notFound }
            log.info("cover fetched, \(picture.jpeg.count) bytes")
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
        case "slide_next", "slide_back":
            guard sharesSlides, VolumeKeys.allowed else { return false }
            SlideKeys.press(next: command["command"] as? String == "slide_next")
        case "slide_show":
            guard sharesSlides, VolumeKeys.allowed else { return false }
            return deck.show()
        default:
            return false
        }
        return true
    }
}
