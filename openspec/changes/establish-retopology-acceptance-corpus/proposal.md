## Why

The existing generated benchmark is useful for detecting drift, but it does not
represent the production inputs or the topology failures that matter to an
automatic-retopology SDK. Its result records also do not pin the full solver,
input, parameter, and platform identity required to compare runs honestly.

## What Changes

- Add a versioned, offline acceptance-corpus manifest with deterministic
  fixtures spanning organic, CAD, open, multi-component, extreme-scale, and
  malformed-input cases.
- Measure and gate mesh validity separately from aesthetic metrics, including
  degenerate faces, non-manifold edges and vertices, boundary loops, and
  self-intersection candidates.
- Record reproducibility metadata for each run: corpus version and hashes,
  solver/ABI/build identity, parameters, seed, achieved counts, platform, and
  failures.
- Make benchmark checks fail for missing required corpus inputs, failed solver
  processes, or incomplete metrics; retain raw result records and failed output
  locations for review.
- Keep performance and device measurements as a documented release workflow;
  they are not treated as deterministic hosted-CI quality gates.

## Capabilities

### New Capabilities

- `retopology-acceptance`: Versioned corpus and reproducible quality/validity
  measurement contracts for automatic retopology.

### Modified Capabilities

- `remeshing-pipeline`: Require the shipped automatic-remeshing path to be
  evaluated by the acceptance gate on its documented offline fixtures.
- `build-and-packaging`: Require the quality gate to fail rather than skip when
  its required acceptance-corpus inputs or identities are unavailable.

## Impact

Changes `tools/bench`, checked-in test fixtures and baselines, CTest/CI gates,
and benchmark documentation. It does not alter the public remeshing API or
claim cross-toolchain bit identity.
