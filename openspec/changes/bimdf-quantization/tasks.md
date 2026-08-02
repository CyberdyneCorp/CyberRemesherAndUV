# Tasks: bimdf-quantization

- [ ] 1. Motorcycle-graph / T-mesh extraction from the seamless parameterization
       (trace separatrices from cones and feature junctions; arcs = T-mesh edges
       with real-valued lengths from the relaxed solve)
- [ ] 2. Bi-directed flow-network construction per the Bi-MDF paper: arc
       variables = integer isoline counts, node conservation from T-mesh faces,
       feature-pinned arcs fixed (gap-#1 semantics preserved)
- [ ] 3. In-tree min-deviation-flow solver (successive shortest augmenting path
       with convex costs; no third-party deps)
- [ ] 4. Wire behind CYBER_QC_BIMDF into solveSeamlessReduced as the integer
       assignment, greedy rounding fallback + kill switch; byte-exact revert
- [ ] 5. Corpus + multi-density A/B vs greedy (bench gates + cad_sweep.py);
       flip default only on a clean win; ROADMAP entry with numbers
