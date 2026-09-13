## Why

The current Swift package relies on repository-relative headers, unsafe compiler
flags and a manually built native library. That works for in-tree development,
but an iOS application cannot consume it as a versioned, self-contained library
artifact.

## What Changes

- Build the C ABI as an XCFramework for iOS device and simulator architectures.
- Provide a SwiftPM distribution surface that consumes that artifact without
  sibling source-tree header or library paths.
- Add a clean external-style consumer build and architecture/layout checks to
  make the packaging contract executable in CI.
- Document the supported iOS build/install path, solver profile and known
  device-validation limitations.

## Capabilities

### New Capabilities

- `ios-sdk-distribution`: Versioned XCFramework and SwiftPM consumption for an
  embedded iOS remeshing library.

### Modified Capabilities

- `build-and-packaging`: Include a verifiable iOS library artifact rather than
  only an application archive in platform delivery.
- `engine-bindings`: Define the supported binary-backed Swift package path for
  third-party iOS hosts.

## Impact

Affected areas include CMake iOS build configuration, the Swift package,
packaging scripts and workflows, release documentation, and a standalone
consumer fixture. The C ABI stays source and ABI compatible.
