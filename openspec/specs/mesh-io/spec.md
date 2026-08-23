# mesh-io Specification

## Purpose
TBD - created by archiving change bootstrap-v1-platform. Update Purpose after archive.
## Requirements
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

### Requirement: Correct polygon handling on import
Importers SHALL preserve or fan-triangulate quads and n-gons correctly according to the caller's requested policy (preserve | triangulate). Under no circumstance SHALL a non-triangulated file be parsed with a fixed stride producing wrong geometry.

#### Scenario: Quad OBJ imports correctly
- **WHEN** an OBJ containing quad faces is imported with the preserve policy
- **THEN** the resulting mesh SHALL contain those quads with correct vertex indices (AutoRemesher silently mis-parsed this case)

### Requirement: Export formats
The system SHALL export meshes to OBJ (+MTL, with normals and UVs), PLY, STL, and glTF 2.0, preserving face arity (quads stay quads in formats that support them); on Apple platforms the application shell SHALL additionally offer USDZ export. Bake outputs SHALL export as PNG and EXR, and a packaged ZIP (mesh + MTL + maps) SHALL be available. FBX SHALL be import-only: an export to `.fbx` SHALL fail with a typed error naming the supported export formats, since writing FBX requires the proprietary binary container and no permissively licensed writer exists.

#### Scenario: Quad-preserving OBJ export
- **WHEN** a quad-dominant remesh result is exported to OBJ
- **THEN** the file SHALL contain 4-vertex `f` records for quads, plus `vt`/`vn` records when UVs/normals exist

#### Scenario: FBX export is refused with an actionable error
- **WHEN** an export targets a `.fbx` path
- **THEN** the operation SHALL fail with an `UnsupportedFormat` error whose message states that FBX is import-only and names the formats that can be written

### Requirement: Loud failure semantics
Every import or export failure (missing file, parse error, unwritable destination, unsupported feature) SHALL produce a typed error with a human-readable message; the GUI SHALL surface it visibly and the CLI SHALL print it to stderr and exit nonzero. Silent failure is prohibited. A decoder SHALL treat every byte it did not write as hostile: no declared count, length or index from a file SHALL be trusted without being checked against the bytes actually present, allocation SHALL be bounded by what the payload can justify rather than by what it declares, and no input SHALL make a decoder loop without progress. A write SHALL NOT be reported successful until the bytes have reached the file.

#### Scenario: Unwritable export path
- **WHEN** an export targets a path that cannot be opened for writing
- **THEN** the operation SHALL fail with an explicit error naming the path and reason (AutoRemesher wrote nothing and reported nothing)

#### Scenario: Corrupt input file
- **WHEN** a malformed glTF file is imported
- **THEN** the importer SHALL reject it with a parse error and the current document SHALL remain unchanged

#### Scenario: A forged count or index is refused, not executed
- **WHEN** a file declares a section length, element count, list length or accessor/bufferView/buffer index that the payload cannot support — including a length chosen to overflow a bounds check (a section declaring 2^64−24 bytes), an out-of-range OBJ `vt`/`vn` index, a PLY element count larger than the remaining bytes, and a glTF accessor count exceeding its decoded buffer
- **THEN** the importer SHALL reject the file with a typed error, SHALL NOT read or write outside its own buffers, and SHALL NOT allocate on the declared figure

#### Scenario: Malformed input terminates in bounded time and memory
- **WHEN** a truncated, self-contradictory or adversarial file is imported — a PLY whose body ends mid-element, an element declaring zero properties with a nonzero count, or a compressed image stream whose expansion exceeds what its own header implies
- **THEN** the call SHALL return a typed error rather than hang or exhaust memory, and the peak allocation SHALL stay bounded by the size the file's own header can justify

#### Scenario: A write that fails at close is a failure
- **WHEN** a mesh, image or document write fills the disk or exceeds a quota, so the failure occurs when the buffer is flushed rather than when it is written
- **THEN** the call SHALL report failure naming the path, and SHALL NOT report success and lose the error in a destructor

### Requirement: Import scale and unit sanity
Importers SHALL report the bounding box of imported geometry, and the application SHALL be able to normalize or preserve source units; the choice SHALL be recorded in the document so exports round-trip at original scale.

#### Scenario: Round-trip preserves scale
- **WHEN** a mesh is imported, retopologized, and exported
- **THEN** the exported EditMesh SHALL be in the same coordinate scale as the imported Target

### Requirement: Named export presets
Export SHALL support named presets that bundle: the set of bake maps to emit,
the output file naming pattern, color-space conventions per map (sRGB vs
linear), the normal-map green-channel convention (+Y or −Y), the mesh format
and its per-format flags, and unit/axis metadata where the chosen format
carries it. The engine SHALL ship built-in presets `blender`, `unity`,
`unreal`, and `gltf-generic`, and SHALL load user-defined preset files using
the same schema.

#### Scenario: One preset produces a ready-to-import set
- **WHEN** an export runs with the `blender` preset on a baked document
- **THEN** the output SHALL contain the mesh and exactly the preset's map set, named per its pattern, with the normal map in the preset's green-channel convention

#### Scenario: User preset behaves like a built-in
- **WHEN** a user preset file with a valid schema is passed by path
- **THEN** the export SHALL honor it exactly as it would a built-in preset

### Requirement: Preset schema is versioned and fails loudly
Preset files SHALL declare a schema version. Loading a preset with an
incompatible schema version SHALL fail with a typed error naming the found and
supported versions, and SHALL NOT silently ignore unknown fields that change
output content.

#### Scenario: Incompatible preset is rejected loudly
- **WHEN** a preset file declares a schema version the engine does not support
- **THEN** the export SHALL fail with a typed error identifying both versions, producing no partial output

