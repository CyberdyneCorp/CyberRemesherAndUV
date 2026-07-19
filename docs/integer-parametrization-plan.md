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
3. 🟡 **Extraction — STARTED; needs the full `ExtractMesh` port.** A shortcut was
   tried and rejected: collapsing on `edge_diff==0` and taking the unit-edge
   lattice adjacency does NOT yield a clean quad mesh — it reproduces the same
   lattice under-connectivity the collapse-and-walk extractor had (spot ~39%
   valence-4 by that measure), because it skips the real machinery. Diagnosis of
   the corrected `edge_diff` (env `CYBER_ISOLVE_DIAG`): on a properly-sized mesh
   (spot at target edge length) it is in the right regime — 4248 unit + 2986
   diagonal edges — confirming the solve produces a usable grid; on a mesh
   coarser than the grid (a low-subdiv icosphere) edges span many cells (large
   diffs), so the mesh MUST be at ~target edge length first.
   **The real extraction is QuadriFlow's `ExtractMesh` / `ComputeIndexMap` tail:**
   (a) `subdivide_edgeDiff` — split any edge whose |diff|>1 so every edge spans ≤1
   grid cell; (b) `FixFlipHierarchy`/`FixFlipSat` — repair flipped/degenerate
   cells; (c) assign integer coordinates and build the compact quad mesh
   (`O_compact`/`F_compact`) — grid vertex = output vertex, grid cell = quad. Port
   from `parametrizer.cpp` `ComputeIndexMap` (line ~78 onward) + `parametrizer-flip.cpp`.
   Measure irregular % (target < 15%, from the solve's ~47 spot singularities) and
   validity vs the current extractor. Open-mesh/boundary + sharp handling remain
   robustness follow-ups. **Substantial — its own focused session.**
4. ◻ **Quality + promote** — median/CV/feature/robustness vs QuadriFlow; if it
   wins, make it the extractor default and retire the collapse-and-walk path.

## Open risk (revised after Milestone 1)

The Milestone-1 finding *lowers* the top risk: the orientation field is clean and
the position defects are small/local, so the solve has a tractable job. Residual
watch item: the cylinder is only ~63% clean-face (position-field boundary noise),
so Stage 2 must handle a fair density of ±1 defects — expected, but confirm the
flow scales. The acceptance test for Milestone 1's math is the cylinder
orientation holonomy = 0 (regression added).
