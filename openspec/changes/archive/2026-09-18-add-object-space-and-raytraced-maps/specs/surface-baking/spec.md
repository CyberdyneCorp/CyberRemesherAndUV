## MODIFIED Requirements

### Requirement: Bakeable map types
The bake stage SHALL bake from the Target onto the EditMesh's UV layout: tangent-space normal maps, ambient occlusion, displacement/height, color maps (from Target vertex colors, including polypaint, or from a Target texture when the Target has its own UVs — texture-to-texture baking), object-space normal maps, object-space position maps, bent normal maps and thickness maps. Output resolution SHALL be user-selectable up to at least 4096².

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

## ADDED Requirements

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
Every bake SHALL report, with its output image, the basis needed to interpret the numbers in it: whether the values are raw, a direction in the texel's tangent frame, a direction in object space, a position rescaled over a bounding box, or a distance in model units; the up axis a direction or position was expressed in; the bounding box a position was rescaled over; and the factor a distance was multiplied by.

That record SHALL be reachable from every entry point that produces a map — the C ABI, the export-bundle result, the language bindings — and SHALL appear in the run's machine-readable report alongside the file it describes.

#### Scenario: A position map can be decoded
- **WHEN** a consumer reads an object-space position map together with its recorded basis
- **THEN** it SHALL be able to recover the model-unit coordinate of any texel from the recorded bounding box and up axis

#### Scenario: A thickness map carries its scale
- **WHEN** a thickness map is baked with a non-default scale factor
- **THEN** the recorded basis SHALL name that factor, so the underlying distance is recoverable

#### Scenario: The report names the basis of every written map
- **WHEN** an export bundle writes its maps and a machine-readable report
- **THEN** each map's entry in the report SHALL carry its encoding basis, and the object-space entries SHALL carry their up axis and bounding box
