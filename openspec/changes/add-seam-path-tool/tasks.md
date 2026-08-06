# Tasks: add-seam-path-tool

- [x] 1. Edge-cost model (feature tags, dihedral/curvature bias, flat-region
       penalty) over the existing shortest-path machinery

  Landed in `src/uv/include/cyber/uv/seam_path.hpp` + `src/uv/src/seam_path.cpp`:
  `SeamCostOptions` (per-unit-length multipliers: flat 1.0, feature 0.25,
  concave 0.35 past a 20° crease, positive floor 1e-3), `edgeSignedDihedral`
  (degrees; **positive in a valley, negative on a ridge**, 0 for flat, boundary
  and non-manifold edges, sign from the orientation-robust symmetric centroid
  test), `seamEdgeCost` = length × cheapest applicable weight, and
  `routeSeamSegment`. Every weight is clamped to a strictly positive floor, so
  no ABI-supplied setting can break Dijkstra's non-negative-weight requirement.

  Reused rather than duplicated: `src/retopo/include/cyber/retopo/paths.hpp`
  gained `template <class WeightFn> weightedVertexPath(...)` — the existing
  Dijkstra body with the hard-coded Euclidean step replaced by a functor — and
  `shortestVertexPath` is now a one-line wrapper over it. Verified
  behaviour-neutral: the full unit run before and after the refactor was
  identical at 331 cases / 127792 assertions, so Path Distribute and
  `cyber_mesh_shortest_vertex_path` are unaffected. `src/uv/CMakeLists.txt`
  takes retopo's include dir WITHOUT a link (both headers are header-only and
  core-only; src/uv configures before src/retopo and `CYBER_BUILD_RETOPO` can
  be OFF) — the `cyber_bake` ↔ `cyber_imageio` precedent. Proven with an
  explicit throwaway `-DCYBER_BUILD_RETOPO=OFF` configure: `cyber_uv` compiles
  and links there. (That configuration still fails later at
  `cyber_capi_shared`, which links `cyber_retopo` unconditionally —
  pre-existing on main, untouched here.)

