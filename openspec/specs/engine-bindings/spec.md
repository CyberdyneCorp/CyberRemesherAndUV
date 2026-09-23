# engine-bindings Specification

## Purpose
The exported surface of the engine and the discipline that keeps it honest: a
C ABI facade covering the whole capability set, Python bindings that reach all
of it, and a Swift package for iPad. It exists because the bindings are not a
convenience layer — Python is the integration-test harness the suite actually
runs, so a capability that is unreachable from a binding is untested, and
parity between the CLI, C and Python surfaces is a release rule rather than an
aspiration.
## Requirements
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

For iOS consumers, the supported package distribution SHALL resolve its native C ABI from a versioned XCFramework rather than requiring repository-relative unsafe include or linker flags. It SHALL support both device and simulator builds under the same public Swift API and SHALL document the CPU solver profile and optional-solver availability of the distributed artifact.

Each Swift remesh operation SHALL represent exactly one native execution. It SHALL expose idempotent explicit cancellation, borrow and retain its input until that execution has reached a terminal state, and document that callers MUST NOT mutate that input while the job is active. It SHALL give every concurrent or subsequent awaiter the same terminal result or error. Task cancellation while awaiting SHALL request cancellation of that shared job without abandoning its native worker. Progress delivery SHALL be monotonic, serialized for consumers, and finish exactly once; callbacks SHALL never outlive the operation's retained control state. A successful result SHALL be independently owned by the caller, while cancellation and failure SHALL not mutate the input.

#### Scenario: Third-party iPad app hosts the library
- **WHEN** an external iPad app adds the Swift package, attaches a Metal layer, loads a Target, and forwards Apple Pencil events
- **THEN** stroke-based retopology SHALL function inside that app with the same behavior as the first-party shell

#### Scenario: Swift task cancellation
- **WHEN** a remesh launched via the Swift async API has its enclosing Task cancelled
- **THEN** the engine SHALL cancel cooperatively and the call SHALL throw the cancellation error

#### Scenario: Explicit job cancellation and repeated awaits
- **WHEN** a caller cancels a running `RemeshOperation` and awaits its value from more than one task
- **THEN** the engine SHALL receive one cooperative cancellation request, execute at most once, and every awaiter SHALL observe the same cancelled terminal error

#### Scenario: Packaged iOS host builds without source paths
- **WHEN** a third-party iOS app adds the binary-backed package from a clean checkout
- **THEN** it SHALL compile the public Swift API without configuring a path to `capi/include` or a locally built native library

### Requirement: Binding parity and release discipline
Python and Swift bindings SHALL be version-locked to the engine release and covered in CI on every supported platform lane (Python: desktop OSes; Swift: macOS + iOS simulator). New ABI entry points SHALL fail CI until both bindings expose them or a pending registration exists.

The gate SHALL be a runnable check in the test suite, not a convention: `tests/packaging/test_swift_abi_parity.py` (ctest case `swift_abi_parity`) fails when a Swift source references a C symbol the header does not declare or declares with a different arity, and per-surface parity checks in the Python test suite fail when a header symbol is added without being bound. A capability the ABI exposes but the bindings do not SHALL be recorded as a pending registration, so the gap is visible rather than assumed absent.

The gate SHALL run in BOTH directions. Checking only that a binding references nothing absent from the header cannot detect an entry point no binding reaches, which is the drift that occurred: a mobile host held the complete stroke grammar and none of the operations that apply a recognized gesture. Every declared `cyber_*` entry point SHALL therefore be bound in each binding or listed in that binding's checked-in pending-registration list, and an unlisted, unbound entry point SHALL fail CI.

The header -> binding direction SHALL be one shared check applied to every binding, not a per-binding copy, so the checks cannot drift apart the way the bindings did. It SHALL run without a built engine, so it covers the lanes where the library does not load. A symbol named only in a comment or string literal SHALL NOT count as bound.

A pending registration SHALL be rejected when the header no longer declares the entry point, and SHALL be rejected when the binding already binds it, so the list can be read as an accurate statement of what is missing without being re-checked.

A host embedding the library on a mobile platform SHALL be able to reach the retopology surface end to end through Swift: Target snapping, face and strip construction, contours, boundary fill, surface cut, patch clone, loop operations and guided remeshing — and SHALL be able to finish an asset: UV unwrapping and baking. The desktop Python binding SHALL reach the same drawing surface, including stroke interpretation, so the gesture path has regression coverage on the harness that can run it.

#### Scenario: Surface drift is caught
- **WHEN** an ABI entry point is added without a matching Python or Swift wrapper
- **THEN** the parity check SHALL fail naming the missing wrapper

#### Scenario: The gate is executable
- **WHEN** the test suite runs on any platform, including those without a Swift toolchain
- **THEN** the source-level parity gate SHALL run as an ordinary test case and fail on a symbol mismatch

#### Scenario: An unbound entry point is not silently tolerated
- **WHEN** a declared `cyber_*` entry point is bound by neither the Swift sources nor that binding's pending-registration list
- **THEN** the parity gate SHALL fail naming that entry point

#### Scenario: A mobile host can apply a recognized gesture
- **WHEN** a Swift host interprets a stroke as a build action and applies it
- **THEN** the corresponding retopology entry point and a Target snapper SHALL both be reachable from Swift

#### Scenario: Python has the same gate
- **WHEN** a declared entry point is bound by no Python source and not registered as pending
- **THEN** the Python parity gate SHALL fail naming it, without needing the engine library to load

#### Scenario: A registration for a bound entry point is rejected
- **WHEN** a binding's pending-registration list names an entry point that binding already binds
- **THEN** the gate SHALL fail naming it, so the list stays an accurate statement of what is missing

