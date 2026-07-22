# Retopology Roadmap — Beating QuadriFlow-quality

Goal: make CyberRemesher's automatic quad retopology **better than QuadriFlow**
across four axes — quality-per-polygon, median quad angle, feature/CAD fidelity,
and robustness — not just competitive on one.

## Update — 2026-07-22 (default is now quad-cover; the gap is mostly closed)

The numbers in the rest of this doc describe the older `instant-meshes` extractor
and are **two generations stale**. The shipped **default is now `quad-cover`**
(`RemeshParams.quad_method`), and a fresh benchmark (`examples/11_benchmark.py`
metrics, ~3000 quads, spot/fandisk/rocker/bunny) reframes Phase 4:

- **Irregular-vertex half of the exit gate is already MET** — ~4% irregular
  (spot 4.1 / fandisk 3.3 / rocker 4.6 / bunny 4.9), gate was <15%, and it is
  ~100% *true* field cones (no extractor headroom). The old "36% spurious
  singularities" problem belongs to the retired extractor.
- **Median-angle half** — the dependency-free *native* quad-cover solver trails
  QuadriFlow by ~1.5–4.9° (mean ~3.4°). But the **vendored in-process Geogram
  field** (`-DCYBER_WITH_QUADCOVER=ON`, MIT) **beats QuadriFlow on median AND
  irregular on 3/4 organic models** — spot 83.6 vs 82.5, rocker 83.5 vs 82.2,
  bunny 83.0 vs 82.2 — losing only fandisk (80.9 vs 85.0, a CAD crease-alignment
  problem, not smoothness). Verified end-to-end, both backends reproduced.

So Phase 4 is **not** the "only a global integer-parametrization rewrite can close
it" hard core this doc claims: the organic median gap is closed by a build flag
(promote the vendored field to the default build). What genuinely remains is (1)
surfacing that as the stock default, and (2) the fandisk/CAD median (crease
alignment). Everything below is retained for history.

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
- **Phase 2 — enabling fix DONE; QF-beating Phase-4-gated.** The extractor now
  uses a **per-vertex spacing** derived from local mesh density, so it tracks
  adaptive sizing instead of over-merging it — the adaptive quad count no longer
  collapses (smooth model 72 → 625 quads, ~8×; uniform behaviour unchanged).
  Adaptivity is now validated: it **beats our own uniform sizing on quality-per-
  polygon on 4/5 models** (fandisk 0.46% vs 0.82% surface dev at matched count).
  But it does **not** yet beat QuadriFlow's *absolute* fidelity per polygon —
  that's limited by our base mesh quality (the ~36% spurious singularities), i.e.
  **coupled to Phase 4, not to adaptivity.** (The earlier "3/5 win vs QuadriFlow"
  was an artifact of the collapsed ~70–480-quad counts, where QuadriFlow itself
  degrades; at honest counts it leads.)
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

## Phase 2 — Win on adaptivity (quality-per-polygon) — 🟢 Fix landed · ⛓ QF-gated by Phase 4

QuadriFlow is uniform. We have curvature-adaptive sizing (`adaptivity`). Concentrate
quads where curvature is high → better fidelity per polygon.
- ✅ **Variable-spacing extractor** — the position-field extractor now takes a
  per-vertex spacing (local density), so it tracks adaptive sizing instead of
  over-merging it. Count no longer collapses; uniform path unchanged.
- ✅ **Adaptivity validated** — beats our own uniform sizing on quality-per-polygon
  on 4/5 corpus models.
- ⛓ **Beating QuadriFlow absolutely is coupled to Phase 4** — our per-polygon
  fidelity is capped by the ~36% spurious singularities, not by sizing. Revisit
  this exit after Phase 4 lands.
- ◻ **Optional:** budget-preserving sizing so `adaptivity` honors the target count
  (a renorm was tried and reverted — it destabilized dev on 2 models; needs a
  gentler, mesh-quality-aware formulation).
**Exit:** ours ≥ QuadriFlow on quality-per-polygon on ≥ 4/5 corpus models *(blocked
on Phase 4)*; adaptivity beats our own uniform sizing on ≥ 4/5 *(met)*.

## Phase 3 — Win on features & robustness — 🟡 measured; MIXED (win on hard geometry)

Metrics built (`feature_error`, `mesh_validity`) and wired into the benchmark.
Honest finding — **not a clean win**:
- ✅ **Robustness on hard-edged / box geometry** — the genuine strength. On a
  subdivided cube QuadriFlow catastrophically tears (598 boundary edges, 2.87%
  feature error) while ours is clean (0 defects, 0.3%). QuadriFlow degenerates on
  sharp box corners; we don't.
- ❌ **Complex smooth CAD/organic** — QuadriFlow leads. Better crease alignment
  (fandisk 0.43% vs 0.83%, cheburashka 0.57% vs 1.27%) and our extractor still
  leaves validity **defects** it doesn't (cheburashka 74 boundary edges, fandisk
  2 non-manifold). Hole-fill doesn't close them — a real extractor bug.
**Follow-ups:** (a) fix the extractor's scattered validity defects on complex
models; (b) hard-align the field to sharp creases for better CAD feature loops.
**Exit (partial):** robustness win on hard-surface geometry *(met)*; ≥ QuadriFlow
feature alignment + validity on the smooth corpus *(not met — extractor bugs)*.

## Phase 4 — Close the median-angle gap (the hard core) — 🔴 local approaches exhausted

Reduce spurious singularities (36% irregular → target < 10%) for angle parity.
- ❌ **4a. Local valence optimization** (edge rotation to cancel val-3/5 pairs) —
  TRIED, net-negative. Trades topology for geometry: ungated it cut irregular
  vertices 38→31% but wrecked edge-length CV (bunny 0.21→0.46) by shearing quads;
  gated to preserve geometry it becomes a no-op. Triangle-pair merge was also
  neutral. Local post-hoc surgery can't fix this without wrecking shape.
- ◻ **4b. Global integer parametrization** (QuadriFlow's method): spanning-tree
  integer integration + min-cost-flow holonomy resolution, producing clean
  topology *and* geometry from the start. The remaining real lever — a large,
  high-risk extractor rewrite, genuinely multi-session. **Planned in detail:**
  [`docs/integer-parametrization-plan.md`](integer-parametrization-plan.md)
  (Stage 1 coords → Stage 2 integer solve → Stage 3 extraction, behind a new
  `quad_method="integer"` until it beats the current path).
**Exit:** irregular-vertex % < 15% and median angle ≥ QuadriFlow. **Only 4b can
get there; the local shortcuts are proven dead ends.**

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
