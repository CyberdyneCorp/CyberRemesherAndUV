# engine-bindings — retopology mesh operations

Delta for `add-python-retopo-op-bindings`.

## ADDED Requirements

### Requirement: Retopology mesh-operation bindings
The Python binding SHALL expose the retopology mesh operations that need no
stroke geometry — `cyber_retopo_triangulate`, `_relax`, `_snap_all`,
`_delete_faces`, `_dissolve_edges`, `_insert_loop`, `_merge_vertices` and
`_rotate_edge` — as `Mesh` methods, so a cage built or subdivided from Python
can also be cleaned up, tightened and reprojected there. Each method SHALL
document the C header's element-id stability contract, because caller-side
annotations (pins, loop tags, hidden faces) are keyed on those ids. The batch
operations SHALL preserve the ABI's skip-don't-fail contract, and the
single-element operations SHALL surface the ABI's refusal as the typed error
with the mesh unchanged.

#### Scenario: Whole-mesh commands reach Python
- **WHEN** a caller invokes `Mesh.triangulate`, `Mesh.relax` or `Mesh.snap_all`
- **THEN** the operation SHALL run on the engine and report what the C entry point reports — the resulting face count, or the moved-vertex count and largest displacement

#### Scenario: Relax without a brush centre relaxes the whole mesh
- **WHEN** `Mesh.relax` is called with no centre
- **THEN** every vertex SHALL be relaxed regardless of any radius passed, because the ABI has no separate relax-all entry point and spells it as a non-positive radius

#### Scenario: Pinned vertices are immune
- **WHEN** a vertex id is passed in the `pinned` list of `Mesh.relax` or `Mesh.snap_all`
- **THEN** that vertex's position SHALL be unchanged by the call and SHALL NOT be counted as moved

#### Scenario: Batch operations skip what they cannot act on
- **WHEN** `Mesh.delete_faces` or `Mesh.dissolve_edges` is given ids that are dead, out of range, or ineligible
- **THEN** those ids SHALL be skipped rather than raising, and the returned count SHALL be how many elements were actually removed or dissolved

#### Scenario: A refused single-element operation leaves the mesh unchanged
- **WHEN** `Mesh.rotate_edge`, `Mesh.insert_loop` or `Mesh.merge_vertices` is given arguments the engine refuses
- **THEN** it SHALL raise the typed error carrying the engine's message, and the mesh's vertex and face counts SHALL be unchanged