#### Scenario: One binding runs the whole workflow
- **WHEN** a Swift host snaps to a Target, builds a contour tube, unwraps it and bakes a normal map
- **THEN** every step SHALL be reachable from Swift without calling the C ABI directly

### Requirement: Automatic UV atlas binding
The C ABI and Python bindings SHALL expose the automatic UV atlas so a caller can, in one call, generate a packed UV atlas for a mesh and read back its quality. The C symbols SHALL always be declared (stable ABI) even in a build without the UV module, returning a runtime error there rather than being absent.

#### Scenario: One-call atlas from Python
- **WHEN** a caller invokes `Mesh.unwrap_atlas` (C: `cyber_uv_atlas`) on a loaded mesh
- **THEN** the binding SHALL seam, unwrap, re-orient, and pack the mesh in place and return an atlas result — chart count, dropped-chart count, seam-edge count, max/RMS conformal distortion, flipped- and fallback-chart counts, packed area (geometry coverage), packed bounding-box area, and texel density — and a subsequent OBJ save SHALL emit the per-corner UVs (`vt` / `f v/vt`)

#### Scenario: Atlas parameters mirror the engine options
- **WHEN** a caller supplies atlas parameters (C: `CyberAtlasParams` / Python: `AtlasParams`)
- **THEN** the binding SHALL honour the chart-angle bound, the chart-merge toggle and distortion cap, the re-orientation toggle, and the pack margin / texture size, defaulting them from the engine's own defaults via `cyber_default_atlas_params`

#### Scenario: ABI stable without the UV module
- **WHEN** the engine is built without the UV module
- **THEN** `cyber_uv_atlas` SHALL still be a declared, linkable symbol that returns a runtime error status (not a missing symbol), so binaries built against the header keep a stable ABI

#### Scenario: The atlas is cancellable from a host
- **WHEN** a host calls `cyber_uv_atlas_cancellable` with progress and cancel callbacks and its cancel callback returns non-zero
- **THEN** the call SHALL return `CYBER_ERR_CANCELLED` and leave the mesh exactly as it was, and the header SHALL document that the default chart-merge cap is the expensive part of the call so a host knows why it needs the cancellable entry point

### Requirement: Mesh state duplication, write-back and element resolution
The C ABI and Python bindings SHALL let a caller duplicate a mesh in memory, write vertex positions back, and resolve an element id to geometry, so before/after comparison, undo snapshots and overlay rendering are possible without leaving the process. A duplicate SHALL be lossless (no serialization round trip) and SHALL preserve element ids. Position write-back SHALL use the same compacted vertex order the position reader returns and SHALL reject a count that does not match the vertex count.

#### Scenario: Lossless in-memory duplicate
- **WHEN** a caller duplicates a mesh (C: `cyber_mesh_clone` / Python: `Mesh.copy`) and compares the copy's positions against the original
- **THEN** the positions SHALL be bit-identical, the copy SHALL carry the handle's statistics, hidden-face and tagged-edge overlays, soft-selection weight field and saved selection slots, element ids SHALL address the same elements in both, and editing either mesh SHALL leave the other unchanged

#### Scenario: Snapshot restored exactly
- **WHEN** a caller reads positions, edits the mesh, and writes the saved positions back (C: `cyber_mesh_set_positions` / Python: `Mesh.set_positions` or the `Mesh.positions` setter)
- **THEN** the mesh SHALL return to bit-identical positions with topology, ids and overlays untouched, and a write whose float count is not three times the vertex count SHALL be rejected with an invalid-argument error leaving the mesh unchanged

#### Scenario: A committed seam is drawable
- **WHEN** a caller commits a seam path and asks for the endpoints and positions of each committed edge id (C: `cyber_mesh_edge_endpoints` / `cyber_mesh_vertex_position`; Python: `Mesh.edge_endpoints` / `Mesh.vertex_position`)
- **THEN** each live edge id SHALL resolve to its two vertex ids and each live vertex id to its position, and an id that is not alive SHALL report absence rather than a fabricated result

#### Scenario: A fixed-size element query returns a safe loop bound
- **WHEN** a caller asks for the faces adjacent to a non-manifold edge (3+ faces, which the engine supports and tags) through an entry point whose out arrays are of declared fixed size (C: `cyber_mesh_edge_faces`)
- **THEN** the returned count SHALL be the number of entries WRITTEN, never larger than the declared array size, so the return value is always a safe bound for a loop over those arrays, and the element's true, unclamped count SHALL be reachable through a separate query (C: `cyber_mesh_edge_face_count`)

### Requirement: Weighted-edit reports count distinct vertices
The weighted transform and weighted relax reports (C: `CyberSoftTransformReport` / Python: `SoftTransformReport`) SHALL report DISTINCT vertices in every field, never per-iteration writes, so a count can never exceed the mesh's vertex count. The C ABI header, the Python docstring and the Swift wrapper SHALL state the same meaning.

#### Scenario: A multi-iteration relax stays inside the mesh
- **WHEN** a caller runs a weighted relax for N iterations over a selected region
- **THEN** the reported moved count SHALL equal the number of distinct vertices written — independent of N — SHALL not exceed the mesh's vertex count, and the reported re-snapped count SHALL not exceed it

### Requirement: In-memory sculpt handoff is reachable from the bindings
Both documented sculpt-handoff profiles — the file profile and the in-memory buffer profile — SHALL have a binding surface. The buffer profile SHALL apply the SAME version gate as the file profile, so an in-process producer cannot bypass it, and SHALL reject an optional payload whose length does not match the vertex count rather than reading past its end.