- [x] 2. Pending-path state: waypoints, per-segment routes, reposition/delete
       with local re-route, commit → seam edges, resume marker semantics

  `cyber::uv::SeamPath`: ordered waypoints, one cached `Segment` per
  consecutive pair (`vertices`, `routed`, `routeRevision`).
  `addWaypoint` / `moveWaypoint` / `moveWaypointTo(index, Vec3, radius)` (drag,
  via `retopo::nearestVertex`) / `removeWaypoint` / `clearPending`.
  **Local re-route isolation is enforced, not incidental:** an edit re-routes
  only the at most two segments touching the waypoint and bumps only their
  `routeRevision`; an interior delete merges its two segments into one
  re-routed span. `commit(SeamSet&)` marks `pendingEdges()` into the existing
  seam model, returns a `SeamCommit` undo record listing **only newly marked**
  edges, arms the resume marker at the last vertex and clears the pending
  waypoints; a not-fully-routed path commits nothing and stays pending for
  repair. `dropResumeMarker()` touches no SeamSet.
  `revertCommit(SeamSet&, SeamCommit)` restores the exact pre-commit state.

  Snapshot semantics (matching `CyberSnapper`) are documented on the class, and
  `commit` re-resolves each edge through `Mesh::edgeBetween` rather than
  trusting the cache. `SeamSet` also gained a deterministic ascending `edges()`
  accessor (the hash set's own iteration order is unspecified).

- [~] 3. ABI tool ops + Python/Swift bindings; undo integration

  **Landed:** 25 additive C entry points in `capi/include/cyber_capi.h` /
  `capi/src/capi.cpp` — `CyberSeamSet` / `CyberSeamPath` opaque handles, a new
  `CyberSeamPathOptions` struct (its own struct, never trailing fields on an
  existing one) with `cyber_default_seam_path_options`,
  `cyber_mesh_edge_signed_dihedral`, and the full `cyber_seam_set_*` /
  `cyber_seam_path_*` surface, all behind `CYBER_CAPI_WITH_UV` with
  no-op/NOT_SUPPORTED behaviour otherwise, exactly like `cyber_uv_atlas`. No
  existing struct layout changed, so clients need no recompile. Python:
  `CyberSeamPathOptions` ctypes mirror + `_declare_seam_path` in `_ffi.py`,
  `SeamCostParams` / `SeamSet` / `SeamPath` wrappers in `api.py` (plus
  `Mesh.edge_signed_dihedral`), exported from `__init__.py`.

  **Undo integration is the `SeamCommit` record + `revertCommit`, NOT an
  `app::UndoStack` command.** `src/app/CMakeLists.txt` states that `cyber_app`
  depends on core alone by design; linking `cyber_uv` into it to host a
  concrete `SeamCommitCommand` would break that layering for one adapter. The
  undoable unit ships fully tested at the uv level and over the ABI (the commit
  call returns the newly marked edge ids; erasing exactly those is the undo),
  and the `app::Command` adapter is left to the shell. **That adapter is not
  done.**

  **Swift is written but NOT compiled:**
  `swift/Sources/CyberRemesher/SeamPath.swift` follows the `Mesh.swift` /
  `SoftSelection.swift` idiom, but no swift/swiftc toolchain exists in this
  environment and no CI job on this branch builds the Swift package
  (`publish-swift.yml` runs only on `swift-v*` tags). It is covered only by the
  source-level parity gate in `python/cyberremesh/tests/test_seam_path.py`,
  which asserts every ABI symbol is referenced there — that proves presence,
  not compilability.

  **App shells: not verified.** `apps/mobile/shared/toolbar.default.json` gained
  a `seampath` action structurally identical to the existing `cut` entry (JSON
  re-parsed to confirm it is well-formed), but that file is self-labelled
  "_unverified — not built/validated in CI", `apps/desktop` contains no C++/UI
  sources at all, and no iPadOS/Android/desktop shell can be run here. No
  interactive verification of the drag gesture under a live viewport either;
  `moveWaypointTo` is tested only at the API/ABI level.

- [x] 4. Tests: groove-following vs geodesic, local re-route isolation,
       commit/resume/drop invariants, seam-model integration (gesture unwrap
       over a routed seam)

  `tests/uv/test_seam_path.cpp` (14 cases), registered in `tests/CMakeLists.txt`
  inside the `if(TARGET cyber_uv)` block:

  - *Route follows the groove* — `makeGrooveGrid` sinks the y==1 row of a 7×5
    grid into a valley; the dog-leg groove is Euclidean-LONGER (8.24) than the
    flat run straight across (6.0). The test asserts (a) unweighted
    `retopo::shortestVertexPath` takes the flat run and it is strictly shorter,
    (b) `routeSeamSegment` / `SeamPath` returns exactly the groove chain, (c)
    the groove's summed `seamEdgeCost` is strictly lower — so the bias, not the
    geometry, does the work. **Mutation-checked:** forcing the weight to a
    constant 1.0 makes 3 cases / 6 assertions fail, so the test cannot pass
    vacuously. A companion case isolates the feature-tag term on flat geometry.
  - *Fix a deviating waypoint* — a 4-waypoint path; after `moveWaypoint(1, …)`
    segments 0 and 1 re-routed and their revisions incremented while segment 2
    is identical **and its revision is unchanged**. Companions: endpoint move
    (exactly one segment), interior delete (merge, tail untouched),
    `moveWaypointTo` snap-or-no-op.
  - *Commit then resume* — commit marks every path edge, arms the marker at the
    last vertex, clears the pending path; SUBCASE (a) a new waypoint seeds the
    route at the marker, SUBCASE (b) `dropResumeMarker()` starts fresh while
    the SeamSet is byte-identical (same size and same per-edge `isSeam` over
    the whole edge capacity).
  - Supporting: cost-model sign/ordering and the positive-floor clamp;
    seam-model integration (a committed route splits the grid into 2 islands
    and `unwrapIslandToUv` unfolds both); unroutable segment (disconnected
    components: `routed()` false, commit marks nothing, then repairable);
    `revertCommit` exactness (a pre-existing seam on the route is not in the
    record and survives the revert); `clearPending`; invalid-input rejection.

  Mirrored end to end at the ABI level in `tests/capi/test_capi.cpp`
  ("capi seam path: route, edit, commit, resume, drop", self-skips when the
  engine is built without UV) and over ctypes in
  `python/cyberremesh/tests/test_seam_path.py` (registered in the ctest python
  foreach; parity gate + functional route/edit/commit/resume/drop, SKIP 77
  convention).

  Results: unit 346 cases / 128085 assertions, 1 failure — the pre-existing
  `tests/quadrangulate/test_seamless_solver.cpp:646` `seamlessUvResidual`.
  Baseline before this change was 331 / 127792 with the same single failure.
  `ctest -j 4`: 16 tests, 14 pass, 2 fail — `unit` (that same case) and `bench`
  (pre-existing sphere singularities/angle regression). Both were verified
  failing on unmodified main and are untouched here.

- [x] 5. Docs + CHANGELOG

  `README.md` gains an "Auto-routed seam paths" section next to the automatic
  UV atlas (routing bias, editable-until-commit with per-segment revisions,
  commit/resume/drop and the undo record, a short Python snippet).
  `CHANGELOG.md` gains an entry under the existing `## Unreleased` /
  `### Added` (added, not replaced), recording the additive ABI and the
  behaviour-identical `shortestVertexPath` → `weightedVertexPath` refactor. No
  version bump — matching how the hole-fill fix was handled.

## Verification notes

- Format gate (`clang-format 18.1.8` over `src apps tests`, the exact CI
  command): clean except the pre-existing `src/core/src/quad/quadcover.cpp`.
  `capi/` is outside the gate, so `cyber_capi.h` was left at its existing
  formatting and only the new block was appended (its diff is +121/-0).
- The build is warning-free for every touched file under the tree's
  `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wold-style-cast`.
- Cognitive complexity: assessed **by reading**, not measured — no `clang-tidy`
  is installed here, so the `cognitive-complexity` skill's C++ analyzer could
  not run. The largest new functions (`edgeSignedDihedral`,
  `SeamPath::removeWaypoint`) have 3–4 branch points; everything sits far
  inside the backend band (~15).
