## MODIFIED Requirements

### Requirement: Packing
The system SHALL pack islands automatically with correct output scale, and support manual packing aided by snapping modes (adjustable grid, pixel centers, pixel corners, symmetry lines), texel-density and vertex-count readouts, and detection helpers for overlapping islands. Packing SHALL handle at least 1 000 islands / 100 000 UV vertices without failure.

**Packing and UDIM tiles.** Automatic packing targets the 0–1 UV square, which under the
`1001 + u + 10*v` numbering `surface-baking` bakes with is TILE 1001. The packer does not
allocate tiles: a multi-tile layout is authored by the user or arrives with an imported
mesh, and packing an already multi-tile layout would collapse it back into one tile and
silently destroy the arrangement. Baking reads whatever tiles the layout occupies, so a
layout that was packed here bakes as the single tile 1001 and an imported multi-tile layout
bakes as the set it occupies, with no packing step in between.

#### Scenario: Automatic pack
- **WHEN** automatic packing runs on unwrapped islands
- **THEN** islands SHALL be placed within the 0–1 UV square without overlaps at the requested margin, preserving relative texel density unless told otherwise

#### Scenario: A packed layout is the single tile 1001
- **WHEN** a mesh packed by the automatic packer is queried for its occupied UDIM tiles
- **THEN** exactly tile 1001 SHALL be reported
