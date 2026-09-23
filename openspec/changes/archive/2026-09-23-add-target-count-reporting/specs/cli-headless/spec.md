## ADDED Requirements

### Requirement: Count outcomes in the machine-readable report

The CLI JSON report SHALL include requested/effective/calibrated/final counts,
per-island outcomes, retained attempt, and termination reason.

#### Scenario: Host records a miss

- **WHEN** a bounded target-count request misses tolerance
- **THEN** the report SHALL identify the miss without treating the run as a
  cancellation or a pipeline error
