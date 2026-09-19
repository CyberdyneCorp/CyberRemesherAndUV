## Why

Epic #86 makes this repository the baker for
[CyberTexel](https://github.com/CyberdyneCorp/CyberTexel), and two of its generators have
no input until this change lands (#89). A "dust settles on upward faces" or "rain streaks
downward" generator needs the surface normal **in world space** — the orientation is gone
by the time a tangent-space normal exists. A material that has to keep a constant
real-world texel scale across islands packed at different densities needs **UV density**,
which is also the map that shows an artist an uneven layout before they discover it by
painting.

There is a trap in the first of the two, and it decides whether the map means anything.
#87 already shipped `object-normal`, and its author recorded honestly that **this engine
has one model space** — "world" and "object" name the same space here. A world-space
direction map implemented naively therefore emits pixels **bit-identical to
`object-normal`** and ships a duplicate under a second name. #89 says the point of the map
is that it "follows the object's placement transform", and that transform does not exist
in this engine today. So the map is only worth shipping together with the parameter that
makes it distinct.

## What Changes

- **A PLACEMENT TRANSFORM becomes a bake parameter.** `BakeParams::placement` is a 4x4
  row-major object→world matrix, default identity, carried through the C ABI, the CLI, the
  export bundle and both language bindings. It is the transform a host has already applied
  to put the asset in its scene, and without it "world space" is a name for the space the
  engine already had.
- **A WORLD-SPACE DIRECTION map** (`world-direction`): the Target's surface normal carried
  into world space by the placement's **inverse transpose**, renormalized, re-expressed in
  the requested up axis and encoded `n * 0.5 + 0.5`. Under an identity placement it is
  bit-identical to `object-normal` **by construction and on purpose** — that is the
  statement that the two maps differ by the transform and by nothing else. Under any other
  placement it differs, which is the whole reason it exists.
- **A UV DENSITY map** (`uv-density`): texels per unit of surface area at the requested
  output resolution, as a single-channel scalar. Two normalizations behind a flag —
  **absolute** texels-per-unit for a scale-locked material, **relative** to the map's own
  mean for spotting unevenness. The mode, and the mean the relative form divided by, are
  recorded with the output and in the JSON report, so a relative map converts back to an
  absolute one.
- **A documented sentinel for a degenerate face.** A covered texel whose face has no UV
  area or no surface area has no density; it holds exactly `0`, which no defined density
  can take, and it is excluded from the mean rather than poisoning it with an infinity.
- **Two new encoding bases** (`world-direction`, `uv-density`) so border padding keeps
  doing the right thing without being told: the direction map is renormalized after
  extrapolation, the density scalar is not, and the density map declares its own value
  range — `[0, +inf)`, not `[0,1]`.
- **Both maps are requestable through every entry point the existing maps are**: the C ABI
  (`cyber_bake`, `cyber_bake_field`, `cyber_bake_provider_bake`), the map catalogue the
  provider advertises, export presets, the CLI's `--bake` list, and the Python and Swift
  bindings — each validating the two new parameters identically.
- **The UDIM interaction is STATED, not claimed**: #91 has not landed, so the spec records
  a forward constraint on what a UDIM-aware bake must preserve rather than asserting
  behaviour that exists.

## Capabilities

### New Capabilities

_None._

### Modified Capabilities

- `surface-baking`: adds the two map types, the placement transform they need, the density
  normalization mode and its sentinel, and the padding/value-range rules for the new
  encoding bases; extends the recorded encoding basis with the placement and the density
  record.

## Impact

- `src/bake/`: `bake.hpp` (two `BakeMap` enumerators, two `EncodingBasis` enumerators, a
  `DensityNormalization` enum, `BakeParams::placement` / `::densityNormalization`,
  `BakeEncoding::placement` / `::densityNormalization` / `::densityMean`), `bake.cpp`
  (shading, validation, the relative normalization pass), `map_catalog.cpp`,
  `border_padding.cpp`.
- `capi/`: ABI 1.23 — two `CyberBakeMap` and two `CyberEncodingBasis` enumerators, a
  `CyberDensityNormalization` enum, two appended `CyberBakeParams` members, a new
  `CyberImageDensity` struct with `cyber_image_density` and `cyber_image_placement`, and
  two appended `CyberBakeProviderResult` members (additive under the descriptor-size rule,
  whose floor is frozen at the 1.22 layout).
- `src/core/export_preset.*`, `src/exportbundle/`: two `PresetMap` kinds, the placement and
  density-normalization bundle parameters.
- `apps/cli/`: `--placement`, `--density`, the `--bake` map list, the JSON report's
  encoding record.
- `python/`, `swift/`: the same surface, plus the binding-parity registrations.
- Docs: `CHANGELOG.md`, `README.md`.
