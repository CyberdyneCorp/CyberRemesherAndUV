# build-and-packaging (delta)

## MODIFIED Requirements

### Requirement: Permissive-license dependency policy
All dependencies SHALL be permissively licensed (MIT/BSD/Apache-2.0/MPL-2.0 or equivalent). GPL/LGPL code SHALL NOT be linked. CI SHALL run an automated license audit of the dependency manifest and fail on violations. Third-party attributions SHALL ship in the About panel and packages.

The audit's subject SHALL be **what is compiled into a shipped binary**, not what a manifest happens to list: a dependency vendored outside the manifest's directory, or fetched at configure time, is in scope exactly like a checked-in one. The audit SHALL fail when anything linked into a shipped binary is missing from the attribution file, and each entry's licence SHALL be recorded from that dependency's own licence text rather than from its reputation.

#### Scenario: License gate
- **WHEN** a dependency with a GPL license is added to the manifest
- **THEN** the CI license audit SHALL fail naming the dependency

#### Scenario: A vendored tree outside the manifest is still audited
- **WHEN** sources are vendored or fetched into a location the manifest does not cover and are linked into a shipped binary — as AutoRemesher (MIT), the Geogram subset (BSD-3-Clause) and Eigen (MPL-2.0) were
- **THEN** the audit SHALL see them, SHALL fail if their licences are not permissive, and SHALL fail if they are absent from the third-party notices that ship with each artifact

### Requirement: Test gates
CI SHALL gate every merge on: unit tests (core modules), mesh-kernel property tests, stroke-recognizer trace tests, backend parity tests (on Metal and CUDA hardware lanes), and the golden-mesh regression suite (recorded baselines for quad count, non-quad count, singularity count, and Hausdorff distance with tolerances on a permissively-licensed corpus).

The **release** lane SHALL be gated too: no artifact SHALL be published from a tree whose suite has not been run in the release job itself, and the platform legs SHALL test the configuration they ship — including the field solver, so a release cannot silently carry a different quadrangulator than the one CI measured. A scheduled hardening lane SHALL re-run the suite under AddressSanitizer/UndefinedBehaviorSanitizer (in each quadrangulator configuration the project ships) and ThreadSanitizer, and SHALL run the fuzz harnesses; the fuzz harnesses and a seed corpus SHALL be checked in, and replaying that corpus SHALL run as an ordinary test case on every CI leg so the harnesses cannot rot between scheduled runs.

#### Scenario: Regression drift fails CI
- **WHEN** a change alters a golden mesh's quad count beyond tolerance
- **THEN** CI SHALL fail showing baseline vs. observed metrics

#### Scenario: Untested artifacts are never published
- **WHEN** the release workflow runs
- **THEN** the packaging jobs SHALL depend on a job that runs the test suite, and a failing suite SHALL block every artifact

#### Scenario: Sanitizer and fuzz coverage is scheduled, and its inputs are kept
- **WHEN** the scheduled hardening lane runs
- **THEN** it SHALL execute the suite under ASan/UBSan and TSan and run the fuzz targets, and the seed corpus checked in alongside them SHALL replay as a test case on ordinary CI legs

### Requirement: Platform packages
CI SHALL produce installable artifacts for macOS (signed/notarized DMG), Windows (zip and installer), Linux (AppImage), iPadOS/iOS (archive for TestFlight/App Store lanes), and Android (APK/AAB), with artifact names carrying the semantic version. Tagged releases SHALL publish a GitHub Release with the artifacts attached (AutoRemesher only uploaded CI artifacts).

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

### Requirement: Style and static analysis gates
CI SHALL enforce clang-format on project code for every PR, with the formatter version pinned so the gate is reproducible; violations fail the job with the diff visible. The project SHALL additionally ship a clang-tidy configuration for editors and local runs. The documentation SHALL state which of these gates a merge and which advise: a claimed gate that does not exist is worse than an acknowledged absence, and the packaging test suite SHALL check that claim against the workflows.

#### Scenario: Unformatted PR fails
- **WHEN** a PR contains formatting violations
- **THEN** the format job SHALL fail showing the required diff

#### Scenario: Documented gates match the workflows
- **WHEN** the README describes a CI gate
- **THEN** a test SHALL verify that a workflow implements it, and SHALL fail when the documentation claims an analysis gate the workflows do not run
