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

        Settings {
            SettingsView(link: link)
        }
    }
}

extension Link {
    enum Health { case off, good, checking, trouble }

    var health: Health {
        guard sharing else { return .off }
        if key.isEmpty { return .trouble }
        switch reach {
        case .answering: return reachesBack == false ? .trouble : reachesBack == true ? .good : .checking
        case .unknown: return .checking
        case .refused, .unreachable: return .trouble
        }
    }

    var symbol: String {
        switch health {
        case .off: return "antenna.radiowaves.left.and.right.slash"
        case .good, .checking: return track != nil ? "dot.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right"
        case .trouble: return "exclamationmark.triangle"
        }
    }

    var dot: Color {
        switch health {
        case .off: return .secondary
        case .good: return .green
        case .checking: return .yellow
        case .trouble: return .orange
        }
    }

    var status: String {
        guard sharing else { return "Not sharing" }
        if key.isEmpty { return "Needs the panel's key" }
        switch reach {
        case .unknown: return "Looking for \(panel)"
        case .answering:
            switch reachesBack {
            case true: return "Connected to \(panel)"
            case false: return "\(panel) cannot reach this Mac"
            default: return "Connected, checking back"
            }
        case .refused: return "Wrong key, or not the panel"
        case .unreachable: return "\(panel) not reachable"
        }
    }
}

private struct Menu: View {
    @ObservedObject var link: Link
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        VStack(spacing: 10) {
            HStack(spacing: 8) {
                Circle().fill(link.dot).frame(width: 8, height: 8)
                Text(link.status).font(.callout).lineLimit(1)
                Spacer()
                Toggle("Share with the desk panel", isOn: $link.sharing)
                    .labelsHidden()
                    .toggleStyle(.switch)
                    .controlSize(.small)
            }
            if link.sharing {
                Playing(link: link)
            }
            if link.sharing && link.asksForKeys {
                Button {
                    VolumeKeys.askToAllow()
                } label: {
                    Label("Let the panel step this display's volume", systemImage: "speaker.wave.2")
                        .font(.caption)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
            }
            HStack {
                Button("Settings…") {
                    NSApp.activate()
                    openSettings()
                }
                Spacer()
                Button("Quit") { NSApplication.shared.terminate(nil) }
            }
            .buttonStyle(.glass)
            .controlSize(.small)
        }
        .padding(12)
        .frame(width: 300)
    }
}

private struct Playing: View {
    @ObservedObject var link: Link

    var body: some View {
        HStack(spacing: 10) {
            cover
            VStack(alignment: .leading, spacing: 3) {
                if let track = link.track {
                    Text(track.title).font(.callout.weight(.medium)).lineLimit(1)
                    Text([track.artist, track.app].filter { !$0.isEmpty }.joined(separator: ", "))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    if track.duration > 0 { Bar(track: track) }
                } else {
                    Text("Nothing playing").font(.callout).foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 0)
            if let track = link.track {
                Button {
                    link.playPause()
                } label: {
                    Image(systemName: track.playing ? "pause.fill" : "play.fill").frame(width: 14, height: 14)
                }
                .buttonStyle(.glass)
                .buttonBorderShape(.circle)
            }
        }
        .padding(8)
        .glassEffect(.regular, in: .rect(cornerRadius: 12))
    }

    @ViewBuilder private var cover: some View {
        let shape = RoundedRectangle(cornerRadius: 6, style: .continuous)
        if let image = link.cover {
            Image(nsImage: image)
                .resizable()
                .aspectRatio(contentMode: .fill)
                .frame(width: 44, height: 44)
                .clipShape(shape)
        } else {
            shape.fill(.quaternary)
                .frame(width: 44, height: 44)
                .overlay(Image(systemName: link.track == nil ? "music.note" : "play.rectangle").foregroundStyle(.secondary))
        }
    }
}

private struct Bar: View {
    let track: Track

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { context in
            ProgressView(value: min(track.position(now: context.date), track.duration), total: track.duration)
                .progressViewStyle(.linear)
                .controlSize(.mini)
                .tint(.secondary)
        }
    }
}

private struct SettingsView: View {
    @ObservedObject var link: Link

    var body: some View {
        Form {
            Section("Panel") {
                TextField("Address", text: $link.panel, prompt: Text("smart-flexispot"))
                SecureField("Key", text: $link.key, prompt: Text("DESK_LINK_KEY, 64 hex digits"))
                LabeledContent("Link") {
                    HStack(spacing: 6) {
                        Circle().fill(link.dot).frame(width: 8, height: 8)
                        Text(link.status)
                    }
                }
                if !link.firmware.isEmpty {
                    LabeledContent("Firmware", value: link.firmware)
                }
            }
            Section("Share") {
                Toggle("What plays on this Mac", isOn: $link.sharing)
                Toggle("Claude Code sessions", isOn: $link.sharesClaude)
            }
            Section {
                LabeledContent("Volume keys") {
                    if VolumeKeys.allowed {
                        Text("Allowed").foregroundStyle(.secondary)
                    } else {
                        Button("Allow…") { VolumeKeys.askToAllow() }
                    }
                }
            } footer: {
                Text("For a display whose volume macOS cannot set: with MonitorControl running, the panel's − and + press the volume keys.")
            }
            Section {
                Toggle("Open at login", isOn: $link.opensAtLogin)
            }
        }
        .formStyle(.grouped)
        .frame(width: 440)
        .fixedSize(horizontal: false, vertical: true)
        // Opened, nothing is being typed in yet: the address is not selected.
        .onAppear { DispatchQueue.main.async { NSApp.keyWindow?.makeFirstResponder(nil) } }
    }
}
