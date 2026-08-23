# build-and-packaging Specification

## Purpose
TBD - created by archiving change bootstrap-v1-platform. Update Purpose after archive.
## Requirements
### Requirement: C++20 CMake build
The project SHALL build with CMake (presets for every platform/backend combination) as strict C++20 with warnings-as-errors on the project's own code. The core engine SHALL compile with no GUI, GPU, or platform SDK present (CPU-only headless configuration).

Objective-C++ sources SHALL be declared as such with `enable_language(OBJCXX)`
and compiled with ARC. Adding a `.mm` file to a target without declaring the
language leaves CMake compiling it with the plain C++ driver, where every
`new…`/`alloc` result leaks because nothing owns it.

The project's own code SHALL NOT depend on standard-library facilities that a
shipping target's standard library does not provide. libc++ — the standard
library of both Apple platforms and the Android NDK — declares the
floating-point `std::from_chars` overloads deleted, so code that needs them
SHALL carry an equivalent path with the same grammar, and that path SHALL be
compiled and tested on every platform rather than only on the ones that take it.

#### Scenario: Minimal configuration builds
- **WHEN** the CPU-only headless preset is configured on a clean Linux container
- **THEN** the core engine, CLI, and unit tests SHALL build and pass without any GPU SDK installed

#### Scenario: The CLI builds against libc++
- **WHEN** the CLI is compiled for a mobile target, whose standard library is libc++
- **THEN** it SHALL compile, and its numeric flags SHALL accept and reject exactly the values they do on libstdc++

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
CI SHALL enforce clang-format on project code for every PR, with the formatter version pinned so the gate is reproducible; violations fail the job with the diff visible. The project SHALL additionally ship a clang-tidy configuration for editors and local runs. The documentation SHALL state which of these gates a merge and which advise: a claimed gate that does not exist is worse than an acknowledged absence, and the packaging test suite SHALL check that claim against the workflows.

#### Scenario: Unformatted PR fails
- **WHEN** a PR contains formatting violations
- **THEN** the format job SHALL fail showing the required diff

#### Scenario: Documented gates match the workflows
- **WHEN** the README describes a CI gate
- **THEN** a test SHALL verify that a workflow implements it, and SHALL fail when the documentation claims an analysis gate the workflows do not run

### Requirement: Every shipped preset configures
Each preset the project publishes SHALL configure successfully on a host that
has the toolchain that preset names, with no other setting supplied. A preset
SHALL NOT require an SDK the target platform does not have: the Android NDK
ships no OpenCL, so the Android preset SHALL leave the OpenCL backend off.

A preset's `displayName` SHALL describe only what it actually enables. Naming a
backend the preset does not turn on is a defect, because the preset is the
project's own statement of what a platform build contains.

A `find_package` for an optional backend SHALL fail with a message that names
the reason and the way out, rather than with the generic "could not find"
that leaves a reader no way to tell a missing package from an impossible one.

#### Scenario: The Android preset configures against an NDK
- **WHEN** the Android preset is configured with the NDK toolchain file and nothing else
- **THEN** the configure SHALL succeed, and CI SHALL build the library, the CLI and the test suite from it

#### Scenario: An impossible backend explains itself
- **WHEN** a build enables a compute backend whose SDK is absent
- **THEN** the configure SHALL fail with a message naming the missing package and the option that disables the backend

### Requirement: The shipped ISA is executed in CI
CI SHALL build the library, the CLI and the test suite for 64-bit ARM and RUN
the suite there, not merely compile it. x86-64 lanes cannot observe a defect
that depends on the mobile targets' word size, alignment, `char` signedness or
floating-point contraction, and the shipping targets are all 64-bit ARM.

The lane MAY use a cross-compiler and a user-mode instruction emulator rather
than a device; it SHALL NOT require one, so that it can gate every change.

#### Scenario: An architecture-dependent result is caught before release
- **WHEN** a change makes a result differ between x86-64 and aarch64
- **THEN** the arm64 lane SHALL fail, naming the case, on the change that introduced it

### Requirement: No source file is beyond every lane's reach
Every source file the project ships SHALL be compiled by at least one lane that
BLOCKS a merge. Where a file needs a toolchain CI cannot be gated on — the
Metal backend's Objective-C++, which needs Apple's — the project SHALL provide
a gate that runs everywhere: parsing the file against declaration-only stub
headers that mirror the SDK's signatures exactly.

Such a gate SHALL be incapable of passing vacuously. It SHALL verify on every
run that a deliberately mistyped selector is REJECTED, so a stub that grew too
permissive, or a compiler that stopped diagnosing, is reported rather than
silently turning the gate into a rubber stamp. The gate SHALL state what it
does not prove — linking, kernel compilation and results still need the real
SDK and real hardware.

#### Scenario: A selector typo in the Metal backend is caught off Apple
- **WHEN** a method call in the Metal backend is misspelled, or its argument types stop matching the SDK
- **THEN** the syntax gate SHALL fail on every platform, without an Apple toolchain

#### Scenario: The gate proves itself on every run
- **WHEN** the syntax gate runs
- **THEN** it SHALL also compile a deliberately mutated copy and SHALL fail if that copy is accepted

