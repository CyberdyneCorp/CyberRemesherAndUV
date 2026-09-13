## 1. Eligibility and scoring

- [x] 1.1 Record finite-coordinate, zero-length-edge, and empty-candidate
      validity in the internal quality score.
- [x] 1.2 Replace the median-driven angle term with a documented p95 absolute
      corner-angle deviation while retaining median diagnostics.
- [x] 1.3 Reject geometry-ineligible candidates before score comparison and
      include eligibility/tail diagnostics in selection debug output.

## 2. Regression coverage

- [x] 2.1 Add constructed invalid-geometry and degenerate-edge tests proving
      they cannot beat an eligible candidate.
- [x] 2.2 Add an extreme-corner regression proving a good median cannot hide a
      poor tail-angle score, plus deterministic tie coverage.

## 3. Verification

- [x] 3.1 Run focused quadrangulation tests and the CPU-headless test suite.
- [x] 3.2 Validate this change and all OpenSpec artifacts strictly.
