# Design — UDIM-aware baking

## The decision the whole change turns on: one acceleration structure

#91 names it and it is worth restating because the wrong version costs nothing to write and
is invisible afterwards. The obvious shape of this feature is:

```
for each occupied tile:
    faces = faces whose UVs lie in this tile
    bvh   = Bvh(faces)
    bake into this tile's image
```

Every map it produces looks right. Ambient occlusion, bent normal and thickness are the
three that are wrong, and they are wrong in a way that reads as an artistic choice: an arm
that shadows a torso simply stops shadowing it once the two are packed into different
tiles. There is no visual signature. There is no assertion in an ordinary bake test that
fails.

So the structure is built ONCE, over the whole Target, and only the set of texels written
varies between tiles. That is also the cheaper implementation — a BVH build plus a
`flatten()` copy per tile is linear in the Target's triangle count times the tile count, on
the one axis a bake is supposed to be flat in — so there is no tension between the correct
version and the fast one.

Concretely, a `BakeContext` is assembled once and every tile is shaded against it:

| gathered once | why it cannot be per tile |
| --- | --- |
| `Bvh` + `FlatBvh` | the requirement above |
| Target vertex normals | a property of the Target, not of a tile |
| the Target's curvature field and its auto range | an auto range is a percentile over the whole Target; per tile, the same crease would saturate differently in each |
| the object-space bounds | see "Encoding metadata" below |
| the Target's id column and colour table | see "Encoding metadata" below |
| the EditMesh's vertex normals | a property of the EditMesh |

The only thing that varies per tile is a `Vec2` subtracted from every UV before
rasterization.

## Tile detection: exact overlap, not a bounding box

A tile is occupied when a triangle of the UV layout overlaps it. The cheap version — mark
every tile a triangle's UV bounding box touches — over-reports for a triangle that reaches
diagonally around a tile corner, and #91 makes over-reporting a correctness problem rather
than a performance one: "a mesh occupying three tiles out of a possible hundred must cost
three", and every reported tile is an allocated image.

