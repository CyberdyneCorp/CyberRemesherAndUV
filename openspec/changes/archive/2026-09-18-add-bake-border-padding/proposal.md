## Why

A baked map stops at the edge of each UV island. Everything downstream reaches past
that edge: bilinear sampling at the island border, mip generation, and block
compression all mix the outermost covered texel with whatever the background holds.
The result is a dark rim on every seam, and it gets worse with every mip level.

`surface-baking` has no padding requirement today, and `mesh-io`'s export presets add
none either. CyberTexel dilates its own paint output and pads at export, but it cannot
repair a map that arrived unpadded — by then the information is gone (epic #86, issue
#90).

The cheap fix — copy the edge texel outward and iterate — is what most bakers ship,
and it is precisely what produces a visible hard ring under mipmapping: the padded
band is a flat plateau, so the mip chain averages a step discontinuity instead of a
continuing surface. ArmorPaint's dilation instead searches outward for the nearest
covered texel and *continues the gradient* along the direction it found
(`paint/shaders/dilate_pass.kong:64-112`, zlib). That is what this change implements.

Padding is a post-process over finished texels, so it is also the one stage that can
silently destroy issue #88's id maps: an interpolated id colour is a colour that maps
to no id, and a consumer comparing at zero tolerance would fail on the whole padded
band. Channel semantics therefore decide the fill rule, and the rule is chosen from
the recorded encoding basis rather than from the map's name.

## What Changes

- **A padding stage runs on every baked map before `bake()` returns**, over the same
  coverage mask the rasteriser already produced. It costs no rays, no cage projection
  and no Target access.
- **A configurable radius in texels**, `BakeParams::paddingRadius`, defaulting to
  **8**. Zero disables padding entirely. A negative radius is refused with an empty
  image, exactly as every other out-of-range bake parameter is.
- **Extrapolation, not repetition.** The band grows one ring at a time; each new texel
  takes the covered texels among its eight compass neighbours, continues the linear
  gradient each of them defines (`2*v1 - v2` along that direction), and averages the
  results. Because each ring reads the one before it, the value is clamped into the
  range the covered texels span widened by that range's own width, which bounds a
  noisy map's compounding without flattening a genuine continuation.
- **The fill rule follows the channel semantics, read off `EncodingBasis`:**
  - `TangentNormal` / `ObjectNormal` — extrapolate, then **renormalize** to unit
    length, so a padded band decodes to directions rather than to shortened vectors.
  - `IdColor` — **nearest neighbour, exact copy, never extrapolated.** An id map's
    texels are keys; averaging two of them produces a colour that resolves to no id
    and breaks the zero-tolerance comparison #88 exists for.
  - everything else — extrapolate, no renormalization. A scalar map has no unit
    length to restore and renormalizing one would be meaningless.
- **Padding is reported**, as `BakeResult::padding`: the radius applied, the mode
  actually used (`none` / `nearest` / `extrapolate` / `extrapolate-unit`) and the
  number of texels the band wrote. It travels with the image through the C ABI, the
  export bundle and both bindings, and appears in the CLI's JSON report beside the
  file it describes.
- **Reachable from every entry point the existing maps are**: `cyber_bake` /
  `cyber_bake_field`, the export bundle, the CLI (`--padding`), Python and Swift. Each
  entry point that validates bake parameters validates this one identically.
- ABI 1.21, additive: one `CyberBakeParams` member, one `CyberBundleParams` member,
  `CyberPaddingMode`, `CyberImagePadding` and two accessors.

## Capabilities

### Modified Capabilities

- `surface-baking`: a new requirement fixes the padding stage — the radius and its
  default, extrapolation rather than repetition, the per-basis fill rule including the
  nearest-neighbour rule for id maps, and the reported record. The id-map requirement
  is amended: the reserved "no id" black is what a texel the bake wrote NOTHING to
  holds, and a texel in the padded band is a texel the bake wrote.

## Impact

- `src/bake/` — `BakeParams::paddingRadius`, `BakePadding` / `PaddingMode`, the new
  `padBorders()` stage and its coverage mask.
- `src/exportbundle/` — `BundleParams::paddingRadius`, `BundleFile::padding`.
- `capi/` — ABI 1.21 (`capi/abi/cyber_capi-1.21.json`), `CyberPaddingMode`,
  `CyberImagePadding`, `cyber_image_padding`, `cyber_bundle_result_file_padding`.
- `apps/cli/` — `--padding <int>`, the `padding` block in the JSON report's `outputs`.
- `python/`, `swift/` — `padding_radius` / `paddingRadius`, `ImagePadding`, the image
  and bundle-file accessors; both binding-parity gates.
- `tests/bake/test_padding.cpp` (new), `tests/capi`, `tests/exportbundle`, CHANGELOG,
  README bake documentation.
