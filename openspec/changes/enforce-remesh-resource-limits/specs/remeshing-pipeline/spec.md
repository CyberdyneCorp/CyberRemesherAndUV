## ADDED Requirements

### Requirement: Caller-configured topology ceilings

The remeshing pipeline SHALL accept optional independent ceilings for input,
intermediate and output vertex and face counts. A zero ceiling SHALL disable
only that dimension. The pipeline SHALL reject a limit before the operation
that would exceed it, name the stage, requested count and allowed count, and
leave the caller input unchanged.

#### Scenario: Input is over its budget

- **GIVEN** an input mesh has more faces than `maxInputFaces`
- **WHEN** remeshing starts
- **THEN** it SHALL fail before making the pipeline work copy

#### Scenario: Adaptive refinement reaches an intermediate ceiling

- **GIVEN** a coarse mesh requires splits to reach its target edge length
- **AND** `maxIntermediateFaces` is lower than the next split result
- **WHEN** the isotropic stage runs
- **THEN** it SHALL stop before creating that result and report the limit

#### Scenario: Limits are disabled

- **GIVEN** every limit is zero
- **WHEN** remeshing runs
- **THEN** it SHALL use the existing unbounded behaviour
