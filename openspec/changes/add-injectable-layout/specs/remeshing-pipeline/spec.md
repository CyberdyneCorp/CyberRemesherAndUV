# remeshing-pipeline — the layout reaches the output

## ADDED Requirements

### Requirement: Layout arcs are injectable on organic meshes

The quantized topology layout SHALL reach the output mesh on organic input, not
only on crease-pinned input. Injectability SHALL be MEASURED and reported per
run — the fraction of layout arcs whose length reduces onto the solver's integer
free variables — with the failure attributed by cause rather than counted as one
opaque total.

Injectability alone SHALL NOT be treated as success. It can be driven to 1.0
without changing any output mesh, which is the failure mode this track has
already hit twice, so it SHALL be paired with a measure of how much of the
Bi-MDF optimum the realized integers actually achieve.

#### Scenario: The injectability of a run is reportable

- **WHEN** a caller runs the ZRemesher path with a run report attached
- **THEN** the report SHALL carry the arc count, the non-injectable count split
  by cause, and the number of arcs actually injected

#### Scenario: A crease-pinned mesh stays fully injectable

- **WHEN** a mesh whose arcs run between pinned crease isolines is remeshed
- **THEN** every arc SHALL remain injectable, so the change cannot regress the
  case that already works

#### Scenario: Output reach is measured alongside injectability

- **WHEN** injectability improves on an organic mesh
- **THEN** the realized arc-deviation energy SHALL be reported against the
  Bi-MDF optimum, so an improvement that does not reach the mesh is visible as
  such rather than read as success
