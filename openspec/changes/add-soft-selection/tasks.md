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

- [x] 2. Selection ops: clear/invert/expand/contract/smooth(1|5|10),
       save/load named slots persisted in the document

  **Persistence CLOSED.** It was previously partial: `Document::save/load`
  serialized `softSelections` and the C ABI kept its own slot map on the mesh
  handle, and nothing in the tree moved a slot between them in either direction,
  so a slot saved through the ABI was absent from the saved document and a slot
  in a loaded document was invisible to the ABI. The seam is now a real
  `CyberDocument` handle in the C ABI (`capi/src/capi.cpp`, gated on
  `CYBER_CAPI_WITH_APP` exactly like the UV handles): `cyber_document_create` /
  `_free`, `_set_target` / `_set_edit_mesh`, `_target` / `_edit_mesh`,
  `_slot_count` / `_slot_name` / `_slot_weights`, `_save` / `_load` /
  `_save_file` / `_load_file`. `_set_edit_mesh` copies the handle's slot map into
  the document and `_edit_mesh` hands back a fresh handle carrying it, so a
  selection genuinely survives save → load. Exposed as `cyberremesh.Document`
  (Python) and `Document.swift` (Swift). Proved end to end by
  `tests/capi/test_capi.cpp` ("capi document round-trips named soft-selection
  slots end to end", plus a file round trip) and by `check_document_round_trip`
  in `python/cyberremesh/tests/test_soft_selection.py`.

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

- [x] 3. Weighted transform (T/R/S) + weighted relax with in-operation
       re-snap to the Target

  **Pin gap CLOSED.** `cyber_retopo_selection_transform` used to hard-code
  `nullptr` for the `PinSet`, so through the C ABI, Python and Swift a pinned
  vertex WAS moved by a weighted transform even though the header promised the
  opposite. The additive `cyber_retopo_selection_transform_pinned` now threads
  the list through to `transformWeighted`, and the old entry point is that call
  with an empty list, so no existing signature or struct moved. Bound as
  `Mesh.transform_selection(..., pinned=[...])` in Python and
  `transformSelection(_:snapper:resnapEpsilon:pinned:)` in Swift. The failing
  case — a pinned vertex at weight 1.0 — is pinned by "capi weighted transform
  honours a pin on a high-weight vertex" (`tests/capi/test_capi.cpp`) and
  `check_transform_honours_pins` in the Python suite.

  **Report semantics documented.** `examples/16_soft_selection.py` reported 193
  moved / 180 re-snapped with no account of the 13. Measured: all 13 carry a
  positive but negligible weight (<= 1.8e-33, produced by the one-ring smoothing
  at the far tail of the gradient), so `lerp(pos, xf(pos), w)` rounds to `pos` in
  float — the op writes the same value back (counted in `moved`) and the Target
  re-projection has nothing to correct (excluded from `resnapped`). `moved`
  counts WRITES, not displacements. Stated precisely in the
  `CyberSoftTransformReport` block of `cyber_capi.h`, measured rather than
  asserted by the example, and pinned by "capi weighted transform counts a
  negligible-weight vertex as moved".

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

  **Follow-up landed:** the missing pin route is now
  `cyber_retopo_selection_transform_pinned`, appended rather than added to the
  existing signature so the ABI stays additive, plus the 13 `cyber_document_*`
  entry points that persist the slots (tasks 2 and 3). `check_parity` in
  `python/cyberremesh/tests/test_soft_selection.py` was widened to cover the
  document symbols, so all 31 must stay bound in `_ffi.py`, `api.py` and the
  Swift package.

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
