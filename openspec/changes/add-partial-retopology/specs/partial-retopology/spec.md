## Purpose

Partial retopology lets a host replace selected topology while retaining a
completed exterior and enough correspondence to update dependent annotations.

## ADDED Requirements

### Requirement: Selected region has explicit frozen-border semantics

The system SHALL accept a non-empty set of source face IDs as a selected
region. In exact frozen-border mode, it SHALL retain every boundary vertex's
identity, position and cyclic order, and SHALL leave all exterior faces,
exterior vertices and their connectivity unchanged.

#### Scenario: Interior patch is retopologised

- **WHEN** a manifold connected face selection has one simple interior boundary
- **THEN** the returned mesh SHALL retain the selected boundary vertices and
  every exterior face exactly while replacing only selected-region faces

#### Scenario: Invalid boundary is rejected before mutation

- **WHEN** a selection has a non-manifold, branching or self-touching boundary
- **THEN** the operation SHALL return a rejected result with a reason and the
  source mesh SHALL remain unchanged

### Requirement: Feasibility and topology policy are explicit

The operation SHALL report whether the requested boundary and output policy are
feasible before committing. A request for all-quad output that cannot satisfy
its boundary parity SHALL be rejected or report its allowed residual polygon
types according to an explicit policy; it SHALL never silently alter the frozen
border to make the request fit.

#### Scenario: Odd boundary all-quad request

- **WHEN** an exact frozen-border selection has boundary parity incompatible
  with all-quad filling and residual polygons are forbidden
- **THEN** the result SHALL be rejected with a parity reason and no exterior
  topology or boundary identity changes

### Requirement: Result contains source correspondence

The operation SHALL return identity correspondence for retained source
vertices, and for each newly created output vertex SHALL return the closest
source primitive, barycentric coordinates, distance and confidence. It SHALL
not transfer correspondence across disconnected components or declared
material/group barriers.

#### Scenario: New vertex on an analytic source patch

- **WHEN** a selected planar or curved patch is retopologised
- **THEN** every newly created vertex correspondence SHALL identify a source
  primitive and barycentric coordinates that reconstruct a point within the
  reported distance

### Requirement: Attribute transfer is never implicit

The operation SHALL transfer continuous vertex data by interpolation and
categorical face group/material IDs by an explicit categorical rule. It SHALL
preserve discontinuous corner data only where the seam mapping is known; every
unsupported attribute SHALL appear in the result report rather than being
silently discarded.

#### Scenario: UV seam cannot be transferred

- **WHEN** a selected region includes a corner UV seam without a one-to-one
  corner mapping
- **THEN** the result SHALL list that attribute as unsupported and SHALL NOT
  assign a guessed UV value across the seam
