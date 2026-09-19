## 1. Bake core

- [x] 1.1 `BakeMap::WorldDirection` and `BakeMap::UvDensity` appended to the enum, and
      `EncodingBasis::WorldDirection` / `::UvDensity` appended beside them; existing
      enumerators keep their numbers.
- [x] 1.2 `DensityNormalization { Absolute, Relative }`, `BakeParams::placement` (4x4
      row-major, identity default) and `BakeParams::densityNormalization`.
- [x] 1.3 `BakeEncoding` records the placement applied, the density normalization mode and
      the measured mean, beside the existing basis members.
- [x] 1.4 Catalogue rows for both maps (`world-direction`, `uv-density`), keeping the
      static_assert that every enumerator has one.
- [x] 1.5 World-direction shading: the cage hit's smooth normal carried by the placement's
      inverse transpose, renormalized, then the up axis, then `n*0.5+0.5`; an EXACT skip
      for an identity linear part so the identity case equals `object-normal` bit for bit.
- [x] 1.6 UV density: the rasteriser stores each sub-triangle's `uvArea / surfaceArea`
      ratio with the texel; shading multiplies by `width * height`; an undefined ratio
      takes the zero sentinel.
- [x] 1.7 The relative normalization pass over the finished image (defined texels only, in
      raster order), the measured mean into the encoding, and it runs before padding.
- [x] 1.8 `paramsUsable` refuses a non-finite or singular placement for the map that reads
      it; `neutralPadding` and `encodingFor` cover both new maps.
- [x] 1.9 `paddingModeFor` gives the world direction the unit-renormalizing rule and the
      density the plain extrapolating one; the density declares `[0, +inf)`.

## 2. C ABI (1.23, additive)

- [x] 2.1 `CYBER_BAKE_WORLD_DIRECTION` / `CYBER_BAKE_UV_DENSITY`,
      `CYBER_ENCODING_WORLD_DIRECTION` / `CYBER_ENCODING_UV_DENSITY`, and
      `CyberDensityNormalization`.
- [x] 2.2 `CyberBakeParams` gains `placement[16]` and `densityNormalization`, with
      `cyber_default_bake_params` filling both and the shared validator refusing a
      non-finite or singular placement and an unknown normalization identically for
      `cyber_bake`, `cyber_bake_field`, `cyber_bake_provider_bake` and the export bundle.
- [x] 2.3 `CyberImageDensity`, `cyber_image_density` and `cyber_image_placement`.
- [x] 2.4 `CyberBakeProviderResult` gains `density` and `placement` at the end, with the
      accepted floor frozen at the ABI 1.22 layout so a 1.22 caller is still served.
- [x] 2.5 ABI minor 1.22 -> 1.23, and the regenerated `capi/abi/cyber_capi-1.23.json`
      pinned by the manifest test.

## 3. Presets, bundle, CLI

- [x] 3.1 `PresetMap::WorldDirection` / `::UvDensity` with the names `world-direction` and
      `uv-density`, and the bundle's map mapping.
- [x] 3.2 `BundleParams` carries the placement and the density normalization; the bundle's
      up-axis warning covers the world-direction map.
- [x] 3.3 CLI `--placement <16 csv floats>` and `--density <absolute|relative>`, the
      `--bake` help list, and argument validation that refuses the same values the ABI does.
- [x] 3.4 The JSON report writes the placement for a world-direction entry and the
      normalization/mean for a density entry.

## 4. Bindings

- [x] 4.1 Python: the two map kinds, the two bases, `DensityNormalization`,
      `BakeParams.placement` / `.density_normalization`, `Image.density` / `.placement`,
      the provider result's new members, and the bundle parameters.
- [x] 4.2 Swift: the same surface, with `BakeParameters.cValue` rebuilt by member
      assignment so a future append cannot silently break it.
- [x] 4.3 Both binding-parity gates green with no new pending registrations.

## 5. Tests

- [x] 5.1 Identity placement equals `object-normal` at zero tolerance; a quarter-turn
      placement carries every decoded direction through that rotation; a non-uniform scale
      follows the inverse transpose and not the plain multiply.
- [x] 5.2 A singular or non-finite placement is refused with no image, at the engine and at
      the C ABI; a non-identity placement changes no other map.
- [x] 5.3 Absolute density on a known layout equals `uvArea * w * h / surfaceArea`;
      doubling the resolution quadruples it; an unevenly packed pair of islands separates.
- [x] 5.4 Relative density divides by the mean of the defined texels; the reported mean is
      the absolute one.
- [x] 5.5 A zero-area UV face takes the zero sentinel, the map holds no non-finite value,
      and the mean is unchanged by that face.
- [x] 5.6 The density map is unchanged by swapping the Target.
- [x] 5.7 Padding: the world direction's band decodes to unit directions inside `[0,1]`; the
      density's band is extrapolated, never negative, and never renormalized.
- [x] 5.8 Catalogue, C ABI, CLI and Python coverage for both maps: advertised, requestable,
      reported in the JSON report, and subject to the texel ceiling.
- [x] 5.9 Register every new test file in `tests/CMakeLists.txt`.

## 6. Documentation

- [x] 6.1 `CHANGELOG.md` under `## [Unreleased]`, and `README.md`'s map list.
- [x] 6.2 Resolve every task above, `openspec archive` the change, and re-run the gates.
