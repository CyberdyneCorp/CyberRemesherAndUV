# Tasks: smooth (Catmull-Clark) subdivision alongside the linear path

## 1. Capture the linear baseline before touching anything

- [x] 1.1 Hash the float bits of `Mesh::linearSubdivide` output for a cube (2 levels), an open patch (2 levels) and a triangle (1 level) from the pre-change build
- [x] 1.2 Bake those hashes into a test so any drift in the default path fails loudly

## 2. Core: name the subdivision vertices

- [x] 2.1 `Mesh::SubdivisionMap` (source vertex/edge/face id -> result vertex id)
- [x] 2.2 `Mesh::linearSubdivide(SubdivisionMap* correspondence = nullptr)`, filling it only when asked; the built mesh is unchanged

## 3. Retopo: the Catmull-Clark rules

- [x] 3.1 `SubdivisionMode` + `retopo::subdivide(mesh, mode)` in `subdivision.hpp` / `subdivision.cpp`
- [x] 3.2 Crease predicate: feature-tagged OR not exactly two incident faces — the same predicate `relaxQuadMesh()` freezes on
- [x] 3.3 Edge points: smooth `(v0 + v1 + f0 + f1) / 4`; crease edges keep the midpoint the linear pass wrote
- [x] 3.4 Vertex points: smooth `(F + 2R + (n-3)P) / n`; two creases -> `(a + 6P + b) / 8`; three or more creases, or a valence-2 border vertex, -> frozen
- [x] 3.5 Read every rule off the SOURCE mesh, so no vertex sees another's new position
- [x] 3.6 `subdivideAll(mesh, mode = Linear)` keeps the existing call sites compiling and behaving identically

## 4. C ABI and Python

- [x] 4.1 `CyberSubdivisionMode` + `cyber_retopo_subdivide_ex`; `cyber_retopo_subdivide` forwards with `CYBER_SUBDIV_LINEAR`
- [x] 4.2 Reject an unknown mode with `CYBER_ERR_INVALID_ARG`
- [x] 4.3 Replace the header comment claiming the engine has no smooth subdivision; state why the mode arrived as a sibling entry point rather than a new parameter
- [x] 4.4 `_ffi.py` declarations + `Subdivision` enum + `Mesh.subdivide(project_to=None, mode=Subdivision.LINEAR)`, exported from `__init__.py`

## 5. Tests

- [x] 5.1 `tests/retopo/test_subdivision.cpp`: linear output matches the pre-change hashes, through `retopo::subdivide`, `subdivideAll` and `Mesh::linearSubdivide` alike
- [x] 5.2 Cube convergence, quantified: the worst interior dihedral falls by at least 0.35 per level (measured 90 -> 43.3 -> 26.5 -> 13.9 -> 7.1 degrees) while the linear path holds at exactly 90 forever
- [x] 5.3 Cube roundness, quantified: vertex-radius spread 0.568 of the mean (linear, 3 levels) vs 0.028 (smooth) — a 19x tighter shell
- [x] 5.4 Open planar patch: bounding box, planarity and border straightness preserved exactly through 3 smooth levels
- [x] 5.5 Open tube: both boundary rings stay in their own plane (z exactly 0 and 2) at every level, rather than creeping along the axis
- [x] 5.6 Fully feature-tagged cube: Catmull-Clark returns it as an exact cube
- [x] 5.7 Attributes are identical between modes (the smooth rules write positions only)
- [x] 5.8 Python: smooth mode rounds a cube with no Target, keeps an open patch's border, rejects an unknown mode; the default stays linear
- [x] 5.9 Verify each new test FAILS when the behaviour it covers is reverted

## 6. Documentation

- [x] 6.1 `README.md` subdivision paragraph: linear default, smooth mode, what each is for
- [x] 6.2 `CHANGELOG.md` entry
- [x] 6.3 `examples/16_subdivide.py` docstring no longer states subdivision is linear full stop
