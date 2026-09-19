## Why

`grep -ri udim openspec/specs` returned nothing in this repository before this change, and
`uv-editing` packs into a single unit square. UDIM is how a production asset with more than
one texture set's worth of detail is laid out — it is the layout a character or a vehicle
actually ships with — and
[CyberTexel](https://github.com/CyberdyneCorp/CyberTexel) supports it throughout
(`mesh-and-texture-sets`: tiles allocated on demand, strokes crossing tile borders, per-tile
export). Epic #86 makes this repository its baker. A baker that can only write the unit
square makes the whole seam a desktop demo (#91).

There is one requirement in #91 that decides whether the feature is correct or merely
plausible, and it is not the file naming. **Rays cast for ambient occlusion, bent normal and
thickness must see the WHOLE mesh, not only the tile being written.** The obvious
implementation — loop over tiles, build an acceleration structure from that tile's faces,
bake — produces occlusion that looks entirely reasonable and is wrong: an arm that occludes
a torso stops occluding it the moment the two are packed into different tiles. Nothing about
the output reveals it. So the acceleration structure is built ONCE over the whole Target and
only the set of texels written varies, and a scenario asserts occlusion across a tile
boundary.

## What Changes

- **Tile detection.** `udimTiles()` reports the occupied tiles of an EditMesh's UV layout
  under the standard `1001 + u + 10*v` numbering, ascending by tile number, using an exact
  triangle-vs-tile overlap test rather than a bounding box — a conservative box would
  allocate tiles the layout does not reach and break the cost guarantee below. UVs outside
  the addressable grid are COUNTED and reported, never silently dropped.
- **One output per occupied tile, per map type.** `bakeUdim()` returns one image per
  occupied tile. It allocates for OCCUPIED TILES ONLY: a mesh occupying three tiles of a
  possible hundred costs three.
- **One acceleration structure for the whole set.** The BVH, its flattened image, the Target
  normals, the Target's curvature field, the object-space bounds and the id table are built
  ONCE and shared by every tile. Rays therefore see the whole mesh by construction.
- **The texel ceiling applies per tile AND in aggregate**, and a refusal names WHICH of the
  two it hit. These are two distinct diagnostics, not one.
- **Whole-set encoding metadata.** The object-space bounds an object-position map rescales
  over are the WHOLE MESH's, identical in every tile, so one decode works across the set;
  the id-to-colour table is the whole Target's, so the same material is the same colour in
  tile 1001 and in tile 1002; and a relative UV-density map divides by the mean of the whole
  set, which is the forward constraint the `uv-editing`/`surface-baking` specs already
  recorded against this change.
- **Padding is per tile and stops at the tile border.** A tile's band is grown from that
  tile's own covered texels alone. A neighbouring tile's content cannot bleed across a seam,
  and a seam is not extrapolated across as though it were island interior.
- **`{udim}` in the export-preset naming pattern.** The tile number reaches `mesh-io`'s
  filename pattern; a non-UDIM export expands it to `1001`, which is what the unit square
  is. A UDIM export whose pattern omits the token is REFUSED, because every tile would
  overwrite the previous one.
- **Every entry point that supplies its own low-poly.** The C ABI (`cyber_udim_tiles`,
  `cyber_bake_udim` and the result handle), the export bundle and the Python and Swift
  bindings, each validating the new parameters identically. The CLI reports the tile list
  and the per-file tile but takes no `--udim` flag: it always bakes onto a freshly remeshed,
  freshly unwrapped low-poly, which the packer puts in the unit square, so it is the
  tile-1001 case by construction.
- **A note in `uv-editing`** stating how packing interacts with tiles: the packer targets
  the unit square, which is tile 1001, and a multi-tile layout is authored or imported
  rather than produced by it.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `surface-baking`: adds UDIM-aware baking — tile detection and reporting, one output per
  occupied tile, the whole-mesh acceleration structure, the per-tile and aggregate texel
  ceilings with distinct refusals, the whole-set encoding metadata, and the per-tile padding
  rule at a tile border.
- `mesh-io`: the `{udim}` naming-pattern token and the refusal when a multi-tile export
  omits it.
- `uv-editing`: a note stating that the packer targets tile 1001 and how a multi-tile layout
  arrives.

## Impact

- `src/bake/`: `bake.hpp` (`UdimTile`, `udimTiles()`, `UdimBakeResult`, `UdimRefusal`,
  `bakeUdim()`), `bake.cpp` (the shared per-tile pipeline the single-tile `bake()` now also
  takes, the tile rasterization offset, the deferred density normalization).
- `capi/`: ABI 1.24 — `cyber_udim_tiles`, `cyber_bake_udim`, the `CyberUdimBake` handle and
  its accessors.
- `src/core/export_preset.*`: the `{udim}` token.
- `src/exportbundle/`: `BundleParams::udim`, one file per tile per map, the tile in the
  report row.
- `apps/cli/`: the detected-tile list and the per-file tile number in the JSON report.
- `python/`, `swift/`: the same surface.
- Docs: `CHANGELOG.md`, `README.md`.
