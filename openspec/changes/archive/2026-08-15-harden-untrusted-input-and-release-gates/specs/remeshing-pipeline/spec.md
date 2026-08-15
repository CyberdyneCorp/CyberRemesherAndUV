# remeshing-pipeline (delta)

## MODIFIED Requirements

### Requirement: Feature-preserving isotropic stage
The isotropic stage SHALL split long edges, collapse short edges, flip toward valence 6, tangentially smooth, and re-project to the source surface, keeping edge lengths within a documented band around the (adaptivity-scaled) target. Feature-edge vertices SHALL NOT be collapsed, projected off the feature, or crossed by edge flips; when smooth-normal degrees > 0 projection SHALL target a smoothed (PN-triangle or equivalent) surface.

The stage SHALL terminate on any finite input, whatever its coordinates. A refinement operation that cannot make progress SHALL NOT be re-attempted: an edge already at the resolution the float grid can express at its own coordinate magnitude, or a split whose midpoint rounds onto an endpoint, SHALL be left alone. The stage SHALL additionally run under an element budget derived from the input's surface area and the finest target the scale field allows, so residual non-convergence produces a bounded, under-refined mesh rather than an allocation failure. These guards SHALL NOT alter output for inputs at ordinary coordinates.

#### Scenario: Edge flips respect features
- **WHEN** the isotropic stage runs on a mesh with tagged sharp edges
- **THEN** no edge flip SHALL create an edge crossing a feature edge (AutoRemesher's flip guard was commented out)

#### Scenario: Geometry far from the origin terminates
- **WHEN** a mesh whose coordinates are large relative to its features is remeshed — for example a radius-1 sphere at world (5e5, 5e5, 5e5), which arises from centimetre units, site coordinates or a baked scene transform — at a target edge length the float grid cannot resolve there
- **THEN** the stage SHALL terminate with a bounded element count and the run SHALL complete, rather than growing until allocation fails and taking the host process with it

#### Scenario: The guards are inert at ordinary coordinates
- **WHEN** the corpus is remeshed at ordinary coordinates across densities on every quadrangulator path
- **THEN** the output SHALL be byte-identical to the output produced without the termination guards
