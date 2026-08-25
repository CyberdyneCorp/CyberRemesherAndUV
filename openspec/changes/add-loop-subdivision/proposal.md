# Triangle-to-triangle densification (Loop subdivision)

## Why

There is no way to take a triangle mesh and get a denser triangle mesh.

`Mesh::linearSubdivide` — reached as `cyber::retopo::subdivideAll`,
`cyber_retopo_subdivide` and `Mesh.subdivide` — is Catmull-Clark *topology*: it
splits every n-gon into n quads around its centroid. Handed a triangle mesh it
returns a QUAD mesh, three quads per triangle. That is the right answer when the
caller wants quads and the wrong one for every case where the triangles are the
point: a displacement or simulation cage, a collision proxy, a bake source, or
simply "this triangle mesh is too coarse".

The engine's only other densifier, the isotropic remesher, is C++-only and
re-tessellates rather than refines, so it cannot be used to add resolution while
keeping the existing vertices.

So a caller with a triangle mesh has two options today, and both change what
they have: accept quads, or accept a rebuilt tessellation.

## What changes

- `cyber::retopo::loopSubdivide` in `src/retopo`: Loop subdivision, one triangle
  into four, with the standard weights — 3/8-3/8-1/8-1/8 for interior edge
  points, the valence-dependent beta mask for interior vertices — and the
  boundary rules that keep an open mesh's border where it is (midpoints for
  boundary edge points, the 1/8, 3/4, 1/8 curve rule for boundary vertices,
  never the interior mask).
- Two modes, named, never inferred: `Smooth` (true Loop weights, the shape
  changes) and `Linear` (pure 1-to-4 split, no vertex repositioning — more
  polygons, same shape).
- A non-triangle input is refused with a typed error naming the offending face
  and its side count. It is NOT fan-triangulated on the caller's behalf; that is
  a topology decision only the caller can make, and `Mesh::triangulate()` is the
  explicit opt-in.
- New C ABI entry point `cyber_retopo_loop_subdivide` with a
  `CyberLoopSubdivideMode` and the same optional reprojection snapper
  `cyber_retopo_subdivide` takes, carrying an equivalent ELEMENT-ID STABILITY
  note.
- New `CyberStatus` value `CYBER_ERR_UNSUPPORTED_TOPOLOGY`: the mesh is
  well-formed but its topology is not what the operation is defined on. Distinct
  from `CYBER_ERR_INVALID_PARAM` — nothing the caller passed is wrong, and the
  fix is a mesh edit rather than a different argument.
- Python `Mesh.loop_subdivide(mode, project_to=None)`, the `LoopSubdivideMode`
  constants and a typed `UnsupportedTopologyError`. Also `Mesh.triangulate()`,
  which the refusal points callers at and which the binding did not expose.

## Impact

Additive. No existing entry point changes signature or behaviour;
`cyber_retopo_subdivide` is untouched except that it now shares its reprojection
loop with the new call. The new status code is appended, so existing values keep
their numbers. Affected specs: `manual-retopology`, `engine-bindings`.
