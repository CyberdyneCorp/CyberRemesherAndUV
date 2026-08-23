# engine-bindings — format-agnostic I/O and subdivision

Delta for `add-fbx-import-and-subdivide-binding`.

## ADDED Requirements

### Requirement: Format-agnostic mesh load and save binding
The C ABI SHALL expose `cyber_mesh_load` and `cyber_mesh_save`, which dispatch
on the file extension across every format the engine supports, and the Python
binding SHALL expose them as `Mesh.load` / `Mesh.save`. The pre-existing
`cyber_mesh_load_obj` / `cyber_mesh_save_obj` entry points (and their
`Mesh.load_obj` / `Mesh.save_obj` wrappers) SHALL remain as aliases with
identical behaviour, so callers written against them keep working.

#### Scenario: One call loads any supported format
- **WHEN** a caller invokes `Mesh.load` on an OBJ, PLY, STL, glTF, GLB, or FBX file
- **THEN** the mesh SHALL load through the same entry point, and an unsupported extension SHALL raise the typed unsupported-format error naming the path

#### Scenario: The `_obj` aliases keep working
- **WHEN** existing code calls `Mesh.load_obj` / `cyber_mesh_load_obj`
- **THEN** it SHALL behave exactly as `Mesh.load` / `cyber_mesh_load` does

### Requirement: Subdivision binding
The Python binding SHALL expose linear subdivision over `cyber_retopo_subdivide`
as `Mesh.subdivide`, splitting every n-gon into n quads (Catmull-Clark topology,
no smoothing) in place and returning the resulting face count. It SHALL accept
an optional projection target; when given, every vertex of the subdivided mesh
SHALL be projected onto that target's surface, which is what recovers curvature
that linear subdivision alone cannot add.

#### Scenario: Subdividing quadruples a quad mesh
- **WHEN** `Mesh.subdivide()` is called on a mesh of N quads
- **THEN** the mesh SHALL afterwards hold 4N quads and the reported face count SHALL equal the mesh's face count

#### Scenario: Subdivide and reproject recovers curvature
- **WHEN** a coarse mesh is subdivided with a curved surface passed as the projection target
- **THEN** the new vertices SHALL lie on that surface rather than on the coarse mesh's flat facets

#### Scenario: Subdividing an empty mesh fails loudly
- **WHEN** `Mesh.subdivide()` is called on a mesh with no faces
- **THEN** it SHALL raise the typed empty-mesh error rather than silently doing nothing

### Requirement: Every Python binding test and the offline examples are registered with CTest
Each Python binding test file SHALL be registered as a CTest case against the
library built in the same tree, under the existing capability-gated convention
(`SKIP_RETURN_CODE 77` when the C ABI library cannot be loaded), so no binding
test can sit in the tree unrun. The Python examples that need neither network
nor downloaded models SHALL additionally be executed end-to-end by a registered
smoke test, so the example gallery cannot rot unnoticed.

#### Scenario: A new binding test cannot be forgotten
- **WHEN** a test file is added under the Python binding test directory
- **THEN** it SHALL be registered with CTest and run in the same lane as the existing binding tests

#### Scenario: A broken example is caught
- **WHEN** a change breaks an offline example script
- **THEN** the example smoke test SHALL fail, naming the failing script
