# Tasks: bimdf-quantization

- [x] 1. Motorcycle-graph / T-mesh extraction from the seamless parameterization
       (trace separatrices from cones and feature junctions; arcs = T-mesh edges
       with real-valued lengths from the relaxed solve)
       — landed (`src/quadrangulate/src/bimdf_quantize.cpp`): lockstep tracing,
       fold-robust contour following, exact symbolic arc lengths, crease
       chains, quad-patch extraction. Clean on CAD (box_sharp: 8 nodes /
       12 arcs / 6 patches, exprErr 0). The "work-mesh crack" fallbacks were
       non-manifold edges made by the isotropic collapse pass (no link
       condition) — fixed in the mesh kernel (`Mesh::collapseEdge` +
       CollapsePass obstruction flips); box_sharp@16000 is now injectable.
       Fold tolerance at cone launches landed (signed UV wedge ranking +
       rescue candidates, entry-edge re-crossing, regular-vertex
       pass-through, graceful ray abandonment): every corpus mesh now traces
       a complete T-mesh (spot 18/194 non-quad patches, was 107/222;
       nefertiti@4000 596/2316; armadillo@4000 294/1386); the residual
       non-quad orbits are fold-corrupted corner SECTORS (rotation side),
       dominated by degenerate ≤2-corner orbits.
- [x] 2. Bi-directed flow-network construction per the Bi-MDF paper: arc
       variables = integer isoline counts, node conservation from T-mesh faces,
       feature-pinned arcs fixed (gap-#1 semantics preserved)
       — split-node template in half-cell units; crease arcs are ordinary
       arcs (their transverse lattice pins live in the c_e integers, which the
       back-substitution leaves to the reduction); min-one bounds are the
       degenerate-assignment guard with `raisedToMin` reporting.
- [x] 3. In-tree min-deviation-flow solver (successive shortest augmenting path
       with convex costs; no third-party deps)
       — S1 approximate path: T-join parity + Hochbaum double cover on the
       extracted SPFA `MinCostFlow` (`min_cost_flow.hpp`), convex PWL costs as
       parallel arcs; unit-tested (`tests/quadrangulate/test_bimdf_quantize.cpp`).
       S2 (M=2 matching refinement) not started.
- [x] 4. Wire behind CYBER_QC_BIMDF into solveSeamlessReduced as the integer
       assignment, greedy rounding fallback + kill switch; byte-exact revert
       — back-substitution through the run's actual pivot expressions, one
       batch pin + single direct.resolve, greedy finishes the remainder;
       `CYBER_QC_BIMDF=report` = A/B mode without injection; flag off is
       byte-exact (ctest 14/14; box output geometry identical on/off).
- [ ] 5. Corpus + multi-density A/B vs greedy (bench gates + cad_sweep.py);
       flip default only on a clean win; ROADMAP entry with numbers
       — STILL BLOCKED, three stages further along (branch
       `feat/bimdf-tail`, after `feat/bimdf-sectors`): (i) QEx Alg-8
       sectors + even-sum polygonal template (see ROADMAP 2026-08-02);
       (ii) twin-arc merge / bigon collapse with phantom quarters (rejected
       orbits nefertiti@4000 377 → 143, deg ≤2 224 → 49; armadillo 174 →
       45; spot 11 → 3); (iii) LOCAL CONTAINMENT — failed rays, rejected
       orbits and relax-inconsistent quads no longer refuse the T-mesh
       (their arcs are excluded from injection, greedy rounds them
       locally), plus solve-side support (fixed boundary constants,
       T-join ground, infeasibility drop valve, side-level anti-collapse
       floors, ±1 repair sweep) — every corpus organic AND the open-boundary
       stanford-bunny now BUILD and SOLVE (nefertiti@4000: 1896/2078
       patches, halfIntegral 0, sideViolation 2; bunny: 441/471). The
       injection blocker is now precisely the JOINT HALF-INTEGER LATTICE:
       ~half the eliminated pivots land at frac exactly 0.5 (spot 71/136
       clean, nefertiti 861/1649) because the network does not model the
       parity coupling of half-integer cone positions; partial pinning
       (CYBER_QC_BIMDF=force) measured: bunny ears 37 → 31 with per-arc
       floor 0, but nefertiti sing 396 → 431 — so DEFAULT injection
       requires full consistency + zero contained arcs (A/B: every cell
       hash SAME, dE identical; bench check green both states; box_sharp
       8 cones / 1.00 / 0.0°; ctest 14/14). Gates spot@3000 pure flow
       762.9 (≥ 1000) and nefertiti@4000 pure sing 419 (≤ 200) NOT met.
       Ranked next: parity-aware quantization (lattice constraints into the
       Bi-MDF), guided rounding (integral-by-construction greedy toward the
       flow targets), boundary arcs. Default NOT flipped.
