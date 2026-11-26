// swift-tools-version:5.9
// Built with the Command Line Tools only (no Xcode): `swift build -c release` in this folder.
import PackageDescription

let package = Package(
    name: "iMacDisplay",
    platforms: [.macOS(.v14)],
    targets: [
        // Declarations for Apple's private virtual-display classes (they live in CoreGraphics).
        .target(name: "CVirtualDisplay", linkerSettings: [.linkedFramework("CoreGraphics")]),
        .target(name: "IMDKit", dependencies: ["CVirtualDisplay"], linkerSettings: [.linkedFramework("NetFS")]),
        .executableTarget(name: "imd", dependencies: ["IMDKit"]),                 // test tool for development
        .executableTarget(name: "iMacDisplayApp", dependencies: ["IMDKit"]),      // the app: the one thing the user installs
    ]
)
