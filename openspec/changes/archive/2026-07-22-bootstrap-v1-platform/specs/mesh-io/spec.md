# mesh-io — Import and Export

## ADDED Requirements

### Requirement: Import formats
The system SHALL import meshes from OBJ, PLY, STL, and glTF 2.0 (.gltf/.glb), reading vertex positions, normals, UVs, and vertex colors where the format provides them. OBJ vertex colors SHALL be read from both the xyzrgb extension and ZBrush polypaint conventions.

#### Scenario: OBJ with polypaint vertex colors
- **WHEN** a ZBrush-exported OBJ with vertex colors is imported as a Target
- **THEN** the mesh SHALL load with its per-vertex colors available to the viewport and baking

### Requirement: Correct polygon handling on import
Importers SHALL preserve or fan-triangulate quads and n-gons correctly according to the caller's requested policy (preserve | triangulate). Under no circumstance SHALL a non-triangulated file be parsed with a fixed stride producing wrong geometry.

#### Scenario: Quad OBJ imports correctly
- **WHEN** an OBJ containing quad faces is imported with the preserve policy
- **THEN** the resulting mesh SHALL contain those quads with correct vertex indices (AutoRemesher silently mis-parsed this case)

### Requirement: Export formats
The system SHALL export meshes to OBJ (+MTL, with normals and UVs), PLY, STL, and glTF 2.0, preserving face arity (quads stay quads in formats that support them); on Apple platforms the application shell SHALL additionally offer USDZ export. Bake outputs SHALL export as PNG and EXR, and a packaged ZIP (mesh + MTL + maps) SHALL be available.

#### Scenario: Quad-preserving OBJ export
- **WHEN** a quad-dominant remesh result is exported to OBJ
- **THEN** the file SHALL contain 4-vertex `f` records for quads, plus `vt`/`vn` records when UVs/normals exist

### Requirement: Loud failure semantics
Every import or export failure (missing file, parse error, unwritable destination, unsupported feature) SHALL produce a typed error with a human-readable message; the GUI SHALL surface it visibly and the CLI SHALL print it to stderr and exit nonzero. Silent failure is prohibited.

#### Scenario: Unwritable export path
- **WHEN** an export targets a path that cannot be opened for writing
- **THEN** the operation SHALL fail with an explicit error naming the path and reason (AutoRemesher wrote nothing and reported nothing)

#### Scenario: Corrupt input file
- **WHEN** a malformed glTF file is imported
- **THEN** the importer SHALL reject it with a parse error and the current document SHALL remain unchanged

### Requirement: Import scale and unit sanity
Importers SHALL report the bounding box of imported geometry, and the application SHALL be able to normalize or preserve source units; the choice SHALL be recorded in the document so exports round-trip at original scale.

#### Scenario: Round-trip preserves scale
- **WHEN** a mesh is imported, retopologized, and exported
- **THEN** the exported EditMesh SHALL be in the same coordinate scale as the imported Target
