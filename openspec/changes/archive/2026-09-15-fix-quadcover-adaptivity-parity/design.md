## Context

See proposal.md. `remeshShared()` already converts and validates the C ABI
parameters. Its factory captures the hole-fill and feature values but hardcodes
adaptivity for the quad-cover and selector-based ZRemesher paths. The dedicated
`cyber_remesh_zremesher()` entry point already forwards the value.

## Goals / Non-Goals

**Goals:**

- Make C ABI extractor construction use the same validated adaptivity as the
  CLI and dedicated ZRemesher entry point.
- Protect the forwarding behavior with output-sensitive C ABI regressions.

**Non-Goals:**

- Recalibrate adaptive sizing, change count acceptance, or claim that a
  particular adaptivity value is globally higher quality.
- Change C ABI layouts or solve the broader cross-language parity audit.

## Decisions

- Capture `cppParams.adaptivity` beside the existing extractor-specific
  parameters. This is the validated value and avoids a second validation path.
- Forward it to `makeQuadCoverQuadrangulator()` and
  `ZRemesherOptions::adaptivity`. This matches the CLI and dedicated ZRemesher
  route; retaining the hardcoded uniform value would preserve a known inert
  parameter.
- Test an explicit `0.0` and `1.0` on an input whose native quad-cover output
  differs, while also asserting that repeated same-value runs match. A test
  comparing two no-op values would not detect dropped forwarding.
- Make the open-surface cleanup's uniformity regression set `adaptivity=0.0`
  explicitly. Its edge-length-CV assertion tests a uniform grid; adaptive
  sizing intentionally changes that distribution and must not be smuggled in
  through the former C ABI override.
- Keep the regression C ABI-focused. A CLI comparison is useful broader
  coverage but can be affected by file/report formatting and would not isolate
  this factory boundary.

## Risks / Trade-offs

- [Default output changes for C ABI callers] → Match the canonical documented
  default and record the migration in the changelog.
- [A chosen fixture may not be sensitive on a different solver route] → Use a
  torus with varying curvature and assert same-value determinism before
  comparing adaptive and uniform output.
- [Output changes could be blamed on count calibration] → Retain PR #45's
  regression and use the same target and all non-adaptivity parameters in both
  test cases.

## Migration Plan

Release as a behavior fix without an ABI bump. Consumers requiring the prior
uniform behavior set adaptivity to `0.0`; consumers relying on the canonical
default receive the same setting as the CLI. Revert by restoring the two
factory overrides if an emergency rollback is required.
