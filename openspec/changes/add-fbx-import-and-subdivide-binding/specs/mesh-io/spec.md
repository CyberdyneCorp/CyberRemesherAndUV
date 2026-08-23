# mesh-io — FBX import

Delta for `add-fbx-import-and-subdivide-binding`.

## MODIFIED Requirements

### Requirement: Import formats
The system SHALL import meshes from OBJ, PLY, STL, glTF 2.0 (.gltf/.glb), and
FBX (.fbx, binary and ASCII, geometry only), reading vertex positions, normals,
UVs, and vertex colors where the format provides them. OBJ vertex colors SHALL
be read from both the xyzrgb extension and ZBrush polypaint conventions. FBX
import SHALL apply each mesh node's world transform and rotate the file's axis
convention into the engine frame (right-handed Y-up), so a Z-up export arrives
oriented like the same model exported to OBJ. Coordinates SHALL keep their
authored scale — FBX's declared unit routinely contradicts its own coordinates
(Blender writes "1 unit = 1 cm" into the header while exporting a 2 m cube as
±1), so rescaling by it would break parity with every other importer, all of
which preserve source units; callers normalize from the reported bounds
instead. FBX content that is not geometry (animation, skinning, materials,
cameras, lights) SHALL be ignored rather than rejected.

#### Scenario: OBJ with polypaint vertex colors
- **WHEN** a ZBrush-exported OBJ with vertex colors is imported as a Target
- **THEN** the mesh SHALL load with its per-vertex colors available to the viewport and baking

#### Scenario: FBX quad mesh imports at its authored arity
- **WHEN** an FBX file containing quad faces is imported with the preserve policy
- **THEN** the resulting mesh SHALL contain those quads as 4-vertex faces, not fans of triangles

#### Scenario: Axis convention is normalized, authored scale is preserved
- **WHEN** the same cube is exported to FBX twice, once Y-up and once Z-up, and both are imported
- **THEN** the two imported meshes SHALL agree in bounding box to within floating-point tolerance, and that bounding box SHALL match the cube's authored size rather than a unit-rescaled one

#### Scenario: FBX with multiple mesh nodes
- **WHEN** an FBX file containing several transformed mesh nodes is imported
- **THEN** every mesh node SHALL appear in the imported mesh, positioned by its world transform

### Requirement: Export formats
The system SHALL export meshes to OBJ (+MTL, with normals and UVs), PLY, STL, and glTF 2.0, preserving face arity (quads stay quads in formats that support them); on Apple platforms the application shell SHALL additionally offer USDZ export. Bake outputs SHALL export as PNG and EXR, and a packaged ZIP (mesh + MTL + maps) SHALL be available. FBX SHALL be import-only: an export to `.fbx` SHALL fail with a typed error naming the supported export formats, since writing FBX requires the proprietary binary container and no permissively licensed writer exists.

#### Scenario: Quad-preserving OBJ export
- **WHEN** a quad-dominant remesh result is exported to OBJ
- **THEN** the file SHALL contain 4-vertex `f` records for quads, plus `vt`/`vn` records when UVs/normals exist

#### Scenario: FBX export is refused with an actionable error
- **WHEN** an export targets a `.fbx` path
- **THEN** the operation SHALL fail with an `UnsupportedFormat` error whose message states that FBX is import-only and names the formats that can be written
