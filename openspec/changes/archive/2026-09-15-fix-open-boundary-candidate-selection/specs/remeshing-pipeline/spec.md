## MODIFIED Requirements

### Requirement: Quality-driven candidate selection

The pipeline SHALL support a quality mode that solves more than one candidate
and selects between them by a single published score covering geometry, quad
shape, topology, flow, guide adherence, features and symmetry. Selection SHALL
be deterministic, with a stable tie-break, and the selected candidate SHALL be
named in the run report. A candidate with non-manifold edges or a boundary
component count different from the input's SHALL be ineligible before aesthetic
terms are compared; the number of edges used to tessellate a preserved boundary
SHALL NOT itself make an otherwise eligible candidate lose.

#### Scenario: Best mode never picks a worse candidate

- **WHEN** the quality mode solves two candidates
- **THEN** the selected candidate's score SHALL be greater than or equal to
  every other eligible candidate's score, and the report SHALL name it

#### Scenario: Open boundary is preserved

- **WHEN** two candidates for an open input preserve its boundary-component
  count but use different numbers of boundary edges
- **THEN** selection SHALL rank them by their quality score rather than raw
  boundary-edge count

#### Scenario: New boundary component is rejected

- **WHEN** a candidate for an open input introduces an additional boundary
  component
- **THEN** it SHALL lose to an otherwise eligible candidate regardless of its
  aesthetic score

#### Scenario: Selection is stable

- **WHEN** two candidates score equal within tolerance
- **THEN** the same candidate SHALL be selected on every run of the same input
