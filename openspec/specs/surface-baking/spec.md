# surface-baking Specification

## Purpose
Transferring detail from a high-resolution Target onto the remeshed low-poly
result: normal, ambient occlusion, curvature and cavity maps, the editable cage
that controls the projection, and the component links that decide what bakes
against what. It exists because baking is where a retopology result becomes
usable — and because it is the most expensive stage in a run, it must be
accelerated, cancellable and previewable rather than a blind wait.
## Requirements
### Requirement: Bakeable map types
The bake stage SHALL bake from the Target onto the EditMesh's UV layout: tangent-space normal maps, ambient occlusion, displacement/height, color maps (from Target vertex colors, including polypaint, or from a Target texture when the Target has its own UVs — texture-to-texture baking), object-space normal maps, object-space position maps, world-space direction maps, bent normal maps, thickness maps, UV density maps, material ID maps and object ID maps. Output resolution SHALL be user-selectable up to 16384² — each dimension up to 16384 texels — for every map type. An output above what the host can hold in memory at once SHALL be reachable through the regioned path of "Regioned baking with a bounded working set", never by allocating the whole output at once.

Every map type SHALL be requestable through the same entry points, and SHALL honour the same projection cage, texel ceiling, progress reporting and cooperative cancellation. The one map that reads nothing from where the cage ray lands — the UV density map, a property of the EditMesh's own UV layout — SHALL still accept the cage and SHALL be unchanged by it; see "UV density maps". Every numeric parameter a map reads SHALL have a stated default, range and meaning, and a value outside that range SHALL be refused — the bake returns no image — rather than substituted with a default, whichever entry point the request arrives through.

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

### Requirement: Editable bake cage
The bake SHALL use a projection cage derived from the EditMesh, editable with the core actions: Tweak adjusts cage distance with brush falloff and double-tap sets per-vertex cage distance, Relax smooths the cage, Erase resets edited regions to default. Cage state SHALL persist in the document.

#### Scenario: Per-vertex cage distance
- **WHEN** the user double-taps a cage vertex and enters a distance
- **THEN** that vertex's cage offset SHALL change independently of the brush falloff

### Requirement: Bake correctness and preview
Because retopo, UVs, and bake share one scene, bakes SHALL be free of scale mismatches and tangent-basis inconsistencies by construction: the tangent basis used for baking SHALL be identical to the one exported with the mesh. The viewport SHALL preview bake results on the EditMesh with a repositionable preview light (Move action).

#### Scenario: Exported normal map renders correctly
- **WHEN** the exported mesh and normal map are loaded in a standard glTF viewer
- **THEN** shading SHALL match the in-app bake preview without seams or inverted channels

### Requirement: Accelerated, cancellable baking
Bake ray casting SHALL dispatch through the compute-acceleration layer (GPU when available, CPU fallback), report progress, and honor cooperative cancellation, leaving prior bake results untouched on cancel.

#### Scenario: Cancel a bake
- **WHEN** cancellation is requested mid-bake
- **THEN** the bake SHALL stop within 100 ms and previously baked maps SHALL remain as they were

### Requirement: Curvature and cavity maps
The bake stage SHALL bake a curvature map from the Target onto the EditMesh's
UV layout: signed surface curvature encoded around a midpoint gray, with
convex regions brighter and concave regions darker, normalized by a
user-controllable curvature range. A cavity variant SHALL also be available
that encodes concavity only (flat and convex regions map to white), suitable
for direct use as a multiply mask.

When the curvature range is left at 0 the bake SHALL derive it from the Target
as a percentile of |curvature| weighted by the surface area each sample speaks
for, so a region influences the range in proportion to the area it covers and
not to the number of vertices sitting on it.

Curvature baking SHALL follow the same rules as the other map types: the same
cage projection, output resolution up to 16384² (through the regioned path where the output exceeds the working set),
GPU dispatch with progress reporting and cancellation, and PNG/EXR output.

#### Scenario: Curvature bake distinguishes edges from crevices
- **WHEN** a curvature bake runs against a Target with both sharp convex edges and deep concave seams
- **THEN** the convex edges SHALL read brighter than the midpoint and the concave seams darker, at the requested resolution

#### Scenario: Cavity variant masks concavity only
- **WHEN** a cavity bake runs on the same Target
- **THEN** concave seams SHALL read dark while flat and convex regions read white

#### Scenario: Auto range is not captured by a dense sliver fan
- **WHEN** an auto-ranged curvature bake runs against a Target whose parameterization piles a large share of its vertices onto a vanishing share of its area, such as the sliver fans at a UV sphere's poles
- **THEN** the range SHALL be set by the curvature of the bulk of the surface, leaving the interior detail legible rather than compressed toward the midpoint

#### Scenario: Curvature respects the cage
- **WHEN** the projection cage is edited and the curvature bake re-runs
- **THEN** the sampled regions SHALL follow the edited cage exactly as a normal-map bake would

### Requirement: A host can cap bake texel allocation

