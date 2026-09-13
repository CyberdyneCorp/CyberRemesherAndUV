# Tasks

- [x] Add `ResourceLimits` and pre-copy/final-output enforcement to the C++ pipeline.
- [x] Bound adaptive isotropic edge splitting by caller intermediate face and vertex limits.
- [x] Add an additive C ABI limits POD and plain remesh entry point, including diagnostics.
- [x] Expose the limits through Python and Swift without changing existing calls.
- [x] Add below/above-limit C++, C ABI, Python and Swift regression coverage.
- [x] Validate OpenSpec and project test gates.
- [ ] Add exact sparse-factor and retained-candidate storage ceilings to the
      native QuadCover path, with typed pipeline diagnostics.
- [ ] Extend the additive bindings limits API and cover below/above-limit
      native-solver and candidate-selection regressions.
