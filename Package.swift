// swift-tools-version:5.10
import PackageDescription

let package = Package(
    name: "DeskLink",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(name: "DeskLink", path: "Sources/DeskLink")
    ]
)
