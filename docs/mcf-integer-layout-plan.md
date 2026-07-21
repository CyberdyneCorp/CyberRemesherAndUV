# Min-cost-flow integer layout — matching QuadriFlow on thin features

## Motivation (why this, why now)

The native QuadCover path trails QuadriFlow specifically on thin, high-curvature features — the
Stanford-bunny **ears** are the flagship: ~35 irregular vertices in the ear region vs QuadriFlow's
~8. Six downstream levers were measured and rejected (feature-edge threshold, curvature-weighted
seam routing, multiresolution cross field, tube-aware coarsening, tightened count calibration, and a
post-extraction ear re-quad) — see `docs/native-miq-plan.md` M8 and the memory findings. All failed
for the **same structural reason**, confirmed by reading the QuadriFlow source
(`examples/reference/QuadriFlow/src`):

- QF does **not** use adaptive sizing here (`flag_adaptive_scale` is off by default) — it lays the
  ear out at the same uniform edge length we do. The difference is **where the singularities go**.
- QF's pipeline ends with a **global min-cost max-flow integer layout** (`main.cpp:112` →
  `ComputeIndexMap` → `ComputeMaxFlow` → `optimize_integer_constraints(..., use_minimum_cost_flow)`,
  `optimizer.cpp:1218`, backed by `flow.hpp` = Boost Boykov–Kolmogorov / LEMON preflow). It encodes
  the quad grid as per-edge integer differences (`edge_diff`) with a flow-conservation constraint per
  non-singular face, and solves a **min-cost flow** for the cheapest integer-seamless correction. On
  a tapering tube the unavoidable ring-reductions are thereby concentrated into a **few optimally
  placed singularities**.
- Our native path has **no global integer-layout optimizer**: it solves the seam MIQ, then traces
  isolines **locally** (`extractIsolineQuads`), so reductions land wherever isolines collide and
  **scatter**. Our integer path (`makeIntegerQuadrangulator`, M3–5) uses `BuildTriangleManifold` +
  `FixValence` + greedy flips, also not a flow — bunny irr 12–14. So neither path has QF's optimizer.

**Conclusion:** the gap is the min-cost-flow integer-layout stage. This is the one lever with the
ceiling to actually close the ear gap; everything else is downstream of where the damage is done.

## Tooling — company numerical libraries (no Boost/LEMON)

