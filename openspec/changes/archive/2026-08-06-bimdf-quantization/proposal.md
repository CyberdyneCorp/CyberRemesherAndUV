# Proposal: bimdf-quantization

## Why

Two independent measurement campaigns (2026-08-02, docs/ROADMAP.md) converge on
the same remaining lever for the quad-cover default:

- **Flow-loop length** (gap #2): after eliminating the loop terminators
  (residual triangles via `--pure-quads`, then valence-3/5 dipoles), mean loop
  length improved 41 → 433 on spot but still trails QuadriFlow's 1811. The
  remaining ~4x is global grid structure — how integer isoline counts are
  assigned — not local defects.
- **Dense-organic singularities** (gap #4): field-level dipole annihilation
  saturates at nefertiti 316-347 vs QuadriFlow's 80 (349 of 521 cones
  unpartnerable past the pseudo-feature pin web). The residual cones are a
  consequence of per-seam greedy integer rounding, which our own reduced MIQ
  performs one translation at a time with no global objective.

The literature's answer to both is quantization as a global optimization:
QuadWild's Bi-MDF (Heistermann et al. 2023, "Min-Deviation-Flow in Bi-directed
Graphs for T-Mesh Quantization") solves the integer assignment as a
min-deviation flow over the T-mesh/motorcycle-graph structure — 0.49% of the
IQP runtime with 11% lower energy, and it is the measured difference between
"wavy" and "clean" edge flow in the quadwild-bimdf reference binary our bench
already runs. GPL discipline: quadwild/libsatsuma code is never read or
vendored; the implementation derives from the papers alone.

## What Changes

- The native seamless solve's greedy integer rounding (solveSeamlessReduced)
  gains an alternative quantization backend: a motorcycle-graph / T-mesh
  decomposition of the seamless parameterization, arc length variables, and a
  min-deviation-flow solve (min-cost flow with convex per-arc deviation costs
  on a bi-directed graph) assigning all integer isoline counts globally.
- Feature-seam integer pins (the shipped gap-#1 lever) become flow constraints
  (fixed arcs), preserving box_sharp recall 1.00 / 8 cones / 0.0 degrees.
- Ships behind `CYBER_QC_BIMDF` with the greedy rounding as fallback and kill
  switch; becomes default only on a full-corpus, multi-density win per the
  repo's measurement rules (count-matched, >=7 densities).

## Impact

- Affected specs: remeshing-pipeline (quantization stage semantics)
- Affected code: src/quadrangulate (seamless_solver, new quantize module);
  no third-party deps (in-tree min-cost-flow; license audit unchanged)
- Exit gate: spot flow_loop_mean_len >= 1000 count-matched; nefertiti
  cyber-pure singularities <= 200; no regression on any recorded bench metric.
