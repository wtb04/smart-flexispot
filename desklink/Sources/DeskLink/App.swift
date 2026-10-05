import SwiftUI

@main
struct DeskLinkApp: App {
    @StateObject private var link = Link()

    var body: some Scene {
        MenuBarExtra {
            Menu(link: link)
        } label: {
            Image(systemName: link.symbol)
        }
        .menuBarExtraStyle(.window)
    }
}

extension Link {
    enum Health { case off, good, trouble }

    var health: Health {
        guard sharing else { return .off }
        switch reach {
        case .answering: return reachesBack == false ? .trouble : .good
        case .unknown: return key.isEmpty ? .trouble : .good
        case .refused, .unreachable: return .trouble
        }
    }

    var symbol: String {
        switch health {
        case .off: return "antenna.radiowaves.left.and.right.slash"
        case .good: return track != nil ? "dot.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right"
        case .trouble: return "exclamationmark.triangle"
        }
    }
}

private struct Menu: View {
    @ObservedObject var link: Link
    @State private var showsSettings = false

    var body: some View {
        VStack(spacing: 12) {
            header
            if link.sharing { playing }
            if link.sharing && link.asksForKeys {
                HStack {
                    Text("The panel can step this display's volume").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button("Allow") { VolumeKeys.askToAllow() }.buttonStyle(.glass).controlSize(.small)
                }
            }
            settings
            HStack {
                Spacer()
                Button("Quit Desk Link") { NSApplication.shared.terminate(nil) }
                    .buttonStyle(.glass)
                    .controlSize(.small)
            }
        }
        .padding(14)
        .frame(width: 320)
    }

    private var header: some View {
        HStack(alignment: .center, spacing: 10) {
            VStack(alignment: .leading, spacing: 3) {
                Text("Desk Link").font(.headline)
                HStack(spacing: 6) {
                    Circle().fill(dot).frame(width: 8, height: 8)
                    Text(status).font(.subheadline).foregroundStyle(.secondary)
                }
            }
            Spacer()
            Toggle("Share with the desk panel", isOn: $link.sharing)
                .labelsHidden()
                .toggleStyle(.switch)
        }
    }

    @ViewBuilder private var playing: some View {
        if let track = link.track {
            HStack(spacing: 12) {
                cover
                VStack(alignment: .leading, spacing: 4) {
                    Text(track.title).font(.callout.weight(.semibold)).lineLimit(2)
                    Text([track.artist, track.app].filter { !$0.isEmpty }.joined(separator: ", "))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    if track.duration > 0 { Progress(track: track) }
                }
                Spacer(minLength: 0)
            }
            .padding(10)
            .glassEffect(.regular, in: .rect(cornerRadius: 14))
        } else {
            HStack {
                Image(systemName: "music.note").foregroundStyle(.tertiary)
                Text("Nothing playing").foregroundStyle(.secondary)
                Spacer()
            }
            .padding(10)
            .glassEffect(.regular, in: .rect(cornerRadius: 14))
        }
    }

    @ViewBuilder private var cover: some View {
        let shape = RoundedRectangle(cornerRadius: 8, style: .continuous)
        if let image = link.cover {
            Image(nsImage: image)
                .resizable()
                .aspectRatio(contentMode: .fill)
                .frame(width: 72, height: 52)
                .clipShape(shape)
        } else {
            shape.fill(.quaternary)
                .frame(width: 72, height: 52)
                .overlay(Image(systemName: "play.rectangle").foregroundStyle(.secondary))
        }
    }

    private var settings: some View {
        DisclosureGroup("Panel", isExpanded: $showsSettings) {
            Form {
                TextField("Address", text: $link.panel, prompt: Text("smart-flexispot"))
                SecureField("Key", text: $link.key, prompt: Text("DESK_LINK_KEY"))
                Toggle("Claude Code sessions", isOn: $link.sharesClaude)
                Toggle("Open at login", isOn: $link.opensAtLogin)
            }
            .formStyle(.grouped)
            .scrollDisabled(true)
            .frame(height: 190)
        }
        .font(.subheadline)
    }

    private var dot: Color {
        switch link.health {
        case .off: return .secondary
        case .good: return link.reach == .answering && link.reachesBack == true ? .green : .yellow
        case .trouble: return .orange
        }
    }

    private var status: String {
        guard link.sharing else { return "Not sharing" }
        if link.key.isEmpty { return "Needs the panel's key" }
        switch link.reach {
        case .unknown: return "Looking for \(link.panel)"
        case .answering:
            switch link.reachesBack {
            case true: return "Connected to \(link.panel)"
            case false: return "\(link.panel) cannot reach this Mac"
            default: return "\(link.panel) answers, checking back"
            }
        case .refused: return "Wrong key, or not the panel"
        case .unreachable: return "\(link.panel) not reachable"
        }
    }
}

private struct Progress: View {
    let track: Track

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { context in
            let at = track.position(now: context.date)
            VStack(alignment: .leading, spacing: 2) {
                ProgressView(value: min(at, track.duration), total: track.duration)
                    .progressViewStyle(.linear)
                    .controlSize(.small)
                HStack {
                    Text(clock(at))
                    Spacer()
                    Text(clock(track.duration))
                }
                .font(.caption2.monospacedDigit())
                .foregroundStyle(.secondary)
            }
        }
    }

    private func clock(_ seconds: Double) -> String {
        let whole = Int(seconds)
        let (h, m, s) = (whole / 3600, whole / 60 % 60, whole % 60)
        return h > 0 ? String(format: "%d:%02d:%02d", h, m, s) : String(format: "%d:%02d", m, s)
    }
}
