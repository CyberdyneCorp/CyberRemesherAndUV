## Purpose

Provides a versioned, self-contained iOS library artifact that host applications
can resolve through SwiftPM and use without repository-relative native paths.

## ADDED Requirements

### Requirement: iOS binary-backed Swift package distribution
The project SHALL produce a versioned XCFramework containing the C ABI for both
iOS device and iOS Simulator. It SHALL ship a SwiftPM package surface that uses
that artifact and does not require a consumer to configure header-search or
library-search paths into this repository. The artifact SHALL contain the
public C header and all runtime code needed by the supported CPU remeshing
profile.

#### Scenario: Clean consumer resolves the package
- **WHEN** a clean iOS consumer project adds the released Swift package and builds for an iOS Simulator
- **THEN** it SHALL compile and link a remesh call without a sibling source checkout or manual native linker settings

#### Scenario: Device and simulator slices are present
- **WHEN** the release XCFramework is inspected
- **THEN** it SHALL contain an arm64 iOS-device slice and a simulator slice compatible with the supported simulator architectures

### Requirement: Executable iOS packaging evidence
The project SHALL validate the XCFramework layout and a clean consumer build in
CI on an Apple runner. The validation SHALL exercise CPU remeshing in memory,
the runtime ABI compatibility check, progress observation and explicit job
cancellation. Device measurements SHALL be recorded separately when a physical
device is available and SHALL NOT be inferred from simulator execution.

#### Scenario: Simulator consumer regression
- **WHEN** the iOS packaging validation runs on its supported Apple CI runner
- **THEN** the clean consumer SHALL build and its integration tests SHALL run on an iOS Simulator
