## 1. Core bake

- [x] 1.1 `BakeParams::paddingRadius`, defaulting to 8 texels; zero disables; a
      negative value joins the existing `paramsUsable()` rejection and yields an
      empty image.
- [x] 1.2 `PaddingMode` and `BakePadding` (radius applied, mode used, texels filled)
      on `BakeResult`, filled for every map.
- [x] 1.3 The padding stage: a coverage mask from the rasteriser's own texel list, the
      band grown one ring per pass over the eight compass neighbours, no unordered
      container and no dependence on visit order.
- [x] 1.4 Extrapolation: `2*v1 - v2` along each covered direction, averaged over the
      directions that hit, falling back to `v1` where no second sample exists.
- [x] 1.5 Bound every extrapolated channel to the covered range widened by that
      range's own width, so compounding is bounded without flattening a continuation.
- [x] 1.6 Fill rule from `EncodingBasis`: renormalize `TangentNormal` / `ObjectNormal`,
      nearest-neighbour verbatim for `IdColor`, plain extrapolation otherwise.
- [x] 1.7 Padding runs on both the raycast and the field-evaluator path, honours
      cooperative cancellation, and is skipped on an abandoned bake (empty image).

## 2. Entry points

- [x] 2.1 C ABI: `CyberBakeParams::paddingRadius`, `CyberBundleParams::paddingRadius`,
      `CyberPaddingMode`, `CyberImagePadding`, `cyber_image_padding` and
      `cyber_bundle_result_file_padding`; the negative-radius rejection in
      `cyber_bake`, `cyber_bake_field` and `cyber_export_bundle_write`. ABI 1.21 manifest
      pinned, additive against 1.20.
- [x] 2.2 Export bundle: `BundleParams::paddingRadius` forwarded to every map's bake,
      `BundleFile::padding` recorded per file.
- [x] 2.3 CLI: `--padding <int>` with its help text and range check; the `padding`
      block in the JSON report's `outputs` entries.
- [x] 2.4 Python: `BakeParams.padding_radius`, `PaddingMode`, `ImagePadding`,
      `Image.padding` and the bundle-file equivalent.
- [x] 2.5 Swift: the same surface, and both binding-parity gates green.

## 3. Tests

- [x] 3.1 A gradient running off an island is CONTINUED by the padded band, not
      repeated: successive band texels keep moving in the gradient's direction and
      differ from the edge texel.
- [x] 3.2 A normal map's padded band decodes to unit-length directions, in both the
      tangent and the object-space basis.
- [x] 3.3 A scalar map's padded band is extrapolated and NOT renormalized.
- [x] 3.4 An id map's padded band holds only exact colours from the reported table,
      resolvable at zero tolerance, with no interpolated colour anywhere.
- [x] 3.5 Radius zero returns the pre-padding image texel for texel; the reported
      radius is zero and the mode is "none".
- [x] 3.6 A negative radius is refused with an empty image, at `bake()` and at every
      entry point that validates parameters.
- [x] 3.7 The band is at most `radius` texels wide: a texel farther than that from
      every island still holds the map's background value.
- [x] 3.8 Determinism: the same bake pads identically twice, and the band does not
      depend on texel visit order.
- [x] 3.9 No padded texel leaves the covered range by more than that range's width,
      and the bound does NOT flatten a ramp's continuation.
- [x] 3.10 The padding record reaches the C ABI, the export-bundle result and the CLI
      report.

## 4. Documentation

- [x] 4.1 CHANGELOG under `## [Unreleased]`.
- [x] 4.2 README bake documentation: the radius, its default, zero to disable, and the
      per-basis fill rule.
