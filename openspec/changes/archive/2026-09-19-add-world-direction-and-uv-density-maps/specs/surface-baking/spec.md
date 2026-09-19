## ADDED Requirements

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
arrives through. The placement SHALL be checked only for a map that reads it, so a bake
that worked before still works.

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

**The value range a UV density map's encoding guarantees SHALL be `[0, +infinity)`** — a
density is never negative, and the ratio of texels to surface area has no upper bound. It
is explicitly NOT `[0,1]` in either normalization mode. Its padded band SHALL therefore be
extrapolated and SHALL NOT be renormalized — there is no unit length to restore — and
SHALL be confined by that range and by the compounding limit of "Bake output padding
across UV island borders".

**UDIM (forward constraint).** UDIM-aware baking does not exist yet; this states what it
must preserve when it arrives, and asserts nothing about today's behaviour. A face SHALL
have its density computed against the resolution of the TILE it lands in, so that a set of
tiles baked at one resolution reports the same density as a single map at that resolution;
and the relative mean SHALL be taken over the WHOLE SET of tiles rather than per tile,
because a per-tile mean would report every tile as average and hide exactly the unevenness
the relative mode exists to show.

UV density SHALL follow the same rules as every other map type: the same projection cage,
output resolution, texel ceiling, progress reporting, cooperative cancellation and border
padding, and SHALL be requestable through every entry point the other maps are.

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

#### Scenario: A zero-area UV face takes the sentinel and does not poison the mean
- **WHEN** a UV density map is baked in relative mode over a layout containing a face with zero UV area or zero surface area
- **THEN** every texel of that face SHALL hold exactly zero, no texel of the map SHALL hold an infinity or a NaN, and the reported mean SHALL be the mean of the DEFINED texels alone — the same mean the map would report without that face

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

## MODIFIED Requirements

### Requirement: Bakeable map types
The bake stage SHALL bake from the Target onto the EditMesh's UV layout: tangent-space normal maps, ambient occlusion, displacement/height, color maps (from Target vertex colors, including polypaint, or from a Target texture when the Target has its own UVs — texture-to-texture baking), object-space normal maps, object-space position maps, world-space direction maps, bent normal maps, thickness maps, UV density maps, material ID maps and object ID maps. Output resolution SHALL be user-selectable up to at least 4096².

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
