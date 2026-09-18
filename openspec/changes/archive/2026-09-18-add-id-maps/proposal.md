## Why

A colour-ID map is how an artist selects "the leather strap" without masking it by
hand, and it is what CyberTexel's Colour ID selection and polygon-fill tools read
(epic #86, issue #88). Material and object identity are known HERE, at bake time,
and nowhere downstream: `surface-baking` has no map that carries them, and no other
stage of the pipeline could produce one.

The value of such a map is **exact colour comparison at zero tolerance**. Two things
destroy it silently: anti-aliasing or resampling the boundaries, and deriving the
colours from anything that iterates an unordered container — this repository's solver
already differs between libc++ and libstdc++ for exactly that reason. Both have to be
ruled out by construction, not by care.

## What Changes

- Two new `BakeMap` values: `MaterialId` and `ObjectId`, requestable through every
  entry point the existing maps are — the C ABI, the export presets, the CLI's
  `--bake`, Python and Swift — honouring the same cage, component links, texel
  ceiling, progress reporting and cancellation.
- **A stated id source per map.** `MaterialId` reads the Target's face-domain int32
  `material_id` column; `ObjectId` reads `object_id`, then `group_id`, and falls back
  to the Target's face-connected components when no column declares one. The source
  actually used is reported.
- **A stated colour rule.** The colour is a 24-bit integer avalanche hash of the id,
  lifted so every channel lands in `[64, 255]`, quantised onto the 8-bit lattice.
  Integer-only, so it is bit-identical across runs, platforms and standard libraries;
  `(0,0,0)` is reserved for "no id" and no assigned colour can collide with it.
- **The id-to-colour mapping is REPORTED**, not merely baked into pixels: the bake
  returns a table, ordered ascending by id, of every id on the Target and the exact
  8-bit colour written for it. It reaches a host through `cyber_image_id_color*`,
  `cyber_bundle_result_file_id_color*`, both bindings, and the CLI's JSON report.
- **The output is written without anything that would perturb an exact comparison.**
  The bake point-samples with no anti-aliasing; the PNG writer is lossless; an id map
  is never sRGB-encoded even when a preset asks for it (that is reported as a warning,
  matching how the bundle already handles sRGB-into-EXR).
- A new `EncodingBasis::IdColor`, so a consumer learns from the recorded basis alone
  that these texels are exact keys rather than filterable values.
- ABI 1.20, additive: two `CyberBakeMap` values, one `CyberEncodingBasis` value,
  `CyberIdColor` and six accessors.

## Capabilities

### Modified Capabilities

- `surface-baking`: the bakeable map set gains material ID and object ID; a new
  requirement fixes the id source, the colour rule, the reported table and the
  no-filtering guarantee; the encoding-basis requirement gains the id table.

## Impact

- `src/bake/` — `BakeMap`, `BakeEncoding`, the rasterized shading path, neutral
  padding, id collection.
- `src/core/export_preset.*`, `src/exportbundle/` — two preset map names, the sRGB
  refusal for id maps.
- `capi/` — ABI 1.20 (`capi/abi/cyber_capi-1.20.json`), new enum values, `CyberIdColor`
  and its accessors.
- `apps/cli/` — `--bake material-id,object-id`, the `idColors` block in the JSON report.
- `python/`, `swift/` — the new map values, `Image.id_colors` / `id_source` and the
  bundle-file equivalents; both parity gates.
- `tests/bake/test_id_maps.cpp` (new), `tests/capi`, `tests/exportbundle`, CHANGELOG,
  README/docs map vocabulary.
