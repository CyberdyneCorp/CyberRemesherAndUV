# remeshing-pipeline — Automatic Quad Remeshing

## ADDED Requirements

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

#### Scenario: Pure-quad post-pass
- **WHEN** the pure-quad option is enabled
- **THEN** the output SHALL contain only quads, or the report SHALL explicitly list the residual non-quads that could not be resolved

#### Scenario: Pure-quad element quality
- **WHEN** the pure-quad option is enabled
- **THEN** the construction (quad-dominant remesh at a fraction of the target density, then one linear subdivision) SHALL be followed by a tangential-relaxation de-slivering pass — interior vertices slide toward their 1-ring centroid within the tangent plane, re-projecting onto the source surface each iteration, with feature (sharp-crease) and boundary vertices held fixed — so the quads are well shaped (no slivers or collapsed edges) rather than merely all four-sided, while the silhouette follows the source curvature

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
