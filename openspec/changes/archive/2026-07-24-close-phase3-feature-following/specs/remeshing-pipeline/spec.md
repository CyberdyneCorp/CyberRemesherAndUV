# remeshing-pipeline (delta)

## MODIFIED Requirements

### Requirement: Quad extraction and pure-quad option
Extraction SHALL trace integer isolines into a connection graph, simplify it (dissolve valence-2 vertices, collapse degenerate edges/cycles, prune dangling chains), and enumerate faces. Default output is quad-dominant. When the pure-quad option is enabled, a post-pass SHALL eliminate non-quads where topologically achievable (splitting/merging into quads) and the report SHALL state the exact residual non-quad count.

#### Scenario: Non-quad accounting
- **WHEN** any remesh completes
- **THEN** the report SHALL state quad count, non-quad count by arity, vertex count, and singularity count

#### Scenario: QuadCover seamless-UV extractor is the default
- **WHEN** a remesh runs without an explicit quadMethod
- **THEN** the pipeline SHALL use the `quad-cover` quadrangulator by default (CLI, C ABI, and bindings), which parameterizes each island with a seamless-UV solve and extracts integer isolines into quads, and SHALL degrade to the field-aligned quadrangulator (never silently to a corrupt mesh) when no seamless-UV solver can produce a valid result for an island

#### Scenario: Element quality against the QuadriFlow reference
- **WHEN** the default `quad-cover` path runs on the benchmark corpus (spot, fandisk, rocker-arm, cheburashka, stanford-bunny) with the in-process reference field compiled in (see the seamless-UV backend requirement), at a matched quad count
- **THEN** it SHALL match or beat QuadriFlow on median quad angle AND irregular-vertex count on **spot, rocker-arm and stanford-bunny**, and the benchmark SHALL report the per-model comparison rather than a single aggregate
- **AND** **fandisk** (median 83 vs 85, irregular 3% vs 1%) and **cheburashka** (median 80 vs 82, irregular 4% vs 2%, edge-length CV 0.22 vs 0.15) are known exceptions, tracked as the crease-alignment gap below; a claim of a corpus-wide or organic-corpus win on these axes is NOT supported by measurement

#### Scenario: Comparisons are matched on achieved quad count
- **WHEN** the benchmark compares the default against a reference, or one of our arms against another, on any per-polygon metric
- **THEN** both arms SHALL be driven to a matched *achieved* quad count by a bounded search, not compared at a matched *request*, because `target_quad_count` is a request that the extractors undershoot and QuadriFlow overshoots — historically by 11–16% at the same request
- **AND** a search that cannot reach the target within tolerance SHALL be reported as a miss rather than silently scored, since `feature_error` falls roughly as `count^-0.5` and an unmatched density gap is worth a large fraction of the gaps being measured

#### Scenario: Hole filling closes each boundary loop exactly once
- **WHEN** the isoline extractor closes a boundary loop with quads
- **THEN** each loop SHALL be closed at most once, and the extracted mesh SHALL NOT contain two faces built on the same vertex set
- **AND** this SHALL hold for the terminal 3-gon and 4-gon cases as well as the general reduction path, because the filler runs twice per loop (once score-checked, once not) and relies on the first pass to consume what it filled
- **AND** a duplicated face SHALL be treated as a defect in its own right, not only via its symptoms: two coincident quads are edge-count-manifold on their own, and the non-manifold rim only appears after the pure-quad subdivision gives each its own face point

#### Scenario: Topological validity is the corpus-wide guarantee
- **WHEN** the default `quad-cover` path runs on any model in the benchmark corpus
- **THEN** its output SHALL be at least as topologically valid as QuadriFlow's on every model, counting non-manifold edges, boundary edges and degenerate faces — measured as **0 defects on all SIX corpus models across requested densities 2600–4200**, against QuadriFlow's **680 on the cube** and **32 on stanford-bunny** at matched quad counts
- **AND** this SHALL be treated as the pipeline's strongest claim against the reference: it is the only axis on which the default beats QuadriFlow corpus-wide
- **AND** any defect figure SHALL be quoted with the density it was measured at. Before the hole-fill fix, stanford-bunny's count was non-monotone in density (8 at 2660 quads, 0 at 3004, 16 at 3308) purely according to whether the trace left a 4-edge loop, so a single-density reading was not a floor
- **AND** it SHALL remain manifold on degenerate flat CAD input where QuadriFlow tears (`examples/12_cad_robustness.py`). The reference's tear count scales with the requested density and is not a fixed figure; the normative part is that ours is 0 and QuadriFlow's is in the hundreds

