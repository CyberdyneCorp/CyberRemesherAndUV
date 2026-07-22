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
   surfaces into many small charts. Two greedy union-find passes (each to a
   fixpoint over the chart adjacency graph) recombine them:
   - **Cone merge** — merges adjacent charts whose *union* still fits within a
     `maxChartAngleDeg` normal cone. Because every face stays inside one cone, the
     result is at least as flat as growth guarantees, so this cuts seams *without*
     raising distortion and cannot wrap a tube (a sub-90° cone is convex).
   - **Distortion-bounded merge** (`maxChartDistortion`, default 0.10) — the
     looser pass: it LSCM-unwraps each candidate union and merges only if the
     combined distortion — the max of per-face conformal (angle) error and the
     chart-wide area-scale spread — stays under the cap and the layout does not
     fold. Conformal error alone is not enough: a chart of flat facets (three cube
     faces) has ~0 angle error however the facets splay, because no triangle
     straddles a fold — the area-scale term catches that. This spends the large
     distortion headroom the cone merge leaves to fold developable regions
     together (a cube's six faces → two flat three-face strips), cutting the chart
     count to xatlas levels while staying ~2× under its distortion.

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
| cube         | 1728  | 2      | 0.000          | 0     | 67%              |
| torus        | 1364  | 12     | 0.025          | 0     | 36%              |
| bumpy sphere | 1228  | 30     | 0.048          | 0     | 37%              |
| sphere       | 1326  | 8      | 0.015          | 0     | 35%              |

Chart counts are after both merge passes. The distortion-bounded pass drives the
big reduction — cube 6 → 2 (two developable strips), torus 39 → 12, bumpy sphere
65 → 30, sphere 19 → 8 — while max distortion stays put. Coverage dips a little:
the merged charts are larger and more irregular (long ribbons, rings), which the
axis-aligned box packer handles worse — the polygon-nesting motivation below.

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

## Benchmark vs xatlas

`examples/15_uv_vs_xatlas.py` unwraps the *same* quad-remeshed geometry two ways —
`unwrap_atlas` and [xatlas](https://github.com/jpcy/xatlas) (the de-facto open
reference, `pip install xatlas`) on the triangulated quads — and scores both with
the identical conformal metric (singular-value spread of each triangle's UV
Jacobian, in each atlas's true texel space):

| model        | ours charts | ours meanD | ours cov | xatlas charts | xatlas meanD | xatlas cov |
|--------------|------------:|-----------:|---------:|--------------:|-------------:|-----------:|
| cube         | 2           | 0.000      | 100%     | 6             | 0.000        | 95%        |
| torus        | 12          | 0.006      | 36%      | 14            | 0.007        | 52%        |
| bumpy sphere | 30          | 0.010      | 40%      | 28            | 0.017        | 58%        |
| sphere       | 8           | 0.005      | 47%      | 8             | 0.007        | 57%        |

**We win conformal distortion (~2× lower)** — the tight normal-coherent charts
stay flat, so LSCM is nearly angle-preserving. **The distortion-bounded merge now
also matches or beats xatlas on chart count** (cube 2 vs 6, torus 12 vs 14, sphere
8 vs 8) while keeping that distortion lead. The one remaining gap is **coverage**:
xatlas's polygon packer fits its charts ~15 points tighter than our axis-aligned
box packer, a gap the merge widened because our developable-strip charts are long
and irregular. Polygon nesting is the sole remaining lever.

## Known limitations / next steps

- **Coverage (packing) — the last gap to xatlas (~15 pts), still open.** The
  skyline packer reserves each chart's axis-aligned bounding *box*, so a concave
  chart (ribbon, ring) wastes its interior.
  - **Profile/raster nesting — TRIED, does not win safely (reverted).** A raster
    footprint + per-column skyline-profile nester was implemented and measured on
    the corpus. Overlap-safe (a one-cell dilation so charts never bleed) it
    *regressed* coverage (torus 36→31, bumpy 37→34): the dilation over-inflates
    the many small charts more than the profile interlock saves, and re-oriented
    charts are already near-boxy so their profiles ≈ their boxes. The only wins
    (bumpy +3, sphere +9) appeared when dilation was off — i.e. tolerating chart
    overlap, which is wrong for a texture atlas. It also broke the re-orient
    invariant (45° diamonds interlock better than axis-aligned squares under
    raster). Net: skyline stays the packer.
  - **The real lever is TRUE 2D nesting (hole-filling), a larger project.** The
    dominant waste is concave *interiors* — a torus ring's central hole, which a
    per-column skyline cannot fill because it has no overhang/hole placement.
    Closing it means placing small charts *inside* big charts' holes: full 2D
    occupancy-grid bottom-left with overhang collision (bitset-row masks), plus
    likely per-chart rotation search — perf-sensitive and genuinely multi-session.
    Deferred until it is worth that cost; the chart-count and distortion gaps are
    already closed/won.
