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

#### Scenario: Topological validity is the corpus-wide guarantee
- **WHEN** the default `quad-cover` path runs on any model in the benchmark corpus
- **THEN** its output SHALL be at least as topologically valid as QuadriFlow's on every model, counting non-manifold edges, boundary edges and degenerate faces — measured as 0 defects on spot, fandisk, rocker-arm and cheburashka, and 8 vs QuadriFlow's 38 on stanford-bunny
- **AND** it SHALL remain manifold on degenerate flat CAD input where QuadriFlow tears: a subdivided sharp-edged cube at ~2100 quads gives 0 defects against QuadriFlow's 576 boundary edges (`examples/12_cad_robustness.py`). The reference's tear count scales with the requested density and is not a fixed figure; the normative part is that ours is 0 and QuadriFlow's is in the hundreds

#### Scenario: Feature-following is a known gap
- **WHEN** output fidelity to source creases is measured against QuadriFlow
- **THEN** the default SHALL be reported as trailing, not matching: fandisk 0.82 vs 0.41 (2.0x), cheburashka 1.15 vs 0.57 (2.0x) and rocker-arm 0.61 vs 0.40 (1.5x), with spot a tie (0.46 vs 0.44, only 73 crease edges) and stanford-bunny confounded because the metric counts its scan-hole boundary as a feature
- **AND** the cause SHALL be recorded as un-pinned integer-grid *phase* (the cross field is crease-aligned; the quad loops sit about half a cell off the creases), so that closing it is understood to require per-feature-edge integer constraints in the parameterization rather than a post-extraction snap

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
