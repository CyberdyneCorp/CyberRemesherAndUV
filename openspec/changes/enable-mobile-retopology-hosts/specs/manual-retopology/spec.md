## MODIFIED Requirements

### Requirement: Advanced build tools
The system SHALL provide these tools (available via a customizable tool gallery): **BuildQuad** / **BuildTri** (drag from boundary elements to extrude single faces with automatic vertex merging), **DrawStrip** (paint a quad strip following the stroke, matching source quad size), **Contours** (sample the Target with cross-section strokes and loft the resulting rings into a quad tube), **ExtendBoundary** (select boundary edges by stroke or press-and-hold, then extrude quad strips by camera movement — single step, repeated, or continuous — plus grid fill and triangle-fan fill of boundary regions with controllable orientation), **PatchClone** (stroke-select a face patch, then stamp copies elsewhere, with flip), **TransformVertices** (lasso-select vertices, then move/rotate/scale them by camera movement, reporting per-vertex snap success), **PathDistribute** (redistribute vertices along the path between stroke endpoints), **SurfaceCut** (knife-cut new edges across faces), and **LoopInfo** (inspect vertex/edge count, boundary length, and snap state of the loop under the cursor).

Contours SHALL derive a cutting plane from each stroke, take the Target cross-section nearest that stroke, resample every ring to one shared span count, and loft consecutive rings into quads. Rings SHALL be lofted in the order supplied. The seam SHALL NOT spiral and orientation SHALL be consistent across every band of one tube. A stroke that names no usable plane or no Target cross-section SHALL be reported by index and SHALL NOT produce geometry.

#### Scenario: Camera-driven boundary extrusion
- **WHEN** boundary edges are selected via ExtendBoundary and the user orbits the camera
- **THEN** quad strips SHALL extrude from the selected boundary following the camera motion

#### Scenario: Grid fill
- **WHEN** a closed boundary region is grid-filled
- **THEN** the region SHALL fill with a regular quad grid whose orientation is adjustable via anchor handles

#### Scenario: Contour strokes build a tube
- **WHEN** three cross-section strokes are drawn down a cylindrical Target region with a span count of 12
- **THEN** three rings of 12 vertices SHALL be produced and lofted into 24 quads, every vertex on the Target surface

#### Scenario: Contour seam does not spiral
- **WHEN** consecutive contour rings are lofted
- **THEN** each ring's resampling SHALL start at the sample nearest the previous ring's start, and adjacent rings whose traversal directions oppose SHALL be reversed before lofting, so every quad of the tube carries consistent orientation

#### Scenario: Unusable contour stroke is named
- **WHEN** a contour stroke degenerates to a point, or its plane misses the Target
- **THEN** the operation SHALL report that stroke's index and SHALL NOT add faces for it
