# build-and-packaging — Build System, CI, and Distribution

## ADDED Requirements

### Requirement: C++20 CMake build
The project SHALL build with CMake (presets for every platform/backend combination) as strict C++20 with warnings-as-errors on the project's own code. The core engine SHALL compile with no GUI, GPU, or platform SDK present (CPU-only headless configuration).

#### Scenario: Minimal configuration builds
- **WHEN** the CPU-only headless preset is configured on a clean Linux container
- **THEN** the core engine, CLI, and unit tests SHALL build and pass without any GPU SDK installed

### Requirement: Permissive-license dependency policy
All dependencies SHALL be permissively licensed (MIT/BSD/Apache-2.0/MPL-2.0 or equivalent). GPL/LGPL code SHALL NOT be linked. CI SHALL run an automated license audit of the dependency manifest and fail on violations. Third-party attributions SHALL ship in the About panel and packages.

#### Scenario: License gate
- **WHEN** a dependency with a GPL license is added to the manifest
- **THEN** the CI license audit SHALL fail naming the dependency

### Requirement: Test gates
CI SHALL gate every merge on: unit tests (core modules), mesh-kernel property tests, stroke-recognizer trace tests, backend parity tests (on Metal and CUDA hardware lanes), and the golden-mesh regression suite (recorded baselines for quad count, non-quad count, singularity count, and Hausdorff distance with tolerances on a permissively-licensed corpus).

#### Scenario: Regression drift fails CI
- **WHEN** a change alters a golden mesh's quad count beyond tolerance
- **THEN** CI SHALL fail showing baseline vs. observed metrics

### Requirement: Platform packages
CI SHALL produce installable artifacts for macOS (signed/notarized DMG), Windows (zip and installer), Linux (AppImage), iPadOS/iOS (archive for TestFlight/App Store lanes), and Android (APK/AAB), with artifact names carrying the semantic version. Tagged releases SHALL publish a GitHub Release with the artifacts attached (AutoRemesher only uploaded CI artifacts).

#### Scenario: Tagged release publishes
- **WHEN** a version tag is pushed and CI succeeds
- **THEN** a GitHub Release SHALL exist with all platform artifacts attached

### Requirement: Package smoke tests
Each desktop package SHALL be smoke-tested from the packaged form (mounted DMG / extracted zip / AppImage): a CLI remesh of a reference model asserting a valid output file and exit code 0, plus an app-launch screenshot. Mobile artifacts SHALL at minimum boot in a simulator/emulator in CI.

#### Scenario: Smoke failure blocks artifacts
- **WHEN** the packaged CLI remesh exits nonzero
- **THEN** the job SHALL fail and the package SHALL NOT be published

### Requirement: Single version identity
The semantic version SHALL originate from one source of truth, be embedded in binaries (`--version`, About panel), artifact filenames, and release tags, with no possibility of divergence.

#### Scenario: Consistent version everywhere
- **WHEN** release 1.2.0 artifacts are inspected
- **THEN** binary version output, artifact names, and the release tag SHALL all read 1.2.0

### Requirement: Style and static analysis gates
CI SHALL enforce clang-format and clang-tidy on project code for every PR; violations fail the job with the diff/report visible.

#### Scenario: Unformatted PR fails
- **WHEN** a PR contains formatting violations
- **THEN** the format job SHALL fail showing the required diff
