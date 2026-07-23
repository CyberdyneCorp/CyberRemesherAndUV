# Tasks: fix-hole-fill-double-close

- [x] Fix `IsolineExtractor::fixHoleWithQuads` so the terminal 3-/4-gon branches consume
      `hole`, and apply the same half-edge-already-taken guard the >4 path uses.
- [x] Measure the corpus effect on the shipped default: 5 models x 5 densities
      (2600/3000/3400/3800/4200), non-manifold edges before -> after.
- [x] Verify the other four models are unaffected, hashing positions and faces
      separately so a topology-preserving numeric shift is not reported as identity.
- [x] Add a regression test on a fixture that actually reproduces the double fill
      (torus knot), and verify it fails against the pre-fix code.
- [x] Match the benchmark's Phase 1/3 comparisons on achieved quad count rather than
      on the request, and register `examples/test_count_match.py` with ctest.
- [x] Update `docs/ROADMAP.md` Phase 3 and `CHANGELOG.md`.
- [x] Update the `remeshing-pipeline` spec: hole-fill contract, validity guarantee,
      feature-following gap and its cause.
- [x] `openspec validate --strict` and the full `ctest` suite.
