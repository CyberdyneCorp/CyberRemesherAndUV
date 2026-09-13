// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CyberRemesherConsumer",
    platforms: [.iOS(.v15)],
    dependencies: [.package(path: "../CyberRemesher")],
    targets: [
        .executableTarget(
            name: "CyberRemesherConsumer",
            dependencies: [.product(name: "CyberRemesher", package: "CyberRemesher")]
        ),
    ]
)
