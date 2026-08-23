# Retopology Roadmap — Beating QuadriFlow-quality

Goal: make CyberRemesher's automatic quad retopology **better than QuadriFlow**
across four axes — quality-per-polygon, median quad angle, feature/CAD fidelity,
and robustness — not just competitive on one.

## Next — Bi-MDF quantization (openspec change `bimdf-quantization`)

Both remaining quality gaps converge on the same lever: flow-loop length
(41→433 landed; QuadriFlow 1811 — the residual ~4x is global grid structure)
and dense-organic singularities (dipole annihilation saturates at nefertiti
316-347 vs QF 80 — the residual cones come from per-seam greedy rounding).
The quantization-as-global-optimization answer (QuadWild's Bi-MDF, Heistermann
et al. 2023) replaces greedy integer rounding with a min-deviation-flow solve
over the T-mesh: papers only (GPL binaries stay external benchmarks), in-tree
min-cost flow, `CYBER_QC_BIMDF` gated, greedy fallback byte-exact. Exit gate:
spot flow_loop_mean_len >= 1000 count-matched AND nefertiti cyber-pure
singularities <= 200 with no recorded-metric regression, multi-density per the
measurement rules. See openspec/changes/bimdf-quantization/.

### Update — 2026-08-02 (branch `feat/bimdf-quantization`): pipeline landed opt-in; CAD path end-to-end, organics fall back at the T-mesh

What landed (all behind `CYBER_QC_BIMDF`; flag off is byte-exact, ctest 14/14):

- **Quantization scoreboard** (`CYBER_QC_DEBUG`): reduced Dirichlet energy
  E(w) = ½wᵀ(M+ridge)w − gᵀw printed at the relaxed optimum and after the
  final solve — greedy baselines at target 4000: box_sharp +1.588,
  spot +7.060, nefertiti +101.054. A second line reports the realized T-mesh
  arc-deviation energy whenever a T-mesh was built.
- **Motorcycle-graph T-mesh tracer** (`bimdf_quantize.cpp`): lockstep
  (smallest-curve-first) separatrix tracing with per-face contour following
  (fold-robust), seam transport, exact symbolic arc lengths over the promoted
  variables z=[u|v|t|c] (box exprErr 0, organics ≤1.2e-3), crease chains with
  stub pruning, combinatorial quarter-arithmetic rotations, quad-patch
  extraction with per-corner validation.
- **Bi-MDF S1 solve** (paper's approximate path, in half-cell units): split-node
  template, T-join parity adjustment (forest + DFS), Hochbaum double cover on
  the in-tree SPFA min-cost flow, convex relative-deviation costs as parallel
  arcs, min-one collapse guard with `raisedToMin` reporting. Unit-tested on
  synthetic T-meshes (cube classes, T-node consistency vs naive rounding,
  degenerate-arc guard).
- **Injection**: arc assignment back-substituted onto the free-integer basis
  through the run's actual pivot expressions (structural-violation arcs
  excluded), Gauss-Jordan, one-batch pin (tCap-validated, never clamped) +
  single `direct.resolve`; greedy finishes the remainder.
  `CYBER_QC_BIMDF=report` solves and reports without injecting.

Measured state: **box_sharp runs the full path** — textbook T-mesh
(8 nodes/12 arcs/6 patches, side mismatch 0), solve energy 0.017, zero side
violation, back-substitution maxFrac 0.0000, 5 integers injected, output
geometry identical to greedy (the flow agrees where greedy is already
optimal; recall 1.00 / 8 cones preserved trivially). **Organics fall back**:
the relaxed map's UV foldovers around index −1 cones corrupt patch corner
sectors — spot 107/222 patches non-quad, fandisk 71/110, nefertiti hits a
work-mesh crack; every fallback path reproduces greedy unchanged. The exit
gates (spot flow loops ≥ 1000, nefertiti ≤ 200 sings) are therefore NOT yet
reachable: the blocker is T-mesh extraction on folded relaxed maps, not the
flow solve. Candidate next steps, in value order: (a) locally-injective
relaxed substrate (or per-cone fold repair) before tracing; (b) per-patch
even-sum template (paper Fig 10d) so partially-valid T-meshes still quantize;
(c) the M=2 matching refinement (S2) once S1 bites. Default flip is NOT
proposed — opt-in only until the gates are met on the full corpus.

A/B table (CYBER_QC_BIMDF on vs off, output OBJ hashes compared with the
mtllib line stripped; `dE` = reduced-energy delta over the relaxed optimum,
identical on/off in every cell; `bench.py check` green with the flag on AND
off, box_sharp recall 1.00 / 8 cones / 0.0° both ways):

| mesh       | quads | T-mesh                          | injected | output |
|------------|------:|---------------------------------|---------:|--------|
| box_sharp  |  1000 | ok — 12 arcs / 6 patches        |        5 | SAME   |
| box_sharp  |  4000 | ok — 12 arcs / 6 patches        |        5 | SAME   |
| box_sharp  | 16000 | fail (crack in fan)             |        0 | SAME   |
| cylinder   |  1000 | fail (ray hit crack)            |        0 | SAME   |
| cylinder   |  4000 | fail (crack in fan)             |        0 | SAME   |
| sphere     |  4000 | fail (10/12 non-quad patches)   |        0 | SAME   |
| torus      |  4000 | fail (9/12 non-quad patches)    |        0 | SAME   |
| fandisk    |  4000 | fail (71/110 non-quad patches)  |        0 | SAME   |
| spot       |  4000 | fail (107/222 non-quad patches) |        0 | SAME   |
| nefertiti  |  4000 | fail (ray hit crack)            |        0 | SAME   |
| armadillo  |  4000 | fail (ray hit crack)            |        0 | SAME   |

### Update — 2026-08-02 (branch `feat/substrate-repair`): the "cracks" were kernel-made non-manifold edges — fixed; tracer is fold-tolerant at cone launches

Four-lens diagnosis (fold mechanism / crack census / literature / template
scoping) pinned both substrate failures to exact mechanisms:

- **"Work-mesh cracks" were never cracks.** Every closed corpus input reaches
  Bi-MDF with ZERO boundary edges; the defect was NON-MANIFOLD edges
  (edgeFaceCount>2: duplicate-triangle fins and merged-edge fans) created
  exclusively by the isotropic CollapsePass because `Mesh::collapseEdge` had
  no link condition. **Fixed in the kernel**: `collapseEdge` now refuses
  collapses whose endpoints share a neighbor beyond the incident-face apexes,
  and CollapsePass clears such obstructions with ordinary manifold-safe edge
  flips before retrying (a bare refusal deadlocked sliver bands at the
  cylinder density crossing: @3000 went open-surface, recall 0.77). With the
  flip resolution the default path IMPROVES: cylinder@3000 sing 18→8
  (recall 1.00, closed), @4500 sing 56→32, @1400 sing 15→6; box_sharp
  byte-identical at all 16 sweep cells; bench check green (cylinder@600
  trades quads 452→430, sing 6→5, angle 9.2→7.1 for haus 0.0052→0.0059,
  recall 0.96→0.95 — within all tolerances). Ellipsoid pure-quad nominals
  moved: median min angle 80.7→84.2, edgeCV 0.230→0.240 (threshold 0.25,
  justification in the test).
- **Folds are intrinsic to the hard-constrained map at negative-index cones**
  (a PL fan cannot wind >2π injectively): introduced by the reduced phase,
  doubled by rounding, ~95% within 1 edge of a cone. Per the literature
  (QEx/QGP/Bi-MDF), the industrial answer is a fold-TOLERANT tracer, not map
  repair. Landed: signed UV wedge tests at cone launches (candidates whose
  launch level provably exits their face rank first; UV-launchable rescue
  sites are admitted past the 3D margin cutoff), entry-edge re-crossing for
  folded triangles mid-trace, regular-vertex pass-through, and graceful
  abandonment of untraceable separatrices (T-mesh refused with statistics
  instead of aborting at the first fold; `failedRays`/`degraded` reported).

A/B table after both fixes (CYBER_QC_BIMDF=report; output still SAME
everywhere — no organic reaches injection yet):

| mesh       | quads | T-mesh (was → now)                                        |
|------------|------:|-----------------------------------------------------------|
| box_sharp  |  1000 | ok — 12 arcs / 6 patches, energy 0.014 = greedy            |
| box_sharp  |  4000 | ok — same                                                  |
| box_sharp  | 16000 | crack in fan → **ok, injectable (maxFrac 0.0000)**         |
| cylinder   |  1000 | ray hit crack → 15/32 non-quad                             |
| cylinder   |  4000 | crack in fan → 7/18 non-quad                               |
| sphere     |  4000 | 10/12 → 15/18 non-quad                                     |
| torus      |  4000 | 9/12 → 23/24 non-quad                                      |
| fandisk    |  4000 | 71/110 → 39/128 non-quad                                   |
| spot       |  4000 | 107/222 → **18/194 non-quad** (91% quad patches)           |
| nefertiti  |  4000 | ray hit crack → 596/2316 non-quad (first complete T-mesh)  |
| nefertiti  |  8000 | no level exit → 463/2015 non-quad                          |
| armadillo  |  4000 | ray hit crack → 294/1386 non-quad (79% quad)               |
| armadillo  |  8000 | no level exit → 309/1394 non-quad                          |

Corner-valence histograms of the rejected orbits (now printed in the tmesh
line): nefertiti@4000 = 306 degenerate (≤2 corners), 182 template-coverable
(3/5/6), 108 high (>6); spot = 8 degenerate / 6 coverable / 4 high. The
even-sum polygonal template (follow-up (b)) therefore still unblocks ZERO
meshes on its own — the degenerate bigons/monogons are fold-corrupted corner
sectors around the remaining folded wedges (rotation-side, not launch-side).
Exit gates (spot flow ≥ 1000, nefertiti pure sing ≤ 200) remain unreachable
until those sectors are repaired; next lever in rank order: QEx Alg-8
signed-angle sector classification in `buildRotation` (the launch-side
equivalent landed here), then the even-sum template as mop-up, then QGP §7.1
dynamic re-linearization. Default flip NOT proposed.

### Update — 2026-08-02 (branch `feat/bimdf-sectors`): QEx Alg-8 sector classification + even-sum polygonal template landed; blocker is now twin-ray bigons

The two ranked levers landed (`CYBER_QC_BIMDF` only; flag off byte-exact,
ctest 14/14, bench check green both flag states, CAD spot-check box+cylinder
32/32 cells match the recorded sweep):

- **QEx Algorithm 8 signed-angle sector classification** in `buildRotation`:
  signed per-wedge UV angles (folds negative, cancelling their double
  cover), maximal-sign runs with the mod-4 winding lifted to the smallest
  total that seats every incident arc end (a negative run is either a
  full-turn fold-back, +2π, or a small re-covered backtrack, +0 — the raw
  sum alone cannot tell), largest-remainder gap rounding, all-1 fast path
  when winding == end count. Along the way the SEAM HOLONOMY became the
  launch authority: at cones where the recorded field index disagrees with
  the fan's accumulated seam rotation (sphere 5/10, cylinder 2, fandisk 1;
  organics 0 — the "73 nefertiti mismatches" first measured were a diag
  artifact on |i|≥2 cones), the separatrix count now follows the holonomy
  (the map cannot close a layout around any other winding). Degraded nodes
  nefertiti@4000 306 → 44 (282 repaired).
- **Even-sum polygonal patch template** (paper §4.4): 3-6-corner patches
  with clean corner/pass-through sectors are accepted and quantized via one
  interior node per patch, a tail-tail routing edge per side and a
  head-head even-sum loop (σ=+2 ⇒ boundary sum = 2·loop, even by
  construction); T-join parity reaches interior nodes through the routing
  edges; `poly=`/`polyOdd=` reported, polyOdd 0 everywhere measured.
  Unit-tested (hexagon beside quads; odd-boundary pillow).

Rejected-patch counts (report mode, was @48a09cc → after Alg-8 → after
both; "deg" = degenerate ≤2-corner orbits):

| mesh       | quads | rejects was → Alg-8 → both | deg was → now |
|------------|------:|-----------------------------|---------------|
| box_sharp 1k/4k/16k | | ok — byte-identical, energy 0.014 | — |
| cylinder   |  1000 | 15/32 → 12/32 → **9/32**    | 9 → 8         |
| cylinder   |  4000 | 7/18 → 9/20 → **6/20**      | 4 → 5         |
| sphere     |  4000 | 15/18 → 16/22 → **12/22**   | 8 → 5         |
| torus      |  4000 | 23/24 → 17/20 → **14/22**   | 11 → 6        |
| fandisk    |  4000 | 39/128 → 37/124 → **31/124**| 20 → 17       |
| spot       |  4000 | 18/194 → 14/188 → **11/188**| 8 → 5         |
| nefertiti  |  4000 | 596/2316 → 554/2230 → **377/2230** | 306 → 224 |
| nefertiti  |  8000 | 463/2015 → 415/1939 → **268/1939** | 215 → 152 |
| armadillo  |  4000 | 294/1386 → 259/1334 → **174/1334** | 144 → 107 |
| armadillo  |  8000 | 309/1394 → 289/1352 → **198/1352** | 153 → 119 |

