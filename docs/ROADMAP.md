# Retopology Roadmap — Beating QuadriFlow-quality

Goal: make CyberRemesher's automatic quad retopology **better than QuadriFlow**
across four axes — quality-per-polygon, median quad angle, feature/CAD fidelity,
and robustness — not just competitive on one.

## Update — 2026-08-01 (CI scoreboard lands; CLI was measuring the wrong solver; isotropic adaptivity explosions fixed)

- **`tools/bench/` benchmark harness** (new): deterministic generated corpus +
  sha256-pinned real models, recorded-baseline regression gate in ctest
  (`bench`), external competitor binaries (QuadriFlow, Instant Meshes,
  **quadwild-bimdf** — the current open-source quality bar, run as a GPL
  binary only), and **edge-flow loop metrics** (`flow_turning_mean`,
  `flow_loop_mean_len`) that quantify the wavy-flow/spiral axis the shape
  metrics miss. Complements `examples/10_vs_reference.py`.
- **The CLI hardcoded `field-aligned`** — every CLI run (and anything
  benchmarking through it) measured the retired method, not the documented
  quad-cover default. Fixed with `--quad-method` (default quad-cover). On
  spot/3000/adaptivity-0 this alone moves singularities 854 → 179 and mean
  corner-angle deviation 28.4° → 11.2°.
- **Isotropic adaptivity explosions fixed** (25k+ quads for a 600-quad capped
  cylinder, benchmark-caught; affects every method that runs the pipeline
  isotropic stage): crease angle defect excluded from the curvature source,
  Laplacian smoothing replaced by a gradation-limited sizing field, the scale
  field made Eulerian (sampled from the `ReferenceSurface`, not carried on
  drifting vertices), and feature tagging given an epsilon so exactly-90°
  dihedrals tag deterministically. Regression test in `test_pipeline.cpp`.
- **Native-solve perf (2026-08-01, later): calibration loop de-duplicated +
  probe-predicted initial scaling.** Two commits on the quad-cover path:
  - *Byte-identical hoist:* the isotropic pre-remesh, cross field and
    `buildSeamlessSetup` depend only on (mesh, edge length, adaptivity, feature
    threshold) — never on the calibration `scaling` — so they now compute once
    (`NativeSolveContext`) and each calibration attempt re-runs only the
    spacing-dependent solve + extraction. sha256 of the output OBJ verified
    unchanged on all 7 corpus meshes. nefertiti@8000 220s → 203s,
    armadillo@8000 98s → 82s.
  - *Calibration probe (default-on, kill switch `CYBER_QC_NO_PROBE`):* the
    hardcoded 0.5 initial scaling overshoots the extracted count 1.5-3x on
    every corpus mesh, forcing a second full solve. A relaxed-Poisson-only
    probe (`relaxedCellArea`, ~3-9% of a full solve) predicts the mesh-specific
    scaling `s0 = sqrt(eta·cells/target)`; eta defaults to the
    corpus-calibrated 1.0 for the relaxed triangle-area measure
    (`CYBER_QC_EXTRACT_EFF` overrides), and the 2-attempt loop stays as the
    safety net. 5/7 corpus meshes (incl. every expensive one) now land in ONE
    solve: nefertiti 220s → **117s**, armadillo 98s → **52s** (sphere/torus,
    whose ARAP polish grows UV area ~1.65x, still take 2 — same as before).
    Counts land 0.87-0.99 of target (window 0.75-1.33); bench check green,
    box_sharp keeps its perfect grid (8 cones, 0° angle dev), spot improves
    (sing 72→63); nefertiti/armadillo drift ≤5% on sing/angle. Vendored route
    untouched (probe is native-only); outputs with `CYBER_QC_NO_PROBE=1`
    byte-match the pre-probe build.
- **Gap #2, flow-loop length (2026-08-02): the loop killers are the residual
  triangles, then dipoles; quantization is what remains.** Loop-termination
  census on spot: 47 leftover triangles account for essentially all 118 open
  loop ends (quad-dominant default, mean loop 41). `--pure-quads` removes
  them: mean loop 41 → **319** at ratio 1.000 with angle IMPROVING 6.7→6.5
  (the bench now carries a `cyber-pure` solver row — QuadriFlow/quadwild are
  pure-quad, so the quad-dominant comparison understated us structurally).
  With triangles gone the dipole canceller's effect unmasks: spot loops
  319 → **433** (+36%) at sing 95→93 — the stack is 41 → 433 (10.5×).
  Corpus cyber-pure: nefertiti sing 441 / recall 0.89, armadillo sing 404 /
  recall 0.77 (recall dips on armadillo under the pure post-pass — noted).
  QuadriFlow spot sits at 1811: the remaining ~4× is global grid structure —
  the Bi-MDF-style quantization lever, unchanged as the endgame for this gap.
- **Native-solve perf (2026-08-02): direct sparse-Cholesky solve
  (`CYBER_QC_DIRECT`, default on; kill switch `CYBER_QC_NO_DIRECT`).** The two
  solve operators are fixed across all their re-solves, so both are now
  factored ONCE by an in-tree double-precision simplicial LL^T with RCM
  ordering (`sparse_cholesky.{hpp,cpp}`, dependency-free — the license audit
  stays clean) and cached in `NativeSolveContext` across the probe and every
  calibration attempt (spacing only scales the RHS):
  - *Phase 1 — pinned Poisson:* every `relaxedSolve` (initial + up to 6 ARAP
    re-solves, ×2 coordinates) becomes a pair of exact back-substitutions
    instead of cold-tolerance float CG. The probe's relaxed phase on
    nefertiti@8000 drops 8.5s → 0.07s.
  - *Phase 2 — reduced integer phase:* the reduced operator M = Tᵗ·L₂·T is
    formed explicitly (fill is benign: nefertiti nnz(M)=340k → factor 18.0M,
    armadillo 328k → 11.4M, spot 35k → 0.39M), factored once, and the ~50-77
    masked CG re-solves of the greedy rounding collapse into exact bordered
    (Woodbury/KKT) solves on the ≤2813 pinned integer DOF — same fixed point
    as `maskedSolve`, greedy pin schedule untouched, `totalCg` 80408 → 0 on
    nefertiti. `CYBER_QC_FUSED_OPERATOR` keeps the CG loop on the explicit M
    (one spmv/iter) as the fill-pathology fallback.
  - *Measured (M2 Max, Release):* nefertiti@8000 wall 118.9s → **36.7s**
    (solve 95.5s → 21.7s, 4.4x); armadillo@8000 wall 57.8s → **24.6s** (solve
    36.8s → 8.1s, 4.5x); spot@3000 solve 270ms → 49ms. Remaining solve cost on
    nefertiti is the one-time inverse-int-block build (D=11.6s, factor=3.1s) —
    an AMD ordering is the next lever there.
  - *Gates:* ctest 13/13, `bench.py check` green (box_sharp 8 cones / 0° angle
    / recall 1.00). Full A/B (generated corpus + spot/nefertiti/armadillo,
    direct vs `CYBER_QC_NO_DIRECT`): generated corpus metric-identical; real
    meshes within noise (sing: spot 63→64, nefertiti 659→664, armadillo
    609→597; angle/hausdorff/recall/flow all ≤±2%, no bench-tolerance
    violation). Kill-switch output verified content-identical to the
    pre-change build (armadillo@8000 sha256, modulo the OBJ's self-referential
    mtllib line). Numerical drift `CYBER_QC_DIRECT_CHECK`: max
    |uv_direct − uv_cg| = 9.2e-5 on spot (the CG truncation error removed).
