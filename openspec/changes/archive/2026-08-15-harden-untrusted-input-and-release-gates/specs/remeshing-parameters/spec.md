# remeshing-parameters (delta)

## MODIFIED Requirements

### Requirement: Canonical parameter set
The remesher SHALL expose exactly these user-facing parameters, defined once in a single source of truth consumed by GUI, CLI, and network entry points:

| Parameter | Type | Default | Valid range |
|---|---|---|---|
| targetQuadCount | int | 50 000 | 100 – 2 000 000 |
| edgeScale | float | 1.0 | 0.5 – 4.0 |
| sharpEdgeDegrees | float | 90.0 | 30.0 – 180.0 |
| smoothNormalDegrees | float | 0.0 | 0.0 – 180.0 |
| adaptivity | float | 1.0 | 0.0 – 1.0 |
| pureQuads | bool | false | — |
| holeFillMaxBoundary | int | 64 | 0 (never fill) – 10 000 |
| smallPatchPolicy | enum | keep-largest | keep-largest \| keep-all \| min-faces(N) |
| quadMethod | enum | quad-cover | quad-cover \| field-aligned \| instant-meshes \| integer |

`quadMethod` is the extractor selector rather than a solver parameter, so it lives on the entry-point structs (`CyberRemeshParams.quadMethod`, `RemeshParams.quad_method`) rather than in the shared `Parameters` struct; the default is `quad-cover`, degrading to `field-aligned` where no seamless-UV solver is present (see `remeshing-pipeline`).

#### Scenario: Defaults applied
- **WHEN** a remesh is invoked with no explicit parameters
- **THEN** the engine SHALL run with exactly the defaults above

### Requirement: No inert parameters
Every parameter accepted by any entry point SHALL affect pipeline behavior. Parameters with no implemented effect SHALL NOT be exposed. This SHALL hold **per entry point**: a parameter that reaches the pipeline from one caller (the CLI) and is dropped on the way from another (the C ABI, and therefore every binding over it) is inert for that caller, whatever the shared parameter struct says. The value a caller supplies SHALL reach every stage the parameter's documented semantics name, including stages inside an extractor that carries a default of its own.

#### Scenario: Exposed equals implemented
- **WHEN** the set of exposed parameters is compared with the set read by the pipeline
- **THEN** they SHALL be identical (AutoRemesher's ModelType was accepted but never read)

#### Scenario: The same value produces the same run from every entry point
- **WHEN** the same parameter set is run through the CLI and through the C ABI (and the Python and Swift bindings over it) on the same input
- **THEN** each entry point SHALL produce the same result, and no entry point SHALL substitute a component's own default for a value the caller supplied — as `cyber_remesh` did for `sharpEdgeDegrees`, which reached the CLI's pipeline but not the ABI's extractor, so the documented default of 90° could never take effect and the crease-pinning behaviour it gates was unreachable from every binding
