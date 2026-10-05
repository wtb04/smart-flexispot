// swift-tools-version:5.10
import PackageDescription

let package = Package(
    name: "DeskLink",
    platforms: [.macOS("26.0")],
    targets: [
        .executableTarget(name: "DeskLink", path: "Sources/DeskLink")
    ]
)
