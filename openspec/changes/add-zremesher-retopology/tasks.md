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
- [ ] B3. Deterministic exact-vertex crossing rules (ties broken by a stable
       key, never by floating-point sign).
       Gate: zero organic T-mesh fallbacks caused by folded patch sectors on
       spot / fandisk / nefertiti; synthetic negative-index cone fixture passes.

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

- [ ] D1. `SizingField` h(x) fusing painted density, curvature, feature
       proximity and local thickness into one per-face target spacing.
- [ ] D2. Feed the sizing field into Bi-MDF arc target lengths.
- [ ] D3. Feed it into the position field and the final projected optimization.
- [ ] D4. Legacy behavior byte-identical when the sizing field is disabled.
       Gate: painted-density, target-count and thin-feature regression suites.

## Phase E — Artist topology guides

- [ ] E1. Guide mode (orientation vs topology) in the core guidance model and
       its serialization.
- [ ] E2. Orientation mode stays byte-compatible with today's flow guides.
- [ ] E3. Deterministic projection of topology guides to surface paths.
- [ ] E4. Insert topology guides into `TopologyLayout` as first-class arcs.
- [ ] E5. Closed topology guides (loop guides).
       Gate: sphere-equator, eye-loop and mouth-loop fixtures show measurable
       topology-edge adherence.

## Phase F — Semantic boundaries and symmetry

- [ ] F1. `ConstraintField` (semantic / group / user-preserved boundaries).
- [ ] F2. Connect groups, material boundaries and user-preserved edges to the
       field pinning, the layout and the sizing field.
- [ ] F3. Forced X/Y/Z exact half-mesh solve producing mirrored connectivity,
       not merely mirrored positions.
- [ ] F4. Automatic symmetry detection, only once forced symmetry is solid.

## Phase G — Candidate selection

- [ ] G1. Build native and multiresolution cross-field candidates.
- [ ] G2. Common `layout_score` over geometry, quad shape, topology, flow,
       guide adherence, features and symmetry.
- [ ] G3. `Best` mode runs both candidates and picks by score with a stable
       tie-break.
- [ ] G4. `Balanced` mode predicts or cheaply probes instead of solving both.
       Gate: `Best` is never worse than either candidate by the score, plus a
       manual visual inspection so the score itself does not become the bug.

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
