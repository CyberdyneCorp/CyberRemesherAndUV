## ADDED Requirements

### Requirement: Acceptance-corpus evaluation of the shipped pipeline

The shipped automatic-remeshing configuration SHALL be evaluated against the
offline acceptance corpus with declared geometry-validity and quality metrics.
The result SHALL distinguish a metric estimate from an exact geometric proof,
and a failed or invalid fixture SHALL not be silently omitted.

#### Scenario: Release-path corpus run

- **WHEN** the release-path acceptance command runs the configured automatic
  remesher on the required offline fixtures
- **THEN** it SHALL report every fixture's achieved count, validity outcome,
  quality metrics, and failure state in a reviewable record