So occupancy uses the separating-axis test for a triangle against an axis-aligned square:
five axes (the two box axes and the three edge normals), a projection interval on each, and
an overlap on all five. Exact, branch-light, and it reads only the triangle's own
coordinates — no container iteration order anywhere near it, which matters in this
repository (the solver's libc++/libstdc++ divergence came from exactly that).

The candidate tiles come from the triangle's UV bounding box, so the test runs a handful of
times per triangle rather than once per addressable tile.

**Rejected: rasterize and see which texels land.** It is the most faithful definition — a
tile is occupied when the bake would write a texel into it — but it makes occupancy
resolution-dependent (a sliver that covers no texel centre at 512 covers one at 4096) and
it costs a full rasterization of every candidate tile just to answer "which tiles?", which
is the question a host asks BEFORE it agrees to pay for a bake.

## The addressable grid

`1001 + u + 10*v` is base-10 in `u`, so only `u` in `[0, 9]` has a tile number. A negative
`v` has none either. The upper bound on `v` is a choice: the numbering itself is unbounded,
but a layout with `v = 10^6` is a mistake in the file rather than a request for a million
images. The grid is bounded at `v <= 999`, i.e. tiles 1001..10990, which is past every
convention in use, and coordinates outside it are COUNTED and reported.

Counting rather than dropping is the point. A layout authored in a convention this
numbering cannot address — Mudbox's `u1_v1` form, or UVs in `[-1, 0]` — would otherwise
produce a bake with a quietly missing region, which is the exact failure mode `mesh-io`'s
"loud failure semantics" exists to rule out.

## The two ceilings

`BakeParams::maxPixels` is a ceiling on texels. A UDIM set has two meaningful readings of
it and #91 requires both:

- **per tile** — `width * height > maxPixels`. One tile is bigger than the host will
  allocate. The fix is a smaller resolution.
- **aggregate** — `width * height * tileCount > maxPixels` while one tile fits. Each tile
  fits; the set does not. The fix is a smaller resolution OR a layout using fewer tiles OR a
  bigger budget — a different decision from the first, made with different information.

They are reported as two `UdimRefusal` values with two messages. The per-tile check runs
first and wins when both trip, because it is the more specific statement: if one tile is
already too big, "and there are four of them" is not the thing to tell the caller.

The aggregate product is computed with an overflow guard (`tiles > max / (w*h)`) rather
than by multiplying and hoping, since `width * height * tileCount` is exactly the kind of
product that wraps.

## Encoding metadata is the whole set's, not the tile's

Three of the recorded encodings are derived from the mesh rather than from a texel, and for
each the per-tile answer is wrong:

- **Object-space bounds.** `ObjectPosition` writes `(p - min) / (max - min)`. Per-tile
  bounds make the same 3D point decode to different coordinates depending on which tile its
  texel landed in — the encoding record exists so a consumer can decode, and a decode that
  depends on the tile is not one. The bounds are the union over the EditMesh and the Target,
  computed once.
- **The id table.** Per-tile tables would renumber nothing (the colour is a pure function of
  the id) but would LIST different ids per tile, so a consumer resolving a picked colour in
  tile 1002 could fail to find an id that tile 1001's table has. Both the colour and the
  table are whole-Target, so a host's saved selection survives crossing a tile.
- **The relative density mean.** Already fixed by the forward constraint the `uv-editing`
  and `surface-baking` specs recorded when #89 landed: the mean is over the whole set. A
  per-tile mean reports every tile as average, which hides exactly what the relative mode
  exists to show.

The mean is the one that forces a restructuring. Normalization has to happen after EVERY
tile has been shaded and BEFORE any tile is padded (padding continues the values finally
written). So the per-tile pipeline splits into:

```
per tile:  rasterize -> allocate -> shade -> keep the covered coordinates
whole set: accumulate the density mean -> apply it to every tile
per tile:  pad -> report
```

Keeping the covered COORDINATES (two ints) rather than the full `Texel` (position, normal,
tangent, bitangent, ratio — about sixty bytes) between the two per-tile phases is what
keeps this affordable: a 2048² tile holds at most 4M texels, so 32 MB per tile of
coordinates against 250 MB of texels.

The single-tile `bake()` takes the same pipeline with one tile at origin `(0, 0)` and an
unchanged result: subtracting `0.0f` from a UV is the identity in IEEE 754, so the
rasterization is bit-identical to what it was.

## Padding at a tile seam

Each tile is its own image with its own coverage set. A texel just across a seam belongs to
the next tile's image and is not in this tile's coverage, so the band at a seam is grown
from THIS tile's island alone — which is exactly the behaviour at the edge of the image
anywhere else, and it falls out of the per-tile decomposition rather than needing a rule.

Both failure modes #91 warns about are therefore structurally impossible rather than
merely avoided: no neighbouring tile's value can be read (it is in a different buffer), and
a seam cannot be extrapolated across as island interior (the extrapolation only ever reads
covered texels, and across the seam there are none).

The scenario still asserts it, because "structurally impossible" is a statement about
today's structure.

## `{udim}` in the naming pattern

`presetMapFileName()` gains a tile argument that expands a `{udim}` token. A non-UDIM
export passes 1001, because the unit square IS tile 1001 — so one preset works for both and
`{basename}_{map}.{udim}.{ext}` means the same thing either way.

The refusal that matters: a multi-tile export through a pattern WITHOUT `{udim}` writes
every tile to one path, each overwriting the last, while the report lists all of them. The
bundle already refuses two MAPS that expand to one name for the same reason; this is the
same failure one axis over, and it is refused with a message naming the pattern.

**Rejected: appending the tile automatically when the pattern lacks the token.** It would
silently change the file names a non-UDIM export has always produced the moment a mesh
gains a second tile, which is a worse surprise than a refusal.

## The C ABI shape

Tile detection and baking are separate entry points, because "what am I about to allocate?"
is a question a host asks before it commits:

```c
CyberStatus cyber_udim_tiles(const CyberMesh* low, int* out_tiles, size_t max, size_t* count);
CyberStatus cyber_bake_udim(low, high, map, params, CyberUdimBake** out);
size_t      cyber_udim_bake_count(const CyberUdimBake*);
CyberStatus cyber_udim_bake_tile(const CyberUdimBake*, size_t index, int* out_tile);
CyberStatus cyber_udim_bake_image(const CyberUdimBake*, size_t index, CyberImage** out);
void        cyber_udim_bake_free(CyberUdimBake*);
```

A handle rather than a caller-allocated array of images: the tile count is not knowable
before the call, the images are already opaque (`CyberImage`), and the alternative — bake,
then ask how many, then bake again — repeats the expensive half.

`cyber_udim_bake_image` hands out a fresh `CyberImage` the caller frees, which keeps the
existing `cyber_image_*` accessors working unchanged on it: encoding, padding, density,
placement and the id table are all per-image and all already have readers.

The two ceiling refusals both map to `CYBER_ERR_RUNTIME`, which is what the single-map
ceiling already returns, and they are told apart by `cyber_last_error()` — the same way
every other diagnostic in this ABI is.
