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
2. ◻ **Integer solve** — Stage 2 min-cost-flow over the face dual; push flow to
   cancel the ±1 translation defects; verify residual singularities drop to a
   sparse set. **This is the next focused session.** Reuse the field connection
   already built in Milestone 1.
3. ◻ **Extraction** — Stage 3; measure irregular % (target < 15%) and validity
   (watertight, manifold) vs the current extractor.
4. ◻ **Quality + promote** — median/CV/feature/robustness vs QuadriFlow; if it
   wins, make it the extractor default and retire the collapse-and-walk path.

## Open risk (revised after Milestone 1)

The Milestone-1 finding *lowers* the top risk: the orientation field is clean and
the position defects are small/local, so the solve has a tractable job. Residual
watch item: the cylinder is only ~63% clean-face (position-field boundary noise),
so Stage 2 must handle a fair density of ±1 defects — expected, but confirm the
flow scales. The acceptance test for Milestone 1's math is the cylinder
orientation holonomy = 0 (regression added).
