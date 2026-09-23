## MODIFIED Requirements

### Requirement: Canonical parameter set

The canonical parameter set SHALL retain all existing defaults and add an
optional target-count policy only through an additive sibling configuration
surface. The policy SHALL state tolerance and maximum attempts, both validated
before the pipeline begins; omitting it SHALL preserve current behavior.

#### Scenario: Defaults applied

- **WHEN** a remesh is invoked with no explicit parameters
- **THEN** the engine SHALL run with exactly the existing documented defaults

#### Scenario: Omitted policy preserves defaults

- **WHEN** a caller uses the existing remesh entry point without a count policy
- **THEN** the output and two-attempt calibration behavior SHALL remain unchanged
