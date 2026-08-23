# Tasks: add-curvature-bake

- [x] 1. Curvature estimation on the Target (per-vertex signed mean curvature
       via cotangent Laplacian or fitted quadric; cached on the Target, BVH-
       sampled like color)
       — landed as `cyber/bake/curvature.hpp` + `src/curvature.cpp`: Meyer et
       al. cotangent mean-curvature normal over a mixed Voronoi area (obtuse
       triangles fall back to halved/quartered area so every share stays
       positive and the sign cannot flip), n-gons fan-triangulated exactly as
       the BVH does, boundary 1-rings replaced by the mean of their interior
       neighbours. Convex reads positive; a sphere of radius r reads 1/r.
       Sampled at the cage hit by barycentric blend (`hitScalar`), the same way
       Color is. NOT cached on the Target: `bake()` takes `const Mesh&`, and
       the estimate is O(V) against an O(texels) raycast, so it is recomputed
       per call — expose a cache only if a profile ever justifies it.
- [x] 2. Curvature + cavity map types in the bake dispatcher (range
       normalization parameter, midpoint-gray / concavity-only encodings)
       — `BakeMap::Curvature` / `BakeMap::Cavity`, single channel each.
       `BakeParams::curvatureRange` = 0 means auto: the 95th percentile of
       |curvature| over the Target (`curvatureScale`), which is
       scale-independent and outlier-resistant. A flat Target (range 0) and a
       missed cage ray both encode to the neutral value — mid-gray for
       curvature, white for cavity.
- [~] 3. Reachability: CLI map-type flags, C ABI, Python and Swift bindings,
       bake report entries
       — `CYBER_BAKE_CURVATURE` / `CYBER_BAKE_CAVITY` + the `curvatureRange`
       field in `CyberBakeParams`; `BakeMap.CURVATURE` / `.CAVITY` and
       `BakeParams.curvature_range` in Python; `app::BakeMapKind` extended
       append-only so saved documents keep their meaning. Downgraded to PARTIAL:
       the C ABI and Python halves are done, and the CLI half now exists too —
       since `add-export-presets` / `add-claycore-bridge` landed a baking CLI,
       `cyberremesh --bake curvature,cavity --texture-size 64` writes
       `*_curvature.png` / `*_cavity.png` and lists both in the report's
       `outputs` block (verified on `cube.obj`). **Still missing: Swift.**
       `swift/Sources/CyberRemesher/` has no bake wrapper of any kind, so that
       named surface is unimplemented. The original note here ("the CLI has no
       bake command at all … and there is no bake report") was true when written
       and is now stale.
- [x] 4. Tests: encoding correctness on analytic shapes (sphere, saddle,
       filleted box), cage-following parity with the normal bake, cancel path
       — `tests/bake/test_curvature.cpp`, 10 cases: sphere reads 1/r and
       positive, plane reads 0, the `z = x^2 - y^2` saddle vanishes at the seat
       while the field stays alive around it (percentile matches the analytic
       0.354 peak), percentile ignores a pinched outlier, corrugated Target
       gives crest > 0.7 / trough < 0.3, cavity gives crest white / trough
       dark, flat Target is neutral for both, an explicit range overrides the
       auto fit, curvature loses and regains the Target on exactly the cage
       distances the normal bake does, and both variants honour cancellation.
       A filleted box was dropped in favour of the corrugation: it tests the
       same convex-vs-concave discrimination with an analytic curvature to
       check against.
       Plus a Python binding check that the widened `CyberBakeParams` crosses
       the ctypes boundary intact.
- [x] 5. Docs + CHANGELOG
       — CHANGELOG `## Unreleased` (with a note that 0.3.0-0.5.0 were tagged
       without entries), `examples/07_baking.py` bakes curvature + cavity
       alongside normal/displacement/AO.
