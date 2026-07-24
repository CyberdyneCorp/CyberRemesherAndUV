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

## Phase 3 — Win on features & robustness — 🟡 measured; MIXED (win on hard geometry)

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
**Exit (partial):** robustness win on hard-surface geometry *(met)*; validity on
the smooth corpus *(**met** — 5/5 vs QuadriFlow)*; feature alignment *(not met —
0/5, corpus-wide)*.

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
- 🟡 **(c2) Actually crease-align the cross field — MEASURED, promising, NOT
  shipped.** Hard directional constraints on crease edges in the 4-RoSy solve,
  without widening the hard-seam set (`isFeatureEdge`, period jumps and the cut
  graph untouched). **Alone it is net-negative** — feature −4% but median
  83.4 → 79.8, irregular 3.3 → 5.0 — because pinning the field to 55 fragments
  with 136 dangling ends injects conflicting directions. **On top of (c1) it
  roughly doubles c1's gain**: feature −18.9% / −12.4% / −12.8% vs baseline, with
  median and irregular *improving* at 2600 and 3400. Blocker: **median −2.3° at
  3000 specifically**. Worth finishing — understand why 3000 behaves differently
  before shipping. This ordering (c1 then c2) is now evidence, not a guess.
- ◻ **(c3) Give the ARAP polish a restoring force toward the field.** A *clamp* was
  tried (every face saturates whatever cap it is given: 5/10/20/30/45 → 5/10/20/30/44)
  and a 4-RoSy fundamental-domain wrap was tried (worse — map-vs-target 5°→17°). A
  **penalty term pulling the Jacobian back toward the field** is a different
  mechanism and was never built. Without it, (c2) cannot reach the map: the map
  runs ~24° off the field even with the polish disabled.

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
- ◻ **(c7) Knöppel–Crane globally-optimal direction field** (native eigensolver).
  Recorded as deferred "only if the Geogram dependency is ever forbidden" — i.e.
  deferred as a *dependency* play, never evaluated as a *quality* play. Given (c2),
  a better field is now on the critical path.

**Tier 3 — narrower, concrete**

- ◻ **(c8) Finish the M3 open-surface cleanup** — the `simplifyGraph` turn-angle
  guard is explicitly not done, which is why `CYBER_QC_OPEN_CLEANUP` stays opt-in.
  Measured prize on an open paraboloid: **median 50° → 80°**. Best
  value-per-effort item here.
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
