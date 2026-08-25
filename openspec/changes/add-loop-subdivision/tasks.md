# Tasks: triangle-to-triangle densification

## 1. Engine

- [x] 1.1 `cyber::retopo::loopSubdivide` in `src/retopo`: 1 triangle -> 4, interior edge mask 3/8-3/8-1/8-1/8, interior vertex mask with Loop's valence-dependent beta
- [x] 1.2 Boundary rules: boundary edge points are midpoints, boundary vertices use 1/8, 3/4, 1/8 along the boundary curve — never the interior mask
- [x] 1.3 `LoopSubdivideMode::Smooth` and `::Linear`, differing only in vertex placement; linear leaves original positions bit-identical
- [x] 1.4 `LoopSubdivideError` naming the refusal, with the offending face and its side count; validation runs over the whole mesh before anything is built, so a refusal is total
- [x] 1.5 Attribute and feature-edge propagation follows `Mesh::linearSubdivide`'s documented policy

## 2. C ABI

- [x] 2.1 `CYBER_ERR_UNSUPPORTED_TOPOLOGY` appended to `CyberStatus`, plus its `cyber_status_string` entry
- [x] 2.2 `CyberLoopSubdivideMode` and `cyber_retopo_loop_subdivide(mesh, mode, snapper, out_faces)`
- [x] 2.3 An ELEMENT-ID STABILITY note equivalent to `cyber_retopo_subdivide`'s, and the id-keyed handle state cleared on success only
- [x] 2.4 The refusal message names the face and its arity and points at `cyber_retopo_triangulate`
- [x] 2.5 Version script needs no change: `capi/cyber_capi.map` exports `cyber_*` by wildcard
- [x] 2.6 Swift `CyberError` gains `.unsupportedTopology` so it still mirrors the C enum one for one

## 3. Python

- [x] 3.1 Declare `cyber_retopo_loop_subdivide` and `cyber_retopo_triangulate` in `_ffi.py`, with the mode and status constants
- [x] 3.2 `Mesh.loop_subdivide(mode=LoopSubdivideMode.SMOOTH, project_to=None)` and `Mesh.triangulate()`
- [x] 3.3 `UnsupportedTopologyError` raised from `_raise_status`; export it and `LoopSubdivideMode`

## 4. Tests

- [x] 4.1 C++: face count is exactly 4x per triangle, in both modes, and compounds across levels
- [x] 4.2 C++: linear mode leaves every original vertex position untouched and adds exact midpoints
- [x] 4.3 C++: smooth mode moves original vertices, but an open mesh's boundary stays on its curve
- [x] 4.4 C++: a quad input is refused with the typed error naming the face; `triangulate()` then makes it work
- [x] 4.5 Python: both modes through the binding, the default mode is the documented one, and a quad raises `UnsupportedTopologyError`

## 5. Docs

- [x] 5.1 README: show the triangle path next to the quad one
- [x] 5.2 CHANGELOG entry
