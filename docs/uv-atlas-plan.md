# Automatic UV atlas

The UV module (`src/uv/`) began as an *interactive* editor toolkit: hand-drawn
seams (`SeamSet`), single-island LSCM unwrap (`lscmUnwrap`), shelf packing
(`packBoxes` / `packIslands`), distortion metrics (`measureDistortion`), plus 2D
layout / symmetrize. What it lacked was a **non-interactive path** — a way to go
from an arbitrary mesh to a packed UV atlas without a human drawing every seam.
`atlas.hpp` / `atlas.cpp` fill that gap (the "Smart UV Project" family).

## Pipeline

`unwrapAtlas(mesh, options)`:

1. **Chart growth** — greedy normal-coherent seed growth. From each ascending-id
   seed face, flood-fill neighbours whose normal stays within `maxChartAngleDeg`
   of the seed normal. Deterministic; every face lands in exactly one chart.
2. **Merge** (`mergeCharts`, default on) — greedy seed growth fragments bumpy
   surfaces into many small charts that share a compatible orientation. This pass
   merges adjacent charts whose *union* still fits within a `maxChartAngleDeg`
   normal cone (union-find to a fixpoint over the adjacency graph). Because every
   face of a merged chart stays inside one cone, the result is at least as flat as
   growth guarantees — so it cuts seam length (bumpy sphere 87 → 65 charts)
   *without* raising distortion, and cannot wrap a tube (a sub-90° cone is convex).
   `autoSeams` then marks the edges between the final charts as seams (mesh
   boundary edges are already island boundaries and are left unmarked).
3. **Charts** — `computeIslands(mesh, seams)` re-derives the face partition from
   the seams (single tested code path).
4. **Unwrap** — `lscmUnwrap` per chart (conformal, in-house CGLS). A degenerate
   chart LSCM rejects falls back to orthographic **planar projection** onto the
   chart's average-normal plane, so every chart always receives UVs.
5. **Re-orient** (`reorientCharts`, default on) — rotate each chart to its
   minimum-area bounding rectangle. LSCM fixes orientation from its two pinned
   corners, so charts otherwise land at arbitrary angles and waste space in the
   axis-aligned shelf packer. The optimal rectangle always shares an edge with
   the chart's convex hull, so it suffices to test every hull-edge direction
   (Andrew's monotone-chain hull + per-edge bounding box); the longer side is
   left horizontal. Being a similarity, this leaves conformal distortion and
   flips unchanged.
6. **Measure** — `measureDistortion` per chart, aggregated into max/RMS angular
   (conformal) error and a flipped-chart count.
7. **Pack** — `packIslands` into the unit square with a uniform scale, using the
   bottom-left **skyline** strategy (`PackStrategy::Skyline`): a per-column height
   map over a fixed-width strip drops each box into the lowest gap, filling the
   vertical slack the shelf packer leaves. Several strip widths are tried and the
   one with the smallest square bounding extent wins. (The interactive path keeps
   the simpler `Shelf` default.)

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

`examples/14_uv_atlas.py` (remesh to quads → unwrap → pack), 40° chart angle.
*Coverage* is the fraction of the unit UV square filled by actual geometry (the
real texel-efficiency measure), with chart re-orientation off vs on:

| model        | quads | charts | max angle dist | flips | coverage (final) |
|--------------|------:|-------:|---------------:|------:|-----------------:|
| cube         | 1728  | 6      | 0.000          | 0     | 67%              |
| torus        | 1364  | 39     | 0.024          | 0     | 40%              |
| bumpy sphere | 1228  | 65     | 0.048          | 0     | 41%              |
| sphere       | 1326  | 19     | 0.015          | 0     | 48%              |

Chart counts are after the merge pass (bumpy sphere 87 → 65, torus 42 → 39,
sphere 21 → 19; the cube's orthogonal faces cannot merge).

Angle distortion is conformal error in `[0,1)`; 0 = angle-preserving. Normal-
coherent charts stay near-flat, so LSCM is essentially conformal and never flips.
The two packing levers stack on top of the base LSCM+shelf coverage:

| model        | base (shelf) | + re-orient | + skyline (final) |
|--------------|-------------:|------------:|------------------:|
| cube         | 33%          | 67%         | 67%               |
| torus        | 30%          | 34%         | 40%               |
| bumpy sphere | 31%          | 34%         | 41%               |
| sphere       | 38%          | 42%         | 48%               |

Re-orientation gives the biggest lift on box-like meshes (the cube's 45°-diamond
faces become axis-aligned squares and double their coverage); skyline packing then
adds a steady 6–7 points by filling the gaps between the irregular organic charts.

## Known limitations / next steps

- **Packing** — the skyline packer works on axis-aligned bounding *boxes*, so an
  L-shaped or ring chart still reserves its whole box. True polygon nesting (pack
  against the chart outline, not its box) is the next lever, worth most on the
  concave organic charts (the torus/sphere rings visibly waste their interior).
- **Benchmark** — compare distortion + packing against xatlas / Blender Smart UV
  Project on a shared corpus, mirroring the remesher-vs-QuadriFlow harness.
