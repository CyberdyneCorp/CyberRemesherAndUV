# iOS XCFramework distribution

`build_xcframework.sh` stages a self-contained, static arm64 iOS distribution:

- `CyberRemesherC.xcframework` has `ios-arm64` and `ios-arm64-simulator`
  slices, its public C header, and module maps.
- `CyberRemesher/` is the matching SwiftPM package. Its public
  `CyberRemesher` target wraps the binary framework; an application does not
  configure a C header or native-library search path.
- `CyberRemesherConsumer/` is an external-style fixture copied beside the
  staged package. It has no path to the repository C API or CMake output.

Build and validate the release-shaped artifact with:

```sh
CMAKE_BUILD_PARALLEL_LEVEL=4 packaging/ios/validate_xcframework.sh
```

The validation script builds both slices, resolves the staged consumer against
the artifact, creates a temporary app bundle, and launches it on an available
iPhone simulator. Set `IOS_SIMULATOR_UDID` to select a particular simulator.
The probe checks the C/Swift ABI, remeshes an in-memory mesh, reads back its
authored polygons, observes the progress stream, and verifies explicit
cooperative cancellation.

The artifact is CPU-only: Metal and the renderer are disabled, and the optional
in-process QuadCover/Geogram solver is not packaged. Hosts use the native
seamless-UV solver and retain ownership of rendering, Metal layers, and UI.
The minimum deployment target is iOS 15.

This script stages a local release artifact; release automation must publish its
zip and checksum before an application can declare a remote SwiftPM binary
target. Simulator verification is an executable CI gate. Physical-device
validation uses a caller-supplied development profile and identity, so team
credentials never enter the repository:

```sh
IOS_DEVICE_ID=<devicectl-identifier> \
IOS_PROVISIONING_PROFILE=/path/to/development.mobileprovision \
IOS_SIGNING_IDENTITY='Apple Development: Name (TEAMID)' \
packaging/ios/validate_device_xcframework.sh
```

It installs the same staged consumer, verifies ABI/remesh/progress/cancellation
on hardware, and prints the elapsed wall time. Repeat and thermal behaviour,
peak memory, and cancellation latency remain named release measurements; they
must not be inferred from a simulator run.
