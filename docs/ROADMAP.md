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
  QuadriFlow by ~1.5–4.9° (mean ~3.4°). The **vendored in-process Geogram field**
  (`-DCYBER_WITH_QUADCOVER=ON`, MIT) beats QuadriFlow on median AND irregular on
  spot (83.6 vs 82.5), rocker (83.5 vs 82.2) and bunny (83.0 vs 82.2), losing
  fandisk (80.9 vs 85.0). Verified end-to-end, both backends reproduced.
  - ⚠️ **Correction (2026-07-22): the "3/4 organic models" framing was selection
    bias.** That trio counts **rocker-arm**, the *mechanical* model, as organic,
    and omits **cheburashka**, an actual organic character — which **loses on
    both axes** (80 vs 82 median, 4% vs 2% irregular, and the worst edge-CV gap
    in the corpus at 0.22 vs 0.15). On the real organic set (spot, cheburashka,
    bunny) it is **2/3**, not 3/4. Corpus-wide the default is **3/5 on median and
    3/5 on irregular**. The win is real but narrower than recorded; quote 3/5,
    and name cheburashka alongside fandisk as the losses.

So Phase 4 is **not** the "only a global integer-parametrization rewrite can close
it" hard core this doc claims. **DONE:** the `cpu-headless` preset now builds with
`-DCYBER_WITH_QUADCOVER=ON`, so the stock default (no env vars) uses the vendored
Geogram field and reproduces spot 84 / rocker 85 / bunny 83 — beating QuadriFlow
on median *and* irregular on 3/5 of the corpus (losing fandisk and cheburashka).
Full test suite green against that build (only the pre-existing integer-extractor
WIP fails).

**Current standing of the default vs QuadriFlow** (2026-07-22, `--target-quads
3000`, re-measured after the harness was pointed at the shipped extractor — it had
been scoring the retired one in Phases 2–3):

