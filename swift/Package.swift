// swift-tools-version:5.9
//
// UNVERIFIED: This SwiftPM package cannot be built in the headless Linux CI —
// it requires a Swift toolchain (Xcode 15+ on macOS/iPadOS) *and* the
// `cyber_capi` shared library + `cyber_capi.h` produced by the `capi/` module
// (tasks 13.1). The header is consumed through the system-library shim in
// `Sources/CCyberRemesher`. The header search path and link path below point at
// the in-repo capi build tree by default; real integrations should override
// them (see README.md). Structure, types and control flow are implemented for
// real — there are no throwing stubs — but nothing here has been compiled.
import PackageDescription

// Relative to this package root (`swift/`). The capi headers live in the
// sibling module and the built dylib is produced by the CMake build. Xcode
// consumers should instead set HEADER_SEARCH_PATHS / LIBRARY_SEARCH_PATHS on
// their app target and can drop these unsafe flags.
let capiHeaderSearchPath = "../src/capi/include"

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
