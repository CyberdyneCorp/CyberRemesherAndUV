# remeshing-pipeline — ZRemesher-class automatic retopology

## ADDED Requirements

### Requirement: Explicit topology layout

The seamless path SHALL build an explicit topology layout — nodes
(singularities, boundary and feature corners, T-junctions, guide anchors,
symmetry anchors), arcs between them, and the patches those arcs bound — as a
first-class intermediate artifact, separate from the quantizer that consumes
it. The layout SHALL be reusable by singularity scoring, guides, symmetry,
semantic groups, extraction, quality scoring and debug export. Building the
layout SHALL NOT change the quantized result: for every input that quantizes
successfully today, the output SHALL be byte-identical.

#### Scenario: Layout is built before quantization

- **WHEN** an island is quantized through the global integer path
- **THEN** the run SHALL first produce a layout of nodes, arcs and patches,
  and the quantizer SHALL consume that layout rather than re-deriving it

#### Scenario: Refactor is byte-exact

- **WHEN** a mesh that quantizes successfully is remeshed before and after the
  layout is introduced, with identical parameters
- **THEN** both runs SHALL produce byte-identical output meshes

### Requirement: Layout validation and debug export

Every built layout SHALL be validated against explicit combinatorial
invariants: arcs have valid endpoints, every arc sample lies on a live source
face, no sample is non-finite, internal arcs bound exactly two patches and
boundary arcs exactly one, patch walks close without illegally repeating an
arc, feature arcs stay on feature chains, and symmetry arcs stay on the
symmetry plane. Validation SHALL be deterministic and SHALL report the first
violated invariant. The layout SHALL be exportable for inspection as a
machine-readable report and as a polyline mesh.

#### Scenario: Invalid layout is reported, not used silently

- **WHEN** a built layout violates an invariant
- **THEN** the layout SHALL be marked invalid, the violated invariant SHALL be
  named in the run report, and the pipeline SHALL fall back rather than
  quantize against the broken layout

#### Scenario: Layout is inspectable

- **WHEN** layout export is requested for a solve
- **THEN** the engine SHALL write the layout's nodes, arcs, patches and stats
  in a machine-readable form and a polyline mesh, both reproducible byte-for-
  byte across runs on the same input

### Requirement: Fold-robust layout tracing

Layout tracing SHALL be robust to foldovers in the relaxed parameterization: a
locally inverted UV triangle MAY degrade the geometric accuracy of an arc's
samples but SHALL NOT corrupt the layout graph. Tracing decisions SHALL be
combinatorial (quarter-frame transport and stable ordering keys) rather than
derived from the sign of a UV triangle area, and exact-vertex crossings SHALL
be resolved by a deterministic rule.

#### Scenario: Folded relaxed UV still yields a valid layout

- **WHEN** the relaxed parameterization of an organic mesh contains local
  foldovers
- **THEN** the layout SHALL remain combinatorially valid and the global
  quantizer SHALL run, instead of falling back to per-translation rounding

#### Scenario: Negative-index cone traces

- **WHEN** the field contains a negative-index cone
- **THEN** every separatrix launched from that cone SHALL terminate at a layout
  node and the surrounding patches SHALL close

### Requirement: Singularity placement optimization

The pipeline SHALL support optimizing where extraordinary vertices live,
scoring each singularity by a weighted cost over curvature, feature proximity,
guide proximity, thin-feature risk and boundary proximity. Relocation SHALL be
deterministic, SHALL preserve the total cross-field index exactly, and SHALL be
accepted only when it strictly lowers the weighted cost without introducing
topological defects.

#### Scenario: Relocation conserves index

- **WHEN** singularity optimization relocates any cone
- **THEN** the sum of cross-field indices SHALL be unchanged and the layout
  SHALL still validate

#### Scenario: No illegal move

- **WHEN** a candidate relocation would place a cone on a locked feature
  corner, a symmetry anchor or a boundary that forbids it
- **THEN** the move SHALL be rejected and the original placement kept

### Requirement: Local feature size is preserved

The pipeline SHALL detect thin plates and tubes and SHALL keep enough quad
rings across a thin region for it to remain non-degenerate, refusing sizing
that would collapse a thin feature to a single edge chain.

#### Scenario: Thin plate keeps its rings

- **WHEN** a mesh containing a plate thinner than the target edge length is
  remeshed