#### Scenario: A handoff arrives as plain arrays
- **WHEN** a caller passes positions, triangle indices and the optional normal / colour / `material_mix` payloads (C: `cyber_handoff_open_buffers` / Python: `Mesh.load_handoff_buffers`)
- **THEN** the binding SHALL produce a Target mesh and the same declared-payload report the file profile returns, with no intermediate file written

#### Scenario: The version gate holds through memory
- **WHEN** an in-memory handoff declares a version this engine does not support
- **THEN** the call SHALL fail with the typed incompatible-version error naming both versions and SHALL produce no partial Target

### Requirement: Named export presets and bundles have a binding surface
The C ABI and Python bindings SHALL expose named export presets: listing the built-ins, resolving one by name or by file path, reading everything it declares, and writing its bundle for a low/high mesh pair. The preset DATA half SHALL be available in every configuration, since presets live in core; only the bundle WRITER may depend on the UV module, and where it is absent the symbol SHALL still be declared and return a runtime error rather than being missing.

#### Scenario: Presets are listed, resolved and read
- **WHEN** a caller lists the built-ins and resolves one (C: `cyber_export_preset_builtin_name` / `cyber_export_preset_resolve`; Python: `builtin_presets` / `ExportPreset.resolve`)
- **THEN** the binding SHALL report the preset's name, schema version, mesh and texture container, naming pattern, units, up axis, resolution, normal-map green-channel convention, and its map list with each map's colour space and `{map}` substitution token, and SHALL expand the naming pattern for a given basename so a caller never re-implements the token rules

#### Scenario: A preset from an unsupported schema is refused loudly
- **WHEN** a caller resolves a preset file declaring a schema version this engine does not support
- **THEN** the call SHALL fail with the typed incompatible-version error (Python: `IncompatibleVersionError`) naming both versions and SHALL produce no partially honoured preset; a name that is neither built in nor a readable file SHALL fail with an invalid-argument error whose message lists the built-ins

#### Scenario: A bundle is written from the bindings
- **WHEN** a caller writes a preset bundle for a low/high mesh pair (C: `cyber_export_bundle_write` / Python: `write_bundle`)
- **THEN** the binding SHALL write the mesh plus one baked map per preset entry and report every file with its kind, colour space and pixel size, plus whether the low-poly had to be unwrapped and that unwrap's chart count and distortion, and SHALL surface a preset/extension mismatch as a warning rather than resolving it silently

#### Scenario: ABI stable without the export-bundle module
- **WHEN** the engine is built without the UV module (and therefore without the export-bundle writer)
- **THEN** listing, resolving and reading presets SHALL still work, and `cyber_export_bundle_write` SHALL remain a declared, linkable symbol that returns a runtime error status naming the missing module

### Requirement: Format-agnostic mesh load and save binding
The C ABI SHALL expose `cyber_mesh_load` and `cyber_mesh_save`, which dispatch
on the file extension across every format the engine supports, and the Python
binding SHALL expose them as `Mesh.load` / `Mesh.save`. The pre-existing
`cyber_mesh_load_obj` / `cyber_mesh_save_obj` entry points (and their
`Mesh.load_obj` / `Mesh.save_obj` wrappers) SHALL remain as aliases with
identical behaviour, so callers written against them keep working.

#### Scenario: One call loads any supported format
- **WHEN** a caller invokes `Mesh.load` on an OBJ, PLY, STL, glTF, GLB, or FBX file
- **THEN** the mesh SHALL load through the same entry point, and an unsupported extension SHALL raise the typed unsupported-format error naming the path

#### Scenario: The `_obj` aliases keep working
- **WHEN** existing code calls `Mesh.load_obj` / `cyber_mesh_load_obj`
- **THEN** it SHALL behave exactly as `Mesh.load` / `cyber_mesh_load` does

### Requirement: Subdivision binding
The Python binding SHALL expose subdivision over the C ABI as `Mesh.subdivide`,
splitting every n-gon into n quads (Catmull-Clark topology) in place and
returning the resulting face count. It SHALL accept a mode selecting LINEAR
(Catmull-Clark topology, no smoothing) or CATMULL-CLARK (the smooth rules), with
LINEAR as the default so that existing calls keep their exact behaviour. It
SHALL accept an optional projection target; when given, every vertex of the
subdivided mesh SHALL be projected onto that target's surface, which is what
recovers curvature that linear subdivision alone cannot add — and which remains
available in the smooth mode, where it is the more accurate answer whenever a
Target exists.

The C ABI SHALL carry the mode on a SIBLING entry point rather than by changing
the signature of the published mode-less one, so that already-compiled callers
keep working; the mode-less entry point SHALL be defined as the linear case of
the mode-taking one rather than a second implementation. An unrecognised mode
SHALL be rejected with the typed invalid-argument error, leaving the mesh
untouched.

#### Scenario: Subdividing quadruples a quad mesh
- **WHEN** `Mesh.subdivide()` is called on a mesh of N quads
- **THEN** the mesh SHALL afterwards hold 4N quads and the reported face count SHALL equal the mesh's face count

#### Scenario: Subdivide and reproject recovers curvature
- **WHEN** a coarse mesh is subdivided with a curved surface passed as the projection target
- **THEN** the new vertices SHALL lie on that surface rather than on the coarse mesh's flat facets

#### Scenario: Smooth subdivision needs no projection target
- **WHEN** `Mesh.subdivide()` is called in Catmull-Clark mode on a faceted cage with no projection target
- **THEN** the result SHALL be measurably rounder than the same mesh subdivided linearly, and calling it without naming a mode SHALL still produce the faceted linear result

