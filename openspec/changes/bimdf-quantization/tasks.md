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
       — STILL BLOCKED, one stage further along: the crack class is gone and
       every corpus mesh traces a complete T-mesh, but the fold-corrupted
       corner sectors (degenerate ≤2-corner orbits: nefertiti@4000 306 of 596
       rejected, spot 8 of 18) keep organics from patch validation, so the
       gates (spot flow_loop_mean_len >= 1000, nefertiti cyber-pure
       sings <= 200) are not yet measurable. Measured corner-valence
       histograms show the even-sum polygonal template alone would unblock 0
       meshes (template-coverable 3/5/6 are the minority of rejects). A/B
       harness green with the flag on AND off (box outputs byte-identical
       incl. @16000, energy 0.014 = greedy). Ranked next: QEx Alg-8
       signed-angle sector classification in buildRotation, then the
       even-sum template as mop-up, then QGP §7.1 re-linearization.
       Default NOT flipped.
