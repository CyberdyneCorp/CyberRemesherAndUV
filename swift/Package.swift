// swift-tools-version:5.9
//
// This SwiftPM package needs a Swift toolchain (Xcode 15+ on macOS/iPadOS) and
// the `cyber_capi` shared library from the `capi/` module, so it is built on the
// macOS runner only — see the `swift-package` job in .github/workflows/ci.yml.
// The header it binds is checked against the Swift sources on every platform by
// tests/packaging/test_swift_abi_parity.py (ctest: `swift_abi_parity`).
//
// The header is consumed through the system-library shim in
// `Sources/CCyberRemesher`. The search paths below point at the in-repo capi
// tree by default; real integrations should override them (see README.md).
import PackageDescription

// Relative to this package root (`swift/`). `cyber_capi.h` is checked in; the
// dylib it declares is produced by the CMake build. Xcode consumers should
// instead set HEADER_SEARCH_PATHS / LIBRARY_SEARCH_PATHS on their app target
// and can drop these unsafe flags.
let capiHeaderSearchPath = "../capi/include"

let package = Package(
    name: "CyberRemesher",
    platforms: [
        .iOS(.v15),
        .macOS(.v12),
    ],
    products: [
        .library(name: "CyberRemesher", targets: ["CyberRemesher"]),
    ],
    targets: [
        // C system-library target exposing the versioned C ABI facade
        // (`cyber_capi.h`) through a stable shim header + module map.
        .systemLibrary(
            name: "CCyberRemesher",
            path: "Sources/CCyberRemesher"
        ),

        // Idiomatic Swift surface: typed errors, RAII mesh handle,
        // async/await remeshing with progress + cancellation bridging,
        // UIKit/PencilKit forwarding and CAMetalLayer attachment.
        .target(
            name: "CyberRemesher",
            dependencies: ["CCyberRemesher"],
            swiftSettings: [
                // Lets the shim's `#include <cyber_capi.h>` resolve against the
                // in-repo capi headers. Best-effort; overridable by consumers.
                .unsafeFlags(["-Xcc", "-I\(capiHeaderSearchPath)"])
            ],
            linkerSettings: [
                .linkedLibrary("cyber_capi")
            ]
        ),
    ]
)
