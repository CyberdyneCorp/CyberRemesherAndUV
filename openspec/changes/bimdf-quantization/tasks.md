# Tasks: bimdf-quantization

- [x] 1. Motorcycle-graph / T-mesh extraction from the seamless parameterization
       (trace separatrices from cones and feature junctions; arcs = T-mesh edges
       with real-valued lengths from the relaxed solve)
       — landed (`src/quadrangulate/src/bimdf_quantize.cpp`): lockstep tracing,
       fold-robust contour following, exact symbolic arc lengths, crease
       chains, quad-patch extraction. Clean on CAD (box_sharp: 8 nodes /
       12 arcs / 6 patches, exprErr 0); organic meshes still fail patch
       validation on folded relaxed charts (spot 107/222 non-quad patches,
       nefertiti blocked by a work-mesh crack) and fall back to greedy.
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
       — BLOCKED on T-mesh extraction over folded relaxed maps: the exit-gate
       meshes (spot, nefertiti) currently fall back to greedy, so the gates
       (spot flow_loop_mean_len >= 1000, nefertiti cyber-pure sings <= 200)
       are not yet measurable. A/B harness runs confirm no regression with the
       flag on (fallback reproduces greedy; box identical). ROADMAP updated
       with the landed state and the ranked follow-ups (injective substrate /
       even-sum template / S2). Default NOT flipped.
