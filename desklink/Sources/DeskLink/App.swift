import SwiftUI

@main
struct DeskLinkApp: App {
    @StateObject private var link = Link()

    var body: some Scene {
        MenuBarExtra {
            Menu(link: link)
        } label: {
            Image(nsImage: MenuIcon.image(for: link.health, playing: link.track != nil))
        }
        .menuBarExtraStyle(.window)

        Settings {
            SettingsView(link: link)
        }
        .windowResizability(.contentSize)
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

/// The menu bar's icon, the panel as a display: as is when connected, with
/// play when something plays, struck through when off and with a warning when
/// something is wrong. Templates, so the menu bar colours them.
enum MenuIcon {
    static func image(for health: Link.Health, playing: Bool) -> NSImage {
        switch health {
        case .off: return struck("display")
        case .trouble: return symbol("display.trianglebadge.exclamationmark")
        case .good, .checking: return symbol(playing ? "play.display" : "display")
        }
    }

    private static let configuration = NSImage.SymbolConfiguration(pointSize: 15, weight: .regular)
        .applying(.preferringMonochrome())  // an outline, as the play and warning ones are

    private static func symbol(_ name: String) -> NSImage {
        let image = NSImage(systemSymbolName: name, accessibilityDescription: "Desk Link")!
            .withSymbolConfiguration(configuration)!
        image.isTemplate = true
        return image
    }

    // SF Symbols has no display.slash: one drawn as Apple draws theirs, the
    // stroke with a gap cut either side of it.
    private static func struck(_ name: String) -> NSImage {
        let base = symbol(name)
        let image = NSImage(size: base.size, flipped: false) { rect in
            base.draw(in: rect)
            let stroke = NSBezierPath()
            stroke.move(to: NSPoint(x: rect.minX + 1.5, y: rect.maxY - 1.5))
            stroke.line(to: NSPoint(x: rect.maxX - 1.5, y: rect.minY + 1.5))
            stroke.lineCapStyle = .round
            NSGraphicsContext.current?.compositingOperation = .destinationOut
            stroke.lineWidth = 4.5
            stroke.stroke()
            NSGraphicsContext.current?.compositingOperation = .sourceOver
            NSColor.black.setStroke()
            stroke.lineWidth = 1.6
            stroke.stroke()
            return true
        }
        image.isTemplate = true
        return image
    }
}

private struct Menu: View {
    @ObservedObject var link: Link
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        VStack(spacing: 10) {
            if link.sharing {
                Playing(link: link)
            }
            HStack(spacing: 8) {
                Circle().fill(link.dot).frame(width: 7, height: 7)
                Text(link.status).font(.callout).foregroundStyle(.secondary).lineLimit(1)
                Spacer()
                Toggle("Share with the desk panel", isOn: $link.sharing)
                    .labelsHidden()
                    .toggleStyle(.switch)
                    .controlSize(.mini)
                SwiftUI.Menu {
                    Button("Settings…") {
                        NSApp.activate()
                        openSettings()
                    }
                    Divider()
                    Button("Quit Desk Link") { NSApplication.shared.terminate(nil) }
                } label: {
                    Image(systemName: "ellipsis")
                }
                .menuStyle(.button)
                .buttonStyle(.glass)
                .buttonBorderShape(.circle)
                .menuIndicator(.hidden)
                .controlSize(.small)
                .fixedSize()
            }
            .padding(.horizontal, 4)
        }
        .padding(10)
        .frame(width: 300)
    }
}

private struct Playing: View {
    @ObservedObject var link: Link

