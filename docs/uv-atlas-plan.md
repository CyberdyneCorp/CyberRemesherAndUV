# Automatic UV atlas

The UV module (`src/uv/`) began as an *interactive* editor toolkit: hand-drawn
seams (`SeamSet`), single-island LSCM unwrap (`lscmUnwrap`), shelf packing
(`packBoxes` / `packIslands`), distortion metrics (`measureDistortion`), plus 2D
layout / symmetrize. What it lacked was a **non-interactive path** — a way to go
from an arbitrary mesh to a packed UV atlas without a human drawing every seam.
`atlas.hpp` / `atlas.cpp` fill that gap (the "Smart UV Project" family).

## Pipeline

`unwrapAtlas(mesh, options)`:

1. **Seam generation** (`autoSeams`) — greedy normal-coherent chart growth. From
   each ascending-id seed face, flood-fill neighbours whose normal stays within
   `maxChartAngleDeg` of the seed normal. Deterministic; every face lands in
   exactly one chart. Edges between different charts become seams (mesh boundary
   edges are already island boundaries and are left unmarked).
2. **Charts** — `computeIslands(mesh, seams)` re-derives the face partition from
   the seams (single tested code path).
3. **Unwrap** — `lscmUnwrap` per chart (conformal, in-house CGLS). A degenerate
   chart LSCM rejects falls back to orthographic **planar projection** onto the
   chart's average-normal plane, so every chart always receives UVs.
4. **Measure** — `measureDistortion` per chart, aggregated into max/RMS angular
   (conformal) error and a flipped-chart count. Measured *before* packing, which
   applies a per-chart similarity (angle-preserving).
5. **Pack** — `packIslands` into the unit square with a uniform scale.

Returns `AtlasResult` (chart count, seam edges, max/RMS angle distortion,
flipped + fallback chart counts, packed-area fraction, texel density).

## Surfaces

- **C++**: `cyber::uv::unwrapAtlas` / `autoSeams` (`src/uv/include/cyber/uv/atlas.hpp`).
- **C ABI**: `cyber_uv_atlas` / `cyber_default_atlas_params`
  (`capi/include/cyber_capi.h`). Compiled in only when `cyber_uv` is linked
  (`CYBER_CAPI_WITH_UV`); the symbols are always declared so the ABI is stable,
  and return `CYBER_ERR_RUNTIME` when the UV module is absent.
- **Python**: `Mesh.unwrap_atlas(AtlasParams) -> AtlasResult`, in place; a
  following `save_obj` emits `vt` / `f v/vt`.

## Results

`examples/14_uv_atlas.py` (remesh to quads → unwrap → pack), 40° chart angle:

| model        | quads | charts | max angle dist | flips | packed |
|--------------|------:|-------:|---------------:|------:|-------:|
| cube         | 1728  | 6      | 0.000          | 0     | 67%    |
| torus        | 1364  | 42     | 0.024          | 0     | 64%    |
| bumpy sphere | 1228  | 87     | 0.051          | 0     | 63%    |
| sphere       | 1326  | 21     | 0.015          | 0     | 68%    |

Angle distortion is conformal error in `[0,1)`; 0 = angle-preserving. Normal-
coherent charts stay near-flat, so LSCM is essentially conformal and never
flips.

## Known limitations / next steps

- **Chart orientation** — LSCM fixes orientation from its two pinned corners, so
  charts land at arbitrary angles (e.g. the cube's faces pack as 45°-rotated
  squares). A principal-axis / min-bounding-box re-orientation before packing
  would raise packing efficiency.
- **Packing** — shelf packing on axis-aligned bounding boxes wastes the gaps
  between rotated/irregular charts. A tighter (rotating / polygon) packer would
  push past ~65%.
- **Chart count on organic meshes** — a fixed normal threshold makes many small
  charts on bumpy surfaces. A merge pass (combine adjacent charts while distortion
  stays under a bound) would cut seam length.
- **Benchmark** — compare distortion + packing against xatlas / Blender Smart UV
  Project on a shared corpus, mirroring the remesher-vs-QuadriFlow harness.
