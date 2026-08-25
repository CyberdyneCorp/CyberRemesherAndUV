# manual-retopology — triangle-to-triangle densification

Delta for `add-loop-subdivision`.

## ADDED Requirements

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
