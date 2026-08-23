# surface-baking Specification

## Purpose
TBD - created by archiving change bootstrap-v1-platform. Update Purpose after archive.
## Requirements
### Requirement: Bakeable map types
The bake stage SHALL bake from the Target onto the EditMesh's UV layout: tangent-space normal maps, ambient occlusion, displacement/height, and color maps (from Target vertex colors, including polypaint, or from a Target texture when the Target has its own UVs — texture-to-texture baking). Output resolution SHALL be user-selectable up to at least 4096².

#### Scenario: Normal + color bake
- **WHEN** a bake runs on an EditMesh with valid UVs against a vertex-colored Target
- **THEN** a tangent-space normal map and a color map SHALL be produced at the requested resolution

#### Scenario: Texture-to-texture bake
- **WHEN** the Target carries UVs and a color texture
- **THEN** the bake SHALL sample the Target texture as the color source

### Requirement: Editable bake cage
The bake SHALL use a projection cage derived from the EditMesh, editable with the core actions: Tweak adjusts cage distance with brush falloff and double-tap sets per-vertex cage distance, Relax smooths the cage, Erase resets edited regions to default. Cage state SHALL persist in the document.

#### Scenario: Per-vertex cage distance
- **WHEN** the user double-taps a cage vertex and enters a distance
- **THEN** that vertex's cage offset SHALL change independently of the brush falloff

### Requirement: Component links and selective baking
When Target and EditMesh have multiple components, the user SHALL be able to draw explicit high→low component links so each EditMesh component bakes only from its linked Target components; drawing an X over a component SHALL bake that component alone. Unlinked components SHALL use nearest-surface matching by default.

#### Scenario: Linked components do not bleed
- **WHEN** two overlapping Target components are linked to distinct EditMesh components
- **THEN** each EditMesh component's maps SHALL contain only its linked source's detail

### Requirement: Bake correctness and preview
Because retopo, UVs, and bake share one scene, bakes SHALL be free of scale mismatches and tangent-basis inconsistencies by construction: the tangent basis used for baking SHALL be identical to the one exported with the mesh. The viewport SHALL preview bake results on the EditMesh with a repositionable preview light (Move action).

#### Scenario: Exported normal map renders correctly
- **WHEN** the exported mesh and normal map are loaded in a standard glTF viewer
- **THEN** shading SHALL match the in-app bake preview without seams or inverted channels

### Requirement: Accelerated, cancellable baking
Bake ray casting SHALL dispatch through the compute-acceleration layer (GPU when available, CPU fallback), report progress, and honor cooperative cancellation, leaving prior bake results untouched on cancel.

#### Scenario: Cancel a bake
- **WHEN** cancellation is requested mid-bake
- **THEN** the bake SHALL stop within 100 ms and previously baked maps SHALL remain as they were

### Requirement: Curvature and cavity maps
The bake stage SHALL bake a curvature map from the Target onto the EditMesh's
UV layout: signed surface curvature encoded around a midpoint gray, with
convex regions brighter and concave regions darker, normalized by a
user-controllable curvature range. A cavity variant SHALL also be available
that encodes concavity only (flat and convex regions map to white), suitable
for direct use as a multiply mask.

When the curvature range is left at 0 the bake SHALL derive it from the Target
as a percentile of |curvature| weighted by the surface area each sample speaks
for, so a region influences the range in proportion to the area it covers and
not to the number of vertices sitting on it.

Curvature baking SHALL follow the same rules as the other map types: the same
cage projection, component links, output resolution up to at least 4096²,
GPU dispatch with progress reporting and cancellation, and PNG/EXR output.

#### Scenario: Curvature bake distinguishes edges from crevices
- **WHEN** a curvature bake runs against a Target with both sharp convex edges and deep concave seams
- **THEN** the convex edges SHALL read brighter than the midpoint and the concave seams darker, at the requested resolution

#### Scenario: Cavity variant masks concavity only
- **WHEN** a cavity bake runs on the same Target
- **THEN** concave seams SHALL read dark while flat and convex regions read white

#### Scenario: Auto range is not captured by a dense sliver fan
- **WHEN** an auto-ranged curvature bake runs against a Target whose parameterization piles a large share of its vertices onto a vanishing share of its area, such as the sliver fans at a UV sphere's poles
- **THEN** the range SHALL be set by the curvature of the bulk of the surface, leaving the interior detail legible rather than compressed toward the midpoint

#### Scenario: Curvature respects the cage
- **WHEN** the projection cage is edited and the curvature bake re-runs
- **THEN** the sampled regions SHALL follow the edited cage exactly as a normal-map bake would