- **THEN** the plate SHALL retain at least the minimum quad rings across its
  thickness and its two sides SHALL NOT be bridged into one

### Requirement: Exact topology symmetry

When symmetry about a named axis is requested, the result SHALL be symmetric in
CONNECTIVITY, not only in vertex position: one half SHALL be solved and its
topology mirrored, with a deterministic rule for elements on the centerline.

#### Scenario: Mirrored topology is exact

- **WHEN** a mesh is remeshed with forced symmetry about an axis
- **THEN** mirroring the output's connectivity across that plane SHALL
  reproduce the output exactly, and vertices on the plane SHALL remain on it

### Requirement: Quality-driven candidate selection

The pipeline SHALL support a quality mode that solves more than one candidate
and selects between them by a single published score covering geometry, quad
shape, topology, flow, guide adherence, features and symmetry. Selection SHALL
be deterministic, with a stable tie-break, and the selected candidate SHALL be
named in the run report.

#### Scenario: Best mode never picks a worse candidate

- **WHEN** the quality mode solves two candidates
- **THEN** the selected candidate's score SHALL be greater than or equal to
  every other candidate's score, and the report SHALL name it

#### Scenario: Selection is stable

- **WHEN** two candidates score equal within tolerance
- **THEN** the same candidate SHALL be selected on every run of the same input

## MODIFIED Requirements

### Requirement: Flow guides constrain the cross field

Flow guides SHALL carry an explicit mode. In ORIENTATION mode a guide biases
the cross field only, exactly as today, and its behavior SHALL remain
byte-compatible with the shipped flow-guide path. In TOPOLOGY mode a guide
SHALL additionally be projected to a deterministic surface path and inserted
into the topology layout as a first-class arc, so the extracted quads follow
it as an actual edge chain. Closed topology guides SHALL produce a continuous
edge loop. Guides that cannot be honored SHALL be reported, never silently
dropped.

#### Scenario: Loops follow a guide

- **WHEN** a remesh runs with a guide drawn across a limb junction
- **THEN** extracted edge loops within the guide's influence radius SHALL follow the guide's direction within the documented angular tolerance

#### Scenario: No guides, no change

- **WHEN** a remesh runs with an empty guide set
- **THEN** the output SHALL be byte-identical to the same run without the guide inputs

#### Scenario: Orientation mode is unchanged

- **WHEN** a guide is supplied in orientation mode
- **THEN** the output SHALL be byte-identical to the same run before topology
  guides existed

#### Scenario: Closed topology guide becomes a loop

- **WHEN** a closed guide is drawn around a sphere in topology mode
- **THEN** the output SHALL contain a continuous quad edge loop following that
  guide, and the run report SHALL record the achieved adherence

#### Scenario: Conflicting hard guide is reported

- **WHEN** a topology guide conflicts with a pinned crease
- **THEN** the conflict SHALL be reported with both constraints named, and the
  documented precedence SHALL decide the outcome

### Requirement: Painted density scales local sizing

Painted density SHALL be one input to a single unified sizing field h(x) that
also fuses curvature, feature proximity and local thickness into the per-face
target spacing consumed by the parameterization, the quantizer, the position
field and the final optimization. When the unified sizing field is disabled,
sizing SHALL behave exactly as before and output SHALL be byte-identical.

#### Scenario: Painted region densifies

- **WHEN** a remesh runs with density 4.0 painted on a region and 1.0 elsewhere
- **THEN** quads inside the painted region SHALL be smaller than outside by approximately the documented sizing relation, and total quad count SHALL respect the requested target within its guarantee

#### Scenario: A density of 1.0 everywhere is a no-op

- **WHEN** a remesh runs with a density of 1.0 at every vertex (or every face)
- **THEN** validation SHALL drop the neutral density and report it as a non-fatal issue, and the output SHALL be byte-identical to the same run with no density supplied — on every backend, including the default quad-cover, whose route selection would otherwise change merely because guidance is present

#### Scenario: One sizing decision, many inputs

- **WHEN** a mesh with painted density, high curvature regions and thin
  features is remeshed with the unified sizing field enabled
- **THEN** all four influences SHALL be visible in one target-spacing field and
  the run report SHALL record each contribution's weight

#### Scenario: Disabled sizing field is byte-exact

- **WHEN** the unified sizing field is disabled
- **THEN** the output SHALL be byte-identical to the shipped sizing path
