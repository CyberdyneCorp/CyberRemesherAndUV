# engine-bindings Specification

## Purpose
TBD - created by archiving change bootstrap-v1-platform. Update Purpose after archive.
## Requirements
### Requirement: Full-surface C ABI facade
The library SHALL be exposed through a versioned C ABI (opaque handles, plain C types, integer error codes, C function-pointer callbacks) covering the **entire library surface**, not only the headless pipeline: mesh I/O and inspection, the remeshing pipeline with canonical parameters, the document/session layer (create/open/save documents, Target/EditMesh access, stage switching), the tool command layer (invoke any retopo/UV/bake action, inject synthetic input — stroke point sequences, taps, modifier chords), undo/redo, UV unwrap/pack, baking, diagnostics, and compute-backend selection. No C++ types SHALL cross the boundary. The ABI SHALL carry a runtime-queryable semantic version; minor releases SHALL be additive only.

#### Scenario: Interactive tool drivable without a UI
- **WHEN** a C ABI client creates a document, loads a Target, and injects a closed-quad stroke sequence into the Pencil action
- **THEN** the EditMesh SHALL contain the created face exactly as if the stroke had come from a touchscreen

#### Scenario: ABI version query
- **WHEN** a client compiled against ABI 1.x loads a 1.y (y > x) library
- **THEN** all 1.x entry points SHALL work unchanged

### Requirement: Python bindings cover every capability for desktop testing
The project SHALL ship a pip-installable Python package (CPython 3.10+; wheels for macOS arm64/x86_64, Windows x86_64, Linux x86_64/aarch64) wrapping the full C ABI so that **every library capability is exercisable from Python on a desktop machine**: remeshing, I/O, UV, bake, document lifecycle, undo/redo, stroke/gesture injection into every tool, mesh state inspection (NumPy-compatible vertex/face/attribute views), and backend selection. Exceptions map from error codes; long calls release the GIL and accept progress callables and cancellation tokens.

#### Scenario: Scripted retopo session
- **WHEN** a Python test loads a Target, injects strokes to build faces, runs Relax, tweaks a vertex, undoes twice, and reads back the EditMesh arrays
- **THEN** every step SHALL behave identically to the same operations performed in the app, and the arrays SHALL reflect the exact resulting topology

#### Scenario: Full-capability coverage gate
- **WHEN** the set of tool actions and engine operations is compared against the Python API surface in CI
- **THEN** any library capability not reachable from Python SHALL fail the build (explicitly registered pending items excepted)

### Requirement: Python is the integration-test harness
The project's integration and interaction test suites (stroke-grammar traces, golden-mesh pipeline runs, undo/document invariants, backend parity orchestration) SHALL be written against the Python bindings and run on desktop CI, so the bindings are exercised as a first-class product on every merge.

#### Scenario: Recorded trace replayed from Python
- **WHEN** a recorded stroke trace is replayed through the Python API in CI
- **THEN** the resulting EditMesh SHALL match the recorded expected topology

### Requirement: Swift package is the supported path to the library on iPad
The project SHALL ship a Swift package (SwiftPM; iPadOS/iOS and macOS) wrapping the same C ABI with idiomatic Swift — typed `throws` errors, value-type parameters, async/await for long operations with progress, Task-cancellation bridging — sufficient to build a complete iPad experience on top of it: document/session control, all tool actions, forwarding of UIKit/PencilKit touch and stylus events into the input layer, viewport attachment to a caller-supplied `CAMetalLayer`, and export/bake. The project's own iPadOS shell SHALL consume this package (not private hooks), guaranteeing third parties get the same capability surface.

#### Scenario: Third-party iPad app hosts the library
- **WHEN** an external iPad app adds the Swift package, attaches a Metal layer, loads a Target, and forwards Apple Pencil events
- **THEN** stroke-based retopology SHALL function inside that app with the same behavior as the first-party shell

#### Scenario: Swift task cancellation
- **WHEN** a remesh launched via the Swift async API has its enclosing Task cancelled
- **THEN** the engine SHALL cancel cooperatively and the call SHALL throw the cancellation error

### Requirement: Binding parity and release discipline
Python and Swift bindings SHALL be version-locked to the engine release and covered in CI on every supported platform lane (Python: desktop OSes; Swift: macOS + iOS simulator). New ABI entry points SHALL fail CI until both bindings expose them or a pending registration exists.

#### Scenario: Surface drift is caught
- **WHEN** an ABI entry point is added without a matching Python or Swift wrapper
- **THEN** the parity check SHALL fail naming the missing wrapper

### Requirement: Automatic UV atlas binding
The C ABI and Python bindings SHALL expose the automatic UV atlas so a caller can, in one call, generate a packed UV atlas for a mesh and read back its quality. The C symbols SHALL always be declared (stable ABI) even in a build without the UV module, returning a runtime error there rather than being absent.

#### Scenario: One-call atlas from Python
- **WHEN** a caller invokes `Mesh.unwrap_atlas` (C: `cyber_uv_atlas`) on a loaded mesh
- **THEN** the binding SHALL seam, unwrap, re-orient, and pack the mesh in place and return an atlas result — chart count, seam-edge count, max/RMS conformal distortion, flipped- and fallback-chart counts, packed area, and texel density — and a subsequent OBJ save SHALL emit the per-corner UVs (`vt` / `f v/vt`)

#### Scenario: Atlas parameters mirror the engine options
- **WHEN** a caller supplies atlas parameters (C: `CyberAtlasParams` / Python: `AtlasParams`)
- **THEN** the binding SHALL honour the chart-angle bound, the chart-merge toggle and distortion cap, the re-orientation toggle, and the pack margin / texture size, defaulting them from the engine's own defaults via `cyber_default_atlas_params`

#### Scenario: ABI stable without the UV module
- **WHEN** the engine is built without the UV module
- **THEN** `cyber_uv_atlas` SHALL still be a declared, linkable symbol that returns a runtime error status (not a missing symbol), so binaries built against the header keep a stable ABI

