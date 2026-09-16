## MODIFIED Requirements

### Requirement: No inert parameters
Every parameter accepted by any entry point SHALL affect pipeline behavior.
Parameters with no implemented effect SHALL NOT be exposed. This SHALL hold
**per entry point**: a parameter that reaches the pipeline from one caller
(the CLI) and is dropped on the way from another (the C ABI, and therefore
every binding over it) is inert for that caller, whatever the shared parameter
struct says. The value a caller supplies SHALL reach every stage the
parameter's documented semantics name, including stages inside an extractor
that carries a default of its own.

#### Scenario: Exposed equals implemented
- **WHEN** the set of exposed parameters is compared with the set read by the
  pipeline
- **THEN** they SHALL be identical (AutoRemesher's ModelType was accepted but
  never read)

#### Scenario: The same value produces the same run from every entry point
- **WHEN** the same parameter set is run through the CLI and through the C ABI
  (and the Python and Swift bindings over it) on the same input
- **THEN** each entry point SHALL produce the same result, and no entry point
  SHALL substitute a component's own default for a value the caller supplied
  — as `cyber_remesh` did for `sharpEdgeDegrees`, which reached the CLI's
  pipeline but not the ABI's extractor, so the documented default of 90° could
  never take effect and the crease-pinning behaviour it gates was unreachable
  from every binding
- **AND** `adaptivity` SHALL reach the quad-cover and ZRemesher selector
  extractors with its validated caller value, including the canonical default
  of `1.0`; an explicit `0.0` SHALL retain uniform sizing
