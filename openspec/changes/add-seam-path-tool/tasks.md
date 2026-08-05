# Tasks: add-seam-path-tool

- [ ] 1. Edge-cost model (feature tags, dihedral/curvature bias, flat-region
       penalty) over the existing shortest-path machinery
- [ ] 2. Pending-path state: waypoints, per-segment routes, reposition/delete
       with local re-route, commit → seam edges, resume marker semantics
- [ ] 3. ABI tool ops + Python/Swift bindings; undo integration
- [ ] 4. Tests: groove-following vs geodesic, local re-route isolation,
       commit/resume/drop invariants, seam-model integration (gesture unwrap
       over a routed seam)
- [ ] 5. Docs + CHANGELOG