#### Scenario: An unknown mode is refused
- **WHEN** `Mesh.subdivide()` is called with a mode value the engine does not define
- **THEN** it SHALL raise the typed invalid-argument error and the mesh SHALL be left exactly as it was

#### Scenario: Subdividing an empty mesh fails loudly
- **WHEN** `Mesh.subdivide()` is called on a mesh with no faces
- **THEN** it SHALL raise the typed empty-mesh error rather than silently doing nothing

### Requirement: Every Python binding test and the offline examples are registered with CTest
Each Python binding test file SHALL be registered as a CTest case against the
library built in the same tree, under the existing capability-gated convention
(`SKIP_RETURN_CODE 77` when the C ABI library cannot be loaded), so no binding
test can sit in the tree unrun. The Python examples that need neither network
nor downloaded models SHALL additionally be executed end-to-end by a registered
smoke test, so the example gallery cannot rot unnoticed.

#### Scenario: A new binding test cannot be forgotten
- **WHEN** a test file is added under the Python binding test directory
- **THEN** it SHALL be registered with CTest and run in the same lane as the existing binding tests

#### Scenario: A broken example is caught
- **WHEN** a change breaks an offline example script
- **THEN** the example smoke test SHALL fail, naming the failing script

### Requirement: Worker-thread cap on the C ABI
The C ABI SHALL expose the library's worker-thread cap: a setter that takes the
maximum number of workers (0 meaning uncapped) and a getter that reports the
current value. It SHALL be callable at any time from any thread, since a host
adjusts its thread budget between operations rather than once at startup.

A negative value SHALL be rejected as an invalid argument and SHALL leave the
current cap untouched, so a host never ends up with an unbounded engine because
it passed a bad number.

The documentation on the setter SHALL state that the cap changes speed and CPU
load only, never results, because that is what makes it safe for a host to move
mid-session.

#### Scenario: A host bounds the engine and reads the bound back
- **WHEN** a host sets the worker cap through the C ABI and then queries it
- **THEN** the query SHALL return the value that was set, and the active device's reported thread count SHALL reflect it

#### Scenario: A bad value changes nothing
- **WHEN** a negative cap is passed
- **THEN** the call SHALL return an invalid-argument status, set an error message, and leave the previous cap in force

### Requirement: Soft selection is reachable from every binding
The C ABI SHALL expose gradient selection (line, sphere, painted), the
selection operations (clear, invert, expand, contract, smooth, save, load),
and weighted transform/relax; the Python and Swift bindings SHALL reach the
same surface under the existing binding-parity rules.

#### Scenario: Parity across bindings
- **WHEN** the binding-parity check runs
- **THEN** every soft-selection capability reachable from Python SHALL be reachable from the C ABI and Swift package

### Requirement: Seam-driven unwrap binding
The C ABI and the Python binding SHALL expose an unwrap that takes a seam set
the caller built — by marking edges directly or by committing routed seam paths
— and parameterizes the mesh along it, writing the per-corner UV attribute in
place and reporting through the same result structure the automatic atlas uses.
A cancellable variant SHALL be provided on the same terms as the automatic
atlas, observing cancellation before any UV is written.

The binding SHALL also expose the sew operation over the same seam set.

#### Scenario: Routed seams reach an unwrap
- **WHEN** a caller commits a routed seam path into a seam set and then unwraps along that set through the binding
- **THEN** the mesh SHALL carry UVs cut at the committed path, with no C++ code required

#### Scenario: Null seam set is refused
- **WHEN** the seam-driven unwrap is called with a null seam set
- **THEN** it SHALL return the invalid-argument status and name the argument, rather than silently falling back to automatic seaming

#### Scenario: Cancelled before any write
- **WHEN** a caller cancels a seam-driven unwrap
- **THEN** the call SHALL report cancellation and the mesh SHALL be left exactly as it was

### Requirement: Retopology mesh-operation bindings
The Python binding SHALL expose the retopology mesh operations that need no
stroke geometry — `cyber_retopo_triangulate`, `_relax`, `_snap_all`,
`_delete_faces`, `_dissolve_edges`, `_insert_loop`, `_merge_vertices` and
`_rotate_edge` — as `Mesh` methods, so a cage built or subdivided from Python
can also be cleaned up, tightened and reprojected there. Each method SHALL
document the C header's element-id stability contract, because caller-side
annotations (pins, loop tags, hidden faces) are keyed on those ids. The batch
operations SHALL preserve the ABI's skip-don't-fail contract, and the
single-element operations SHALL surface the ABI's refusal as the typed error
with the mesh unchanged.

#### Scenario: Whole-mesh commands reach Python
- **WHEN** a caller invokes `Mesh.triangulate`, `Mesh.relax` or `Mesh.snap_all`
- **THEN** the operation SHALL run on the engine and report what the C entry point reports — the resulting face count, or the moved-vertex count and largest displacement

#### Scenario: Relax without a brush centre relaxes the whole mesh
- **WHEN** `Mesh.relax` is called with no centre
- **THEN** every vertex SHALL be relaxed regardless of any radius passed, because the ABI has no separate relax-all entry point and spells it as a non-positive radius

#### Scenario: Pinned vertices are immune
- **WHEN** a vertex id is passed in the `pinned` list of `Mesh.relax` or `Mesh.snap_all`
- **THEN** that vertex's position SHALL be unchanged by the call and SHALL NOT be counted as moved

