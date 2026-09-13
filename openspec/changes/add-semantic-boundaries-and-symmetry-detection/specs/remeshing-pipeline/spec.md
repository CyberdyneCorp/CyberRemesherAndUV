# remeshing-pipeline — semantic boundaries and symmetry detection

## ADDED Requirements

### Requirement: Semantic boundaries are preserved as edge loops

The pipeline SHALL accept per-face group / material ids through the typed bulk
indexed-mesh descriptor: a face-domain `int32` column named `group_id` or
`material_id`. It SHALL treat the edges where adjacent faces disagree as feature
edges — pinned by the seamless solve, present in the topology layout, and
honoured by the sizing field — so a material boundary comes back as an edge loop
rather than being crossed by quads. OBJ `g` / `usemtl` records are not imported
as semantic ids; callers needing that identity SHALL use the explicit typed
descriptor rather than relying on an implicit file-format mapping.

#### Scenario: A material boundary survives remeshing

- **WHEN** a mesh carrying two materials is remeshed
- **THEN** the boundary between them SHALL appear in the output as a continuous
  edge loop, and SHALL be reported in the layout as arcs

#### Scenario: Semantic tags are never demoted

- **WHEN** a semantic boundary edge lies on a surface the dihedral re-tag would
  classify as smooth
- **THEN** the edge SHALL remain tagged as a feature, because semantic tagging
  is applied after the dihedral re-tag

### Requirement: Symmetry is detected and reported before it is applied

The pipeline SHALL be able to detect a symmetry plane and report it WITHOUT
applying it. Vertex matching SHALL be nearest-within-tolerance; quantizing
positions to a tolerance grid and comparing keys SHALL NOT be used, because it
both collides distinct vertices and misses partners across a cell boundary.
Detection SHALL NOT select the symmetry axis automatically until its threshold
has been calibrated on the corpus.

#### Scenario: A symmetric model reports its plane

- **WHEN** a symmetric model is analysed
- **THEN** the run report SHALL name the detected plane, and the output SHALL be
  unchanged by the detection

#### Scenario: A nearly-symmetric model is not silently mirrored

- **WHEN** a model is symmetric only within a loose tolerance
- **THEN** detection SHALL NOT apply symmetry, because applying it would be a
  silent lossy edit
