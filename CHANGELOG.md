# Changelog

> Note: releases 0.3.0, 0.4.0 and 0.5.0 were tagged without changelog entries;
> their content is recorded in `docs/ROADMAP.md`. Entries resume here.

## Unreleased

### Added

- **Named soft-selection slots now persist with the document, end to end.**
  The two halves existed and nothing connected them: `cyber::app::Document`
  serialized a `softSelections` map and the C ABI kept its own slot map on
  `CyberMesh`, but no code in the tree moved weights between them in either
  direction — a slot saved through the ABI was absent from the saved document,
  and a slot in a loaded document was invisible to the ABI, so the
  manual-retopology spec's "savable to and loadable from named slots persisted
  with the document" was unreachable. The seam is now a real document handle:
  13 additive `cyber_document_*` entry points (`_create` / `_free`,
  `_set_target` / `_set_edit_mesh`, `_target` / `_edit_mesh`, `_slot_count` /
  `_slot_name` / `_slot_weights`, `_save` / `_load` / `_save_file` /
  `_load_file`), exposed as `cyberremesh.Document` in Python and
  `Document.swift` in Swift. `set_edit_mesh` copies the handle's named slots
  into the document and `edit_mesh()` hands back a fresh handle carrying them,
  so a weighted selection survives serialize → drop every handle → load. The
  seam is BY VALUE and explicit: nothing is aliased or kept in sync, and the
  live (unnamed) weight field is deliberately not persisted. Implemented only
  when the application-shell library is in the build (`-DCYBER_BUILD_APP=ON`,
  the default); without it the symbols still exist so the ABI is stable and
  every call reports the missing module, exactly like the UV handles.

- **`cyber_retopo_selection_transform_pinned`** — the weighted transform can
  finally honor pins from the C ABI, Python and Swift. See *Fixed*.

- **Sculpt handoff bridge** (openspec change `add-claycore-bridge`): the
  receiving half of a `sculpt -> retopo -> UV -> bake` pipeline, landed with
  **zero hard dependency on any sculpting or volumetric engine**. Three
  additive pieces, all reachable from C++, the C ABI and Python:

  - **Versioned sculpt handoff ingest** (`src/handoff`, `cyber::handoff`). A
    triangle mesh with positions, per-vertex normals, vertex colors and a
    `material_mix` weight arrives as a Target through a file (PLY profile, or
    `.gltf`/`.glb` declaring `asset.extras.cyberSculptHandoff` — that route
    carries the version, the producer label, geometry, vertex colors and
    per-vertex normals; `material_mix` is the one payload glTF cannot express
    and its absence is warned about, not passed over), a stream (stdin), or
    plain in-memory buffers
    with no intermediate file. The version gate runs on the header text **before any
    geometry is read**, so a bad version can never produce a partial Target,
    however malformed the rest of the payload is; the rejection is the existing
    typed `io::ErrorCode::IncompatibleVersion` (`CYBER_ERR_INCOMPATIBLE_VERSION`
    on the C ABI, `IncompatibleVersionError` in Python) and names both the
    version found and the version supported. A newer *minor* is refused rather
    than read with the unknown parts dropped, matching the export-preset
    unknown-field precedent. A triangle the mesh refuses (a repeated vertex
    index) is counted and warned about — `Handoff::droppedFaces`,
    `CyberHandoffInfo::droppedFaces`, `HandoffInfo.dropped_faces`,
    `handoff.droppedFaces` in the CLI report — rather than dropped in silence.
    CLI: `--target <path|->`, mutually exclusive with `--input`, plus a
    `handoff` block in the JSON report and `--bake <csv>` to override the
    preset's map set.
  - **Field-sampled baking through an evaluator interface**
    (`cyber::bake::FieldEvaluator`, `BakeParams::field`, `cyber_bake_field`,
    `cyberremesh.FieldEvaluator` / `bake_field`). A pure-abstract interface —
    signed distance, gradient, occlusion, plus a free central-difference
    curvature default — is the only coupling point to a volumetric engine. With
    one attached, normal / AO / curvature / cavity sphere-trace the cage ray
    through the field and read exact gradients, and the Target mesh becomes
    optional for those four maps. **Without one, output is bit-identical to the
    pre-bridge raycast implementation**, pinned by FNV checksums over all seven
    maps captured from the unmodified binary before the refactor.
  - **Conform** (`cyber::retopo::conform`, `cyber_conform`,
    `cyberremesh.conform`). Re-snaps an EditMesh onto a replaced Target,
    preserving topology exactly (only `setPosition` is ever called), and reports
    the **maximum AND RMS** deviation plus every vertex past a caller-set
    threshold. It completes and flags rather than silently stretching. The same
    code now backs the manual-retopology spec's whole-mesh "snap all vertices to
    Target" command (`retopo::snapAll`), which had never been implemented, so
    the two cannot diverge.

  Format: `docs/sculpt-handoff-format.md`. Demo: `examples/18_sculpt_handoff.py`
  (a synthetic producer plus the one-command run, from a file and over a pipe).

  **Two honest gaps.** (1) The handoff format is defined **unilaterally by this
  repository**. The proposal's task 1 called for one shared document agreed with
  ClayCore's export-profile change; ClayCore is not in this tree and no
  negotiation took place, so the versioning is proven only against files this
  repo writes — "loud on both sides" is demonstrated on our side only. (2) No
  real volumetric evaluator was exercised. The field path is validated against
  analytic SDF test doubles (a sphere and a plane), which proves the interface
  and the sampling math but not interoperability with any actual sculpting
  engine.

