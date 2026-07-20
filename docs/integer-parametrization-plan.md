# Integer-Parametrization Extractor — Rewrite Plan (Phase 4b)

The path to close the median-angle gap with QuadriFlow. This is a dedicated,
multi-session build; this doc is the entry point so coding starts with direction.

## Why (grounded in the measured diagnosis)

- Our **bulk valence-4 quads already match QuadriFlow** (median 80.6°) — but they
  are only ~21% of the mesh. **~36% of vertices are irregular**, most of them
  *spurious* singularities (val-3/5 scattered by the local collapse-and-walk
  extraction), not true field singularities.
- **Every local shortcut is a proven dead end** (this session): valence-optimizing
  edge rotation trades topology for geometry (nets negative), triangle-pair merge
  is neutral, and the rotation-system walk drops malformed orbits at defects
  (holes → downstream non-manifold). See `memory/instant-meshes-next-step.md`.
- QuadriFlow's advantage is a **globally consistent integer parametrization**: it
  produces clean topology *and* geometry from the start, so singularities appear
  only where the field genuinely demands them, sparsely.

## What to build

Replace the collapse-and-walk extractor (`buildCollapsedGraph` + `extractFaces`
in `quad_extract.cpp`) with a global integer-grid extractor. **Keep** the field
stage (`computePositionField`: orientation 4-RoSy + position field + per-vertex
`scale`) — it is validated and reused.

### Stage 1 — Global integer coordinates (per vertex, per chart)
Assign each vertex an integer 2-D coordinate `(i,j)` in a consistent chart:
1. Build a spanning tree of the mesh graph.
2. Integrate coordinates along tree edges: for edge `(u,v)`, transport u's frame
   to v with the 4-RoSy **rotation** `r_uv` (from the orientation field) and add
   the **integer translation** `t_uv` (from the position field / `compatPosition`,
   already computed in the current A1 classification).
3. Non-tree edges carry a **holonomy defect** (the loop doesn't close to an
   integer): `d = c_v - (R(r_uv) c_u + t_uv)`.

### Stage 2 — Integer optimization (make loops close)
Resolve the defects so the grid is globally consistent, exactly where QuadriFlow
wins. Formulate as a **min-cost flow** over the face dual graph: each face is a
node, adjacent faces share an arc, push integer flow to cancel per-face defects
at minimum total cost. Reuse the bundled network-flow helper
(`ECMaxFlowHelper`/LEMON pattern the reference build already vendors) rather than
adding a dependency. Singularities are the residual defects that cannot be
cancelled — these are the *true, sparse* ones.

### Stage 3 — Extract the quad mesh from the integer grid
With consistent `(i,j)`, extract faces by tracing integer iso-lines / unit grid
cells (the standard QuadriFlow/`Parametrizer::ExtractMesh` step). Output is
watertight and mostly valence-4 by construction — no rotation-system walk, no
malformed-orbit holes, no post-hoc rotation.

## Integration & guardrails

- New file `src/quadrangulate/src/integer_extract.cpp`; wire behind a new
  `quad_method` value (`"integer"` / `CYBER_QUAD_INTEGER`) so the existing
  `field-aligned` (default) and `instant-meshes` paths stay byte-identical while
  it stabilizes. Promote to the extractor default only once it beats the current
  one on the benchmark.
- Licensing: QuadriFlow is BSD-3 (permissive) — clean-room reimplement the
  algorithm, attribute in `THIRD_PARTY_NOTICES.md`, copy no source. AutoRemesher /
  QuadCover / CoMISo are GPL — ideas only.
- Every milestone gates on `examples/11_benchmark.py`: irregular-vertex % and
  median angle are the North Star. Exit: **irregular % < 15% and median ≥
  QuadriFlow** on the corpus.

## Milestones (each its own focused session / workflow)