#### Scenario: Batch operations skip what they cannot act on
- **WHEN** `Mesh.delete_faces` or `Mesh.dissolve_edges` is given ids that are dead, out of range, or ineligible
- **THEN** those ids SHALL be skipped rather than raising, and the returned count SHALL be how many elements were actually removed or dissolved

#### Scenario: A refused single-element operation leaves the mesh unchanged
- **WHEN** `Mesh.rotate_edge`, `Mesh.insert_loop` or `Mesh.merge_vertices` is given arguments the engine refuses
- **THEN** it SHALL raise the typed error carrying the engine's message, and the mesh's vertex and face counts SHALL be unchanged

### Requirement: Loop subdivision binding
The C ABI and the Python binding SHALL expose Loop subdivision over a triangle
mesh, splitting every triangle into four in place and reporting the resulting
face count. The smoothing mode SHALL be an explicit argument of the call, with
the smooth and linear meanings documented on the binding itself, so a caller who
asks for resolution without a shape change cannot receive smoothing by accident.

They SHALL accept the same optional projection target the quad subdivision
accepts; when given, every vertex of the refined mesh SHALL be projected onto
that target's surface.

A non-triangle input SHALL be reported through a status code distinct from the
invalid-argument and invalid-parameter codes, with a message naming the
offending face and its side count, and the binding SHALL raise a correspondingly
distinct exception type. The bindings SHALL also expose the triangulation the
refusal points callers at.

#### Scenario: A triangle mesh densifies without becoming quads
- **WHEN** `Mesh.loop_subdivide()` is called on a mesh of N triangles
- **THEN** the mesh SHALL afterwards hold 4N triangles and the reported face count SHALL equal the mesh's face count

#### Scenario: Linear mode is reachable and does not smooth
- **WHEN** `Mesh.loop_subdivide(LoopSubdivideMode.LINEAR)` is called
- **THEN** every original vertex position SHALL still be present in the refined mesh

#### Scenario: A quad mesh raises the topology error
- **WHEN** `Mesh.loop_subdivide()` is called on a mesh containing quads
- **THEN** it SHALL raise the typed unsupported-topology error naming the face and its side count, and the mesh SHALL be unchanged

### Requirement: Isotropic remeshing binding
The C ABI and the Python binding SHALL expose the engine's adaptive isotropic
(triangle) remesher as a one-call operation on a mesh handle, so a caller can
densify, re-tessellate or decimate a mesh to a target edge length without
running the quad pipeline. Parameters SHALL mirror the engine's own options —
target edge length, iteration count, curvature adaptivity and smooth-normal
degrees — and SHALL be fillable from the engine defaults, except the target
edge length, which is world-space and SHALL be left for the caller to supply
rather than invented.

The entry point SHALL assemble the inputs the C++ contract requires: it SHALL
build the projection reference from the input surface before any remeshing, it
SHALL tag feature edges from a supplied dihedral threshold, and it SHALL
triangulate a non-triangulated input rather than rejecting it. The header SHALL
state which of those it does, that the result is therefore a triangle mesh, and
that every element id is invalidated.

A capability of the engine's isotropic options that the binding does not expose
— painted density — SHALL be recorded in the header as a stated omission naming
where it is reachable instead, so the gap is visible rather than assumed absent.

#### Scenario: Target edge length controls density
- **WHEN** a caller remeshes the same mesh at two target edge lengths through the binding
- **THEN** the smaller target SHALL produce a substantially denser mesh whose mean edge length tracks the request, and the reported face count SHALL equal the mesh's own

#### Scenario: Adaptivity reaches the engine
- **WHEN** two runs differ only in the adaptivity parameter
- **THEN** their results SHALL differ, and the adaptive run SHALL show a wider edge-length distribution than the uniform one; repeating either run SHALL reproduce it exactly

#### Scenario: A quad mesh is remeshed, not refused
- **WHEN** a caller passes a mesh carrying quads or n-gons
- **THEN** the call SHALL triangulate it and remesh it, returning a mesh of triangles only

#### Scenario: An unusable target edge length is refused
- **WHEN** the target edge length is zero, negative or non-finite, or the iteration count is below one
- **THEN** the call SHALL return the invalid-parameter status naming the field, and the mesh SHALL be left exactly as it was

### Requirement: ZRemesher is reachable from every binding

The C ABI and the Python bindings SHALL expose the ZRemesher quad method and
its parameters — quality mode, adaptive sizing, local-feature-size
preservation, symmetry, guide mode — and SHALL expose the layout statistics and
quality score from the run report. Parity SHALL hold: any ZRemesher capability
reachable from the CLI SHALL be reachable from Python.

#### Scenario: Python drives the ZRemesher path

- **WHEN** a Python caller requests the ZRemesher quad method with a quality
  mode and a symmetry axis
- **THEN** the remesh SHALL run through that path and the returned report SHALL
  carry the layout statistics and the quality score

#### Scenario: Topology guides from Python

- **WHEN** a Python caller supplies a closed guide in topology mode
- **THEN** the binding SHALL forward the guide mode, and the report SHALL
  record the achieved guide adherence

### Requirement: Current integration contract

The project SHALL publish one current document that distinguishes supported,
experimental, scaffolded and build-dependent integration paths, and SHALL link
to it from the root documentation. Historical roadmaps and benchmark records
SHALL be labelled as dated evidence rather than current guarantees.

#### Scenario: Mobile consumer evaluates support

- **WHEN** an iOS or Android consumer evaluates the library
- **THEN** the documentation SHALL state whether the path has device evidence,
  cross-compilation evidence only, or no published SDK artifact

#### Scenario: ABI consumer evaluates reproducibility

