import AppKit
import Foundation

struct Track: Equatable {
    var app: String
    var bundle: String
    var title: String
    var artist: String
    var playing: Bool
    var duration: Double  // 0 when it has none, as a live stream
    var elapsed: Double   // at `at`
    var at: Date
    var rate: Double
    var skips: Bool       // false for an ad, which the player will not skip
    var artwork: Data?

    func position(now: Date = Date()) -> Double {
        guard playing else { return elapsed }
        let position = elapsed + now.timeIntervalSince(at) * (rate > 0 ? rate : 1)
        return duration > 0 ? min(position, duration) : position
    }
}

enum Command: Int {
    case play = 0, pause = 1, next = 4, previous = 5  // MediaRemote's own numbers
}

/// What macOS has as Now Playing, from the adapter: Apple shut the
/// MediaRemote framework to other apps in macOS 15.4, and the adapter reaches
/// it through /usr/bin/perl, which Apple's entitlements still let in.
final class NowPlayingSource: @unchecked Sendable {  // its state is kept on its own queue
    var onChange: ((Track?) -> Void)?

    private let script: URL
    private let framework: URL
    private let queue = DispatchQueue(label: "nl.w-tb.desklink.nowplaying")
    private var process: Process?
    private var pending = Data()
    private var state: [String: Any] = [:]
    private var wanted = false

    init(resources: URL) {
        script = resources.appendingPathComponent("mediaremote-adapter.pl")
        framework = resources.appendingPathComponent("MediaRemoteAdapter.framework")
    }

    func start() {
        queue.async {
            self.wanted = true
            self.launch()
        }
    }

    /// At once, for quitting: the adapter outlives the app otherwise.
    func stopNow() {
        queue.sync {
            self.wanted = false
            self.process?.terminate()
            self.process = nil
        }
    }

    /// One left running by a Desk Link that crashed.
    func clearLeftovers() {
        let clear = Process()
        clear.executableURL = URL(fileURLWithPath: "/usr/bin/pkill")
        clear.arguments = ["-f", script.path]
        try? clear.run()
        clear.waitUntilExit()
    }

    func stop() {
        queue.async {
            self.wanted = false
            self.process?.terminate()
            self.process = nil
            self.state = [:]
        }
    }

    func send(_ command: Command) {
        run(["send", String(command.rawValue)])
    }

    func seek(to seconds: Double) {
        run(["seek", String(Int64(max(seconds, 0) * 1_000_000))])
    }

    private func adapter(_ arguments: [String]) -> Process {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/perl")
        process.arguments = [script.path, framework.path] + arguments
        process.standardError = FileHandle.nullDevice
        return process
    }

    private func run(_ arguments: [String]) {
        let process = adapter(arguments)
        process.standardOutput = FileHandle.nullDevice
        try? process.run()
    }

    private func launch() {
        guard wanted, process == nil else { return }
        let process = adapter(["stream", "--micros", "--debounce=200"])
        let output = Pipe()
        process.standardOutput = output
        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            self?.queue.async { self?.take(data) }
        }
        process.terminationHandler = { [weak self] _ in
            output.fileHandleForReading.readabilityHandler = nil
            self?.queue.asyncAfter(deadline: .now() + 2) {
                guard let self, self.process === process else { return }
                self.process = nil
                self.pending.removeAll()
                self.launch()
            }
        }
        do {
            try process.run()
            self.process = process
        } catch {
            NSLog("Desk Link: the adapter would not start: \(error)")
        }
    }

    private func take(_ data: Data) {
        pending.append(data)
        while let end = pending.firstIndex(of: UInt8(ascii: "\n")) {
            let line = pending[pending.startIndex..<end]
            pending.removeSubrange(pending.startIndex...end)
            guard let message = try? JSONSerialization.jsonObject(with: line) as? [String: Any],
                  let payload = message["payload"] as? [String: Any] else { continue }
            if message["diff"] as? Bool == true {
                for (key, value) in payload {
                    if value is NSNull { state.removeValue(forKey: key) } else { state[key] = value }
                }
            } else {
                state = payload
            }
            let track = Self.track(from: state)
            DispatchQueue.main.async { self.onChange?(track) }
        }
    }

    private static func track(from state: [String: Any]) -> Track? {
        guard let title = state["title"] as? String, !title.isEmpty else { return nil }
        let micros = { (key: String) in (state[key] as? NSNumber)?.doubleValue ?? 0 }
        let stamp = micros("timestampEpochMicros")
        let bundle = state["parentApplicationBundleIdentifier"] as? String
            ?? state["bundleIdentifier"] as? String ?? ""
        return Track(
            app: appName(bundle),
            bundle: bundle,
            title: title,
            artist: state["artist"] as? String ?? "",
            playing: state["playing"] as? Bool ?? false,
            duration: micros("durationMicros") / 1_000_000,
            elapsed: micros("elapsedTimeMicros") / 1_000_000,
            at: stamp > 0 ? Date(timeIntervalSince1970: stamp / 1_000_000) : Date(),
            rate: (state["playbackRate"] as? NSNumber)?.doubleValue ?? 1,
            skips: state["prohibitsSkip"] as? Bool != true,
            artwork: (state["artworkData"] as? String).flatMap { Data(base64Encoded: $0) })
    }

    private static func appName(_ bundle: String) -> String {
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bundle) else { return "" }
        return FileManager.default.displayName(atPath: url.path)
            .replacingOccurrences(of: ".app", with: "")
    }
}