1. ✅ **Coordinates + defect measurement — DONE** (`measureIntegerConsistency`,
   commit 4451261). Per-face holonomy of the field connection. **Finding: the
   field is VIABLE for the solve.** The orientation field is clean — only 24–48
   singular faces on the corpus (cylinder 0), so the earlier fear (from a
   measurement bug) is unfounded. Remaining defects are *translation*, small and
   local (meanDefect ~0.7–1.7, mostly ±1 at cell boundaries — the position-field
   round(0.5) noise), which is exactly what Stage 2 resolves. Two connection-math
   fixes were needed and are locked by the cylinder test: rotation index =
   `bb − ba` (aligned-rep index difference), and per-FACE (contractible) holonomy
   rather than spanning-tree loops (a cylinder's circumference legitimately wraps).
2. ✅ **Integer solve — DONE** (commit 27839cb). The min-cost flow cancels the
   position-field's spurious divergences: **icosphere 133→10 singularities**
   (near the ~8 genus-0 minimum), **spot 658→47** (~14×), **bunny 667→68** (~10×).
   Both checkpoints green — the residual recomputed from edge_diff/orient matches
   the position singularities (setup correct), and the flow removes the spurious
   defects. Single-level, closed genus-0; the `DisajointOrientTree` global
   alignment + `MinCostFlow` engine are the machinery. Regression test locks
   icosphere post < 2/5 pre. **Details below kept for reference.** Studied
   QuadriFlow's actual algorithm
   (`optimize_integer_constraints` in `examples/reference/QuadriFlow`) and
   reimplemented the reusable engine: **`MinCostFlow`** (SPFA successive shortest
   paths, unit-tested, commit a2de102). The exact formulation to build on it:
   - Each **face** has **2 constraint equations** (one per coordinate component):
     `equationID = face*2 + k`.
   - Each **edge** carries an integer diff `edge_diff[e]` (Vector2i). For face `i`,
     edge `j` with rotation `FQ[i][j]`, map the edge's two components to the face's
     equations via `index = rshift90((e*2+1, e*2+2), FQ[i][j])`; `index[k] = ±(comp)`
     gives which edge-component feeds equation `k` and its **sign** `s`.
   - `initial[equationID] = Σ s · edge_diff[e][comp]` — the residual divergence.
   - **Arcs:** for each edge-component bordering two equations with **opposite**
     signs, add an arc between them (adjusting that diff moves flow). Source→
     positive-`initial` equations (supply), negative→sink (demand).
   - Solve min-cost flow; the flow is the integer correction to `edge_diff`; apply
     and iterate until no residual (edge_capacity ~2 per pass).
   **Landed so far:** the `MinCostFlow` engine (tested) and the flow's INPUT —
   `debugPositionSingularities` (QuadriFlow's `ComputePositionSingularities`:
   per-face joint alignment of the 3 crosses, summed integer edge diffs; a nonzero
   sum is a singularity). Validated: cylinder 0.8% singular, corpus ~11% (spot
   658 ≈ the ~700 diagnosed spurious singularities). This is cleaner than the
   pairwise holonomy (the ±1 noise cancels within a face).

   **Remaining (the intricate heart):** build `edge_diff` + per-face `face_edgeOrients`
   (FQ) via the **`DisajointOrientTree`** — a union-find with relative orientation
   that spans the face graph (merging non-singular, non-sharp edges) to rotate
   every face into one global orientation frame; then label connected components
   so supply==demand per component; then build the flow network (per-face 2
   equations, edge-component→equation via `rshift90` signs, residual supply/demand,
   opposite-sign arcs), solve with `MinCostFlow`, apply the flow to `edge_diff`,
   iterate. QuadriFlow does it multi-resolution for scale; single-level is fine for
   our ~5k-face corpus. **Acceptance: cylinder residual → ~0 after the solve.**
   Port faithfully from `parametrizer-int.cpp` (BuildEdgeInfo, BuildIntegerConstraints,
   ComputeMaxFlow) and `optimizer.cpp` (optimize_integer_constraints); validate
   cylinder-first at every step (Milestone 1 had 2 sign bugs caught only that way).
3. 🟢 **Extraction — WATERTIGHT MANIFOLD DONE; irregular %/holes → Milestone 4.**
   `BuildTriangleManifold` ported (single-level): the extractor is now a valid
   manifold, all-quad and near-watertight across the whole corpus. Refactored the
   solve into a reusable
   `computeIntegerGrid` (commit a94fab7) that returns the post-flow grid state
   (tris, e2e, corrected edge_diff, face_edgeIds, orients) — QuadriFlow's
   post-`ComputeMaxFlow` state.
   - **(a) `subdivide_edgeDiff` — DONE (commit af02889).** Faithful port, but
     operating **purely on per-half-edge local-frame diffs** — a key simplification:
     the canonical `edge_diff`/`face_edgeOrients` mirror QuadriFlow keeps in
     lockstep (`AnalyzeOrient`/`FixOrient`/`face_spaces`) is **unneeded**, because
     the extractor's collapse (`diff==0`) and diagonal (`|diff|==(1,1)`) tests are
     rotation-invariant and those routines never mutate `diffs` themselves.
     Validated on icosphere: `maxDiff` → 1 at every spacing (1.0× mean 1280→1900
     tris, 0.5× 1280→6384, 0.33× 1280→16794), bounded, terminates. Regression test
     locks the invariant.
   - **(c-first) collapse + diagonal-pairing — DONE but hole-prone (commit
     37efffb).** Collapse zero-diff edges → grid vertices (mean field position),
     pair the two triangles across each `(1,1)` diagonal into a quad
     (`extractIntegerQuadMesh`). Output was **all-quad** (nonQuad==0) but **NOT
     WATERTIGHT:** ~30% true holes (icosphere: 312 boundary edges / 500 quads,
     39–52% irregular), and non-manifold at pinch points.
   - **(c-manifold) `BuildTriangleManifold` — DONE (this session).** Ported
     QuadriFlow's `BuildTriangleManifold` (parametrizer-flip.cpp ~267–580),
     single-level: (1) collapse zero-diff edges into a **compact triangle mesh**
     (the single-level stand-in for `DownsampleEdgeGraph` — drop triangles that
     straddle a collapsed edge); (2) **orbit-walk** each vertex fan into a per-fan
     manifold id, then the **manifold-repair loop** splits any non-manifold
     (bowtie/pinch) vertex into one vertex per directed-edge sub-loop; (3) pair
     `(1,1)` diagonals into quads; (4) drop degenerate quads; (5) **`FixHoles`**
     traces residual boundary loops (quad `E2E`) and fills each `<25`-vertex loop
     with `QuadEnergy` min-angle-DP quads (guarded by a recursion budget so a
     pathological loop can't blow up the exponential search). **Result across the
     whole corpus: a valid manifold, all-quad, near-watertight** — icosphere
     312→**26 boundary edges**, 45%→**20% irregular**; spot/bunny/fandisk/
     cheburashka/rocker-arm all `validate()`-clean, boundary edges ~1.5–4% of
     total, irregular 22–28%. Regression test asserts manifold + `nonQuad==0` +
     `boundary < quads/10` + `irregular < verts/4`.
   - **REMAINING — irregular % 22–28% (target < 15%) + a few residual triangular
     holes.** Boundary vertices from the small unfillable triangular holes (a cell
     diagonal with one clean triangle → a size-3 loop `QuadEnergy` cannot quad-fill)
     count as irregular and inflate the %. The remaining levers, deferred to
     Milestone 4 (quality/promote): port **`FixValence`** (parametrizer-mesh.cpp —
     doublet/valence-3 cleanup), close triangular holes (edge collapse / merge into
     a neighbour), and optionally `FixFlipHierarchy`/`FixFlipSat` for flipped cells.
     Open-mesh/boundary + sharp handling remain robustness follow-ups.
4. 🟡 **Quality + promote — doublet cleanup landed; promote gate NOT met (extraction
   fidelity is the real lever).** Measured the extractor's valence distribution and
   ran a head-to-head vs the current default extractor.
   - ✅ **Interior valence-2 doublet dissolution** (QuadriFlow `FixValence`, "Remove
     Valence 2") ported and shipped — restricted to *interior* doublets (merging a
     boundary valence-2 vertex opens seams: measured net-negative, irregular AND
     boundary both up). Modest safe win: irregular spot 28→25%, fandisk 22→21%,
     cheburashka 24→22%, rocker 26→24%, bunny 25→23%; boundary unchanged, still
     manifold. The high-valence split / decrease-valence passes are **net-negative
     on this extractor** (they open seams / trade topology for geometry, echoing the
     Phase-4a local-surgery finding) and are deliberately omitted.
   - ✅ **Head-to-head (native density):** the integer extractor **beats the current
     default extractor** on irregular % on all 5 corpus models (spot 25% vs 44%,
     rocker 24% vs 68%, cheburashka 22% vs 46%, bunny 23% vs 37%) and is watertight
     + manifold where the default tears.
   - ❌ **Promote gate NOT met.** Two blockers: (a) at the coarse ~3000-quad
     *benchmark* density the integer path degrades to ~37–49% irregular (opposite of
     the default, which is bad at native but ~15% at benchmark density); (b) neither
     reaches QuadriFlow's ~3% irregular. **Root cause LOCALISED (measured): the
     collapse→pair→manifold extraction corrupts a clean grid.** Residual
     singularities are *preserved exactly through subdivision* (spot solve 82 →
     after-subdivide 82, fandisk 180→180, cheburashka 234→234 — `SubdivideStats.
     residualAfter`), so the solve and `subdivide_edgeDiff` are clean; yet the
     extracted quad mesh has **5–10× more** irregular vertices (spot 82 → ~550),
     dominated by *interior* val3/val5. The multiplication happens entirely in
     `buildManifoldTris` / `buildQuadMesh`. Keep the current default extractor.
   - 🔬 **Bug-hunt round 1 (multi-agent workflow) — narrowed further, three suspects
     ruled out with numbers.** (a) The **pairing is clean**: spot forms 2391 quads
     from ~2439 cells, only 44 singletons + 26 rejects, **0 key collisions**. (b)
     **Vertex identity is clean**: manifold verts exceed distinct grid cells by only
     +0.4–2.6% (no orbit-walk fragmentation). (c) The **field is clean**: only **44
     orientation (rotation) singularities** on spot (fandisk 36, cheburashka 77) —
     and orientation holonomy is what SETS quad valence (val3/5), *not* the position
     residual (82) that the earlier note compared against. Yet extraction yields
     **388 interior irregular** on spot — ~9× the 44 field singularities. So ~344
     spurious val3/val5 are baked into the **grid connectivity** — round 2 (below)
     pins this as a quantization limit of the single-level collapse, resolved only
     by the multi-res `DownsampleEdgeGraph`.
   - 🔬 **Bug-hunt round 2 (workflow) — VERDICT: fundamental single-level limit, no
     fixable bug.** All three lenses agreed (conf 0.62–0.78); the pinpoint agent's
     decisive test settles it. The ~9× excess is **quantization**, not a discrete
     defect: irregular density scales *smoothly and inversely* with subdivision
     amount (spot 0.8× spacing → 3324 midpoints → 15.6% density; 1.4× → 968 →
     20.7%) — a knob, not a bug. The val3/val5 come as **adjacent dipole
     staircases** (55–61% of interior val3 sit next to a val5; v3≈v5 counts), the
     signature of a coarse grid, plus high-valence **collapse hubs**. This also
     *overturned* round 1's guess that subdivision was the culprit: `subdivide`
     is conforming and clean (each split's sub-face diffs sum to zero); the driver
     is the **zero-diff collapse**. Root cause: the collapse merges only vertices
     joined by a *direct* zero-diff edge, but two vertices share a grid cell iff
     *some path* between them has diffs summing to zero (net-zero equivalence, e.g.
     +1 then −1 around a corner). Local transitivity ⊊ lattice equivalence → some
     true grid vertices split (val3) and neighbours over-merge (val5/hubs). We have
     **no flip-repair layer at all**; QuadriFlow closes exactly this with
     `FixFlipHierarchy` over a coarsened `DownsampleEdgeGraph`. → **Phase 5.**

5. 🟠 **Field/grid foundation — the multi-resolution flip-repair layer (Phase 5)
   — BUILT IN FULL; real gain, but does NOT beat QuadriFlow.** Bottom line: the
   entire documented lever was implemented and validated (5a coarsen + 5b greedy
   flip repair + 5d SAT/MaxSAT), improving the integer extractor's interior
   irregular from ~25% to **~20%** — a genuine advance on the hardest axis — but it
   **plateaus far above QuadriFlow's ~3%** and the `<15%` exit is not met. The
   remaining gap is a deeper limitation of the single-level extraction structure
   that flip repair alone cannot close; the shipped default remains the
   field-aligned quadrangulator. Details below.
   The one remaining lever to beat QuadriFlow on singularities / median angle. It
   slots **between** the (done, validated) integer solve and the (done, validated)
   extraction — build a coarsened edge-graph hierarchy, repair flipped cells
   coarse-to-fine so the finest collapse is flip-free = one clean node per cell,
   then extract as today. QuadriFlow is MIT → clean-room port is licence-clean.

   **Port surface** (from `examples/reference/QuadriFlow/src`):

   | Piece | Source | ~Lines | Notes |
   |---|---|---|---|
   | `Hierarchy::DownsampleEdgeGraph` | hierarchy.cpp:417 | ~200 | build coarsened `(F2E,E2F,EdgeDiff,FQ)` levels |
   | `Hierarchy::FixFlip` (greedy) | hierarchy.cpp:923 | ~150 | detect (signed-area<0) + repair flipped cells |
   | `Hierarchy::UpdateGraphValue` | hierarchy.cpp:410 | ~40 | push repaired diffs back to the fine level |
   | `FixFlipSat` + `localsat` | hierarchy.cpp:640 + localsat.cpp | ~350 | **optional** SAT refinement — defer |

   ~400 lines for the greedy path (skip the SAT variant first) — the most
   index-heavy code in the project; genuinely multi-session.

   **Sub-milestones:**
   - ✅ **5a — DONE** (commit fd6e4e8). `EdgeHierarchy` (DownsampleEdgeGraph +
     PropagateEdge + UpdateGraphValue) + `faceArea`/`countFlipped`. Validated the
     highest-risk part first: coarsen+propagate with no repair is an **exact
     identity** on edge_diff (`roundTripMismatch == 0`) on cylinder (6 levels) and
     icosphere (4 levels). Confirmed the flipped-cell count ≈ interior-irregular
     count (spot 371 flips vs ~388 irregular) — the flips ARE the dipoles.
   - ✅ **5b — DONE** (commit a4ca742). Greedy `FixFlip` + `CheckShrink`, wired into
     `extractIntegerQuadMesh`. **Real win, all 5 models:** interior irregular spot
     25.2→**20.2%**, fandisk 21.9→19.8%, cheburashka 24.2→**18.9%**, rocker
     25.9→22.4%, bunny 24.7→20.8%; still manifold / all-quad / watertight, residual
     invariant. Flips: spot 371→198, cheburashka 609→363.
   - 🟡 **5c — measured; greedy at its limit.** ~19–22% irregular (target <15%,
     QuadriFlow ~3%). The greedy is provably converged (identical at maxLen 2/4 and
     under repeated passes): the residual ~half of the flips are multi-cell clusters
     no single-ring shrink can fix. **Not across the line yet.**
   - 🟠 **5d — `FixFlipSat` DONE + measured NET-NEGATIVE; NOT on the default path**
     (commits 2103f5f solver, b065da3 glue, dc8f647 wired, 35bf5d1 unwired).
     Correction that reshaped it: `localsat.cpp` is **not a solver** — it is a CNF
     encoder that shells out to the external **`minisat` binary** (localsat.cpp:39),
     and `FixFlipSat` no-ops without it (hierarchy.cpp:641). QuadriFlow bundles no
     solver, and an external `minisat` dep is rejected — so we wrote a **self-
     contained bounded backtracking ternary-CSP solver** from scratch (hard exactly-
     one + face zero-sum equalities, area-≥0 flip inequalities; `debugTernaryCsp`
     regression), ported `FixFlipSat`'s patch extraction + constraint encoding, and
     added the `SubMesh↔IntegerGrid` inversion so it runs on the unit grid
     (`debugSubMeshDiffRoundTrip == 0`, validated). **Result: net-negative.** It
     optimises the *coarse*-level flip count, which does not reliably lower the
     *final* mesh's irregulars (spot 263→249, ~1pt; cheburashka slightly worse) and
     costs **3–21 s/mesh**. A **MaxSAT** branch-and-bound variant (minimise flips —
     which QuadriFlow's decision-only minisat cannot) gave **no downstream gain**
     (coarse-flip optimisation ≠ final quality). So the SAT is kept as validated
     infrastructure (`debugSatPreservesIntegrability`) but is **off the default
     extractor**; the shipped integer path is fast greedy-only flip repair (~20%).

   **Exit:** interior irregular approaches the field's true singularity count —
   **irregular % < 15% and median angle ≥ QuadriFlow** on the corpus (gated on
   `examples/11_benchmark.py`), promoting the integer extractor to default.

   **Risk — high, specific:** (1) the hierarchy is intricate (cross-level union-find
   + orientation bookkeeping). (2) M3 dropped the canonical `edge_diff`/
   `face_edgeOrients` mirror (works on per-half-edge *local-frame* diffs, since the
   single-level pairing tests are rotation-invariant); `DownsampleEdgeGraph` threads
   the canonical `EdgeDiff`/`FQ` through levels, so Phase 5 likely forces carrying
   that mirror after all — partially reworking `computeIntegerGrid`'s output.

   **Recommended entry:** a time-boxed **5a+5b spike** — the smallest slice that can
   show spot's dipoles collapsing — as a go/no-go before committing the full port.
   No cheaper alternative exists: the only single-level-fixable slice (midpoint
   T-junctions) is ~45% co-located but only ~1.3× enriched, so it can't dent the 9×
   gap. Weigh against the wins already banked (adaptivity 4/5, hard-surface
   robustness) — Phase 5 buys the *median-angle* axis specifically.

## Open risk (revised after Milestone 1)

The Milestone-1 finding *lowers* the top risk: the orientation field is clean and
the position defects are small/local, so the solve has a tractable job. Residual
watch item: the cylinder is only ~63% clean-face (position-field boundary noise),
so Stage 2 must handle a fair density of ±1 defects — expected, but confirm the
flow scales. The acceptance test for Milestone 1's math is the cylinder
orientation holonomy = 0 (regression added).
