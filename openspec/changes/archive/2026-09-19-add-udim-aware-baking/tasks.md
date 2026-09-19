## 1. Tile detection

- [x] 1.1 `UdimTile { u, v, number }` and `udimTileNumber(u, v)` = `1001 + u + 10*v` in
      `bake.hpp`, with the addressable grid (`u` in `[0,9]`, `v` in `[0,999]`) stated.
- [x] 1.2 `UdimLayout udimTiles(const Mesh&)`: the occupied tiles ascending by number plus
      the count of faces outside the addressable grid, using exact triangle-vs-square
      overlap and touching no unordered container.
- [x] 1.3 A packed (unit-square) layout reports exactly tile 1001.

## 2. Bake core

- [x] 2.1 Split `bake()` into a `BakeContext` built once (EditMesh normals, BVH, flattened
      BVH, Target normals, object bounds, id field, raster sources) and a per-tile shade.
- [x] 2.2 The rasterizer takes a tile origin and subtracts it from every UV; origin `(0,0)`
      is bit-identical to today.
- [x] 2.3 Density normalization splits into an accumulate pass and an apply pass, so a UDIM
      set divides by the mean of the WHOLE set, before padding.
- [x] 2.4 `bakeUdim()` returns one `BakeResult` per occupied tile, sharing one context, with
      progress over the whole set and cancellation polled between tiles.
- [x] 2.5 `UdimRefusal { None, Parameters, NoOccupiedTiles, PerTileCeiling, AggregateCeiling,
      FieldContract }`
      with a message per refusal; the per-tile check runs first and the aggregate product is
      overflow-guarded.
- [x] 2.6 `bake()` reimplemented on the shared pipeline with an unchanged result.

## 3. Presets and the export bundle

- [x] 3.1 `{udim}` token in `presetMapFileName()`, expanding to the tile number and to 1001
      for a non-UDIM export.
- [x] 3.2 `BundleParams::udim`; `writeBundle` bakes the set once per map and writes one file
      per tile, recording the tile on each `BundleFile`.
- [x] 3.3 A multi-tile bundle through a pattern without `{udim}` is refused, naming the
      pattern.

## 4. C ABI (1.24, additive)

- [x] 4.1 `cyber_udim_tiles`.
- [x] 4.2 `CyberUdimBake` with `cyber_bake_udim`, `cyber_udim_bake_count`,
      `cyber_udim_bake_tile`, `cyber_udim_bake_image`, `cyber_udim_bake_free`.
- [x] 4.3 The same parameter validation `cyber_bake` applies, and the two ceiling refusals
      distinguishable through `cyber_last_error()`.
- [x] 4.4 ABI minor 1.23 -> 1.24, `CyberBundleParams::udim`,
      `cyber_bundle_result_file_udim_tile`, and the regenerated
      `capi/abi/cyber_capi-1.24.json` pinned by the manifest test.

## 5. CLI and bindings

- [x] 5.1 CLI `--udim`, the detected tile list printed before baking, and the tile number on
      each map row of the JSON report.
- [x] 5.2 Python: `udim_tiles()`, `bake_udim()` and the bundle's `udim` flag.
- [x] 5.3 Swift: the same surface, with the binding-parity gate green.

## 6. Tests

- [x] 6.1 Tile detection: three occupied tiles of a hundred, ascending order, exact overlap,
      unaddressable coordinates counted.
- [x] 6.2 Cost: a three-tile layout produces exactly three images and no more.
- [x] 6.3 **Cross-tile occlusion**: an occluder whose UVs lie in another tile still occludes,
      and the map matches the single-tile arrangement.
- [x] 6.4 Per-tile and aggregate ceiling refusals, each naming its own ceiling.
- [x] 6.5 Id-colour consistency across tiles, and one identical table per tile.
- [x] 6.6 Object-position bounds identical across tiles.
- [x] 6.7 Relative density divides by the whole set's mean.
- [x] 6.8 A tile's padded band does not hold the neighbouring tile's value.
- [x] 6.9 A single-tile layout through the UDIM path equals the ordinary bake texel for
      texel.
- [x] 6.10 Cancellation abandons the whole set.
- [x] 6.11 `{udim}` naming: expansion per tile, 1001 for a non-UDIM export, and the refusal
      when a multi-tile export omits the token.
- [x] 6.12 C ABI coverage of the tile query, the handle and the two refusals; CLI coverage
      of `--udim`.

## 7. Docs

- [x] 7.1 `CHANGELOG.md` under `## [Unreleased]`.
- [x] 7.2 `README.md` UDIM section.
