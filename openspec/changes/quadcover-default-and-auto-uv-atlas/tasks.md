# Tasks: quadcover-default-and-auto-uv-atlas

These document work already SHIPPED and verified; this change brings the specs
into sync with it. Checked items are complete in the codebase.

## 1. Default quadrangulator = quad-cover

- [x] `RemeshParams.quad_method` / `cyber_default_params` default to `quad-cover`
      (api.py, capi).
- [x] `quadCoverAvailable()` returns true unconditionally (a dependency-free
      native seamless-UV solver is always compiled in).
- [x] `cpu-headless` preset builds `-DCYBER_WITH_QUADCOVER=ON` so the stock
      default uses the in-process Geogram field.
- [x] Benchmark: quad-cover beats QuadriFlow on median + irregular on the organic
      corpus (spot 84.2 / rocker 84.5 / bunny 83.3), fandisk the CAD residual.

## 2. Seamless-UV backends + feature routing

- [x] `computeSeamlessUv` orders vendored (Geogram) → native → field-aligned
      fallback; `CYBER_QC_NO_NATIVE` disables the native path.
- [x] `creaseEdgeFraction(mesh, degrees)` const discriminator; crease-heavy
      meshes (≥ `CYBER_QC_ROUTE_CREASE`, default 2%) route to the native feature-
      aware solver first (`CYBER_QC_NO_ROUTE` kill-switch).
- [x] Base relax raised to 40 for quad-cover/integer (uniform bases) — corpus-
      wide median lift at no CV cost.

## 3. Automatic UV atlas

- [x] `cyber::uv::unwrapAtlas`: autoSeams (normal-coherent growth) → cone merge +
      distortion-bounded merge → per-chart LSCM (+ planar fallback) → min-area
      re-orient → skyline pack; returns `AtlasResult`.
- [x] `AtlasOptions` toggles: maxChartAngleDeg, mergeCharts, maxChartDistortion,
      reorientCharts, pack margin/textureSize.
- [x] C++ unit tests (cube charts, low-distortion atlas, determinism, merge,
      re-orient, packing).

## 4. Bindings

- [x] C ABI `cyber_uv_atlas` / `cyber_default_atlas_params` (gated behind
      `CYBER_CAPI_WITH_UV`; symbols always declared).
- [x] Python `Mesh.unwrap_atlas(AtlasParams) -> AtlasResult`; `python_test_uv_atlas`.

## 5. Docs / examples

- [x] `examples/14_uv_atlas.py`, `examples/15_uv_vs_xatlas.py`.
- [x] `docs/uv-atlas-plan.md`, `docs/ROADMAP.md`, README updated.
- [ ] `docs/roadmap_dashboard.html` refreshed (this change's companion task).
