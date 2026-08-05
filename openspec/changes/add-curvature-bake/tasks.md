# Tasks: add-curvature-bake

- [ ] 1. Curvature estimation on the Target (per-vertex signed mean curvature
       via cotangent Laplacian or fitted quadric; cached on the Target, BVH-
       sampled like color)
- [ ] 2. Curvature + cavity map types in the bake dispatcher (range
       normalization parameter, midpoint-gray / concavity-only encodings)
- [ ] 3. Reachability: CLI map-type flags, C ABI, Python and Swift bindings,
       bake report entries
- [ ] 4. Tests: encoding correctness on analytic shapes (sphere, saddle,
       filleted box), cage-following parity with the normal bake, cancel path
- [ ] 5. Docs + CHANGELOG