- **WHEN** a host integrates through the C ABI
- **THEN** the documentation SHALL distinguish ABI compatibility from engine and
  solver identity required for reproducible output

### Requirement: Bake provider surface for an external map consumer

The C ABI SHALL expose a BAKE PROVIDER surface: a capability query, a bake request
carrying the consumer's progress and cancellation callbacks, and a result record
describing the pixels. A consumer SHALL be able to enumerate the producible maps, request
each one, and receive pixels plus their complete metadata while linking nothing beyond
this repository's C ABI. The surface SHALL be reachable from the Python and Swift
bindings on the same terms as the rest of the ABI.

**The capability query.** The library SHALL report the map types THIS BUILD can produce,
and a consumer SHALL be able to read that set before it offers a map to a user. Each
advertised map SHALL carry: its map code, a stable machine name, the number of float
channels a texel holds, the colour space the texels are in, the encoding basis a bake of
it reports under default parameters, and whether a field evaluator alone can produce it.
The advertised names SHALL be the same names the other entry points accept for those
maps, so a consumer joining a named export preset to the advertised set never has to
translate between two spellings.

**The request.** A request SHALL name the EditMesh/Target pair, the map, the bake
parameters, and MAY carry a progress callback, a cancellation callback and one opaque
user pointer shared by both. Every parameter the existing bake entry points validate SHALL
be validated here, and the request SHALL honour the same projection cage, texel ceiling,
border padding, progress reporting and cooperative cancellation every existing map already
honours — it SHALL introduce no provider-specific exception to that shared path. This
surface MAY refuse a parameter the existing entry points accept, or refuse it with a more
precise result code; it SHALL NOT accept one they refuse.

**Caller-owned buffers with two-call sizing.** The pixels SHALL be written into a buffer
the CALLER owns, and so SHALL the id-to-colour table of an id map. A request that supplies
no pixel buffer SHALL validate the whole request and report the sizes WITHOUT baking, so
a consumer can size its allocation, and learn that a request would be refused, without
paying for a bake. A PIXEL buffer too small for the result SHALL be refused, naming the
capacity supplied and the capacity required, rather than filled partway — a consumer can
compute that count exactly from the capability query before it calls, so a short one is a
consumer-side bug. The ID TABLE SHALL instead follow the two-call convention, because the
number of ids is not knowable until the bake has read the Target: a short or absent id
buffer SHALL NOT fail the request, SHALL be filled to exactly the capacity the caller
stated and to no byte beyond it, and the result SHALL report the TOTAL id count so the
consumer can allocate that many and ask again.

**Result metadata travels with the pixels.** The result SHALL carry the image's width,
height and channel count, the encoding basis, the up axis, the normal green-channel
convention, the bounding box an object-space position was rescaled over, the factor a
distance was multiplied by, the padding radius applied with the fill rule used and the
texels the band wrote, the id source and id-table size of an id map, and the number of
texels the UV layout covered. Nothing a consumer needs in order to interpret the pixels
SHALL be documented out of band.

**Descriptors state their own size.** Every descriptor of this surface SHALL carry its own
size as its first member, and SHALL be passed one at a time by pointer and never as an
array. The library SHALL read an input member, and write an output member, only when the
caller's stated size covers it. A member appended to such a descriptor in a later release
SHALL therefore keep its documented default for a caller compiled against the earlier
layout, and SHALL NOT be a breaking change. A stated size below the first published
layout SHALL be refused naming both sizes.

**A map this build cannot produce is refused by name.** A request for a map that is not in
the advertised set SHALL FAIL, and the failure SHALL name the requested map and list the
set actually advertised. It SHALL NOT return a neutral, blank or default-valued image: a
consumer that silently receives flat grey where it asked for curvature produces work that
is subtly wrong rather than visibly broken.

**Cancellation hands back nothing.** When cancellation is requested mid-bake the call
SHALL return promptly with a result code distinct from every other failure, and SHALL
leave the caller's pixel and id buffers exactly as they were. No partial map, no partly
grown padding band and no partial id table SHALL ever be handed back.

#### Scenario: A consumer enumerates and requests every advertised map

- **WHEN** a consumer reads the advertised map set and requests each map in it for an
  EditMesh/Target pair
- **THEN** every request SHALL succeed, SHALL fill the consumer's own buffer with the
  channel count the query advertised for that map, and SHALL report the encoding basis,
  up axis, normal green-channel convention and padding record of the map it produced

#### Scenario: The sizing call costs no bake

- **WHEN** a consumer issues a request with no pixel buffer
- **THEN** the call SHALL validate the request and report the width, height, channel count
  and required buffer length, and SHALL NOT cast a ray or write any pixel

#### Scenario: A short buffer is refused, not filled partway

- **WHEN** a consumer issues a request whose pixel buffer is smaller than the reported
  requirement
- **THEN** the call SHALL fail naming both the supplied and the required capacity, and
  SHALL write no pixel into that buffer

#### Scenario: An unproducible map is named, never substituted

- **WHEN** a consumer requests a map that is not in the advertised set
- **THEN** the call SHALL fail, the diagnostic SHALL name the requested map and list the
  advertised set, and no image SHALL be produced

#### Scenario: Cancelling mid-bake leaves the consumer's buffer untouched

- **WHEN** a consumer's cancellation callback reports cancellation while a bake is running
- **THEN** the call SHALL return the cancelled result code promptly and every byte of the
  consumer's pixel buffer SHALL hold what it held before the call

#### Scenario: Progress is reported while the map accumulates

- **WHEN** a consumer attaches a progress callback to a request for a ray-traced map
- **THEN** progress SHALL be reported repeatedly as texels accumulate, not once at the end

