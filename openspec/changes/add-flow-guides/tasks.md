# Tasks: add-flow-guides

- [x] 1. Guide representation + projection onto the Target (polyline →
       per-triangle tangent constraints with radius falloff)

  Landed as `src/core/include/cyber/core/guidance.hpp` +
  `src/core/src/guidance.cpp`: `FlowGuide`, `DensityField`, `Guidance`, and
  `GuidanceField` — a purely geometric sampler that owns a `ReferenceSurface`,
  snaps every guide point onto the Target (recording the max snap distance) and
  snapshots per-input-triangle density corners the way `isotropic.cpp`'s
  `ScaleField` already does. `guideAt(p, normal)` returns the strongest guide
  weight with a smooth `(1 - (d/r)^2)^2` falloff, an AABB early reject and a
  normal-consistency gate; `densityAt(p)` barycentric-interpolates and clamps.
  Because it is geometric, it survives triangulate / weld / orient / island
  split / isotropic remesh with no index remapping.

  NOT done: geodesic influence. Distance is Euclidean; the normal gate covers
  opposing normals but not a tight fold whose normals agree. Documented in
  `docs/flow-guides.md` as a limitation, not silently.

- [x] 2. Constrained cross-field smoothing term (soft alignment; strength
       parameter); verify no regression on the unguided corpus

  `crossfield.cpp` gains `buildGuideConstraints` (per dense face: weight plus
  the 4-RoSy `(cos 4a, sin 4a)` encoding of the guide tangent projected into the
  face plane) and a blend inside `transportSmooth`'s existing sweep, behind
  `if (!bias.empty() && bias[c].weight > 0)`. An unguided run passes an empty
  vector, so the sweep is textually identical. Threaded through
  `computeCrossField`, `computeCrossFieldFromOrientation` (the current default
  multires path) and `buildSeamlessSetup`. Hard pins still win: a guide reaching
  a pinned face is counted in `CrossField::guideConflictFaces`, never applied
  over the pin.

  Unguided verification, done BEFORE the rest of the change went in and again
  at the end: `ctest -R bench` matches the recorded baseline number for number
  (same two pre-existing sphere failures, same values `singularities 21 -> 37`,
  `angle_dev_mean 8.555 -> 11.45`), and the CLI corpus was re-run and `cmp`'d
  against a pre-change baseline. `fandisk` at 1000 and 4000 quads is
  byte-for-byte identical and is deterministic (4 runs, 1 distinct output).

  `spot` and `stanford-bunny` could NOT be used as oracles: on this build
  (`CYBER_WITH_QUADCOVER=ON`) they route to the vendored Geogram solve, which is
  run-to-run non-deterministic on unmodified `main` — 6 identical `spot`
  invocations against the stock binary produced 6 distinct outputs. That is a
  pre-existing property of the vendored route, verified by stashing this change
  and rebuilding; it is recorded rather than papered over, and the byte-identity
  claim rests instead on `fandisk`, on `ctest -R bench`, and on the in-process
  bit-comparisons (cross-field `real`/`imag`, two `remesh()` runs compared as
  raw float bits plus face lists, and a CLI `cmp` on the deterministic
  field-aligned path).

