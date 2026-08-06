# Tasks: add-soft-selection

- [x] 1. Weight computation: line gradient (with 15° snap), sphere falloff,
       painted accumulation (+ subtract), stored per-vertex on EditMesh

  Landed in `src/retopo/include/cyber/retopo/soft_selection.hpp` (header-only,
  matching pins/relax/move/build_tools). `SoftSelection` holds a `std::vector<float>`
  indexed by `VertexId`, clamped to [0,1] on every write. `selectLine` ramps
  `applyFalloff(t)` with `t = dot(p - A, axis)/|axis|²` clamped to [0,1] — 0 at A,
  1 at B, 1 beyond B, 0 behind A. `snapDirectionToIncrement` quantizes the drag to
  15° increments **in the plane perpendicular to a caller-supplied `viewDir`** (the
  convention `drawStripPath`/`surfaceCutSegment` already use); the choice is
  documented in the header and in the ABI comment because a world-axis convention
  would be a different semantic. `selectSphere` and `paintSelection` /
  `paintSelectionStroke` (AABB-prefiltered) round it out, with four falloff curves
  (Linear/Smooth/Sharp/Round).

- [~] 2. Selection ops: clear/invert/expand/contract/smooth(1|5|10),
       save/load named slots persisted in the document

  **PARTIAL — the selection ops are done; "persisted in the document" is NOT.**
  Both halves exist and neither is connected to the other: `Document::save/load`
  serializes `softSelections` (`src/app/src/document.cpp:205-215`, `269-276`) and
  the C ABI keeps a slot map on the mesh handle (`capi/src/capi.cpp:111`,
  `2602-2640`), but nothing moves a slot from one to the other. Outside
  `tests/app/test_app.cpp` no code anywhere writes `Document::softSelections`
  (only hits: `document.hpp:77`, `document.cpp`, that test), and `cyber_capi.h`
  exposes no document surface at all, so the ABI slot map dies with the handle.
  No selection can actually persist today; the shell that would bridge them does
  not exist (see task 6).

  `clearSelection`, `invertSelection`, `expandSelection`/`contractSelection`
  (double-buffered morphological max/min over the closed one-ring) and
  `smoothSelection` (double-buffered closed-ring average). 1/5/10 are argument
  values, not an enum. Named slots: `cyber::app::Document::softSelections`
  (`std::map<std::string, std::vector<float>>`) persisted as **section id 5**,
  written only when non-empty, with `kFormatVersion` deliberately unchanged —
  `load` rejects `version > kFormatVersion`, so bumping it would make older
  binaries refuse files they can in fact read, while the existing unknown-section
  skip already gives forward compatibility. The C ABI keeps a session-lifetime
  slot map on the mesh handle (`_save`/`_load`/`_slot_count`/`_slot_name`); the
  shell is what moves those into the Document.

- [~] 3. Weighted transform (T/R/S) + weighted relax with in-operation
       re-snap to the Target

  **PARTIAL — the engine half is complete; the pin half of the documented
  invariant is C++-only.** `cyber_retopo_selection_transform` takes no `pinned`
  argument and passes `nullptr` for pins (`capi/src/capi.cpp:2653-2654`), unlike
  `cyber_retopo_selection_relax` (`2667-2670`) and every other mutating op
  (`cyber_retopo_move` 1520, `_relax` 1544, `_transform_vertices` 2337), so
  through the C ABI, Python and Swift a pinned vertex IS moved by a weighted
  transform. `cyber_capi.h:928-930` nevertheless promises "a vertex whose weight
  is 0 (**and any pinned vertex**) is skipped entirely" for the whole
  soft-selection section — that half of the header contract is unreachable, and
  the header has been corrected to say so pending the missing parameter. The
  zero-weight half holds everywhere and is tested.

  `transformWeighted(mesh, selection, Affine, snapper, pins, resnapEpsilon)` and
  `relaxWeighted(...)`, both returning `retopo::ResnapReport {moved, resnapped,
  maxSnapDistance}` (new struct in `snapping.hpp`). Re-projection happens in the
  same pass as the move — there is no separate snap call, and the ABI comment says
  so explicitly. Vertices with weight <= 0 and pinned vertices are skipped
  entirely, which is what makes the zero-weight bit-identity invariant true (an
  untouched vertex is never re-snapped either).

  `relax.hpp` was refactored first: the sweep body became
  `detail::relaxSweep(...)` templated on an extra-weight functor, with `relax()`
  a one-line call passing a constant 1. Verified behaviour-neutral BEFORE any
  feature code went in (full `cyber_tests` run showed only the pre-existing
  `seamlessUvResidual` failure), and a regression case
  ("weighted relax reproduces plain relax when every weight is 1") pins it.

