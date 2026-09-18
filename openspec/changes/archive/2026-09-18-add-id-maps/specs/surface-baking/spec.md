## ADDED Requirements

### Requirement: Material ID and object ID maps
The bake stage SHALL bake a MATERIAL ID map and an OBJECT ID map: one flat colour per
material assignment, and one per object or submesh, on the EditMesh's UV layout.

**Where the ids come from.** The material id SHALL be the Target's face-domain integer
`material_id` column. The object id SHALL be the Target's face-domain integer
`object_id` column, then its `group_id` column, and, when neither exists, the index of
the Target's face-connected component — a multi-part asset merged into one mesh carries
its parts nowhere else. The source actually used SHALL be reported with the map, so a
consumer is never left guessing which of them answered.

**How a colour is assigned.** The colour SHALL be a pure function of the integer id
alone, computed with integer arithmetic only, so that the same id yields the same colour
across runs, machines, compilers and standard libraries. It SHALL NOT be derived from
the iteration order of any container, from a counter advanced during traversal, or from
floating-point transcendental functions, none of which are reproducible across
toolchains. Every assigned colour SHALL lie on the 8-bit lattice, so that writing the
map to an 8-bit container and reading it back returns the same colour exactly. The
colour `(0, 0, 0)` SHALL be reserved to mean "no id" — it SHALL be the value of an
uncovered texel and of a texel whose cage ray reached no Target surface, and no assigned
id SHALL ever take it.

**What is reported.** The bake SHALL report, alongside the image, a table of every
distinct id on the Target and the exact colour written for it, ordered ascending by id.
That table SHALL be reachable from every entry point that produces a map — the C ABI,
the export-bundle result, the language bindings — and SHALL appear in the run's
machine-readable report beside the file it describes. A consumer SHALL be able to
resolve a colour picked out of the map back to the id it was assigned to using that
table alone.

**How it is written.** An id map SHALL be written without anti-aliasing, filtering,
resampling, colour-space conversion or lossy compression: any of these perturb a
boundary texel, and an exact comparison at zero tolerance — the mode a colour-ID
selection uses — then fails on every boundary in the map. A request to write an id map
in a non-linear colour space SHALL be reported and the map written verbatim, rather than
honoured.

Both maps SHALL follow the same rules as the other map types: the same cage projection,
component links, output resolution, texel ceiling, progress reporting and cooperative
cancellation, and SHALL be requestable through every entry point the other maps are.

#### Scenario: One flat colour per material
- **WHEN** a material ID map is baked against a Target whose faces carry two distinct material ids
- **THEN** the texels over each material SHALL hold a single colour, the two colours SHALL differ, and no texel SHALL hold a colour that is neither of them nor the reserved "no id" value

#### Scenario: A merged multi-part asset still separates
- **WHEN** an object ID map is baked against a Target that declares no object or group column but consists of two disconnected parts
- **THEN** the two parts SHALL take different colours, and the reported source SHALL name the face-connected component fallback rather than a column

#### Scenario: A declared column wins over the fallback
- **WHEN** an object ID map is baked against a Target that declares an object column assigning one id to two disconnected parts
- **THEN** both parts SHALL take the SAME colour, because the declared column is the authority

#### Scenario: The same input gives the same colours
- **WHEN** an id map is baked twice from the same input, or on two platforms
- **THEN** the two images SHALL be identical texel for texel and the two reported tables SHALL be identical, because the colour depends on the id and on nothing else

#### Scenario: Neighbouring ids do not have to be neighbouring colours
- **WHEN** an id map is baked over a run of consecutive ids
- **THEN** each colour SHALL depend only on its own id, so that inserting, removing or renumbering one id SHALL NOT change the colour of any other

#### Scenario: The map can be resolved back to ids
- **WHEN** a consumer picks the colour of any covered texel and looks it up in the reported table
- **THEN** exactly one entry SHALL match, and its id SHALL be the id of the Target surface under that texel

#### Scenario: Every colour survives the written file
- **WHEN** an id map is written to the bake's 8-bit output container and read back
- **THEN** every texel SHALL hold exactly the colour the bake assigned, with no tolerance

