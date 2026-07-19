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
3. 🟡 **Extraction — IN PROGRESS. Stages (a) + first (c) landed; needs
   `BuildTriangleManifold`.** Refactored the solve into a reusable
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
     (`extractIntegerQuadMesh`). Output is **all-quad** (nonQuad==0). **BUT NOT
     WATERTIGHT:** ~30% true holes (icosphere: 312 boundary edges / 500 quads,
     39–52% irregular). Confirmed *true holes* (a cell diagonal with only one clean
     triangle), not winding artifacts — the mesh edge model is undirected, so
     boundary edges = genuinely single-quad cells.
   - **REMAINING — the fix is QuadriFlow's `BuildTriangleManifold`
     (parametrizer-flip.cpp ~267–580).** Direct pairing on the *collapsed fine*
     triangulation is fundamentally hole-prone; QuadriFlow first reconstructs a
     **clean compact triangle manifold** over the grid vertices (orbit walks +
     manifold-repair loop over the fully-downsampled edge graph `F2E/E2F/EdgeDiff/FQ`),
     *then* pairs diagonals, then `FixHoles`/`FixValence`. Single-level: build the
     compact-triangle edge structure directly (skip `DownsampleEdgeGraph`'s
     multi-resolution). Also port `FixFlipHierarchy`/`FixFlipSat` (parametrizer-flip.cpp)
     to repair flipped cells, and `FixValence`/`FixHoles` (parametrizer-mesh.cpp).
     Target: watertight, irregular % < 15% (from the solve's ~47 spot
     singularities). Open-mesh/boundary + sharp handling remain robustness
     follow-ups. **The next focused session.**
4. ◻ **Quality + promote** — median/CV/feature/robustness vs QuadriFlow; if it
   wins, make it the extractor default and retire the collapse-and-walk path.

## Open risk (revised after Milestone 1)

The Milestone-1 finding *lowers* the top risk: the orientation field is clean and
the position defects are small/local, so the solve has a tractable job. Residual
watch item: the cylinder is only ~63% clean-face (position-field boundary noise),
so Stage 2 must handle a fair density of ±1 defects — expected, but confirm the
flow scales. The acceptance test for Milestone 1's math is the cylinder
orientation holonomy = 0 (regression added).