| model | med° ours/QF | irr% ours/QF | CV ours/QF | defects ours/QF | feature ours/QF |
|---|---|---|---|---|---|
| spot | **84**/82 | **2**/3 | **0.12**/0.14 | **0**/0 | 0.46/**0.44** |
| fandisk | 83/**85** | 3/**1** | **0.16**/0.19 | **0**/0 | 0.82/**0.41** |
| rocker-arm | **85**/82 | **1**/3 | 0.15/0.15 | **0**/0 | 0.61/**0.40** |
| cheburashka | 80/**82** | 4/**2** | 0.22/**0.15** | **0**/0 | 1.15/**0.57** |
| stanford-bunny | **83**/82 | **3**/4 | 0.21/**0.17** | **8**/38 | 1.32/**0.58** |

Read across: we lead on **topological validity (5/5)**, lead on median angle and
irregular % (3/5 each), and trail on **feature-following**.

Read the feature column carefully — the raw 0/5 overstates it. `feature_error`
counts open boundaries as features (`examples/common.py:359`), and per-model crease
data is: spot 73 creases, fandisk 710, rocker-arm 394, cheburashka 316, bunny 774
of which **223 (29%) are the scan hole's boundary**. So the honest reading is
**3 real losses** — fandisk 2.0x, cheburashka 2.0x, rocker-arm 1.5x — with **spot
a tie** (0.46 vs 0.44 on only 73 creases) and **bunny confounded**. Still the
largest open quality gap, and notably **not CAD-only**: cheburashka is an organic
character with 316 genuine creases and a full 2x gap.

**Relax lever measured + shipped.** Bumping the quad-cover base relax 10→40 (its
Geogram base is uniform enough, like the integer grid) is a free, corpus-wide
median win — measured +0.3..+1.0° on organic with edge-CV flat-to-lower and
irregular % unchanged; now the default (spot 84.2 / rocker 84.5 / bunny 83.3,
cv 0.12–0.21). It is a *general* position lever, not model-specific.

**Fandisk/CAD median — mostly closed by backend routing (shipped).** A workflow
reframed it: the gap was **84% global extractor squareness** (vendored Geogram's
quads sit at ~81° even *far* from creases), not crease-following. Our native
feature-aware seamless solver marks sharp edges as hard seams and pins the
feature-bounded patches, so it makes squarer quads on CAD parts. `computeSeamlessUv`
now routes crease-heavy meshes (interior-crease-fraction ≥ 2%, via the non-mutating
`creaseEdgeFraction`) to the native solver first, keeping smooth organics on the
vendored Geogram path. Verified count-matched (~2970 quads, not a resolution
artifact): **fandisk 80.7 → 83.4 (+2.7°, ~63% of the gap), 0 defects**; every
organic **byte-identical** (`CYBER_QC_NO_ROUTE` kill-switch A/B). The residual to
QuadriFlow (83.4 vs 85.0) is the ~16% crease-localized part — a genuine field-level
per-edge integer-constraint project (QuadriFlow's feature flow), **deferred** as
not worth the multi-week cost for 1.6°. Everything below is retained for history.

## Where we are — HISTORICAL (2026-07, the retired `instant-meshes` extractor)

> ⚠️ **This section is superseded.** It describes `quad_method="instant-meshes"`,
> which has not been the default since quad-cover took over. For the shipped
> default's standing vs QuadriFlow, see the per-model table in the 2026-07-22
> update at the top of this doc. Kept because the phases below still reference
> its diagnosis, and because it records how the earlier picture was framed.

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

**How stale, concretely** — the same three models on today's default: median
**84 / 83 / 83°** (was 78/79/76) against QuadriFlow's 82/85/82, edge CV **0.12 /
0.16 / 0.21**, and irregular **2 / 3 / 3%** — not the ~36% above. The "36%
spurious singularities" problem, and every phase below that is gated on it,
belongs to this retired extractor.

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
  ~~Adaptivity is now validated: it beats our own uniform sizing on quality-per-
  polygon on 4/5 models (fandisk 0.46% vs 0.82% surface dev at matched count).~~
  **RETRACTED 2026-07-22 — 2/5, see Phase 2 below.** Ironically this bullet
  already diagnosed the failure mode it fell to: it notes the earlier "3/5 win vs
  QuadriFlow" was an artifact of collapsed quad counts, but the 4/5 figure was
  measured the same way — the arms never converged on a count, and fandisk's
  "win" was scored against a degenerate 22.61% baseline.
  Adaptivity also does **not** beat QuadriFlow's *absolute* fidelity per polygon.
  The old explanation — capped by ~36% spurious singularities, **coupled to Phase
  4** — no longer holds either: the default now runs at 1–4% irregular and still
  trails, so per-polygon fidelity is not singularity-gated.
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
- ❌ **The 4/5 "adaptivity validated" claim does NOT reproduce — it was a
  measurement artifact (2026-07-22).** The harness matched counts by requesting
  `achieved * 1.3` once, which left the adaptive and uniform arms 20–30% apart
  down at ~200–400 quads, where both extractors degrade. fandisk's "win" rested
  on a *uniform* baseline reporting **22.61%** surface deviation — a degenerate
  extraction, not a measurement; at matched count that same run is **0.30%**.
  With both arms driven through `search_matched_count`, adaptivity beats uniform
  on **2/5** — and one of those (cheburashka, 0.28 vs 0.29%) is inside noise,
  while rocker-arm is ~5× *worse* (1.30 vs 0.27%).
- ❌ **Adaptive sizing cannot even reach benchmark density on 2/5 models** —
  it saturates at 2034 quads (cheburashka) and 916 (stanford-bunny) against a
  3000 request, so those models have no matched-count comparison at all.
- ⚠️ **Caveat on the corrected numbers:** the count-match request ceiling bounds
  how hard the adaptive arm can be pushed, so on saturating models the pair is
  matched to each other but below target (spot lands ~657q). Rows are
  self-consistent; cross-model comparison is not meaningful.
- ◻ **Optional:** budget-preserving sizing so `adaptivity` honors the target count
  (a renorm was tried and reverted — it destabilized dev on 2 models; needs a
  gentler, mesh-quality-aware formulation).
**Exit:** ours ≥ QuadriFlow on quality-per-polygon on ≥ 4/5 corpus models *(not
met — QuadriFlow leads on all 5)*; adaptivity beats our own uniform sizing on
≥ 4/5 *(**NOT met — 2/5**; the earlier "met" was the artifact above)*.

**Recommendation: descope.** The shipped `quad-cover` default is uniform-only by
design (capi hardcodes adaptivity 0; the isotropic stage that consumes
`params.adaptivity` is bypassed for it), so this phase measures a lever the
default cannot use, and the lever does not win where it can be used.

## Phase 3 — Win on features & robustness — 🟦 CLOSED 2026-07-24 (validity won outright; feature-following a known limitation)

Metrics built (`feature_error`, `mesh_validity`) and wired into the benchmark.
Honest finding — **not a clean win**:
- ✅ **Robustness on hard-edged / box geometry** — the genuine strength. On a
  subdivided cube QuadriFlow catastrophically tears (598 boundary edges, 2.87%
  feature error) while ours is clean (0 defects, 0.3%). QuadriFlow degenerates on
  sharp box corners; we don't.
- ✅ **Validity on the smooth corpus — MET, and we beat QuadriFlow (2026-07-22).**
  Follow-up (a) below was a property of the **retired** position-field extractor,
  which the harness was still scoring. Re-measured on the shipped `quad-cover`
  default, topological defects are **0 on 4/5 models** and 8 vs QuadriFlow's 38
  on stanford-bunny — **5/5 ≥ QuadriFlow**. The same run shows the retired
  extractor's 110 (cheburashka) and 30 (fandisk), which is what the old
  "hole-fill doesn't close them — a real extractor bug" note referred to. It does
  not describe what ships.
  - ✅ **Now 0 on 5/5 (2026-07-23) — bunny's residual 8 was a bug, not its scan
    geometry.** `IsolineExtractor::fixHoleWithQuads` closed a boundary loop
    **twice**: `fixHoles` calls it once score-checked and once not, relying on the
    first pass to consume `hole` (an in/out parameter), and the terminal 4-gon
    branch returned with `hole` still full. Two coincident quads are
    edge-count-manifold on their own, so nothing downstream rejected them; the
    pure-quad subdivision then gave each its own face point and turned the shared
    rim into a genuine non-manifold edge. Measured on the shipped default over
    5 models × 5 densities (2600/3000/3400/3800/4200): **stanford-bunny 40 → 0**
    non-manifold edges (8/8/0/16/8 before), the other four 0 → 0 with 20 of 25
    cells bit-identical (rocker-arm @2600 keeps an identical face list, vertices
    move ≤1.1e-5). This is the mechanism behind bunny's long-recorded "defect
    lottery" — it was never density noise, it was whether the trace happened to
    leave a 4-edge loop. **Quote defect counts with the density they were measured
    at**; before the fix a single-density reading was not a floor.
- ❌ **Feature-following — NOT met, 0/5.** ⚠️ **Restated 2026-07-23 at matched
  achieved quad count** — the figures below the strikethrough were measured at a
  matched *request*, where QuadriFlow landed 11–16% denser, and `feature_error`
  falls roughly as `count^-0.5`. Count-matched: fandisk **0.75 vs 0.41** (1.83x),
  cheburashka **1.00 vs 0.56** (1.79x), rocker-arm **0.48 vs 0.42** (1.14x), spot
  **0.41 vs 0.35** (1.17x), stanford-bunny **1.36 vs 0.48** (confounded — 29% of
  its "features" are the scan hole's open boundary, which the metric counts as a
  feature). ~~fandisk 0.82/0.41 (2.0x), cheburashka 1.15/0.57 (2.0x), rocker-arm
  0.61/0.40 (1.5x), spot a tie 0.46/0.44.~~ **rocker-arm was never a 1.5x loss and
  spot was never a tie**; the two real gaps are fandisk and cheburashka. The datum
  that survives the restatement: **cheburashka is an organic character with 316
  genuine dihedral creases and a ~1.8x gap**, so this is broader than the "fandisk
  CAD residual" framing at the top of this doc.
- 🔴 **Cheap levers are exhausted — three measured and reverted.** Root cause is
  known (M1: the cross-field IS crease-aligned, but the integer grid **phase** is
  un-pinned, so loops sit ~half a cell off the creases — a grid-phase problem, not
  an alignment one). **M2a** post-extraction vertex snap: fandisk 1.20→1.05, still
  2x QF — a vertex snap cannot create a loop that isn't there. **M2b** crease
  gauge-pin in `solveSeamlessReduced`: no feature gain and **introduced 8 defects**,
  breaking the validity win. **M2c** (2026-07-22) lowering `CYBER_QC_ROUTE_CREASE`
  so lightly-creased meshes reach the feature-aware native solver: cheburashka
  1.15→1.06% (~8%, still 1.9x QF) but **rocker-arm regresses −7° median**, irr
  1→5% — net-negative, threshold stays 2%. See `cad-feature-robustness` memory.
**Follow-ups:** ~~(a) fix the extractor's scattered validity defects~~ *(done —
retired-extractor issue; the default is clean)*; ~~**(b) per-feature-edge integer
constraints in the parameterization** (QuadriFlow's `ComputeIndexMap` sharp-edge
path)~~ — **BUILT AND MEASURED INERT (2026-07-23), do not re-attempt as scoped.**
Two independent reasons, both measured:
  1. **Reach is 1/5 by routing.** `computeSeamlessUv` sends a mesh to the native
     solver only above a 2% interior-crease fraction. Measured: fandisk 0.0364,
     cheburashka 0.0142, rocker-arm 0.0074, spot 0.0051, bunny 0.0036. **Only
     fandisk clears it** — the other four never execute a line of the constraint
     code, so a native-solver lever cannot move them at all. Lowering the
     threshold is M2c, already dead.
  2. **The premise is false.** M1's "the cross field IS crease-aligned, only the
     grid phase is off" does not hold: fandisk's median 4-RoSy deviation from its
     crease directions measures ~21°, where a *random* field gives ~22.5°. There
     is nothing running along the creases to pin a grid to. The constraint
     mechanism was driven to exactness (crease level sets landing bit-exactly on
     the integer lattice) and still moved feature error by less than a sixth of
     the run-to-run noise, while costing median angle on the one model it reaches.
  **Open follow-ups: see the untried-lever list below.**
### Phase 3 close-out (2026-07-24)

**Two of three exit criteria are MET and one is closed as a known limitation.**

| criterion | verdict |
|---|---|
| Robustness on hard-surface geometry | ✅ **met** |
| Topological validity on the corpus | ✅ **met, and we beat QuadriFlow outright — 6/6** |
| Feature alignment | ⛔ **not met, closed as a known limitation — 1/6** |

**What we win, and it is the strongest claim in the project.** Topological
validity is **0 defects on all six corpus models**, at every density measured
(2600–4200), against QuadriFlow's **680 on the cube** and **32 on stanford-bunny**.
Nothing else in the roadmap is a clean corpus-wide win over the reference. It also
survived a real bug this cycle: bunny's long-recorded "defect lottery" turned out
to be a hole-filler closing a 4-edge loop twice (**40 → 0**), shipped in v0.2.4.

**What we do not win.** Feature-following is **1/6** — and read that honestly: the
single win is the **cube**, which was *added* to the corpus this cycle because we
already won it. We did not start beating QuadriFlow on any model we previously
lost. fandisk improved materially (**0.75 → 0.62**, gap 1.83x → 1.48x) and **still
loses**. cheburashka is untouched at 1.00 vs 0.56.

**Why it is being closed rather than continued — nine measured levers:**

| lever | layer | verdict |
|---|---|---|
| M2a vertex snap | post-extraction | dead |
| M2b gauge pin | solver gauge | dead, +8 defects |
| M2c routing threshold | routing | dead, rocker −7° |
| M2d per-edge integer constraints | parameterization | **refuted** (1/5 reach; premise false) |
| feature-degree widening | shared threshold | dead, median −5.9° |
| field alignment, ungated | field | regressed flat CAD |
| consistency gate | field | no-op |
| edge-ring planarity gate | field | under-applies |
| **(c1) crease preservation** | **pre-remesh** | ✅ **shipped** |
| **(c2) planarity-gated alignment** | **field** | ✅ **shipped** |
| (c1b) vendored crease visibility | vendored pre-remesh | **refuted** (count artifact) |

Two wins out of eleven attempts, both confined to the single model that clears the
2% native-routing gate. **The structural cap is the reason to stop:** 4 of 6 models
run on the vendored Geogram backend, and the one lever that reaches them (c1b) is a
count artifact that cannot be decoupled without patching vendored source. Any
further feature-following work must first answer *how it reaches the vendored path
at all* — not propose another constraint.

**If it is ever reopened**, the honest entry points are (c4) cone placement at
crease corners, (c6) crease-preserving surface projection, and (c7) a
globally-optimal direction field — all Tier 2, none cheap, and all still capped at
1/6 until the routing question is answered.

**Superseded exit note:** *robustness win on hard-surface geometry (met); validity
on the smooth corpus (met — now 6/6, was recorded 5/5 before the cube joined the
corpus); feature alignment (not met — was recorded 0/5).*

### Untried levers for retopology quality (2026-07-24)

Everything M1–M2d attacked lives in the **grid / constraint** layer. The 2026-07-23
refutation showed two problems *upstream* of that layer which nobody has touched:

- the cross field is **not** crease-aligned (fandisk ~21° median deviation from its
  crease directions; a *random* field gives ~22.5°), and
- the ARAP polish has **no restoring force toward the field at all** — median
  |rotation| climbs monotonically to ~43°, the maximum a 4-RoSy target can be off
  by, on both fandisk (CAD) and spot (organic).

So the levers below are ranked by that reframing, not by the old grid-phase story.
Anything already measured dead is listed at the end — check it before proposing.

**Tier 1 — opened by the refutation**

- ✅ **(c1) Preserve crease polylines through the isotropic pre-remesh — DONE
  (2026-07-24), the first lever that works.** Diagnosis confirmed and quantified
  directly: on fandisk at ~3000 quads the crease network went from **706 edges in
  ONE connected component** (2 dangling ends, 22 junctions) to **449 edges in 55
  components with 136 dangling ends** — 36% of the creases destroyed outright and
  the rest shattered. Root cause: `isotropicRemesh` *does* protect features
  (never collapses feature vertices, flips feature edges or smooths them —
  `isotropic.cpp:210/254/318`) but only sees what `tagFeatureEdges` marked, and
  that call takes an **included** angle, so the shipped 40 means "face-normal
  angle ≥ 140" — fandisk has **zero** such edges, so the remesher was told the
  part has no features. Fix: tag **wide before** the remesh (protect) and keep the
  **narrow tag after** it (seams unchanged), via `CYBER_QC_PRESERVE_CREASE_DEG`,
  default 135. Crease network now survives **exactly** (706 edges, 1 component, 2
  dangling — identical to the source). Measured: feature error −11.3% / −6.9% /
  −5.1% at 2600 / 3000 / 3400 quads, **median flat** (−0.02 / −0.45 / +0.72),
  normal error −1.3° / −0.6°, 0 defects, and **12 of 15 corpus cells
  byte-identical** (only fandisk routes native). One regression: fandisk@3000 edge
  CV 0.159 → 0.179. Costs ~15% more triangles in the working mesh, since creases
  can no longer be collapsed across.
- ✅ **(c2) Actually crease-align the cross field — DONE (2026-07-24), default on
  behind a planarity gate.** `computeCrossField` gained a
  `creaseAlignDegrees` parameter (`CYBER_QC_FIELD_CREASE_DEG` overrides,
  `CYBER_QC_FIELD_CREASE_DEG` overrides): any interior edge whose face-normal
  angle exceeds it pins its faces to the crease direction, while
  `Mesh::isFeatureEdge` — and therefore the seam set, period jumps and cut graph —
  is untouched. That split matters: widening the *shared* threshold instead costs
  median 83.4 → 77.5 (see the dead-lever list).
  - **Order is load-bearing, and it is now evidence rather than a guess.** Applied
    BEFORE (c1) it is net-negative — feature −4% but median 83.4 → 79.8 and
    irregular 3.3 → 5.0 — because pinning the field to 55 crease fragments with
    136 dangling ends injects conflicting directions. Applied AFTER (c1), with the
    crease network intact, it is a win.
  - ⚠️ **A 3-density sample said "blocked on a −2.3° median at 3000". That was
    under-sampling, not a defect.** Median-vs-density on fandisk jitters with
    sd 0.90° on *identical code* (81.10 / 81.95 / 82.97 / 83.40 / 82.61 / 81.16 /
    83.32 across 2600–3800). Re-measured over **7 densities**: feature
    0.7271 → 0.6846 (**−5.8%, better at 6/7**), edge CV 0.1656 → 0.1505 (**−9.1%,
    6/7**), irregular 3.63 → 3.31 (**−8.8%, 6/7**), median 82.36 → 81.94
    (**−0.42 ± 0.35, paired t ≈ 1.2, p ≈ 0.28 — not distinguishable from zero**).
    Three metrics improve; the fourth does not measurably move. **Lesson: sample
    density before calling a per-density delta a regression** — this is the same
    under-sampling class that produced the retracted Phase 2 result.
  - Corpus regression: **12 of 15 cells byte-identical** (only fandisk routes
    native), 0 defects everywhere.
  - 🔴 **REGRESSION found by visual inspection of the examples, after it had
    already been merged — it is now default OFF.** Two separate mistakes, both
    invisible to the corpus harness:
    1. **Flat CAD gets worse.** On the `04_sharp_edges` subdivided cube, c2 takes
       edge CV **0.201 → 0.397** and slivers **0.1% → 1.2%** (median 84.0 → 82.3).
       Pinning every face of a flat panel to its four differently-oriented
       boundary creases over-constrains a field that should stay smooth. c1 alone
       is strictly better on that model than both the pre-session baseline AND
       c1+c2. The corpus (spot/fandisk/rocker/cheburashka/bunny) contains **no
       flat-CAD model**, so it could not see this.
    2. **Blast radius was wider than measured.** `computeCrossField`
       has two callers — `seamless_solver.cpp:181` (native quad-cover, the one
       measured) and `field_quadrangulator.cpp:464`, the **field-aligned**
       quadrangulator that is the universal fallback and the flat-CAD route.
       Defaulting the parameter changed both. The harness only ever exercises
       `quad_method="quad-cover"`, so it was structurally blind to the second.
    **Three gate attempts, all measured (2026-07-24). The third works:**
    - ❌ **Consistency gate — measured NO-OP, do not retry.** Hypothesis: a face
      touching several creases (every triangle round a cube corner touches two
      perpendicular ones) pins to whichever comes first in vertex order, so
      neighbours resolve it differently. Gated on the mean-resultant length of the
      face's crease directions in 4-RoSy. It changed nothing on the cube
      (CV still 0.397 at 900 quads) because a crease-adjacent triangle almost
      always touches exactly ONE crease, so the gate never fires.
    - 🔬 **The mechanism is now measured, and it is NOT over-constraining.**
      `CYBER_QC_FIELD_STATS` reports the frozen fraction: crease alignment freezes
      **52.0% of fandisk's faces and improves it**, but only **14.7% of the cube's
      and degrades it**. fandisk freezes 3.5x more and gets better. The real
      distinction is that **a flat panel's cross field is degenerate** — every
      orientation is equally smooth — so pinning a border band imposes arbitrary
      structure the interior cannot reconcile, whereas a curved surface has a
      curvature-driven preference that crease pins reinforce. A gate must therefore
      test the *surface*, not the constraint set: skip alignment where the
      neighbourhood is planar.
    - ❌ **Planarity gate v1, EDGE ring — under-applies, do not use.** Skipped the pin
      when every neighbour across the face's non-crease *edges* was coplanar. It
      protected the cube, but it also gated off a genuinely curved fixture: a
      crease-adjacent triangle's edge-neighbours can all sit in the same row of the
      fold, and a developable surface is exactly coplanar along that row, so the
      test never sees the direction the surface actually bends in.
    - ✅ **Planarity gate v2, VERTEX ring — SHIPPED.** Same idea over the vertex
      ring, which reaches past that row, with faces more than 45° from the face
      excluded so the fold itself is not counted as its own evidence. Discriminates
      exactly: on a curved fixture 24 of 24 crease edges are pinned, on a flat one
      24 of 24 are gated off and the solve is bit-identical. Measured on fandisk
      over 7 densities vs c1 alone: **feature −4.6% (better 7/7)**, **edge CV
      0.1656 → 0.1472 (the best of every arm tried, better even than ungated)**,
      irregular 3.63 → 3.38, median −0.30 ± 0.31 (not distinguishable from zero).
      Flat CAD is **bit-identical**, and the corpus is **15/18 cells byte-identical**.
      Residual: fandisk@2600 alone regresses (median −1.79, CV +0.017, irregular
      +0.70) — one point inside the sd-0.90 density jitter, against a 7/7 feature
      win.
    - ✅ **The corpus blind spot is CLOSED.** `cube` is now a synthesised member of
      the benchmark corpus (`common.SYNTHETIC_MODELS`, `11_benchmark.DEFAULT_MODELS`).
      It immediately earned its place twice over: it reproduces the regression that
      slipped through (cube@900 edge CV 0.201 → 0.397), **and** it surfaced a win the
      scored benchmark could not previously see — on flat CAD we BEAT QuadriFlow on
      feature-following (**0.98% vs 2.69%**) with **0 defects against its 680**.
      Corpus-wide the headline moves from feature 0/5 to **1/6** and defects to
      **6/6**.
- ◻ **(c3) Give the ARAP polish a restoring force toward the field.** A *clamp* was
  tried (every face saturates whatever cap it is given: 5/10/20/30/45 → 5/10/20/30/44)
  and a 4-RoSy fundamental-domain wrap was tried (worse — map-vs-target 5°→17°). A
  **penalty term pulling the Jacobian back toward the field** is a different
  mechanism and was never built. Without it, (c2) cannot reach the map: the map
  runs ~24° off the field even with the polish disabled.

- ❌ **(c1b) Vendored-path crease visibility — REFUTED (2026-07-24). Do not retry.**
  The vendored solver never calls `setSharpEdgeDegrees`, so it runs at AutoRemesher's
  default 90° where Geogram's is 45, and at 90 the corpus sees almost none of its
  creases (constrained edges at 45 vs 90: fandisk 706/299, cheburashka 283/163,
  rocker-arm 223/10, bunny 379/20, spot 45/0). It looked like the exact analogue of
  (c1) applied where **4 of 6 models actually route** — the only visible path to
  cheburashka's 1.8x gap. **It is a COUNT ARTIFACT, exactly as an earlier workflow
  round claimed.** Lowering it inflates the mesh instead of aligning it: at 45°,
  quad counts go stanford-bunny 2644 → **15105 (+471%)**, rocker-arm +48%,
  cheburashka +31%. Since `feature_error` falls as `count^-0.5`, the apparent
  feature gains (cheburashka 1.15 → 0.80) are bought entirely with polygons. Where
  counts stay comparable the quality collapses: median rocker-arm −11.3°,
  cheburashka −8.9°, stanford-bunny −27°. 60° is milder and still fails (bunny
  +157% count, median −19°). Root cause it cannot escape: the value feeds BOTH the
  vendored pre-remesh (`autoremesher.cpp:291`) AND the quad_cover parameterizer's
  hard-edge constraints (`:534`), and unlike the native path those cannot be
  decoupled without patching vendored source — so it behaves like the shared-threshold
  widening that was already measured net-negative natively.

**Tier 2 — structural**

- ◻ **(c4) Force cones at crease corners / junctions.** Singularity *placement* is
  unconstrained today; part of QuadriFlow's crease behaviour comes from where its
  cones land.
- ◻ **(c5) Replace the 2% routing threshold with a measured best-of-both.** Today
  `creaseEdgeFraction ≥ 0.02` is a proxy reaching only 1/5 of the corpus, and M2c
  proved tuning the number is net-negative. Running *both* backends and scoring
  them (median / irregular / CV / defects) gives best-of on every model. Costs a
  second solve; must be kill-switchable. Also unblocks any native-solver work from
  being capped at 1/5.
- ◻ **(c6) Crease-preserving surface projection.** M2a found the output carries
  almost no detectable feature edges because *projection smooths creases* (only 17
  tagged on fandisk). The relax path pins feature vertices and the projection then
  undoes it. Helps feature error *and* median.
- 🔬 **(c7) Knöppel–Crane globally-optimal direction field** (native eigensolver)
  — **BUILT and measured 2026-07-24; a partial win, kept behind `CYBER_QC_KC_FIELD`.**
  Recorded as deferred "only if the Geogram dependency is ever forbidden" — i.e.
  deferred as a *dependency* play, never evaluated as a *quality* play. Given (c2),
  a better field is now on the critical path.
  - **What shipped (flag-gated).** `computeCrossFieldKnoppelCrane`
    (`src/quadrangulate/src/crossfield.cpp`) builds the true PER-FACE (dual)
    connection Laplacian `L = D − W(transport)` over the *same* per-face frames,
    transport phases and feature/crease pins as the iterative field (c1/c2 reused
    verbatim via the extracted `buildFrameAndConstraints` prologue), then takes its
    smallest generalized eigenvector by inverse power iteration on the existing
    spmv + CG — dependency-free, no SciPP. `M = L + εB` (Tikhonov shift → SPD),
    diagonal lumped-mass `B`, and feature/crease pins imposed by **exact reduced
    elimination** (constrained faces removed from the eigen-solve and held at their
    exact pin; `CYBER_QC_KC_PENALTY` restores the earlier `P=1e6` soft penalty pins —
    see lever (iii)). Warm-started from `computeCrossField`; **falls back to it on any
    CG divergence**, so KC can only help or no-op. Gated behind `CYBER_QC_KC_FIELD`;
    off ⇒ byte-identical (goldens pass, both backends).
  - **Result — reduces native spurious singularities and irregular %, moving toward
    Geogram (env `CYBER_QC_ROUTE_CREASE=0.0001` forces organics native; counts
    matched <5%):**
    | model | metric | current native | KC native (exact pins) | vendored (Geogram) |
    |---|---|---|---|---|
    | spot | irregular % | 4.63 | **2.96** | 1.99 |
    | spot | median | 79.93 | **82.64** | 84.24 |
    | spot | field singular= | 77 | **73** | — |
    | cheburashka | irregular % | 5.71 | **4.74** | 3.68 |
    | cheburashka | median | 79.72 | **81.97** | 80.38 |

    On the count-matched spot row KC now closes **~63%** of the native→Geogram
    irregular gap (4.63 → 2.96, landing **inside the 2–3% exit band**) and recovers
    median (+2.7 toward vendored). On cheburashka it still beats current native
    (5.71 → 4.74) though by less than the penalty variant (4.28 — see lever (iii)).
    The global field places fewer, better cones (spot 77 → 73, cheburashka 111 → 94),
    as the thesis predicted. It does **not** reach Geogram's 2.0% on spot, so it is a
    partial win, not a full close; not yet enough on its own to route organics native
    by default — held back chiefly by the feature-following trade-off (lever (iii)).
  - **Lever (i) dual-cotan edge weights — BUILT and REFUTED 2026-07-24.** Added the
    DEC-consistent dual-graph finite-volume weight `w = |shared edge| /
    dist(centroid_f, centroid_g)` (the Hodge star `⋆1` on the *dual* mesh — the
    cotan-analogue for a per-FACE field), opt-in behind `CYBER_QC_KC_DUAL_COTAN`,
    A/B'd against uniform at the same forced-native routing. **It does not beat
    uniform.** On the near-uniform remeshing corpus the ratio `|e|/|dual e|` is
    nearly constant (≈1.8; equilateral triangles give 1.73), so it barely differs
    from uniform, and on the count-matched spot row it *regresses*:

    | model | metric | KC uniform | KC dual-cotan |
    |---|---|---|---|
    | spot | irregular % | **3.81** | 4.18 |
    | spot | median | **82.23** | 80.51 |
    | spot | field singular= | **73** | 75 |
    | cheburashka | irregular % | 4.28 | **4.14** |
    | cheburashka | field singular= | **94** | 98 |

    Mixed and net-negative (fewer cones on both models under uniform), so uniform
    stays the KC default; dual-cotan is kept opt-in for reproducibility. The finding
    is informative: the ~2× spurious-singularity gap vs Geogram is dominated by the
    **per-FACE discretization**, not the edge weighting — reweighting the same
    face-based energy does not relocate the geometrically-pinned cones. Also
    hardened the inverse-iteration convergence monitor: it now stops on the B-norm
    *change in the field* (`deltaU`, sign-disambiguated) and logs the honest
    penalty-free Dirichlet energy `E = Σ w‖u_f − R u_g‖²`, instead of the raw
    Rayleigh quotient of `M` (which `P=1e6` penalty pins dominate); this also
    improved cheburashka KC (irregular 4.62 → 4.28).
  - **Lever (ii) paper-faithful per-VERTEX cotan connection Laplacian — BUILT and
    REFUTED 2026-07-24.** Implemented `computeCrossFieldKnoppelCraneVertex`
    (`src/quadrangulate/src/crossfield.cpp`, gated `CYBER_QC_KC_FIELD` +
    `CYBER_QC_KC_VERTEX`): the connection Laplacian on the *primal* mesh with the
    standard cotan weights `w = (cot α + cot β)/2` (negative weights clamped to keep
    it PSD) and barycentric per-vertex mass `B` — the exact discretization of the
    paper. The same inverse iteration (reused `solveSmallestField`) takes its
    smallest generalized eigenvector; feature/boundary vertices are pinned, and the
    converged per-vertex field is **projected onto the faces** the extractor reads,
    reasserting the per-face feature/crease pins so c1/c2 are byte-identical to the
    per-face field. Warm-started from and falling back to the iterative field.
    **It does not beat the per-FACE build — it is worse on both models, and on
    cheburashka it does not beat the current iterative field at all** (counts matched
    <5%, `CYBER_QC_ROUTE_CREASE=0.0001`):

    | model | metric | current native | KC per-FACE | KC per-VERTEX cotan |
    |---|---|---|---|---|
    | spot | field singular= | 77 | **73** | 81 |
    | spot | irregular % | 4.63 | **3.81** | 4.04 |
    | cheburashka | field singular= | 111 | **94** | 130 |
    | cheburashka | irregular % | 5.71 | **4.28** | 5.73 |

    Uniform-weight per-vertex (`CYBER_QC_KC_VERTEX_UNIFORM`) is *less bad* than cotan
    (spot singular 77, cheburashka 124) but still worse than per-face, so the cotan
    weighting is not the cause — **the vertex→face projection is.** The per-vertex
    eigenvector is a genuinely smoother *vertex* field (its Dirichlet energy
    converges), but the singularity metric reads the *per-face* field the extractor
    consumes, and the projection (averaging three vertex phases into a common 4-RoSy
    representative) re-winds phases across faces and *injects* cones. This is exactly
    the inert/regress failure mode flagged during design as the primary risk of any
    per-vertex build, now measured. The finding **confirms the per-FACE (dual)
    discretization is the correct domain**: it writes the field on the same domain the
    metric reads, with no lossy projection, which is why it — and not the
    paper-faithful vertex build — is the KC that helps. Per-face uniform-weight KC
    stays the KC default; the per-vertex build is kept opt-in for reproducibility.
    Both levers (i) dual-cotan and (ii) per-vertex cotan refuted, the residual spot
    3.81 → ~2.0 gap to Geogram is **not** a field-*discretization* gap reachable by
    reweighting or re-domaining the connection Laplacian — but see lever (iii), where
    the pin *formulation* (not the discretization) closes much of it.
  - **Lever (iii) exact reduced-elimination feature pins — BUILT 2026-07-24, now the
    KC default; a further irregular-% win on spot that TRADES OFF feature-following.**
    The `P=1e6` penalty pins are a *soft* Dirichlet BC (they distort `M`'s
    conditioning and hold the pin only to ~1e-6). Replaced them with **exact reduced
    elimination**: constrained faces are dropped from the eigen-solve (their rows made
    identity, their coupling to free faces moved to a constant RHS `−M_FC u_C`) and
    held at their exact pin, so `M = [M_FF, I]` is a clean SPD block system with the
    interior solved against the *exact* feature directions. Isolated in
    `assembleFaceKcSystem` + a `fixedMask` path in `solveSmallestField` (which
    B-normalises only the free block; the pinned block is a fixed inhomogeneity, not
    part of the `|u|=1` eigen-constraint). Default on; `CYBER_QC_KC_PENALTY` restores
    the soft pins. A/B at the same forced-native routing (counts matched <5%):

    | model | metric | current native | KC penalty pins | KC exact pins (default) |
    |---|---|---|---|---|
    | spot | irregular % | 4.63 | 3.81 | **2.96** |
    | spot | median | 79.93 | 82.23 | **82.64** |
    | spot | feature (↓) | 0.5035 | **0.4778** | 0.6648 |
    | spot | field singular= | 77 | 73 | 73 |
    | cheburashka | irregular % | 5.71 | **4.28** | 4.74 |
    | cheburashka | median | 79.72 | 81.48 | **81.97** |
    | cheburashka | feature (↓) | 0.8266 | 0.9791 | 1.038 |
    | cheburashka | field singular= | 111 | 94 | 94 |

    **Nuanced, and informative about the feature regression.** Exact pins give the best
    irregular % yet on spot (2.96, into the exit band) and the best median, but
    (a) on cheburashka they are *worse* on irregular than the penalty variant
    (4.74 vs 4.28), and (b) **feature-following gets *worse* on both organics**
    (spot 0.4778 → 0.6648, cheburashka 0.9791 → 1.038) — even though the pins are now
    held *exactly*. The field singularity count is identical to the penalty variant
    (73 / 94), so the irregular/feature deltas are entirely downstream cone *placement*,
    not cone *count*. This **refutes the "penalty-leak causes the feature regression"
    hypothesis**: hardening the pins to float precision does not recover crease
    adherence — it makes it slightly worse, because the cleaner (un-penalty-distorted)
    interior is *smoother* and so shears quad rows off the creases a little more. The
    feature regression is therefore an **interior global-smoothing** effect intrinsic to
    a globally-optimal field, not a boundary-condition artifact. Net: exact pins are the
    correct hard-constraint formulation and the strongest spot result, so they are the
    KC default, but the residual native→Geogram gap and the feature trade-off keep KC
    behind `CYBER_QC_KC_FIELD` — organics are **not** routed native by default. The
    remaining untested lever toward feature parity is a feature-neighbourhood alignment
    band (propagate the crease direction one–two rings into the interior to fight the
    smoothing shear), not a change to the connection Laplacian.

**Tier 3 — narrower, concrete**

- 🔬 **(c8) Finish the M3 open-surface cleanup — MIS-DIAGNOSED (2026-07-24). The
  prize is real; the named blocker is not the blocker.**
  - **Prize confirmed.** Open paraboloid at 1200 quads, `CYBER_QC_OPEN_CLEANUP`
    off vs on: faces **136 → 954**, median **52.8° → 78.7°**. The cleanup is what
    lets an open surface trace properly at all.
  - ❌ **The `simplifyGraph` turn-angle guard is NOT what blocks it.** Built it —
    dissolve a valence-2 node only when near-collinear (the two directions out of
    it at least 150° apart), so genuine rim corners survive. Measured: faces
    954 → 1066, median 78.7 → **75.6**, edge CV 1.696 → **1.890**. It makes both
    metrics slightly WORSE, and the flat-grid corner symptom the old TODO names
    ("interior 25→20, 7 triangles") did not reproduce. Reverted.
  - ✅ **SHIPPED (2026-07-24) — default on, opt out with `CYBER_QC_NO_OPEN_CLEANUP`.**
    Blocker was `simplifyGraph` over-dissolving
    valence-2 isoline samples on open surfaces.** Bisected the whole cleanup
    pipeline by env-gating each step on the open paraboloid at request 1200
    (all-quad, 0 defects throughout):
    | config | faces | median | edge CV |
    |---|---|---|---|
    | cleanup off (default) | 136 | 52.8 | 0.442 |
    | cleanup on (shipped opt-in) | 954 | 78.7 | **1.696** |
    | **fixHoles only, `simplifyGraph` removed** | 1920 | 68.7 | **0.312** |
    `fixHoles` is what makes the open trace work (it fills the under-traced gaps,
    136 → ~2000 faces). `simplifyGraph` is what wrecks uniformity: it dissolves
    every valence-2 node, and on a properly traced open surface most isoline
    samples are legitimately valence-2, so it merges cells into long uneven quads —
    roughly halving the face count (1920 → 954) and taking edge CV from **0.312 to
    1.696** on the SAME request. The collapse steps (`collapseShortEdges`,
    `collapseTriangles`, `removeSingleEndpoints`) are near-no-ops here; they only
    matter because they re-trigger `simplifyGraph`. This is a controlled toggle,
    not a count artifact: `simplifyGraph` is itself what changes the count.
  - **The fix: run the open cleanup WITHOUT `simplifyGraph` on open islands, and
    default it on.** Open islands now take the raw graph and skip straight to
    `fixHoles`; only closed islands run the collapse pipeline. Gated on the
    existing `m_preserveInputBoundary` (= `!closed`), so **the closed corpus is
    byte-identical** — verified 18/18 corpus cells and all 7 broken-robustness
    cases (2 open) still manifold. Regression guard:
    `python/cyberremesh/tests/test_open_surface_cleanup.py`, verified discriminating
    (fails on the 92-face under-trace and CV 0.93 with `CYBER_QC_NO_OPEN_CLEANUP`).
  - Effect where the paraboloid classifies open (low density): request 900 goes
    from **92 faces / median 27° / CV 0.93** to **~1744 quads / median 78° / CV
    0.27**. This was the largest untapped single-metric prize in the list; it is
    now the default.
- ◻ **(c9) Tube-aware coarsening** for the multiresolution cross field — named as
  "the real fix" after multires was found to help smooth models but bridge thin
  tubes (the bunny-ears case). Identified, never built.
- ◻ **(c10) cheburashka edge-CV** (0.22 vs QuadriFlow 0.15, the corpus's widest CV
  gap). Shape-match bought ~20% corpus-wide; nothing model-specific has been tried
  for the outlier.

**Measured dead — do not re-try** (each has a numbered entry above or in
`cad-feature-robustness`): M2a vertex snap · M2b gauge-pin · M2c routing-threshold ·
**M2d per-feature-edge integer constraints** · (4a) local valence optimization ·
multi-resolution coarse extraction (proven byte-identical no-op) · T-junction
cleanup / `FixFlipSat` · QuadriFlow flip-repair order · adaptive sizing for
quad-cover (irregular/CV explode) · equiareal MIQ term · min-cost-flow port ·
feature-degree sweep (re-measured 2026-07-24: widening the SHARED threshold moves feature 0.82 → 0.77 but costs median 83.4 → 77.5, CV 0.159 → 0.252 and irregular 3.3 → 7.1, because every extra tagged edge becomes another hard seam — this is what motivated splitting the preserve/seam thresholds in c1) · curvature-weighted seam routing · ARAP clamp · ARAP RoSy wrap.

⚠️ (c2) and (c3) are inferences from the ~21° / ~24° measurements, not themselves
measured hypotheses — A/B them like anything else.

## Phase 4 — Close the median-angle gap — ✅ largely closed by the quad-cover default

> ⚠️ **Premise superseded.** The framing below — "the hard core", 36% irregular,
> only a global rewrite can close it — describes the retired extractor. The
> shipped default runs at **1–4% irregular** and **beats QuadriFlow on median on
> 3/5** (spot 84/82, rocker 85/82, bunny 83/82; losing fandisk 83/85 and
> cheburashka 80/82). The remaining median gap is the crease-alignment problem
> tracked in Phase 3, not a singularity problem. The 4a/4b history is retained
> because it records what was measured and why the local levers failed.

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
