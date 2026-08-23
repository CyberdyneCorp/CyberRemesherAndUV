# pipeline-bridge Specification

Delta for `add-claycore-bridge` (new capability).

## ADDED Requirements

### Requirement: Sculpt handoff ingest
The engine SHALL accept a versioned sculpt handoff — a triangle mesh with
positions, normals, vertex colors, and a material-mix attribute — as a Target
for remeshing, UV, and baking, through two routes: a file handoff (documented
PLY/GLB profile) and an in-memory buffer handoff for on-device composition.
An incompatible handoff version SHALL be rejected with a typed error naming
the found and supported versions.

#### Scenario: File handoff becomes a Target
- **WHEN** a valid handoff file is opened as a Target
- **THEN** remeshing, unwrapping and baking SHALL operate on it exactly as on a natively imported mesh, with vertex colors available as a bake source

#### Scenario: Version mismatch is loud
- **WHEN** a handoff declares an unsupported version
- **THEN** ingest SHALL fail with a typed error naming both versions and no partial Target SHALL be created

#### Scenario: Geometry loss is loud
- **WHEN** a handoff describes triangles the mesh cannot accept (a repeated vertex index) on any route
- **THEN** ingest SHALL keep the rest of the surface and SHALL report the number of dropped triangles — a count on the result and a warning — rather than report success with a silently smaller Target

#### Scenario: glTF handoff matches the documented profile
- **WHEN** a `.gltf`/`.glb` handoff declares `asset.extras.cyberSculptHandoff`
- **THEN** the version gate, the producer label, geometry, vertex colors and per-vertex normals SHALL all be read as documented, and any documented payload the container cannot carry SHALL be named in the result's warnings

### Requirement: Field-sampled baking through an evaluator interface
Baking SHALL accept an optional field evaluator — an abstract interface
providing signed distance, gradient, and ambient-occlusion queries at world
points — and, when present, SHALL sample normal, AO, and curvature data from
the evaluator instead of raycasting the Target mesh. Without an evaluator,
baking SHALL fall back to the existing raycast path with unchanged output
contracts. The engine SHALL NOT link against any specific volumetric engine;
the evaluator is the only coupling point.

#### Scenario: Evaluator-backed normal bake
- **WHEN** a normal bake runs with a field evaluator attached
- **THEN** sampled normals SHALL come from the evaluator's gradient queries and the output map SHALL satisfy the same correctness scenario as the raycast path

#### Scenario: No evaluator, no behavior change
- **WHEN** a bake runs without an evaluator
- **THEN** outputs SHALL be identical to the pre-bridge raycast implementation

### Requirement: Conform to an updated Target
The engine SHALL re-snap an existing EditMesh to a replaced Target surface
while preserving the EditMesh's topology exactly, and SHALL report the
maximum and RMS vertex deviation of the conform. Conform SHALL NOT silently
stretch: when any vertex's deviation exceeds a caller-set threshold, the
result SHALL flag those vertices for review rather than discard the operation.

#### Scenario: Sculpt changed after retopo
- **WHEN** a Target is replaced by a newer handoff of the same sculpt and conform runs
- **THEN** every EditMesh vertex SHALL lie on the new surface, connectivity SHALL be unchanged, and the report SHALL carry max and RMS deviation

#### Scenario: Large deviation is flagged
- **WHEN** the new Target diverges beyond the threshold under part of the EditMesh
- **THEN** the affected vertices SHALL be flagged in the result and the operation SHALL still complete