- [x] 3. Density field plumbing into target sizing (clamp, report)

  Three sizing paths honor density:
  - `IsotropicOptions::density` → `ScaleField` multiplies its result by
    `1/sqrt(density)` and its uniform early-out now also requires a null density
    pointer. With a null pointer the arithmetic is unchanged (`x * 1.0f == x`).
  - The native seamless solve: `solveParameterizationImpl` caches a per-face
    `densityScale = sqrt(density)` on `FaceData` and assembles the RHS with
    `invS * densityScale`. It touches the RHS ONLY, exactly like `spacing`
    already does, so `SeamlessSolveCache`'s factorizations stay valid across the
    calibration attempts. Also fed to the quad-cover isotropic pre-remesh (that
    backend skips the pipeline's isotropic stage).
  - The pure-quad shape-match relax scales its uniform square radius by
    `1/sqrt(density)` so it does not re-uniformize a painted region.

  Clamps live in `validateGuidance` (`remesh_params.{hpp,cpp}`): one aggregated
  issue naming the clamped count and the effective range, not a per-value flood.
  Measured: painting 4.0 on half a plane gives a painted/unpainted mean edge
  ratio of **0.5006** against the documented `1/sqrt(4) = 0.5`.

  Re-verified after review: a post-clamp neutral density is dropped at the one
  shared validation gate (`remesh_params.cpp:165-177`, reached from
  `pipeline.cpp:552` for every entry point) with a non-fatal issue, so density
  1.0 everywhere cannot change the route or the mesh. Both byte-identity cases
  pass on this build, including the one on the shipped default backend
  (`tests/core/test_flow_guides_pipeline.cpp:262`, `:276`).

- [~] 4. Per-island loud reporting for unhonored guidance; routing audit for
       both backends

  **PARTIAL — the audit has one measured hole: `CYBER_QC_NO_NATIVE` drops
  guidance silently.** That env kill switch clears `haveNative`
  (`quadcover_extractor.cpp:1249`), which drops out of the `routeNative`
  condition at `1265-1269`, so a guided island goes to the vendored Geogram
  solve WITHOUT passing through the `ctx->guidanceHonored = false` assignment at
  `1280` — and `NativeSolveContext::guidanceHonored` defaults to `true`
  (`quadcover_extractor.hpp:103`). `m_unhonored` therefore stays empty
  (`3543-3549`) and the pipeline records `guidesHonored = true`
  (`pipeline.cpp:808-811`). Measured on `fandisk --target-quads 600` with one
  guide: the report says `"guidesHonored": true, "reason": ""` both with and
  without `CYBER_QC_NO_NATIVE=1`, while the output changes (12 vs 79 non-quads,
  0.27s vs 1.55s) — i.e. the run took the vendored route, where every guidance
  hook (`iso.density`, the cross-field bias, the seamless RHS) lives inside
  `prepareNativeSolve` and is never reached. Everything else in this task is
  done and tested; the unhonored path is covered only by a test-double backend
  (`tests/core/test_flow_guides_pipeline.cpp:363`), never by the real
  quad-cover route.

  `IQuadrangulator::acceptGuidance` defaults to DECLINING with a reason, so
  greedy / instant-meshes / integer report rather than silently ignore.
  `IQuadrangulator::unhonoredGuidance()` reports guidance accepted up front but
  lost at run time. `PipelineResult::islandGuidance` carries one row per island
  (`guidesInRange`, `guidesHonored`, `densityHonored`, `reason`) and is filled
  on every loop exit, including islands that failed before quadrangulation.

  Routing audit: quad-cover accepts and FORCES the native seamless route,
  because the vendored Geogram solve builds its own frame field from its own
  scalar density inside sources we do not patch. If native declines and the
  vendored path runs, the island reports "the vendored Geogram quad_cover solve
  has no guide/density hook". The calibration probe is enabled when guidance
  forces native even where the vendored solver is available, so guided runs do
  not start at the hardcoded 0.5 scaling. Field-aligned accepts guides
  (its own `computeCrossField` call) and density (via the pipeline's isotropic
  stage).

  Note (measured, documented in `docs/flow-guides.md`): the forced native route
  is a real quality trade on `-DCYBER_WITH_QUADCOVER=ON` builds — native sits at
  ~4-5% irregular where the vendored solve reaches 1-4% on smooth organics.

- [~] 5. Reachability: C ABI, Python, bridge message, CLI sidecar file format

  Landed:
  - **C ABI**: new `cyber_remesh_guided` + `CyberFlowGuide` / `CyberGuidance` /
    `CyberWarningCb`. `cyber_remesh` and `CyberRemeshParams` are untouched and
    ABI-identical (the Python ctypes mirror and the Swift binding depend on the
    layout); both entry points share one static implementation.
  - **Python**: `FlowGuide` dataclass, `remesh(..., guides=, density=,
    density_per_face=)`, warnings attached as `Mesh.guidance_warnings` AND
    re-raised through `warnings.warn`. Exported from `__init__`.
  - **Bridge**: `push_guides` / `pull_guides` / `clear_guides` / `push_density`
    / `pull_density` on `BridgeSession` + `message.cpp`, mutex-guarded, cleared
    by `clear_scene` and `close_document`. `kProtocolVersion` is deliberately
    NOT bumped: adding commands is backward compatible, bumping would reject
    every existing client.
  - **CLI**: `--guides <file.json>` sidecar (`{"version":1,"guides":[...],
    "density":{"perVertex":[...]}}`) plus a `guidance` block in `--report`
    carrying post-clamp values, the clamp range, the issues and the per-island
    rows. A missing / unreadable / malformed sidecar, an unknown `version`, or a
    fatal validation issue is exit 2 naming the file; unhonored guidance also
    goes to stderr unless `--quiet`.

  PARTIAL because: the bridge messages are round-tripped in-process
  (`tests/net/test_bridge.cpp`) but **not exercised from a live Blender / Unity /
  Unreal add-on session** — no DCC can be driven from this build environment.
  The Swift binding was not extended either; it still sees only `cyber_remesh`,
  which is unchanged and keeps working.

  Also partial, and previously undisclosed: **no ctest-registered Python test
  covers the guidance surface.** Every sibling change ships one
  (`tests/CMakeLists.txt:155-158` registers `test_soft_selection`,
  `test_seam_path`, `test_handoff`, `test_export_presets`, …); there is no
  `test_flow_guides.py`, and no existing python test passes `guides=` or
  `density=`. The surface does work — `remesh(m, p, guides=[FlowGuide(...)])`
  returns a guided mesh and re-raises `island 0: 1 guide(s) in range, guides
  honored, density NOT honored`, and a neutral density warns "ignored" — but the
  only thing exercising it is `examples/17_flow_guides.py`, which ctest does not
  run.

- [~] 6. Guided test corpus + exit gate measurement (≤15° mean deviation in
       radius; unguided metrics unchanged)

  **The gate is NOT met corpus-wide. Reporting the measured numbers, not a
  tuned threshold.**

  `examples/17_flow_guides.py` draws a guide along `spot` and `stanford-bunny`
  at 2000 quads on the default (quad-cover) backend and measures the mean
  minimum angle mod 90 between the guide tangent and each in-radius output
  face's edge directions (random baseline 22.5):

  | model | radius | unguided | guided | gate |
  |---|---|---|---|---|
  | spot | 0.03 x diag | 30.01 | 18.17 | not met |
  | stanford-bunny | 0.03 x diag | 15.55 | 13.15 | met |
  | spot | 0.06 x diag | 26.74 | 25.10 | not met |
  | stanford-bunny | 0.06 x diag | 15.40 | 11.31 | met |
  | spot | 0.12 x diag | 24.08 | 19.69 | not met |
  | stanford-bunny | 0.12 x diag | 16.06 | 15.63 | not met |

  **Mean guided 17.17 deg, worst 25.10 deg; 2 of 6 runs at or under 15.**
  Guides improve alignment on every single run. The constraint is strong at the
  field stage — a flat grid with a 45-degree guide goes from 44.84 to 11.29
  degrees off the tangent — and end to end through the field-aligned backend on
  a tube it goes from 12.08 to 9.23 degrees (that C++ case asserts `<= 15`, and
  it passes at its measured 9.23). The loss is dilution between the cross field
  and the extracted mesh: the seamless Poisson solve, integer rounding and
  isoline extraction each shed angular fidelity. Carrying the guide into the
  seamless solve as a per-face target-frame ROTATION (not only as a field seed)
  is the identified follow-up; it is not part of this change.

  Unguided metrics unchanged: `ctest -R bench` matches the recorded baseline
  exactly and the `fandisk` corpus OBJs are byte-identical (see task 2 for why
  `spot` / `stanford-bunny` cannot serve as byte oracles on this build).

- [x] 7. Docs + CHANGELOG

  `docs/flow-guides.md` documents the sizing relation, the clamp ranges and
  defaults, the sidecar JSON schema, the bridge commands, the per-backend
  honor table, the forced native route and its quality trade, the soft-vs-hard
  constraint rule, the Euclidean-influence limitation, and the measured exit
  gate with its honest NOT-MET verdict. `CHANGELOG.md` gains an entry under
  `## Unreleased` / `### Added` stating the same measured number and the
  byte-identity guarantee. `README.md` was left alone: no existing documented
  surface changed (`cyber_remesh`, `CyberRemeshParams` and every current CLI
  flag behave exactly as before).
