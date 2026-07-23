# Changelog

## 0.1.1

Maintenance release. No engine behaviour change on the default path — the
headline retopology and UV features all shipped in 0.1.0. This release corrects
what the project *claims* about them, fixes the benchmark that produced some of
those claims, and adds an opt-in experimental path.

### Fixed

- **Benchmark measured the wrong extractor.** `examples/11_benchmark.py` scored
  the retired `instant-meshes` position-field extractor in its comparison
  phases rather than the shipped `quad-cover` default. Every conclusion drawn
  from those phases was about code that is no longer the default.
- **Benchmark could hang indefinitely.** The count-match search applied an
  unbounded multiplicative correction with a 40x ceiling. On models whose quad
  count saturates it escalated to a ~120k-quad request, allocated ~4 GB and
  never returned. Now bounded by a 4x ceiling with a saturation guard;
  regression-tested in `examples/test_count_match.py`.

### Added

- `CYBER_QC_OPEN_CLEANUP` (experimental, opt-in): runs the isoline graph
  cleanup on open surfaces, with the input rim preserved. Partial — the
  `simplifyGraph` turn-angle guard is not implemented, so it is off by default
  and the default path is byte-unchanged.
- `version_identity` test: the four Python version declarations were unlinked
  from `CMakeLists.txt`, so a release bump could miss one and ship a wheel
  disagreeing with the engine inside it.
- `hole_fill_policy` test: pins a pre-existing gap where the default
  `quad-cover` extractor ignores `holeFillMaxBoundary` (see Known issues).

### Changed — claims corrected against measurement

- The quad-cover default beats QuadriFlow on median angle **and**
  irregular-vertex count on **3 of 5** corpus models (spot, rocker-arm,
  stanford-bunny), not "across the organic corpus". The earlier framing counted
  mechanical rocker-arm as organic and omitted cheburashka, which loses both
  axes. Corrected in the spec, README, Python API docs and roadmap.
- Topological validity is now stated normatively: the default is at least as
  valid as QuadriFlow on all 5 models, and stays manifold on flat CAD input
  where QuadriFlow tears (cube: 0 defects vs 576 boundary edges at ~2100 quads).
- Feature-following is recorded as a known gap (fandisk 2.0x, cheburashka 2.0x,
  rocker-arm 1.5x) with its root cause — un-pinned integer-grid phase.
- README's stanford-bunny comparison quoted QuadriFlow at 80 deg / 6%
  irregular; measured is 82 deg / 4%.
- `15_uv_vs_xatlas` claimed xatlas packs "~7 pts" tighter; measured is +10 on
  average, +18 at worst. Now derived from the measured rows.

### Known issues

- **`holeFillMaxBoundary` has no effect on the default `quad-cover` method.**
  It is applied as a post-pass over the quadrangulator's output, and quad-cover
  closes holes during extraction, so a loop longer than the limit is not left
  open as the spec requires. `field-aligned` honours the parameter;
  `instant-meshes` never fills. Tracked with the open-surface work.
- Open surfaces remain a limitation on the default path: rims are closed rather
  than preserved.

## 0.1.0

First tagged release: quad remeshing, retopology, UV editing and baking in a
C++20 engine, with the `quad-cover` default quadrangulator, the one-call
automatic UV atlas, and the Python/C ABI bindings.