#### Scenario: An id map's table arrives with its pixels

- **WHEN** a consumer requests an id map and supplies an id-table buffer
- **THEN** the result SHALL report the id source and the total number of distinct ids, the
  buffer SHALL hold those rows ascending by id, and a colour picked out of the returned
  pixels SHALL resolve to exactly one of them at zero tolerance

#### Scenario: A descriptor from an older caller keeps the documented defaults

- **WHEN** a request states a descriptor size larger than this build's layout, as a caller
  compiled against a later header would
- **THEN** the call SHALL succeed, reading only the members this build's layout covers,
  and a stated size below the first published layout SHALL be refused naming both sizes

#### Scenario: The advertised names are the names the rest of the ABI accepts

- **WHEN** the advertised map names are compared with the map names an export preset
  declares and the headless CLI accepts
- **THEN** the three lists SHALL name the same maps with the same spellings

#### Scenario: A short id table is filled to its capacity and reports the total

- **WHEN** a consumer requests an id map with an id-table capacity smaller than the number
  of ids the Target carries
- **THEN** the call SHALL succeed, SHALL write exactly that many rows and no byte past
  them, and SHALL report the TOTAL id count, so that asking again with that capacity
  returns the whole table

#### Scenario: Only an appearance map is advertised as sRGB

- **WHEN** a consumer reads the colour space of every advertised map
- **THEN** each SHALL be stated as either linear or sRGB, and only the colour map SHALL be
  sRGB, so a consumer never puts a transfer curve on a normal, a distance or an id key,
  and never ships an appearance map flat

#### Scenario: The projection cage reaches the bake

- **WHEN** one EditMesh/Target pair is requested twice through the provider, once with a
  projection cage too short to reach the Target and once with one long enough
- **THEN** the two results SHALL differ, and only the longer cage SHALL report the
  Target's surface — the cage SHALL NOT be a parameter that is merely carried

#### Scenario: The Python and Swift bindings drive the same surface

- **WHEN** the provider is driven from the Python binding and from the Swift binding
- **THEN** each SHALL enumerate the same advertised set, produce for a given map the same
  pixels the C entry point produces, receive repeated progress reports and observe a
  cancellation, with no host-side C required

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

### Requirement: Regioned baking is reachable from the bindings

The C ABI SHALL expose the working-set bound as a member APPENDED to the sized
`CyberBakeParams` and `CyberBundleParams`, which is additive under "Sized parameter structs"
(ABI 2.1); a caller stating the 2.0 size SHALL get the default of zero (no bound), and no
byte past its stated size SHALL be read or written. The C ABI SHALL expose a regioned bake
of one map that hands each band's rows to a host callback in ascending order, accepts an
optional field evaluator, reports progress and honours cooperative cancellation, and whose
result carries every encoding, padding and id record an ordinary bake's image carries. A
host callback that asks to stop SHALL abandon the bake with an I/O status. The region facts
(region height, halo, region count, working set held) SHALL be readable for a baked image
and for every map of a bundle. Every parameter an ordinary bake validates SHALL be validated
identically by the regioned bake, and the host's texel ceiling SHALL apply to its output.
The entry points that return a whole image accept the working-set bound and do not read it,
because their result is the whole output by construction.

Python and Swift SHALL expose the same regioned bake and the working-set bound on bakes,
setting `structSize` themselves. Python SHALL also expose the bound on bundles; Swift binds no
bundle writer at all, so it has none to extend. A binding whose integer type can hold a
negative bound SHALL refuse one rather than let it wrap to the unsigned "no bound" range.

The image a regioned bake returns carries the map's metadata and NO pixels. Every entry point
that encodes an image's pixels SHALL refuse such an image with `CYBER_ERR_INVALID_ARG` and
never read past its empty buffer.

#### Scenario: A 2.0 caller gets the default working-set bound
- **WHEN** a caller states the ABI 2.0 size of `CyberBakeParams` or `CyberBundleParams`
- **THEN** the defaults call SHALL write nothing past that size and the bake SHALL run with no working-set bound

#### Scenario: A regioned bake through the C ABI equals the ordinary bake
- **WHEN** a host bakes a map through the regioned entry point with a bound that splits it into several regions
- **THEN** the rows its callback receives, assembled, SHALL equal the ordinary bake's pixels, and the region facts SHALL report more than one region

#### Scenario: The bindings stream a regioned bake
- **WHEN** the Python or Swift binding runs a regioned bake
- **THEN** it SHALL deliver the rows in ascending order and the assembled map SHALL equal the ordinary bake

#### Scenario: Saving a regioned bake's image is refused
- **WHEN** a host saves the image a regioned bake returned, for a one-, three- or four-channel map
- **THEN** the save SHALL fail with `CYBER_ERR_INVALID_ARG`, SHALL write no file, and the process SHALL NOT crash

#### Scenario: A negative bound is refused by the binding
- **WHEN** the Python binding is given a negative working-set bound for a bake or a bundle
- **THEN** it SHALL raise before calling the engine rather than bake with no bound

### Requirement: The ABI carries its own version, distinct from the engine's

