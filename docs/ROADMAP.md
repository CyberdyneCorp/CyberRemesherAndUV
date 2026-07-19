# Retopology Roadmap — Beating QuadriFlow-quality

Goal: make CyberRemesher's automatic quad retopology **better than QuadriFlow**
across four axes — quality-per-polygon, median quad angle, feature/CAD fidelity,
and robustness — not just competitive on one.

## Where we are (2026-07)

The position-field extractor (`quad_method="instant-meshes"`, an Instant-Meshes
clean-room port) is shipped and opt-in. Measured against QuadriFlow (uniform
sizing, matched quad count, on spot / fandisk / stanford-bunny):

| Metric | Us (extractor) | QuadriFlow |
|---|---|---|
| Median smallest-quad-angle | 78 / 79 / 76° | 83 / 85 / 80° |
| Edge-length CV (lower better) | 0.17 / 0.19 / 0.21 | 0.12 / 0.14 / 0.17 |
| Sliver rate | < 2% | ~0–1% |

We **match on uniformity**, trail **~4–6° on median angle**. Root cause is
**diagnosed and quantified** (see `memory` / commit history): the bulk
valence-4 quads are already QuadriFlow-quality (median 80.6°), but only ~21% of
quads are bulk — ~36% of vertices are irregular (val-3/5), and most are
*extraction artifacts*, not true field singularities. Cheap levers (extraction
density, field iterations, geometric relaxation, valence recovery) are exhausted
and characterised; closing the last gap is a genuine topology build.

## North Star

Every phase exits on a number from a single automated benchmark (Phase 1). We
do not claim "better" without a harness number that shows it.

## Status (2026-07)

- **Phase 1 — DONE.** `examples/11_benchmark.py` scores the corpus vs QuadriFlow
  on surface deviation, normal error, median angle, edge CV, and irregular-vertex
  %. Uniform, matched target (~3000 quads), best-of-ours vs QuadriFlow: ours ties
  on surface dev / normal error (2/5 models each), trails on median angle and
  singularity count (0/5) — the known extraction-singularity gap. It immediately
  earned its keep by falsifying the naive Phase-2 hypothesis (below).
- **Phase 2 — PARTIAL.** Quality-per-polygon (ours adaptive vs QuadriFlow at the
  same output count): ours wins on **3/5** models (spot, fandisk, bunny) and
  loses on rocker-arm and cheburashka. Not yet robust. The benchmark exposed the
  blockers: (a) the position-field extractor uses a single global spacing, so it
  **over-merges adaptive-density input and collapses the quad count** (target
  2500 → ~200–650); (b) the curvature sizing puts large quads across gently
  curved fillets, hurting fidelity on some models. Both are the tracked Phase-2
  work; the win is real where it lands but needs a variable-spacing extractor and
  a better sizing field to generalize.
- **Bonus finding:** the QuadriFlow-in-every-example panels show a clear
  feature-preservation win — on a cube QuadriFlow rounds the edges and tears
  holes (20% slivers) while our feature-aware remesh keeps them crisp. Feeds
  Phase 3.

---

## Phase 1 — Define & measure "better" (foundation) — ✅ DONE

Today's harness measures QuadriFlow's home turf (uniform, matched-count, median
angle). Build metrics that capture *real* retopology quality:

- **Quality-per-polygon**: Hausdorff + normal-error vs the source at matched
  polygon count (rewards adaptivity — QuadriFlow is uniform-only).
- **Feature-following error**: mean distance of quad edges from sharp creases.
- **Singularity count / irregular-vertex %**: track the diagnosed weakness.
- **Robustness**: success + manifold rate over the *full* common-3d-test-models
  corpus, not 3 hand-picked models.

**Deliverable:** `examples/11_benchmark.py` — an automated scored benchmark,
runnable in CI, producing a per-model table and an aggregate score vs QuadriFlow.
**Exit:** the benchmark runs green and reproduces the numbers above.

## Phase 2 — Win on adaptivity (quality-per-polygon) — 🟡 PARTIAL (3/5)

QuadriFlow is uniform. We have curvature-adaptive sizing (`adaptivity`). Concentrate
quads where curvature is high → same fidelity at fewer polygons. The quality-per-
polygon win lands on 3/5 models today. To make it robust:
- **Variable-spacing extractor**: the position-field extractor must accept a
  per-vertex target spacing (e.g. local edge length) instead of one global
  spacing, so it stops over-merging adaptive input and honors the quad budget.
- **Better sizing field**: refine on gently-curved fillets (currently spans them
  with large quads), and stabilize the target-count contract across models.
**Exit:** ours ≥ QuadriFlow on quality-per-polygon (surface dev at matched output
count) on ≥ 4/5 corpus models, and adaptivity honors the target count within ±25%.

## Phase 3 — Win on features & robustness

- **Sharp-feature field constraints**: hard-align the orientation field to
  creases so CAD edges become clean quad loops (QuadriFlow blurs them).
- **Robustness pass**: broaden noisy-scan / degenerate-input handling across the
  whole corpus.
**Exit:** lower feature-following error than QuadriFlow on fandisk-class models;
≥ QuadriFlow manifold-success rate across the corpus.

## Phase 4 — Close the median-angle gap (the hard core)

Reduce spurious singularities (36% irregular → target < 10%) for angle parity:

- **4a. Quad singularity cancellation** (Tarini/Bunin): post-extraction surgery
  moving +¼/−¼ defects along quad chains to annihilate. Incremental; attacks the
  measured problem without a rewrite. Try first.
- **4b. Global integer parametrization** (QuadriFlow's actual method): spanning-tree
  integer integration + min-cost-flow holonomy resolution, replacing the
  collapse extractor. Big rewrite; only if 4a plateaus.
**Exit:** irregular-vertex % < 15% and median angle ≥ QuadriFlow on the harness.

## Phase 5 — Field foundation (enables 2–4)

Stronger orientation-field optimization (fewer, better-placed singularities at
the source) + the position-field integer optimization. Feeds every phase;
overlaps 4b.
**Exit:** raw-extraction corner-skew floor < 8° (currently ~13°).

---

## Sequencing

`1 → 2 → 3 → 4 → 5`. Phases 1–3 are where we *actually beat* QuadriFlow and are
lower-risk — bank them first. Phase 4 is the expensive median-angle parity fight;
worth doing, but it must not block the winnable advantages. Each phase is one
OpenSpec change proposal with the exit criterion above as its acceptance test.

## Guardrails

- The default `field-aligned` quadrangulator and golden tests stay
  byte-identical unless a change explicitly targets them.
- GPL sources (AutoRemesher, QuadCover/CoMISo) are idea references only, never
  copied. QuadriFlow / Instant Meshes are permissive and attributed.
- Every claimed improvement ships with a harness number and a regression test.
