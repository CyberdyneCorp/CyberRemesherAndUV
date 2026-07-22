# Proposal: quadcover-default-and-auto-uv-atlas

## Why

Two capabilities have shipped since `bootstrap-v1-platform` and outrun its specs,
which now describe behaviour that no longer matches the engine:

1. **The default quadrangulator changed from field-aligned to quad-cover.** The
   `remeshing-pipeline` spec still states "the field-aligned quadrangulator is the
   default for the CLI and C ABI / bindings." That is false: `quad-cover` (a
   QuadCover seamless-UV isoline extractor) is the default, and — built with the
   in-process Geogram field (`-DCYBER_WITH_QUADCOVER`, the `cpu-headless` preset)
   — it **beats QuadriFlow on median quad angle and irregular-vertex count** on
   organic meshes. The spec captures none of the backend selection or the CAD
   crease-routing that make this work.
2. **An automatic UV atlas capability was added.** The `uv-editing` spec covers
   only the *interactive* editor (hand-drawn seams, gesture unwrap, manual/auto
   packing). The engine now also has a one-call automatic path — mesh in, packed
   atlas out, no manual seams — exposed through the C ABI (`cyber_uv_atlas`) and
   Python (`Mesh.unwrap_atlas`), which `engine-bindings` does not mention.

This change amends the specs to match the shipped, tested behaviour.

## What Changes

- **Correct the default quadrangulator** to `quad-cover` in `remeshing-pipeline`;
  field-aligned becomes the always-available fallback.
- **Specify the seamless-UV backend selection**: an always-compiled dependency-
  free native solver, an optional in-process vendored Geogram solver
  (`-DCYBER_WITH_QUADCOVER`) that reaches reference quality, and an optional
  external CLI reference path; plus **feature routing** that sends crease-heavy
  (CAD) meshes to the feature-aware native solver.
- **Add the automatic UV atlas** to `uv-editing`: normal-coherent chart growth,
  cone + distortion-bounded chart merge, per-chart LSCM (with a planar-projection
  fallback), minimum-area chart re-orientation, and skyline packing into the unit
  square, reporting chart count and conformal distortion.
- **Add the atlas binding** to `engine-bindings`: `cyber_uv_atlas` /
  `Mesh.unwrap_atlas`, returning chart/distortion/packing statistics.

## Capabilities

### Modified

- `remeshing-pipeline`: default quadrangulator is quad-cover; document the
  seamless-UV backends and feature routing.

### Extended (new requirements)

- `uv-editing`: automatic (non-interactive) UV atlas.
- `engine-bindings`: the automatic-atlas C ABI + Python surface.

## Out of scope

- True 2D (hole-filling) UV nesting — measured as the remaining coverage gap to
  xatlas; profile nesting was tried and reverted as a non-win. Deferred, not
  specified here.
- The native seamless solver becoming the *silent* default in dependency-free
  builds beyond its current fallback role (its coarse-count quality is tracked
  separately).
