# uv-editing Specification

## Purpose
The UV half of the workflow, from seams to a packed layout: marking seams on
the 3D mesh, gesture unwrap, manipulating UVs on the model and in 2D, packing,
distortion visualization, auto-routed seam paths, and a one-call automatic
atlas. It exists to cover both ends of the same job — the automatic atlas for
"just give me UVs", the seam and path tools for the cuts an artist wants placed
deliberately — over one seam model, so a routed seam and a hand-marked one are
indistinguishable to everything downstream.
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

#### Scenario: The packing readout reports coverage the caller can act on
- **WHEN** the automatic atlas reports how much of the UV square it filled
- **THEN** the packed-area readout SHALL be the fraction the chart GEOMETRY covers (the summed UV face areas), not the fraction covered by the charts' bounding boxes, and the bounding-box fraction SHALL be reported separately as a distinct packer-tightness readout
- **AND** a chart that ends up degenerate (no UV area, so invisible in the packed layout) SHALL NOT be counted in the chart count; it SHALL be reported as a dropped chart, so chart count plus dropped charts equals the number of islands the seams produced

#### Scenario: Chart merging trades seams against distortion under a bound
- **WHEN** chart merging is enabled
- **THEN** the atlas SHALL first merge adjacent charts whose union stays within one normal cone (fewer seams with no rise in distortion), then optionally merge further while the combined conformal-plus-area distortion of the LSCM-unwrapped union stays at or below a caller-set cap (folding developable regions together to cut the chart count), and SHALL NOT merge charts that would fold or exceed the cap

#### Scenario: The merge pass is bounded work, not unbounded work
- **WHEN** the distortion-capped merge pass drives its fixpoint over adjacent chart pairs
- **THEN** it SHALL NOT re-run the trial unwrap for a pair it already rejected while neither of the two charts has changed, so the cost of the pass tracks the merges it actually performs rather than the number of fixpoint rounds

#### Scenario: A long unwrap is cancellable and observable
- **WHEN** a caller supplies a cancellation token (and optionally a progress sink) to the automatic atlas and cancels it
- **THEN** the atlas SHALL abort within one trial unwrap, SHALL report the run as cancelled, and SHALL leave the mesh exactly as it was — no partial atlas, no UV attribute the mesh did not already carry — and the export-bundle writer SHALL pass its own token into the unwrap it performs on a low-poly that carries no UVs

#### Scenario: Charts are re-oriented before packing
- **WHEN** chart re-orientation is enabled
- **THEN** each chart SHALL be rotated to its minimum-area bounding rectangle before packing (a similarity that leaves conformal distortion and flip state unchanged), so the packer wastes less space and texel density rises

#### Scenario: Conformal quality is preserved
- **WHEN** the automatic atlas unwraps normal-coherent charts on a remeshed quad mesh
- **THEN** the maximum per-chart conformal (angle) distortion SHALL stay low (angle-preserving charts) with no flipped charts, and the result SHALL be exposed for benchmarking against a reference unwrapper (e.g. xatlas) on chart count, distortion, and coverage

### Requirement: Auto-routed seam paths
In addition to stroke-over-edges seam editing, the UV stage SHALL support
routed seam paths: the user places an ordered sequence of waypoints on the
EditMesh, and between consecutive waypoints the engine SHALL route a
least-cost path along mesh edges, with an edge-cost model that prefers
feature-tagged and high-curvature edges over flat-region crossings. A crease
SHALL be recognised by the MAGNITUDE of its dihedral, so concave (valley) and
convex (ridge) feature lines are both routable, each with its own tunable
weight exposed through the C ABI and the language bindings.

#### Scenario: Route follows the groove
- **WHEN** two waypoints are placed on either side of a concave feature line
- **THEN** the routed path SHALL follow edges along the feature rather than the geodesic shortcut across flat surface

#### Scenario: Route follows a convex ridge
- **WHEN** two waypoints are placed at either end of a convex feature line, with a flat but slightly shorter crossing available
- **THEN** the routed path SHALL follow the ridge rather than the flat shortcut, and setting the convex weight to the flat weight SHALL restore the plain geodesic

### Requirement: Pending paths are editable until commit
Every placed waypoint SHALL be repositionable and deletable before commit,
with the affected route segments re-computed on each edit. Commit SHALL
convert the routed path into seam edges under the existing seam model (so
gesture unwrap and sew behave as with any other seam). After commit a resume
marker SHALL allow continuing the path from the last point; explicitly
dropping the resume marker SHALL NOT modify any committed seam.

#### Scenario: Fix a deviating waypoint
- **WHEN** an intermediate waypoint is dragged to a better position before commit
- **THEN** only the adjacent route segments SHALL re-route and the rest of the pending path SHALL be unchanged

#### Scenario: Commit then resume
- **WHEN** a path is committed and a new waypoint is placed with the resume marker active
- **THEN** the new route SHALL start from the last committed point, and dropping the marker instead SHALL leave all committed seams intact

### Requirement: Unwrap along caller-supplied seams
The automatic atlas SHALL accept a caller-supplied seam set and use it as the
cut instead of computing its own. When one is supplied the chart-growth and
chart-merge stages SHALL NOT run, because the seams already define the charts;
every later stage — island computation, conformal unwrap with the planar
fallback, chart re-orientation, packing and the reported metrics — SHALL be the
same code path the automatic atlas uses, so a seam-driven unwrap is reported in
the same terms as an automatic one.

A run given no seam set SHALL be unchanged, byte for byte, from the automatic
atlas as it behaves today.

#### Scenario: Marked seams define the charts
- **WHEN** a caller marks a seam set that cuts the mesh into a known number of islands and unwraps along it
- **THEN** the result SHALL report that many charts, and the charts SHALL be split exactly at the marked edges rather than at edges the engine chose

#### Scenario: An empty seam set is one chart
- **WHEN** a caller unwraps along a seam set with no edges marked on a closed mesh
- **THEN** the whole mesh SHALL unwrap as a single chart rather than being auto-seamed

#### Scenario: Chart growth options are inert
- **WHEN** a caller supplies a seam set together with chart-angle or chart-merge options
- **THEN** the charts SHALL follow the seams alone, and the result SHALL be identical to the same call with those options at any other value

### Requirement: Sewing islands back together is reachable
Sewing SHALL be reachable from the C ABI and the language bindings, not only
from C++, so that the seam set a binding consumer edits can be both cut and
sewn. Sewing merges two islands by removing the seam edges that separate them
and welding the corners across each removed edge.

#### Scenario: Sewing removes the cut
- **WHEN** a caller sews an edge that a previous unwrap had cut
- **THEN** that edge SHALL no longer be a seam, and a subsequent unwrap SHALL treat the two islands it separated as one

