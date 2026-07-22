# manual-retopology — Stroke-Based Retopology

## ADDED Requirements

### Requirement: Target and EditMesh scene model
A retopology document SHALL contain one read-only **Target** (the imported high-poly reference, with vertex colors when present) and one **EditMesh** (the low-poly mesh under construction). EditMesh vertices SHALL continuously snap to the Target surface as they are created or moved; snapping SHALL use the accelerated closest-point path. An optional vertex-snap modifier SHALL snap EditMesh vertices to Target *vertices* instead of the surface. A 2D image MAY be loaded as a flat snapping target.

#### Scenario: Moved vertex reprojects
- **WHEN** a user drags an EditMesh vertex with Tweak
- **THEN** on release the vertex SHALL lie on the Target surface at the closest point to the drop position

#### Scenario: Vertex-snap modifier
- **WHEN** the extra-finger snap modifier is held during a Tweak drag
- **THEN** the vertex SHALL snap to the nearest Target vertex rather than the surface

### Requirement: Core actions coherent across stages
The toolset SHALL be organized as five core actions — **Pencil** (stroke gestures), **Relax**, **Move**, **Tweak**, **Erase** — whose semantics stay coherent in every stage (retopo, UV, bake): Relax always smooths the active data (topology / UVs / cage), Move always drags with surface-geodesic brush falloff (disconnected components unaffected), Tweak always manipulates single elements, Erase always removes under the stroke.

#### Scenario: Relax pins grid corners
- **WHEN** Relax is applied over a regular quad grid region
- **THEN** grid corner vertices SHALL be automatically pinned so the grid pattern is preserved

#### Scenario: Erase pressure scales coarseness
- **WHEN** Erase strokes are drawn with increasing stylus pressure
- **THEN** the removal footprint SHALL grow with pressure

### Requirement: Pencil stroke grammar
The Pencil action SHALL recognize at minimum these gestures on the Target/EditMesh: closed quad/tri shape → create face; stroke from existing edges/vertices → extend topology; drag across faces → insert edge loop; scribble/X over geometry → delete it; straight line between two vertices → merge/collapse pair (two adjacent triangles → quad); line between two boundary loops with equal vertex count → bridge; circle over an edge → rotate edge (redirect loop flow); closed loop around a cylindrical region → extrude cylinder; double-tap on a vertex/edge-loop → enter Tweak (move vertex / slide loop). Recognition SHALL be tolerant of imperfect strokes.

#### Scenario: Closed stroke creates a quad
- **WHEN** a roughly four-cornered closed stroke is drawn on the Target surface
- **THEN** a new quad face SHALL be created with its vertices snapped to the surface

#### Scenario: X-stroke deletes
- **WHEN** an X is drawn over EditMesh faces
- **THEN** the faces under the X SHALL be deleted

#### Scenario: Unrecognized stroke gives feedback
- **WHEN** a stroke matches no gesture
- **THEN** the system SHALL show non-intrusive visual feedback that nothing was recognized (CozyBlanket silently ignored these, a top usability complaint)

### Requirement: Advanced build tools
The system SHALL provide these tools (available via a customizable tool gallery): **BuildQuad** / **BuildTri** (drag from boundary elements to extrude single faces with automatic vertex merging), **DrawStrip** (paint a quad strip following the stroke, matching source quad size), **ExtendBoundary** (select boundary edges by stroke or press-and-hold, then extrude quad strips by camera movement — single step, repeated, or continuous — plus grid fill and triangle-fan fill of boundary regions with controllable orientation), **PatchClone** (stroke-select a face patch, then stamp copies elsewhere, with flip), **TransformVertices** (lasso-select vertices, then move/rotate/scale them by camera movement, reporting per-vertex snap success), **PathDistribute** (redistribute vertices along the path between stroke endpoints), **SurfaceCut** (knife-cut new edges across faces), and **LoopInfo** (inspect vertex/edge count, boundary length, and snap state of the loop under the cursor).

#### Scenario: Camera-driven boundary extrusion
- **WHEN** boundary edges are selected via ExtendBoundary and the user orbits the camera
- **THEN** quad strips SHALL extrude from the selected boundary following the camera motion

#### Scenario: Grid fill
- **WHEN** a closed boundary region is grid-filled
- **THEN** the region SHALL fill with a regular quad grid whose orientation is adjustable via anchor handles

### Requirement: Pinning
Vertices SHALL be pinnable individually and per edge loop; pinned vertices SHALL be visually marked and immune to Relax and Move (but movable by explicit Tweak). Pins SHALL be clearable en masse.

#### Scenario: Pinned vertex resists Relax
- **WHEN** Relax is painted over a region containing pinned vertices
- **THEN** pinned vertices SHALL NOT move

### Requirement: Symmetry
The system SHALL support mirror symmetry across a plane: edits on one side replicate to the other, vertices near the plane snap onto it, the working side SHALL be switchable, and an "apply symmetry" command SHALL bake the mirror into real geometry. Symmetry state SHALL be visible (plane rim indicator) and queryable via the network bridge.

#### Scenario: Mirrored face creation
- **WHEN** symmetry is on and a face is drawn on the left side
- **THEN** the mirrored face SHALL appear on the right side with plane-adjacent vertices snapped to the plane

### Requirement: Auto Relax mode
When Auto Relax is enabled, every topology-modifying operation SHALL be followed by an automatic local relax of surrounding topology to maintain even quad distribution.

#### Scenario: Even quads after build
- **WHEN** Auto Relax is on and a quad strip is extruded
- **THEN** the new and neighboring vertices SHALL be relaxed toward uniform spacing automatically

### Requirement: Visibility control
The system SHALL support stroke-lasso hiding and showing of Target and EditMesh regions, visibility inversion, show-all, per-component visibility toggles, configurable EditMesh occlusion depth against the Target, EditMesh opacity, and back-face exclusion of selections.

#### Scenario: Lasso hide
- **WHEN** a closed lasso is drawn starting from empty space
- **THEN** the faces inside SHALL be hidden until shown again

### Requirement: Whole-mesh commands
The system SHALL provide EditMesh-level commands: snap all vertices to Target, relax all, subdivide, triangulate, clear loop tags, clear pins, and landmark loop color-tagging (drawing along an existing loop marks it with a persistent color).

#### Scenario: Landmark loop tagging
- **WHEN** the user draws along an existing edge loop in tagging mode
- **THEN** that loop SHALL be marked with a distinct color that persists until cleared

### Requirement: Interactive performance floor
Retopology interaction (stroke recognition, snapping, local relax) SHALL remain interactive (< 33 ms response) with a Target of at least 5 million triangles and an EditMesh of at least 100 000 vertices on reference hardware (Apple M1 iPad Pro class; mid-range desktop GPU).

#### Scenario: Large sculpt stays interactive
- **WHEN** a 5 M-triangle Target is loaded on reference hardware
- **THEN** drawing and tweaking SHALL respond within 33 ms per interaction
