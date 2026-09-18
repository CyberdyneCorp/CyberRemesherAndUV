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

### Requirement: Editable bake cage
The bake SHALL use a projection cage derived from the EditMesh, editable with the core actions: Tweak adjusts cage distance with brush falloff and double-tap sets per-vertex cage distance, Relax smooths the cage, Erase resets edited regions to default. Cage state SHALL persist in the document.

#### Scenario: Per-vertex cage distance
- **WHEN** the user double-taps a cage vertex and enters a distance
- **THEN** that vertex's cage offset SHALL change independently of the brush falloff

### Requirement: Component links and selective baking
When Target and EditMesh have multiple components, the user SHALL be able to draw explicit high→low component links so each EditMesh component bakes only from its linked Target components; drawing an X over a component SHALL bake that component alone. Unlinked components SHALL use nearest-surface matching by default.

#### Scenario: Linked components do not bleed
- **WHEN** two overlapping Target components are linked to distinct EditMesh components
- **THEN** each EditMesh component's maps SHALL contain only its linked source's detail

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
cage projection, component links, output resolution up to at least 4096²,
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
texels. It SHALL reject a request whose `width * height` exceeds that ceiling
before UV rasterization or output image allocation. Zero SHALL disable the
ceiling, and an overflowed texel product SHALL be rejected.

#### Scenario: Requested bake is over budget

- **GIVEN** a host sets a texel ceiling below the requested width times height
- **WHEN** it starts a bake
- **THEN** the operation SHALL fail with a diagnostic naming the request and ceiling
- **AND** no output image SHALL be returned

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

