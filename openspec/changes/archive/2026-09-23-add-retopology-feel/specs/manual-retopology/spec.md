## MODIFIED Requirements

### Requirement: Auto Relax mode
When Auto Relax is enabled, every topology-modifying operation SHALL be followed by an automatic local relax of surrounding topology to maintain even quad distribution.

The relax SHALL be scoped to the edit's TOPOLOGICAL neighbourhood — the vertices within a given number of edge hops of the vertices the operation produced — and not to a spatial radius, so an edit on one side of a thin form does not disturb the other side. Vertices outside the neighbourhood SHALL be left bit-identical. Relax weight SHALL fall off with hop distance so the neighbourhood's edge shows no step in quad size. A host SHALL be able to request this relax explicitly for a set of seed vertices; the library SHALL NOT apply it implicitly inside other operations.

#### Scenario: Even quads after build
- **WHEN** Auto Relax is on and a quad strip is extruded
- **THEN** the new and neighboring vertices SHALL be relaxed toward uniform spacing automatically

#### Scenario: Relax stays within its neighbourhood
- **WHEN** a region relax is seeded from one vertex with a neighbourhood of one ring
- **THEN** only that vertex and its one-ring SHALL move, and every other vertex SHALL keep its exact position

#### Scenario: Neighbourhood is topological, not spatial
- **WHEN** a region relax is seeded on one surface that lies close to, but is not connected with, another surface
- **THEN** the unconnected surface SHALL NOT move

#### Scenario: Neighbourhood reaches its full extent
- **WHEN** a vertex exactly N edge hops from the seed is out of place
- **THEN** a relax with a neighbourhood of N rings SHALL move it, and one of N-1 rings SHALL NOT

### Requirement: Pencil stroke grammar
The Pencil action SHALL recognize at minimum these gestures on the Target/EditMesh: closed quad/tri shape → create face; stroke from existing edges/vertices → extend topology; drag across faces → insert edge loop; scribble/X over geometry → delete it; straight line between two vertices → merge/collapse pair (two adjacent triangles → quad); line between two boundary loops with equal vertex count → bridge; circle over an edge → rotate edge (redirect loop flow); closed loop around a cylindrical region → extrude cylinder; double-tap on a vertex/edge-loop → enter Tweak (move vertex / slide loop). Recognition SHALL be tolerant of imperfect strokes.

A loop slide SHALL move every vertex of the edge loop toward the same side of the loop by the same fraction of its rail, SHALL produce the same result regardless of which loop edge it is started from, and SHALL refuse a fraction whose magnitude reaches a full rail. A vertex with no quad on the requested side SHALL stay in place.

#### Scenario: Closed stroke creates a quad
- **WHEN** a roughly four-cornered closed stroke is drawn on the Target surface
- **THEN** a new quad face SHALL be created with its vertices snapped to the surface

#### Scenario: X-stroke deletes
- **WHEN** an X is drawn over EditMesh faces
- **THEN** the faces under the X SHALL be deleted

#### Scenario: Unrecognized stroke gives feedback
- **WHEN** a stroke matches no gesture
- **THEN** the system SHALL show non-intrusive visual feedback that nothing was recognized (CozyBlanket silently ignored these, a top usability complaint)

#### Scenario: Sliding a closed loop does not twist it
- **WHEN** an edge loop that closes around a tube is slid
- **THEN** every vertex of the loop SHALL move toward the same neighbouring loop

#### Scenario: A slide cannot collapse a loop
- **WHEN** a loop slide is requested with a fraction of a full rail or more
- **THEN** it SHALL be refused and the mesh SHALL be unchanged
