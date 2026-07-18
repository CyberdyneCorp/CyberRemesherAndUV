// swift-tools-version:5.9
//
// UNVERIFIED: iPadOS shell package manifest. Not built in headless Linux CI —
// it depends on the local `CyberRemesher` package (`../../../swift`) which in
// turn needs the `cyber_capi` library and an Apple SDK. The SwiftUI/PencilKit
// views only compile inside an iOS *app* target in Xcode; this manifest exists
// so the toolkit-agnostic shell logic can be resolved/type-checked with
// `swift build` and so the file tree reads as a real package. Not wired into
// the repo CMake tree.
import PackageDescription

let package = Package(
    name: "CyberRemesherShell",
    platforms: [
        .iOS(.v15),
        .macOS(.v12),
    ],
    products: [
        .library(name: "CyberRemesherShell", targets: ["CyberRemesherApp"]),
    ],
    dependencies: [
        // The first-party binding package — the shell's only engine dependency.
        .package(path: "../../../swift"),
    ],
    targets: [
        .target(
            name: "CyberRemesherApp",
            dependencies: [
                .product(name: "CyberRemesher", package: "swift"),
            ],
            resources: [
                // The 8.6 data-driven UI model, shared with the Android shell.
                .copy("../../shared/stages.json"),
                .copy("../../shared/toolbar.default.json"),
            ]
        ),
    ]
)
