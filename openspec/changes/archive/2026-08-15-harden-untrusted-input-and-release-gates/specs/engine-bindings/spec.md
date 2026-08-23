# engine-bindings (delta)

## MODIFIED Requirements

### Requirement: Full-surface C ABI facade
The library SHALL be exposed through a versioned C ABI (opaque handles, plain C types, integer error codes, C function-pointer callbacks) covering the **entire library surface**, not only the headless pipeline: mesh I/O and inspection, the remeshing pipeline with canonical parameters, the document/session layer (create/open/save documents, Target/EditMesh access, stage switching), the tool command layer (invoke any retopo/UV/bake action, inject synthetic input — stroke point sequences, taps, modifier chords), undo/redo, UV unwrap/pack, baking, diagnostics, and compute-backend selection. No C++ types SHALL cross the boundary. The ABI SHALL carry a runtime-queryable semantic version; minor releases SHALL be additive only.

The shared library's **exported** surface SHALL be exactly that ABI. Symbols from vendored third-party code linked in from static archives SHALL NOT be exported, because the library is meant to run inside host processes (DCCs) that carry their own copies of the same third-party code and would otherwise interpose on ours, or be interposed on by it. Every shipping platform SHALL have an export policy stated in the build files — a linker version script on ELF, an exported-symbols list on Mach-O, and an explicit policy on Windows, where a library that exports nothing produces no import library and cannot be linked at all. No error SHALL propagate out of an entry point as a C++ exception; every failure SHALL leave the boundary as a status code.

#### Scenario: Interactive tool drivable without a UI
- **WHEN** a C ABI client creates a document, loads a Target, and injects a closed-quad stroke sequence into the Pencil action
- **THEN** the EditMesh SHALL contain the created face exactly as if the stroke had come from a touchscreen

#### Scenario: ABI version query
- **WHEN** a client compiled against ABI 1.x loads a 1.y (y > x) library
- **THEN** all 1.x entry points SHALL work unchanged

#### Scenario: Only the ABI is exported
- **WHEN** the exported symbols of the shipped shared library are inspected on any platform
- **THEN** they SHALL consist of the `cyber_*` entry points, the vendored third-party definitions SHALL NOT appear, and the library SHALL still produce whatever import artifact its platform's consumers link against

#### Scenario: No exception crosses the boundary
- **WHEN** an entry point's implementation throws — a parser type error, an allocation failure, or a solver's own exception
- **THEN** the call SHALL return a status code with a retrievable message rather than unwinding into the caller's frame

### Requirement: Binding parity and release discipline
Python and Swift bindings SHALL be version-locked to the engine release and covered in CI on every supported platform lane (Python: desktop OSes; Swift: macOS + iOS simulator). New ABI entry points SHALL fail CI until both bindings expose them or a pending registration exists.

The gate SHALL be a runnable check in the test suite, not a convention: `tests/packaging/test_swift_abi_parity.py` (ctest case `swift_abi_parity`) fails when a Swift source references a C symbol the header does not declare or declares with a different arity, and per-surface parity checks in the Python test suite fail when a header symbol is added without being bound. A capability the ABI exposes but the bindings do not SHALL be recorded as a pending registration, so the gap is visible rather than assumed absent.

#### Scenario: Surface drift is caught
- **WHEN** an ABI entry point is added without a matching Python or Swift wrapper
- **THEN** the parity check SHALL fail naming the missing wrapper

#### Scenario: The gate is executable
- **WHEN** the test suite runs on any platform, including those without a Swift toolchain
- **THEN** the source-level parity gate SHALL run as an ordinary test case and fail on a symbol mismatch
