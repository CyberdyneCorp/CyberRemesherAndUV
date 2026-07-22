# uv-editing Specification

## Purpose
TBD - created by archiving change bootstrap-v1-platform. Update Purpose after archive.
## Requirements
### Requirement: Seam editing on the 3D mesh
In the UV stage, Pencil strokes over EditMesh edges SHALL create, extend, or delete seams (partial strokes allowed); Erase SHALL delete seams under the stroke; fully cut islands SHALL be visually distinguished (color change). Drawing over an existing seam SHALL sew it back.

#### Scenario: Draw a seam
- **WHEN** the user draws along a path of edges in UV mode
- **THEN** those edges SHALL become seam edges and the affected region SHALL indicate its cut state

### Requirement: Gesture unwrap
Drawing an X (or square) over an island SHALL unwrap/unfold that island using an angle-based or equivalent conformal method with automatic corner pinning; re-running the gesture after seam changes SHALL re-unfold. Automatic island symmetrization SHALL be available for islands with detectable symmetric topology.

#### Scenario: X to unwrap
- **WHEN** an X is drawn over a fully seamed island
- **THEN** the island SHALL unfold with low angular distortion and appear in the 2D layout view

### Requirement: On-model UV manipulation (UV3D)
The system SHALL allow island UV transforms directly on the 3D model: Tweak with multitouch SHALL translate, rotate, and scale an island's UVs on the surface (usable with a loaded tileable preview texture); Relax SHALL smooth UVs of the island under the cursor with corners auto-pinned; Clone SHALL copy UVs between islands of matching topology; UV pins SHALL constrain unfolds.

#### Scenario: Texture-aligned adjustment
- **WHEN** a tileable texture is displayed and the user two-finger-transforms an island on the model
- **THEN** the texture mapping SHALL update live under the gesture

### Requirement: 2D layout tools (UV2D)
The 2D view SHALL provide: island transform via Tweak (stroke on upper half rotates, lower half scales), double-tap distribution of overlapping islands, grid straightening of grid-topology islands into axis-aligned grids, partial symmetrization around vertices under the cursor, vertex-level UV tweak/relax/move, and merge/stitch of islands by drawing one over another with boundary fitting.

#### Scenario: Grid straightening
- **WHEN** grid straightening is applied to an island with regular grid topology
- **THEN** the island's UVs SHALL become an axis-aligned rectangular grid

### Requirement: Packing
The system SHALL pack islands automatically with correct output scale, and support manual packing aided by snapping modes (adjustable grid, pixel centers, pixel corners, symmetry lines), texel-density and vertex-count readouts, and detection helpers for overlapping islands. Packing SHALL handle at least 1 000 islands / 100 000 UV vertices without failure.

#### Scenario: Automatic pack
- **WHEN** automatic packing runs on unwrapped islands
- **THEN** islands SHALL be placed within the 0–1 UV square without overlaps at the requested margin, preserving relative texel density unless told otherwise

### Requirement: Distortion visualization
The UV stage SHALL provide a checker preview and a quantitative distortion overlay (stretch/shear coloring) in both 3D and 2D views, plus flipped-island indication (orientation arrows or equivalent).

#### Scenario: Flipped island is visible
- **WHEN** an island is mirrored in UV space
- **THEN** its flipped state SHALL be visually distinguishable from correctly oriented islands

### Requirement: Automatic UV atlas
In addition to the interactive editor (hand-drawn seams, gesture unwrap, manual/automatic packing), the UV capability SHALL provide a non-interactive automatic atlas: given a mesh with no UVs and no hand-drawn seams, it SHALL seam the mesh into charts, unwrap each chart, pack them into the unit UV square, and write the per-corner UV attribute, reporting chart count and distortion so callers can gate on quality. The pipeline SHALL be deterministic and each stage SHALL be independently controllable.

#### Scenario: Mesh in, packed atlas out
- **WHEN** the automatic atlas runs on a mesh that carries no UVs
- **THEN** it SHALL grow charts by normal coherence, LSCM-unwrap each chart (falling back to a planar projection for a chart LSCM rejects), and pack the charts into the 0–1 UV square without overlaps, leaving every corner with a UV coordinate and returning chart count, maximum and RMS conformal (angle) distortion, flipped-chart count, and packed-area / texel-density readouts

#### Scenario: Chart merging trades seams against distortion under a bound
- **WHEN** chart merging is enabled
- **THEN** the atlas SHALL first merge adjacent charts whose union stays within one normal cone (fewer seams with no rise in distortion), then optionally merge further while the combined conformal-plus-area distortion of the LSCM-unwrapped union stays at or below a caller-set cap (folding developable regions together to cut the chart count), and SHALL NOT merge charts that would fold or exceed the cap

#### Scenario: Charts are re-oriented before packing
- **WHEN** chart re-orientation is enabled
- **THEN** each chart SHALL be rotated to its minimum-area bounding rectangle before packing (a similarity that leaves conformal distortion and flip state unchanged), so the packer wastes less space and texel density rises

#### Scenario: Conformal quality is preserved
- **WHEN** the automatic atlas unwraps normal-coherent charts on a remeshed quad mesh
- **THEN** the maximum per-chart conformal (angle) distortion SHALL stay low (angle-preserving charts) with no flipped charts, and the result SHALL be exposed for benchmarking against a reference unwrapper (e.g. xatlas) on chart count, distortion, and coverage

