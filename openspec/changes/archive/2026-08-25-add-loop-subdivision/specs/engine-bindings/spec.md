# engine-bindings — Loop subdivision binding

Delta for `add-loop-subdivision`.

## ADDED Requirements

### Requirement: Loop subdivision binding
The C ABI and the Python binding SHALL expose Loop subdivision over a triangle
mesh, splitting every triangle into four in place and reporting the resulting
face count. The smoothing mode SHALL be an explicit argument of the call, with
the smooth and linear meanings documented on the binding itself, so a caller who
asks for resolution without a shape change cannot receive smoothing by accident.

They SHALL accept the same optional projection target the quad subdivision
accepts; when given, every vertex of the refined mesh SHALL be projected onto
that target's surface.

A non-triangle input SHALL be reported through a status code distinct from the
invalid-argument and invalid-parameter codes, with a message naming the
offending face and its side count, and the binding SHALL raise a correspondingly
distinct exception type. The bindings SHALL also expose the triangulation the
refusal points callers at.

#### Scenario: A triangle mesh densifies without becoming quads
- **WHEN** `Mesh.loop_subdivide()` is called on a mesh of N triangles
- **THEN** the mesh SHALL afterwards hold 4N triangles and the reported face count SHALL equal the mesh's face count

#### Scenario: Linear mode is reachable and does not smooth
- **WHEN** `Mesh.loop_subdivide(LoopSubdivideMode.LINEAR)` is called
- **THEN** every original vertex position SHALL still be present in the refined mesh

#### Scenario: A quad mesh raises the topology error
- **WHEN** `Mesh.loop_subdivide()` is called on a mesh containing quads
- **THEN** it SHALL raise the typed unsupported-topology error naming the face and its side count, and the mesh SHALL be unchanged
