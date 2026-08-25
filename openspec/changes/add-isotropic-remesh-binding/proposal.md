# Isotropic (triangle) remeshing in the bindings

## Why

`cyber::remesh::isotropicRemesh` is real density control — target edge length,
iteration count, curvature adaptivity, PN-triangle smoothing — and it has no C
entry point at all. It is reachable only as an internal stage of the quad
pipeline (`pipeline.cpp`) and of the quad-cover extractor's own harness.

So a binding consumer cannot densify or re-tessellate a triangle mesh. There is
no way, from C, Python or Swift, to answer "give me more polygons to work with"
— the most-asked-for operation on an imported mesh, and the prerequisite for
much of what the ABI offers on top of it: a bake needs a target dense enough to
carry detail, a manual retopology session needs a workable Target, and a
low-poly scan needs resolution before the quad pipeline has anything to align a
field to.

The bindings therefore ship a quad remesher and a UV/bake/retopo toolkit over
an input the caller has no way to re-tessellate. `cyber_retopo_subdivide` is
the closest thing and is not the same operation: linear subdivision quadruples
the face count in one uncontrollable jump, follows no target edge length, and
cannot decimate.

## What changes

- New C ABI parameter struct `CyberIsotropicParams` mirroring the useful subset
  of `IsotropicOptions` (target edge length, iterations, adaptivity, smooth-
  normal degrees) plus the feature-tagging threshold the entry point applies on
  the caller's behalf, with a `cyber_default_isotropic_params` filler like the
  atlas has.
- New C ABI entry point `cyber_mesh_isotropic_remesh`, remeshing a mesh handle
  in place and reporting the resulting face count.
- The entry point assembles what the C++ contract makes a caller assemble, so
  the operation is one call: it triangulates a non-triangulated input (rather
  than rejecting it), tags feature edges, and builds the projection reference
  from the input surface before any remeshing.
- Painted density (`IsotropicOptions::density`) is deliberately NOT mirrored;
  it stays reachable through `cyber_remesh_guided`, and the omission is stated
  in the header so the gap is visible rather than assumed absent.
- Python `IsotropicParams` and `Mesh.isotropic_remesh(params)`, where `params`
  may also be a bare float read as the target edge length.

## Impact

Additive. No existing entry point changes signature or behaviour, and the
engine's `isotropicRemesh` is called exactly as the pipeline already calls it.
Affected specs: `engine-bindings`.
