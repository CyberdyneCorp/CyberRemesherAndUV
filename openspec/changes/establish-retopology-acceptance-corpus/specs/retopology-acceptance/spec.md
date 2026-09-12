## Purpose

Defines the reproducible corpus, validity evidence, and quality records used to
decide whether an automatic-retopology build is acceptable for release.

## ADDED Requirements

### Requirement: Versioned offline acceptance corpus

The project SHALL provide a checked-in, versioned acceptance-corpus manifest
whose required fixtures run without network access. Each fixture SHALL state
its category, deterministic input identity, target request, and expected input
outcome. The required set SHALL cover organic, CAD/feature, open-boundary,
multiple-component, extreme-scale, and malformed-input cases.

#### Scenario: Required fixture is absent or changed

- **WHEN** a required fixture cannot be generated, its content hash differs, or
  its declared input outcome is not observed
- **THEN** the acceptance command SHALL fail and identify the fixture and
  violated expectation

### Requirement: Topology validity precedes quality acceptance

The acceptance result SHALL report face degeneracy, non-manifold edges and
vertices, boundary-loop information, and self-intersection evidence separately
from aesthetic metrics. A result with a required validity violation SHALL fail
acceptance even when its count, distance, or quad-shape metrics improve.

#### Scenario: Invalid output has favorable shape metrics

- **WHEN** a remesher writes output with an invalid topology condition
- **THEN** the result record SHALL identify that condition and the acceptance
  command SHALL fail without accepting the quality metrics as a pass

### Requirement: Reproducible result identity

Every benchmark result SHALL identify the corpus version and input hash,
parameters and seed, achieved output count, solver/build/ABI identity when
available, platform/toolchain, metric version, and an output or failure-artifact
location. Baseline comparison SHALL reject incompatible identities rather than
reporting them as numerical drift.

#### Scenario: Solver or corpus changes after baseline recording

- **WHEN** a check is requested with a solver or corpus identity that differs
  from the approved baseline
- **THEN** the command SHALL refuse the comparison and state which identity
  differs

### Requirement: Failed work remains reviewable

The harness SHALL retain an inspectable result record for every attempted
fixture/solver pair, including subprocess failures, timeouts, and invalid
outputs. It SHALL not omit failed rows from the summary or convert them into a
successful aggregate.

#### Scenario: External or local solver exits without an output

- **WHEN** a solver process exits unsuccessfully or fails to produce its output
- **THEN** the harness SHALL return nonzero and record the command, failure
  reason, and intended output artifact path