- **Flow guides and painted density** (openspec change `add-flow-guides`): two
  explicit, opt-in *local* controls on an otherwise fully global pipeline.
  **Flow guides** are 3D polylines drawn on or near the Target whose tangent the
  4-RoSy cross field is softly biased toward within a per-guide influence radius
  and strength. **Painted density** is a per-vertex or per-face scalar on the
  Target that multiplies local sizing under the documented relation
  `localEdgeLength = baseEdgeLength / sqrt(density)` — density is a
  quads-per-unit-area multiplier, so it composes with the area-derived
  `targetQuadCount` instead of fighting it. Nothing here is inferred; this is
  deliberately distinct from the automatic curvature adaptivity the roadmap
  descoped after measurement retracted its claimed win.

  **Unguided output is byte-identical**, structurally rather than by tuning:
  every guidance path sits behind a null/empty check, so a run without guidance
  executes textually the same arithmetic as before. Verified by bit-comparing
  `CrossField::real/imag`, by two in-process `remesh()` runs compared as raw
  float bits plus face lists, by a CLI `cmp` of an empty-guide-list run against
  no `--guides` at all, by `ctest -R bench` matching its recorded baseline
  number for number, and by re-running `fandisk` at 1000 and 4000 quads
  byte-for-byte against a pre-change baseline. `spot` and `stanford-bunny`
  could NOT serve as oracles: on the vendored Geogram quad-cover route they are
  already run-to-run non-deterministic on unmodified `main` (6 identical spot
  invocations produced 6 distinct outputs there), which is a pre-existing
  property of that route and is recorded here rather than papered over.

  **A density of 1.0 everywhere is a no-op too.** 1.0 is the identity of the
  sizing relation, but merely *carrying* a density field forces the native
  seamless route, so an all-1.0 paint used to change the mesh on the shipped
  default backend (`fandisk --target-quads 1000`: md5 `3add9afb` unguided vs
  `b390b987` with `perVertex=[1.0]*6475`). `validateGuidance` now drops a
  post-clamp neutral density and reports it as a non-fatal issue, so it is
  byte-identical to no density at all; a uniform density of any other value
  still applies normally.

  **Guidance is honored loudly or rejected loudly.** `IQuadrangulator` grows
  `acceptGuidance`, whose default body *declines* and states why, plus an
  `unhonoredGuidance()` query for guidance accepted up front but lost at run
  time. `PipelineResult::islandGuidance` carries one row per island naming the
  guides in range, whether guides and density were honored, and the reason if
  not. Supplying guidance **forces the native seamless route** on the
  quad-cover backend, because the vendored Geogram solve has no hook for either
  input — a documented quality trade (native ~4-5% irregular vs vendored 1-4%
  on smooth organics), not a hidden one. Two ways guidance could still slip
  through that audit are closed: the developer kill switch `CYBER_QC_NO_NATIVE`
  disables the native solve, and a guided island then takes the vendored route —
  it is now reported as unhonored, naming the env var, instead of being counted
  as honored; and a guide whose reached faces are ALL owned by hard pins (feature
  edges, boundaries, crease alignment) leaves the field bit-for-bit unguided, so
  it is now reported with its absorbed-face count rather than counted as honored
  (a partial absorption still counts as honored — the guide did move the field).
  Zero-radius guides, guides with fewer
  than two points, non-finite values and mismatched density array lengths are
  rejected as fatal rather than silently ignored; out-of-range strength and
  density values clamp with a reported effective value.

  Reachable from the C ABI (`cyber_remesh_guided` + `CyberWarningCb`;
  `cyber_remesh` and `CyberRemeshParams` are untouched and ABI-identical),
  Python (`FlowGuide`, `remesh(..., guides=, density=)`, warnings surfaced both
  as `Mesh.guidance_warnings` and through `warnings.warn`), the network bridge
  (`push_guides` / `pull_guides` / `clear_guides` / `push_density` /
  `pull_density`, no protocol-version bump), and the CLI (`--guides
  <file.json>` sidecar plus a `guidance` block in `--report`). Every sidecar
  field is type-checked before it is read — a string where a number belongs is
  an exit-2 argument error naming the file and the field, where it previously
  reached an unguarded JSON accessor and aborted the process (SIGABRT) — and
  `main` has a last-resort handler so no escaping exception can ever replace a
  diagnosis with a crash. See `docs/flow-guides.md`.

  **Exit gate: NOT met corpus-wide, reported as measured.** The proposal's gate
  is <=15 deg mean deviation between extracted loop direction and the guide
  tangent inside the influence radius (random baseline 22.5). Measured by
  `examples/17_flow_guides.py` on `spot` and `stanford-bunny` at 2000 quads
  across three influence radii: guided **17.17 deg mean, 25.10 deg worst**, with
  2 of 6 runs at or under 15. Guides improve alignment on **every** run
  (e.g. spot 30.01 -> 18.17, bunny 15.40 -> 11.31), and the constraint is strong
  at the field stage (a flat grid with a 45-deg guide goes 44.84 -> 11.29 deg,
  and a tube end-to-end through the field-aligned backend 12.08 -> 9.23). The
  loss is dilution between the cross field and the extracted mesh — the seamless
  Poisson solve, integer rounding and isoline extraction each shed angular
  fidelity. Carrying the guide into the seamless solve as a per-face
  target-frame rotation is the follow-up.

- **Auto-routed seam paths** (openspec change `add-seam-path-tool`): the UV
  Path tool. Place waypoints on the EditMesh and the engine routes a least-cost
  edge path between consecutive ones, under a cost model that discounts
  feature-tagged edges and concave (valley) edges past a crease angle — so a
  two-click hop **follows the groove** instead of cutting the geodesic shortcut
  across flat surface, which is what makes spiral-looped auto-retopo output
  seamable at all.

  **The pending path stays editable until commit.** Any waypoint can be
  repositioned (by id, or dragged onto the nearest vertex within a radius) or
  deleted, and an edit re-routes **only the segments adjacent to it**; every
  other segment keeps both its route and its per-segment `routeRevision`
  counter, so a viewport can redraw exactly what changed. Committing marks the
  route into the existing `cyber::uv::SeamSet` — the one seam model, the same
  `mark`/`erase`/`sew` set a hand-drawn seam edits, so `computeIslands`,
  `unwrapIslandToUv` and `stitchAlongSeams` treat a routed seam exactly like a
  hand-marked one (all three are C++ only: the bindings expose the seam set and
  the path, not island/unwrap/stitch over it, and there is no stroke→seam
  gesture entry point on any surface) — and arms a **resume marker** so
  the next waypoint continues from the last committed point. Commit returns a
  `SeamCommit` undo record listing only the edges it *newly* marked, so
  `revertCommit` restores the exact pre-commit state and edges that were
  already seams survive the undo; dropping the resume marker only forgets where
  to continue and never touches a committed seam. A path with an unroutable
  segment (disconnected components) commits nothing and stays pending for
  repair.

  Reachable from the C ABI (`cyber_seam_set_*` / `cyber_seam_path_*` plus
  `cyber_mesh_edge_signed_dihedral`, all additive — no existing struct layout
  changed), Python (`SeamSet`, `SeamPath`, `SeamCostParams`) and the Swift
  package. Internally this reuses the existing shortest-path machinery:
  `cyber::retopo::shortestVertexPath` was generalized into a templated
  `weightedVertexPath` and is now a one-line wrapper over it, so Path Distribute
  and `cyber_mesh_shortest_vertex_path` are behaviour-identical.

