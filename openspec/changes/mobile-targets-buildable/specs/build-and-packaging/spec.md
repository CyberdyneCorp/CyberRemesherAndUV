# build-and-packaging — mobile targets, arm64 execution, Objective-C++ coverage

Delta for `mobile-targets-buildable`.

## ADDED Requirements

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

## MODIFIED Requirements

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
