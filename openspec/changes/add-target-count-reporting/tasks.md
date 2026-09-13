## 1. Model and pipeline

- [x] 1.1 Define count-policy and per-island/run outcome types, including
  requested/effective/calibrated/final counts and termination reason.
- [x] 1.2 Thread extractor attempt metadata through the pipeline while keeping
  default behavior byte-identical.
- [x] 1.3 Implement opt-in bounded retained-incumbent search and validity gate.

## 2. Host surfaces

- [x] 2.1 Add additive C ABI policy/report entry points and ABI manifest tests.
- [x] 2.2 Add Python and Swift report/policy bindings.
- [x] 2.3 Add CLI JSON report fields and documentation.

## 3. Verification

- [ ] 3.1 Add non-monotone, multi-island, pure-quad, tolerance-miss, and
  invalid-closer-candidate regressions.
- [x] 3.2 Run native, ABI, binding, CLI and strict OpenSpec gates.
