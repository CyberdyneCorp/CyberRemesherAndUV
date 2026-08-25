# Proposal: smooth (Catmull-Clark) subdivision alongside the linear path

## Why

The engine's only subdivision is linear. `cyber_capi.h` says so outright on
`cyber_retopo_subdivide`: *"the engine has no smooth/limit-surface
subdivision"*. Every subdivide call therefore multiplies the quad count and
adds no curvature — the new vertices land on the facets the coarse cage
already had.

The single way to get curvature back today is to pass a `Snapper` and reproject
onto a Target. That works, and it is the accurate answer *when a Target
exists* — but it makes curvature conditional on still having the source scan.
The cases it leaves unserved are ordinary:

- a retopologised cage handed on without the sculpt behind it (the handoff is
  the deliverable; the multi-million-triangle source is not);
- a blockout or box-modelled cage that never had a Target at all — the classic
  subdivision-surface workflow, where the coarse mesh *is* the authoring
  representation;
- any consumer that wants the standard Catmull-Clark limit surface, which is
  what DCC tools, OpenSubdiv and game engines mean by "subdivide".

Nothing about the linear path is wrong — it is the right default for
"subdivide the cage, then pull it back onto the scan", and it is what every
existing caller asked for. The gap is that it is the *only* option.

## What Changes

- **A Catmull-Clark mode in `src/retopo`.** `retopo::subdivide(mesh, mode)`
  with `SubdivisionMode::{Linear, CatmullClark}`. Both modes share ONE topology
  pass (`Mesh::linearSubdivide`, which already builds Catmull-Clark topology);
  the smooth mode then repositions the vertex- and edge-points. Face points are
  the face centroid in both modes — that is already the Catmull-Clark face
  point.
- **Linear stays the default and stays byte-identical.** `subdivideAll(mesh)`
  and `cyber_retopo_subdivide` keep their exact behaviour, pinned by a test
  that hashes the float bits of the linear output against values captured from
  the build immediately before this change.
- **Creases use the sharp rule.** An edge is a crease when it is tagged as a
  feature edge OR is not shared by exactly two faces (boundary, non-manifold,
  wire) — the same "feature or boundary" predicate `relaxQuadMesh()` freezes
  vertices on in `src/core/src/pipeline.cpp`. The two stages must agree, or a
  subdivision that rounds a crease off is followed by a relax that pins the
  rounded result. Crease edge points stay at the midpoint; a vertex with two
  creases follows the cubic B-spline crease curve; a vertex with three or more
  creases, or a valence-2 border vertex (a patch corner), is frozen.
- **`Mesh::linearSubdivide` gains an optional correspondence map.** The smooth
  rules can only reposition the vertex- and edge-points if the caller can name
  them. The parameter defaults to `nullptr` and does not change what is built.
- **A sibling C entry point, not a changed signature.**
  `cyber_retopo_subdivide_ex(mesh, snapper, mode, out_faces)`; the existing
  `cyber_retopo_subdivide` becomes a forwarder passing `CYBER_SUBDIV_LINEAR`.
  Growing a parameter on the existing symbol would break every already-compiled
  caller of a published C ABI; a sibling breaks nobody, and making the old name
  a forwarder is what stops the two paths from drifting apart.
- **Python `Mesh.subdivide(project_to=None, mode=Subdivision.LINEAR)`** with a
  `Subdivision` enum mirroring the C one.
- **The header comment claiming the engine has no smooth subdivision is
  replaced**, along with the README paragraph that repeats it.

## Capabilities

### Modified Capabilities

- `manual-retopology`: the whole-mesh subdivide command gains an opt-in
  smooth mode with documented crease/boundary behaviour.
- `engine-bindings`: the subdivision binding gains the mode selector; linear
  remains the default.

## Impact

- `src/retopo/{include/cyber/retopo/subdivision.hpp, src/subdivision.cpp,
  CMakeLists.txt}`, `src/retopo/include/cyber/retopo/commands.hpp`;
  `src/core/{include/cyber/core/mesh.hpp, src/mesh_ops.cpp}` (additive
  out-parameter only); `capi/{include/cyber_capi.h, src/capi.cpp}`;
  `python/cyberremesh`; `tests/retopo/test_subdivision.cpp`;
  `python/cyberremesh/tests/test_subdivide.py`; `README.md`; `CHANGELOG.md`.
- Purely additive at every layer: no existing entry point changes signature,
  and the linear output is unchanged bit-for-bit.
- Non-goals: per-edge fractional crease *sharpness* weights (creases here are
  binary — infinitely sharp or smooth, matching how the rest of the pipeline
  treats feature tags); explicit limit-position projection (iterating the
  smooth mode converges to the limit surface, but no closed-form limit push is
  offered); hierarchical/adaptive subdivision.
