## MODIFIED Requirements

### Requirement: Quality-driven candidate selection

The pipeline SHALL support a quality mode that solves more than one candidate
and selects between them by a single published score covering geometry, quad
shape, topology, flow, guide adherence, features and symmetry. Selection SHALL
be deterministic, with a stable tie-break, and the selected candidate SHALL be
named in the run report. A candidate with non-manifold edges, a boundary
component count different from the input's, non-finite live-vertex positions,
zero-length live edges, or no faces SHALL be ineligible before aesthetic terms
are compared. Quad-corner shape SHALL use a documented upper-tail absolute
deviation from 90 degrees; a favorable median angle alone SHALL NOT allow a
candidate with severely distorted corners to win.

#### Scenario: Invalid candidate is rejected

- **WHEN** one quality-mode candidate has a non-finite coordinate, zero-length
  edge, or no faces and another is geometry-eligible
- **THEN** the invalid candidate SHALL lose regardless of its aesthetic score

#### Scenario: Best mode never picks a worse candidate

- **WHEN** the quality mode solves two candidates
- **THEN** the selected candidate's score SHALL be greater than or equal to
  every other eligible candidate's score, and the report SHALL name it

#### Scenario: Tail distortion affects selection

- **WHEN** two geometry-eligible candidates have similarly favorable median
  angles but one has a worse upper tail of corner-angle deviation
- **THEN** the candidate with the better tail statistic SHALL receive the
  higher angle-quality term

#### Scenario: Selection is stable

- **WHEN** two eligible candidates score equal within tolerance
- **THEN** the same candidate SHALL be selected on every run of the same input