- **Soft selection for manual retopology** (openspec change
  `add-soft-selection`): a per-vertex weight field in [0,1] over the EditMesh,
  filled from three region sources — a **line gradient** (0 at the anchor, 1 at
  the end and 1 beyond it, with optional 15° angle snapping measured in the
  view plane), a **sphere** with an easing falloff, and **painted strokes**
  (pressure-accumulating, with a subtract mode and a one-call gesture route) —
  reshaped by clear / invert / expand / contract / smooth, and consumed by a
  **weighted transform** (translate, rotate, scale scaled per vertex by the
  weight) and a **weighted relax**.

  **Auto re-snap is part of the operation, not a cleanup pass.** The weighted
  transform and relax re-project the vertices they move onto the Target inside
  the same pass, so a taper or a pose never peels the retopo off the sculpt and
  no `snap_all` follow-up is needed — running one would also drag the vertices
  the selection deliberately left alone. Vertices at weight 0 are skipped
  entirely, so they are bit-identical afterwards; pinned vertices are skipped
  too wherever pins can be supplied, which through the C ABI today means the
  weighted relax only — `cyber_retopo_selection_transform` has no `pinned`
  argument yet, so on that call weight is the only thing that holds a vertex
  still.

  Reachable from the C ABI (`cyber_retopo_selection_*`, including the
  x/y/z/pressure stroke route), Python (`Mesh.select_line`, `paint_selection`,
  `transform_selection`, the new `Snapper` wrapper, …) and the Swift package.
  Named selection slots have both halves of their persistence in place but are
  **not yet persisted end to end**: `cyber::app::Document` gains an
  **append-only** section that is written only when slots exist, so documents
  saved by earlier builds still load and documents without slots keep the exact
  previous byte layout (`kFormatVersion` deliberately unchanged), while the C
  ABI's `cyber_retopo_selection_save`/`_load` slots live on the mesh handle for
  the session. Nothing connects the two yet — that is shell work, and until it
  exists a saved slot does not survive the process.

