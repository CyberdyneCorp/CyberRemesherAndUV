# engine-bindings — isotropic remeshing

Delta for `add-isotropic-remesh-binding`.

## ADDED Requirements

### Requirement: Isotropic remeshing binding
The C ABI and the Python binding SHALL expose the engine's adaptive isotropic
(triangle) remesher as a one-call operation on a mesh handle, so a caller can
densify, re-tessellate or decimate a mesh to a target edge length without
running the quad pipeline. Parameters SHALL mirror the engine's own options —
target edge length, iteration count, curvature adaptivity and smooth-normal
degrees — and SHALL be fillable from the engine defaults, except the target
edge length, which is world-space and SHALL be left for the caller to supply
rather than invented.

The entry point SHALL assemble the inputs the C++ contract requires: it SHALL
build the projection reference from the input surface before any remeshing, it
SHALL tag feature edges from a supplied dihedral threshold, and it SHALL
triangulate a non-triangulated input rather than rejecting it. The header SHALL
state which of those it does, that the result is therefore a triangle mesh, and
that every element id is invalidated.

A capability of the engine's isotropic options that the binding does not expose
— painted density — SHALL be recorded in the header as a stated omission naming
where it is reachable instead, so the gap is visible rather than assumed absent.

#### Scenario: Target edge length controls density
- **WHEN** a caller remeshes the same mesh at two target edge lengths through the binding
- **THEN** the smaller target SHALL produce a substantially denser mesh whose mean edge length tracks the request, and the reported face count SHALL equal the mesh's own

#### Scenario: Adaptivity reaches the engine
- **WHEN** two runs differ only in the adaptivity parameter
- **THEN** their results SHALL differ, and the adaptive run SHALL show a wider edge-length distribution than the uniform one; repeating either run SHALL reproduce it exactly

#### Scenario: A quad mesh is remeshed, not refused
- **WHEN** a caller passes a mesh carrying quads or n-gons
- **THEN** the call SHALL triangulate it and remesh it, returning a mesh of triangles only

#### Scenario: An unusable target edge length is refused
- **WHEN** the target edge length is zero, negative or non-finite, or the iteration count is below one
- **THEN** the call SHALL return the invalid-parameter status naming the field, and the mesh SHALL be left exactly as it was
