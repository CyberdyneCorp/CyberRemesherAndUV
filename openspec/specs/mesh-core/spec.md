# mesh-core Specification

## Purpose
TBD - created by archiving change bootstrap-v1-platform. Update Purpose after archive.
## Requirements
### Requirement: Half-edge mesh kernel
The core SHALL provide a half-edge mesh data structure supporting triangles, quads, and n-gons with O(1) adjacency queries (vertex→outgoing half-edges, face→boundary loop, edge→adjacent faces), implemented in pure C++20 with no UI-toolkit or platform dependency.

#### Scenario: Build from indexed faces
- **WHEN** a mesh is constructed from an indexed vertex/face list containing a mix of triangles and quads
- **THEN** the kernel SHALL build a valid half-edge structure preserving face arity and vertex order, and round-trip back to the identical indexed representation

### Requirement: Structural invariants after every operator
Every mutating operator (edge collapse, edge flip, edge split, face insert/delete, vertex merge, subdivide, triangulate) SHALL leave the half-edge structure internally consistent (twin/next/prev cycles closed, no dangling references). A debug-mode validation routine SHALL verify these invariants and SHALL be exercised by property-based tests.

#### Scenario: Property test on random operator sequences
- **WHEN** a randomized sequence of collapses, flips, and splits is applied to a test mesh in a debug build
- **THEN** the invariant validator SHALL pass after every single operation, and any violation SHALL identify the offending element

### Requirement: Non-manifold representation without data loss
The kernel SHALL represent and load non-manifold input (edges shared by more than two faces, bow-tie vertices, inconsistent winding) without dropping or overwriting faces, and SHALL expose diagnostics enumerating each non-manifold element. Connected-component (island) detection SHALL account for every input face exactly once.

#### Scenario: Edge shared by three faces survives island detection
- **WHEN** island detection runs on a mesh where one edge is shared by three faces
- **THEN** all three faces SHALL be present in the resulting islands and none SHALL be silently discarded (AutoRemesher's last-writer-wins edge map lost connectivity here)

#### Scenario: Non-manifold diagnostics
- **WHEN** a non-manifold mesh is loaded
- **THEN** the kernel SHALL report the counts and element indices of non-manifold edges and vertices through the diagnostics API

### Requirement: Feature edge tagging
The kernel SHALL tag feature (sharp) edges by dihedral-angle threshold and SHALL expose the tags to all pipeline stages; tags SHALL survive mesh copies and remap consistently through operators that preserve the edge.

#### Scenario: Threshold tagging
- **WHEN** feature detection runs with a 90° threshold on a cube
- **THEN** exactly the 12 cube edges SHALL be tagged as features

### Requirement: Spatial acceleration structure
The kernel SHALL provide a BVH over triangles supporting closest-point queries and ray casting, with a documented interface consumable by the acceleration backends (surface projection, snapping, baking).

#### Scenario: Closest-point projection
- **WHEN** a query point near a surface is projected via the BVH
- **THEN** the returned point SHALL lie on the mesh surface and be the true closest point within documented floating-point tolerance

### Requirement: Generic element attributes
The kernel SHALL support named, typed per-vertex, per-edge, per-face, and per-corner attributes (at minimum: float, int, vec2, vec3, vec4/color), which mutating operators SHALL propagate or interpolate according to a documented policy.

#### Scenario: Vertex colors survive subdivision
- **WHEN** a mesh with a per-vertex color attribute is subdivided
- **THEN** new vertices SHALL carry interpolated colors and existing vertices SHALL keep theirs

### Requirement: Deterministic operations
Given identical input, parameters, and random seed, every kernel operation SHALL produce bitwise-identical results across runs on the same backend and platform.

#### Scenario: Repeated run equality
- **WHEN** the same operator sequence runs twice on the same machine
- **THEN** the resulting meshes SHALL be bitwise identical