#### Scenario: Feature-following is a known limitation, and Phase 3 is closed on it
- **WHEN** output fidelity to source creases is measured against QuadriFlow at a matched achieved quad count
- **THEN** the default SHALL be reported as trailing on **5 of 6** corpus models: cheburashka 1.00 vs 0.56, fandisk 0.62 vs 0.42, rocker-arm 0.48 vs 0.42, spot 0.41 vs 0.35, and stanford-bunny 1.36 vs 0.48 — the last marked confounded, because the metric counts its scan-hole boundary as a feature and 29% of its "features" are that rim
- **AND** the single model we win — **cube, 0.98 vs 2.69** — SHALL NOT be presented as closing the gap: it entered the corpus because it was already won, so the corpus-wide figure moving from 0/5 to 1/6 reflects added coverage, not improvement on a model previously lost
- **AND** this SHALL be recorded as a **known limitation** rather than an open work item. Eleven levers were measured across the post-extraction, gauge, routing, parameterization, field, pre-remesh and vendored-pre-remesh layers; two shipped (crease preservation through the isotropic stage, and planarity-gated crease field-alignment) and both are confined to the one model that clears the 2% native-routing threshold
- **AND** any proposal to reopen it SHALL first answer how it reaches the **vendored** backend, on which 4 of the 6 models run. The one lever that targets it (lowering the vendored sharp-edge threshold) is a count artifact — at 45° stanford-bunny's quad count rises 2644 → 15105 (+471%) — and cannot be decoupled without patching vendored source
- **AND** the cause SHALL NOT be recorded as un-pinned integer-grid *phase*: per-feature-edge integer constraints were implemented against that premise, verified exact on the maps they reach, and measured inert

#### Scenario: Field-aligned pairing maximizes quad-dominance
- **WHEN** the field-aligned quadrangulator pairs triangles into quads
- **THEN** it SHALL compute a maximum matching on the triangle-adjacency graph (a weighted-greedy seed that prefers field-diagonal, high-quality merges, followed by augmenting-path improvement that rescues triangles a greedy pass would strand), so quad-dominance is maximized (~95%+ on clean input) rather than left at the ~76–84% a greedy maximal matching produces, while surviving quad edges still follow the cross field. The field-aligned quadrangulator is the always-available fallback; it is selected explicitly via quadMethod and is the automatic degrade target when the default quad-cover path declines an island.

#### Scenario: Position-field quadrangulator option
- **WHEN** quadMethod selects the position-field (Instant-Meshes-style) extractor
- **THEN** the pipeline SHALL build a per-vertex 4-RoSy orientation field and a lattice position field, collapse mesh vertices into lattice cells (with post-collapse edge recovery so the collapsed graph reaches near-grid valence), and extract faces by a rotation-system walk with greedy-cut hole filling, yielding more uniform, field-aligned edge flow (edge-length CV competitive with field-based references such as QuadriFlow) with fewer, better-placed singularities; residual singularity triangles SHALL be resolved by the pure-quad path. It is offered as an opt-in alternative, never copying GPL sources. It is NOT the default and SHALL NOT be used as the subject of comparison claims about the shipped pipeline.

#### Scenario: Pure-quad post-pass
- **WHEN** the pure-quad option is enabled
- **THEN** the output SHALL contain only quads, or the report SHALL explicitly list the residual non-quads that could not be resolved

#### Scenario: Pure-quad element quality
- **WHEN** the pure-quad option is enabled
- **THEN** the construction (quad-dominant remesh at a fraction of the target density, then one linear subdivision) SHALL be followed by a tangential-relaxation de-slivering pass — interior vertices slide toward their 1-ring centroid within the tangent plane, re-projecting onto the source surface each iteration, with feature (sharp-crease) and boundary vertices held fixed — so the quads are well shaped (no slivers or collapsed edges) rather than merely all four-sided, while the silhouette follows the source curvature. For the position-field and quad-cover quadrangulators the relaxation SHALL instead fit each quad to a common-sized square (shape matching), regularising 90-degree corners and equal edge lengths together for uniformity competitive with field-based references. Charts produced on a uniform (integer-grid or seamless) base SHALL take a longer projected base-relax before subdivision, lifting median angle at no edge-length-CV cost.
