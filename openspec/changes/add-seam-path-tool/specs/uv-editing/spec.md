# uv-editing — auto-routed seam paths

Delta for `add-seam-path-tool`.

## ADDED Requirements

### Requirement: Auto-routed seam paths
In addition to stroke-over-edges seam editing, the UV stage SHALL support
routed seam paths: the user places an ordered sequence of waypoints on the
EditMesh, and between consecutive waypoints the engine SHALL route a
least-cost path along mesh edges, with an edge-cost model that prefers
feature-tagged and high-curvature edges over flat-region crossings. A crease
SHALL be recognised by the MAGNITUDE of its dihedral, so concave (valley) and
convex (ridge) feature lines are both routable, each with its own tunable
weight exposed through the C ABI and the language bindings.

#### Scenario: Route follows the groove
- **WHEN** two waypoints are placed on either side of a concave feature line
- **THEN** the routed path SHALL follow edges along the feature rather than the geodesic shortcut across flat surface

#### Scenario: Route follows a convex ridge
- **WHEN** two waypoints are placed at either end of a convex feature line, with a flat but slightly shorter crossing available
- **THEN** the routed path SHALL follow the ridge rather than the flat shortcut, and setting the convex weight to the flat weight SHALL restore the plain geodesic

### Requirement: Pending paths are editable until commit
Every placed waypoint SHALL be repositionable and deletable before commit,
with the affected route segments re-computed on each edit. Commit SHALL
convert the routed path into seam edges under the existing seam model (so
gesture unwrap and sew behave as with any other seam). After commit a resume
marker SHALL allow continuing the path from the last point; explicitly
dropping the resume marker SHALL NOT modify any committed seam.

#### Scenario: Fix a deviating waypoint
- **WHEN** an intermediate waypoint is dragged to a better position before commit
- **THEN** only the adjacent route segments SHALL re-route and the rest of the pending path SHALL be unchanged

#### Scenario: Commit then resume
- **WHEN** a path is committed and a new waypoint is placed with the resume marker active
- **THEN** the new route SHALL start from the last committed point, and dropping the marker instead SHALL leave all committed seams intact
