## 1. Model and pipeline

- [ ] 1.1 Define count-policy and per-island/run outcome types, including
  requested/effective/calibrated/final counts and termination reason.
- [ ] 1.2 Thread extractor attempt metadata through the pipeline while keeping
  default behavior byte-identical.
- [ ] 1.3 Implement opt-in bounded retained-incumbent search and validity gate.

## 2. Host surfaces

- [ ] 2.1 Add additive C ABI policy/report entry points and ABI manifest tests.
- [ ] 2.2 Add Python and Swift report/policy bindings.
- [ ] 2.3 Add CLI JSON report fields and documentation.

## 3. Verification

- [ ] 3.1 Add non-monotone, multi-island, pure-quad, tolerance-miss, and
  invalid-closer-candidate regressions.
- [ ] 3.2 Run native, ABI, binding, CLI and strict OpenSpec gates.
