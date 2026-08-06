# Changelog

> Note: releases 0.3.0, 0.4.0 and 0.5.0 were tagged without changelog entries;
> their content is recorded in `docs/ROADMAP.md`. Entries resume here.

## Unreleased

### Added

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

### Changed

- `CyberBakeParams` gained a trailing `curvatureRange` field. The struct is
  passed by pointer and callers allocate it, so a client compiled against the
  older header must be recompiled; always initialise via
  `cyber_default_bake_params`.
- `cmake/CompilerWarnings.cmake` gained `CYBER_WARNINGS_AS_ERRORS` (default
  `ON`, so CI is unchanged). Toolchains newer than CI's emit diagnostics of
  their own — GCC 13 raises a known `-Wstringop-overflow` false positive from
  inside libstdc++ — and there was previously no way to build the tree locally
  to look at them.

### Fixed

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
