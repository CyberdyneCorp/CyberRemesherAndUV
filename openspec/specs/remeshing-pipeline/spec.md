# remeshing-pipeline Specification

## Purpose
TBD - created by archiving change bootstrap-v1-platform. Update Purpose after archive.
## Requirements
### Requirement: Pipeline stages
The automatic remesher SHALL execute, in order: (1) derive the target edge length from total surface area and the target quad count; (2) split the input into islands (accounting for every face — see mesh-core); (3) adaptively isotropic-remesh each island; (4) compute a frame-field-guided global parameterization per island; (5) extract quad-dominant faces by tracing integer UV isolines; (6) run cleanup passes; (7) merge per-island results deterministically (stable island order). Islands SHALL be processed in parallel where the solver permits.

#### Scenario: Multi-island input
- **WHEN** a mesh with several disconnected shells is remeshed
- **THEN** each shell SHALL be remeshed independently and the merged output SHALL contain every successful island in a deterministic order

#### Scenario: Empty input
- **WHEN** a mesh with zero faces is submitted
- **THEN** the pipeline SHALL return a typed "empty input" error without crashing, dividing by zero, or reporting success

### Requirement: Guarded target computation
The target-edge-length computation SHALL validate its inputs: a non-positive target quad count or zero surface area SHALL produce a typed parameter error before any stage runs. No code path SHALL evaluate a division whose denominator can be zero from user input.

#### Scenario: Zero target quads
- **WHEN** a caller requests 0 target quads
- **THEN** the pipeline SHALL reject the request with a parameter error (AutoRemesher produced inf/NaN edge length here)

### Requirement: Feature-preserving isotropic stage
The isotropic stage SHALL split long edges, collapse short edges, flip toward valence 6, tangentially smooth, and re-project to the source surface, keeping edge lengths within a documented band around the (adaptivity-scaled) target. Feature-edge vertices SHALL NOT be collapsed, projected off the feature, or crossed by edge flips; when smooth-normal degrees > 0 projection SHALL target a smoothed (PN-triangle or equivalent) surface.

#### Scenario: Edge flips respect features
- **WHEN** the isotropic stage runs on a mesh with tagged sharp edges
- **THEN** no edge flip SHALL create an edge crossing a feature edge (AutoRemesher's flip guard was commented out)

### Requirement: Per-island parameterization with instance-local state
Each island SHALL be parameterized by a frame-field-driven mixed-integer quad-cover solve behind a solver interface (`IParameterizer`), with hard feature-edge constraints and per-face adaptive scaling. Solver progress and diagnostic state SHALL be instance-local so multiple islands can solve concurrently (no process-global solver state).

#### Scenario: Concurrent island solves
- **WHEN** a mesh with 4+ islands is remeshed on a multi-core machine
- **THEN** parameterization of distinct islands SHALL be able to proceed concurrently (AutoRemesher serialized these behind a global-progress spinlock)

### Requirement: Island failures are reported, not swallowed
When an island's parameterization or extraction fails, the pipeline SHALL record a per-island diagnostic (island index, face count, failure stage, reason) in the run report and surface a warning to the caller; the remaining islands SHALL still complete. The overall run SHALL be marked partial, not successful.

#### Scenario: One island fails
- **WHEN** one of three islands throws during parameterization
- **THEN** the result SHALL contain the two successful islands, the report SHALL identify the failed island and reason, and the run status SHALL be "partial" (AutoRemesher dropped the island silently)

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

### Requirement: Explicit cleanup policies
Hole filling, small-patch removal, and non-manifold face removal SHALL be governed by named, documented policies with user-visible parameters (maximum hole-boundary length; minimum patch size or keep-all), not hard-coded constants. Defaults SHALL be recorded in remeshing-parameters.

#### Scenario: Large hole policy
- **WHEN** extraction leaves a boundary loop longer than the configured hole-fill limit
- **THEN** the loop SHALL be left open and the report SHALL list it (AutoRemesher hard-coded 65 vertices and reported nothing)

#### Scenario: Disconnected patch policy
- **WHEN** extraction produces a main body and small disconnected patches with the keep-all policy selected
- **THEN** all patches SHALL survive to the output (AutoRemesher always discarded all but the largest)

### Requirement: Progress reporting and cooperative cancellation
Every pipeline entry point SHALL accept a progress sink and a cancellation token. Progress SHALL be monotonic per run, weighted by island size across stages. Cancellation SHALL be honored within 100 ms worst case at any stage, SHALL leave the input document untouched (results commit atomically at completion only), and SHALL release all worker resources.

#### Scenario: Cancel mid-parameterization
- **WHEN** cancellation is requested while an island solve is running
- **THEN** the run SHALL stop within 100 ms, return a "cancelled" status, and the document SHALL be exactly as before the run started (AutoRemesher had no cancellation at all)

#### Scenario: Monotonic progress
- **WHEN** worker threads report progress out of order
- **THEN** the merged progress value visible to the caller SHALL never decrease

### Requirement: Deterministic output
Given identical input mesh, parameters, and seed, the pipeline SHALL produce identical output meshes on the same backend and platform, including island ordering in the merged result.

#### Scenario: Reproducible remesh
- **WHEN** the same OBJ is remeshed twice with the same parameters on the same machine and backend
- **THEN** both outputs SHALL be identical

### Requirement: Seamless-UV backend selection and feature routing
The quad-cover quadrangulator SHALL resolve its seamless-UV solve through a backend selector that always yields a usable result without an external dependency, prefers the highest-quality available backend, and routes crease-heavy (CAD) meshes to the feature-aware backend. Backend order, routing thresholds, and the disabling of any backend SHALL be controllable without recompilation.

#### Scenario: Dependency-free by default
- **WHEN** the engine is built without the optional in-process reference solver and no external solver path is configured
- **THEN** quad-cover SHALL still produce a valid seamless-UV parameterization via an always-compiled native solver (quad-cover is never reported as "unavailable" for lack of an external binary), degrading to field-aligned only if the native solve itself declines an island

#### Scenario: Reference field when compiled in
- **WHEN** the engine is built with the optional in-process reference field (`-DCYBER_WITH_QUADCOVER`, the shipping preset)
- **THEN** the selector SHALL prefer that field for smooth/organic meshes, reaching median-angle and irregular-vertex quality that matches or beats QuadriFlow, with the native solver retained as a fallback

#### Scenario: Crease-heavy meshes route to the feature-aware solver
- **WHEN** a mesh's fraction of interior sharp (crease) edges exceeds the routing threshold (a CAD part such as fandisk)
- **THEN** the selector SHALL try the feature-aware native seamless solver (which marks sharp edges as hard seams and pins the feature-bounded patches) first, producing squarer quads on the CAD part than the reference field, while smooth meshes below the threshold are unaffected (their output is byte-identical to routing disabled)

