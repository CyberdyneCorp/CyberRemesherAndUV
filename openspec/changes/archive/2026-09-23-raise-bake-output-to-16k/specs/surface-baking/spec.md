## ADDED Requirements

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
in the working set.

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
each region: at least every 2048 shaded texels on each worker, every 1024 rasterized faces,
every scratch row read or written, and between padding rings. Its latency SHALL therefore be
bounded by the work of one of those units — proportional to the working set or the mesh,
and never to the output size or to the number of regions. A cancelled regioned bake SHALL
emit no further rows and SHALL report the cancellation.

**Progress SHALL stay smooth.** Progress SHALL be reported inside each region's shading at
the same texel step an ordinary bake uses, so a regioned bake does not step once per region.

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

#### Scenario: Progress is finer than one step per region
- **WHEN** a regioned bake reports progress
- **THEN** it SHALL report several increasing values inside each region's shading rather than one per region

## MODIFIED Requirements

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
cage projection, component links, output resolution up to 16384² (through the regioned path where the output exceeds the working set),
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
