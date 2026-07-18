# remeshing-parameters — Parameter Semantics

## ADDED Requirements

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

#### Scenario: Defaults applied
- **WHEN** a remesh is invoked with no explicit parameters
- **THEN** the engine SHALL run with exactly the defaults above

### Requirement: Validation at every entry point
Every entry point SHALL validate parameters against the canonical ranges before the pipeline starts: out-of-range numeric values SHALL be clamped with a user-visible warning naming the parameter, original value, and clamped value; non-numeric or type-mismatched values SHALL be rejected with a typed error. No entry point SHALL forward unvalidated values to the engine.

#### Scenario: Out-of-range CLI value clamps with warning
- **WHEN** the CLI receives `--edge-scale 10.0`
- **THEN** the run SHALL proceed with 4.0 and a warning SHALL be printed naming the clamp (AutoRemesher passed 10.0 through silently)

#### Scenario: Non-numeric value rejected
- **WHEN** the CLI receives `--target-quads abc`
- **THEN** the invocation SHALL fail with a parameter error and a nonzero exit code (AutoRemesher silently converted this to 0)

### Requirement: Semantics of each parameter
targetQuadCount SHALL drive the derived target edge length (area-based, guarded against zero); edgeScale SHALL scale parameterization density; sharpEdgeDegrees SHALL define the feature-edge dihedral threshold used by both the isotropic stage and parameterization hard constraints; smoothNormalDegrees > 0 SHALL enable smooth-surface projection; adaptivity SHALL modulate local edge length by curvature (0 = uniform), with the same scaling feeding isotropic targets and parameterization density.

#### Scenario: Adaptivity zero is uniform
- **WHEN** adaptivity is 0.0
- **THEN** all vertices SHALL use the uniform base target edge length and the parameterization SHALL receive uniform scaling

### Requirement: No inert parameters
Every parameter accepted by any entry point SHALL affect pipeline behavior. Parameters with no implemented effect SHALL NOT be exposed.

#### Scenario: Exposed equals implemented
- **WHEN** the set of exposed parameters is compared with the set read by the pipeline
- **THEN** they SHALL be identical (AutoRemesher's ModelType was accepted but never read)