    var body: some View {
        if let track = link.track {
            VStack(spacing: 8) {
                HStack(spacing: 10) {
                    Cover(image: link.cover)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(track.title).font(.callout.weight(.semibold)).lineLimit(1)
                        Text([track.artist, track.app].filter { !$0.isEmpty }.joined(separator: ", "))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                    Spacer(minLength: 0)
                    Button {
                        link.playPause()
                    } label: {
                        Image(systemName: track.playing ? "pause.fill" : "play.fill").frame(width: 16, height: 16)
                    }
                    .buttonStyle(.glass)
                    .buttonBorderShape(.circle)
                }
                if track.duration > 0 { Bar(track: track) }
            }
            .padding(10)
            .glassEffect(.regular, in: .rect(cornerRadius: 14))
        }
    }
}

private struct Cover: View {
    let image: NSImage?

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: 7, style: .continuous)
        Group {
            if let image {
                Image(nsImage: image).resizable().aspectRatio(contentMode: .fill)
            } else {
                shape.fill(.quaternary).overlay(Image(systemName: "music.note").foregroundStyle(.secondary))
            }
        }
        .frame(width: 44, height: 44)
        .clipShape(shape)
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

/// A row's icon, as System Settings has them: a white symbol on a colour.
private struct Tile: View {
    let symbol: String
    let colour: Color

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: 11, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: 22, height: 22)
            .background(colour.gradient, in: .rect(cornerRadius: 6, style: .continuous))
    }
}

private struct SettingsView: View {
    @ObservedObject var link: Link

    var body: some View {
        Form {
            Section {
                HStack(spacing: 14) {
                    Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 56, height: 56)
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Desk Link").font(.title3.weight(.semibold))
                        HStack(spacing: 6) {
                            Circle().fill(link.dot).frame(width: 7, height: 7)
                            Text(link.status).foregroundStyle(.secondary)
                        }
                        .font(.callout)
                        if !link.firmware.isEmpty {
                            Text("Panel firmware \(link.firmware)").font(.caption).foregroundStyle(.tertiary)
                        }
                    }
                }
                .padding(.vertical, 4)
            }

            Section("Panel") {
                LabeledContent {
                    TextField("Address", text: $link.panel, prompt: Text("smart-flexispot")).labelsHidden()
                } label: {
                    Label { Text("Address") } icon: { Tile(symbol: "network", colour: .blue) }
                }
                LabeledContent {
                    SecureField("Key", text: $link.key, prompt: Text("64 hex digits"))
                        .labelsHidden()
                        .frame(maxWidth: 200)  // the dots of a long key run on past the edge otherwise
                } label: {
                    Label { Text("Key") } icon: { Tile(symbol: "key.fill", colour: .gray) }
                }
            }

            Section("Share") {
                Toggle(isOn: $link.sharing) {
                    Label { Text("What plays on this Mac") } icon: { Tile(symbol: "play.fill", colour: .orange) }
                }
                Toggle(isOn: $link.sharesClaude) {
                    Label { Text("Claude Code sessions") } icon: { Tile(symbol: "terminal.fill", colour: .purple) }
                }
            }

            Section {
                LabeledContent {
                    if VolumeKeys.allowed {
                        Text("Allowed").foregroundStyle(.secondary)
                    } else {
                        Button("Allow…") { VolumeKeys.askToAllow() }
                    }
                } label: {
                    Label { Text("Volume keys") } icon: { Tile(symbol: "speaker.wave.2.fill", colour: .pink) }
                }
            } footer: {
                Text("For a display whose volume macOS cannot set: with MonitorControl running, the panel's − and + press the volume keys.")
            }

            Section {
                Toggle(isOn: $link.opensAtLogin) {
                    Label { Text("Open at login") } icon: { Tile(symbol: "power", colour: .green) }
                }
            }

        }
        .formStyle(.grouped)
        .scrollDisabled(true)
        .frame(width: 460)
        .fixedSize(horizontal: false, vertical: true)
        .safeAreaInset(edge: .bottom) {
            HStack {
                Spacer()
                Button("Quit Desk Link") { NSApplication.shared.terminate(nil) }
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 18)
        }
        .toolbar(removing: .title)
        // With no title, the toolbar's own background was left, a small
        // rectangle at the top while the window is not in front.
        .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
        .containerBackground(.thickMaterial, for: .window)
        // Opened, nothing is being typed in yet: the address is not selected.
        .onAppear { DispatchQueue.main.async { NSApp.keyWindow?.makeFirstResponder(nil) } }
    }
}
