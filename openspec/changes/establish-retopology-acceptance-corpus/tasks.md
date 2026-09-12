## 1. Corpus contract

- [ ] 1.1 Define a versioned acceptance manifest with required fixture classes,
  generator parameters, input expectations, and content hashes.
- [ ] 1.2 Add deterministic offline fixtures for organic, CAD, open-boundary,
  multi-component, extreme-scale, and malformed-input coverage.
- [ ] 1.3 Validate manifest schema, generated hashes, and malformed-input
  expectations before starting a solver.

## 2. Validity and reproducibility metrics

- [ ] 2.1 Add topology-validity metrics for invalid indices, degenerate faces,
  non-manifold edges/vertices, boundary loops, and triangle intersections.
- [ ] 2.2 Add adversarial metric tests with known valid and invalid meshes,
  including an output with attractive quad metrics but invalid topology.
- [ ] 2.3 Version metric output and attach corpus/input, command, parameter,
  seed, achieved-count, solver/build/ABI, platform, and artifact identity to
  every result row.

## 3. Acceptance harness and gates

- [ ] 3.1 Run and record every required fixture/solver pair, preserving timeout,
  subprocess, invalid-output, and missing-artifact failures.
- [ ] 3.2 Require identity-compatible baselines and enforce validity before
  quality drift comparisons.
- [ ] 3.3 Update CTest and the hardening quality lane so required evidence is a
  failure, not a skip; upload raw records and output artifacts for review.

## 4. Documentation and verification

- [ ] 4.1 Document deterministic acceptance versus controlled-device
  performance/RSS measurement commands and their different guarantees.
- [ ] 4.2 Record approved baselines for every required hosted toolchain and
  verify a clean acceptance run.
- [ ] 4.3 Run focused metric tests, the acceptance command, project test gates,
  and strict OpenSpec validation.
