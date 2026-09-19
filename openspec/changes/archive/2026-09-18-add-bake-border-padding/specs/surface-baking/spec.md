## ADDED Requirements

### Requirement: Bake output padding across UV island borders
Every baked map SHALL be PADDED before it is returned: the texels immediately outside
each UV island SHALL be filled from the island they border, so that bilinear sampling,
mip generation and block compression at the border read baked values rather than the
background. Padding SHALL apply to every map type, through every entry point that
produces a map, and SHALL take no map-specific exception.

**The radius.** The padding radius SHALL be configurable per bake and SHALL be
expressed in TEXELS, because what it has to cover — a bilinear tap, a mip level, a
compression block — is measured in texels. Its default SHALL be stated and SHALL be 8
texels, which covers the first three mip levels and two 4x4 compression blocks. A
radius of zero SHALL disable padding entirely and return the map exactly as it was
baked. A negative radius SHALL be refused — the bake returns no image — rather than
substituted with a default, whichever entry point the request arrives through. The
padded band SHALL be at most `radius` texels wide, measured outward from the island.

**Extrapolation, not repetition.** A padded texel SHALL CONTINUE the gradient running
off the island rather than repeat the island's edge value. For each padded texel the
bake SHALL find the nearest covered texels among the eight compass directions and
continue, along each such direction, the linear gradient that direction's samples
define. Repeating the edge value leaves a flat plateau whose boundary is a step
discontinuity, and a mip chain averages that step into a visible hard ring; continuing
the gradient does not. A direction along which no second sample exists SHALL fall back
to the nearest value, which is the only information that direction carries.

Because each ring of the band reads the ring before it, the continuation SHALL be
bounded: a padded texel SHALL NOT leave the range the map's own covered texels span by
more than the width of that range. Bounding it to the covered range ALONE would be
wrong, because a map that ramps across its island reaches its own extreme AT the
border and every continuation would flatten back into the repeated edge value.

**Channel semantics decide the fill rule**, and SHALL be taken from the map's recorded
encoding basis rather than from its name:

- A DIRECTION map — a tangent-space or object-space normal, including a bent normal —
  SHALL be RENORMALIZED to unit length after extrapolation. An extrapolated direction
  is not a unit direction, and a shortened normal darkens the padded band in any
  shader that does not renormalize.
- An ID COLOUR map SHALL be padded by NEAREST NEIGHBOUR ONLY: the exact colour of the
  nearest covered texel, copied verbatim, chosen deterministically. It SHALL NOT be
  extrapolated, interpolated or averaged. An interpolated id colour is a colour that
  resolves to no id, which destroys the zero-tolerance comparison an id map exists
  for.
- A SCALAR or POSITION map SHALL NOT be renormalized. There is no unit length to
  restore, and imposing one would replace the map's values with directions.

**What is reported.** The bake SHALL report, alongside the image, the padding radius
applied, the fill rule actually used, and the number of texels the padded band wrote.
That record SHALL be reachable from every entry point that produces a map — the C ABI,
the export-bundle result, the language bindings — and SHALL appear in the run's
machine-readable report beside the file it describes.

**Determinism.** The padded band SHALL depend only on the baked texels and the radius:
the same bake SHALL pad identically across runs, machines, compilers and standard
libraries, and SHALL NOT depend on the order in which texels are visited or on the
iteration order of any container.

#### Scenario: A padded texel continues a gradient instead of repeating the edge
- **WHEN** a map whose values run as a gradient across an island is baked with a
  non-zero padding radius
- **THEN** successive texels of the padded band SHALL continue that gradient outward
- **AND** they SHALL NOT all hold the value of the island's edge texel

#### Scenario: A normal map's padded band decodes to unit directions
- **WHEN** a tangent-space or object-space normal map is baked with a non-zero padding
  radius
- **THEN** every texel of the padded band SHALL decode to a direction of unit length,
  not to a shortened extrapolated vector

#### Scenario: A scalar map is not renormalized
- **WHEN** a single-channel map is baked with a non-zero padding radius
- **THEN** its padded texels SHALL hold extrapolated values of that map, and SHALL NOT
  be driven to a unit magnitude

#### Scenario: An id map's padded band holds exact id colours
- **WHEN** an id map is baked with a non-zero padding radius
- **THEN** every texel of the padded band SHALL hold, verbatim, one of the colours in
  the reported id-to-colour table, resolvable at zero tolerance
- **AND** no padded texel SHALL hold a colour interpolated between two ids

#### Scenario: Zero radius returns the map unpadded
- **WHEN** a bake is requested with a padding radius of zero
- **THEN** the returned image SHALL be identical, texel for texel, to the same bake
  before padding existed, and the reported radius SHALL be zero

#### Scenario: A negative radius is refused, not defaulted
- **WHEN** a bake is requested with a negative padding radius through any entry point
- **THEN** the bake SHALL fail and return no image, rather than substituting the
  default

#### Scenario: A continuation cannot compound without bound
- **WHEN** a map is baked with a padding radius over an island whose values are not
  locally linear
- **THEN** no padded texel SHALL leave the range its covered texels span by more than
  the width of that range

#### Scenario: Padding does not reach beyond its radius
- **WHEN** a map is baked with a padding radius and a UV layout that leaves background
  well outside it
- **THEN** a texel farther than the radius from every island SHALL still hold the
  map's background value

#### Scenario: The report names the padding of every written map
- **WHEN** an export bundle writes its maps and a machine-readable report
- **THEN** each map's entry in the report SHALL carry the padding radius applied, the
  fill rule used, and the number of padded texels

## MODIFIED Requirements

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
colour `(0, 0, 0)` SHALL be reserved to mean "no id" — it SHALL be the value of a texel
the bake wrote nothing to, and of a texel whose cage ray reached no Target surface, and
no assigned id SHALL ever take it. A texel in the map's PADDED BAND is a texel the bake
wrote: it holds the exact colour of the nearest covered texel, under the
nearest-neighbour rule of "Bake output padding across UV island borders", and only
texels beyond that band hold the reserved value.

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
output resolution, texel ceiling, progress reporting, cooperative cancellation and
border padding, and SHALL be requestable through every entry point the other maps are.
They SHALL introduce no id-specific exception to that shared path — including the
component-link selection of "Component links and selective baking", which the bake
entry point applies to no map type today and which, once it does, SHALL reach the id
maps on the same terms as the rest.

#### Scenario: An id map takes the shared bake path unchanged
- **WHEN** an id map and any other raster map are baked from the same EditMesh/Target pair with the same parameters
- **THEN** the id map SHALL honour the cage distance, the UV layout, the texel ceiling, the progress reporting and the cancellation exactly as that other map does, taking no id-specific parameter and making no id-specific exception

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
