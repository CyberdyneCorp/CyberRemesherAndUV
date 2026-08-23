# remeshing-pipeline — flow guides and painted density

Delta for `add-flow-guides`.

## ADDED Requirements

### Requirement: Flow guides constrain the cross field
The pipeline SHALL accept zero or more flow guides — ordered polylines on or
near the Target surface — and SHALL bias the cross field near each guide
toward the guide's tangent direction, as a soft constraint with per-guide
strength and influence radius. With no guides supplied the pipeline SHALL
behave byte-identically to today.

#### Scenario: Loops follow a guide
- **WHEN** a remesh runs with a guide drawn across a limb junction
- **THEN** extracted edge loops within the guide's influence radius SHALL follow the guide's direction within the documented angular tolerance

#### Scenario: No guides, no change
- **WHEN** a remesh runs with an empty guide set
- **THEN** the output SHALL be byte-identical to the same run without the guide inputs

### Requirement: Painted density scales local sizing
The pipeline SHALL accept an optional per-vertex or per-face scalar density
field on the Target and SHALL scale local target edge sizing by it, clamped to
a documented range. A density of 1.0 everywhere SHALL reproduce today's
uniform sizing byte-identically.

#### Scenario: Painted region densifies
- **WHEN** a remesh runs with density 4.0 painted on a region and 1.0 elsewhere
- **THEN** quads inside the painted region SHALL be smaller than outside by approximately the documented sizing relation, and total quad count SHALL respect the requested target within its guarantee

#### Scenario: A density of 1.0 everywhere is a no-op
- **WHEN** a remesh runs with a density of 1.0 at every vertex (or every face)
- **THEN** validation SHALL drop the neutral density and report it as a non-fatal issue, and the output SHALL be byte-identical to the same run with no density supplied — on every backend, including the default quad-cover, whose route selection would otherwise change merely because guidance is present

### Requirement: Guidance is honored loudly or rejected loudly
The pipeline SHALL NOT silently ignore supplied guidance: when a backend or
island path cannot honor guides or density, the per-island report SHALL name
which inputs were not honored and why.

#### Scenario: Unsupported path reports, not drops
- **WHEN** an island routes to a backend that does not implement guide constraints
- **THEN** the island's report entry SHALL name the ignored guides and the reason
