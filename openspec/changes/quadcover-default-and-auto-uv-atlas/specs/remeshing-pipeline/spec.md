# remeshing-pipeline (delta)

## MODIFIED Requirements

### Requirement: Quad extraction and pure-quad option
Extraction SHALL trace integer isolines into a connection graph, simplify it (dissolve valence-2 vertices, collapse degenerate edges/cycles, prune dangling chains), and enumerate faces. Default output is quad-dominant. When the pure-quad option is enabled, a post-pass SHALL eliminate non-quads where topologically achievable (splitting/merging into quads) and the report SHALL state the exact residual non-quad count.

#### Scenario: Non-quad accounting
- **WHEN** any remesh completes
- **THEN** the report SHALL state quad count, non-quad count by arity, vertex count, and singularity count

#### Scenario: QuadCover seamless-UV extractor is the default
- **WHEN** a remesh runs without an explicit quadMethod
- **THEN** the pipeline SHALL use the `quad-cover` quadrangulator by default (CLI, C ABI, and bindings), which parameterizes each island with a seamless-UV solve and extracts integer isolines into quads; on the organic test corpus it SHALL match or beat QuadriFlow on median quad angle AND irregular-vertex count when the in-process reference field is compiled in (see the seamless-UV backend requirement), and SHALL degrade to the field-aligned quadrangulator (never silently to a corrupt mesh) when no seamless-UV solver can produce a valid result for an island

#### Scenario: Field-aligned pairing maximizes quad-dominance
- **WHEN** the field-aligned quadrangulator pairs triangles into quads
- **THEN** it SHALL compute a maximum matching on the triangle-adjacency graph (a weighted-greedy seed that prefers field-diagonal, high-quality merges, followed by augmenting-path improvement that rescues triangles a greedy pass would strand), so quad-dominance is maximized (~95%+ on clean input) rather than left at the ~76–84% a greedy maximal matching produces, while surviving quad edges still follow the cross field. The field-aligned quadrangulator is the always-available fallback; it is selected explicitly via quadMethod and is the automatic degrade target when the default quad-cover path declines an island.

#### Scenario: Position-field quadrangulator option
- **WHEN** quadMethod selects the position-field (Instant-Meshes-style) extractor
- **THEN** the pipeline SHALL build a per-vertex 4-RoSy orientation field and a lattice position field, collapse mesh vertices into lattice cells (with post-collapse edge recovery so the collapsed graph reaches near-grid valence), and extract faces by a rotation-system walk with greedy-cut hole filling, yielding more uniform, field-aligned edge flow (edge-length CV competitive with field-based references such as QuadriFlow) with fewer, better-placed singularities; residual singularity triangles SHALL be resolved by the pure-quad path. It is offered as an opt-in alternative, never copying GPL sources.

#### Scenario: Pure-quad post-pass
- **WHEN** the pure-quad option is enabled
- **THEN** the output SHALL contain only quads, or the report SHALL explicitly list the residual non-quads that could not be resolved

#### Scenario: Pure-quad element quality
- **WHEN** the pure-quad option is enabled
- **THEN** the construction (quad-dominant remesh at a fraction of the target density, then one linear subdivision) SHALL be followed by a tangential-relaxation de-slivering pass — interior vertices slide toward their 1-ring centroid within the tangent plane, re-projecting onto the source surface each iteration, with feature (sharp-crease) and boundary vertices held fixed — so the quads are well shaped (no slivers or collapsed edges) rather than merely all four-sided, while the silhouette follows the source curvature. For the position-field and quad-cover quadrangulators the relaxation SHALL instead fit each quad to a common-sized square (shape matching), regularising 90-degree corners and equal edge lengths together for uniformity competitive with field-based references. Charts produced on a uniform (integer-grid or seamless) base SHALL take a longer projected base-relax before subdivision, lifting median angle at no edge-length-CV cost.

## ADDED Requirements

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