Injection coverage: box_sharp only (all densities; @1000 injects 5
integers, maxFrac 0.0000; @600 back-substitutes to exact half-integers,
maxFrac 0.5, pins 0 — output byte-identical to greedy either way). Full A/B
(on vs off, cyber + cyber-pure, corpus + spot@3000/nefertiti@8000/
armadillo@8000): every cell's output hash SAME, dE identical (spot mixed
7.57 / pure 7.89; nefertiti mixed 94.59 / pure 164.69; armadillo mixed
52.02 / pure 72.05), all metrics unchanged (spot pure flow 762.9;
nefertiti pure sing 518). Bunny@3000 BIMDF=on: input has open boundaries →
tracer refuses ("ray reached an open boundary") → fallback; ear irregulars
37 = recorded greedy (QuadriFlow 20).

**Exit gates NOT met** (spot flow ≥ 1000, nefertiti pure sing ≤ 200): no
organic reaches injection. The blocker is no longer sector classification:
the remaining rejects are dominated by TWIN-SEPARATRIX BIGONS and the
spur/wander orbits they induce (orbit dumps show the same arc traversed on
both sides and twin arc pairs cone→same-target, e.g. nefertiti a1135/a1136
from one cone; 2-corner orbits 201 of 377 rejects at @4000), plus
`failedRays` (nefertiti 31/3338, armadillo 3/1946) which refuses the
T-mesh outright. Next levers in rank order: (a) QEx Alg-7-style T-mesh
simplification — merge coincident twin arcs / collapse zero-width bigon
strips before patch validation; (b) eliminate residual `failedRays` (alt
launch sites at the handful of exhausted cones); (c) QGP §7.1 dynamic
re-linearization to reduce fold damage at the source. Default flip NOT
proposed.

### Update — 2026-08-03 (branch `feat/bimdf-tail`): twin-arc merge + local containment landed; organics now BUILD and SOLVE end-to-end; the injection blocker is the joint half-integer lattice

Three levers landed (`CYBER_QC_BIMDF` only; flag off byte-exact by
construction — no non-flag path touched; ctest 14/14; bench check green
both flag states, box_sharp row identical: 8 cones / recall 1.00 / 0.0°):

- **Twin-arc merge / bigon collapse** (QEx-sanitization spirit, arc-pair
  driven): two live non-feature arcs with the same endpoints and near-zero
  relaxed length bound a provably zero-area pocket and MERGE; when the
  opposite endpoint is a plain 3-valent T-node the whole pocket FUSES into
  the launching cone (both sides dropped, the crossing ray's continuation
  retargeted). Merged twins are theta-coincident, so the survivor carries a
  PHANTOM quarter (buildRotation widens its sector and lifts the winding
  target). Fixpoint loop with orbit re-tracing; guards: feature arcs
  untouched, measurable-extent pockets (genuine thin strips) left to the
  quantizer, retargets must anchor in the cone fan, no surgery may orphan a
  cone. Rejected orbits (was round-2 → now): nefertiti@4000 377 → **143**
  (deg ≤2: 224 → 49), @8000 268 → **92**; armadillo@4000 174 → **45**,
  @8000 198 → **48**; spot 11 → **3**; fandisk 31 → **16**; cylinder@1000
  9 → **4**. Explicitly reverted after measurement: zero-length T-T node
  contraction (fixed spot's last knot but degraded nefertiti 143 → 201 via
  cross-chart T-node fans — recorded as a dead end).
- **Local containment (failedRays + rejected orbits)**: a failed launch or
  rejected orbit no longer refuses the T-mesh. Rejected orbits' arcs are
  EXCLUDED from injection (greedy rounds them locally); orbits touching
  abandoned-launch cones and 4-corner orbits whose RELAXED opposite sides
  mismatch (> 0.75 cells — wander orbits closing around fold damage) are
  contained too. Solve-side support: excluded-interior arcs leave the
  network, boundary arcs enter their one side as fixed constants (a
  dangling variable side needs a free cover loop, whose copy-crossing edges
  MEASURABLY caused the half-integral solutions), fixed arcs hang from a
  virtual ground in the T-join forest (odd components repair through them),
  and a cover-infeasibility valve drops deficit patches and retries.
  Anti-collapse floor moved from arcs to SIDES (an arc that is a whole side
  keeps min-one; subdividing arcs may hit zero — a blanket per-arc min-one
  inflated nefertiti@4000 by ~2500 half-cells). Result: nefertiti@4000
  tmesh ok=1 with 1896/2078 patches accepted, solve halfIntegral 0,
  sideViolation 2, polyOdd 5, energy 1309 vs greedy-realized 1879; spot
  184/189; armadillo, fandisk, cylinder, sphere, torus all ok=1.
- **Open-boundary containment (bunny unblocked at the T-mesh level)**: rays
  reaching an open boundary are abandoned + contained, boundary vertex fans
  degrade instead of hard-failing. stanford-bunny@3000 now builds a
  441-patch T-mesh (12 failed rays, 30 excluded orbits) and solves cleanly
  (halfIntegral 0, polyOdd 0). Honest scope: true boundary ARCS (QGP-style
  boundary chains with length variables) are NOT implemented; boundary
  regions fall back to greedy locally.

**THE INJECTION BLOCKER, precisely diagnosed**: with organics building and
solving, back-substitution now runs — and the elimination leaves ~half the
pivots at EXACTLY frac 0.5 (spot 71/136 clean, nefertiti 861/1649, bunny
190/366; parity census: all 510 spot arc rows have half-integer reduced
coefficients). The Bi-MDF network models per-patch consistency + polygonal
evenness, but NOT the joint parity lattice of the half-integer cone
positions: a Bi-MDF-consistent x is generically outside the image of the
integer free basis, so exact injection is impossible and the residue is
structural (dropping "inconsistent" rows cascades to 350/515 arcs —
measured). Partial pinning (only cleanly-determined variables,
`CYBER_QC_BIMDF=force`) was fully measured: bunny@3000 WITH per-arc floor 0
is a real win (sing 135 → 120, ear irregulars 37 → **31** vs QuadriFlow
20, flow 37.5 → 38.9), spot mixed (flow 97.3 → 106.3, sing 41 → 44), but
nefertiti regresses (396 → 431) and with side floors the bunny win
reverses (ears 42) — the pinned subset assumes flow values for the
unpinned rest and greedy gives them something else. DEFAULT injection
therefore requires FULL consistency: every pivot clean AND zero contained
arcs (an exact-but-partial inject on cylinder@1000 cost haus 0.037 →
0.067; box_sharp@1000 pure sing 19 → 23 — both reverted by the gate).