- **Feature-pinning lever (in progress, OPT-IN):** `CYBER_QC_FEATURE_DEG=90
  CYBER_QC_PLANAR_FILL=1 CYBER_QC_UV_SNAP=1` lifts box_sharp recall
  **0.04 → 0.73** (organics neutral: spot 179→168 sing, recall 0.59→0.64) via
  three pieces: (a) the native solve-binding feature tag honors the caller
  (was hardcoded 40° — 90° CAD edges were invisible to seams/field pins;
  plumbed through `makeQuadCoverQuadrangulator(..., featureDegrees)`);
  (b) NEW feature-seam integer pinning in `solveSeamlessReduced` (per feature
  seam: level-set row + promoted integer `c_e`; `CYBER_QC_NO_FEATURE_PIN`) —
  seams WITHOUT pinning regress (patch grids disagree, recall stays 0.04 at
  angle 32.5°): pin + seam must ship together; (c) planar flood-fill of
  feature pins across coplanar regions (the missing alternative to lever c2's
  gate: extend the pin as a constant field over the whole flat patch instead
  of disabling it — the pinned-ring-vs-diagonal-interior conflict is what
  regressed the cube in c2's ungated variant).
  **Blocker before default-on:** with feature seams the solved map SHEARS
  (box angle 12.5°→36°, sing 48→86, cylinder recall 1.00→0.58). Localized by
  instrumentation: field 100% axis-aligned, cone census exactly the 8 corner
  cones, targets exact, relaxed CG converged (120 iters) — yet the relaxed
  map is diagonal and the reduced phase only partially recovers. Mixed-axis
  targets (162 x̂/126 ŷ on one flat patch) come from the cone-spanning CUT
  TREE routing through patch interiors (legitimate seams, but they thread
  flat panels). Disproven en route: ARAP polish (map-level A/B, exonerated;
  `CYBER_QC_NO_ARAP` added), angle()/direction() branch mismatch (fix
  measured WORSE, reverted). Live leads, in order: (1) route the
  cone-spanning cut tree ALONG feature curves instead of through flat patch
  interiors (creases are already cuts — the tree between corner cones can
  follow them; no interior seams ⇒ per-patch solves become exact grids);
  (2) the greedy rounding order with ~144 pinned `c_e` integers (round crease
  constants first / merge per crease chain).
  - **SHEAR BLOCKER SOLVED (2026-08-01, later): three compounding solver bugs,
    all lever-gated fixes in `seamless_solver.cpp` + one tag filter in
    `quadcover_extractor.cpp`.** Lever-on now: box_sharp recall **0.73 → 1.00**
    at angle 36.2° → **0.00002°**, sing 86 → **8** (the corner cones, all-quad);
    cylinder recall 0.46 → **0.98**, hausdorff p99 0.022 → **0.0052**, sing 10.
    Organics neutral-to-better vs lever-off: spot sing 64→49 angle 17.1→8.5
    (improves), nefertiti sing 73→73 angle 28.4→27.5, armadillo sing 149→157
    (+5.4%) angle 17.3→16.5. Lever-off bit-compatible (ctest 13/13,
    `bench check` OK, no baseline re-record). Root causes, each measured:
    1. **Combed-target branch mismatch** (the shear itself): the comb and the
       period jumps live on `CrossField::angle()` (θ∈[0,90°)), but the per-face
       target frame `e0` was reconstructed from `direction()` (θ∈(-45°,45°]) —
       a per-face quarter-turn offset wherever raw θ<0, i.e. mixed x̂/ŷ targets
       on ONE coplanar patch (box top measured 178/110) whose Poisson
       compromise is the uniform ~32° diagonal. Fix: `combedDirection()`
       reconstructs on the comb's own angle() convention. Relaxed-phase grad
       histogram goes 100% of +z faces in the 30–35° bin → 100% in the 0–5°
       bin. (The earlier "measured WORSE, reverted" attempt was this fix
       WITHOUT #2/#3 — consistent targets make the wrong seam rho bite harder;
       the trio must ship together.)
    2. **Seam transition rho had the comb difference negated**: combed frame of
       B = frame of A rotated by Δ = p + comb[B] − comb[A] quarter-turns, and a
       grid whose frame rotates by +Δ has coordinates rotating by −Δ, so
       uv_B = R^(−Δ) uv_A + t. The code used comb[B] − comb[A] − p — wrong by a
       half-turn whenever the comb difference is odd (reverses the along-crease
       coordinate; the reduced phase then destroys the now-perfect relaxed map:
       recall 0.08, hausdorff 0.54 with #1 alone).
    3. **periodJump was never computed for feature edges** (historically fine —
       the comb never crosses them) — but rho needs the crease's intrinsic
       field jump p. Without it: box recall 0.10, cylinder 0.51 even with #1+#2.
    4. **Feature re-tag noise filter** (`resolvableCreaseSegments` +
       `filterFeatureEdgesToReference`): `CYBER_QC_FEATURE_DEG=90` re-tags the
       COARSE remesh, and organic scans have plenty of over-threshold dihedrals
       that are sampling artifacts, not creases (nefertiti lever-on sing 216 at
       the previous state). Keep a re-tagged edge only if it traces the ORIGINAL
       mesh's sharp network restricted to chains ≥ 2 output cells long, or still
       qualifies at the historical lever-off knife-edge threshold — so the
       lever-on feature set degrades exactly to the lever-off one on organics
       (nefertiti featureEdges 1518 = lever-off set, sing back to 73).
    - **NEW DISPROVEN (do not retry): routing the cut tree along creases**
      (0/1-Dijkstra, feature edges cost 0 — old live lead #1). Unnecessary for
      CAD once #1–#3 are in (box/cylinder identical with plain BFS) and it
      REGRESSES organics with knife-edge wrinkle networks (nefertiti sing
      80 → 205: the tree snakes along wrinkles and shreds the map). Also
      disproven twice earlier as pre-seeding (cylinder hausdorff 0.41 — breaks
      the disk-opening invariant). The greedy-rounding lead (#2 above) was
      never needed: with correct rho the box solve leaves only 3 free integers
      and the rounding is exact.
    - Diagnostics that found it now ship behind `CYBER_QC_FIELD_STATS`:
      per-patch axis-mix census, non-feature cut-edge census, per-phase
      grad(u) deviation histograms (relaxed vs reduced).
    - **DEFAULT-ON (2026-08-01, final):** the lever now ships as the default.
      Activation is value+topology-based, not env-based: the CLI binds
      `--sharp-edge` (default 90) into the native solve, and feature binding
      engages only when (a) the effective threshold is wider than the
      historical knife-edge, (b) the filtered remesh actually carries interior
      feature edges, and (c) the surface is CLOSED (same boundary gate as the
      ARAP polish — binding features on boundaried scans measurably blew up
      the open-surface cleanup suite and is future work). Featureless and open
      meshes take the historical path; the planar flood fill seeds ONLY from
      crease pins (spreading boundary pins regressed open flat scans). Env
      trio retired in favor of kill switches: `CYBER_QC_FEATURE_DEG=40`
      restores knife-edge binding, `CYBER_QC_NO_PLANAR_FILL`,
      `CYBER_QC_NO_UV_SNAP`, `CYBER_QC_NO_FEATURE_PIN`. Final defaults, target
      600: box_sharp recall **1.000**, sing **8**, angle **0.00°**, pure quads;
      cylinder recall **0.945**, sing 9, hausdorff 0.0053. Full suite 13/13,
      bench gate green; baselines re-recorded to lock the new floor.
    - Noted en route: the native path is NONDETERMINISTIC run-to-run on the
      same machine (torus outputs differ bit-wise across identical
      invocations; parallel CG reduction suspected) — violates the
      remeshing-pipeline determinism requirement independently of this work.
- **quad-quality-push finding — IMPLEMENTED (2026-08-02, `feat/dipole-quadcover`):**
  the shipped val-3/5 dipole canceller (`fixValence`) was unreachable from the
  quad-cover path (sole caller was the integer extractor). It is now exposed as
  `quadMeshValenceCleanup` (position_field.hpp) — the same doublet-dissolution +
  edge-rotation fixpoint, extended with `pinned` vertices (treated exactly like
  boundary) and `reservedEdges` — and runs in `QuadCoverQuadrangulator` after cap
  elimination and the count-calibration loop. Non-quad caps are frozen (vertices
  pinned, edges reserved) and crease vertices (face-pair dihedral over the run's
  feature threshold) are pinned so quads never rotate off a feature line. The
  integer path is byte-identical (empty constraints); kill switch
  `CYBER_QC_NO_DIPOLE`. Measured (A/B via the switch, cyber only): singularities
  armadillo 581 -> 550, nefertiti 650 -> 587, torus 68 -> 60, spot 72 -> 70,
  sphere 37 -> 35; feature recall, hausdorff and angle unchanged (< 0.1 deg / <
  0.002 recall drift); bunny-ear irregulars 46 -> 41. `flow_loop_mean_len` did
  NOT move (armadillo 20.2 -> 20.1) — dipole density was not the binding
  constraint on loop length; the remaining flow-loop gap needs a global lever
  (Bi-MDF-style quantization), not local surgery.
- **Gap #4, dense-organic singularity count (2026-08-02, `feat/field-singularities`):
  cone dipoles are cancellable at the FIELD level; the multires hierarchy is
  now shippable but still gated.** Two levers, measured separately and stacked.
  This is explicitly NOT a retry of the "field levers exhausted" finding (c7):
  that ruled out *tuning the smoother's energy* on the single-level field —
  lever B is a topological edit of the field's singularity set (smoothing
  provably cannot annihilate a dipole), and lever A is the hierarchy c7 left
  open behind its broken prolongation.
  - **B — field-level dipole annihilation (`annihilateFieldDipoles`, DEFAULT
    ON, kill switch `CYBER_QC_NO_CONE_MERGE`, radius
    `CYBER_QC_CONE_MERGE_RADIUS` default 5 hops).** `buildSeamlessSetup`
    promoted EVERY cross-field defect to a cone — no merge, prune or
    relocation anywhere in the solve — and on organics most defects are
    topologically-null noise (nefertiti: 997 singular vertices at totalIndex
    122, armadillo 489 at 27). The pass BFSes a short path between a close-by
    (+1,-1) pair over interior non-feature edges away from pinned faces, edits
    the signed period jumps along it (standard 4-RoSy merge: a unit change
    shifts the endpoint indices by ∓1, interiors cancel), then re-relaxes the
    face angles with jumps frozen so the field absorbs the 90° residual.
    Everything downstream is recomputed from the modified field, so
    `periodJump` / `combedDirection` / rho semantics and the feature binding
    (0e0b554) are untouched.
  - **A — multires hand-off relax (`CYBER_QC_CROSSFIELD_MULTIRES`, still
    OPT-IN).** `computeCrossField`'s three phases are now shared helpers
    (`initFrames` / `applyPins` / `transportSmooth`), and
    `computeCrossFieldFromOrientation` pins identically then relaxes its
    vertex-to-face hand-off with the same converged transport solve seeded
    from the hierarchy. Stock output verified byte-identical. Per the plan's
    mandatory scope cut, `smoothOrientation`'s convergence is NOT touched (it
    feeds `computePositionField`, which the InstantMeshes and integer
    extractors also consume).
  - **Measured** (cyber-pure = `--pure-quads`, target 8000 / spot 3000):

    | config | nefertiti sing/angle/recall/haus/loops | armadillo | spot |
    |---|---|---|---|
    | baseline | 401 / 9.04 / 0.887 / 0.0038 / 2601 | 377 / 9.11 / 0.761 / 0.0071 / 483 | 93 / 6.43 / 0.587 / 0.0059 / 433 |
    | B (default) | **347** / 9.50 / 0.861 / 0.0040 / 1631 | **299** / 9.02 / 0.806 / 0.0060 / 497 | **79** / 6.72 / 0.541 / 0.0048 / 561 |
    | A only | 346 / 8.72 / 0.870 / 0.0040 / 1187 | 297 / 8.76 / 0.820 / 0.0059 / 457 | 87 / 7.49 / 0.645 / 0.0046 / 811 |
    | A+B | **316** / 8.51 / 0.902 / 0.0039 / 1892 | **231** / 8.68 / 0.743 / 0.0062 / 430 | **65** / 7.81 / 0.610 / 0.0067 / 1224 |

    fandisk@3000 (B, default): sing 77 → 68, recall 0.413 → 0.431, hausdorff
    0.0089 → 0.0080. Generated corpus unchanged, box_sharp keeps 8 cones /
    0.00° / recall 1.00; ctest 13/13, `bench.py check` green.
  - **Exit gate NOT met.** The gate was nefertiti ≤ 250 AND armadillo ≤ 250.
    armadillo reaches 231 with A+B (and 299 on the default); nefertiti bottoms
    out at 316. Its limiter is measurable and specific: 1805 feature edges on
    the coarse remesh pin a dense wrinkle web, so 349 of its 521 +cones never
    find a partner over non-pinned paths at all, and radius 12/16 outputs are
    byte-identical to radius 8 — the pass is saturated, not under-tuned.
    Getting nefertiti under 250 needs the pseudo-feature web itself addressed
    (or a global quantization lever), not more dipole surgery.
  - **NEW DO-NOT-RETRY:**
    - *Pinned-adjacent cancellation paths* (`CYBER_QC_CONE_MERGE_LOOSE`,
      removed): letting paths ride pinned-face borders took nefertiti's
      attempts 199 → 866 but cancellations only 171 → 172 — 632 reverts. The
      injected 90° residual cannot diffuse past a pinned face, exactly the
      shear-band failure predicted; the plan's "skip paths adjacent to pins"
      is confirmed necessary, not optional.
    - *A smoothness-energy homotopy guard* (removed): "region residual energy
      must not increase" rejected 72 of armadillo's 169 pairs whose acceptance
      was already metric-clean, and it caught NEITHER torus failure mode. Use
      the region-disk and curvature-structure guards instead.
    - *A global genus gate* instead of a per-region disk check: organic work
      meshes carry accidental bridge handles (armadillo: 56 non-manifold pinch
      edges; the excised complex is genus-positive) so a genus gate disables
      the pass entirely on exactly the meshes it helps.
    - *Flipping `CYBER_QC_CROSSFIELD_MULTIRES` to default*: the corpus A/B is
      NOT clean. box_sharp and cylinder are bit-identical and sphere improves
      sharply (sing 35 → 24, angle 13.1 → 7.8), fandisk improves (sing 50 → 47,
      recall 0.772 → 0.730), but the **torus regresses past bench tolerance**
      (hausdorff 0.0281 → 0.0495, +76% vs a 30% budget; angle 20.4 → 25.6).
      Keep it env-gated until the handle case is understood.
  - **Two guards were bench-caught and are load-bearing** — both found by the
    torus, both invisible to the vertex-index verification:
    1. *Region disk topology.* Per-vertex index checks span the dual cycle
       space only when the relaxed faces form a disk. On an annulus region the
       wrap generator's rotational holonomy is unchecked, so a jump edit can
       twist the field 90° around a tube with every index intact: torus
       sing 64 → 140 / angle 20.4 → 30.7 while its FIELD got *cleaner*
       (32 → 6 cones). Regions that wrap retry at 1/2 then 1/3 radius.
    2. *Curvature structure.* Geometry-demanded cone pairs are not noise;
       cancelling them still cost the torus sing 64 → 75 / angle 20.4 → 26.1.
       The discriminator is the 2-ring defect CONSISTENCY ratio |Σd|/Σ|d|:
       structured curvature has every vertex curving the same way (torus pairs
       r≈0.99), wrinkle noise alternates (armadillo median r 0.36). Reject at
       |defect| > 10° and r > 0.6 — 13/13 torus pairs, 18% of armadillo's.
- **The wall, quantified** (generated corpus, target 600, defaults): quad-cover
  wins singularity structure outright (cylinder 21 vs QuadriFlow 8 at similar
  angle quality; sphere 37 vs 8) but **feature recall collapses on sharp
  geometry — box_sharp 0.04 vs QuadriFlow 1.00** (Phase 3's known
  feature-following limitation, now a tracked number). Priorities that follow:
  feature-constrained seamless UVs / extraction snapping first; flow-loop
  length second (quadwild's Bi-MDF quantization is the reference point).

## Update — 2026-07-22 (default is now quad-cover; the gap is mostly closed)

The numbers in the rest of this doc describe the older `instant-meshes` extractor
and are **two generations stale**. The shipped **default is now `quad-cover`**
(`RemeshParams.quad_method`), and a fresh benchmark (`examples/11_benchmark.py`
metrics, ~3000 quads, spot/fandisk/rocker/bunny) reframes Phase 4:

- **Irregular-vertex half of the exit gate is already MET** — ~4% irregular
  (spot 4.1 / fandisk 3.3 / rocker 4.6 / bunny 4.9), gate was <15%, and it is
  ~100% *true* field cones (no extractor headroom). The old "36% spurious
  singularities" problem belongs to the retired extractor.
- **Median-angle half** — the dependency-free *native* quad-cover solver trails
  QuadriFlow by ~1.5–4.9° (mean ~3.4°). The **vendored in-process Geogram field**
  (`-DCYBER_WITH_QUADCOVER=ON`, MIT) beats QuadriFlow on median AND irregular on
  spot (83.6 vs 82.5), rocker (83.5 vs 82.2) and bunny (83.0 vs 82.2), losing
  fandisk (80.9 vs 85.0). Verified end-to-end, both backends reproduced.
  - ⚠️ **Correction (2026-07-22): the "3/4 organic models" framing was selection
    bias.** That trio counts **rocker-arm**, the *mechanical* model, as organic,
    and omits **cheburashka**, an actual organic character — which **loses on
    both axes** (80 vs 82 median, 4% vs 2% irregular, and the worst edge-CV gap
    in the corpus at 0.22 vs 0.15). On the real organic set (spot, cheburashka,
    bunny) it is **2/3**, not 3/4. Corpus-wide the default is **3/5 on median and
    3/5 on irregular**. The win is real but narrower than recorded; quote 3/5,
    and name cheburashka alongside fandisk as the losses.

So Phase 4 is **not** the "only a global integer-parametrization rewrite can close
it" hard core this doc claims. **DONE:** the `cpu-headless` preset now builds with
`-DCYBER_WITH_QUADCOVER=ON`, so the stock default (no env vars) uses the vendored
Geogram field and reproduces spot 84 / rocker 85 / bunny 83 — beating QuadriFlow
on median *and* irregular on 3/5 of the corpus (losing fandisk and cheburashka).
Full test suite green against that build (only the pre-existing integer-extractor
WIP fails).

**Current standing of the default vs QuadriFlow** (2026-07-22, `--target-quads
3000`, re-measured after the harness was pointed at the shipped extractor — it had
been scoring the retired one in Phases 2–3):

| model | med° ours/QF | irr% ours/QF | CV ours/QF | defects ours/QF | feature ours/QF |
|---|---|---|---|---|---|
| spot | **84**/82 | **2**/3 | **0.12**/0.14 | **0**/0 | 0.46/**0.44** |
| fandisk | 83/**85** | 3/**1** | **0.16**/0.19 | **0**/0 | 0.82/**0.41** |
| rocker-arm | **85**/82 | **1**/3 | 0.15/0.15 | **0**/0 | 0.61/**0.40** |
| cheburashka | 80/**82** | 4/**2** | 0.22/**0.15** | **0**/0 | 1.15/**0.57** |
| stanford-bunny | **83**/82 | **3**/4 | 0.21/**0.17** | **8**/38 | 1.32/**0.58** |

Read across: we lead on **topological validity (5/5)**, lead on median angle and
irregular % (3/5 each), and trail on **feature-following**.

Read the feature column carefully — the raw 0/5 overstates it. `feature_error`
counts open boundaries as features (`examples/common.py:359`), and per-model crease
data is: spot 73 creases, fandisk 710, rocker-arm 394, cheburashka 316, bunny 774
of which **223 (29%) are the scan hole's boundary**. So the honest reading is
**3 real losses** — fandisk 2.0x, cheburashka 2.0x, rocker-arm 1.5x — with **spot
a tie** (0.46 vs 0.44 on only 73 creases) and **bunny confounded**. Still the
largest open quality gap, and notably **not CAD-only**: cheburashka is an organic
character with 316 genuine creases and a full 2x gap.

**Relax lever measured + shipped.** Bumping the quad-cover base relax 10→40 (its
Geogram base is uniform enough, like the integer grid) is a free, corpus-wide
median win — measured +0.3..+1.0° on organic with edge-CV flat-to-lower and
irregular % unchanged; now the default (spot 84.2 / rocker 84.5 / bunny 83.3,
cv 0.12–0.21). It is a *general* position lever, not model-specific.

**Fandisk/CAD median — mostly closed by backend routing (shipped).** A workflow
reframed it: the gap was **84% global extractor squareness** (vendored Geogram's
quads sit at ~81° even *far* from creases), not crease-following. Our native
feature-aware seamless solver marks sharp edges as hard seams and pins the
feature-bounded patches, so it makes squarer quads on CAD parts. `computeSeamlessUv`
now routes crease-heavy meshes (interior-crease-fraction ≥ 2%, via the non-mutating
`creaseEdgeFraction`) to the native solver first, keeping smooth organics on the
vendored Geogram path. Verified count-matched (~2970 quads, not a resolution
artifact): **fandisk 80.7 → 83.4 (+2.7°, ~63% of the gap), 0 defects**; every
organic **byte-identical** (`CYBER_QC_NO_ROUTE` kill-switch A/B). The residual to
QuadriFlow (83.4 vs 85.0) is the ~16% crease-localized part — a genuine field-level
per-edge integer-constraint project (QuadriFlow's feature flow), **deferred** as
not worth the multi-week cost for 1.6°. Everything below is retained for history.

## Where we are — HISTORICAL (2026-07, the retired `instant-meshes` extractor)

> ⚠️ **This section is superseded.** It describes `quad_method="instant-meshes"`,
> which has not been the default since quad-cover took over. For the shipped
> default's standing vs QuadriFlow, see the per-model table in the 2026-07-22
> update at the top of this doc. Kept because the phases below still reference
> its diagnosis, and because it records how the earlier picture was framed.

The position-field extractor (`quad_method="instant-meshes"`, an Instant-Meshes
clean-room port) is shipped and opt-in. Measured against QuadriFlow (uniform
sizing, matched quad count, on spot / fandisk / stanford-bunny):

| Metric | Us (extractor) | QuadriFlow |
|---|---|---|
| Median smallest-quad-angle | 78 / 79 / 76° | 83 / 85 / 80° |
| Edge-length CV (lower better) | 0.17 / 0.19 / 0.21 | 0.12 / 0.14 / 0.17 |
| Sliver rate | < 2% | ~0–1% |

We **match on uniformity**, trail **~4–6° on median angle**. Root cause is
**diagnosed and quantified** (see `memory` / commit history): the bulk
valence-4 quads are already QuadriFlow-quality (median 80.6°), but only ~21% of
quads are bulk — ~36% of vertices are irregular (val-3/5), and most are
*extraction artifacts*, not true field singularities. Cheap levers (extraction
density, field iterations, geometric relaxation, valence recovery) are exhausted
and characterised; closing the last gap is a genuine topology build.

**How stale, concretely** — the same three models on today's default: median
**84 / 83 / 83°** (was 78/79/76) against QuadriFlow's 82/85/82, edge CV **0.12 /
0.16 / 0.21**, and irregular **2 / 3 / 3%** — not the ~36% above. The "36%
spurious singularities" problem, and every phase below that is gated on it,
belongs to this retired extractor.

## North Star

Every phase exits on a number from a single automated benchmark (Phase 1). We
do not claim "better" without a harness number that shows it.

## Status (2026-07)

- **Phase 1 — DONE.** `examples/11_benchmark.py` scores the corpus vs QuadriFlow
  on surface deviation, normal error, median angle, edge CV, and irregular-vertex
  %. Uniform, matched target (~3000 quads), best-of-ours vs QuadriFlow: ours ties
  on surface dev / normal error (2/5 models each), trails on median angle and
  singularity count (0/5) — the known extraction-singularity gap. It immediately
  earned its keep by falsifying the naive Phase-2 hypothesis (below).
- **Phase 2 — enabling fix DONE; QF-beating Phase-4-gated.** The extractor now
  uses a **per-vertex spacing** derived from local mesh density, so it tracks
  adaptive sizing instead of over-merging it — the adaptive quad count no longer
  collapses (smooth model 72 → 625 quads, ~8×; uniform behaviour unchanged).
  ~~Adaptivity is now validated: it beats our own uniform sizing on quality-per-
  polygon on 4/5 models (fandisk 0.46% vs 0.82% surface dev at matched count).~~
  **RETRACTED 2026-07-22 — 2/5, see Phase 2 below.** Ironically this bullet
  already diagnosed the failure mode it fell to: it notes the earlier "3/5 win vs
  QuadriFlow" was an artifact of collapsed quad counts, but the 4/5 figure was
  measured the same way — the arms never converged on a count, and fandisk's
  "win" was scored against a degenerate 22.61% baseline.
  Adaptivity also does **not** beat QuadriFlow's *absolute* fidelity per polygon.
  The old explanation — capped by ~36% spurious singularities, **coupled to Phase
  4** — no longer holds either: the default now runs at 1–4% irregular and still
  trails, so per-polygon fidelity is not singularity-gated.
- **Bonus finding:** the QuadriFlow-in-every-example panels show a clear
  feature-preservation win — on a cube QuadriFlow rounds the edges and tears
  holes (20% slivers) while our feature-aware remesh keeps them crisp. Feeds
  Phase 3.

---

## Phase 1 — Define & measure "better" (foundation) — ✅ DONE

Today's harness measures QuadriFlow's home turf (uniform, matched-count, median
angle). Build metrics that capture *real* retopology quality:

- **Quality-per-polygon**: Hausdorff + normal-error vs the source at matched
  polygon count (rewards adaptivity — QuadriFlow is uniform-only).
- **Feature-following error**: mean distance of quad edges from sharp creases.
- **Singularity count / irregular-vertex %**: track the diagnosed weakness.
- **Robustness**: success + manifold rate over the *full* common-3d-test-models
  corpus, not 3 hand-picked models.

**Deliverable:** `examples/11_benchmark.py` — an automated scored benchmark,
runnable in CI, producing a per-model table and an aggregate score vs QuadriFlow.
**Exit:** the benchmark runs green and reproduces the numbers above.

## Phase 2 — Win on adaptivity (quality-per-polygon) — 🟢 Fix landed · ⛓ QF-gated by Phase 4

QuadriFlow is uniform. We have curvature-adaptive sizing (`adaptivity`). Concentrate
quads where curvature is high → better fidelity per polygon.
- ✅ **Variable-spacing extractor** — the position-field extractor now takes a
  per-vertex spacing (local density), so it tracks adaptive sizing instead of
  over-merging it. Count no longer collapses; uniform path unchanged.
- ❌ **The 4/5 "adaptivity validated" claim does NOT reproduce — it was a
  measurement artifact (2026-07-22).** The harness matched counts by requesting
  `achieved * 1.3` once, which left the adaptive and uniform arms 20–30% apart
  down at ~200–400 quads, where both extractors degrade. fandisk's "win" rested
  on a *uniform* baseline reporting **22.61%** surface deviation — a degenerate
  extraction, not a measurement; at matched count that same run is **0.30%**.
  With both arms driven through `search_matched_count`, adaptivity beats uniform
  on **2/5** — and one of those (cheburashka, 0.28 vs 0.29%) is inside noise,
  while rocker-arm is ~5× *worse* (1.30 vs 0.27%).
- ❌ **Adaptive sizing cannot even reach benchmark density on 2/5 models** —
  it saturates at 2034 quads (cheburashka) and 916 (stanford-bunny) against a
  3000 request, so those models have no matched-count comparison at all.
- ⚠️ **Caveat on the corrected numbers:** the count-match request ceiling bounds
  how hard the adaptive arm can be pushed, so on saturating models the pair is
  matched to each other but below target (spot lands ~657q). Rows are
  self-consistent; cross-model comparison is not meaningful.
- ◻ **Optional:** budget-preserving sizing so `adaptivity` honors the target count
  (a renorm was tried and reverted — it destabilized dev on 2 models; needs a
  gentler, mesh-quality-aware formulation).
**Exit:** ours ≥ QuadriFlow on quality-per-polygon on ≥ 4/5 corpus models *(not
met — QuadriFlow leads on all 5)*; adaptivity beats our own uniform sizing on
≥ 4/5 *(**NOT met — 2/5**; the earlier "met" was the artifact above)*.

**Recommendation: descope.** The shipped `quad-cover` default is uniform-only by
design (capi hardcodes adaptivity 0; the isotropic stage that consumes
`params.adaptivity` is bypassed for it), so this phase measures a lever the
default cannot use, and the lever does not win where it can be used.

## Phase 3 — Win on features & robustness — 🟦 CLOSED 2026-07-24 (validity won outright; feature-following a known limitation)

Metrics built (`feature_error`, `mesh_validity`) and wired into the benchmark.
Honest finding — **not a clean win**:
- ✅ **Robustness on hard-edged / box geometry** — the genuine strength. On a
  subdivided cube QuadriFlow catastrophically tears (598 boundary edges, 2.87%
  feature error) while ours is clean (0 defects, 0.3%). QuadriFlow degenerates on
  sharp box corners; we don't.
- ✅ **Validity on the smooth corpus — MET, and we beat QuadriFlow (2026-07-22).**
  Follow-up (a) below was a property of the **retired** position-field extractor,
  which the harness was still scoring. Re-measured on the shipped `quad-cover`
  default, topological defects are **0 on 4/5 models** and 8 vs QuadriFlow's 38
  on stanford-bunny — **5/5 ≥ QuadriFlow**. The same run shows the retired
  extractor's 110 (cheburashka) and 30 (fandisk), which is what the old
  "hole-fill doesn't close them — a real extractor bug" note referred to. It does
  not describe what ships.
  - ✅ **Now 0 on 5/5 (2026-07-23) — bunny's residual 8 was a bug, not its scan
    geometry.** `IsolineExtractor::fixHoleWithQuads` closed a boundary loop
    **twice**: `fixHoles` calls it once score-checked and once not, relying on the
    first pass to consume `hole` (an in/out parameter), and the terminal 4-gon
    branch returned with `hole` still full. Two coincident quads are
    edge-count-manifold on their own, so nothing downstream rejected them; the
    pure-quad subdivision then gave each its own face point and turned the shared
    rim into a genuine non-manifold edge. Measured on the shipped default over
    5 models × 5 densities (2600/3000/3400/3800/4200): **stanford-bunny 40 → 0**
    non-manifold edges (8/8/0/16/8 before), the other four 0 → 0 with 20 of 25
    cells bit-identical (rocker-arm @2600 keeps an identical face list, vertices
    move ≤1.1e-5). This is the mechanism behind bunny's long-recorded "defect
    lottery" — it was never density noise, it was whether the trace happened to
    leave a 4-edge loop. **Quote defect counts with the density they were measured
    at**; before the fix a single-density reading was not a floor.
- ❌ **Feature-following — NOT met, 0/5.** ⚠️ **Restated 2026-07-23 at matched
  achieved quad count** — the figures below the strikethrough were measured at a
  matched *request*, where QuadriFlow landed 11–16% denser, and `feature_error`
  falls roughly as `count^-0.5`. Count-matched: fandisk **0.75 vs 0.41** (1.83x),
  cheburashka **1.00 vs 0.56** (1.79x), rocker-arm **0.48 vs 0.42** (1.14x), spot
  **0.41 vs 0.35** (1.17x), stanford-bunny **1.36 vs 0.48** (confounded — 29% of
  its "features" are the scan hole's open boundary, which the metric counts as a
  feature). ~~fandisk 0.82/0.41 (2.0x), cheburashka 1.15/0.57 (2.0x), rocker-arm
  0.61/0.40 (1.5x), spot a tie 0.46/0.44.~~ **rocker-arm was never a 1.5x loss and
  spot was never a tie**; the two real gaps are fandisk and cheburashka. The datum
  that survives the restatement: **cheburashka is an organic character with 316
  genuine dihedral creases and a ~1.8x gap**, so this is broader than the "fandisk
  CAD residual" framing at the top of this doc.
- 🔴 **Cheap levers are exhausted — three measured and reverted.** Root cause is
  known (M1: the cross-field IS crease-aligned, but the integer grid **phase** is
  un-pinned, so loops sit ~half a cell off the creases — a grid-phase problem, not
  an alignment one). **M2a** post-extraction vertex snap: fandisk 1.20→1.05, still
  2x QF — a vertex snap cannot create a loop that isn't there. **M2b** crease
  gauge-pin in `solveSeamlessReduced`: no feature gain and **introduced 8 defects**,
  breaking the validity win. **M2c** (2026-07-22) lowering `CYBER_QC_ROUTE_CREASE`
  so lightly-creased meshes reach the feature-aware native solver: cheburashka
  1.15→1.06% (~8%, still 1.9x QF) but **rocker-arm regresses −7° median**, irr
  1→5% — net-negative, threshold stays 2%. See `cad-feature-robustness` memory.
**Follow-ups:** ~~(a) fix the extractor's scattered validity defects~~ *(done —
retired-extractor issue; the default is clean)*; ~~**(b) per-feature-edge integer
constraints in the parameterization** (QuadriFlow's `ComputeIndexMap` sharp-edge
path)~~ — **BUILT AND MEASURED INERT (2026-07-23), do not re-attempt as scoped.**
Two independent reasons, both measured:
  1. **Reach is 1/5 by routing.** `computeSeamlessUv` sends a mesh to the native
     solver only above a 2% interior-crease fraction. Measured: fandisk 0.0364,
     cheburashka 0.0142, rocker-arm 0.0074, spot 0.0051, bunny 0.0036. **Only
     fandisk clears it** — the other four never execute a line of the constraint
     code, so a native-solver lever cannot move them at all. Lowering the
     threshold is M2c, already dead.
  2. **The premise is false.** M1's "the cross field IS crease-aligned, only the
     grid phase is off" does not hold: fandisk's median 4-RoSy deviation from its
     crease directions measures ~21°, where a *random* field gives ~22.5°. There
     is nothing running along the creases to pin a grid to. The constraint
     mechanism was driven to exactness (crease level sets landing bit-exactly on
     the integer lattice) and still moved feature error by less than a sixth of
     the run-to-run noise, while costing median angle on the one model it reaches.
  **Open follow-ups: see the untried-lever list below.**
### Phase 3 close-out (2026-07-24)

**Two of three exit criteria are MET and one is closed as a known limitation.**

| criterion | verdict |
|---|---|
| Robustness on hard-surface geometry | ✅ **met** |
| Topological validity on the corpus | ✅ **met, and we beat QuadriFlow outright — 6/6** |
| Feature alignment | ⛔ **not met, closed as a known limitation — 1/6** |

**What we win, and it is the strongest claim in the project.** Topological
validity is **0 defects on all six corpus models**, at every density measured
(2600–4200), against QuadriFlow's **680 on the cube** and **32 on stanford-bunny**.
Nothing else in the roadmap is a clean corpus-wide win over the reference. It also
survived a real bug this cycle: bunny's long-recorded "defect lottery" turned out
to be a hole-filler closing a 4-edge loop twice (**40 → 0**), shipped in v0.2.4.

**What we do not win.** Feature-following is **1/6** — and read that honestly: the
single win is the **cube**, which was *added* to the corpus this cycle because we
already won it. We did not start beating QuadriFlow on any model we previously
lost. fandisk improved materially (**0.75 → 0.62**, gap 1.83x → 1.48x) and **still
loses**. cheburashka is untouched at 1.00 vs 0.56.

**Why it is being closed rather than continued — nine measured levers:**

| lever | layer | verdict |
|---|---|---|
| M2a vertex snap | post-extraction | dead |
| M2b gauge pin | solver gauge | dead, +8 defects |
| M2c routing threshold | routing | dead, rocker −7° |
| M2d per-edge integer constraints | parameterization | **refuted** (1/5 reach; premise false) |
| feature-degree widening | shared threshold | dead, median −5.9° |
| field alignment, ungated | field | regressed flat CAD |
| consistency gate | field | no-op |
| edge-ring planarity gate | field | under-applies |
| **(c1) crease preservation** | **pre-remesh** | ✅ **shipped** |
| **(c2) planarity-gated alignment** | **field** | ✅ **shipped** |
| (c1b) vendored crease visibility | vendored pre-remesh | **refuted** (count artifact) |

Two wins out of eleven attempts, both confined to the single model that clears the
2% native-routing gate. **The structural cap is the reason to stop:** 4 of 6 models
run on the vendored Geogram backend, and the one lever that reaches them (c1b) is a
count artifact that cannot be decoupled without patching vendored source. Any
further feature-following work must first answer *how it reaches the vendored path
at all* — not propose another constraint.

**If it is ever reopened**, the remaining honest entry points are (c4) cone
placement at crease corners and (c6) crease-preserving surface projection — both
Tier 2, neither cheap, and both still capped at 1/6 until the routing question is
answered. **(c7) a globally-optimal direction field is no longer an entry point:
it was built and measured (below), recovers under half the native→Geogram gap, and
established that the residual is per-face *discretization*, not the field — so
field-level levers are exhausted.**

**Superseded exit note:** *robustness win on hard-surface geometry (met); validity
on the smooth corpus (met — now 6/6, was recorded 5/5 before the cube joined the
corpus); feature alignment (not met — was recorded 0/5).*

### Untried levers for retopology quality (2026-07-24)

Everything M1–M2d attacked lives in the **grid / constraint** layer. The 2026-07-23
refutation showed two problems *upstream* of that layer which nobody has touched:

- the cross field is **not** crease-aligned (fandisk ~21° median deviation from its
  crease directions; a *random* field gives ~22.5°), and
- the ARAP polish has **no restoring force toward the field at all** — median
  |rotation| climbs monotonically to ~43°, the maximum a 4-RoSy target can be off
  by, on both fandisk (CAD) and spot (organic).

So the levers below are ranked by that reframing, not by the old grid-phase story.
Anything already measured dead is listed at the end — check it before proposing.

**Tier 1 — opened by the refutation**

- ✅ **(c1) Preserve crease polylines through the isotropic pre-remesh — DONE
  (2026-07-24), the first lever that works.** Diagnosis confirmed and quantified
  directly: on fandisk at ~3000 quads the crease network went from **706 edges in
  ONE connected component** (2 dangling ends, 22 junctions) to **449 edges in 55
  components with 136 dangling ends** — 36% of the creases destroyed outright and
  the rest shattered. Root cause: `isotropicRemesh` *does* protect features
  (never collapses feature vertices, flips feature edges or smooths them —
  `isotropic.cpp:210/254/318`) but only sees what `tagFeatureEdges` marked, and
  that call takes an **included** angle, so the shipped 40 means "face-normal
  angle ≥ 140" — fandisk has **zero** such edges, so the remesher was told the
  part has no features. Fix: tag **wide before** the remesh (protect) and keep the
  **narrow tag after** it (seams unchanged), via `CYBER_QC_PRESERVE_CREASE_DEG`,
  default 135. Crease network now survives **exactly** (706 edges, 1 component, 2
  dangling — identical to the source). Measured: feature error −11.3% / −6.9% /
  −5.1% at 2600 / 3000 / 3400 quads, **median flat** (−0.02 / −0.45 / +0.72),
  normal error −1.3° / −0.6°, 0 defects, and **12 of 15 corpus cells
  byte-identical** (only fandisk routes native). One regression: fandisk@3000 edge
  CV 0.159 → 0.179. Costs ~15% more triangles in the working mesh, since creases
  can no longer be collapsed across.
- ✅ **(c2) Actually crease-align the cross field — DONE (2026-07-24), default on
  behind a planarity gate.** `computeCrossField` gained a
  `creaseAlignDegrees` parameter (`CYBER_QC_FIELD_CREASE_DEG` overrides,
  `CYBER_QC_FIELD_CREASE_DEG` overrides): any interior edge whose face-normal
  angle exceeds it pins its faces to the crease direction, while
  `Mesh::isFeatureEdge` — and therefore the seam set, period jumps and cut graph —
  is untouched. That split matters: widening the *shared* threshold instead costs
  median 83.4 → 77.5 (see the dead-lever list).
  - **Order is load-bearing, and it is now evidence rather than a guess.** Applied
    BEFORE (c1) it is net-negative — feature −4% but median 83.4 → 79.8 and
    irregular 3.3 → 5.0 — because pinning the field to 55 crease fragments with
    136 dangling ends injects conflicting directions. Applied AFTER (c1), with the
    crease network intact, it is a win.
  - ⚠️ **A 3-density sample said "blocked on a −2.3° median at 3000". That was
    under-sampling, not a defect.** Median-vs-density on fandisk jitters with
    sd 0.90° on *identical code* (81.10 / 81.95 / 82.97 / 83.40 / 82.61 / 81.16 /
    83.32 across 2600–3800). Re-measured over **7 densities**: feature
    0.7271 → 0.6846 (**−5.8%, better at 6/7**), edge CV 0.1656 → 0.1505 (**−9.1%,
    6/7**), irregular 3.63 → 3.31 (**−8.8%, 6/7**), median 82.36 → 81.94
    (**−0.42 ± 0.35, paired t ≈ 1.2, p ≈ 0.28 — not distinguishable from zero**).
    Three metrics improve; the fourth does not measurably move. **Lesson: sample
    density before calling a per-density delta a regression** — this is the same
    under-sampling class that produced the retracted Phase 2 result.
  - Corpus regression: **12 of 15 cells byte-identical** (only fandisk routes
    native), 0 defects everywhere.
  - 🔴 **REGRESSION found by visual inspection of the examples, after it had
    already been merged — it is now default OFF.** Two separate mistakes, both
    invisible to the corpus harness:
    1. **Flat CAD gets worse.** On the `04_sharp_edges` subdivided cube, c2 takes
       edge CV **0.201 → 0.397** and slivers **0.1% → 1.2%** (median 84.0 → 82.3).
       Pinning every face of a flat panel to its four differently-oriented
       boundary creases over-constrains a field that should stay smooth. c1 alone
       is strictly better on that model than both the pre-session baseline AND
       c1+c2. The corpus (spot/fandisk/rocker/cheburashka/bunny) contains **no
       flat-CAD model**, so it could not see this.
    2. **Blast radius was wider than measured.** `computeCrossField`
       has two callers — `seamless_solver.cpp:181` (native quad-cover, the one
       measured) and `field_quadrangulator.cpp:464`, the **field-aligned**
       quadrangulator that is the universal fallback and the flat-CAD route.
       Defaulting the parameter changed both. The harness only ever exercises
       `quad_method="quad-cover"`, so it was structurally blind to the second.
    **Three gate attempts, all measured (2026-07-24). The third works:**
    - ❌ **Consistency gate — measured NO-OP, do not retry.** Hypothesis: a face
      touching several creases (every triangle round a cube corner touches two
      perpendicular ones) pins to whichever comes first in vertex order, so
      neighbours resolve it differently. Gated on the mean-resultant length of the
      face's crease directions in 4-RoSy. It changed nothing on the cube
      (CV still 0.397 at 900 quads) because a crease-adjacent triangle almost
      always touches exactly ONE crease, so the gate never fires.
    - 🔬 **The mechanism is now measured, and it is NOT over-constraining.**
      `CYBER_QC_FIELD_STATS` reports the frozen fraction: crease alignment freezes
      **52.0% of fandisk's faces and improves it**, but only **14.7% of the cube's
      and degrades it**. fandisk freezes 3.5x more and gets better. The real
      distinction is that **a flat panel's cross field is degenerate** — every
      orientation is equally smooth — so pinning a border band imposes arbitrary
      structure the interior cannot reconcile, whereas a curved surface has a
      curvature-driven preference that crease pins reinforce. A gate must therefore
      test the *surface*, not the constraint set: skip alignment where the
      neighbourhood is planar.
    - ❌ **Planarity gate v1, EDGE ring — under-applies, do not use.** Skipped the pin
      when every neighbour across the face's non-crease *edges* was coplanar. It
      protected the cube, but it also gated off a genuinely curved fixture: a
      crease-adjacent triangle's edge-neighbours can all sit in the same row of the
      fold, and a developable surface is exactly coplanar along that row, so the
      test never sees the direction the surface actually bends in.
    - ✅ **Planarity gate v2, VERTEX ring — SHIPPED.** Same idea over the vertex
      ring, which reaches past that row, with faces more than 45° from the face
      excluded so the fold itself is not counted as its own evidence. Discriminates
      exactly: on a curved fixture 24 of 24 crease edges are pinned, on a flat one
      24 of 24 are gated off and the solve is bit-identical. Measured on fandisk
      over 7 densities vs c1 alone: **feature −4.6% (better 7/7)**, **edge CV
      0.1656 → 0.1472 (the best of every arm tried, better even than ungated)**,
      irregular 3.63 → 3.38, median −0.30 ± 0.31 (not distinguishable from zero).
      Flat CAD is **bit-identical**, and the corpus is **15/18 cells byte-identical**.
      Residual: fandisk@2600 alone regresses (median −1.79, CV +0.017, irregular
      +0.70) — one point inside the sd-0.90 density jitter, against a 7/7 feature
      win.
    - ✅ **The corpus blind spot is CLOSED.** `cube` is now a synthesised member of
      the benchmark corpus (`common.SYNTHETIC_MODELS`, `11_benchmark.DEFAULT_MODELS`).
      It immediately earned its place twice over: it reproduces the regression that
      slipped through (cube@900 edge CV 0.201 → 0.397), **and** it surfaced a win the
      scored benchmark could not previously see — on flat CAD we BEAT QuadriFlow on
      feature-following (**0.98% vs 2.69%**) with **0 defects against its 680**.
      Corpus-wide the headline moves from feature 0/5 to **1/6** and defects to
      **6/6**.
- ◻ **(c3) Give the ARAP polish a restoring force toward the field.** A *clamp* was
  tried (every face saturates whatever cap it is given: 5/10/20/30/45 → 5/10/20/30/44)
  and a 4-RoSy fundamental-domain wrap was tried (worse — map-vs-target 5°→17°). A
  **penalty term pulling the Jacobian back toward the field** is a different
  mechanism and was never built. Without it, (c2) cannot reach the map: the map
  runs ~24° off the field even with the polish disabled.

- ❌ **(c1b) Vendored-path crease visibility — REFUTED (2026-07-24). Do not retry.**
  The vendored solver never calls `setSharpEdgeDegrees`, so it runs at AutoRemesher's
  default 90° where Geogram's is 45, and at 90 the corpus sees almost none of its
  creases (constrained edges at 45 vs 90: fandisk 706/299, cheburashka 283/163,
  rocker-arm 223/10, bunny 379/20, spot 45/0). It looked like the exact analogue of
  (c1) applied where **4 of 6 models actually route** — the only visible path to
  cheburashka's 1.8x gap. **It is a COUNT ARTIFACT, exactly as an earlier workflow
  round claimed.** Lowering it inflates the mesh instead of aligning it: at 45°,
  quad counts go stanford-bunny 2644 → **15105 (+471%)**, rocker-arm +48%,
  cheburashka +31%. Since `feature_error` falls as `count^-0.5`, the apparent
  feature gains (cheburashka 1.15 → 0.80) are bought entirely with polygons. Where
  counts stay comparable the quality collapses: median rocker-arm −11.3°,
  cheburashka −8.9°, stanford-bunny −27°. 60° is milder and still fails (bunny
  +157% count, median −19°). Root cause it cannot escape: the value feeds BOTH the
  vendored pre-remesh (`autoremesher.cpp:291`) AND the quad_cover parameterizer's
  hard-edge constraints (`:534`), and unlike the native path those cannot be
  decoupled without patching vendored source — so it behaves like the shared-threshold
  widening that was already measured net-negative natively.

**Tier 2 — structural**

- ◻ **(c4) Force cones at crease corners / junctions.** Singularity *placement* is
  unconstrained today; part of QuadriFlow's crease behaviour comes from where its
  cones land.
- ◻ **(c5) Replace the 2% routing threshold with a measured best-of-both.** Today
  `creaseEdgeFraction ≥ 0.02` is a proxy reaching only 1/5 of the corpus, and M2c
  proved tuning the number is net-negative. Running *both* backends and scoring
  them (median / irregular / CV / defects) gives best-of on every model. Costs a
  second solve; must be kill-switchable. Also unblocks any native-solver work from
  being capped at 1/5.
- ◻ **(c6) Crease-preserving surface projection.** M2a found the output carries
  almost no detectable feature edges because *projection smooths creases* (only 17
  tagged on fandisk). The relax path pins feature vertices and the projection then
  undoes it. Helps feature error *and* median.
- 🔬 **(c7) Knöppel–Crane globally-optimal direction field — BUILT AND MEASURED
  (2026-07-24). A real but insufficient field-level win; it does NOT close the gap
  to Geogram, and field-level levers are now exhausted.**
  - **Motivation (de-risked first).** A scoping measurement forced the organics
    through the native solver (`CYBER_QC_ROUTE_CREASE=0.0001`) and decomposed the
    native→vendored gap: it is **field-dominated** — native produces ~2× the
    spurious singularities of Geogram's field (spot irregular 1.99 vendored → 4.63
    native, count-matched to 1%), and the median gap is largely downstream of that.
    So a globally-optimal field was the right lever to try.
  - **What was built.** A dependency-free per-face connection-Laplacian 4-RoSy
    field via inverse power iteration on the existing spmv + CG (no eigensolver
    dependency, no SciPP), reusing the same frames / transport phases / feature
    pins as the iterative field so c1/c2 are honoured. Globally optimal, verified
    by a seed-independent Dirichlet energy.
  - **Result — real, count-matched, independently verified (converged tol 1e-5):**
    | model | metric | current native | **KC native** | vendored (Geogram) |
    |---|---|---|---|---|
    | spot | irregular % | 4.63 | **3.45** | 1.99 |
    | spot | median | 79.93 | **81.87** | 84.24 |
    | spot | field singular | 77 | **73** | — |
    | cheburashka | irregular % | 5.71 | **4.78** | 3.68 |

    KC cuts spurious singularities and recovers median on both organics — it closes
    **~45% of the native→Geogram irregular gap** on the count-matched spot row. But
    it does **not** reach Geogram (3.45 vs 1.99). ⚠️ **Do not quote the "2.96"
    figure that appeared in an interim writeup — it was an under-converged
    inverse-iteration artifact (tol 1e-3), caught by the workflow's own critics;
    the honest converged number is 3.45, reproduced by independent A/B.**
  - **Routing calculus UNCHANGED — the cap is not broken.** On fandisk (the only
    model that routes native by default) KC is flat-to-negative (irregular
    2.90→2.94, median −0.8°); on stanford-bunny it is a count-matched regression.
    So forcing organics native-KC still loses to vendored, and `computeSeamlessUv`
    routing is untouched.
  - **Two sub-variants BUILT AND REFUTED, do not retry:** (i) dual-cotan edge
    weights (regresses spot 3.81→4.18; on a near-uniform remesh the dual/primal
    ratio is nearly constant so it barely differs); (ii) the paper-faithful
    per-vertex cotan Laplacian (the vertex→face projection re-winds phases and
    injects *more* cones than the per-face form).
  - **The conclusion that matters: the residual native→Geogram gap is per-face
    DISCRETIZATION, not local-vs-global optimization.** A globally-optimal field —
    the strongest field-level lever — recovers under half the gap. **Every
    field-level lever (reweighting, re-domaining, global-vs-local) is now measured;
    none closes it.** The remaining path is downstream (extraction / integer
    rounding), NOT a smarter field. ⚠️ Note the workflow suggested "per-feature-edge
    integer constraints" as the next step — that is **M2d, already refuted** (see
    the dead-lever list); its overlap here is a coincidence of naming, not a live
    lead.
  - **Foundation preserved off-main.** The working KC eigensolver lives on branch
    `feat/knoppel-crane-field` (pushed to origin), NOT merged. It is off-by-default
    (`CYBER_QC_KC_FIELD`), byte-identical when off, compiles both with and without
    Geogram, and has a deterministic regression test (KC singular 2 < iterative 8,
    a wide margin). It was kept off main deliberately: it delivers **no default-path
    improvement** (organics still route vendored), so on main it would only add
    build weight and a float-based CI test for code nobody runs. Anyone resuming the
    discretization work should branch from there rather than re-derive the solver.

**Tier 3 — narrower, concrete**

- 🔬 **(c8) Finish the M3 open-surface cleanup — MIS-DIAGNOSED (2026-07-24). The
  prize is real; the named blocker is not the blocker.**
  - **Prize confirmed.** Open paraboloid at 1200 quads, `CYBER_QC_OPEN_CLEANUP`
    off vs on: faces **136 → 954**, median **52.8° → 78.7°**. The cleanup is what
    lets an open surface trace properly at all.
  - ❌ **The `simplifyGraph` turn-angle guard is NOT what blocks it.** Built it —
    dissolve a valence-2 node only when near-collinear (the two directions out of
    it at least 150° apart), so genuine rim corners survive. Measured: faces
    954 → 1066, median 78.7 → **75.6**, edge CV 1.696 → **1.890**. It makes both
    metrics slightly WORSE, and the flat-grid corner symptom the old TODO names
    ("interior 25→20, 7 triangles") did not reproduce. Reverted.
  - ✅ **SHIPPED (2026-07-24) — default on, opt out with `CYBER_QC_NO_OPEN_CLEANUP`.**
    Blocker was `simplifyGraph` over-dissolving
    valence-2 isoline samples on open surfaces.** Bisected the whole cleanup
    pipeline by env-gating each step on the open paraboloid at request 1200
    (all-quad, 0 defects throughout):
    | config | faces | median | edge CV |
    |---|---|---|---|
    | cleanup off (default) | 136 | 52.8 | 0.442 |
    | cleanup on (shipped opt-in) | 954 | 78.7 | **1.696** |
    | **fixHoles only, `simplifyGraph` removed** | 1920 | 68.7 | **0.312** |
    `fixHoles` is what makes the open trace work (it fills the under-traced gaps,
    136 → ~2000 faces). `simplifyGraph` is what wrecks uniformity: it dissolves
    every valence-2 node, and on a properly traced open surface most isoline
    samples are legitimately valence-2, so it merges cells into long uneven quads —
    roughly halving the face count (1920 → 954) and taking edge CV from **0.312 to
    1.696** on the SAME request. The collapse steps (`collapseShortEdges`,
    `collapseTriangles`, `removeSingleEndpoints`) are near-no-ops here; they only
    matter because they re-trigger `simplifyGraph`. This is a controlled toggle,
    not a count artifact: `simplifyGraph` is itself what changes the count.
  - **The fix: run the open cleanup WITHOUT `simplifyGraph` on open islands, and
    default it on.** Open islands now take the raw graph and skip straight to
    `fixHoles`; only closed islands run the collapse pipeline. Gated on the
    existing `m_preserveInputBoundary` (= `!closed`), so **the closed corpus is
    byte-identical** — verified 18/18 corpus cells and all 7 broken-robustness
    cases (2 open) still manifold. Regression guard:
    `python/cyberremesh/tests/test_open_surface_cleanup.py`, verified discriminating
    (fails on the 92-face under-trace and CV 0.93 with `CYBER_QC_NO_OPEN_CLEANUP`).
  - Effect where the paraboloid classifies open (low density): request 900 goes
    from **92 faces / median 27° / CV 0.93** to **~1744 quads / median 78° / CV
    0.27**. This was the largest untapped single-metric prize in the list; it is
    now the default.
- ◻ **(c9) Tube-aware coarsening** for the multiresolution cross field — named as
  "the real fix" after multires was found to help smooth models but bridge thin
  tubes (the bunny-ears case). Identified, never built.
- ◻ **(c10) cheburashka edge-CV** (0.22 vs QuadriFlow 0.15, the corpus's widest CV
  gap). Shape-match bought ~20% corpus-wide; nothing model-specific has been tried
  for the outlier.

**Measured dead — do not re-try** (each has a numbered entry above or in
`cad-feature-robustness`): M2a vertex snap · M2b gauge-pin · M2c routing-threshold ·
**M2d per-feature-edge integer constraints** · (4a) local valence optimization ·
multi-resolution coarse extraction (proven byte-identical no-op) · T-junction
cleanup / `FixFlipSat` · QuadriFlow flip-repair order · adaptive sizing for
quad-cover (irregular/CV explode) · equiareal MIQ term · min-cost-flow port ·
feature-degree sweep (re-measured 2026-07-24: widening the SHARED threshold moves feature 0.82 → 0.77 but costs median 83.4 → 77.5, CV 0.159 → 0.252 and irregular 3.3 → 7.1, because every extra tagged edge becomes another hard seam — this is what motivated splitting the preserve/seam thresholds in c1) · curvature-weighted seam routing · ARAP clamp · ARAP RoSy wrap.

⚠️ (c2) and (c3) are inferences from the ~21° / ~24° measurements, not themselves
measured hypotheses — A/B them like anything else.

## Phase 4 — Close the median-angle gap — ✅ largely closed by the quad-cover default

> ⚠️ **Premise superseded.** The framing below — "the hard core", 36% irregular,
> only a global rewrite can close it — describes the retired extractor. The
> shipped default runs at **1–4% irregular** and **beats QuadriFlow on median on
> 3/5** (spot 84/82, rocker 85/82, bunny 83/82; losing fandisk 83/85 and
> cheburashka 80/82). The remaining median gap is the crease-alignment problem
> tracked in Phase 3, not a singularity problem. The 4a/4b history is retained
> because it records what was measured and why the local levers failed.

Reduce spurious singularities (36% irregular → target < 10%) for angle parity.
- ❌ **4a. Local valence optimization** (edge rotation to cancel val-3/5 pairs) —
  TRIED, net-negative. Trades topology for geometry: ungated it cut irregular
  vertices 38→31% but wrecked edge-length CV (bunny 0.21→0.46) by shearing quads;
  gated to preserve geometry it becomes a no-op. Triangle-pair merge was also
  neutral. Local post-hoc surgery can't fix this without wrecking shape.
- ◻ **4b. Global integer parametrization** (QuadriFlow's method): spanning-tree
  integer integration + min-cost-flow holonomy resolution, producing clean
  topology *and* geometry from the start. The remaining real lever — a large,
  high-risk extractor rewrite, genuinely multi-session. **Planned in detail:**
  [`docs/integer-parametrization-plan.md`](integer-parametrization-plan.md)
  (Stage 1 coords → Stage 2 integer solve → Stage 3 extraction, behind a new
  `quad_method="integer"` until it beats the current path).
**Exit:** irregular-vertex % < 15% and median angle ≥ QuadriFlow. **Only 4b can
get there; the local shortcuts are proven dead ends.**

## Phase 5 — Field foundation (enables 2–4)

Stronger orientation-field optimization (fewer, better-placed singularities at
the source) + the position-field integer optimization. Feeds every phase;
overlaps 4b.
**Exit:** raw-extraction corner-skew floor < 8° (currently ~13°).

---

## Sequencing

`1 → 2 → 3 → 4 → 5`. Phases 1–3 are where we *actually beat* QuadriFlow and are
lower-risk — bank them first. Phase 4 is the expensive median-angle parity fight;
worth doing, but it must not block the winnable advantages. Each phase is one
OpenSpec change proposal with the exit criterion above as its acceptance test.

## Guardrails

- The default `field-aligned` quadrangulator and golden tests stay
  byte-identical unless a change explicitly targets them.
- GPL sources (AutoRemesher, QuadCover/CoMISo) are idea references only, never
  copied. QuadriFlow / Instant Meshes are permissive and attributed.
- Every claimed improvement ships with a harness number and a regression test.
