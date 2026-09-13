## MODIFIED Requirements

### Requirement: Platform packages
CI SHALL produce installable artifacts for macOS (signed/notarized DMG), Windows (zip and installer), Linux (AppImage), iPadOS/iOS (archive for TestFlight/App Store lanes and a versioned XCFramework for library consumers), and Android (APK/AAB), with artifact names carrying the semantic version. Tagged releases SHALL publish a GitHub Release with the artifacts attached (AutoRemesher only uploaded CI artifacts).

Each artifact SHALL be self-sufficient on a stock target machine: a package SHALL carry the runtime libraries its build linked (excluding only those a package of that kind must inherit from the host, such as the dynamic loader and the glibc/GCC core), and the package job SHALL fail when a dependency is neither bundled nor excluded by that rule. A published Python wheel SHALL contain the native library it binds; a wheel that is pure Python is not a shippable artifact.

#### Scenario: Tagged release publishes
- **WHEN** a version tag is pushed and CI succeeds
- **THEN** a GitHub Release SHALL exist with all platform artifacts attached

#### Scenario: A package runs on a machine that has none of the build's libraries
- **WHEN** a desktop package is installed on a stock target machine that lacks the optional libraries the build linked (OpenMP, TBB, zlib)
- **THEN** the packaged binary SHALL start and run, because those libraries travel inside the package

#### Scenario: The wheel carries its engine
- **WHEN** the Python publishing lane builds a wheel
- **THEN** the native shared library SHALL be staged into the package before the wheel is built and SHALL be present in the built wheel

#### Scenario: iOS library consumer uses an XCFramework
- **WHEN** an iOS host resolves the released Swift package on device or simulator
- **THEN** its build SHALL link the versioned XCFramework without repository-relative native search paths
