# pipeline-bridge Specification

## Purpose
Where this engine meets the half of the pipeline it does not own. It ingests a
sculpt through a versioned interchange format, bakes against an abstract field
evaluator, and conforms an existing EditMesh onto an updated Target. It exists
to keep that boundary a *format and an interface* rather than a dependency:
there is no build or link dependency on any sculpting or volumetric engine, and
a handoff declaring a version this engine does not support is refused by name
rather than read with the unknown parts dropped.
## Requirements
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

### Requirement: The bake provider is a seam, not a dependency

The provider surface through which an external consumer requests baked maps SHALL be an
INTERFACE and a set of result records, exactly as the field evaluator is. A consumer SHALL
drive it with nothing but this repository's C ABI: no build dependency, no link dependency
and no source dependency SHALL exist in either direction between this engine and any
consumer of its maps. This repository SHALL carry a headless example that drives the whole
provider surface — enumerating the maps, requesting them, reading the metadata, observing
progress and cancelling a bake — so the seam is exercised here without any consumer
present.

The provider SHALL accept the same field evaluator the bridge already defines. When an
evaluator is supplied in place of a Target mesh, the producible set SHALL be exactly the
maps a field can answer, and a request for any other map SHALL fail naming that map and
listing the producible set — the same refusal a map outside the advertised set gets,
because to that consumer it is outside the advertised set. Which maps a field alone can
produce SHALL be readable from the capability query before a request is made, not only
from a refusal afterwards.

#### Scenario: A consumer drives the provider over the C ABI alone

- **WHEN** a consumer compiled against this repository's C header, and linked against
  nothing else of this repository, enumerates the producible maps and requests one
- **THEN** it SHALL receive the pixels and the result metadata, and this repository SHALL
  require no artifact of that consumer to build, test or run

#### Scenario: The example exercises the surface headlessly

- **WHEN** this repository's bake-provider example runs with no display and no consumer
  present
- **THEN** it SHALL enumerate the advertised maps, request maps of more than one encoding
  basis, print the metadata each one reported, show a refusal naming an unproducible map,
  and show a cancelled request returning no pixels

#### Scenario: A field evaluator narrows the producible set, and says so in advance

- **WHEN** a consumer supplies a field evaluator instead of a Target mesh
- **THEN** the capability query SHALL already mark which maps a field alone can produce,
  a request for one of those SHALL succeed, and a request for any other SHALL fail naming
  that map and listing the maps a field can produce