Instead of Boost/LEMON, use the in-house C++20 libraries:
- **NumPP** (https://github.com/CyberdyneCorp/NumPP) — NumPy port, `numpp::ndarray`, zero required
  deps, `find_package(NumPP)` / `add_subdirectory`.
- **SciPP** (https://github.com/CyberdyneCorp/SciPP) — SciPy port on NumPP, `scipp::scipp`.
  Provides exactly the primitive QF needs, in `scipp::sparse::csgraph`:
  - `MaximumFlowResult maximum_flow(const CsrMatrix& graph, int64_t source, int64_t sink)` — the
    core (matches QF's max-flow feasibility / "full flow").
  - `bellman_ford` / `johnson` / `dijkstra` — for the min-cost refinement via successive-shortest-
    paths (SciPP has no built-in `min_cost_flow`; QF's min-cost is an *option* over max-flow anyway).
  - Also `maximum_bipartite_matching`, and sparse CG/GMRES/`spsolve` (could later replace the
    hand-rolled Poisson CG in `seamless_solver.cpp`).

Both are optional, CMake-gated (like `CYBER_WITH_QUADCOVER`) so the **default build stays
dependency-free** and this is an opt-in enhancement.

## Milestones

**M0 — Toolchain de-risk — ✅ DONE.** Cloned + built + installed NumPP and SciPP standalone in this
environment (C++20, gcc, Release), `find_package(SciPP CONFIG)` resolves both, and a smoke test
(`scipp::sparse::csgraph::maximum_flow` on a 4-node network) returns the correct max-flow = 5. Proves
the exact integration path and the core algorithm are available and working before any repo change.

**M1 — Build integration — ✅ DONE.** `option(CYBER_WITH_SCIPP ...)` (default OFF) + `cmake/SciPP.cmake`
(`cyber_add_scipp()`) clones NumPP+SciPP into `examples/reference/scipp-src/` on demand, builds+installs
both into `${CMAKE_BINARY_DIR}/scipp-install` at configure time (cached), and imports `scipp::scipp`,
which is linked `PRIVATE` into `cyber_quadrangulate` with `CYBER_HAVE_SCIPP`. `src/mcf_layout.cpp`
(always compiled; SciPP includes `#ifdef`-guarded so the default build stays dependency-free) exposes
`mcfMaxFlowSelfTest()`, covered by `tests/quadrangulate/test_mcf_layout.cpp` — built only under the
option. Verified: `-DCYBER_WITH_SCIPP=ON` imports SciPP 1.6.0, the tree builds, and the full unit suite
(256 cases incl. the max-flow test) is green; the default (SciPP-off) build and tests are unchanged.
Wrinkle solved: SciPPConfig does `find_dependency(NumPP)`, so `cmake/SciPP.cmake` sets `NumPP_DIR` +
prepends the prefix to `CMAKE_PREFIX_PATH` before `find_package(SciPP)`.

**M2 — edge_diff representation — ✅ DONE.** `buildMcfEdgeInfo(mesh, PositionField)` in
`src/mcf_edgediff.cpp` ports QuadriFlow's `ComputeOrientationSingularities` +
`ComputePositionSingularities` + `BuildEdgeInfo` (parametrizer-sing.cpp, parametrizer-int.cpp) onto
our `Mesh` + `PositionField` (orientation `q`, lattice position `o`, normal, `spacing`). Produces
per-edge integer differences (`edgeDiff`, a `Vec2i` = lattice cells spanned in u,v), `edgeValues`,
`faceEdgeIds`, and the orientation/position singularity sets — the exact representation the M3 flow
operates on. **Pure integer/vector math (no SciPP)**, done in `double` to match QF's floor-index
precision, so it is covered by the default build. Regression test (`test_mcf_edgediff.cpp`): a flat
unit grid yields zero orientation/position singularities, every edge spans ≤1 cell per axis (axis
edges `(±1,0)/(0,±1)`, diagonals `(±1,±1)`), and every face's stored edge ids close. Full suite (256
cases) green. NOTE: this consumes the `computePositionField` field foundation (with the M8 multires +
tube-aware coarsening), mirroring QF's pipeline rather than our MIQ/isoline path.

**M3 — Integer constraint graph + max-flow.** Port QF `BuildIntegerConstraints` (365 lines) +
`optimize_integer_constraints`, done in verified sub-parts:
- **M3a — constraint graph — ✅ DONE.** `buildMcfConstraints(mesh, field, McfEdgeInfo)` in
  `src/mcf_constraints.cpp` ports the setup half of `BuildIntegerConstraints`: per-face edge
  orientations that align shared edges into one frame via a ported disjoint-set orient tree
  (QuadriFlow's `DisajointOrientTree`), the undirected→directed edge map (`e2d`), the connected
  components of non-fixed edges (`sharpColor`/`numComponents`), and each component's net integer
  residual (`totalFlow`) the flow must cancel. M2 now also exposes the post-flip orientation
  (`McfEdgeInfo::orient`) so the two stages read the same field. Shared field-math helpers factored
  into `src/mcf_detail.hpp`. Pure integer math (no SciPP). Test: on a flat grid → one orientation
  component, zero residual, valid orientations; full suite (256) green.
- **M3b — full-flow variables — ✅ DONE.** `buildMcfFlowSetup(mesh, McfEdgeInfo, McfConstraints)`
  in `src/mcf_flow.cpp` ports the tail of `BuildIntegerConstraints`: the per-scalar-variable table
  (which faces reference each edge_diff component + net sign), the `allowChanges` mask (a variable is
  fixed when it is a zero-component of a sharp edge between two sharp vertices), and the deterministic
  (`rng_seed = 0`) "full-flow" pre-adjustment that nudges a working COPY of `edgeDiff` so each
  component's residual becomes reducible by the flow. Pure integer math (no SciPP). Test: on a clean
  no-feature grid every variable is free, the residual stays zero so `edgeDiff` is untouched, and
  interior edges' variables are referenced by two face slots; full suite (256) green.
- **M3c — max-flow solve — ✅ DONE.** `solveMcfFlow(McfEdgeInfo, McfConstraints, McfFlowSetup)` in
  `src/mcf_layout.cpp` (SciPP-gated) is a single-level port of `optimize_integer_constraints`
  (optimizer.cpp): from each face's oriented edge-diff residual it builds the flow network
  (source → surplus equations, deficit equations → sink, variable-arcs between equations), solves it
  with `scipp::sparse::csgraph::maximum_flow`, and applies the flow back onto a working `edgeDiff`.
  Each regular arc gets a unique middle node so parallel arcs stay distinct in the CSR (`from_coo`
  sums duplicates); `edge_capacity` is raised (≤10 rounds) until the flow saturates supply. The
  QuadriFlow hierarchy (`DownsampleEdgeGraph`) is skipped — a single finest-level solve is correct,
  just slower. Tests (`test_mcf_flow.cpp`, built under `-DCYBER_WITH_SCIPP=ON`): an already-seamless
  grid is left untouched (supply 0, full flow); a perturbed edge is **corrected back to a seamless
  layout** (supply > 0, full flow, every non-singular face's oriented sum returns to zero). SciPP
  suite (259) + default suite (256) green.

**M4 — Min-cost refinement (optional).** Successive-shortest-paths over the residual graph using
`csgraph::johnson`/`dijkstra` for minimum-cost placement (QF's `use_minimum_cost_flow` path).

**M5 — Integer extraction.** Extract quads directly from the corrected integer layout (QF
`ComputeIndexMap` tail + `subdivide` + face tracing) — replaces isoline tracing on this path.

**M6 — Wire as a quadrangulator + benchmark.** Expose as a quad method (e.g. `mcf`, or fold into
quad-cover behind the option), A/B on the bunny ears + full corpus, promote if it wins. Success =
bunny ear irregulars toward QF's ~8 and overall irr% at/below QF, without regressing the corpus.

## Risks

- **Scale/perf of the flow solve** — meshes have 10k+ dual edges; `maximum_flow` must be fast on
  sparse CSR. QF leans on specialized max-flow for exactly this. Validate on the largest corpus mesh
  early (M3), fall back to component-wise solves if needed.
- **Faithful port of the edge-diff bookkeeping** — QF's `parametrizer-int.cpp` (~600 lines) is
  intricate (rotations, sign conventions, sharp/singular handling). This is the bulk of the work and
  the main correctness risk; port with QF's own test data as an oracle where possible.
- **Dependency footprint** — adds two compiled C++20 libs to opt-in builds; the dependency-free
  default (single-level field + isoline extraction) remains untouched.
