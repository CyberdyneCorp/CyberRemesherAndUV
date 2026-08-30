# manual-retopology Specification

## Purpose
Retopology by hand: the Target/EditMesh scene model, the stroke grammar for
drawing quads onto a surface, and the build, pin, symmetry, relax and
visibility tools that operate on the result. It exists for the cases automatic
remeshing cannot own — deliberate edge flow around a face, a deforming joint, a
production topology standard — and it shares the mesh kernel and snapping with
the automatic path so hand-built and generated geometry are the same kind of
thing.
## Requirements
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

### Requirement: Gradient region selection
The retopology stage SHALL compute per-vertex selection weights in [0,1] from
three region sources: a line gradient (anchor to end point, weight ramping
0→1 along the gradient and 1 beyond it, with optional angle snapping in 15°
increments), a sphere (center and radius with an easing falloff), and painted
strokes (weights accumulated under the brush, with a subtract mode).

#### Scenario: Line gradient ramps and saturates
- **WHEN** a line selection is made from anchor A to end B
- **THEN** vertices at A SHALL weigh 0, vertices at B SHALL weigh 1, vertices beyond B SHALL weigh 1, and the ramp between SHALL follow the falloff curve

#### Scenario: Painted weights accumulate
- **WHEN** overlapping paint strokes cover a region
- **THEN** weights SHALL accumulate toward 1 and a subtract stroke SHALL reduce them

### Requirement: Selection operations
Soft selections SHALL support clear, invert, expand, contract, and smoothing
by 1, 5, or 10 steps, and SHALL be savable to and loadable from named slots
persisted with the document.

#### Scenario: Smooth-by-10 softens the gradient
- **WHEN** a hard-edged painted selection is smoothed by 10
- **THEN** the weight transition SHALL widen without moving geometry

### Requirement: Weighted transform with surface glue
Translate, rotate and scale SHALL apply per-vertex scaled by selection weight,
and relax SHALL accept the weights. During and after a weighted transform the
affected vertices SHALL remain snapped to the Target surface; auto-snapping is
part of the operation and SHALL NOT require a separate pass.

#### Scenario: Taper by line gradient stays on the sculpt
- **WHEN** a line selection spans a limb and a scale transform is applied
- **THEN** the limb SHALL taper along the gradient and every affected vertex SHALL lie on the Target surface afterward

#### Scenario: Zero-weight vertices do not move
- **WHEN** any weighted transform runs
- **THEN** vertices with weight 0 SHALL be bit-identical to their pre-transform positions

### Requirement: Subdivision modes
The whole-mesh subdivide command SHALL offer two modes over ONE topology pass
(every n-gon becomes n quads around its centroid, in both): a LINEAR mode that
leaves every new vertex on the source facets, and a CATMULL-CLARK mode that
applies the smooth subdivision rules — face points at the face centroid, edge
points at the average of the edge's endpoints and its two adjacent face points,
and the vertex weighting `(F + 2R + (n-3)P) / n` — so that repeated application
converges toward the limit surface without any Target to reproject onto.

LINEAR SHALL remain the default, and its output SHALL be bit-for-bit what it was
before the smooth mode existed: callers that do not ask for smoothing SHALL NOT
observe any change. The smooth mode SHALL write vertex positions only, leaving
the topology and the propagated vertex/edge/face/corner attributes identical to
the linear mode's.

#### Scenario: Smooth subdivision recovers curvature with no Target
- **WHEN** a coarse faceted cage is subdivided repeatedly in Catmull-Clark mode with no projection target
- **THEN** the worst dihedral angle between adjacent faces SHALL fall substantially at every level, while the same mesh subdivided linearly SHALL retain its original crease angles unchanged however many levels are applied

#### Scenario: The default path is untouched
- **WHEN** the subdivide command is invoked without naming a mode
- **THEN** the result SHALL be bitwise identical to the linear subdivision the engine produced before the smooth mode was added

### Requirement: Creases and boundaries under smooth subdivision
Catmull-Clark subdivision SHALL apply the sharp (crease) rule to any edge that
is tagged as a feature edge or is not shared by exactly two faces — boundary and
non-manifold edges included. This is the same "feature or boundary" predicate the
pipeline's quad relax freezes vertices on, and the two SHALL agree: a smoothing
pass that rounded a crease off would be followed by a relax that pinned the
rounded result.

Consequently: a crease edge's edge point SHALL stay at the edge midpoint; a
vertex with exactly two incident creases SHALL follow the cubic B-spline crease
curve rule; and a vertex with three or more incident creases, or a valence-two
border vertex (the corner of an open patch), SHALL NOT move at all. A mesh with
no feature tags SHALL subdivide fully smoothly.

#### Scenario: An open patch keeps its border
- **WHEN** an open planar patch is subdivided smoothly several times
- **THEN** its bounding box, its planarity and the straightness of its border SHALL be preserved exactly, rather than the border shrinking inward and the corners rounding off

#### Scenario: An open tube's boundary rings stay put
- **WHEN** a tube open at both ends is subdivided smoothly
- **THEN** each end ring SHALL remain in its own plane at every level, rather than creeping along the axis and shortening the tube

#### Scenario: Tagged features stay sharp
- **WHEN** a cube with every edge tagged as a feature edge is subdivided smoothly
- **THEN** the result SHALL still be an exact cube: the corners SHALL NOT move and the new vertices SHALL sit on the cube's faces and edges

### Requirement: Loop subdivision
The system SHALL provide a whole-mesh densification for TRIANGLE meshes that
splits every triangle into four triangles, alongside the existing quad
subdivision. It SHALL offer two named modes that share the same topology and
differ only in vertex placement: a smooth mode using the standard Loop weights
(3/8-3/8-1/8-1/8 for interior edge points, the valence-dependent beta mask for
interior vertices), and a linear mode that is a pure 1-to-4 split leaving every
original vertex position unchanged. The mode SHALL always be chosen by the
caller and never inferred.

Boundary elements SHALL follow the boundary curve rules — midpoints for boundary
edge points and the 1/8, 3/4, 1/8 mask for boundary vertices — so that an open
mesh keeps its border rather than being pulled inward.

A face that is not a triangle SHALL be refused with a typed error naming the
offending face and its side count, leaving the caller's mesh untouched. It SHALL
NOT be triangulated implicitly; triangulation stays a separate, explicit
command.

#### Scenario: A triangle becomes four triangles
- **WHEN** a mesh of N triangles is Loop-subdivided in either mode
- **THEN** it SHALL afterwards hold 4N triangles, and no quads

#### Scenario: Linear mode changes resolution, not shape
- **WHEN** a mesh is subdivided in linear mode
- **THEN** every original vertex SHALL keep its exact position and every added vertex SHALL be an exact edge midpoint

#### Scenario: Smooth mode keeps an open mesh's border
- **WHEN** an open mesh whose boundary lies in a plane is subdivided in smooth mode
- **THEN** interior vertices SHALL move toward the limit surface and every boundary vertex SHALL remain on the boundary curve

#### Scenario: A non-triangle face is named, not silently fixed
- **WHEN** a mesh containing a quad is Loop-subdivided
- **THEN** the call SHALL fail with a typed error naming that face and its side count, and the mesh SHALL be unchanged