- [~] 4. C ABI + stroke route for painted mode; Python + Swift bindings;
       parity gate entries

  **Landed:** 17 `cyber_retopo_selection_*` entry points in
  `capi/include/cyber_capi.h` + `capi/src/capi.cpp`, including the painted-mode
  stroke route (`_paint_stroke` takes x,y,z,pressure quadruplets). The weight
  field and slot map live on `struct CyberMesh` next to `hiddenFaces`/`taggedEdges`
  and are cleared where those are (subdivide, which reassigns every id).
  Selection-only ops go through a new `runSelectionOp` that skips render-cache
  invalidation — they touch no geometry, so a paint stroke must not force a
  render re-upload per sample. Python: full `_ffi.py` declarations (plus the
  previously unbound `cyber_snapper_create`/`_free`), a `Snapper` class and 16
  `Mesh` methods in `api.py`, all re-exported from the package.

  **Partial — the ABI surface is not complete:**
  `cyber_retopo_selection_transform` has no `pinned` / `pinned_count` parameter
  (`capi/include/cyber_capi.h:1042-1044`) and passes `nullptr` to
  `transformWeighted` (`capi/src/capi.cpp:2654`), so the pin support the engine
  function offers is unreachable from any binding — see task 3. Adding the two
  arguments is an ABI change and is left as a follow-up rather than slipped in
  here.

  **Partial:** `swift/Sources/CyberRemesher/SoftSelection.swift` is written
  against the real ABI but **was not compiled** — there is no Swift toolchain in
  this environment (`swift`/`swiftc` are absent) and the whole `swift/` package is
  already self-declared UNVERIFIED and excluded from headless CI. It carries the
  package's standard UNVERIFIED banner. Note the rest of that package is written
  against a *different* (README-described) ABI shape — e.g. it calls
  `cyber_mesh_create_indexed` and `CYBER_STATUS_OK`, neither of which exists in
  `cyber_capi.h`. `SoftSelection.swift` follows the real header, so it does not
  inherit that divergence, but the package as a whole still would not compile.

  **Not done:** "parity gate entries" — there is **no project-wide binding-parity
  gate to register with**. I searched `src`, `tests`, `tools` and `.github`: no
  parity script, no CI job, no pending-registration mechanism, despite the
  engine-bindings spec describing one as if it exists. Instead I built a real,
  runnable source-level gate for this change's surface
  (`python/cyberremesh/tests/test_soft_selection.py::check_parity`), registered it
  in ctest, and it fails loudly if a symbol is added to the header without being
  bound in `_ffi.py`, `api.py` and `SoftSelection.swift`.

- [x] 5. Tests: ramp/saturation, zero-weight immobility, glue invariant,
       save/load round-trip

  `tests/retopo/test_soft_selection.cpp` (11 cases as landed, 13 today — review
  added the non-finite-weight regressions) covers every scenario in the
  delta: line ramp/saturation across all four curves, 15° snapping (including the
  fixed-point and no-snap controls), sphere falloff, paint accumulate/saturate/
  subtract/clamp plus stroke-vs-per-dab equivalence, clear/invert,
  expand/contract, smooth-by-1/5/10 widening the transition with **positions
  compared bit-identical before and after**, the cone-limb taper (per-ring radial
  reduction strictly increasing along the gradient AND every affected vertex
  within 1e-8 squared distance of the Target in one call), weighted relax glue,
  and zero-weight/pin bit-identity across translate/rotate/scale and relax, each
  with and without a snapper.

  `tests/app/test_app.cpp` gains four cases: slot round-trip, the empty-map
  byte-layout check (4 sections, ids 1..4, version unchanged), unknown-section
  forward compatibility (the slot section's id rewritten to one no reader knows
  → load still succeeds with the rest intact), and rejection of an absurd weight
  count. `tests/capi/test_capi.cpp` gains two C-level cases driving the ABI.
  `python/cyberremesh/tests/test_soft_selection.py` drives the Python surface
  functionally plus the parity gate. All registered in `tests/CMakeLists.txt`.

  Full `ctest -j 4`: 13/15 pass; the only failures are the two verified
  pre-existing ones (`unit`'s `seamlessUvResidual`, and `bench`). `cyber_tests`
  is 331 cases / 330 passed / 1 failed (the pre-existing one).

- [~] 6. Docs + CHANGELOG

  **Landed:** CHANGELOG entry under `## Unreleased` / `### Added`;
  `examples/16_soft_selection.py` (runs, prints the weight histogram, the
  re-snap report and a 6.7e-08 worst off-surface distance after the taper) —
  `examples/run_all.py` enumerates only 01–09, so like 10–15 it is not
  registered there; a `soft-selection` action entry in
  `apps/mobile/shared/toolbar.default.json`.

  **Not done / not verified:**
  - README was not touched: it documents the CLI and the remesh/atlas surface,
    neither of which changed.
  - **The app shells do not consume the feature.** `apps/desktop` is an empty
    directory and `apps/mobile/{ipados,android}` need SwiftPM/Gradle, neither of
    which exists here. The toolbar entry is the surface a shell *would* call, in
    a file that self-declares `_unverified` and that nothing in CI validates. The
    proposal's "app shells consume it for their pose/taper tools" is therefore
    NOT delivered.
  - **The interactive-performance floor was not measured.** `selectSphere` /
    `paintSelection` are O(V) per call, so a long stroke on a 100k-vertex
    EditMesh is O(V × samples); the stroke entry point prefilters by the stroke
    AABB grown by the radius, but no benchmark for the < 33 ms floor exists in
    the tree and none was written, so no claim is made.
  - **CLI and network bridge are untouched.** Neither surface is named in the
    spec deltas and the CLI has no retopo command at all today (the same gap
    `add-curvature-bake` recorded for baking). Recorded as a gap, not skipped
    silently.