**A/B (on vs off × cyber/cyber-pure × corpus + spot@3000/4000,
nefertiti@4000/8000, armadillo@4000/8000)**: 21/22 cells hash-SAME with dE
identical (spot pure 7.89, nefertiti@4000 pure 185.28/@8000 164.69,
armadillo pure 92.96/72.05 — @4000 shifts vs round 2 come from the
BVH-merge substrate, not the flag). The one differing cell is
box_sharp@1000 cyber-pure: a FULLY-consistent exact injection choosing
different integers than greedy (dE 5.518 → 5.521, sing 19 → 23) — the
intended flag-on semantics, on by opt-in only. CAD sweep spot-check
(box_sharp + cylinder × 8 densities × 2 arms, flag ON vs the recorded
sweep): 31/32 cells unchanged (quads/sing exact; angle diffs ≤ 3e-3 exist
flag-OFF too — recorded-CSV float noise, verified cell by cell); the one
structural change is box_sharp@400 cyber-pure, again a fully-consistent
exact injection: sing 46 → 30, angle 30.9° → 26.1°, quads 406 → 268
(further from request). Exit gates measured on the unchanged organics:
spot@3000 pure flow_loop **762.9** (gate ≥ 1000), nefertiti@4000 pure sing
**419** (gate ≤ 200) — **NOT met**; injection never engages on organics
under the safety gate. Bunny ears 37 (= greedy; **31** under
force+floor-0, the closest measured approach to QuadriFlow's 20).

Ranked next levers: (a) **parity-aware quantization** — derive the joint
lattice constraints (Smith/Hermite form of the reduced arc system, or arc
parity classes from the cone half-integer offsets) and feed them into the
Bi-MDF as parity constraints, making the flow solution exactly liftable;
(b) **guided rounding** — keep y integral by construction: run the greedy
schedule but choose each pinned integer to minimize the residual of the
Bi-MDF arc targets (captures min-one strip-opening without exact lifting);
(c) boundary arcs for open surfaces. Default flip NOT proposed.

### Update — 2026-08-03 (branch `feat/bimdf-guided`): guided rounding landed as flow ATTRACTION on the greedy schedule; bunny ears 37 → 19 (beats QuadriFlow's 20); spot pure flow-loop 763 → 1274

`CYBER_QC_BIMDF=guided`: the greedy schedule stays the quantizer (every
intermediate state integral + seamless by its own invariant — no lifting
problem), but its re-solves minimize `E(w) + 1/2 Σ_a mu_a (row_a·w −
len_a)²` over the T-mesh arc rows, pulling the relaxed values toward the
Bi-MDF assignment so the UNTOUCHED rounding logic commits flow-consistent
integers; the final solve after the last pin runs unattracted (continuous
DOF stay pure Dirichlet over the chosen integers). `mu_a =
mu/max(len_a,0.5)` (the Bi-MDF deviation normalization), default mu 1.0
(`CYBER_QC_BIMDF_MU`); bunny basin mu 0.75/1.0/1.25/1.5 → sing
101/94/91/131 — flat around 1. The direct engine hands off to the masked
CG while the attraction is active. Exact injection is UNCHANGED
(box_sharp@1000 pure under guided: injected=6, output byte-identical to
flag-on).

**Two measured design falsifications first** (why attraction, and toward
WHICH flow):
- Coordinate-wise steering (floor/ceil per integer scored against the arc
  targets, three scoring variants incl. min-one shortfall) always lost:
  nefertiti@4000 sing 396 → 418/399/422 — arcs are functions of many
  integers, and per-coordinate moves toward the flow wreck the coupled
  remainder.
- The SHIPPED side-floor solve has nothing worth steering toward: its
  deviation energy sits ABOVE the greedy realization (nefertiti@4000
  2711 vs 1879; bunny 397 vs 355 — `raisedToMin` floor inflation). The
  steering therefore consults a floors-OFF solve (`BimdfOptions
  .sideFloors=false`: no side min-one, T-join may close unit arcs to
  their true lower bound): energy 1289 vs greedy 1879 on nefertiti —
  the real headroom, matching round-3's 1309.

**Health gates — guided is an ORGANIC lever** (both measured, both
auto-refuse to pure greedy):
1. `maxSideMismatch <= 0.2`: fold-damaged flows regress under steering
   (nefertiti mm 0.435: sing 396 → 422; armadillo mm 0.496: 255 → 285);
   healthy T-meshes win (spot 0.001, bunny 0.065).
2. crease arcs <= 1% of arcs: steering fights the pinned crease lattice
   on CAD — cylinder@4500 pure collapsed (quads 3792 → 1702, haus 0.0048
   → 0.0499), cylinder@900/@1400 mixed sing 8 → 18 / 2 → 7 (flag-off
   verified to match the recorded sweep on every one of those cells, so
   the damage was steering-caused). cylinder/box/fandisk carry 7.6-18%
   crease arcs, organics ~0 (bunny 1/1338). Forfeits the measured fandisk
   mixed win (sing 50 → 43) for the guarantee.

**Where guided engages (mu=1), guided vs greedy**:
| cell | sing | ears(y>0.125) | flow_loop (loops) | angle | haus | recall |
|---|---|---|---|---|---|---|
| bunny@3000 mixed | 135 → **94** | 37 → **19** (QF 20, round-3 force best 31) | 37.5 → 31.9 (132 → 150) | 7.89 → 8.18 | 0.0079 → **0.0073** | 0.609 → **0.634** |
| spot@3000 pure | 66 → **65** | — | 762.9 → **1274.0** (7 → 4) | 7.92 → **7.69** | 0.0053 → **0.0052** | 0.762 → 0.738 |
| spot@3000 mixed | 39 → 40 | — | 67.8 → **74.6** (74 → 68) | 6.55 → 6.83 | 0.0068 → 0.0070 | 0.698 → 0.674 |
| sphere pure | 17 → 18 | — | 280.8 → 692.0 (5 → 2) | 8.68 → 8.97 | same | — |
| torus pure | engaged, output identical (seam-translation gauge shifts only) |||||

Refused cells (nefertiti@4000/@8000 mixed, armadillo both, all creased
CAD) and never-engaging cells (pure-arm organics whose T-mesh does not
build — nefertiti/armadillo/bunny pure) are byte-identical to flag off.
A/B 22 cells off vs report: hash-SAME 22/22. ctest 14/14; bench check
green BOTH flag states (box_sharp row identical: 8 cones / recall 1.00 /
0.0°); flag-off byte-exact vs the main-branch binary (spot pure,
box_sharp@600 pure, nefertiti@4000 mixed, bunny mixed — mtllib-normalized
hashes SAME); guided deterministic run-to-run. CAD sweep spot-check under
guided: box_sharp+cylinder × 8 densities × 2 arms vs the recorded sweep —
**31/32 unchanged** after the crease gate; the single diff is
box_sharp@400 cyber-pure, the flag-on exact injection round 3 documented,
with identical numbers (quads 406 → 268, sing 46 → 30).

**Exit gates**: spot@3000 pure flow_loop **1274 ≥ 1000** — MET on the
mean, with the honest caveat that the loop census shrank (7 → 4 loops,
quads 2670 → 2548, −4.6%), so "count-matched" is arguable. nefertiti@4000
pure sing ≤ 200 — **NOT met, and unreachable by ANY rounding lever**: the
pure-arm T-mesh does not build (`trail density blowup (wandering
separatrix)`), so no flow information exists to guide with; the wall is
the tracer on the pure path, not the quantizer. Bunny ears 19 vs
QuadriFlow 20 is the round's durable prize.

**Parity-aware quantization (lever a): assessed, NOT implemented —
payoff measured as capped.** The joint parity classes couple arcs through
the all-half-integer reduced rows (round-3 census), so enforcing them is
a coupled GF(2) congruence system on top of the min-cost flow — beyond
the graphic T-join machinery. This round adds the decisive evidence that
exact liftability is not the bottleneck: realizing the flow assignment
exactly is HARMFUL precisely where injection is still blocked
(fold-damaged organics regress under force pinning AND under attraction),
and where the T-mesh is healthy the attraction already banks the win
without lifting (ears 19 vs QF 20). The pure-arm organics fail earlier
(tracer). Parity constraints would unlock exact injection only on meshes
where guided already realizes the benefit. Dropped from the ranked
levers; the tracer's pure-path density blowup takes its slot.

Ranked next: (a) pure-arm T-mesh tracer robustness (the `trail density
blowup` wall — nefertiti/armadillo/bunny pure never reach the flow); (b)
boundary arcs for open surfaces; (c) revisit the crease gate with
crease-aware attraction (recover the forfeited fandisk/cylinder mixed
wins). Default flip NOT proposed: guided is opt-in, wins are
organic-scoped, spot mixed recall −0.023 and sphere pure sing +1 are
real (small) costs.

### Update — 2026-08-03 (branch `feat/bimdf-boundary`): pure-arm tracer wall fixed (self-spiralling separatrices) — every pure organic now builds and solves; QGP boundary arcs landed opt-in; the nefertiti pure gate is measured and NOT met (411, wall = quarter-density fold damage)

**Lever 1 — the pure-arm tracer wall, diagnosed and fixed.** Report-mode
measurement with a per-face trail-density histogram found the mechanism:
the guard trip on nefertiti@4000 pure is ONE separatrix (cone-v 48843)
spiralling through the same face — 511 of the 512 segments in the
tripping face were its OWN trail (the mixed arm's densest face holds 9),
because the crossing scan exempted a ray's own trail from T-junction
termination. Not density miscalibration (mesh average 2.4 segs/face), not
spacing (the guard never fires legitimately). Fixes, all `CYBER_QC_BIMDF`
only:
- Self-hit termination (the canonical motorcycle-graph rule: a ray stops
  at ANY strictly-older trail, its own included) behind a 0.25-cell
  curve-age margin protecting the just-flushed junction point.
- Residual guard trips (parallel self-spirals present no perpendicular
  trail to stop at) ABANDON the ray and contain its region instead of
  refusing the whole T-mesh.
- `arc end not anchored in fan` (armadillo pure — a fold-torn vertex fan
  closing a shorter cycle than an arc's anchor wedge) degrades the node
  into containment instead of hard-failing the build.

Every pure-arm organic now builds and solves: nefertiti@4000 pure 2644
patches (371 contained, sideMismatch 0.731, failedRays 64), armadillo
1392 (164, 0.594), bunny@3000 pure 191/322 (0.704). All round-4 engaged
cells reproduce exactly (bunny ears 19 / sing 94, spot pure flow 1274,
sphere 800 pure 18, torus identical, box_sharp@1000 pure injected=6).

**THE GATE (nefertiti@4000 cyber-pure sing ≤ 200): measured, NOT met.**
With the T-mesh finally built, guided was engaged through the
`CYBER_QC_BIMDF_HEALTH` measurement override: sing 419 (greedy) → 411 at
mu=1; mu=2 forces 310 but costs haus 0.0072 → 0.0194, quads −14% and
flow-loop 456 → 46 — the trajectory shows the gate unreachable by
attraction strength. Engaging fold-damaged meshes regressed every cell
measured (nefertiti mixed 396 → 400, armadillo mixed 255 → 281,
armadillo pure 273 → 293, health-tightening to 0.05 gave 420), so the
round-4 whole-mesh health gate STAYS (a per-arc filter inside engaged
meshes remains — no-op on healthy T-meshes). The wall is the
quarter-density substrate itself: 1297 cones and 110 fold-degraded nodes
on a 15k-face work mesh — rounding levers cap out; the ranked fix is
fold repair / locally-injective substrate, not quantization.

**Lever 2 — QGP boundary arcs, landed opt-in (`CYBER_QC_BIMDF_BARC=1`),
default OFF by measurement.** Boundary loops decompose into boundary arc
chains; separatrices landing on the boundary terminate at T-nodes (all
three ends anchor in the hit face — no seam transport needed); boundary
arcs carry min-one floors, UV-polyline relaxed lengths, no symbolic
expression (`Arc::noExpr` — the free boundary is not grid-aligned), and
enter their single patch side. Two designs measured and falsified:
- VARIABLE boundary arcs (paper semantics) paired through a balance-free
  cover hub produced BIT-IDENTICAL assignments to fixed constants on
  bunny@3000 (the deviation optimum keeps boundary arcs at their rounded
  lengths) while adding the copy-crossing structure round 3 measured to
  go half-integral — boundary arcs therefore enter as FIXED constants
  (the round-3 one-sided machinery); interior arcs of boundary patches
  stay variables tied through side consistency. The floors solve's
  halfIntegral 0 → 12 / sideViolation 5 on bunny arises from the
  boundary PATCH structure itself under either encoding; the floors-off
  steering solve stays integral (half=0).
- Boundary termination as DEFAULT: bunny@3000 mixed coverage rises
  441/471 → 457/485 (failedRays 12 → 3; pure arm 223/364, failedRays
  42 → 15) but the recovered boundary regions reshape the T-mesh and its
  flow globally and the guided flagship regresses across the whole mu
  basin — ears 19 → 37/33/31 at mu 0.75/1.0/1.25, sing 94 → 115-129,
  with the boundary-adjacent steer rows excluded it is STILL 33 —
  round-3's containment of boundary neighborhoods is load-bearing for
  the round-4 win. Default off keeps byte-identical behavior (verified:
  bunny mixed guided ears 19 / sing 94, deterministic run-to-run).

**Gates**: ctest 14/14; bench check green off/report/guided; off vs
report hash-SAME 22/22 (corpus + spot@3000 + nefertiti@4000/8000 +
armadillo@4000/8000 + bunny@3000, both arms, mtllib-normalized);
flag-off byte-exact vs the main-branch binary (spot pure, box_sharp@600
pure, nefertiti@4000 mixed, bunny mixed); CAD sweep spot-check under
guided 31/32 unchanged (the 1 diff = the round-3-documented
box_sharp@400 pure exact injection, identical numbers); box_sharp exact
injection byte-identical. sphere@4000 pure guided differs from greedy
(sing 39 → 56, quads +33%) — verified BIT-IDENTICAL on the round-4
binary, i.e. pre-existing opt-in behavior outside the tracked cells,
not this round's change. Default flip NOT proposed (unchanged
evidence: wins organic-scoped and opt-in; nefertiti gate unmet).

Ranked next: (a) fold repair on the quarter-density pure substrate
(locally-injective relaxed map / QGP §7.1 re-linearization) — now THE
blocker for the nefertiti pure gate with the tracer wall gone; (b)
boundary-region flow quality (make BARC default-viable: why do the
recovered regions steer the global solution off the greedy optimum);
(c) crease-aware attraction (unchanged).

### Update — 2026-08-03 (branch `feat/fold-repair`): substrate fold repair landed (Winslow untangler, folds −91%); the nefertiti pure gate stays NOT met — the measured residue is CONE COUNT, not folds

**Fold census (re)landed** as permanent gated instrumentation
(`CYBER_FOLD_DIAG`): per-phase folded-face counts with cone-distance
attribution (d0/d1/d2/3+) at poisson / arap / reduced-preint /
reduced-repaired / final. nefertiti@4000 pure substrate (15 288 faces):
poisson 28 → arap 15 → reduced-preint **482** (89% d0) → final 1373 —
the diagnosis reproduced at quarter density: the hard-constrained
reduced phase introduces the folds, cone-local.

**Lever 1 — QGP §7.1 dynamic re-linearization: BUILT, MEASURED,
DEFAULTED OFF (`CYBER_QC_FOLD_RELIN`).** Three formulations measured on
nefertiti@4000 pure:
- Monotone active set (QGP-faithful equality penalties, λ=1e4): the
  equality pulls recovered faces back DOWN to the margin and the map
  cascades — folds 482 → 7211, 14 293/15 288 faces active.
- Normalized rows + proximal damping: weakly-controlled faces (tiny
  continuous gradient) become unreachable unit-norm targets whose
  forcing explodes — 6 400-cell displacements at ANY λ (the reduced
  operator has near-floppy directions at the 1e-8 ridge; penalty rows
  were also restricted to continuous frees for the same reason).
- Raw rows, relative-area weight λ/refA², one-sided per-iteration
  active set, best-iterate keep: STABLE but weak — 482 → 382 at λ=1,
  and the margin-slivers it produces break the tracer ("collinear
  separatrix overlap"). Rewinding a >2π wedge at a negative cone is
  intrinsically nonlinear; a convexified linear push cannot do it.

**Lever 2 — regularized-Winslow substrate untangling (Garanzha et al.
2021): LANDED, default `auto`.** Global minimization of the regularized
Winslow energy over the reduced free basis (z = Tuv·w keeps every
iterate EXACTLY seamless — the "fixed k-ring boundary" of the plan is
replaced by the reduction itself, which transports every wedge copy
consistently; per-cone patches would have been seam-coupled anyway).
L-BFGS + Armijo, paper eps-schedule, all double. Substrate-only: the
untangled map feeds ONLY the T-mesh tracer; w is restored so the
rounding path is untouched. Results: nefertiti pure folds **482 → 45**
(0.46s), armadillo pure **198 → 9** (0.15s), bunny **41 → 0** — the
~45 residual folds sit at seam-locked cones (minDet stalls at −0.02;
stall guard stops the stages). Two supporting changes: `bimdf::Charts`
u/v became DOUBLE (a float substrate splits twin separatrix levels by
one ulp, tripping the tracer's collinear-overlap hazard band — measured
dc=5.96e-8), and `auto` engages only above 1% folded faces (always-on
cost the bunny flagship ears 19 → 20 / sing 94 → 100 for zero benefit).

**Chain re-measure (the honest part).** T-mesh on the repaired
substrate: contained regions 371 → 314, sideMismatch 0.731 → 0.627,
twinMerges 491 → 247, repaired 570 → 278 — but degraded nodes 110 → 125
and failedRays 64 → 73 (the repair rearranges fans; classification does
NOT go to ~0). Guided still refuses at the 0.2 health gate; engaged via
`CYBER_QC_BIMDF_HEALTH` (now a full measurement override — it also
skips the crease-fraction refusal, which nefertiti's 293 crease arcs
otherwise trip):

| nefertiti@4000 pure | sing | quads | haus | recall | loops |
|---|---|---|---|---|---|
| greedy (=r5) | 419 | 4108 | 0.0072 | 0.738 | 18/456 |
| guided eng., raw substrate | 439 | 4098 | 0.0059 | 0.784 | 19/431 |
| guided eng. + untangle | 353 | 3403 | 0.0308 | 0.631 | 96/71 |
| + steer-consistency filter (0.25) | 333 | 3355 | 0.0267 | 0.613 | 102/66 |
| multires field, greedy | 385 | 4104 | 0.0054 | 0.796 | 20/410 |
| multires + guided + untangle | **358** | 3722 | 0.0066 | 0.731 | 19/392 |
| multires + untangle + filter | 378 | 3888 | 0.0059 | 0.765 | 16/486 |

armadillo@4000 pure: greedy 273; guided+untangle+filter 238 (haus
0.0172, recall 0.575); multires greedy 303 with the best geometry
(quads 3916, recall 0.675). The steering-vs-substrate tension is
structural: the flow's targets are measured on the repaired map while
the rounding realizes the Dirichlet map, so every sing win below ~390
is bought with quad-count collapse and haus damage; the per-arc
consistency filter (`CYBER_QC_BIMDF_STEERTOL`, default 0 — on the
healthy bunny even ONE skipped arc moved ears 19 → 22) trades along the
same frontier.

**GATE VERDICT: nefertiti cyber-pure sing ≤ 200 NOT met — best 333
(geometry-damaged) / 358 (geometry-sane); QuadriFlow 80.** The measured
residue is the CONE POPULATION, not the folds: 1297 field cones on the
15k-face substrate, and `CYBER_QC_CROSSFIELD_MULTIRES` (measured on the
pure arm only; its torus regression keeps it off) removes just 29
(1297 → 1268). Quantization already collapses ~⅔ of the cones
(1297 → ~400 output sing); no substrate or steering lever changes the
cone budget. Fold repair was the last map-level lever; the redirect is
FIELD-level dipole annihilation at coarse scale (multires un-gating
alone is measured insufficient).

**Gates**: ctest 14/14; bench check green off/report/guided; bunny
default guided ears 19 / sing 94 exact (`auto` skips its 0.2% folds);
box_sharp@1000 pure exact injection (maxFrac 0.0000, injected 6); CAD
guided spot-check vs main 16/16 metric-SAME (box+cylinder ×
400/900/1400/4500 × both arms); organics default guided vs main 5/5
metric-SAME (spot pure/mixed, bunny, nefertiti pure, armadillo pure);
off-vs-report 18/18 metric-SAME. NOTE: byte-hash gates are retired on
this machine — the MAIN binary itself is run-to-run non-byte-
deterministic (two identical flag-off runs hash differently, metrics
identical to 4 decimals); all identity gates above are metric-level,
which main passes against itself.

Ranked next: (a) field-level cone-count reduction on coarse organics —
real dipole annihilation at the substrate scale (larger smoothing
radius / multires made torus-safe), THE binding constraint at 1297
cones; (b) close the guided-reach gap (realized deviation ~1.7× the
flow optimum even when engaged); (c) seam-locked residual folds (~45)
need integer-free participation or T-mesh-side QEx Alg-8 valence
recovery.

### Update — 2026-08-03 (branch `feat/wrinkle-web`): the pseudo-feature web MEASURED and DEMOTED — nefertiti cones 1297 → ~635, sing 419 → 220 with BETTER geometry; armadillo gate MET (159/143)

**The web, named and counted** (new permanent instrumentation:
`CYBER_QC_FIELD_STATS` web/chain census + cones-vs-web + a
dipole-blocking census in `annihilateFieldDipoles`). nefertiti@4000
pure substrate (15 288 faces, h = 20.48):

- *Tag level*: 782 feature edges in 373 chains — **0 resolvable**
  (167 sub-resolution chains / 543 edges, 203 isolated single edges,
  3 long chains / 36 edges that trace nothing in the original). The
  original mesh's ≥90° dihedral network tops out at chain length 31.2
  vs the 2-cell floor of 41: nefertiti has NO sharp network resolvable
  at this density — every tagged edge is a coarse-remesh alias.
- *Align level (the bigger web)*: 6 925 work-mesh edges pass the
  45° crease-ALIGNMENT test and pin 5 583 faces — **43% of the field
  frozen at wrinkle orientations** (armadillo: 2 568 faces, 29.6%).
- *Cones vs web*: 1 297 cones, **1 149 (89%) sit on the align web**
  (443 on tagged edges). armadillo: 651 cones, 571 (88%) on-web.
- *Dipole blocking*: 464 of 550 +cones unmatched; **387 (83%) are
  web-blocked** (a −1 partner exists within radius once feature/pin
  crossings are allowed; 77 guard-rejected, 0 partnerless). armadillo:
  219 of 256 web-blocked. The gap-4 saturation is the web, quantified.

**The lever — resolution-aware feature demotion (default ON, kill
switch `CYBER_QC_NO_FEATURE_DEMOTE`).** After the post-remesh re-tag +
fold/reference filters, an edge keeps FIELD authority (hard seam, field
pin, planar-fill seed, dipole barrier) only if it is resolvable at the
output density:

1. *Coarse-substrate gate*: demotion engages only when the output cell
   spans ≥4 input edges (`CYBER_QC_DEMOTE_MIN_RATIO`) — nefertiti /
   armadillo pure @4000 sit at 6.87/6.59, while bunny default @3000
   (3.19), nefertiti default (3.4), spot (0.85) and every CAD cell
   (0.33–2.78) disengage and keep the historical path bit-for-bit.
   Density-relative by construction: an edge demoted at pure-4000 is
   back at 16000.
2. *Resolvable* = traces the ORIGINAL mesh's crease network restricted
   to chains ≥2 output cells (0.25h tolerance, the reference-filter
   test without its knife-edge exception) **OR** bends persistently at
   output scale on the ORIGINAL surface: probes at ±{0.3, 0.6, 1.0}·h
   across the edge, per-side normals coherent within 20° and the sides
   differing by ≥45° (`CYBER_QC_DEMOTE_PERSIST_DEG`, = the align
   threshold itself). Tag demotion unTAGs (every consumer reads
   `Mesh::isFeatureEdge`, so seams, pins, fill seeds, barriers and the
   featureBinding decision all follow); align demotion ships per-edge
   support flags through `buildSeamlessSetup` → `computeCrossField` /
   `fieldPinnedFaces`.
- *Measured dead ends*: (a) network-test-only demotion shreds the bunny
  ears (19 → 64, sing 94 → 208) — the ears are ridges SMOOTH at
  original resolution but tighter than an output cell, invisible to any
  per-edge dihedral network; (b) persistence measured on the WORK mesh
  cannot work — the coarse remesh aliases sub-cell relief into
  cell-scale corrugation (kept 3 117/5 214 wrinkle pins, cones
  589 → 1 071); (c) a single-distance original-surface probe still
  confuses stripe corrugation of period ≈ h with a crease (kept 2 975,
  cones 1 021). The shipped criterion (multi-distance + side coherence)
  keeps 897/5 214 on nefertiti, 57/2 257 on armadillo, 504/545 on the
  (disengaged-anyway) bunny.

**Results** (shipped defaults unless noted; `CYBER_QC_BIMDF_HEALTH`
still required to engage guided on these substrates — sideMismatch
stays 0.60-0.75):

nefertiti@4000 pure — demoted 725/782 tag edges, field constraint
43% → ~6%, cones **1 297 → 633**, web-blocked +cones **387 → 69**:

| nefertiti@4000 pure    | sing | quads | haus | recall | loops |
|------------------------|-----:|------:|------|--------|-------|
| greedy (round-6 base)  | 419  | 4108  | 0.0072 | 0.738 | 18/456 |
| demote, greedy         | **220** | 3516 | **0.0051** | **0.780** | 8/879 |
| demote, guided engaged | 248  | 3380  | 0.0053 | 0.770 | 14/483 |
| demote + multires, greedy | **176** | 3528 | 0.0052 | 0.779 | 8/882 |
| demote + multires + guided | **150** | 3048 | 0.0055 | 0.748 | 8/762 |

armadillo@4000 pure — cones **651 → 266**, web-blocked 219 → 3:
greedy **159** (haus 0.0133, recall 0.622; base 273 / 0.0173 / 0.647),
guided engaged **143** (0.0135), multires greedy 119 (0.0156),
QuadriFlow ~80.

**GATE VERDICT — nefertiti cyber-pure ≤ 200: MET only with multires
(176 greedy / 150 multires+guided; both env-gated by multires' torus
regression), NOT met on the stock single-level field (greedy 220,
guided 248). armadillo pure: MET on the default path (greedy 159,
guided 143).** Fidelity guard holds everywhere it was measured:
nefertiti hausdorff 0.0051–0.0058 vs 0.0072 baseline — for the first
time a sing win comes with BETTER geometry, angle (19.9° → 12.9°) and
recall (0.738 → 0.780). Honest costs: armadillo raw recall dips
0.647 → 0.622 (its demoted web IS the wart texture; resolvable-feature
recall is not separable — the census shows both organics have ZERO
resolvable chains at the solve threshold, so raw recall against the
input's wrinkle network is the only number there is); pure-arm quad
counts land ~10-14% under the request (3 516/4 000, 3 286/4 000) vs
~3% before — the wrinkle seams were also inflating the count.

**Where the remaining cones live (step-3 answer):** of nefertiti's 633
post-demotion cones, 561 sit on the RETAINED web — the persistent,
resolvable-at-scale crease bands (headdress borders, eyes, lips) — and
the blocking census says the annihilation pass is no longer
web-saturated: 166 unmatched = 90 guard-rejected (curvature-demanded
per the 2-ring consistency guard), 69 blocked by the retained
(legitimate) web, 7 partnerless. Radius 8 now changes the output
(saturation broken) but measures WORSE (242 vs 220) — the residue is
curvature/structure-demanded, not under-tuned pairing. Getting the
stock field under 200 needs the multires hierarchy made default-safe
(its torus handle regression), not more demotion.

**Gates**: ctest 14/14; `bench.py check` OK off/report/guided; bunny
default guided ears **19** / sing **94** exact (gate disengaged at
3.19); box_sharp@1000 pure guided exact injection (maxFrac 0.0000,
injected 6); box_sharp default sing 8 / recall 1.00 / angle 0.0°; CAD
spot-check box+cylinder × {400,900,1400,4500} × both arms: demotion
disengaged 16/16 (coarseness ≤2.78) and on-vs-off **16/16
metric-SAME**; flag-off reproduces the round-6 baselines exactly
(nefertiti 419, armadillo 273, bunny 19/94). Identity gates are
metric-level per the round-6 determinism finding.

Ranked next: (a) make the multires cross-field default-safe (the torus
handle regression is the only blocker between the shipped path and
nefertiti 176/150); (b) guided health on demoted substrates
(sideMismatch 0.60-0.75 still refuses unaided; featureArcs are now 0 so
the crease-fraction refusal is gone); (c) revisit QuadriFlow's
remaining 2× (150 vs 80) — likely cone PLACEMENT, not count.

### Update — 2026-08-03 (branch `feat/multires-default`): multires cross field is DEFAULT — the torus regression was a trapped holonomy winding, fixed by coherent seeding; nefertiti pure ≤ 200 gate MET on the stock path (176)

**The torus mechanism, measured** (new permanent instrumentation:
`CYBER_QC_MR_DUMP` per-level hierarchy dumps + `CYBER_QC_FIELD_DUMP`
per-face field dumps; `CYBER_QC_MR_FLOOR` hierarchy-depth override;
`CYBER_QC_MR_SEED=legacy` seed-basin isolation). The recorded
regression reproduced worse at HEAD (haus 0.0250 → 0.0557, +123%;
sing 62 → 76) — and the mechanism is NOT coarse-level aliasing:

- Depth is not the problem: sweeping `CYBER_QC_MR_FLOOR`
  16→947 (947 = no coarsening at all) never fixes the haus
  (0.0557/0.0338/0.0578/0.0556/0.0634/0.0753/0.0670).
- The multires field has FEWER cones than stock (14 vs 32 on a field
  whose ideal is 0) yet much worse output — cones are not the damage.
- The damage is the field's TWIST STRUCTURE: per-face twist against
  the torus principal directions has global 4-RoSy coherence R = 0.519
  for stock (a near-constant ~67° twist — a uniformly slanted lattice,
  harmless) vs **R = 0.128 for multires, with the twist swinging
  11° → 47° → 5° → 84° → 60° around the major generator**: a trapped
  holonomy winding. Per-node-random (`anyTangent`) seeds land the
  coarse solve in a random holonomy basin; on a closed handle with no
  constraints the basin can wind around a generator, and NO downstream
  smoothing can unwind it (that would require moving cones across the
  generator — the hand-off relax is a contraction that inherits the
  seed's topology by design). The wound field extracts as sheared,
  incoherent cells: edge CV 0.42 → 0.66, haus doubles.

**The fix — coherent seeding of unanchored, non-simply-connected
components** (`position_field.cpp`, opt-in parameter
`coherentSeedUnanchored`, engaged only by
`computeCrossFieldFromOrientation`; the InstantMeshes/integer
extractors keep the historical init bit-for-bit). At the coarsest
level, a connected component gets ONE arbitrary source direction
BFS-parallel-transported across it — one holonomy basin per component,
near-zero winding — exactly and only where a winding invariant exists:

- component has constraints → per-node init (gauge already anchored;
  measured: a transported gauge on top of anchors is WORSE, nefertiti
  204 vs 176);
- component is simply connected (per-component Euler characteristic
  V−E+F = 2 on the base mesh, propagated down the hierarchy) →
  per-node init (H1 = 0, no winding to trap; measured: the transported
  hedgehog seed is worse, sphere haus 0.0070 → 0.0218 at an identical
  8-cone census);
- else (torus: unanchored, χ = 0) → coherent seed.

Torus flips from regression to WIN: sing 62 → **59**, haus
0.0250 → **0.0158**, angle 21.7 → 21.4 vs stock. Anchored meshes
(nefertiti, armadillo, bunny, fandisk) are bit-identical to the
round-7 multires numbers; sphere keeps its legacy-seed basin.

**The default flip** (`seamless_solver.cpp`): the multires cross field
is now the stock path; kill switch `CYBER_QC_NO_CROSSFIELD_MULTIRES`
restores the single-level field (verified: reproduces round-7 stock
exactly — nefertiti 220/3516/0.0051).

**Full corpus, default-on vs default-off (kill switch)**, pure arm
@4000 organics / corpus targets, greedy unless noted:

| mesh | default (multires) | off (stock field) |
|---|---|---|
| nefertiti pure | **176** / haus 0.0052 / recall 0.779 | 220 / 0.0051 / 0.780 |
| nefertiti pure guided+health | **150** / 0.0055 | 248 (r7) |
| armadillo pure | **119** / 0.0156 | 159 / 0.0133 |
| armadillo pure guided | **112** / 0.0146 | 143 (r7) |
| spot pure | **55** / 0.0042 | 65 (r7 A+B) |
| bunny default guided | ears **16** / sing **82** / 0.0069 | ears 19 / sing 94 |
| sphere | **21** / 0.0070 / angle 8.6 | 37 / 0.0059 / 13.1 |
| torus | **59** / 0.0158 | 68 / 0.0256 (baseline) |
| cylinder | **5** / 0.0059 | 9 / 0.0053 |
| box_sharp | 8 / 1.00 / 0.0° | bit-identical |

**GATE VERDICT — nefertiti cyber-pure ≤ 200 on the DEFAULT path (no
env vars): MET — greedy 176, haus 0.0052 ≤ 0.010; guided (health
override) 150. armadillo 119/112 ≤ 200.** Openspec `bimdf-quantization`
task 5 gate closes. Honest notes: nefertiti guided WITHOUT the health
override still refuses (sideMismatch 0.534 — unchanged from round 7) so
the plain-guided arm equals greedy; sphere/cylinder haus baselines move
+0.001/+0.0006 (recorded, inside tolerance) as the price of sing
21/5 vs 37/9 and angle 13.0° → 8.6° / 9.2° → 6.5°.

**Gates**: ctest 14/14; `bench.py check` OK off/report/guided AND
`CYBER_QC_BIMDF=off`; baselines deliberately re-recorded (sphere
37 → 21, torus 68 → 59 + haus 0.0256 → 0.0158, cylinder 9 → 5 — the
diff is the review artifact); box_sharp default 8/1.00/0.0° exact; CAD
spot-check box+cylinder × {400,900,1400,4500} × both arms: box_sharp
8/8 bit-identical on-vs-off, cylinder improves (4500: sing 29 → 18,
angle 30.5 → 5.2) with arms metric-SAME per config (the box_sharp@400
greedy-vs-guided difference is pre-existing: stock reproduces it
bit-for-bit); bunny ears 16 ≤ 20 / sing 82; clang-format clean.

Ranked next: (a) guided health on demoted substrates (nefertiti
sideMismatch 0.534 still refuses unaided — 150 needs the override);
(b) QuadriFlow's remaining ~2× (150-176 vs 80) — cone PLACEMENT;
(c) torus residual: 59 cones vs an ideal 0 — the extraction still
pays for the ~22° constant twist both fields share.

### Update — 2026-08-06 (guided-rounding default-flip A/B on the pinned solver): NO FLIP — the lattice-closure fix moved the greedy baseline under guided; guided is now 9 wins / 22 losses on singularities where it engages

The full default-flip evaluation for `CYBER_QC_BIMDF=guided`, run AFTER
the two foundations of this cycle (vendored-solver pin `b43dc827`;
joint-lattice closure of dependent seam translations in the reduced
integer phase) — precisely because both were expected to shift the
absolute numbers. They did, and the shift refutes the flip.

**Harness** (`tools/bench/bimdf_ab.py`, results in
`tools/bench/bimdf_ab_results.csv`, 348 rows): 6 models (spot, fandisk,
rocker-arm, cheburashka, cube, stanford-bunny) × 7 densities
(500/1000/1500/2000/3000/4500/6000) × both arms (mixed default +
`--pure-quads`) × off/guided × BOTH backends — the Geogram build (`geo`,
the shipped cpu-headless configuration, native solver reached only by
crease routing) and the no-Geogram build (`nogeo`, the ubuntu-CI/stock
backend where the native solver — and therefore the flag — runs on every
mesh). Count-matched per the repo rule: the guided run re-requests
toward the off run's ACHIEVED quad count until within 2% (≤3 attempts);
rows that still miss are marked `count_matched=False` and treated as
failures of guided's count stability, not as comparable cells.

**Noise floor, measured first (the pin makes this possible):** one cell
per backend run 3× per state — geo bunny@3000 + fandisk@3000, nogeo
bunny@3000 off AND guided — every repeat bit-identical on every metric
(quads, sing, angle, haus, flow, recall). Noise floor is ZERO on the
pinned solver; every delta below is real.

**geo backend (shipped default): guided is inert-to-mixed.** 82/84
cells metrically identical (56 never read the flag — vendored route; 28
native-routed CAD cells refused by the crease health gate with the
injection finding no exact solution). The 2 differing cells are cube
exact injections: cube@1000 mixed sing 33 → 24 but angle_dev
1.77 → 1.95 (median 0.295 → 0.342); cube@4500 pure sing 13 → 8 but
flow_loop 110 → 104. Mixed even where it fires.

**nogeo backend (where the flag actually lives): NOT a clean win — not
close.** 84 cells: 26 steered, 56 health-refused (injection still
active), 2 tmesh-only, 53 bit-identical. Where guided differs from off, singularity
wins/losses/ties = **9 / 22 / 53**, and 9 cells FAILED the 2% count
match after 3 re-requests. The load-bearing rows:

| cell (nogeo) | quads off/guided | sing | angle mean | haus p99 | recall | verdict |
|---|---|---|---|---|---|---|
| bunny@3000 mixed | 2515/2535 | 87 → 92 | 7.08 → 6.71 | 0.0067 → 0.0071 | 0.65 → 0.56 | **the round-4 flagship (135 → 94) is REVERSED: off is now 87** |
| bunny@3000 pure | 2156/2168 | 99 → 114 | 10.6 → 12.0 | ~same | 0.38 → 0.37 | loss |
| spot@3000 mixed | 2562/2548 | 24 → 20 | ~same | ~same | 0.57 → 0.58 | win |
| spot@6000 pure | 5012/5246 (4.7%, unmatched) | 54 → **508** | 7.35 → 9.57 | 0.0037 → 0.0039 | — | catastrophic |
| cheburashka@1000 pure | 904/**2504** (unmatched ×3) | 58 → 383 | 15.7 → 31.8 | — | — | count blowup: guided ships 2.8× the request |
| rocker-arm@2000 mixed | 1613/1608 | 71 → 101 | ~same | ~same | 0.53 → 0.60 | loss |
| rocker-arm@4500 mixed | 3786/3355 (11%, unmatched) | 64 → 102 | 13.5 → 16.1 | 0.0050 → **0.0176** | 0.76 → 0.62 | loss + count instability |
| bunny@1000 mixed | 840/837 | 59 → 45 | 8.4 → 10.4 | ~same | 0.25 → 0.28 | injection win, angle cost |
| fandisk (all 14 cells) | — | identical | identical | identical | identical | crease gate refuses; injection never exact |

**Why the flip died: the greedy baseline moved.** The joint-lattice
closure (dependent seam translations / crease offsets now rounded on
their induced half-integer lattice) improved PLAIN GREEDY exactly where
guided used to win — bunny@3000 mixed off went 135 → 87 sing vs the
round-4 baseline, spot pure 66 → 53 — and the same change altered what
the attraction steers (lattice frees participate in the schedule the
flow never modeled). The historical wins did not survive: bunny
94-vs-135 became 92-vs-87, spot pure 65-vs-66 became 65-vs-53. Guided's
steering now often fights a better greedy.

**Decision (repo rule: clean full-corpus win or no flip): NO FLIP.**
Mixed on both backends, material regressions on 4 of 6 models across
densities and arms, two catastrophic cells, and 9/84 count-stability
failures. The health gates as shipped do NOT scope the damage away
(rocker-arm/spot/cheburashka cells above all PASSED the gates and
steered), so a gates-scoped default is not available without new
heuristics — out of scope this round. `CYBER_QC_BIMDF=guided` stays
opt-in. Banked for any future flip attempt: (a) re-baseline guided's
attraction against the lattice-closed greedy (the flow targets predate
it); (b) guided count stability on pure arms (cheburashka 2.8×,
spot@6000 +4.7% with 10× sing) is a correctness-adjacent bug worth its
own investigation; (c) note that today `CYBER_QC_BIMDF=off` is NOT a
no-op — any non-null value builds the T-mesh and arms exact injection;
a real flip needs an explicit off/0 → disabled mapping first.

## Update — 2026-08-02 (CAD density robustness: the sweep's 🔴 failures are FIXED; 32/32 gate cells clean)

The two density-dependent robustness bugs the CAD sweep below exposed are fixed
(branch `feat/cad-robustness`; regression guard
`python/cyberremesh/tests/test_cad_density_robustness.py`; raw data
regenerated in `tools/bench/cad_sweep_results.csv`, 216 runs, density 2510
added to `tools/bench/cad_sweep.py` as a permanent guard).

- **Bug 1 — cylinder exit-4 / 657s: a density-crossing cascade in the isotropic
  stage.** When the request put `4/3·h < rimSegment < 1.6·h` (targets
  ~2300–3000 on the generated cylinder), the split pass halved the rim's
  feature edges into sub-collapse-band halves that NOTHING can remove (feature
  vertices are never collapsed) and the split/collapse loop left its transient
  unconverged at 3 iterations (1.2k → 52k → 24.9k faces at 4500, 2.4x
  over-density, 6 227 sub-1° cap slivers, plus doubled-face flaps). The junk
  normals re-tagged ~900 spurious "knife edges" across the flat caps
  (`filterFeatureEdgesToReference`'s knife-edge exception keeps 180° folds),
  the pinned-Poisson factor went non-SPD, the float CG fallback diverged
  (UV span ~1e17), the divergence guard declined — and the CLI had never wired
  the field-aligned fallback the pipeline API provides, so exit 4. Fixes:
  (a) never split a feature edge into sub-band halves (`isotropic.cpp`);
  (b) keep iterating while the split pass still grows the mesh >10%
  (equilibrium meshes run the exact same 3 iterations — byte-identical);
  (c) untag fold/flap dihedrals by orientation vote (`untagFoldedFeatureEdges`);
  (d) the CLI now passes the field-aligned fallback factory. cylinder@2510:
  exit 4 → **exit 0, recall 1.00, haus 0.0048, 0.5s**; @4500: 657s → **1.8s**.
- **Bug 2 — box_sharp request-dependent geometry drops: fractional seam offsets
  + silent largest-island keep.** The reduced MIQ's Gauss-Jordan preferred a
  NON-UNIT continuous pivot over a unit one; the division spread fractional
  coefficients into integer-only rows, so dependent seam translations landed on
  the fractional lattice after rounding (96/144 crease seams at offsets up to
  3/8 of a cell at 1400). Every cube face's grid disagreed along its creases,
  the caps traced as disconnected islands, and `removeIsolatedFaces` kept only
  the largest island — shipping a "flawless" 0-singularity open tube with both
  z faces missing (haus 0.219) as SUCCESS, density-dependent because hole-fill
  sometimes papered over it. Fixes: (a) run the elimination with both pivot
  policies and keep the one with fewer structural integrality violations
  (dependent integers reaching continuous frees / non-integer coefficients) —
  ties keep the historical reduction byte-exactly (the unit-first policy alone
  BREAKS the cylinder: 39/96 fractional rim seams vs 10, sing 11→52 at 900 —
  measured, hence the scoring-by-measurement; kill switches
  `CYBER_QC_NO_UNIT_PIVOT` / `CYBER_QC_UNIT_PIVOT`); (b) closed surfaces keep
  every island ≥10% of the largest instead of largest-only; (c) an
  area-coverage gate (≥85% of the input island, closed inputs only) DECLINES
  honestly per the remeshing-pipeline spec — the pipeline recovers via the
  fallback or reports a per-island failure (exit 5), never a successful wrong
  answer. box_sharp is now sing 8 / recall 1.00 / haus 0.0054 at ALL 8
  densities (default arm).
- **Collateral extraction fix:** `collapseTriangles` collapsed EVERY graph
  3-cycle to its centroid — at coarse density that deletes genuine corner-cone
  cells (box@100: cube corners chopped up to 0.17 off-surface). It now only
  collapses clusters smaller than a quarter of the average traced edge (its
  actual purpose: near-coincident sampling artifacts). Known-good cells moved
  only monotonically better (cylinder 400/600/900/1400: sing 9/10/11/17 →
  8/6/10/15, recall/haus equal or better).
- **`--pure-quads` CAD lift** (was recall 0.02–0.82 on the cylinder): the pure
  post-pass now (a) projects crease vertices onto the source crease POLYLINE
  (closest-surface projection leaves a curved crease at its chord sagitta),
  (b) freezes relax vertices by distance-to-crease-network instead of the
  dihedral tag (bent coarse wall quads read a 90° rim as ~83° and unfroze it),
  (c) snaps diagonal-crossing vertices within 0.7 subdivided cells onto the
  crease, and (d) raises the quarter-density base so every CLOSED crease loop
  ≥8 base cells long gets ≥18 cells (feature-resolvability floor, capped at
  the full request; sub-resolution wrinkle loops are filtered so organics keep
  their count contract — armadillo-pure stays at ~7.0k quads for 8000).
  Calibration accepts only ±12% for targets <200 (the coarse bases). Honest
  tradeoff: cylinder-pure at 400 now returns 536 quads (+34%) — resolvability
  over count.

**Post-fix sweep (box_sharp + cylinder × {400,600,900,1400,2000,2510,3000,4500}
× default + `--pure-quads`): 32/32 cells exit 0, recall ≥ 0.94, hausdorff-p99
≤ 0.0078, max 1.8s** (gate was recall ≥0.9, haus ≤0.01, <60s). Per-density
recall (ours-default / ours-pure / QF): box 1.00/0.96–1.00/0.95–1.00;
cylinder **0.94–1.00 / 0.97–1.00 / 0.60–0.92 — feature recall now beats
QuadriFlow on the cylinder at EVERY density**, at equal hausdorff (ours
0.0048–0.0078 vs QF 0.0048–0.0093). Still open vs QF on CAD: singularity
count at high density (cylinder@4500: 56 vs 8) and angle on curved walls
(7–14° vs 3.6–4.9°); cylinder-pure@4500 angle 30.6° (its base at 1125 remains
a defective-field pocket — cone placement on curved crease loops is the
remaining deep lever). Gates: ctest 14/14 (new regression suite included),
`bench.py check` green, organics A/B (spot/armadillo, default + pure) within
bench tolerances with armadillo-default recall 0.886→0.930 and
armadillo-pure haus 0.0062→0.0044.

## Update — 2026-08-02 (CAD multi-density confirmation: the "better than QuadriFlow on CAD" claim does NOT bank)

Measurement-only sweep to bank (or refuse) the CAD claim under this file's own
discipline: **count-matched, 7 densities, no single-density claims** (fandisk's
per-density jitter is sd 0.90° on identical code — see 2026-07-24).

- **Protocol.** Meshes: generated `box_sharp` + `cylinder`, plus fandisk
  (fetched from alecjacobson/common-3d-test-models; ⚠️ **its upstream license is
  unstated** — measurement-only use, cached in gitignored `.bench-cache/`, not
  redistributed). Densities: target-quads ∈ {400, 600, 900, 1400, 2000, 3000,
  4500}. Arms: ours default (quad-cover), ours `--pure-quads`, QuadriFlow
  (`-i/-o/-f`). Metrics: `tools/bench/mesh_metrics.py compute_all`. 3 repeats
  per sub-5s cell: **every repeat was byte-identical for every arm** (all sd
  0.0), so the density axis is the only noise source and 7 densities cover it.
  Raw data: `tools/bench/cad_sweep_results.csv` (primary, 185 runs),
  `tools/bench/cad_sweep_matched.csv` (count-matched phase, 42 cells); script
  `tools/bench/cad_sweep.py`.
- **Count-matching required a second phase.** At the same request, ours
  undershoots the target by **5–28%** on these CAD meshes while QuadriFlow
  lands within −9..+3%, so only 1–2 of 7 same-request pairs agreed within 8% —
  nothing bankable either way. A supplementary phase re-ran ours with inflated
  requests (×1.09–1.7, up to 3 calibration iterations) until achieved counts
  agreed with QF's within 8%. That yielded 7/7 matched cells on box_sharp and
  fandisk, 6/7 on cylinder for `--pure-quads` (pairs below are those cells; the
  calibration undershoot is itself a finding — the probe's eta is corpus-tuned,
  not CAD-tuned).

**ours `--pure-quads` vs QuadriFlow, count-matched (achieved ours/QF; sing,
angle°, recall, hausdorff-p99):**

| box_sharp | 396/384 | 598/600 | 902/864 | 1408/1350 | 1944/1944 | 3059/2904 | 4416/4454 |
|---|---|---|---|---|---|---|---|
| sing | 13/**8** | 10/**8** | 44/**8** | 8/8 | 8/8 | 22/**8** | **0**/8 |
| angle | 4.38/**0.24** | 2.81/**0.22** | 9.87/**0.14** | 0.52/**0.16** | 0.88/**0.16** | 3.02/**0.16** | **0.47**/0.60 |
| recall | 0.89/**1.00** | 0.83/**1.00** | 0.25/**1.00** | 1.00/1.00 | 1.00/1.00 | 0.60/**1.00** | 0.11/**0.99** |
| haus | .0060/**.0054** | .0075/**.0054** | .0294/**.0054** | .0054/.0054 | .0054/.0054 | .0313/**.0054** | .219/**.0054** |

| cylinder | 380/364 | 586/607 | 938/903 | (1400 unmatched) | 1756/1884 | 2880/2873 | 4604/4616 |
|---|---|---|---|---|---|---|---|
| sing | 20/**8** | 19/**8** | 16/**8** | — | 22/**8** | 27/**8** | 61/**8** |
| angle | 8.31/**4.89** | 7.03/**4.34** | 4.83/**4.61** | — | 7.09/**3.82** | 10.2/**3.85** | 17.1/**3.56** |
| recall | 0.17/**0.60** | 0.29/**0.80** | 0.36/**0.60** | — | 0.45/**0.78** | 0.52/**0.92** | 0.29/**0.80** |
| haus | .0133/**.0093** | .0099/**.0052** | .0071/**.0065** | — | .0069/**.0050** | .0057/**.0048** | .0059/**.0048** |

| fandisk | 366/365 | 620/611 | 932/885 | 1420/1374 | 1820/1946 | 2608/2724 | 4012/3915 |
|---|---|---|---|---|---|---|---|
| sing | **34**/36 | 42/**34** | 55/**32** | 57/**42** | 65/**32** | 59/**40** | 76/**42** |
| angle | 15.4/**10.8** | **10.2**/10.9 | 11.1/**7.48** | **8.28**/8.72 | **7.84**/8.97 | 11.0/**9.13** | 7.98/**6.43** |
| recall | 0.19/**0.34** | 0.21/**0.40** | 0.40/**0.54** | 0.41/**0.64** | 0.44/**0.78** | 0.54/**0.70** | 0.61/**0.76** |
| haus | .0542/**.0192** | .0271/**.0130** | **.0113**/.0179 | .0095/**.0074** | .0091/**.0051** | .0068/.0067 | .0051/**.0047** |

**Verdicts per axis (ours `--pure-quads` vs QF; better-at-k/n over matched
densities, mean paired delta):**

| axis | box_sharp | cylinder | fandisk |
|---|---|---|---|
| singularities | TIE-worse (1/7, +7.0 ± 14.4) | **LOSS** (0/6, +19.5) | **LOSS** (1/7, +18.6) |
| angle_dev_mean | **LOSS** (1/7, +2.9°) | **LOSS** (0/6, +4.9°) | TIE (3/7, +1.3 ± 2.2°, t ≈ 1.6 — not distinguishable from zero) |
| feature_recall | **LOSS** (0/7, −0.33) | **LOSS** (0/6, −0.40) | **LOSS** (0/7, −0.20) |
| hausdorff_p99 | **LOSS** (2/7) | **LOSS** (0/6) | **LOSS** (1/7) |
| flow_loop_mean_len | TIE (4/7) | **LOSS** (0/6) | **LOSS** (2/7) |
| quad_ratio | TIE (1.0 both) | TIE (1.0 both) | TIE (1.0 both) |

- **Default arm (quad-cover, quad-dominant), count-matched, for context:** it
  is the stronger CAD arm but still does not bank the claim. box_sharp: parity
  with QF at 5/6 matched cells (perfect 8-cone grid, recall 1.0, angle ≤0.9°)
  — QF holds that same perfect grid at **all 7**. cylinder (4/7 matched only):
  **feature_recall WIN 4/4 (0.95–0.98 vs 0.60–0.80)** and hausdorff WIN 4/4,
  but singularities/angle/flow LOSS. fandisk: hausdorff 5/7 better and angle
  5/7 better (mean −0.52 ± 1.71° — not distinguishable from zero at jitter
  sd 0.9°), recall TIE (4/7), singularities **LOSS 0/7 (+28 mean, 108 vs 42 at
  4500)**.
- 🔴 **Robustness findings (new, default arm on CAD)** — both FIXED, see the
  CAD density-robustness update above (the numbers below are the pre-fix
  evidence):
  - `cylinder` at target ≈2500–3000: **hard failure, exit 4 "remeshing
    produced no result"** (reproduced standalone at `--target-quads 2510`);
    at nominal 3000 it returns a defective result (240 sing, 32.6° angle,
    21s) and at 4500 it takes **657s** (vs QF 1.4s) for 171 sing / 44.7°.
  - `box_sharp`: intermittent geometry drops as a function of the *request* —
    nominal 1400/4500 and matched-2000 produce recall 0.08–0.33 with
    hausdorff-p99 ~0.19–0.22 (the box loses faces), while neighboring requests
    are perfect. `--pure-quads` shows the same class at matched-4500
    (haus 0.219, recall 0.11 — a flawless 0-singularity grid on the *wrong*
    geometry). QuadriFlow: 21/21 cells clean, ≤1.5s.
- **Bottom line:** count-matched over 7 densities, QuadriFlow wins CAD on
  singularities, feature recall, and (except fandisk-default) angle; our banked
  CAD wins are narrow — default-arm feature recall + hausdorff on cylinder,
  hausdorff on fandisk — and sit behind the robustness failures above. The
  flat-CAD feature-following win vs QF recorded 2026-07-24 (cube, 0.98% vs
  2.69%) still stands; this sweep does not extend it to a general CAD claim.
  Next levers: the exit-4/657s cylinder pathology, the request-dependent
  geometry drops on box_sharp, CAD count calibration (probe eta), and
  singularity count on curved CAD.

## Update — 2026-08-01 (CI scoreboard lands; CLI was measuring the wrong solver; isotropic adaptivity explosions fixed)

- **`tools/bench/` benchmark harness** (new): deterministic generated corpus +
  sha256-pinned real models, recorded-baseline regression gate in ctest
  (`bench`), external competitor binaries (QuadriFlow, Instant Meshes,
  **quadwild-bimdf** — the current open-source quality bar, run as a GPL
  binary only), and **edge-flow loop metrics** (`flow_turning_mean`,
  `flow_loop_mean_len`) that quantify the wavy-flow/spiral axis the shape
  metrics miss. Complements `examples/10_vs_reference.py`.
- **The CLI hardcoded `field-aligned`** — every CLI run (and anything
  benchmarking through it) measured the retired method, not the documented
  quad-cover default. Fixed with `--quad-method` (default quad-cover). On
  spot/3000/adaptivity-0 this alone moves singularities 854 → 179 and mean
  corner-angle deviation 28.4° → 11.2°.
- **Isotropic adaptivity explosions fixed** (25k+ quads for a 600-quad capped
  cylinder, benchmark-caught; affects every method that runs the pipeline
  isotropic stage): crease angle defect excluded from the curvature source,
  Laplacian smoothing replaced by a gradation-limited sizing field, the scale
  field made Eulerian (sampled from the `ReferenceSurface`, not carried on
  drifting vertices), and feature tagging given an epsilon so exactly-90°
  dihedrals tag deterministically. Regression test in `test_pipeline.cpp`.
- **Native-solve perf (2026-08-01, later): calibration loop de-duplicated +
  probe-predicted initial scaling.** Two commits on the quad-cover path:
  - *Byte-identical hoist:* the isotropic pre-remesh, cross field and
    `buildSeamlessSetup` depend only on (mesh, edge length, adaptivity, feature
    threshold) — never on the calibration `scaling` — so they now compute once
    (`NativeSolveContext`) and each calibration attempt re-runs only the
    spacing-dependent solve + extraction. sha256 of the output OBJ verified
    unchanged on all 7 corpus meshes. nefertiti@8000 220s → 203s,
    armadillo@8000 98s → 82s.
  - *Calibration probe (default-on, kill switch `CYBER_QC_NO_PROBE`):* the
    hardcoded 0.5 initial scaling overshoots the extracted count 1.5-3x on
    every corpus mesh, forcing a second full solve. A relaxed-Poisson-only
    probe (`relaxedCellArea`, ~3-9% of a full solve) predicts the mesh-specific
    scaling `s0 = sqrt(eta·cells/target)`; eta defaults to the
    corpus-calibrated 1.0 for the relaxed triangle-area measure
    (`CYBER_QC_EXTRACT_EFF` overrides), and the 2-attempt loop stays as the
    safety net. 5/7 corpus meshes (incl. every expensive one) now land in ONE
    solve: nefertiti 220s → **117s**, armadillo 98s → **52s** (sphere/torus,
    whose ARAP polish grows UV area ~1.65x, still take 2 — same as before).
    Counts land 0.87-0.99 of target (window 0.75-1.33); bench check green,
    box_sharp keeps its perfect grid (8 cones, 0° angle dev), spot improves
    (sing 72→63); nefertiti/armadillo drift ≤5% on sing/angle. Vendored route
    untouched (probe is native-only); outputs with `CYBER_QC_NO_PROBE=1`
    byte-match the pre-probe build.
- **Gap #2, flow-loop length (2026-08-02): the loop killers are the residual
  triangles, then dipoles; quantization is what remains.** Loop-termination
  census on spot: 47 leftover triangles account for essentially all 118 open
  loop ends (quad-dominant default, mean loop 41). `--pure-quads` removes
  them: mean loop 41 → **319** at ratio 1.000 with angle IMPROVING 6.7→6.5
  (the bench now carries a `cyber-pure` solver row — QuadriFlow/quadwild are
  pure-quad, so the quad-dominant comparison understated us structurally).
  With triangles gone the dipole canceller's effect unmasks: spot loops
  319 → **433** (+36%) at sing 95→93 — the stack is 41 → 433 (10.5×).
  Corpus cyber-pure: nefertiti sing 441 / recall 0.89, armadillo sing 404 /
  recall 0.77 (recall dips on armadillo under the pure post-pass — noted).
  QuadriFlow spot sits at 1811: the remaining ~4× is global grid structure —
  the Bi-MDF-style quantization lever, unchanged as the endgame for this gap.
- **Native-solve perf (2026-08-02): direct sparse-Cholesky solve
  (`CYBER_QC_DIRECT`, default on; kill switch `CYBER_QC_NO_DIRECT`).** The two
  solve operators are fixed across all their re-solves, so both are now
  factored ONCE by an in-tree double-precision simplicial LL^T with RCM
  ordering (`sparse_cholesky.{hpp,cpp}`, dependency-free — the license audit
  stays clean) and cached in `NativeSolveContext` across the probe and every
  calibration attempt (spacing only scales the RHS):
  - *Phase 1 — pinned Poisson:* every `relaxedSolve` (initial + up to 6 ARAP
    re-solves, ×2 coordinates) becomes a pair of exact back-substitutions
    instead of cold-tolerance float CG. The probe's relaxed phase on
    nefertiti@8000 drops 8.5s → 0.07s.
  - *Phase 2 — reduced integer phase:* the reduced operator M = Tᵗ·L₂·T is
    formed explicitly (fill is benign: nefertiti nnz(M)=340k → factor 18.0M,
    armadillo 328k → 11.4M, spot 35k → 0.39M), factored once, and the ~50-77
    masked CG re-solves of the greedy rounding collapse into exact bordered
    (Woodbury/KKT) solves on the ≤2813 pinned integer DOF — same fixed point
    as `maskedSolve`, greedy pin schedule untouched, `totalCg` 80408 → 0 on
    nefertiti. `CYBER_QC_FUSED_OPERATOR` keeps the CG loop on the explicit M
    (one spmv/iter) as the fill-pathology fallback.
  - *Measured (M2 Max, Release):* nefertiti@8000 wall 118.9s → **36.7s**
    (solve 95.5s → 21.7s, 4.4x); armadillo@8000 wall 57.8s → **24.6s** (solve
    36.8s → 8.1s, 4.5x); spot@3000 solve 270ms → 49ms. Remaining solve cost on
    nefertiti is the one-time inverse-int-block build (D=11.6s, factor=3.1s) —
    an AMD ordering is the next lever there.
  - *AMD ordering (2026-08-02):* in-tree approximate-minimum-degree ordering
    (quotient graph with aggressive absorption, mass elimination, and
    supervariable merging; still dependency-free) joins RCM in
    `sparse_cholesky.{hpp,cpp}`; the default measures both symbolically and
    factors whichever fills less (`CYBER_QC_ORDERING=rcm|amd` forces one).
    AMD wins corpus-wide — reduced-operator factor fill nefertiti 16.3M →
    **1.64M** (9.9x), armadillo 15.5M → 1.33M (11.6x), spot 0.50M → 0.10M
    (4.9x); Poisson factor fill ~2.5x lower. Since the Woodbury D-build's
    back-substitutions scale with factor nnz, the whole direct reduced phase
    collapses: nefertiti factor 2.5s → 0.19s, D 6.2s → 0.46s (solve stage
    11.9s → 3.9s, wall 28.3s → 20.3s); armadillo factor 3.8s → 0.14s, D
    3.8s → 0.22s (solve 8.8s → 1.4s); spot solve 66ms → 26ms. Gates: ctest
    green (incl. new `test_sparse_cholesky.cpp` unit tests), `bench.py check`
    green, license audit clean; outputs stay within the direct solver's
    tolerance (ordering only permutes the exact factorization).
  - *Gates:* ctest 13/13, `bench.py check` green (box_sharp 8 cones / 0° angle
    / recall 1.00). Full A/B (generated corpus + spot/nefertiti/armadillo,
    direct vs `CYBER_QC_NO_DIRECT`): generated corpus metric-identical; real
    meshes within noise (sing: spot 63→64, nefertiti 659→664, armadillo
    609→597; angle/hausdorff/recall/flow all ≤±2%, no bench-tolerance
    violation). Kill-switch output verified content-identical to the
    pre-change build (armadillo@8000 sha256, modulo the OBJ's self-referential
    mtllib line). Numerical drift `CYBER_QC_DIRECT_CHECK`: max
    |uv_direct − uv_cg| = 9.2e-5 on spot (the CG truncation error removed).
- **Isotropic-phase perf (2026-08-02): best-first BVH closest-point
  traversal.** Profiling the adaptive isotropic passes (nefertiti@8000:
  scales=28ms split=2.8s collapse=11.1s smooth=1.0s) showed ~895k
  `ScaleField::at`/project closest-point queries at ~15µs each — the
  `Bvh::closestPoint` traversal was unordered LIFO, so it explored far
  subtrees before establishing a bound (the kMaxScale short-circuit fired on
  only 0.5% of collapse candidates; the banded-lengths guard alone issued
  531k queries). Fix: ordered descent — visit the nearer child first, carry
  each deferred child's box distance and prune it against the current best on
  pop. Exact (pruning only skips subtrees that cannot beat the best), so
  every BVH consumer (split/collapse midpoint sampling, smooth+project,
  snapping, baking) speeds up with no semantic change; the Eulerian
  ScaleField design is untouched.
  - *Measured (M2 Max, Release), nefertiti@8000 isotropic phase:* collapse
    **11.2s → 0.51s** (22x), split 2.9s → 0.09s, smooth+project 1.0s → 0.06s;
    phase total 15.2s → **0.74s** (target was <5s), wall 21.7s → **7.4s** —
    the <10s roadmap wall target for nefertiti@8000 is met. armadillo@8000:
    collapse 11.4s → 0.56s, phase 16.5s → 0.83s, wall 21.9s → 6.2s; spot@3000
    collapse 0.18s → 0.03s, wall 0.58s → 0.34s.
  - *Gates:* ctest 14/14 (incl. BVH unit tests and GPU/flat-BVH parity),
    `bench.py check` green. Full-corpus A/B (generated +
    spot/nefertiti/armadillo, cyber and cyber-pure): 11/14 outputs
    byte-identical to the pre-change build (modulo the OBJ's self-referential
    mtllib line); the other 3 (torus/armadillo cyber-pure, cylinder cyber)
    differ only where two reference triangles are exactly equidistant and the
    new visit order returns the other one — same face/vertex counts and
    connectivity, coordinate deltas ≤2e-6 on the generated pair and localized
    to 262/7018 verts (max 0.12% of bbox diagonal) on armadillo, with the
    full metric table (quads/sing/haus/angle/flow/recall) identical at
    reported precision. CAD sweep A/B (box_sharp + cylinder, all 8 densities,
    both engines): all runs green, quads/singularities/recall identical,
    angle/hausdorff deltas ≤0.03% (float dust).
- **Feature-pinning lever (in progress, OPT-IN):** `CYBER_QC_FEATURE_DEG=90
  CYBER_QC_PLANAR_FILL=1 CYBER_QC_UV_SNAP=1` lifts box_sharp recall
  **0.04 → 0.73** (organics neutral: spot 179→168 sing, recall 0.59→0.64) via
  three pieces: (a) the native solve-binding feature tag honors the caller
  (was hardcoded 40° — 90° CAD edges were invisible to seams/field pins;
  plumbed through `makeQuadCoverQuadrangulator(..., featureDegrees)`);
  (b) NEW feature-seam integer pinning in `solveSeamlessReduced` (per feature
  seam: level-set row + promoted integer `c_e`; `CYBER_QC_NO_FEATURE_PIN`) —
  seams WITHOUT pinning regress (patch grids disagree, recall stays 0.04 at
  angle 32.5°): pin + seam must ship together; (c) planar flood-fill of
  feature pins across coplanar regions (the missing alternative to lever c2's
  gate: extend the pin as a constant field over the whole flat patch instead
  of disabling it — the pinned-ring-vs-diagonal-interior conflict is what
  regressed the cube in c2's ungated variant).
  **Blocker before default-on:** with feature seams the solved map SHEARS
  (box angle 12.5°→36°, sing 48→86, cylinder recall 1.00→0.58). Localized by
  instrumentation: field 100% axis-aligned, cone census exactly the 8 corner
  cones, targets exact, relaxed CG converged (120 iters) — yet the relaxed
  map is diagonal and the reduced phase only partially recovers. Mixed-axis
  targets (162 x̂/126 ŷ on one flat patch) come from the cone-spanning CUT
  TREE routing through patch interiors (legitimate seams, but they thread
  flat panels). Disproven en route: ARAP polish (map-level A/B, exonerated;
  `CYBER_QC_NO_ARAP` added), angle()/direction() branch mismatch (fix
  measured WORSE, reverted). Live leads, in order: (1) route the
  cone-spanning cut tree ALONG feature curves instead of through flat patch
  interiors (creases are already cuts — the tree between corner cones can
  follow them; no interior seams ⇒ per-patch solves become exact grids);
  (2) the greedy rounding order with ~144 pinned `c_e` integers (round crease
  constants first / merge per crease chain).
  - **SHEAR BLOCKER SOLVED (2026-08-01, later): three compounding solver bugs,
    all lever-gated fixes in `seamless_solver.cpp` + one tag filter in
    `quadcover_extractor.cpp`.** Lever-on now: box_sharp recall **0.73 → 1.00**
    at angle 36.2° → **0.00002°**, sing 86 → **8** (the corner cones, all-quad);
    cylinder recall 0.46 → **0.98**, hausdorff p99 0.022 → **0.0052**, sing 10.
    Organics neutral-to-better vs lever-off: spot sing 64→49 angle 17.1→8.5
    (improves), nefertiti sing 73→73 angle 28.4→27.5, armadillo sing 149→157
    (+5.4%) angle 17.3→16.5. Lever-off bit-compatible (ctest 13/13,
    `bench check` OK, no baseline re-record). Root causes, each measured:
    1. **Combed-target branch mismatch** (the shear itself): the comb and the
       period jumps live on `CrossField::angle()` (θ∈[0,90°)), but the per-face
       target frame `e0` was reconstructed from `direction()` (θ∈(-45°,45°]) —
       a per-face quarter-turn offset wherever raw θ<0, i.e. mixed x̂/ŷ targets
       on ONE coplanar patch (box top measured 178/110) whose Poisson
       compromise is the uniform ~32° diagonal. Fix: `combedDirection()`
       reconstructs on the comb's own angle() convention. Relaxed-phase grad
       histogram goes 100% of +z faces in the 30–35° bin → 100% in the 0–5°
       bin. (The earlier "measured WORSE, reverted" attempt was this fix
       WITHOUT #2/#3 — consistent targets make the wrong seam rho bite harder;
       the trio must ship together.)
    2. **Seam transition rho had the comb difference negated**: combed frame of
       B = frame of A rotated by Δ = p + comb[B] − comb[A] quarter-turns, and a
       grid whose frame rotates by +Δ has coordinates rotating by −Δ, so
       uv_B = R^(−Δ) uv_A + t. The code used comb[B] − comb[A] − p — wrong by a
       half-turn whenever the comb difference is odd (reverses the along-crease
       coordinate; the reduced phase then destroys the now-perfect relaxed map:
       recall 0.08, hausdorff 0.54 with #1 alone).
    3. **periodJump was never computed for feature edges** (historically fine —
       the comb never crosses them) — but rho needs the crease's intrinsic
       field jump p. Without it: box recall 0.10, cylinder 0.51 even with #1+#2.
    4. **Feature re-tag noise filter** (`resolvableCreaseSegments` +
       `filterFeatureEdgesToReference`): `CYBER_QC_FEATURE_DEG=90` re-tags the
       COARSE remesh, and organic scans have plenty of over-threshold dihedrals
       that are sampling artifacts, not creases (nefertiti lever-on sing 216 at
       the previous state). Keep a re-tagged edge only if it traces the ORIGINAL
       mesh's sharp network restricted to chains ≥ 2 output cells long, or still
       qualifies at the historical lever-off knife-edge threshold — so the
       lever-on feature set degrades exactly to the lever-off one on organics
       (nefertiti featureEdges 1518 = lever-off set, sing back to 73).
    - **NEW DISPROVEN (do not retry): routing the cut tree along creases**
      (0/1-Dijkstra, feature edges cost 0 — old live lead #1). Unnecessary for
      CAD once #1–#3 are in (box/cylinder identical with plain BFS) and it
      REGRESSES organics with knife-edge wrinkle networks (nefertiti sing
      80 → 205: the tree snakes along wrinkles and shreds the map). Also
      disproven twice earlier as pre-seeding (cylinder hausdorff 0.41 — breaks
      the disk-opening invariant). The greedy-rounding lead (#2 above) was
      never needed: with correct rho the box solve leaves only 3 free integers
      and the rounding is exact.
    - Diagnostics that found it now ship behind `CYBER_QC_FIELD_STATS`:
      per-patch axis-mix census, non-feature cut-edge census, per-phase
      grad(u) deviation histograms (relaxed vs reduced).
    - **DEFAULT-ON (2026-08-01, final):** the lever now ships as the default.
      Activation is value+topology-based, not env-based: the CLI binds
      `--sharp-edge` (default 90) into the native solve, and feature binding
      engages only when (a) the effective threshold is wider than the
      historical knife-edge, (b) the filtered remesh actually carries interior
      feature edges, and (c) the surface is CLOSED (same boundary gate as the
      ARAP polish — binding features on boundaried scans measurably blew up
      the open-surface cleanup suite and is future work). Featureless and open
      meshes take the historical path; the planar flood fill seeds ONLY from
      crease pins (spreading boundary pins regressed open flat scans). Env
      trio retired in favor of kill switches: `CYBER_QC_FEATURE_DEG=40`
      restores knife-edge binding, `CYBER_QC_NO_PLANAR_FILL`,
      `CYBER_QC_NO_UV_SNAP`, `CYBER_QC_NO_FEATURE_PIN`. Final defaults, target
      600: box_sharp recall **1.000**, sing **8**, angle **0.00°**, pure quads;
      cylinder recall **0.945**, sing 9, hausdorff 0.0053. Full suite 13/13,
      bench gate green; baselines re-recorded to lock the new floor.
    - Noted en route: the native path is NONDETERMINISTIC run-to-run on the
      same machine (torus outputs differ bit-wise across identical
      invocations; parallel CG reduction suspected) — violates the
      remeshing-pipeline determinism requirement independently of this work.
- **quad-quality-push finding — IMPLEMENTED (2026-08-02, `feat/dipole-quadcover`):**
  the shipped val-3/5 dipole canceller (`fixValence`) was unreachable from the
  quad-cover path (sole caller was the integer extractor). It is now exposed as
  `quadMeshValenceCleanup` (position_field.hpp) — the same doublet-dissolution +
  edge-rotation fixpoint, extended with `pinned` vertices (treated exactly like
  boundary) and `reservedEdges` — and runs in `QuadCoverQuadrangulator` after cap
  elimination and the count-calibration loop. Non-quad caps are frozen (vertices
  pinned, edges reserved) and crease vertices (face-pair dihedral over the run's
  feature threshold) are pinned so quads never rotate off a feature line. The
  integer path is byte-identical (empty constraints); kill switch
  `CYBER_QC_NO_DIPOLE`. Measured (A/B via the switch, cyber only): singularities
  armadillo 581 -> 550, nefertiti 650 -> 587, torus 68 -> 60, spot 72 -> 70,
  sphere 37 -> 35; feature recall, hausdorff and angle unchanged (< 0.1 deg / <
  0.002 recall drift); bunny-ear irregulars 46 -> 41. `flow_loop_mean_len` did
  NOT move (armadillo 20.2 -> 20.1) — dipole density was not the binding
  constraint on loop length; the remaining flow-loop gap needs a global lever
  (Bi-MDF-style quantization), not local surgery.
- **Gap #4, dense-organic singularity count (2026-08-02, `feat/field-singularities`):
  cone dipoles are cancellable at the FIELD level; the multires hierarchy is
  now shippable but still gated.** Two levers, measured separately and stacked.
  This is explicitly NOT a retry of the "field levers exhausted" finding (c7):
  that ruled out *tuning the smoother's energy* on the single-level field —
  lever B is a topological edit of the field's singularity set (smoothing
  provably cannot annihilate a dipole), and lever A is the hierarchy c7 left
  open behind its broken prolongation.
  - **B — field-level dipole annihilation (`annihilateFieldDipoles`, DEFAULT
    ON, kill switch `CYBER_QC_NO_CONE_MERGE`, radius
    `CYBER_QC_CONE_MERGE_RADIUS` default 5 hops).** `buildSeamlessSetup`
    promoted EVERY cross-field defect to a cone — no merge, prune or
    relocation anywhere in the solve — and on organics most defects are
    topologically-null noise (nefertiti: 997 singular vertices at totalIndex
    122, armadillo 489 at 27). The pass BFSes a short path between a close-by
    (+1,-1) pair over interior non-feature edges away from pinned faces, edits
    the signed period jumps along it (standard 4-RoSy merge: a unit change
    shifts the endpoint indices by ∓1, interiors cancel), then re-relaxes the
    face angles with jumps frozen so the field absorbs the 90° residual.
    Everything downstream is recomputed from the modified field, so
    `periodJump` / `combedDirection` / rho semantics and the feature binding
    (0e0b554) are untouched.
  - **A — multires hand-off relax (`CYBER_QC_CROSSFIELD_MULTIRES`, still
    OPT-IN).** `computeCrossField`'s three phases are now shared helpers
    (`initFrames` / `applyPins` / `transportSmooth`), and
    `computeCrossFieldFromOrientation` pins identically then relaxes its
    vertex-to-face hand-off with the same converged transport solve seeded
    from the hierarchy. Stock output verified byte-identical. Per the plan's
    mandatory scope cut, `smoothOrientation`'s convergence is NOT touched (it
    feeds `computePositionField`, which the InstantMeshes and integer
    extractors also consume).
  - **Measured** (cyber-pure = `--pure-quads`, target 8000 / spot 3000):

    | config | nefertiti sing/angle/recall/haus/loops | armadillo | spot |
    |---|---|---|---|
    | baseline | 401 / 9.04 / 0.887 / 0.0038 / 2601 | 377 / 9.11 / 0.761 / 0.0071 / 483 | 93 / 6.43 / 0.587 / 0.0059 / 433 |
    | B (default) | **347** / 9.50 / 0.861 / 0.0040 / 1631 | **299** / 9.02 / 0.806 / 0.0060 / 497 | **79** / 6.72 / 0.541 / 0.0048 / 561 |
    | A only | 346 / 8.72 / 0.870 / 0.0040 / 1187 | 297 / 8.76 / 0.820 / 0.0059 / 457 | 87 / 7.49 / 0.645 / 0.0046 / 811 |
    | A+B | **316** / 8.51 / 0.902 / 0.0039 / 1892 | **231** / 8.68 / 0.743 / 0.0062 / 430 | **65** / 7.81 / 0.610 / 0.0067 / 1224 |

    fandisk@3000 (B, default): sing 77 → 68, recall 0.413 → 0.431, hausdorff
    0.0089 → 0.0080. Generated corpus unchanged, box_sharp keeps 8 cones /
    0.00° / recall 1.00; ctest 13/13, `bench.py check` green.
  - **Exit gate NOT met.** The gate was nefertiti ≤ 250 AND armadillo ≤ 250.
    armadillo reaches 231 with A+B (and 299 on the default); nefertiti bottoms
    out at 316. Its limiter is measurable and specific: 1805 feature edges on
    the coarse remesh pin a dense wrinkle web, so 349 of its 521 +cones never
    find a partner over non-pinned paths at all, and radius 12/16 outputs are
    byte-identical to radius 8 — the pass is saturated, not under-tuned.
    Getting nefertiti under 250 needs the pseudo-feature web itself addressed
    (or a global quantization lever), not more dipole surgery.
  - **NEW DO-NOT-RETRY:**
    - *Pinned-adjacent cancellation paths* (`CYBER_QC_CONE_MERGE_LOOSE`,
      removed): letting paths ride pinned-face borders took nefertiti's
      attempts 199 → 866 but cancellations only 171 → 172 — 632 reverts. The
      injected 90° residual cannot diffuse past a pinned face, exactly the
      shear-band failure predicted; the plan's "skip paths adjacent to pins"
      is confirmed necessary, not optional.
    - *A smoothness-energy homotopy guard* (removed): "region residual energy
      must not increase" rejected 72 of armadillo's 169 pairs whose acceptance
      was already metric-clean, and it caught NEITHER torus failure mode. Use
      the region-disk and curvature-structure guards instead.
    - *A global genus gate* instead of a per-region disk check: organic work
      meshes carry accidental bridge handles (armadillo: 56 non-manifold pinch
      edges; the excised complex is genus-positive) so a genus gate disables
      the pass entirely on exactly the meshes it helps.
    - *Flipping `CYBER_QC_CROSSFIELD_MULTIRES` to default*: the corpus A/B is
      NOT clean. box_sharp and cylinder are bit-identical and sphere improves
      sharply (sing 35 → 24, angle 13.1 → 7.8), fandisk improves (sing 50 → 47,
      recall 0.772 → 0.730), but the **torus regresses past bench tolerance**
      (hausdorff 0.0281 → 0.0495, +76% vs a 30% budget; angle 20.4 → 25.6).
      Keep it env-gated until the handle case is understood.
  - **Two guards were bench-caught and are load-bearing** — both found by the
    torus, both invisible to the vertex-index verification:
    1. *Region disk topology.* Per-vertex index checks span the dual cycle
       space only when the relaxed faces form a disk. On an annulus region the
       wrap generator's rotational holonomy is unchecked, so a jump edit can
       twist the field 90° around a tube with every index intact: torus
       sing 64 → 140 / angle 20.4 → 30.7 while its FIELD got *cleaner*
       (32 → 6 cones). Regions that wrap retry at 1/2 then 1/3 radius.
    2. *Curvature structure.* Geometry-demanded cone pairs are not noise;
       cancelling them still cost the torus sing 64 → 75 / angle 20.4 → 26.1.
       The discriminator is the 2-ring defect CONSISTENCY ratio |Σd|/Σ|d|:
       structured curvature has every vertex curving the same way (torus pairs
       r≈0.99), wrinkle noise alternates (armadillo median r 0.36). Reject at
       |defect| > 10° and r > 0.6 — 13/13 torus pairs, 18% of armadillo's.
- **The wall, quantified** (generated corpus, target 600, defaults): quad-cover
  wins singularity structure outright (cylinder 21 vs QuadriFlow 8 at similar
  angle quality; sphere 37 vs 8) but **feature recall collapses on sharp
  geometry — box_sharp 0.04 vs QuadriFlow 1.00** (Phase 3's known
  feature-following limitation, now a tracked number). Priorities that follow:
  feature-constrained seamless UVs / extraction snapping first; flow-loop
  length second (quadwild's Bi-MDF quantization is the reference point).

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

**If it is ever reopened**, the remaining honest entry points are (c4) cone
placement at crease corners and (c6) crease-preserving surface projection — both
Tier 2, neither cheap, and both still capped at 1/6 until the routing question is
answered. **(c7) a globally-optimal direction field is no longer an entry point:
it was built and measured (below), recovers under half the native→Geogram gap, and
established that the residual is per-face *discretization*, not the field — so
field-level levers are exhausted.**

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
- 🔬 **(c7) Knöppel–Crane globally-optimal direction field — BUILT AND MEASURED
  (2026-07-24). A real but insufficient field-level win; it does NOT close the gap
  to Geogram, and field-level levers are now exhausted.**
  - **Motivation (de-risked first).** A scoping measurement forced the organics
    through the native solver (`CYBER_QC_ROUTE_CREASE=0.0001`) and decomposed the
    native→vendored gap: it is **field-dominated** — native produces ~2× the
    spurious singularities of Geogram's field (spot irregular 1.99 vendored → 4.63
    native, count-matched to 1%), and the median gap is largely downstream of that.
    So a globally-optimal field was the right lever to try.
  - **What was built.** A dependency-free per-face connection-Laplacian 4-RoSy
    field via inverse power iteration on the existing spmv + CG (no eigensolver
    dependency, no SciPP), reusing the same frames / transport phases / feature
    pins as the iterative field so c1/c2 are honoured. Globally optimal, verified
    by a seed-independent Dirichlet energy.
  - **Result — real, count-matched, independently verified (converged tol 1e-5):**
    | model | metric | current native | **KC native** | vendored (Geogram) |
    |---|---|---|---|---|
    | spot | irregular % | 4.63 | **3.45** | 1.99 |
    | spot | median | 79.93 | **81.87** | 84.24 |
    | spot | field singular | 77 | **73** | — |
    | cheburashka | irregular % | 5.71 | **4.78** | 3.68 |

    KC cuts spurious singularities and recovers median on both organics — it closes
    **~45% of the native→Geogram irregular gap** on the count-matched spot row. But
    it does **not** reach Geogram (3.45 vs 1.99). ⚠️ **Do not quote the "2.96"
    figure that appeared in an interim writeup — it was an under-converged
    inverse-iteration artifact (tol 1e-3), caught by the workflow's own critics;
    the honest converged number is 3.45, reproduced by independent A/B.**
  - **Routing calculus UNCHANGED — the cap is not broken.** On fandisk (the only
    model that routes native by default) KC is flat-to-negative (irregular
    2.90→2.94, median −0.8°); on stanford-bunny it is a count-matched regression.
    So forcing organics native-KC still loses to vendored, and `computeSeamlessUv`
    routing is untouched.
  - **Two sub-variants BUILT AND REFUTED, do not retry:** (i) dual-cotan edge
    weights (regresses spot 3.81→4.18; on a near-uniform remesh the dual/primal
    ratio is nearly constant so it barely differs); (ii) the paper-faithful
    per-vertex cotan Laplacian (the vertex→face projection re-winds phases and
    injects *more* cones than the per-face form).
  - **The conclusion that matters: the residual native→Geogram gap is per-face
    DISCRETIZATION, not local-vs-global optimization.** A globally-optimal field —
    the strongest field-level lever — recovers under half the gap. **Every
    field-level lever (reweighting, re-domaining, global-vs-local) is now measured;
    none closes it.** The remaining path is downstream (extraction / integer
    rounding), NOT a smarter field. ⚠️ Note the workflow suggested "per-feature-edge
    integer constraints" as the next step — that is **M2d, already refuted** (see
    the dead-lever list); its overlap here is a coincidence of naming, not a live
    lead.
  - **Foundation preserved off-main.** The working KC eigensolver lives on branch
    `feat/knoppel-crane-field` (pushed to origin), NOT merged. It is off-by-default
    (`CYBER_QC_KC_FIELD`), byte-identical when off, compiles both with and without
    Geogram, and has a deterministic regression test (KC singular 2 < iterative 8,
    a wide margin). It was kept off main deliberately: it delivers **no default-path
    improvement** (organics still route vendored), so on main it would only add
    build weight and a float-based CI test for code nobody runs. Anyone resuming the
    discretization work should branch from there rather than re-derive the solver.

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