- **Global integer quantization for the seamless solve** (openspec change
  `bimdf-quantization`, now archived). The quad-cover path's per-seam greedy
  integer rounding gains an alternative backend: a motorcycle-graph / T-mesh
  decomposition of the seamless parameterization solved as a min-deviation flow
  over a bi-directed graph, after Heistermann et al. 2023. GPL discipline —
  derived from the papers alone; no quadwild/libsatsuma code was read or
  vendored.

  Ships **opt-in and off by default**, behind `CYBER_QC_BIMDF`:
  `report` runs it in A/B mode without injecting, `guided` steers the greedy
  schedule by attracting its re-solves toward a Bi-MDF solve, `force` injects
  directly. With the flag off the output is byte-exact against the previous
  binary.

  Where `guided` engages it is a real win: stanford-bunny@3000 singularities
  135 → 94 with ear irregulars 37 → 19 (beating QuadriFlow's 20), and spot@3000
  pure flow-loop length 762.9 → 1274. **The default was not flipped**, and the
  campaign's own gates were not met: nefertiti@4000 pure singularities reach 411
  against a ≤200 gate. The remaining wall is now quarter-density fold damage
  (1297 cones, 110 degraded nodes), not the tracer or the quantizer — the
  self-spiralling-separatrix tracer wall and the whole-T-mesh refusal were both
  fixed along the way, so every corpus mesh now builds and solves a T-mesh.

  Two design directions were measured and **falsified**, and are recorded so
  they are not retried: coordinate-wise floor/ceil steering (lost on every
  scoring variant) and parity-aware quantization (the parity classes are a
  coupled GF(2) system beyond the graphic T-join, and exact flow realization
  hurts precisely where injection is blocked).

- **Curvature and cavity map baking** (openspec change `add-curvature-bake`),
  the missing quarter of the standard pre-texture recipe (curvature +
  occlusion + normal). `BakeMap::Curvature` encodes the Target's signed mean
  curvature around mid-gray — convex bright, concave dark — and
  `BakeMap::Cavity` keeps concavity only, so flat and convex both read white
  and the map drops straight into a multiply slot. Both are single-channel and
  follow the same cage projection, cancellation and PNG/EXR output as every
  other map: a curvature bake registers texel-for-texel with the normal bake
  taken through the same cage.

  Curvature is estimated with the Meyer et al. cotangent operator over a mixed
  Voronoi area (`cyber/bake/curvature.hpp`), fan-triangulating n-gons exactly as
  the BVH does so the estimate matches what the rays actually hit. Boundary
  vertices, where the cotangent formula has no meaning, take the mean of their
  interior neighbours instead of reading as a spurious crease.

  The new `BakeParams::curvatureRange` sets the curvature magnitude that
  saturates the map; the default of `0` auto-fits it to the 95th percentile of
  `|curvature|` over the Target, which is scale-independent and keeps a single
  pinched vertex from flattening everything to mid-gray. Reachable as
  `CYBER_BAKE_CURVATURE` / `CYBER_BAKE_CAVITY` in the C ABI and
  `BakeMap.CURVATURE` / `BakeMap.CAVITY` in Python.

- **Per-DCC export presets** (openspec change `add-export-presets`). `--preset
  <name|path>` turns a CLI run into a ready-to-hand-off bundle: remesh → auto-UV
  (when the low-poly has none) → bake the preset's map set → write the mesh and
  its maps under the preset's naming, color-space and normal-map conventions.
  Four built-ins ship — `blender`, `unity`, `unreal`, `gltf-generic`, listed by
  `--list-presets`. They differ where the target apps do: only `unreal` uses the
  DirectX green-channel convention, and only `gltf-generic` omits curvature
  (glTF 2.0 core has no slot for it). Color is the one sRGB map everywhere else;
  the data maps stay linear.

  A preset is versioned JSON (`cyber/core/export_preset.hpp`), so a user file
  passed by path behaves exactly like a built-in. An unsupported `schemaVersion`
  is rejected with a typed `IncompatibleVersion` error naming both the found and
  the supported version, and an unrecognised field is an error rather than a
  silent drop. An unknown preset name exits with the argument-error code and
  lists the built-ins, before the remesh rather than after it.

  New CLI flags: `--preset`, `--list-presets`, `--texture-size`, `--ao-samples`,
  `--cage`. The JSON report gains `preset` (name, schema version, resolved map
  set, conventions) and `outputs` (every file produced, with its written color
  space and dimensions).

  Known limitations: the maps are written as sibling files and are **not**
  referenced from the glTF material for the glb-based presets, so those bundles
  are import-ready but not auto-wired. The built-in conventions are encoded from
  each target's documented spec and pinned by unit tests, but have not been
  verified inside Blender, Unity or Unreal.

- **In-memory mesh copy and position write-back** (`cyber_mesh_clone`,
  `cyber_mesh_set_positions`; `Mesh.copy` and `Mesh.set_positions` / the
  `Mesh.positions` setter in Python). There was previously no way to duplicate
  a mesh or restore a previous state without a `save_obj` / `load_obj` round
  trip, which narrows to the OBJ text precision — `examples/16_soft_selection.py`
  was reporting ~8e-07 of spurious vertex movement from that alone, and now
  reports exactly 0. `cyber_mesh_clone` copies the whole handle (geometry,
  statistics, hidden-face and tagged-edge overlays, the soft-selection weight
  field and its saved slots) and preserves element ids, so it works as an undo
  snapshot. `cyber_mesh_set_positions` takes the same compacted order
  `cyber_mesh_copy_positions` returns and rejects a count mismatch rather than
  pairing positions with the wrong vertices.

- **Edge -> vertex-pair and vertex -> position accessors in Python**
  (`Mesh.edge_endpoints`, `Mesh.vertex_position`, over the existing
  `cyber_mesh_edge_endpoints` / `cyber_mesh_vertex_position` ABI). A committed
  seam is a set of opaque edge ids, so `SeamSet.edges()` / `SeamPath.edges()`
  could not be resolved to geometry from Python and a committed seam could not
  be drawn at all; `examples/20_seam_paths.py` now renders one.

- **In-memory sculpt handoff from Python** (`Mesh.load_handoff_buffers`, over
  the existing `cyber_handoff_open_buffers` ABI). Two handoff profiles are
  documented — a file and plain in-memory buffers — but only the file one had a
  Python surface (`Mesh.load_handoff` takes a path), so an in-process producer
  had to write a temporary PLY. The buffer profile carries the same optional
  normal / colour / `material_mix` payloads and applies the SAME version gate,
  so going through memory cannot bypass it; a payload whose length does not
  match the vertex count is rejected in Python rather than read past its end
  (the C struct's pointers carry an implied length).

- **Named export presets and bundles on the C ABI and in Python**
  (`cyber_export_preset_builtin_count` / `_builtin_name` / `_resolve` /
  `_info` / `_map` / `_map_file_name` / `_set_resolution` / `_free`,
  `cyber_default_bundle_params`, `cyber_export_bundle_write` and the
  `cyber_bundle_result_*` readers; `builtin_presets`, `ExportPreset`,
  `PresetMapEntry`, `write_bundle`, `BundleResult` and `BundleFile` in
  Python). Presets were CLI-only: the C ABI had no export entry point at all,
  so `examples/19_export_presets.py` had to shell out to the `cyberremesh`
  binary and re-read its JSON report. The example now drives the bindings
  directly — it also reads each preset's declared mesh container instead of
  carrying a hard-coded extension table, and unwraps the low-poly once so all
  four bundles bake against byte-identical UVs. Preset DATA lives in core, so
  listing, resolving and reading a preset works in **every** configuration;
  only `cyber_export_bundle_write` needs the UV-gated bundle module, and
  without it the symbol is still declared and returns a runtime error naming
  the missing module. A preset file from an unsupported schema surfaces as
  `CYBER_ERR_INCOMPATIBLE_VERSION` / `IncompatibleVersionError` naming both
  versions, and an unknown preset name as an invalid-argument error listing the
  built-ins.

### Changed

- **The vendored AutoRemesher/Geogram sources are now pinned to commit
  `b43dc827edd5d39db2f2c925e1b16d5b33ec8388`.** Both fetch sites
  (`cmake/QuadCoverSolver.cmake` and `examples/reference/build_autoremesher.sh`)
  used to `git clone --depth 1` upstream HEAD, so the quad-cover field solver —
  which every benchmark number flows through — depended on when the checkout
  happened. This was not hypothetical: a stale checkout
  (`e2d9b6f4`) produced a solver that was nondeterministic on the bench sphere
  (696/810 quads across identical runs) while the current upstream commit is
  deterministic (622 quads, 17 singularities, five identical runs). The commit
  now lives in one place, `examples/reference/autoremesher.pin`, read by both
  consumers; the sources are fetched as a shallow fetch of that exact SHA, and
  a checkout found at any other commit is loudly re-fetched to the pin instead
  of silently building the wrong solver.

- **`CyberSoftTransformReport.moved` now counts DISTINCT vertices for the
  weighted relax too, not per-iteration writes.** It already meant distinct
  vertices for `cyber_retopo_selection_transform`, but
  `cyber_retopo_selection_relax` accumulated `updates.size()` once per sweep,
  so the same field meant two different things and a multi-iteration relax
  could report more moved vertices than the mesh has: 12 iterations over 78
  selected vertices of an 861-vertex mesh reported **936**, and the same call
  now reports **78**. `resnapped` is counted the same way (distinct vertices,
  so it can no longer exceed `moved`); `max_snap_distance` is unchanged.
  Positions produced by either op are bit-identical — only the report changed.
  Callers that printed the old number as a "vertex updates" total will see a
  smaller value. The C ABI header, the Python docstring and the Swift wrapper
  now all state this, along with why `moved - resnapped` can be non-zero: with
  the default `resnap_epsilon` of 0 a vertex the re-projection did not have to
  correct at all is not counted as re-snapped.
- `cyberremesh`'s reported `elapsedSeconds` now covers export as well as the
  solve. It previously stopped at the end of the remesh, which understated a
  `--preset` run by the entire bake.
- `CyberBakeParams` gained a trailing `curvatureRange` field. The struct is
  passed by pointer and callers allocate it, so a client compiled against the
  older header must be recompiled; always initialise via
  `cyber_default_bake_params`.
- `io::ErrorCode` gained `IncompatibleVersion` for versioned data files whose
  schema the engine does not support. The C ABI maps it to `CYBER_ERR_IO` like
  the other I/O codes, so no C client behaviour changes.
- `cmake/CompilerWarnings.cmake` gained `CYBER_WARNINGS_AS_ERRORS` (default
  `ON`, so CI is unchanged) as an escape hatch for an unforeseen toolchain
  diagnostic. It is no longer needed for the known one: the GCC 12/13
  `-Wstringop-overflow` false positive from inside libstdc++ is suppressed at
  its single call site instead (see Fixed), so the default configuration builds
  on Ubuntu 24.04 with warnings still enforced everywhere else.
- `libcyber_capi` now exports only its `cyber_*` C ABI (a linker version script
  on ELF, `-exported_symbols_list` on Mach-O). The ~4000 vendored
  Geogram/stb/tinygltf/tinyobj/AutoRemesher definitions linked in from the
  static archives were globally visible and therefore open to ELF
  interposition, which matters precisely where this library is meant to run:
  inside a DCC process that carries its own copy of the same third-party code.
  No `cyber_*` entry point changed.

### Fixed

- **Soft-selection weights were resurrected on recycled vertex ids.** Vertex
  ids come back off the mesh's free list, so the weight of a vertex removed by
  `cyber_retopo_erase` / `_delete_faces` / `_dissolve_edges` /
  `_merge_vertices` was still in the field when the next `_create_face` handed
  that id to a brand-new vertex: the new geometry was born selected and
  `cyber_retopo_selection_transform` / `_relax` silently dragged it (a quad
  authored at z = 0 landed at z = 8.6), and the saved slots carried the ghost
  through a document save/load. Every mutating op now unselects the ids it
  killed, in the live field and in every named slot
  (`cyber::retopo::dropDeadWeights`).
- **A non-finite paint dab wiped the whole selection instead of being
  ignored.** The brush's coverage test was `d >= radius`, which is false for a
  NaN distance, so a dab with a non-finite center or pressure — a viewport
  ray-miss unprojects to NaN — "covered" every vertex whatever the radius, and
  the resulting NaN was sanitized to 0 on the way into the field. The coverage
  test is now NaN-safe, such a dab is dropped (skipped per sample inside a
  stroke, so the rest of the gesture still paints), and
  `cyber_retopo_selection_paint` reports it with `CYBER_ERR_INVALID_ARG`.
- **`cyber_retopo_selection_relax` accepted a strength its sibling rejects.**
  The value went into `RelaxParams` unvalidated while `cyber_retopo_relax`
  gates it on `[0,1]`, so a NaN slider value NaN'd every selected vertex
  position and still returned `CYBER_OK` with a plausible `moved` count. It now
  fails with `CYBER_ERR_INVALID_PARAM` and leaves the mesh untouched.
- **The default configuration could not build on GCC 13 / Ubuntu 24.04.** With
  `CYBER_WARNINGS_AS_ERRORS=ON` (the default) exactly one translation unit
  failed, on a libstdc++ `-Wstringop-overflow` false positive inlined from a
  `vector::insert` range append in `reverseCuthillMcKee`. The only escape was
  disabling warning enforcement for the whole tree. Suppressed at that one
  statement instead, guarded on `__GNUC__ >= 12 && !__clang__`, so nothing else
  loses the diagnostic.
- **`push_guides` read past the end of a short guide point.** Each point's
  components were taken with nlohmann's `operator[](size_type)`, which is
  unchecked on a `const` array node, so a point with fewer than three entries
  indexed past the underlying vector — and because nothing threw, the bridge's
  `try`/`catch` could not turn it into an error reply. Point shape is now
  validated before the components are read.
- **`SeamPath::addWaypoint` mutated the path on a rejected add.** With the
  resume marker armed and no pending waypoints, re-adding the resume vertex
  seeded the marker *before* the duplicate check, so the call returned `false`
  (`cyber_seam_path_add_waypoint` -> 0) while `waypointCount()` went 0 -> 1,
  against the header's "changing nothing" contract. The repeat check now runs
  against the marker as the effective last waypoint.
- **`-DCYBER_BUILD_RETOPO=OFF` configured cleanly and then failed at the final
  link** with `cannot find -lcyber_retopo`: capi links `cyber_retopo`
  unconditionally, and CMake demotes the never-added target to a raw link flag.
  The combination is now rejected at configure time with a message naming both
  options.
- **The native seamless solve left fractional seam translations on crease-heavy
  meshes (sharp-cube residual 0.493).** The reduced MIQ elimination can make a
  seam translation or crease lattice offset DEPENDENT on a continuous free —
  canonically a cone position `x` entering a seam loop as `t = (I - R^rho)·x`
  with `|det(I - R^{±1})| = 2` — and the greedy rounding schedule only pinned
  the independent integers, so those translations stayed at whatever fraction
  the Dirichlet optimum chose and the map was not an integer grid across those
  seams. Such continuous frees now join the rounding schedule on their joint
  sub-integer lattice (step `1/lcm(|integer coeffs|)`, half-integer for the
  `±1`-cone case), pinned by the same scheduler; reductions that were already
  integer-exact produce no lattice frees, and the output is byte-identical
  there (verified: box_sharp, cylinder, cube, sphere, torus, spot native runs).
  The sharp-cube unit gate `seamlessUvResidual < 1e-3` now passes, and fandisk
  (the one corpus mesh with violations) improves at every density 400–3000:
  singularities 61/86/80/112/99 → 46/46/40/48/39, angle_dev_mean and
  edge_length_cv down across the board, feature recall 4/5 densities up,
  count-matched within 2.4%.

- **A pinned vertex was moved by a weighted transform through every binding.**
  `cyber_capi.h` documented, for that exact block, "a vertex whose weight is 0
  (and any pinned vertex) is not moved", and the engine's `transformWeighted()`
  has always accepted a `PinSet` — but `cyber_retopo_selection_transform`
  hard-coded `nullptr` for it, so the only thing that could hold a vertex still
  through the C ABI, Python or Swift was a zero weight. Pinning a vertex at
  weight 1.0 and applying a translation moved it the full distance. The fix is
  additive: `cyber_retopo_selection_transform_pinned(mesh, xf, pinned,
  pinned_count, snapper, resnap_epsilon, out_report)` threads the list through,
  and the existing entry point is now that call with an empty list — no
  signature or struct moved. Bound as
  `Mesh.transform_selection(..., pinned=[...])` and
  `transformSelection(_:snapper:resnapEpsilon:pinned:)`. A pinned vertex is now
  excluded from `report.moved` and is bit-identical after the call.

- **`CyberSoftTransformReport.moved` vs `resnapped`: the gap is now explained
  rather than hand-waved.** `examples/16_soft_selection.py` reported 193 moved
  but only 180 re-snapped with no account of the 13. Measured: all 13 carry a
  positive but *negligible* weight (<= 1.8e-33, produced by one-ring smoothing
  at the far tail of a gradient), so the weighted blend rounds to the vertex's
  own position in float — the op writes the same value back, which is what
  `moved` counts, and the Target re-projection then has nothing to correct,
  which is why `resnapped` skips it. `moved` counts WRITES, not displacements.
  The header now says so precisely, and the example measures the tail instead
  of asserting a reason. No behaviour change.

- **The ambient-occlusion bake was banded, not shaded.** Every texel fired the
  *identical* Hammersley hemisphere set, so openness could only land on the
  `k / aoSamples` lattice **and** neighbouring texels snapped to the same rung.
  With the old default of 16 rays that is 17 possible values laid down in flat
  contours: a 256x256 bake of `spot.obj` used **14 distinct levels** across its
  26 413 charted texels, with 47.7% pinned at 255 and *nothing at all* between
  240 and 254. Two fixes: the sample set now gets a per-texel Cranley-Patterson
  rotation (a deterministic hash of the texel coordinate — still no RNG, a
  re-bake is bit-identical), which converts the banding into fine dither at zero
  extra ray cost; and `aoSamples` defaults to **64** rather than 16, because no
  amount of dithering can add rungs that a 16-ray estimator does not have. On
  that same bake: distinct levels **14 -> 45**, texels at exactly 255 **47.7% ->
  31.6%**, and the fraction of adjacent texel pairs that differ (the banding
  measure) **16.0% -> 55.1%**. Mean openness is unchanged (232.5 -> 232.7 of
  255), so the estimator is still unbiased — the map is not darker, it is
  resolved. **Behaviour change:** every AO bake produces different pixels than
  before, and the default bake costs 4x the AO rays (a 3 000-quad `spot.obj`
  preset run went 8.1s -> 11.0s end to end); pass `--ao-samples 16` /
  `BakeParams::aoSamples = 16` to keep the old budget, which now at least
  dithers. The AO entry of the field-bridge pixel checksum was re-captured for
  this; the other six maps still hold their pre-bridge bits.

- **The UV atlas over-reported coverage and over-counted charts.**
  `AtlasResult::packedArea` (`packed_area` in Python, `packedArea` in the C ABI)
  is documented as "fraction of the unit square covered", but it summed the
  packed islands' BOUNDING BOXES. A chart fills only part of its box, so the
  number ran 1.6-2.3x high on curved models — a torus reported 76.6% against
  35.5% of the UV square actually painted. It is now the summed UV face area of
  the packed charts, i.e. the coverage a texture painter sees; the box figure is
  still available, honestly named, as `packedBoxArea` / `packed_box_area` (how
  tightly the box packer placed the charts). The same
  split lands on `PackResult`: `usedArea` is real coverage, `boxArea` is the box
  fraction (`packBoxes` is given boxes only, so there the two agree).
  `chartCount` also counted charts that never land — an island whose LSCM and
  planar-projection unwraps both come out degenerate covers nothing, so the
  reported count disagreed with the visible layout. Degenerate islands are now
  reported as `droppedCharts` / `dropped_charts`, and `chartCount +
  droppedCharts` is the island count. **Behaviour change:** `packed_area` drops
  (torus 0.766 -> 0.355, bumpy sphere 0.755 -> 0.323, sphere 0.552 -> 0.344,
  cube 0.667 -> 0.766 where re-oriented square charts fill their boxes), and
  `chart_count` drops on meshes carrying degenerate faces. Callers gating on the
  old number should re-baseline against the true value.

- **Seam routing ignored convex creases.** `cyber::uv::seamEdgeCost` discounted
  an edge only when its signed dihedral was `>= creaseDegrees`, i.e. valleys
  only — a ridge reports a NEGATIVE dihedral, so on convex-creased models (every
  CAD part in the corpus: fandisk carries 761 convex crease edges against 115
  concave, rocker-arm 2993 against 716) the router degenerated to a plain
  geodesic and the feature did nothing. Creases now discount whichever way they
  bend, with `convexWeight` (`convex_weight` in Python, `convexWeight` in the C
  ABI and Swift) as a separate, tunable multiplier defaulting to `0.8`;
  `creaseDegrees` is now compared against the dihedral's magnitude. Routing
  between the ends of a convex crease chain, the share of routed length that
  actually rides the crease goes 78.7% -> 97.1% on fandisk (41/57 -> 52/57
  chains followed end to end) and 60.0% -> 81.4% on rocker-arm (48/143 ->
  70/143). **Behaviour change:** a route that used to cross a ridge at full
  cost may now follow it. Valleys are unaffected (`concaveWeight` is untouched
  at 0.35 and still strictly cheaper than a ridge), and the conservative
  default means a ridge detour is only taken when it costs less than ~25% extra
  length; set `convex_weight` to 1.0 for the old behaviour, or down toward 0.45
  for aggressive ridge-following (fandisk 100%, 57/57).

- Two real `-Werror` breaks under GCC 13 that made the tree unbuildable on
  current Ubuntu: a signed/unsigned conversion in `bimdf_quantize.cpp`'s trail
  density counters and a lambda parameter shadowing an outer `b` in its T-join
  sort comparator. Neither changes behaviour.

## 0.2.5

### Added

- **Open / non-watertight surfaces now remesh usefully by default.** An open
  island (a surface with a genuine boundary rim) previously under-traced badly at
  low target density — an open paraboloid at a ~900-quad request came out as ~92
  huge faces at ~27° median angle. The `fixHoles` cleanup that fills the
  under-traced gaps is now on by default (opt out with `CYBER_QC_NO_OPEN_CLEANUP`);
  the same request now produces ~1744 uniform quads at ~78° median, edge-length
  CV ~0.27. What had kept it opt-in was an edge-CV blowup traced to `simplifyGraph`
  over-dissolving legitimately-valence-2 isoline samples on open surfaces; the
  cleanup now skips that step on open islands. Closed meshes are byte-identical.

### Changed

- **Sharper feature fidelity on crease-heavy CAD.** The native seamless-UV path
  now preserves crease networks through the isotropic pre-remesh (they were being
  shredded from one connected network into dozens of fragments) and aligns the
  cross field to creases where the surface is genuinely curved (a planarity gate
  keeps flat panels untouched, where alignment would degrade them). Measured on
  fandisk at matched quad count, feature-following error improves 0.75 → 0.62.
  Smooth organic models are byte-identical.

## 0.2.4

### Fixed

- **The isoline extractor closed a boundary loop twice, producing non-manifold
  output.** `IsolineExtractor::fixHoles` calls `fixHoleWithQuads` once
  score-checked and once not, relying on the first pass to consume `hole` — an
  in/out parameter. The terminal 4-gon branch returned with `hole` still
  populated, so a loop that reduced to exactly four edges was closed by the first
  call and closed again, identically, by the second. Two coincident quads are
  edge-count-manifold on their own, so nothing downstream rejected them; the
  pure-quad subdivision then gave each its own face point and turned the shared
  rim into a genuine non-manifold edge. Measured on the default `quad-cover` path
  over 5 models × 5 densities (2600–4200 quads): **stanford-bunny 40 → 0
  non-manifold edges**, with the other four models unaffected (20 of 25 cells
  bit-identical). This was the cause of bunny's long-recorded non-monotone defect
  count — it was never density noise.

### Changed

- **The benchmark now matches on achieved quad count, not on the request.**
  `target_quad_count` is a request the extractors undershoot and QuadriFlow
  overshoots, so a comparison at equal *request* was scoring densities 11–16%
  apart. Since `feature_error` falls roughly as `count^-0.5`, that density gap was
  worth a large fraction of the reported Phase 3 feature-following gaps. Phase 1
  and Phase 3 now drive both arms through a bounded count-match search, and a miss
  is reported rather than silently scored. `examples/test_count_match.py` guards
  the search and is now registered with ctest (it had lived unrun).

## 0.2.3

### Fixed

- **The published Windows zip could not run on a clean machine.** The build uses
  MinGW GCC, so the exe needs `libstdc++-6` / `libgcc_s_seh-1` /
  `libwinpthread-1` beside it; without them the loader fails and it exits 127
  ("command not found") despite the exe being present. CI worked around this by
  putting `/c/mingw64/bin` on PATH for its own smoke test — a distributable
  archive has to carry the DLLs, and now does.
- **macOS package smoke never resolved the CLI.** `hdiutil` output is
  tab-separated and the volume name contains a space ("CyberRemesher 0.2.3"), so
  whitespace-splitting `awk` returned just the version string.
- `mobile-smoke` is manual-only, matching the release lane: the ios/android
  packages are gated off on tags, so on a tag it could only fail looking for
  artifacts nothing produced.

## 0.2.2

### Fixed

- **The release workflow never published anything.** Every tag from v0.1.0 on
  failed: five of six package targets could not build what they were written to
  package, and the publish job needs all of them. A tag push looked like it
  released and silently did not.
  - `ios` / `android` / `windows-installer` are gated to an explicit
    `workflow_dispatch` input — they package an iOS/Android app that does not
    exist (`apps/mobile` is a placeholder) and need a WiX toolset that is not
    installed.
  - `windows-zip` configured the `windows-cuda` preset, which requires nvcc that
    the GitHub windows runner does not have; it now uses `cpu-headless` with
    Ninja's single-config output paths.
  - `macos` copied `apps/desktop/CyberRemesher` unconditionally; that shell is a
    placeholder on every build, so it now falls back to a CLI-only bundle.
- **Every AppImage produced so far was unrunnable.** `AppRun` exec'd
  `CyberRemesher`, which the CLI-only AppDir never contains. It now falls back
  to the CLI.
- The publish step generates release notes from the commit range instead of
  publishing an empty body, and no longer fails when a gated package is absent.

## 0.2.1

### Fixed

- `test_hole_fill_policy` asserted an exact boundary-loop count, which holds on
  the in-process Geogram backend but not on the dependency-free native one, so
  CI failed on builds without `-DCYBER_WITH_QUADCOVER=ON`. It now asserts the
  policy itself (disabling the fill leaves strictly more open boundary than
  enabling it) rather than a backend-specific number. No engine change.
- clang-format violations that had been failing CI on main since the
  manual-retopology layer landed, plus four in `quadcover_extractor.cpp` from
  the M3 hole-fill work.

## 0.2.0

Adds the manual-retopology engine layer, and fixes a remeshing parameter that
never took effect on the default path.

### Added — manual retopology

- **Manual-retopology engine layer behind the C API**, the surface the
  CyberTopology iOS app is built on: 31 new entry points covering the editing
  verbs (create face, tweak, move with geodesic falloff, relax, erase, delete),
  topology operators (insert loop, dissolve edges, merge vertices, rotate edge,
  create grid, subdivide, triangulate), build tools (build face, grow boundary
  edge, distribute path, surface cut, patch clone, extend boundary grid/fan,
  draw strip), symmetry (snap plane, apply, resymmetrize), Target snapping,
  per-element annotation state, and the stroke interpreter. `cyber_capi` now
  links `cyber_retopo`.
- New retopo headers: `loops.hpp` (quad-ring and edge-loop walks),
  `boundary.hpp` (boundary-loop walks), `picking.hpp`, `paths.hpp`,
  `dissolve.hpp`, `loop_metrics.hpp`, `stroke_interpreter.hpp`.
- Portability fixes for iOS: a Metal pipeline-state typo, a `~len` integer
  promotion in the PNG writer, and compiling out the out-of-process QuadCover
  CLI path (no `std::system` on iOS).

### Fixed

- **Benchmark measured the wrong extractor.** `examples/11_benchmark.py` scored
  the retired `instant-meshes` position-field extractor in its comparison
  phases rather than the shipped `quad-cover` default. Every conclusion drawn
  from those phases was about code that is no longer the default.
- **Benchmark could hang indefinitely.** The count-match search applied an
  unbounded multiplicative correction with a 40x ceiling. On models whose quad
  count saturates it escalated to a ~120k-quad request, allocated ~4 GB and
  never returned. Now bounded by a 4x ceiling with a saturation guard;
  regression-tested in `examples/test_count_match.py`.
- **`holeFillMaxBoundary` was ignored by the default `quad-cover` method.** The
  parameter is applied as a post-pass over the quadrangulator's output, but
  quad-cover closes holes *during* extraction against its own hard-coded limit
  of 65 — the same magic constant the spec criticises AutoRemesher for — so the
  caller's policy never took effect. The run's limit is now threaded into the
  extractor itself (`extractIsolineQuads` ->
  `IsolineExtractor::setHoleFillMaxBoundary`). Loops longer than the limit are
  left open as the spec requires, and a value below 3 disables filling.

  This also gives open surfaces a supported way to keep their rim: at
  `hole_fill_max_boundary=0` a sphere with its cap removed retains its
  boundary instead of being silently closed. The default (64) is unchanged, so
  damaged input still repairs to a watertight mesh.

  Corpus quality is byte-identical (benchmark unchanged on all five models) and
  broken-input robustness stays 7/7. Regression-tested in
  `python/cyberremesh/tests/test_hole_fill_policy.py`, covering both directions
  of the parameter and the over-limit case.

### Added

- `CYBER_QC_OPEN_CLEANUP` (experimental, opt-in): runs the isoline graph
  cleanup on open surfaces, with the input rim preserved. Partial — the
  `simplifyGraph` turn-angle guard is not implemented, so it is off by default
  and the default path is byte-unchanged.
- `version_identity` test: the four Python version declarations were unlinked
  from `CMakeLists.txt`, so a release bump could miss one and ship a wheel
  disagreeing with the engine inside it.
- `hole_fill_policy` test: covers the hole-fill policy across quadrangulators,
  including that an over-limit loop stays open when filling is enabled.

### Changed — claims corrected against measurement

- The quad-cover default beats QuadriFlow on median angle **and**
  irregular-vertex count on **3 of 5** corpus models (spot, rocker-arm,
  stanford-bunny), not "across the organic corpus". The earlier framing counted
  mechanical rocker-arm as organic and omitted cheburashka, which loses both
  axes. Corrected in the spec, README, Python API docs and roadmap.
- Topological validity is now stated normatively: the default is at least as
  valid as QuadriFlow on all 5 models, and stays manifold on flat CAD input
  where QuadriFlow tears (cube: 0 defects vs 576 boundary edges at ~2100 quads).
- Feature-following is recorded as a known gap (fandisk 2.0x, cheburashka 2.0x,
  rocker-arm 1.5x) with its root cause — un-pinned integer-grid phase.
- README's stanford-bunny comparison quoted QuadriFlow at 80 deg / 6%
  irregular; measured is 82 deg / 4%.
- `15_uv_vs_xatlas` claimed xatlas packs "~7 pts" tighter; measured is +10 on
  average, +18 at worst. Now derived from the measured rows.

### Known issues

- The opt-in `instant-meshes` extractor still ignores `holeFillMaxBoundary` (it
  never fills). It is a retired alternative, not the default.
- On the dependency-free **native** seamless-UV backend (builds without
  `-DCYBER_WITH_QUADCOVER=ON`), `hole_fill_max_boundary=0` exposes small tears
  the extractor leaves and the hole fill was previously repairing: a solid
  plane with no hole comes out with 6 boundary loops at 0, and 1 at the default
  64. Disabling the fill on that backend therefore yields a torn mesh. The
  in-process Geogram backend does not have this. Fixing it means repairing the
  native extractor rather than relying on the hole fill to hide it.
- Open surfaces are still not first-class: `hole_fill_max_boundary=0` now keeps
  the rim, but the isoline graph cleanup that gives closed surfaces their quad
  quality remains opt-in on open ones (`CYBER_QC_OPEN_CLEANUP`, partial).
- Feature-following trails QuadriFlow (fandisk 2.0x, cheburashka 2.0x,
  rocker-arm 1.5x); the cause is un-pinned integer-grid phase and the fix needs
  per-feature-edge integer constraints in the parameterization.

## 0.1.0

First tagged release: quad remeshing, retopology, UV editing and baking in a
C++20 engine, with the `quad-cover` default quadrangulator, the one-call
automatic UV atlas, and the Python/C ABI bindings.
