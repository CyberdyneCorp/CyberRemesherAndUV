# surface-baking — High-to-Low Projection Baking

## ADDED Requirements

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
