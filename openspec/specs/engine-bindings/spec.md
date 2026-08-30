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

The gate SHALL be a runnable check in the test suite, not a convention: `tests/packaging/test_swift_abi_parity.py` (ctest case `swift_abi_parity`) fails when a Swift source references a C symbol the header does not declare or declares with a different arity, and per-surface parity checks in the Python test suite fail when a header symbol is added without being bound. A capability the ABI exposes but the bindings do not SHALL be recorded as a pending registration, so the gap is visible rather than assumed absent.

#### Scenario: Surface drift is caught
- **WHEN** an ABI entry point is added without a matching Python or Swift wrapper
- **THEN** the parity check SHALL fail naming the missing wrapper

#### Scenario: The gate is executable
- **WHEN** the test suite runs on any platform, including those without a Swift toolchain
- **THEN** the source-level parity gate SHALL run as an ordinary test case and fail on a symbol mismatch

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