This elaborates the one-line promise already in "Full-surface C ABI facade"
("The ABI SHALL carry a runtime-queryable semantic version; minor releases SHALL
be additive only") into a contract with testable scenarios. That sentence had no
implementation: the only version an embedder could read was the ENGINE's, which
moves for quality work that changes no declaration.

The ABI version SHALL be distinct from the engine's semantic version and SHALL
be declared in the public header as preprocessor constants, so that a consumer
vendoring the source — or generating bindings from the header — obtains it
without reading the build system. It SHALL be major.minor with no patch
component, because no change to the linkable surface is neither additive nor
breaking.

A library SHALL serve a client when the majors are equal and the library's minor
is at least the client's. That rule SHALL be implemented once, inside the
engine, and exposed as an entry point, so that every binding reaches the same
verdict rather than reimplementing the comparison. A mismatch SHALL be reported
as a status code with both versions retrievable; the library SHALL NOT abort,
exit or log, because it runs inside a host process.

The shared library's SONAME SHALL carry the ABI major, not the project major.

Appending an enumerator to an existing enum SHALL NOT be treated as an additive
minor change: an unfixed C enum's value range is inferred from its enumerators,
so handing a client a value outside the range it compiled against is undefined
on the client's side.

#### Scenario: ABI version query

- **WHEN** a client compiled against ABI 1.x loads a 1.y (y > x) library
- **THEN** all 1.x entry points SHALL work unchanged, and the compatibility
  check SHALL report success

#### Scenario: A newer client is refused by an older library

- **WHEN** a client compiled against a LATER ABI minor than the library
  implements asks the library to serve it
- **THEN** the check SHALL fail with both versions named, because the entry
  points the client compiled against are genuinely absent

#### Scenario: The two versions are independent

- **WHEN** the engine's behaviour changes without altering any declaration
- **THEN** the engine version SHALL move and the ABI version SHALL NOT, and a
  matching ABI SHALL NOT be taken as a promise of identical output

### Requirement: ABI declarations are machine-checked beyond byte layout

The release and CTest gates SHALL compare the public C header with a checked-in
ABI manifest. The manifest SHALL record exported function and callback
signatures, enum values and representation, and every concrete struct's field
identity, source type, offset, field size, aggregate size and alignment.
Compiler-measured layout SHALL be validated on each supported packaging
toolchain. A source-level type change or a field added in existing padding SHALL
be reported even when the aggregate `sizeof` is unchanged.

The project SHALL compile and run a previous-release client surface against the
current library. That client SHALL use its historical declarations rather than
including the current header, and SHALL exercise both ordinary calls and
guarded output buffers.

#### Scenario: A same-sized type change is rejected

- **WHEN** a pointer pointee type changes while its pointer-sized field and
  aggregate layout remain the same
- **THEN** manifest validation SHALL fail

#### Scenario: A field consumes trailing padding

- **WHEN** a field is introduced in previously unused trailing padding
- **THEN** manifest validation SHALL fail even if `sizeof` is unchanged

#### Scenario: An old client uses the new library

- **WHEN** the client translation unit is compiled from the retained v0.8.0
  declarations and linked with the current shared library
- **THEN** it SHALL load a mesh, use array/out-param APIs without damaging
  guard bytes, and complete successfully

### Requirement: Element-id staleness is detectable, not only documented

The ABI SHALL expose a monotone counter per mesh handle that changes whenever
that handle's element ids may have been reassigned, so that a host holding
id-keyed state can verify it rather than infer from documentation which
operations preserve ids.

The counter SHALL follow the same distinction the element-id rules draw: an
operation that only moves vertex positions SHALL NOT change it, and an operation
that may create, destroy or rewire elements SHALL change it. It SHALL be a hint
in the SAFE direction only — it may change when ids in fact survived, never the
reverse — so equal values prove ids are valid while differing values only
require the host not to assume. A clone SHALL carry its source's value, because
a clone's ids are the source's ids.

#### Scenario: A positions-only edit preserves the counter

- **WHEN** an operation moves vertex positions without changing topology
- **THEN** the counter SHALL be unchanged, so caller-side annotations survive

#### Scenario: A rebuild changes the counter

- **WHEN** an operation reassigns element ids, such as subdivision
- **THEN** the counter SHALL change, and SHALL never return to an earlier value

### Requirement: A callback is named for the value it returns

Where the engine asks a host for a quantity through a callback, the override
point SHALL be named for what it RETURNS, not for the quantity's inverse or a
related term, so that an implementer following the name computes the value the
engine consumes. Where a name is frozen by the ABI and cannot be corrected, the
polarity SHALL be stated in the documentation a consumer reads when choosing the
operation, not only at the callback's own declaration.

#### Scenario: An implementer following the name is correct

- **WHEN** a host implements the ambient-occlusion callback from its name alone
- **THEN** the value it computes SHALL be the one the bake consumes, so the
  resulting map is not inverted

### Requirement: Bulk indexed polygon exchange

The C ABI SHALL import a mesh from a copying packed-XYZ and CSR polygon
descriptor and SHALL export authored polygon offsets and indices without
triangulating them. The import SHALL reject malformed counts, non-finite
coordinates, non-monotonic offsets, faces with fewer than three corners, and
out-of-range indices without publishing a partial output handle.

#### Scenario: Mixed polygon round trip

- **WHEN** a host imports shared-vertex triangles, quads, and n-gons
- **THEN** authored polygon arities and connectivity SHALL be returned by the
  bulk export

#### Scenario: Invalid descriptor

- **WHEN** a host supplies invalid offsets, indices, or coordinates
- **THEN** import SHALL fail and leave its output pointer unchanged

### Requirement: Count outcomes are available to embedded hosts

The C ABI and supported language bindings SHALL expose an additive count policy
and count outcome report so a host can decide whether to retry without parsing
diagnostic text. Existing parameter/report layouts SHALL remain unchanged.

#### Scenario: ABI-compatible count policy

- **WHEN** a host compiled against the existing remesh parameters invokes the
  existing entry point
- **THEN** it SHALL remain binary compatible and receive existing behavior