The system SHALL allow a host to configure an optional maximum number of bake
texels — the TEXEL CEILING. The ceiling SHALL bound the OUTPUT: the `width * height`
of the map a request produces (per tile, and in aggregate over a UDIM set, as "UDIM-aware
baking" states). It SHALL NOT bound, and SHALL NOT be read as, the memory a bake holds in
flight; that is the separate WORKING-SET bound of "Regioned baking with a bounded working
set", and a request SHALL NOT be refused by the ceiling because of its working set, nor by
the working-set bound at all.

The system SHALL reject a request whose `width * height` exceeds the ceiling before UV
rasterization, before any output image or region is allocated, and before any scratch
storage is created. Zero SHALL disable the ceiling, and an overflowed texel product SHALL be
rejected.

#### Scenario: Requested bake is over budget

- **GIVEN** a host sets a texel ceiling below the requested width times height
- **WHEN** it starts a bake
- **THEN** the operation SHALL fail with a diagnostic naming the request and ceiling
- **AND** no output image SHALL be returned

#### Scenario: The ceiling bounds the output, not the working set

- **GIVEN** a host sets a texel ceiling of 16384 * 16384 and a working-set bound far below it
- **WHEN** it requests a 16384 x 16384 regioned bake
- **THEN** the bake SHALL NOT be refused by either bound
- **AND** it SHALL be produced in regions whose in-flight texels stay within the working-set bound

### Requirement: Object-space normal and position maps
The bake stage SHALL bake an OBJECT-SPACE NORMAL map: the Target's surface normal at the cage hit, expressed in the mesh's own coordinate space and encoded as `n * 0.5 + 0.5`. It SHALL also bake an OBJECT-SPACE POSITION map: the same hit point the position map records in model units, rescaled so that the bake's bounding box spans `[0,1]` on every axis.

Both SHALL take a selectable up axis — y-up (the default, the engine's own convention) or z-up — applied to the values and recorded with the output. Both SHALL cost one projection ray per texel, the same cage ray the normal map casts, and SHALL fall back to the EditMesh's own surface point and normal where that ray misses the Target.

The bounding box SHALL contain every value the map can write, and SHALL be expressed in the same up-axis convention as the map. An axis of zero extent SHALL encode to the midpoint rather than divide by zero.

The existing position map SHALL keep its meaning — the hit point in model units — unchanged.

#### Scenario: Object-space position spans the bake bounds
- **WHEN** an object-space position map is baked against a Target with a non-degenerate bounding box
- **THEN** every covered texel SHALL hold a value inside `[0,1]` on every channel, and texels at opposite ends of the layout SHALL differ along the axis they are separated on

#### Scenario: Up axis is applied, not merely declared
- **WHEN** the same object-space bake is run once as y-up and once as z-up
- **THEN** the two maps SHALL differ, and the z-up values SHALL be the y-up values re-expressed in a z-up frame

#### Scenario: A flat Target does not divide by zero
- **WHEN** an object-space position map is baked against a Target with no extent on one axis
- **THEN** that channel SHALL hold the midpoint value everywhere and no texel SHALL hold a non-finite number

#### Scenario: The world-space position map is unchanged
- **WHEN** a position map is baked before and after object-space maps exist
- **THEN** it SHALL hold the hit point in model units, unencoded, exactly as before

### Requirement: Bent normal and thickness maps
The bake stage SHALL bake a BENT NORMAL map: the average of the hemisphere sample directions that the ambient-occlusion bake found unoccluded, renormalized and encoded as `n * 0.5 + 0.5`. Where every sample is occluded it SHALL fall back to the surface normal. Its frame SHALL be selectable — the texel's tangent frame (the default, matching the tangent-space normal map) or object space — and SHALL be recorded with the output.

The bake stage SHALL also bake a THICKNESS map: the same cosine-weighted hemisphere cast about the INVERTED surface normal, recording for each ray the distance to the first BACK-FACING hit within the occlusion radius. A ray that hits nothing, or that hits a front face, SHALL contribute zero — it never entered material. The mean SHALL be multiplied by a stated scale factor, defaulting to 2.0, and SHALL be written in model units.

Both maps SHALL use the ambient-occlusion bake's sample budget, radius, bias, per-texel sample rotation, projection cage, progress reporting and cooperative cancellation, and SHALL dispatch their rays through the compute-acceleration layer as the ambient-occlusion bake does.

#### Scenario: Bent normal leans away from an occluder
- **WHEN** a bent normal map is baked on a flat surface beside a tall wall
- **THEN** the direction at a texel beside the wall SHALL lean away from it, while a texel far from the wall SHALL stay close to the surface normal

#### Scenario: The bent normal is a unit direction
- **WHEN** a bent normal map is baked in either frame
- **THEN** every covered texel SHALL decode to a direction of unit length, not to the unnormalized sum of the open sample directions

#### Scenario: The tangent frame's axes are not interchangeable
- **WHEN** a bent normal map is baked in the default tangent frame beside an occluder that is asymmetric along the tangent axis alone
- **THEN** the red channel SHALL move and the green channel SHALL stay neutral, and an occluder asymmetric along the bitangent axis alone SHALL do the reverse

#### Scenario: Thickness reads a solid
- **WHEN** a thickness map is baked against a solid Target
- **THEN** covered texels SHALL hold a positive distance of the order of the material behind them, scaled by the stated factor

#### Scenario: Thickness is a distance, bounded by the occlusion radius
- **WHEN** a thickness map is baked against a slab of known depth
- **THEN** the value SHALL be the cosine-weighted mean exit distance for that depth and occlusion radius, times the stated scale
- **AND** an occlusion radius short enough to cut off the grazing paths SHALL reduce the value, because a ray longer than the radius contributes zero

#### Scenario: The cage decides which surface the ray-traced maps sample
- **WHEN** a bent normal or thickness map is baked against a Target the projection cage does not reach
- **THEN** the hemisphere SHALL be anchored on the EditMesh's own surface and frame, and SHALL reach the Target only once the cage is opened far enough to project onto it

#### Scenario: A thin double-sided surface reads near zero
- **WHEN** a thickness map is baked with the cage applied against a thin double-sided Target, whose inverted-normal rays escape without meeting a back face
- **THEN** the map SHALL read near zero rather than the surface's nominal thickness

#### Scenario: The ray-traced maps report progress as they accumulate
- **WHEN** a bent normal, thickness or ambient-occlusion bake runs with a progress sink attached
- **THEN** progress SHALL be reported repeatedly while texels accumulate, not once at the end

#### Scenario: Cancelling a ray-traced bake
- **WHEN** cancellation is requested during a bent normal or thickness bake
- **THEN** the bake SHALL stop and report itself cancelled, exactly as an ambient-occlusion bake does

#### Scenario: The same rays on any backend
- **WHEN** the ray-traced maps are baked on the CPU reference and on an available accelerated backend
- **THEN** the results SHALL agree within the compute-acceleration layer's existing raycast parity tolerance, because they dispatch through the same primitive the ambient-occlusion bake does

#### Scenario: The partition is not part of the answer
- **WHEN** the same ray-traced bake is run with the texel loop handed out in chunks and one texel at a time
- **THEN** the two images SHALL be identical texel for texel, so a map is a property of the rays rather than of how the compute layer split the work

### Requirement: Baked maps record their encoding basis
Every bake SHALL report, with its output image, the basis needed to interpret the numbers in it: whether the values are raw, a direction in the texel's tangent frame, a direction in object space, a direction in world space, a position rescaled over a bounding box, a distance in model units, a UV density, or an exact id colour; the up axis a direction or position was expressed in; the bounding box a position was rescaled over; the placement transform a world-space direction was carried through; the factor a distance was multiplied by; the normalization mode and measured mean of a UV density map; and, for an id colour map, the id source and the id-to-colour table.

An ID COLOUR basis SHALL mean that the texels are exact keys rather than measurements: a consumer SHALL NOT filter, resample or colour-convert such a map, and SHALL compare its texels at zero tolerance.

Every basis SHALL also declare the value range that map's own encoding guarantees, which border padding confines the padded band to. A range SHALL be stated honestly rather than assumed to be `[0,1]`: a UV density map's range is `[0, +infinity)`, and a map whose encoding guarantees no range at all SHALL declare none.

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

#### Scenario: A density map carries its normalization
- **WHEN** a UV density map is baked in either normalization mode
- **THEN** the recorded basis SHALL be the UV-density basis and SHALL carry the mode and the measured mean, so an absolute density is recoverable from a relative map

#### Scenario: The report names the basis of every written map
- **WHEN** an export bundle writes its maps and a machine-readable report
- **THEN** each map's entry in the report SHALL carry its encoding basis, the object-space entries SHALL carry their up axis and bounding box, the world-space direction entry SHALL carry its placement transform, the UV density entry SHALL carry its normalization mode and mean, and the id entries SHALL carry their id source and table

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

The continuation SHALL ALSO stay inside the value range the map's OWN encoding
guarantees, which each map SHALL declare alongside its encoding basis. An object-space
position is `(p - min) / (max - min)` and never leaves `[0,1]`; an ambient occlusion is
a fraction of a hemisphere; a thickness is a distance and is never negative. The
compounding bound above is a bound on RUNAWAY and is deliberately wider than any of
these — for a map spanning `[0,1]` it permits `[-1,2]` — so it cannot serve as this one.
A padded texel outside its map's own range is not a continuation of the map, it is a
value the map's contract says cannot occur: a consumer decoding an object-space position
as `min + v * (max - min)` would be handed a point outside the box the bake recorded.
A map whose encoding guarantees no range — a position in model units, a signed
displacement, a colour copied verbatim off the Target — SHALL be bounded by the
compounding limit alone.

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
applied, the fill rule actually used — none where the band wrote no texel, because no
rule was applied — and the number of texels the padded band wrote.
That record SHALL be reachable from every entry point that produces a map — the C ABI,
the export-bundle result, the language bindings — and SHALL appear in the run's
machine-readable report beside the file it describes.

**Determinism.** The padded band SHALL depend only on the baked texels and the radius:
the same bake SHALL pad identically across runs, machines, compilers and standard
libraries, and SHALL NOT depend on the order in which texels are visited or on the
iteration order of any container. In particular a texel filled by a ring SHALL NOT be a
source for another texel of that SAME ring: each ring SHALL read only the coverage that
existed before it began.

**Cancellation.** The padding stage SHALL poll the cancellation token as it grows the
band, not only before it starts, and SHALL stop between rings and report the bake
cancelled — a radius of 8 on a large map is work a host can be waiting on.

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

#### Scenario: A padded band stays inside the range its map's encoding guarantees
- **WHEN** a map whose encoding guarantees a value range is baked with a padding radius
  over an island whose values ramp into the limits of that range
- **THEN** no texel of the padded band SHALL leave that range: an object-space position's
  band SHALL stay inside the box the map records, an ambient occlusion's band SHALL not
  exceed a whole hemisphere, and a thickness's band SHALL not go negative
- **AND** a map whose encoding guarantees no range SHALL still be free to continue past
  the values its island holds

#### Scenario: The band does not depend on the order texels are visited in
- **WHEN** the same image and coverage are padded twice, the second time with the covered
  texels listed in a different order
- **THEN** the two bands SHALL be identical texel for texel
- **AND** the value of a texel in a ring SHALL be the one its sources from BEFORE that
  ring give it, whichever of its siblings in the same ring were filled first

#### Scenario: Cancellation during the padding stage
- **WHEN** cancellation is requested after the shade has finished but while the padded
  band is still growing
- **THEN** the bake SHALL stop between rings and report itself cancelled
- **AND** the texels the remaining rings would have filled SHALL be left as they were

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

### Requirement: World-space direction map and the placement transform

The bake stage SHALL accept a PLACEMENT TRANSFORM as a bake parameter: the affine
object→world matrix a host has applied to put the asset in its scene, expressed as a 4x4
row-major matrix and defaulting to the IDENTITY. It SHALL be accepted, validated and
recorded identically at every entry point that produces a map.

The bake stage SHALL bake a WORLD-SPACE DIRECTION map: the Target's surface normal at the
cage hit, carried into world space by the placement transform, renormalized to unit
length, expressed in the selected up axis and encoded as `n * 0.5 + 0.5`. It SHALL cost
one projection ray per texel — the same cage ray the normal map casts — and SHALL fall
back to the EditMesh's own surface normal where that ray misses the Target, exactly as the
object-space normal map does.

**The normal SHALL be carried by the placement's inverse transpose**, not by the placement
itself. Under a placement carrying non-uniform scale the two differ, and multiplying a
normal by the placement directly shears it off the surface — which is wrong precisely on
the assets a placement transform exists for.

**Why the transform is part of the map's definition.** This engine has ONE MODEL SPACE:
"world" and "object" name the same space, and without a placement a world-space direction
map would be bit-identical to the object-space normal map and would ship a duplicate under
a second name. The map is defined by what the placement does to it. Accordingly, under an
IDENTITY placement the world-space direction map SHALL equal the object-space normal map
EXACTLY, texel for texel and at zero tolerance — the statement that the two maps differ by
the placement transform and by nothing else — and under any other placement it SHALL
differ.

**What is refused.** A placement with a non-finite element, or whose linear part is
singular and therefore carries no direction anywhere, SHALL be refused — the bake returns
no image — rather than substituted with the identity, whichever entry point the request
arrives through.

The placement SHALL be checked ONLY for a request that produces a map reading it, and a
request that produces no such map SHALL NOT be refused for whatever the placement field
holds — so a bake that worked before still works. This is not a courtesy: the placement
was APPENDED to the parameter block of a published ABI, so a host that zero-fills that
block and assigns the members it knows about — the documented way to write against the
previous version — supplies an all-zero and therefore singular matrix. Checking it
unconditionally would make every map that predates the placement fail on upgrade.

The set of maps that read a placement SHALL be published by the engine and consulted by
every entry point that validates one, rather than restated at each; a request that batches
several maps (an export bundle) SHALL apply the check when ANY map it writes reads a
placement.

**What is recorded.** The placement actually applied SHALL be reported alongside the
image, together with the up axis, so that a consumer can recover the object-space
direction from a world-space one.

**What the placement does NOT change.** No existing map SHALL change meaning: the
object-space normal, object-space position and model-unit position maps SHALL be produced
exactly as before whatever the placement is set to.

The world-space direction map SHALL follow the same rules as every other map type: the
same projection cage, output resolution, texel ceiling, progress reporting, cooperative
cancellation and border padding, and SHALL be requestable through every entry point the
other maps are. Its encoding basis SHALL be a DIRECTION basis, so its padded band SHALL be
renormalized to unit length under "Bake output padding across UV island borders", and its
encoding SHALL guarantee the range `[0,1]` on every channel.

#### Scenario: An identity placement reproduces the object-space normal map exactly
- **WHEN** a world-space direction map and an object-space normal map are baked from the same EditMesh/Target pair with the same parameters and an identity placement
- **THEN** the two images SHALL be identical texel for texel at zero tolerance

#### Scenario: A placement rotation turns the map
- **WHEN** the same world-space direction map is baked with a placement that rotates the asset by a quarter turn
- **THEN** the decoded direction at every covered texel SHALL be the identity bake's direction carried through that rotation, and the two images SHALL differ

#### Scenario: A non-uniform scale does not shear the normal off the surface
- **WHEN** a world-space direction map is baked with a placement carrying a non-uniform scale
- **THEN** the decoded direction SHALL be the surface normal of the placed surface — the object-space normal carried by the placement's inverse transpose — rather than the object-space normal multiplied by the placement

#### Scenario: A singular placement is refused, not defaulted
- **WHEN** a world-space direction map is requested with a placement whose linear part is singular, or that holds a non-finite element, through any entry point
- **THEN** the bake SHALL fail and return no image, rather than substituting the identity

#### Scenario: The placement reaches only the map that reads it
- **WHEN** any map other than the world-space direction map is baked with a non-identity placement
- **THEN** that map SHALL be produced exactly as it is with an identity placement

#### Scenario: A map that reads no placement is not refused by an unusable one
- **WHEN** a map that reads no placement is requested, through any entry point, with a placement that is singular or non-finite — including the all-zero matrix a caller written against the previous ABI leaves behind
- **THEN** the map SHALL be produced normally, and its recorded placement SHALL be the identity

#### Scenario: The placement and the up axis compose in one order
- **WHEN** a world-space direction map is baked with BOTH a placement carrying a non-uniform scale and a non-default up axis
- **THEN** the normal SHALL be carried into world space FIRST and the up axis SHALL re-express that world-space result, rather than the placement being applied to an already re-expressed direction

#### Scenario: A world-space direction map can be decoded back to object space
- **WHEN** a consumer reads a world-space direction map together with its recorded basis
- **THEN** the recorded placement and up axis SHALL be enough to recover the object-space direction of any texel

#### Scenario: The world direction map's padded band decodes to unit directions
- **WHEN** a world-space direction map is baked with a non-zero padding radius
- **THEN** every texel of the padded band SHALL decode to a direction of unit length, and no texel SHALL leave `[0,1]` on any channel

### Requirement: UV density maps

The bake stage SHALL bake a UV DENSITY map onto the EditMesh's UV layout: a single-channel
scalar holding, for each covered texel, the TEXELS PER UNIT OF SURFACE AREA that the
EditMesh's UV layout gives the surface under that texel at the requested output
resolution. The units SHALL be texels per SQUARE model unit; the linear "texels per unit
of length" convention is its square root, and the map SHALL state which it holds rather
than leave a consumer to infer it.

The value SHALL be a property of the EditMesh's UV layout and the requested resolution
alone. It SHALL NOT depend on the Target: a map that changed when the Target changed would
be measuring the wrong thing.

**Normalization SHALL be selectable**, and SHALL default to ABSOLUTE:

- **ABSOLUTE** — texels per unit of surface area as measured, which is what a
  scale-locked material needs in order to hold a constant real-world texel scale.
- **RELATIVE** — each defined texel divided by the MEAN density of the map's own defined
  texels, which is what shows an artist that one island is packed at a different density
  from the rest.

The mode SHALL be recorded with the output, and so SHALL the mean that the relative form
divided by — whichever mode was selected, so that a relative map converts back to an
absolute one and an absolute map still reports what its own average is. When the map has
no defined texel at all the mean SHALL be reported as zero and no texel SHALL be divided.

**The degenerate case SHALL take a documented SENTINEL.** A covered texel whose face has
no UV area, or no surface area, has no density: the ratio is undefined. Such a texel SHALL
hold exactly ZERO, SHALL be excluded from the mean, and SHALL NOT hold an infinity or a
NaN — either of which would poison the mean the relative mode divides by, and neither of
which survives an 8-bit or a clamped write as anything a consumer can recognise. Zero is
safe as the sentinel because no DEFINED density can take it: a texel exists only because a
face covered it, so a defined density is a positive UV area over a positive surface area
and is strictly positive. A value that underflows to zero SHALL be classified as undefined
too, so the sentinel keeps its meaning. It SHALL also be the value of a texel the bake
wrote nothing to, so "no density here" reads the same either way.

The two degenerate faces reach that zero by DIFFERENT routes, and the map is required to
be indistinguishable between them. A face with surface area and no UV AREA covers no texel
at all — it rasterizes to nothing — so its region of the map holds the uncovered
background; a face with UV area and no SURFACE area does cover texels, and each of them is
written the sentinel explicitly. Both read as exactly zero, both are excluded from the
mean, and neither yields an infinity or a NaN anywhere in the image.

**The value range a UV density map's encoding guarantees SHALL be `[0, +infinity)`** — a
density is never negative, and the ratio of texels to surface area has no upper bound. It
is explicitly NOT `[0,1]` in either normalization mode. Its padded band SHALL therefore be
extrapolated and SHALL NOT be renormalized — there is no unit length to restore — and
SHALL be confined by that range and by the compounding limit of "Bake output padding
across UV island borders".

**UDIM.** A face SHALL have its density computed against the resolution of the TILE it
lands in, so that a set of tiles baked at one resolution reports the same density as a
single map at that resolution; and the relative mean SHALL be taken over the WHOLE SET of
tiles rather than per tile, because a per-tile mean would report every tile as average and
hide exactly the unevenness the relative mode exists to show. See "UDIM-aware baking",
which implements this and carries the scenario.

UV density SHALL follow the same rules as every other map type: the same output resolution,
texel ceiling, progress reporting, cooperative cancellation and border padding, and SHALL
be requestable through every entry point the other maps are.

It is the ONE exception to "every map type honours the same projection cage", and the
exception is forced by the paragraph above: a map that reads nothing from where the cage
ray lands cannot be changed by how far that ray travels. Accordingly the projection cage
SHALL be accepted on a UV density request and SHALL NOT change a single texel of the
result. Every other map type, including the world-space direction map, honours the cage in
the ordinary sense — it reads the Target at the hit and falls back to the EditMesh's own
surface where the ray misses.

#### Scenario: A uniformly unwrapped surface reads one density
- **WHEN** a UV density map is baked in absolute mode on an EditMesh whose UV layout gives every face the same texels-per-area
- **THEN** every covered texel SHALL hold the same positive value, equal to the UV area of the layout times the texel count divided by the surface area

#### Scenario: Doubling the resolution doubles the density
- **WHEN** the same absolute UV density bake is run at one resolution and at twice that resolution on each axis
- **THEN** the second map's covered texels SHALL hold four times the first's, because four times as many texels cover the same surface

#### Scenario: An unevenly packed layout shows the unevenness
- **WHEN** a UV density map is baked on an EditMesh with two islands of equal surface area packed at different UV densities
- **THEN** the texels over the more densely packed island SHALL hold a higher value than those over the other

#### Scenario: Relative normalization centres the map on its own mean
- **WHEN** a UV density map is baked in relative mode
- **THEN** every defined texel SHALL hold its absolute density divided by the mean of the map's defined texels, and the mean reported with the map SHALL be that absolute mean

#### Scenario: A face with no surface area takes the sentinel and does not poison the mean
- **WHEN** a UV density map is baked in relative mode over a layout containing a face with UV area and no surface area
- **THEN** every texel that face covers SHALL hold exactly zero, no texel of the map SHALL hold an infinity or a NaN, and the reported mean SHALL be the mean of the DEFINED texels alone — the same mean the map would report without that face

#### Scenario: A face with no UV area covers nothing and changes nothing
- **WHEN** a UV density map is baked over a layout containing a face with surface area whose UV corners are degenerate, so its UV area is zero
- **THEN** that face SHALL write no texel, its region of the map SHALL read the same zero the uncovered background holds, and the reported mean SHALL be the one the map reports without that face

#### Scenario: The mean counts the defined texels, not the image
- **WHEN** a UV density map is baked over a layout that leaves part of the UV square uncovered
- **THEN** the reported mean SHALL be the mean of the covered, defined texels alone, unchanged by how much background surrounds them — and in relative mode a uniformly packed island SHALL therefore read exactly 1 however much background there is

#### Scenario: The normalization mode is recorded
- **WHEN** a UV density map is baked in either mode through any entry point that produces a map
- **THEN** the mode and the measured mean SHALL be reported alongside the image and SHALL appear in the run's machine-readable report beside the file they describe

#### Scenario: A density map's padded band is extrapolated and never negative
- **WHEN** a UV density map is baked with a non-zero padding radius
- **THEN** the band SHALL continue the map's gradient rather than be renormalized to a unit magnitude
- **AND** no texel of the band SHALL hold a negative value

#### Scenario: The density map does not read the Target
- **WHEN** a UV density map is baked twice from the same EditMesh against two different Targets with the same parameters
- **THEN** the two images SHALL be identical texel for texel

#### Scenario: The projection cage moves the world map and not the density map
- **WHEN** the same EditMesh and Target are baked twice with a cage too short to reach the Target and once with a cage that reaches it
- **THEN** the world-space direction map SHALL fall back to the EditMesh's own surface normal in the first and hold the Target's normal in the second
- **AND** the UV density map SHALL be identical texel for texel between the two

### Requirement: UDIM-aware baking

The bake stage SHALL be UDIM-aware: it SHALL detect the occupied tiles of the EditMesh's UV
layout and bake ONE OUTPUT PER OCCUPIED TILE, for every map type, through every entry point
that produces a map.

**The numbering SHALL be the standard one**: the tile whose UV origin is `(u, v)` — covering
`[u, u+1) x [v, v+1)` in UV space — is tile `1001 + u + 10 * v`. The unit square is therefore
tile `1001`, and a bake that is not UDIM-aware is the tile-`1001` case of one that is.

**Occupancy SHALL be exact, not conservative.** A tile SHALL be reported occupied when some
triangle of the UV layout actually OVERLAPS it, not when a triangle's UV bounding box does.
A bounding box reports a tile that a triangle merely reaches around, and every tile reported
is a tile allocated.

**The addressable grid SHALL be bounded and a coordinate outside it SHALL be counted, not
dropped.** `u` outside `[0, 9]` has no tile number under this numbering, and neither does a
negative `v`. UV coordinates that fall outside the addressable grid SHALL NOT be baked, and
their faces SHALL be COUNTED and reported alongside the tile list, so a layout authored in a
convention this numbering cannot address is visible rather than silently missing from the
output.

**Occupied tiles SHALL be reported BEFORE baking starts**, as a list ascending by tile number,
and that list SHALL be obtainable WITHOUT baking — a host has to be able to show what it is
about to allocate, and a refusal has to be able to name what it was asked for.

**Allocation SHALL be for occupied tiles ONLY.** A mesh occupying three tiles of a possible
hundred SHALL cost three images, three rasterizations and three shading passes, not a hundred
of any of them.

**The acceleration structure SHALL be built ONCE over the WHOLE Target and shared by every
tile**, and the rays a bake casts SHALL see the whole mesh whatever tile is being written.
This is the requirement that makes the naive per-tile loop wrong: a structure rebuilt from
the faces whose UVs lie in the tile being written yields occlusion that is entirely plausible
and entirely false — an arm stops occluding the torso the moment the two are packed into
different tiles — and no inspection of the output reveals it. Everything else a bake derives
from the Target that cannot change between tiles — its vertex normals, its curvature field,
its id column — SHALL likewise be derived once.

**The texel ceiling SHALL apply PER TILE and IN AGGREGATE, and a refusal SHALL NAME WHICH.**
A request whose `width * height` exceeds the ceiling SHALL be refused as a PER-TILE overflow;
a request whose `width * height * occupied tile count` exceeds it, while a single tile fits,
SHALL be refused as an AGGREGATE overflow. These are two distinct diagnostics: "this tile is
too big" and "this many tiles of this size are too many" are different problems with
different fixes, and one message covering both tells a host neither. A refusal SHALL produce
no output image for any tile.

**The same two ceilings SHALL bind an entry point that bakes a SET of maps** — the export
bundle — and SHALL be decided BEFORE that entry point writes its first file, so a refusal
leaves no partial bundle behind. A host's ceiling exists to bound what one request may
allocate; a UDIM bundle multiplies that by the occupied-tile count, so a ceiling that stops a
single map must stop the set. An entry point with NO ceiling configured (the value zero)
bakes whatever it was asked for, which is what a caller with no host policy passes.

**Encoding metadata SHALL describe the WHOLE SET, not one tile.** Where a map's encoding is
derived from the mesh rather than from a texel:

- The object-space bounds an object-position map is rescaled over SHALL be the whole mesh's,
  identical in every tile of the set. Bounds computed per tile would make the same
  object-space point decode to different coordinates in different tiles, which is wrong for
  the one consumer the encoding exists for.
- The id-to-colour table an id map reports SHALL be the whole Target's, identical in every
  tile of the set. The same material SHALL therefore take the same colour in tile 1001 and in
  tile 1002; a host's saved selection otherwise breaks the moment it crosses a tile.
- A relative UV-density map SHALL divide by the mean of the WHOLE SET's defined texels, and
  SHALL report that mean with every tile. A per-tile mean reports every tile as average and
  hides exactly the unevenness the relative mode exists to show.

**Padding SHALL be per tile and SHALL STOP AT THE TILE BORDER.** Each tile's band SHALL be
grown from the texels that tile's own UV content covered, and from nothing else. A
neighbouring tile's content SHALL NOT bleed across a seam, and a tile seam SHALL NOT be
extrapolated across as though it were island interior: at a seam the band continues the
island inside THIS tile, exactly as it does at the edge of the image anywhere else. The
radius, fill rule and texel count SHALL be reported per tile.

**A UDIM bake SHALL honour everything a single bake honours**: the projection cage, the map's
own encoding basis and value range, progress reporting over the whole set, and cooperative
cancellation — which SHALL be polled between tiles as well as inside one, and SHALL abandon
the whole set rather than return some tiles and not others.

#### Scenario: Occupied tiles are detected and reported before baking
- **WHEN** an EditMesh whose UV layout occupies tiles 1001, 1002 and 1011 is queried for its
  tiles
- **THEN** exactly those three numbers SHALL be reported, ascending, without a bake having run

#### Scenario: A mesh occupying three tiles costs three
- **WHEN** a UDIM bake runs on an EditMesh whose UV layout occupies three tiles of the ten by
  ten the numbering can address
- **THEN** exactly three images SHALL be produced, one per occupied tile, and no image SHALL
  be allocated for an unoccupied tile

#### Scenario: Occlusion crosses a tile boundary
- **GIVEN** a Target whose occluding geometry has its UVs in a DIFFERENT tile from the
  surface being shaded
- **WHEN** an ambient-occlusion map is baked for the tile holding the shaded surface
- **THEN** the occlusion from that geometry SHALL be present in the result
- **AND** that tile SHALL equal, texel for texel, the map an ORDINARY (non-UDIM) bake of the
  same mesh and parameters produces: the ordinary bake already casts against the whole Target
  and clips the layout to the unit square, so it is the reference for tile 1001, and the two
  part exactly when the UDIM path narrows what the rays can see

#### Scenario: A per-tile overflow and an aggregate overflow are distinct refusals
- **GIVEN** a host sets a texel ceiling
- **WHEN** a UDIM bake requests a width times height above that ceiling
- **THEN** the refusal SHALL name the PER-TILE ceiling, and no image SHALL be returned
- **WHEN** it instead requests a width times height that fits, over a tile count whose product
  with it does not
- **THEN** the refusal SHALL name the AGGREGATE ceiling, SHALL state the tile count, and no
  image SHALL be returned

#### Scenario: A bundle of maps is refused by the ceiling before it writes anything
- **GIVEN** a host sets a texel ceiling that one map of the preset's resolution fits under
- **WHEN** a UDIM export of a layout occupying more tiles than that ceiling allows is written
- **THEN** the export SHALL be refused naming the AGGREGATE ceiling, and no mesh and no map
  file SHALL have been written
- **WHEN** the same export runs with no ceiling configured
- **THEN** it SHALL write every tile

#### Scenario: The tile number reaches the output file name
- **WHEN** a UDIM export runs with a preset whose naming pattern carries the tile token
- **THEN** one file per occupied tile per map SHALL be written, each named with its own tile
  number, and the report SHALL record the tile beside each file

#### Scenario: An id map is the same colour in every tile
- **WHEN** a material-id map is baked over a layout where one material's faces are split
  across two tiles
- **THEN** that material's texels SHALL hold the SAME colour in both tiles
- **AND** the id-to-colour table reported with each tile SHALL be identical

#### Scenario: An object-position map decodes with one box across the set
- **WHEN** an object-position map is baked over a multi-tile layout
- **THEN** every tile SHALL report the same object-space bounds, and a point on the surface
  SHALL decode to the same coordinate whichever tile its texel lies in

#### Scenario: A UV density map reads the tile's resolution and the whole set's mean
- **GIVEN** a multi-tile layout whose islands are packed at different densities
- **WHEN** a UV density map is baked over it
- **THEN** each face's density SHALL be computed against the resolution of the TILE it lands
  in, so the same face reads the same whichever tile it is packed into and the same as a
  single map at that resolution
- **AND** in relative normalization every tile SHALL be divided by the mean of the WHOLE
  SET's defined texels, and SHALL report that one mean, so an island packed more densely
  than the rest reads above 1 and a sparser one below it rather than both reading as average

#### Scenario: A tile's padded band does not read the neighbouring tile
- **GIVEN** two islands holding clearly different values, one ending at a tile seam and the
  other beginning across it
- **WHEN** both tiles are baked with a non-zero padding radius
- **THEN** the band on each side of the seam SHALL continue its OWN tile's island
- **AND** no texel of either band SHALL hold the other tile's value

#### Scenario: UV coordinates outside the addressable grid are reported
- **WHEN** a UDIM bake runs on a layout whose faces include UVs that no `1001 + u + 10*v`
  tile can address
- **THEN** those faces SHALL be counted and reported alongside the tile list rather than
  silently omitted, and the addressable tiles SHALL bake normally

#### Scenario: Cancelling a UDIM bake abandons the whole set
- **WHEN** a UDIM bake over several tiles is cancelled after the first tile has been shaded
- **THEN** the call SHALL report the bake cancelled and SHALL NOT return a partial set of
  tiles as though it had succeeded

#### Scenario: A single-tile layout bakes identically to a non-UDIM bake
- **WHEN** a layout entirely inside the unit square is baked through the UDIM path and
  through the ordinary one with the same parameters
- **THEN** the UDIM path SHALL report exactly tile 1001 and its image SHALL equal the
  ordinary bake's texel for texel

### Requirement: Regioned baking with a bounded working set

A bake SHALL be producible in REGIONS: full-width horizontal bands of the output whose
finished rows are handed to a consumer in ascending order, each output row exactly once,
tiles of a UDIM set in ascending tile order. A regioned bake SHALL NOT hold the whole output
in memory at any point.

**Two bounds.** A request SHALL carry a WORKING-SET bound, in texels, separate from the
host's texel ceiling. The texel ceiling bounds the OUTPUT (see "A host can cap bake texel
allocation"); the working-set bound governs the texels of output image held IN FLIGHT at
once — one region plus its halo. Zero SHALL mean "no bound": the whole output is one region
and the result SHALL be bit-identical to an ordinary bake. The working-set bound SHALL NEVER
refuse a bake: a smaller bound SHALL produce more, smaller regions, and a bound too small to
hold even one row plus its halo SHALL be honoured as far as it can be — one-row regions —
with the working set actually held REPORTED so the host can see its bound was unreachable.
The Target-side data every bake builds once (the acceleration structure, the Target's
normals and curvature field) scales with the MESH, not the output, and SHALL NOT be counted
in the working set. The bound is stated in texels of OUTPUT image; the per-texel shading
frames and padding state of the region in flight are proportional to the same bound (not to
the output) and are not separately counted, so the process's memory SHALL scale with the
bound and the mesh and SHALL NOT grow with the output size under a fixed bound.

**Region boundaries SHALL be invisible.** The assembled output of a regioned bake SHALL be
identical, texel for texel, to the unregioned bake of the same request, for every map type,
at every region height, including the padded band and the recorded encoding. Two things make
this true, and both SHALL hold:

- Every texel SHALL be shaded from its GLOBAL image coordinates and from whole-mesh context
  only, so nothing a texel reads changes with the region it lands in; this includes the
  per-texel sample rotation the hemisphere maps use, which is keyed on the global texel.
- Every stage that reads a texel's NEIGHBOURHOOD SHALL run over a window that overlaps the
  region by a HALO of `max(2 * paddingRadius, derivative footprint)` rows above and below,
  where the derivative footprint is one texel. The padded band needs TWICE its radius, not
  its radius: continuing a gradient reads the covered neighbour and the texel beyond it, so a
  band texel `k` rings out depends on texels up to `2k` away. A screen-space derivative bake
  reads its immediate neighbour, which may lie in the next region; the one-texel footprint is
  stated so that such a bake inherits a correct overlap rather than a region-local
  derivative that shows as a line on every seam. No map in this engine takes a screen-space
  derivative today — curvature and cavity read the Target's curvature field at the cage hit —
  so the footprint is honoured by construction rather than exercised.

**Quantities normalized over the whole image SHALL be computed over the whole image.** A
value derived from a statistic of the whole output — a different number in every region if
taken per region, which assembles into a step in brightness at every seam while each region
looks internally perfect — SHALL be computed over the whole output before any region is
emitted. They are:

- the relative UV-density MEAN, over the whole image, and over the WHOLE SET for a UDIM set,
  as "UV density maps" requires;
- the padded band's COMPOUNDING LIMIT (the covered range widened by its own width, per
  channel), over the whole output image, as "Bake output padding across UV island borders"
  defines it;
- the auto CURVATURE RANGE of a field-sampled curvature or cavity map, which is a percentile
  over the image's sampled texels. It is the one quantity that needs every sample at once:
  a regioned bake SHALL hold one value per covered texel of the image for it, outside the
  working-set bound, unless the request sets an explicit curvature range.

The object-space bounds of an object-space position map, the auto curvature range of a
mesh-sampled curvature map, and an id map's id-to-colour table are whole-MESH quantities and
are unaffected by regioning.

**Scratch storage.** A regioned bake of more than one region MAY hold shaded, not yet padded
rows in scratch storage on disk between shading and assembly, so that each texel is shaded
exactly once; the scratch SHALL be removed on every exit — completion, refusal,
cancellation, failure — and a scratch write failure SHALL abandon the bake with a stated
failure rather than emit a partial map.

**Cancellation SHALL stop within a region.** A regioned bake SHALL poll cancellation inside
each region and inside each of its passes: at least every 2048 shaded texels on each worker,
every 1024 faces of a region's rasterization walk, between padding rings, before each band
of the finalize pass (one read, and possibly one rewrite, of a region's worth of scratch
rows), and before each region's assembly window is read. Its latency SHALL therefore be
bounded by the work of one of those units — proportional to the working set or the mesh,
and never to the output size or to the number of regions. A cancelled regioned bake SHALL
emit no further rows and SHALL report the cancellation.

**Progress SHALL stay smooth.** Progress SHALL be reported inside each region's shading at
the same texel step an ordinary bake uses, so a regioned bake does not step once per region.
Shading SHALL own `[0, 0.8]` of the bar; each image's finalize pass SHALL then report once
per band and its assembly once per region inside `[0.8, 1]`, so the bar also moves while the
scratch is re-read. It SHALL be monotone across a UDIM set.

Regioning SHALL be available for every map type, for an ordinary bake and for a UDIM set,
and SHALL honour the projection cage, the texel ceiling, progress reporting and cooperative
cancellation exactly as the unregioned bake does.

#### Scenario: A regioned bake equals the unregioned bake
- **WHEN** any map type is baked with a working-set bound that splits the output into several regions
- **THEN** the assembled rows SHALL equal the unregioned bake of the same request texel for texel, and the recorded encoding and padding report SHALL be the same

#### Scenario: A gradient crossing a region boundary has no discontinuity
- **GIVEN** a map whose values ramp smoothly down the image
- **WHEN** it is baked in regions whose boundary falls inside the ramp
- **THEN** the texel step across the boundary SHALL be the step the unregioned bake has there, with no discontinuity the unregioned bake lacks

#### Scenario: A padded band crossing a region boundary is seamless
- **GIVEN** an island whose padded band extends across a region boundary
- **WHEN** it is baked in regions with padding enabled
- **THEN** the band SHALL equal the unregioned band on both sides of the boundary

#### Scenario: A relative density map in regions divides by the whole map's mean
- **GIVEN** two islands of different density on opposite sides of a region boundary
- **WHEN** a relative UV-density map is baked in regions
- **THEN** every texel SHALL be divided by the mean over the whole map, and the recorded mean SHALL be that whole-map mean

#### Scenario: The working set is bounded while the output is not
- **WHEN** a map is baked with a working-set bound far smaller than its output
- **THEN** the texels of output image in flight SHALL never exceed the bound (or, when the bound is below one row plus its halo, the reported one-row working set)
- **AND** every output row SHALL arrive exactly once, in ascending order

#### Scenario: A large output under a small working set is produced, not refused
- **WHEN** 8192 x 8192 and 16384 x 16384 outputs of every map type are requested under a working-set bound that holds a small fraction of the output
- **THEN** each SHALL be produced in regions and SHALL NOT be refused
- **AND** a bound below one row plus its halo SHALL be reported as the working set actually held rather than refused

#### Scenario: A cancelled regioned bake stops inside a region
- **WHEN** cancellation is requested while the first region of a many-region bake is being shaded
- **THEN** the bake SHALL stop before that region's shading completes, SHALL emit no rows, and SHALL report the cancellation

#### Scenario: Cancellation is seen within one unit of every later pass
- **WHEN** cancellation is requested on the finalize pass's first band, or on the report that follows the first assembled region
- **THEN** the bake SHALL make no further progress report, SHALL hand no further row to the consumer, and SHALL report the cancellation

#### Scenario: The rasterization walk polls cancellation
- **WHEN** a region is rasterized from an EditMesh of N faces
- **THEN** cancellation SHALL be polled at least N / 1024 times before the region's first texel is shaded

#### Scenario: A process under a fixed bound does not grow with the output
- **WHEN** fully shaded maps of two output sizes, one four times the texels of the other, are baked under the same working-set bound
- **THEN** the larger bake's peak resident memory SHALL stay below half its output's float size and SHALL NOT grow by more than a quarter of the added output

#### Scenario: Progress is finer than one step per region
- **WHEN** a regioned bake reports progress
- **THEN** it SHALL report several increasing values inside each region's shading rather than one per region

### Requirement: A field evaluator's returns are validated at the boundary

A field evaluator is host-supplied code, so the bake SHALL validate what its
callbacks return rather than propagating it. The validation SHALL distinguish
two classes, separated by whether a CORRECT field can produce the value.

A CONTRACT VIOLATION — a NaN distance, a non-finite gradient, a non-finite
curvature, or an openness outside [0,1] beyond float tolerance — SHALL abandon
the whole bake and report which callback failed and where. It SHALL NOT be
sanitized into a usable value: a substituted default conceals the host's defect
and returns an image indistinguishable from a measured one.

A LEGITIMATELY UNDEFINED sample — an infinite distance, which is the ordinary
"nothing here" answer from a field covering a bounded region, or a zero-length
gradient, which is what a signed distance field has on its medial axis — SHALL
end the march for that texel only. That texel SHALL take the same neutral value
an un-hit cage ray produces, and such samples SHALL be counted on the result so
a host can see how much of its field the bake could not reach.

Callback out-params SHALL NOT be pre-seeded with a value that could pass
validation, so that a callback which returns without writing is detected rather
than read as a measurement.

#### Scenario: A broken callback fails the bake

- **WHEN** an evaluator returns a NaN distance, a non-finite gradient, or an
  openness far outside [0,1]
- **THEN** the bake SHALL fail, naming the callback, and SHALL produce no image

#### Scenario: An undefined sample is a counted miss

- **WHEN** an evaluator returns an infinite distance or a zero-length gradient
- **THEN** that texel SHALL take its neutral value, the sample SHALL be counted,
  and the bake SHALL succeed

#### Scenario: A well-behaved field is unaffected

- **WHEN** an evaluator honours its contract
- **THEN** the baked pixels SHALL be unchanged by the validation

### Requirement: Component link model
The system SHALL provide a component-link model over the EditMesh and the Target, where a component is a connected face set: explicit high→low links from an EditMesh component to one or more Target components, a per-component "bake alone" flag (the drawing-X gesture), and source resolution that returns a component's explicit links when it has any and otherwise the single nearest Target component. The model SHALL be serializable with the document's cage state.

The bake stage does NOT yet apply this model: every EditMesh component is baked against the whole Target, so an unlinked component, a linked one and one flagged to bake alone all produce the same maps today. Applying the resolved links to a bake — and exposing link editing through the C ABI and the bindings — is tracked in #103. Until then no map type SHALL claim link-restricted sampling, and a new map SHALL introduce no link-specific exception, so that when links are applied they reach every map on the same terms.

#### Scenario: An explicit link wins over the nearest surface
- **WHEN** an EditMesh component has an explicit link to a Target component that is not the nearest one
- **THEN** resolving its source SHALL return the linked component and report the resolution as explicit

#### Scenario: An unlinked component falls back to the nearest Target component
- **WHEN** an EditMesh component has no explicit link
- **THEN** resolving its source SHALL return the single nearest Target component and report the resolution as the fallback

#### Scenario: A bake does not yet honour links
- **WHEN** a bake runs on an EditMesh whose components carry explicit links
- **THEN** every component SHALL be baked against the whole Target, exactly as if no links existed, and nothing in the bake's output or report SHALL suggest otherwise

