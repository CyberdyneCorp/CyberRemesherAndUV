# engine-bindings — subdivision mode selector

Delta for `add-smooth-subdivision`.

## MODIFIED Requirements

### Requirement: Subdivision binding
The Python binding SHALL expose subdivision over the C ABI as `Mesh.subdivide`,
splitting every n-gon into n quads (Catmull-Clark topology) in place and
returning the resulting face count. It SHALL accept a mode selecting LINEAR
(Catmull-Clark topology, no smoothing) or CATMULL-CLARK (the smooth rules), with
LINEAR as the default so that existing calls keep their exact behaviour. It
SHALL accept an optional projection target; when given, every vertex of the
subdivided mesh SHALL be projected onto that target's surface, which is what
recovers curvature that linear subdivision alone cannot add — and which remains
available in the smooth mode, where it is the more accurate answer whenever a
Target exists.

The C ABI SHALL carry the mode on a SIBLING entry point rather than by changing
the signature of the published mode-less one, so that already-compiled callers
keep working; the mode-less entry point SHALL be defined as the linear case of
the mode-taking one rather than a second implementation. An unrecognised mode
SHALL be rejected with the typed invalid-argument error, leaving the mesh
untouched.

#### Scenario: Subdividing quadruples a quad mesh
- **WHEN** `Mesh.subdivide()` is called on a mesh of N quads
- **THEN** the mesh SHALL afterwards hold 4N quads and the reported face count SHALL equal the mesh's face count

#### Scenario: Subdivide and reproject recovers curvature
- **WHEN** a coarse mesh is subdivided with a curved surface passed as the projection target
- **THEN** the new vertices SHALL lie on that surface rather than on the coarse mesh's flat facets

#### Scenario: Smooth subdivision needs no projection target
- **WHEN** `Mesh.subdivide()` is called in Catmull-Clark mode on a faceted cage with no projection target
- **THEN** the result SHALL be measurably rounder than the same mesh subdivided linearly, and calling it without naming a mode SHALL still produce the faceted linear result

#### Scenario: An unknown mode is refused
- **WHEN** `Mesh.subdivide()` is called with a mode value the engine does not define
- **THEN** it SHALL raise the typed invalid-argument error and the mesh SHALL be left exactly as it was

#### Scenario: Subdividing an empty mesh fails loudly
- **WHEN** `Mesh.subdivide()` is called on a mesh with no faces
- **THEN** it SHALL raise the typed empty-mesh error rather than silently doing nothing
