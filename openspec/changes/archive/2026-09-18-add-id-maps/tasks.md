## 1. Core bake

- [x] 1.1 `BakeMap::MaterialId` and `BakeMap::ObjectId`, appended so every existing
      enum value keeps its number.
- [x] 1.2 The id colour rule: integer avalanche of the id, each channel lifted into
      `[64, 255]`, quantised onto the 8-bit lattice, with `(0,0,0)` reserved and
      unreachable. No floating point on the path from id to byte.
- [x] 1.3 Id source resolution: `material_id` for the material map; `object_id`, then
      `group_id`, then the Target's face-connected components for the object map.
      The components fallback runs only when no column declared one.
- [x] 1.4 `EncodingBasis::IdColor`, plus `BakeEncoding::idSource` and
      `BakeEncoding::idColors` (every distinct Target id, ascending), filled for the
      two id maps and left empty for every other map.
- [x] 1.5 Shading on the existing cage-projection path: the hit face's id, no
      interpolation, no anti-aliasing; a cage miss writes the reserved "no id".
- [x] 1.6 `neutralPadding()` returns the reserved "no id" for both maps.

## 2. Entry points

- [x] 2.1 C ABI: `CYBER_BAKE_MATERIAL_ID` / `CYBER_BAKE_OBJECT_ID`,
      `CYBER_ENCODING_ID_COLOR`, `CyberIdColor`, `cyber_image_id_source` /
      `cyber_image_id_color_count` / `cyber_image_id_color` and the three bundle
      equivalents. ABI 1.20 manifest pinned, additive against 1.19.
- [x] 2.2 Export presets: `material-id` and `object-id` map names.
- [x] 2.3 Export bundle: an id map declared sRGB is written verbatim with a warning.
- [x] 2.4 CLI: the two names in `--bake` and its help; the id source and table in the
      JSON report's `outputs` entries.
- [x] 2.5 Python: the `BakeMap` values, `EncodingBasis.ID_COLOR`, `IdColor`,
      `ImageEncoding.id_source` / `id_colors`, reaching both `Image` and `BundleFile`.
- [x] 2.6 Swift: the same, and both binding-parity gates green.

## 3. Tests

- [x] 3.1 One flat colour per material id; two ids give two colours; no third colour
      appears over the covered area.
- [x] 3.2 A Target with no id column but two disconnected parts separates through the
      component fallback, and the reported source names it.
- [x] 3.3 A declared `object_id` column assigning one id to two disconnected parts
      beats the fallback: both parts take the same colour.
- [x] 3.4 Determinism: the same bake twice is identical texel for texel and table for
      table; the colour of an id does not depend on which other ids are present, on
      the order faces carry them, or on how many there are.
- [x] 3.5 The colour rule is exactly representable: every assigned colour survives a
      PNG round-trip byte for byte, and no assigned colour is the reserved `(0,0,0)`.
- [x] 3.6 A boundary texel holds one id's exact colour or the other's, never a blend.
- [x] 3.7 The reported table resolves every covered texel's colour back to exactly one
      id, and that id is the one under the texel.
- [x] 3.8 Cage, texel ceiling, missing-UV rejection and cancellation behave as they do
      for every other map; an unreached cage writes the reserved "no id".
- [x] 3.9 The table and source reach the C ABI, the export bundle result and the CLI
      report; an id map declared sRGB is written verbatim with a warning.

## 4. Documentation

- [x] 4.1 CHANGELOG under `## [Unreleased]`.
- [x] 4.2 README / docs map list and preset vocabulary.
