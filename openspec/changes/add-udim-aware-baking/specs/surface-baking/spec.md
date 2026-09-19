## ADDED Requirements

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
- **AND** the map SHALL equal the one baked with both parts in a single tile, to within the
  tolerance of the shared sampling

#### Scenario: A per-tile overflow and an aggregate overflow are distinct refusals
- **GIVEN** a host sets a texel ceiling
- **WHEN** a UDIM bake requests a width times height above that ceiling
- **THEN** the refusal SHALL name the PER-TILE ceiling, and no image SHALL be returned
- **WHEN** it instead requests a width times height that fits, over a tile count whose product
  with it does not
- **THEN** the refusal SHALL name the AGGREGATE ceiling, SHALL state the tile count, and no
  image SHALL be returned

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
