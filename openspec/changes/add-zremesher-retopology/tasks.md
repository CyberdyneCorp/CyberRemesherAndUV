# Tasks: add-zremesher-retopology

Implementation order follows the design's Phase A-G sequencing. Every phase
lands behind a flag with the shipped pipeline as the byte-exact default until
its own gate is measured green.

## Phase A — Structural refactor

- [x] A1. Promote the Bi-MDF T-mesh to a reusable `TopologyLayout`
       (`topology_layout.hpp/.cpp`): nodes (singularity / boundary corner /
       feature corner / T-junction / guide anchor / symmetry anchor), arcs
       (separatrix / feature / boundary / guide / symmetry) carrying samples,
       desired and quantized lengths, and patches with ordered boundary sides.
       `bimdf_quantize` keeps only quantization and consumes the layout.
       Gate: byte-identical output on every currently successful Bi-MDF case.
       — landed. The split is by RESPONSIBILITY rather than by moving 3000
       lines of tracing: `bimdf::TMesh` stays the QUANTIZATION view (arc
       lengths + exact symbolic lengths over the solver's promoted variables),
       and `TopologyLayout` is the GEOMETRIC/COMBINATORIAL view with no solver
       variables. Node and arc ids are shared. The tracer gained geometry
       capture (`Charts::captureGeometry` → `TMesh::nodeGeom`/`arcGeom`):
       T-node positions at creation, separatrix polylines sliced out of the
       walk trail by the events' own monotone curve parameter, crease/boundary
       polylines from the chain vertices. Nothing in `solveBimdf` reads any of
       it, so capture cannot move an assignment — verified byte-identical
       output with capture on vs off across all six corpus models.
- [x] A2. Deterministic layout validation (`validateTopologyLayout`) covering
       the Appendix-B invariants, plus layout debug export (JSON report and a
       polyline OBJ) and stats. Gate: every currently successful layout
       validates.
       — landed behind `CYBER_ZR_LAYOUT` (=1 reports; a value is a path prefix
       and also writes `<prefix>.json` / `<prefix>.obj`). Validation separates
       HARD violations (corrupt graph: bad ids, non-finite, out-of-range,
       dangling arcs) from LOCAL ones (a patch whose boundary walk does not
       close), mirroring the tracer's existing containment: the local case is
       reported per patch and its arcs are excluded, the sound remainder
       proceeds. All six corpus models validate; rocker-arm carries one
       contained non-closing patch of 199, which is a Phase B target.
       `examples/21_topology_layout.py` renders the layout over the quads.

## Phase B — Organic robustness

- [x] B0. Measurement first. `examples/22_layout_robustness.py` sweeps the
       corpus x target counts x adaptivity and reports, per cell, the T-mesh
       and layout counters plus a breakdown of WHY each orbit was contained
       (sector winding / corner count / side mismatch / abandoned cone), which
       the tracer did not previously expose. Baseline at 2000 quads,
       adaptivity 0: 178 rejected orbits, 1 non-closing patch, 57 abandoned
       launches; reasons 88 sectors, 52 corners, 9 side mismatch, 29 abandoned
       cone. Without this the phase is guesswork.
- [~] B1/B2. Fold-robust classification, behind `CYBER_ZR_FOLD_REPAIR`
       (`Charts::foldRepair`). Two levers landed, both output-neutral on the
       corpus and both strictly better estimates:
       * **Feasible-rotation projection** (`projectSectors`, unit-tested). The
         [1,2] corner/pass-through range is what a rotation system around a
         node requires, so when the measured winding admits any in-range
         assignment, the closest one beats an out-of-range largest-remainder
         rounding. Infeasible windings still fall through to containment.
       * **Winding lift target**. The QEx Alg. 8 lift targeted the number of
         incident arc ends; at a negative-index cone that undercounts badly
         (a valence-5 cone with two surviving ends lifted to 2, not 5). It now
         targets the topological winding.
       Measured: degraded nodes rocker-arm 17 -> 13, bunny 11 -> 5; reclass
       failures rocker-arm 15 -> 11, bunny 8 -> 2; **rejected orbits
       unchanged** (37, 47) and output byte-identical. The gate is NOT met.
- [x] B3. Deterministic exact-vertex crossing rules (ties broken by a stable
       key, never by floating-point sign).
       — **not needed.** The hard-failure path this would replace
       (`"exact hit on regular vertex"`) fires ZERO times across the whole
       corpus at 2000 quads. The tracer's existing rule — only cone and
       crease-chain vertices are events, a near-hit on a plain regular vertex
       passes through — already covers it. Recorded rather than implemented, so
       nobody spends the effort twice.

### Phase B re-assessed: the gate was measuring something with no output effect

Re-measuring B end to end produced the finding that reframes the whole phase.

**Recovering rejected orbits does not change the output at all.** Sweeping a
recovery lever until the bunny's contained regions fell from 84 to 66 left the
result mesh **byte-identical**, and the quality score unchanged to three
decimals, on every model tried.

The reason is upstream of tracing entirely. The Bi-MDF assignment only reaches
the mesh through INJECTION, and injection needs an arc's symbolic length to
reduce onto the INTEGER free variables. Measured per model:

| model | arcs | non-injectable | injected |
|---|---|---|---|
| cube | 12 | **0** | works |
| stanford-bunny | 585 | 432 (74%) | 0 |
| rocker-arm | 736 | 653 (89%) | 0 |
| cheburashka | 648 | 616 (95%) | 0 |
| fandisk | 644 | 610 (95%) | 0 |
| spot | 438 | **438 (100%)** | 0 |

On a crease-pinned cube every arc runs between pinned crease isolines, so its
length IS an integer combination and the machinery works perfectly. On an
organic mesh a separatrix arc runs between T-nodes whose positions are
continuous, so its length is not expressible in the integer basis and the arc is
dropped. This is the joint half-integer lattice blocker the ROADMAP already
records; what is new here is the measurement of how total it is.

**Consequence: the Phase B gate as written ("zero fallbacks caused by folded
patch sectors") cannot change output on organics**, because even a perfect,
zero-rejection T-mesh would still have 74-100% of its arcs non-injectable. Every
lever measured inert on output for that reason, not because the levers were
wrong. The gate should be replaced by the injectability one before any further
tracing work is done.

**Also measured and reverted: a bigon (two-corner) patch template.** Two-corner
orbits are the single largest rejected shape (bunny 26 of 72, rocker-arm 15 of
35) and the tracer's surgery deliberately KEEPS them as genuine thin strips,
which the templates then refuse for having fewer than three corners. A lune's
constraint is just "both sides carry the same length" — one of the quad
template's two pairs — so it looks like a cheap recovery. Implemented, it
recovered 5 orbits on the bunny with byte-identical output, and then FAILED its
own smallest correctness fixture: on two lunes sharing two arcs the solve
returned 11 and 7 half-cells instead of an equal pair, because the T-join parity
pass breaks a lone balance edge and `maxSideViolation` does not audit non-quads.
Reverted rather than shipped: a quantizer path that fails its own correctness
test, for zero measured benefit, is not worth the risk. A real attempt needs the
paper's template treatment, not one inner edge.

### REFUTED in Phase B (do not retry without new evidence)

- **Overriding the developed winding with the topological one.** The reasoning
  was sound: the winding is the fan's seam holonomy lifted to the field's cone
  index, the developed angle sum only measures a map that folds, and QEx
  Alg. 8 recovers a lost turn *only* by charging +2pi to a NEGATIVE run — so
  the dominant corpus failure, a valence-5 cone whose **fold-free** wedge fan
  develops to a single quarter, is one it structurally cannot fix. Forcing
  `measuredTotal = expectedTotal + nPh` and rescaling the gaps onto it made
  things clearly worse: rejected orbits bunny 47 -> 69, cheburashka 12 -> 23,
  rocker-arm 37 -> 39, fandisk sector rejections 2 -> 4.

### What the measurements say the next lever is

The classifier is largely producing the RIGHT answer; the layout is genuinely
defective where it is contained. Evidence:

- The dominant residual is a cone where the recorded field index and the
  developed fan geometry **disagree outright** (index -1, winding 5, versus a
  fold-free fan developing to 1 quarter). Forcing either side over the other
  corrupts neighbouring orbits, so the disagreement is upstream — in the
  field's cone index or in the parameterization — not in this classifier.
- On the bunny, 37 of 57 abandoned launches are `ray reached an open boundary`,
  not fold damage. Boundary chains already exist (`CYBER_QC_BIMDF_BARC`) and
  take failed launches 20 -> 1 and rejected orbits 47 -> 39, but they are off
  by default because they regress the *guided rounding*. Decoupling the
  layout's tracing options from the shipped quantizer's is the fix, and it
  belongs with the public `zremesher` method (P1), which owns its own
  quantization decisions.
- Nodes that are genuinely UNSEATABLE (winding > 2x their arc ends) are few
  once measured correctly: fandisk 1, cheburashka 2, rocker-arm 3, bunny 37 —
  and the bunny's 37 are exactly its boundary-abandoned rays.

## Phase C — Topology quality

- [x] C1. `GeometryAnalysis` (normalized curvature, thin-feature risk,
       feature/boundary influence) computed once per solve and cached.
       — landed (`geometry_analysis.hpp/.cpp`). Per-vertex salience, all
       normalized to [0, 1] so the optimizer's weights are comparable to each
       other rather than to arbitrary units. Curvature is the widest angle
       between any two face normals around a vertex over pi — a proxy for both
       principal curvatures at once, since Gaussian curvature alone reports a
       cylinder as flat. Feature and boundary influence are multi-source
       Dijkstra over mesh edges (surface distance, not straight-line, so the far
       side of a thin plate is not "near" a crease on the front), with ties
       broken on vertex id. Thickness is the BVH opposing-surface probe from the
       design's Sec. 9.2: an inward ray fan, nearest hit wins, stepped off the
       surface so it cannot re-hit its own triangles. Without a BVH thickness is
       INFINITE, not zero — zero would read as "infinitely thin" and make every
       cone look catastrophic.
- [x] C2. Layout-aware weighted singularity cost + metric reporting.
       — landed (`layout_score.hpp/.cpp`). Quad meshes require extraordinary
       vertices, so counting them is the wrong primary metric: two layouts with
       the same count differ entirely by WHERE the cones sit. The cost is a
       weighted sum of the salience terms, each scaled by |index|. Reported per
       solve alongside the layout stats, and the same `singularityCost` the
       relocation pass will evaluate candidates with — a relocation optimizing a
       different cost than the metric would be optimizing something nobody
       measures.

       Baseline at 2000 quads (`--quad-method zremesher`):

       | model | cones | weighted cost | mean | worst | featureInfluence mean / max |
       |---|---|---|---|---|---|
       | spot | 59 | 137.2 | 2.33 | 10.08 | 0.118 / 1.000 |
       | rocker-arm | 107 | 306.2 | 2.86 | 12.64 | 0.254 / 1.000 |
       | stanford-bunny | 87 | 324.6 | 3.73 | 12.82 | 0.423 / 1.000 |
       | fandisk | 93 | 374.6 | 4.03 | 13.39 | 0.412 / 1.000 |
       | cheburashka | 92 | 434.5 | 4.72 | 20.72 | 0.509 / 1.000 |

       **The first version of this metric was WRONG, caught by a validity check
       before any optimizer chased it.** It scored a cube's eight corner cones
       at mean 6.00 — the worst of the whole corpus — and called 100% of that
       cost badly placed. But eight corner cones ARE a cube's optimal topology;
       an optimizer minimizing that metric would have been driven to make the
       cube worse.

       The fix is to separate the two very different things "a cone near a
       feature" can mean. A cone on a crease CURVE interrupts a loop that should
       run along it, and is a defect. A cone at a feature JUNCTION — a crease
       endpoint, three creases meeting, or a sharp turn — is not: the surface
       branches there and the quads must too. `GeometryAnalysis::featureCorner`
       marks junctions, and the feature term is charged only off them. Cube mean
       6.00 -> 2.00, and the residue is honest (a cube corner really is a
       high-curvature point; the cone there is simply required).

       The corner test is validated by the cube itself: **8 junctions out of 140
       feature vertices**, which is exactly its eight real corners. On the other
       models the junction share also measures crease FRAGMENTATION — fandisk 83
       of 313, matching the known "205 crease edges -> 83 fragments" finding
       exactly; cheburashka 62%; rocker-arm 83%. Where creases are shattered the
       feature term is correspondingly less trustworthy, and the solve now
       reports that share so a reader can tell.

       Cost split at 2000 quads, which is what says whether C3 has anything to
       win — the count term is irreducible, so it bounds any relocation:

       | model | count | curvature | feature | boundary | movable |
       |---|---|---|---|---|---|
       | cube | 8.0 | 8.0 | 0.0 | 0.0 | 50% |
       | spot | 62.0 | 45.2 | 26.0 | 0.0 | 54% |
       | rocker-arm | 110.0 | 79.6 | 72.5 | 0.0 | 58% |
       | fandisk | 100.0 | 96.4 | 62.2 | 0.0 | 61% |
       | cheburashka | 104.0 | 103.5 | 119.0 | 0.0 | 68% |
       | stanford-bunny | 88.0 | 62.9 | 107.2 | 22.6 | 69% |

       **50-69% of the weighted cost is movable**, so relocation has a real
       target. The thin term is 0 throughout because the thickness probe needs a
       BVH over the SOURCE surface and the metric runs on the work mesh
       mid-solve; boundary fires only on the bunny, the one open surface, which
       is correct.
- [~] C3. Deterministic local singularity relocation under a hard total-index
       invariant, rebuilding dependent cut/layout data.
       — BUILT and MEASURED behind `CYBER_ZR_RELOCATE`; **not enabled**, because
       the gate as written is not sufficient.

       The implementation is small because it reuses the calibrated dipole
       machinery rather than duplicating it: that pass already walks a path from
       a cone and forces each vertex to a TARGET index, with a disk-topology
       guard, a structured-curvature guard, a frozen-jump relax and a
       verify-and-revert. Cancellation ends the path at a -1 cone with target 0;
       relocation ends it at a REGULAR vertex with target +1. Only the endpoint
       target and the BFS predicate differ, so every guard is shared. Candidates
       are the surviving +1 cones, worst-placed first, moving only to a strictly
       better vertex, with stable id tie-breaks.

       The stated gate is MET: weighted cost falls on every model, the cone
       COUNT is unchanged, `totalIndex` is unchanged (Poincare-Hopf, pinned by a
       test), the layout still validates, and the cube — whose eight corner
       cones are optimal — is correctly left untouched.

       | model | cost before | after | relocated |
       |---|---|---|---|
       | spot | 133.20 | 132.87 | few |
       | fandisk | 258.63 | 254.91 | 5 |
       | rocker-arm | 262.17 | 258.29 | 12 |
       | cheburashka | 326.49 | 324.42 | 6 |
       | stanford-bunny | 280.62 | 278.72 | few |
       | cube | 16.00 | 16.00 | 0 (correct) |

       **But the layout gets worse where the metric gets better**: excluded arcs
       rocker-arm 76 -> 94 and cheburashka 26 -> 28, non-quad patches fandisk
       24 -> 29 and rocker-arm 13 -> 20. Trading layout quality for a 0.2-1.5%
       metric gain is not a win, and it shows the C gate ("cost improves, defects
       stay zero") does not capture what actually degraded. The honest fix is
       Phase G's combined score, which weighs layout quality alongside cone
       placement; relocation should be re-evaluated against THAT, not against
       the singularity cost alone.

       The rejection counters say why the ceiling is low: `rejectedCurv`
       dominates (rocker-arm 38 of 50 candidates, cheburashka 34 of 40). The
       structured-curvature guard is refusing to move cones the geometry
       demands — the same conclusion Phase B reached from the other direction.
- [~] C4. Conservative dipole cancellation on the layout.
       — the field-level pass already exists and is corpus-calibrated
       (`annihilateFieldDipoles`). What Phase C adds is choosing WHICH pairs to
       cancel by placement: under `CYBER_ZR_CONE_PRIORITY` the +1 cones are
       walked worst-placed first, so the cones the metric charges most get first
       claim on a partner instead of whichever had the lowest vertex id.
       **Measured near-inert**: fandisk 258.63 -> 258.49, rocker-arm
       262.17 -> 261.92, spot and cheburashka unchanged, and only the bunny
       moved materially (280.62 -> 273.00, two fewer cones). Most pairs are not
       contested, so the order rarely decides anything. Kept flag-gated with the
       numbers recorded rather than presented as a win.
       Gate: weighted singularity cost improves, raw topological defects stay
       zero, geometry metrics do not regress materially. **NOT met** — see C3.

## Phase D — Unified density

- [x] D1. `SizingField` h(x) fusing painted density, curvature, feature
       proximity and local thickness into one per-face target spacing.
       — landed (`sizing_field.hpp/.cpp`), with per-term weights so a regression
       is traceable to one input rather than to "the sizing field", hard scale
       bounds (a term that could drive the target toward zero would produce an
       unbounded quad count), and smoothing in LOG space — edge length is
       multiplicative, and smoothing it arithmetically biases every mixed
       neighbourhood coarser (h/2 and 2h average to 1.25h instead of h).
- [~] D2/D3. Feed the sizing field into the consumers.
       — the substrate consumer is WIRED and measured; **off by default,
       because it measured worse on its own yardstick.**

       The consumer that matters is the isotropic pre-remesh, which decides the
       substrate the whole solve runs on. It could not take a per-vertex field
       directly — it mutates the vertex set as it runs, so any per-vertex array
       goes stale immediately, which is why painted density was already a
       SPATIAL query. Its `ScaleField` already samples barycentrically off the
       fixed input surface, so the wiring is an extra per-input-vertex
       multiplier folded in there (`IsotropicOptions::extraVertexScale`, null =
       byte-identical, verified against the pre-change build on spot and
       fandisk).

       Then measured on what it exists for. `examples/23_thin_features.py`
       remeshes closed slabs and a fin at a target edge 3x COARSER than the
       feature is thick and checks whether the two sides stay distinct:

       | | thin plate | thinner plate | thin fin |
       |---|---|---|---|
       | uniform sizing | survived | COLLAPSED | survived |
       | unified sizing | survived | COLLAPSED | **COLLAPSED** |

       Unified sizing takes survival from 2 of 3 to 1 of 3 — the fin survives
       WITHOUT it and collapses with it. On the corpus it also adds cones
       (fandisk 93 -> 104, cheburashka 92 -> 106) for a slightly better MEAN
       placement, i.e. more cones each sitting a little better. Refining the
       substrate does not stop the extraction bridging a thin gap; it spends the
       budget getting there. An earlier variant that also refined near every
       crease was worse still (fandisk 119 cones, and the bunny's totalIndex
       moved 8 -> 4, a topology change).

       Left behind `CYBER_ZR_UNIFIED_SIZING` so the field has a consumer to be
       re-measured through once the EXTRACTION side can act on thickness — which
       is where the evidence says the fix has to be.
- [x] D4. Legacy behavior byte-identical when the sizing field is disabled.
       — verified: with `extraVertexScale` null the isotropic stage is
       byte-identical to the pre-change build, and the corpus returns exactly to
       its baseline numbers with the flag off.
       Gate: painted-density, target-count and thin-feature regression suites.
       **Thin-feature gate NOT met** — and now measured rather than assumed. A
       plate 3x thinner than the target edge collapses either way; that is a
       real limitation of the extraction, reported instead of hidden.

## Phase E — Artist topology guides

- [x] E1. Guide mode (orientation vs topology) in the core guidance model and
       its serialization.
       — `FlowGuide::mode` and `FlowGuide::closed`. The CLI sidecar takes
       `"mode": "orientation" | "topology"` and `"closed"`; an unrecognised mode
       is an ERROR, not a fallback, because a typo'd `"topolgy"` silently
       biasing the field instead of cutting a loop is precisely the failure this
       feature exists to remove. Carried to the backends on `GuidanceField`,
       which is otherwise deliberately geometric — a topology guide is not a
       point query, it has to be projected as a whole polyline.
- [x] E2. Orientation mode stays byte-compatible with today's flow guides.
       — `Orientation` is the default, so a guide authored before the field
       existed, or deserialized from an older sidecar, behaves exactly as before.
- [x] E3. Deterministic projection of topology guides to surface paths.
       — `projectGuideToPath`: snap each guide point to its nearest vertex, then
       JOIN consecutive snaps by shortest edge paths. Joining is what makes the
       result connected — a stroke sampled more coarsely than the mesh would
       otherwise skip vertices and leave gaps no edge chain can follow. Ties
       break on vertex id. It reports `maxDeviation`, so "the mesh is too coarse
       to represent this stroke" is visible rather than silently absorbed, and
       it DECLINES (empty path) rather than returning something broken.
- [x] E4. Insert topology guides into `TopologyLayout` as first-class arcs.
       — `insertGuideArcs` adds a `GuideAnchor` node per path vertex and a
       `Guide` arc between them, all locked: an artist put them there and no
       later stage may relocate them.
- [x] E5. Closed topology guides (loop guides).
       — a closed path's repeated first vertex becomes the closing ARC rather
       than a duplicate node, so a loop guide contributes one arc per node.

       The mechanism deliberately reuses creases rather than growing a parallel
       one. A pinned crease already IS "run an edge loop along this curve": the
       seamless solve makes it a hard seam and pins its isolines. So a topology
       guide is projected to an edge path on the work mesh and tagged as a
       feature — after the dihedral re-tag and its filters, so a guide is never
       mistaken for a sampling artifact and demoted.

       Gate: **MET**, and measured on the RESULT. Adherence is the fraction of
       the guide with an output edge both NEAR it and ALIGNED with it; requiring
       alignment matters, because edges crossing the guide at right angles are
       everywhere in a dense mesh and scored the ignore-the-stroke case at over
       80% before alignment was required.

       | fixture | orientation | topology |
       |---|---|---|
       | sphere equator (a natural field direction runs along it) | 51.6% | **87.5%** |
       | sphere loop tilted 35° (nothing about the geometry wants a loop there) | 52.3% | **86.7%** |

       `examples/24_topology_guides.py` is the artifact those numbers come from.
       A guide that cannot be projected is reported as unhonoured by name, never
       dropped.

## Phase F — Semantic boundaries and symmetry

- [ ] F1. `ConstraintField` (semantic / group / user-preserved boundaries).
- [ ] F2. Connect groups, material boundaries and user-preserved edges to the
       field pinning, the layout and the sizing field.
- [ ] F3. Forced X/Y/Z exact half-mesh solve producing mirrored connectivity,
       not merely mirrored positions.
- [ ] F4. Automatic symmetry detection, only once forced symmetry is solid.

## Phase G — Candidate selection

- [x] G1. Build native and multiresolution cross-field candidates.
       — the two cross fields the engine already has: the multiresolution
       orientation-derived field (the shipped default) and the single-level
       smoothed one. Both take the identical seamless solve and extraction, so
       the comparison isolates the field.
- [x] G2. Common quality score over geometry, quad shape and topology.
       — `scoreQuality` in `layout_score`: median-angle quality, edge
       uniformity, quad purity, irregular-vertex fraction, and a defect term
       weighted 100 so it dominates everything aesthetic. A mesh with excellent
       angles and a crack is not a winner.

       **The first version of this score was blind on the deciding axis.** It
       omitted the irregular-vertex term, and with only angle, uniformity and
       purity it selected the SAME candidate on all five models — the two
       candidates differ mostly in how many extraordinary vertices they need.
       Adding it made the selection actually vary.
- [x] G3. `Best` mode runs both candidates and picks by score with a stable
       tie-break.
       — `--quality best` (`ZRemesherOptions::quality`, or `CYBER_ZR_BEST`).
       Defects decide first, then the total, then non-quad count, then the
       candidate ORDER — which is fixed and documented, so the same input always
       selects the same candidate. The shipped default goes first and only loses
       to a strictly better score.

       This answers the open question the roadmap and the README both record: no
       static "organic vs CAD" threshold can pick the right field for every
       model, because rocker-arm prefers one while spot and cheburashka prefer
       the other and their crease fractions interleave. Measured per input:

       | model | multires | single-level | selected |
       |---|---|---|---|
       | spot | 2.606 | 2.611 | single-level |
       | fandisk | 2.523 | 2.522 | **multires** |
       | rocker-arm | 2.199 | 2.423 | single-level |
       | cheburashka | 1.757 (13 defects) | 2.459 | single-level |
       | stanford-bunny | 2.096 | 2.078 | **multires** |

       It genuinely splits — and it catches a real defect: the multires field
       leaves 13 topological defects on cheburashka, which the score rejects
       outright rather than trading against its better uniformity.
- [ ] G4. `Balanced` mode predicts or cheaply probes instead of solving both.
       — not attempted. `Best` costs a second full solve, and whether a cheap
       probe can predict the winner is only worth asking once the score itself
       is trusted on a wider corpus.
       Gate: `Best` is never worse than either candidate by the score — true by
       construction, and pinned by a test that a candidate with fewer defects
       always wins however good the other one's angles are.

## Product surface

- [x] P1. Public `zremesher` quad method: CLI `--quad-method zremesher` plus
       its flags, C API and Python binding, documented in `examples/README.md`
       and `docs/`.
       — landed. Structurally the quad-cover path with the topology-layout stage
       on and the tracing options the LAYOUT wants rather than the ones the
       shipped quantizer's guided rounding wants. This is what unblocked the
       boundary decoupling Phase B measured but could not use: `quad-cover`
       cannot turn boundary chains on (they reshape the flow its guided rounding
       is tuned against), `zremesher` can, because it does not use that
       rounding. Reachable as `--quad-method zremesher`,
       `CYBER_QUAD_ZREMESHER` (4) on the C ABI, and `quad_method="zremesher"`
       from Python; it always routes native, since the layout is traced from the
       native seamless map. `SeamlessLayoutOptions` replaces reading the
       environment deep inside the tracer, so the option is a caller's choice
       rather than a process-wide flag. Kill switches
       `CYBER_ZR_NO_BOUNDARY_CHAINS` / `CYBER_ZR_NO_FOLD_REPAIR` A/B each lever
       without a rebuild. The run report now names the EFFECTIVE quad method
       (after any build-capability fallback), which it did not before.

       Controlled A/B on the zremesher path, corpus at 2000 quads. Boundary
       chains touch only the one open surface — bunny abandoned launches 4 -> 2
       and 12 more patches recovered, at a flat rejection RATIO (38.6% -> 38.3%);
       every closed model is bit-identical with them on or off. Fold repair cuts
       degraded nodes (bunny 17 -> 11, rocker-arm 17 -> 16, cheburashka 3 -> 1)
       with rejections unchanged. So both levers are coverage and repair wins,
       and **neither moves the Phase B gate** — consistent with the Phase B
       finding that the layout is genuinely defective where it is contained.
- [ ] P2. Example scripts demonstrating the layout, topology guides, symmetry
       and quality modes on the bundled corpus.
- [ ] P3. Benchmark corpus additions (character and hard-surface) and the
       release gates from the design's acceptance table.
