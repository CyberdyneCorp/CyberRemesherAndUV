# Tasks: isotropic remeshing in the bindings

## 1. C ABI

- [x] 1.1 `CyberIsotropicParams` mirroring target edge length, iterations, adaptivity and smooth-normal degrees, plus the `sharpEdgeDegrees` threshold for the feature-tagging pre-pass
- [x] 1.2 `cyber_default_isotropic_params` filler, sourcing its values from `IsotropicOptions` / `Parameters` rather than restating them
- [x] 1.3 `cyber_mesh_isotropic_remesh(CyberMesh*, const CyberIsotropicParams*, size_t* out_faces)` on `runMeshEdit`, so the render caches and the id-keyed handle state are invalidated the way every other topology-rewriting op does
- [x] 1.4 Non-triangulated input is TRIANGULATED, not rejected; the choice and its consequence (the output is a triangle mesh) are stated in the header
- [x] 1.5 Typed, argument-naming errors: `CYBER_ERR_INVALID_ARG` for a null mesh or null params, `CYBER_ERR_EMPTY` for a mesh with no faces, `CYBER_ERR_INVALID_PARAM` for a non-positive/non-finite target edge length or a zero iteration count — all decided before the mesh is touched
- [x] 1.6 No module gating needed: `isotropicRemesh` lives in `cyber_core`, which the C ABI links in every configuration
- [x] 1.7 Version script needs no change: `capi/cyber_capi.map` and `cyber_capi.symbols` export `cyber_*` by wildcard
- [x] 1.8 Painted density is left out and the omission is stated in the header, naming `cyber_remesh_guided` as where it is reachable

## 2. Python

- [x] 2.1 Declare `CyberIsotropicParams`, `cyber_default_isotropic_params` and `cyber_mesh_isotropic_remesh` in `_ffi.py`
- [x] 2.2 `IsotropicParams` dataclass (no default for `target_edge_length` — it is world-space) and `Mesh.isotropic_remesh(params)`, accepting a bare float as the target edge length
- [x] 2.3 Export `IsotropicParams` from `__init__.py`

## 3. Tests

- [x] 3.1 C++ (`tests/capi/test_capi_isotropic.cpp`): output density tracks `targetEdgeLength` — face count and mean edge length both
- [x] 3.2 C++: `adaptivity` reaches the engine's scale field — two runs differing only in the knob disagree, and the adaptive run's edge-length spread is wider; the same value twice is still deterministic
- [x] 3.3 C++: a quad mesh comes back as pure triangles; the defaults filler leaves the target edge length unset; every rejection leaves the mesh untouched
- [x] 3.4 Python (`python/cyberremesh/tests/test_isotropic_remesh.py`, registered in `tests/CMakeLists.txt`): the same density/adaptivity/triangulation claims through the binding, plus a parity check that the header's isotropic symbols and struct fields are bound

## 4. Docs

- [x] 4.1 CHANGELOG entry
- [x] 4.2 README: name the operation next to `Mesh.subdivide`, and say how the two differ
