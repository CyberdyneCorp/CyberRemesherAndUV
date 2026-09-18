## 1. Core bake

- [x] 1.1 `BakeMap::ObjectNormal`, `ObjectPosition`, `BentNormal`, `Thickness`,
      appended so the existing enum values keep their numbers.
- [x] 1.2 `BakeParams::upAxis`, `bentNormalSpace`, `thicknessScale`, each with a
      stated default and range, checked in `paramsUsable` only for the maps that
      read them.
- [x] 1.3 `BakeEncoding` on `BakeResult`, filled for EVERY map: basis, up axis,
      bounds, scale.
- [x] 1.4 Bake bounds: union of both meshes' live vertices, accumulated in the
      requested up-axis convention; zero-extent axis encodes to 0.5.
- [x] 1.5 Object-space normal and position on the existing cage-projection path,
      with the low-poly fallback.
- [x] 1.6 `gatherHemisphere()`: one cosine-weighted batch serving AO's occluded
      count, the bent normal's unoccluded-direction sum and thickness's
      back-facing depth sum. AO output stays bit-identical.
- [x] 1.7 Ray-traced shading pass for AO / bent normal / thickness, with the
      per-texel parallelism, cancellation and neutral padding the AO pass had.
- [x] 1.8 Progress reported from the texel loop on a 1% step, serialised behind
      the bake's own mutex, still ONE `parallelFor` range.

## 2. Entry points

- [x] 2.1 C ABI: four `CyberBakeMap` values, three appended `CyberBakeParams`
      members, `CyberUpAxis` / `CyberBentNormalSpace` / `CyberEncodingBasis` /
      `CyberImageEncoding`, `cyber_image_encoding`,
      `cyber_bundle_result_file_encoding`; parameter validation naming the
      offender. ABI 1.19 manifest pinned, additive against 1.18.
- [x] 2.2 Export presets: `object-normal`, `object-position`, `bent-normal`,
      `thickness` map names; the bundle takes its up axis from the preset's
      declared `upAxis` and records each written map's basis.
- [x] 2.3 CLI: the new names in `--bake` and its help, `--thickness-scale` and
      `--bent-normal-space`, and the encoding basis in the JSON report's
      `outputs` entries.
- [x] 2.4 Python: `BakeMap` values, `BakeParams` fields, `Image.encoding`,
      `BundleFile.encoding`.
- [x] 2.5 Swift: the same, and both parity gates green.

## 3. Tests

- [x] 3.1 Object-space normal: values are the surface normal encoded; a z-up bake
      is the y-up bake re-expressed.
- [x] 3.2 Object-space position: spans [0,1], monotone across the layout,
      decodable from the recorded bounds; flat Target gives 0.5 and no NaN.
- [x] 3.3 `BakeMap::Position` still writes model units (regression against the
      redefinition this change refused).
- [x] 3.4 Bent normal leans away from an occluder; fully enclosed texels fall
      back to the surface normal; tangent vs object space differ as documented.
- [x] 3.5 Thickness on a solid is positive and tracks the material depth and the
      scale factor; a THIN DOUBLE-SIDED Target reads near zero.
- [x] 3.6 AO openness is bit-identical to the pre-change bake.
- [x] 3.7 The ray-traced maps honour cancellation and report progress more than
      once; the texel loop is still one `parallelFor` range.
- [x] 3.8 Every new map honours the cage, the texel ceiling and the missing-UV
      rejection; every new parameter is refused out of range at the C ABI.
- [x] 3.9 The encoding basis reaches the C ABI, both bindings and the CLI report.

## 4. Documentation

- [x] 4.1 CHANGELOG under `## [Unreleased]`.
- [x] 4.2 README / docs map list and preset vocabulary.
