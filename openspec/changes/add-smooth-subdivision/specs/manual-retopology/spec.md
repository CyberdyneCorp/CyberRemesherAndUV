# manual-retopology — smooth subdivision mode

Delta for `add-smooth-subdivision`.

## ADDED Requirements

### Requirement: Subdivision modes
The whole-mesh subdivide command SHALL offer two modes over ONE topology pass
(every n-gon becomes n quads around its centroid, in both): a LINEAR mode that
leaves every new vertex on the source facets, and a CATMULL-CLARK mode that
applies the smooth subdivision rules — face points at the face centroid, edge
points at the average of the edge's endpoints and its two adjacent face points,
and the vertex weighting `(F + 2R + (n-3)P) / n` — so that repeated application
converges toward the limit surface without any Target to reproject onto.

LINEAR SHALL remain the default, and its output SHALL be bit-for-bit what it was
before the smooth mode existed: callers that do not ask for smoothing SHALL NOT
observe any change. The smooth mode SHALL write vertex positions only, leaving
the topology and the propagated vertex/edge/face/corner attributes identical to
the linear mode's.

#### Scenario: Smooth subdivision recovers curvature with no Target
- **WHEN** a coarse faceted cage is subdivided repeatedly in Catmull-Clark mode with no projection target
- **THEN** the worst dihedral angle between adjacent faces SHALL fall substantially at every level, while the same mesh subdivided linearly SHALL retain its original crease angles unchanged however many levels are applied

#### Scenario: The default path is untouched
- **WHEN** the subdivide command is invoked without naming a mode
- **THEN** the result SHALL be bitwise identical to the linear subdivision the engine produced before the smooth mode was added

### Requirement: Creases and boundaries under smooth subdivision
Catmull-Clark subdivision SHALL apply the sharp (crease) rule to any edge that
is tagged as a feature edge or is not shared by exactly two faces — boundary and
non-manifold edges included. This is the same "feature or boundary" predicate the
pipeline's quad relax freezes vertices on, and the two SHALL agree: a smoothing
pass that rounded a crease off would be followed by a relax that pinned the
rounded result.

Consequently: a crease edge's edge point SHALL stay at the edge midpoint; a
vertex with exactly two incident creases SHALL follow the cubic B-spline crease
curve rule; and a vertex with three or more incident creases, or a valence-two
border vertex (the corner of an open patch), SHALL NOT move at all. A mesh with
no feature tags SHALL subdivide fully smoothly.

#### Scenario: An open patch keeps its border
- **WHEN** an open planar patch is subdivided smoothly several times
- **THEN** its bounding box, its planarity and the straightness of its border SHALL be preserved exactly, rather than the border shrinking inward and the corners rounding off

#### Scenario: An open tube's boundary rings stay put
- **WHEN** a tube open at both ends is subdivided smoothly
- **THEN** each end ring SHALL remain in its own plane at every level, rather than creeping along the axis and shortening the tube

#### Scenario: Tagged features stay sharp
- **WHEN** a cube with every edge tagged as a feature edge is subdivided smoothly
- **THEN** the result SHALL still be an exact cube: the corners SHALL NOT move and the new vertices SHALL sit on the cube's faces and edges