#### Scenario: A boundary texel is one id or the other, never a blend
- **WHEN** an id map is baked across the boundary between two ids
- **THEN** every texel on the boundary SHALL hold one of the two ids' exact colours, never an intermediate one

#### Scenario: An id map is not colour-space converted
- **WHEN** an export preset declares a non-linear colour space for an id map
- **THEN** the map SHALL be written verbatim and the request SHALL be reported as a warning

#### Scenario: An id map honours the cage and the ceiling
- **WHEN** an id map is requested with a cage that does not reach the Target, or with a texel budget below the request
- **THEN** it SHALL behave exactly as the other maps do: the unreached texels SHALL take the reserved "no id" value, and an over-budget request SHALL be refused with no output image

## MODIFIED Requirements

### Requirement: Bakeable map types
The bake stage SHALL bake from the Target onto the EditMesh's UV layout: tangent-space normal maps, ambient occlusion, displacement/height, color maps (from Target vertex colors, including polypaint, or from a Target texture when the Target has its own UVs — texture-to-texture baking), object-space normal maps, object-space position maps, bent normal maps, thickness maps, material ID maps and object ID maps. Output resolution SHALL be user-selectable up to at least 4096².

Every map type SHALL be requestable through the same entry points, and SHALL honour the same projection cage, texel ceiling, progress reporting and cooperative cancellation. Every numeric parameter a map reads SHALL have a stated default, range and meaning, and a value outside that range SHALL be refused — the bake returns no image — rather than substituted with a default, whichever entry point the request arrives through.

#### Scenario: Normal + color bake
- **WHEN** a bake runs on an EditMesh with valid UVs against a vertex-colored Target
- **THEN** a tangent-space normal map and a color map SHALL be produced at the requested resolution

#### Scenario: Texture-to-texture bake
- **WHEN** the Target carries UVs and a color texture
- **THEN** the bake SHALL sample the Target texture as the color source

#### Scenario: A new map type is reachable everywhere the old ones are
- **WHEN** a host names any bakeable map through the C ABI, an export preset, the CLI's bake list or either language binding
- **THEN** the map SHALL be produced, SHALL appear in the run's JSON report, and SHALL be subject to the host's texel ceiling

#### Scenario: An out-of-range parameter is refused, not defaulted
- **WHEN** a bake is requested with a parameter the chosen map reads set outside its documented range
- **THEN** the bake SHALL fail and return no image, rather than substituting a default

### Requirement: Baked maps record their encoding basis
Every bake SHALL report, with its output image, the basis needed to interpret the numbers in it: whether the values are raw, a direction in the texel's tangent frame, a direction in object space, a position rescaled over a bounding box, a distance in model units, or an exact id colour; the up axis a direction or position was expressed in; the bounding box a position was rescaled over; the factor a distance was multiplied by; and, for an id colour map, the id source and the id-to-colour table.

An ID COLOUR basis SHALL mean that the texels are exact keys rather than measurements: a consumer SHALL NOT filter, resample or colour-convert such a map, and SHALL compare its texels at zero tolerance.

That record SHALL be reachable from every entry point that produces a map — the C ABI, the export-bundle result, the language bindings — and SHALL appear in the run's machine-readable report alongside the file it describes.

#### Scenario: A position map can be decoded
- **WHEN** a consumer reads an object-space position map together with its recorded basis
- **THEN** it SHALL be able to recover the model-unit coordinate of any texel from the recorded bounding box and up axis

#### Scenario: A thickness map carries its scale
- **WHEN** a thickness map is baked with a non-default scale factor
- **THEN** the recorded basis SHALL name that factor, so the underlying distance is recoverable

#### Scenario: An id map carries its table
- **WHEN** an id map is baked
- **THEN** the recorded basis SHALL be the id-colour basis, and SHALL carry the id source and the ordered id-to-colour table

#### Scenario: The report names the basis of every written map
- **WHEN** an export bundle writes its maps and a machine-readable report
- **THEN** each map's entry in the report SHALL carry its encoding basis, the object-space entries SHALL carry their up axis and bounding box, and the id entries SHALL carry their id source and table
