import SwiftUI

@main
struct DeskLinkApp: App {
    @StateObject private var link = Link()

    var body: some Scene {
        MenuBarExtra {
            Menu(link: link)
        } label: {
            Image(systemName: link.sharing ? "antenna.radiowaves.left.and.right"
                                           : "antenna.radiowaves.left.and.right.slash")
        }
        .menuBarExtraStyle(.window)
    }
}

private struct Menu: View {
    @ObservedObject var link: Link

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Toggle("Share with the desk panel", isOn: $link.sharing)
                .toggleStyle(.switch)
                .font(.headline)
            status
            Divider()
            Form {
                TextField("Panel", text: $link.panel, prompt: Text("smart-flexispot"))
                SecureField("Key", text: $link.key, prompt: Text("LAPTOP_KEY"))
                Toggle("Open at login", isOn: $link.opensAtLogin)
            }
            Divider()
            Button("Quit Desk Link") { NSApplication.shared.terminate(nil) }
        }
        .padding(16)
        .frame(width: 300)
    }

    @ViewBuilder private var status: some View {
        if !link.sharing {
            Text("Not sharing").foregroundStyle(.secondary)
        } else {
            VStack(alignment: .leading, spacing: 4) {
                if let track = link.track {
                    Text(track.title).lineLimit(2)
                    Text([track.artist, track.app].filter { !$0.isEmpty }.joined(separator: ", "))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                } else {
                    Text("Nothing playing").foregroundStyle(.secondary)
                }
                Text(reach).font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private var reach: String {
        if link.key.isEmpty { return "Needs the panel's key" }
        switch link.reach {
        case .unknown: return "Panel not asked yet"
        case .answering: return "Panel answering"
        case .refused: return "Panel refused the key"
        case .unreachable: return "Panel not reachable"
        }
    }
}
