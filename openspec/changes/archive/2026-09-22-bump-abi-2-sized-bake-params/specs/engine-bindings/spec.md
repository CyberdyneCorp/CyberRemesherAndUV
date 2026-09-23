## MODIFIED Requirements

### Requirement: Full-surface C ABI facade
The library SHALL be exposed through a versioned C ABI (opaque handles, plain C types, integer error codes, C function-pointer callbacks) covering the **entire library surface**, not only the headless pipeline: mesh I/O and inspection, the remeshing pipeline with canonical parameters, the document/session layer (create/open/save documents, Target/EditMesh access, stage switching), the tool command layer (invoke any retopo/UV/bake action, inject synthetic input — stroke point sequences, taps, modifier chords), undo/redo, UV unwrap/pack, baking, diagnostics, and compute-backend selection. No C++ types SHALL cross the boundary. The ABI SHALL carry a runtime-queryable semantic version; minor releases SHALL be additive only, and any change that is not additive SHALL increment the major, which the shared library's soname SHALL carry. A client compiled against a different major SHALL be refused loudly — by the loader and by `cyber_abi_check` — rather than served.

The shared library's **exported** surface SHALL be exactly that ABI. Symbols from vendored third-party code linked in from static archives SHALL NOT be exported, because the library is meant to run inside host processes (DCCs) that carry their own copies of the same third-party code and would otherwise interpose on ours, or be interposed on by it. Every shipping platform SHALL have an export policy stated in the build files — a linker version script on ELF, an exported-symbols list on Mach-O, and an explicit policy on Windows, where a library that exports nothing produces no import library and cannot be linked at all. No error SHALL propagate out of an entry point as a C++ exception; every failure SHALL leave the boundary as a status code.

#### Scenario: Interactive tool drivable without a UI
- **WHEN** a C ABI client creates a document, loads a Target, and injects a closed-quad stroke sequence into the Pencil action
- **THEN** the EditMesh SHALL contain the created face exactly as if the stroke had come from a touchscreen

#### Scenario: ABI version query
- **WHEN** a client compiled against ABI N.x loads an N.y (y > x) library
- **THEN** all N.x entry points SHALL work unchanged

#### Scenario: A client of an older major is refused, not served
- **WHEN** a client compiled against ABI 1.x asks a 2.y library whether it can be served
- **THEN** `cyber_abi_check` SHALL return `CYBER_ERR_INCOMPATIBLE_VERSION` naming both versions
- **AND** the shared library SHALL carry a different soname, so a binary linked against the 1.x library does not load it at all

#### Scenario: Only the ABI is exported
- **WHEN** the exported symbols of the shipped shared library are inspected on any platform
- **THEN** they SHALL consist of the `cyber_*` entry points, the vendored third-party definitions SHALL NOT appear, and the library SHALL still produce whatever import artifact its platform's consumers link against

#### Scenario: No exception crosses the boundary
- **WHEN** an entry point's implementation throws — a parser type error, an allocation failure, or a solver's own exception
- **THEN** the call SHALL return a status code with a retrievable message rather than unwinding into the caller's frame

## ADDED Requirements

### Requirement: Sized parameter structs
`CyberBakeParams` and `CyberBundleParams` SHALL carry `size_t structSize` as their first member, set by the caller to `sizeof` the struct as its header declares it, under the same rule as the bake-provider descriptors: they are passed one at a time by pointer and never as an array, and the library SHALL read an input member, and `cyber_default_*_params` SHALL write one, only where the caller's stated size covers it. A member the stated size does not cover SHALL take its ENGINE DEFAULT, not zero. Appending a member to one of these structs is therefore additive and SHALL NOT increment the major.

A stated size below the ABI 2.0 layout SHALL be refused with `CYBER_ERR_INVALID_ARG` naming both sizes, and that floor SHALL NOT move when a member is appended later. A stated size larger than this build's layout SHALL be served, reading only the members this build knows. `cyber_default_bake_params` and `cyber_default_bundle_params` SHALL return a status: a `NULL` struct or a size below the floor is refused and the struct left untouched, never silently skipped.

#### Scenario: The defaults are written only as far as the caller's struct reaches
- **WHEN** a caller sets `structSize` and calls `cyber_default_bake_params`
- **THEN** every member its size covers SHALL hold the engine default
- **AND** no byte past `structSize` SHALL be written

#### Scenario: An unset size is refused, not guessed
- **WHEN** a caller calls `cyber_default_bake_params` with `structSize` below the 2.0 layout
- **THEN** it SHALL return `CYBER_ERR_INVALID_ARG` naming the stated and minimum sizes
- **AND** the struct SHALL be left exactly as it was

#### Scenario: A newer caller's larger struct is served
- **WHEN** a caller compiled against a later 2.x header passes a struct larger than this build's layout
- **THEN** the bake SHALL run with the members this build knows
- **AND** the trailing members SHALL be neither read as parameters nor written

#### Scenario: The bindings set the size themselves
- **WHEN** the Python or Swift binding bakes or writes a bundle
- **THEN** it SHALL set `structSize` itself, so neither binding's public API exposes it
