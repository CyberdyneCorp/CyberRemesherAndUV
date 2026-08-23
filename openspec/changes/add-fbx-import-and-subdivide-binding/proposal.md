# Proposal: FBX import + subdivision and format-agnostic I/O in the bindings

## Why

Two gaps sit between a user and the first thing they try: *load my model and
add resolution*.

1. **FBX cannot be loaded at all.** The importer dispatches OBJ / PLY / STL /
   glTF; `.fbx` falls through to `UnsupportedFormat`. FBX is the interchange
   format the DCC tools this engine targets (Maya, 3ds Max, Blender, Unreal,
   Unity) default to, and `examples/README.md` currently tells the user to go
   convert the file elsewhere first. It was called out as a top user request
   in the founding design review (`bootstrap-v1-platform/design.md`) and never
   built.

2. **Subdivision is unreachable from Python.** `Mesh::linearSubdivide` exists
   in the core and `cyber_retopo_subdivide` exists in the C ABI, but the
   Python `Mesh` exposes only `load_obj` / `save_obj` / `unwrap_atlas`. This
   violates the standing `engine-bindings` requirement that every capability
   be exercisable from Python, and it is the operation a caller reaches for
   right after loading a mesh.

A third, smaller wart falls out of the first: the C entry point named
`cyber_mesh_load_obj` has dispatched on file extension since PLY/STL/glTF
landed, so its name lies about what it loads — and adding FBX makes the lie
louder.

## What Changes

- **FBX import.** Vendor [ufbx](https://github.com/ufbx/ufbx) (MIT, single
  translation unit, matching the tinyobjloader / happly / tinygltf precedent)
  and add `.fbx` to the import dispatch: positions, polygon connectivity at
  exact arity, per-corner normals and UVs, vertex colors, with the file's unit
  and axis conventions normalized to the engine's metre / Y-up frame and all
  mesh nodes merged under their world transforms.
- **FBX export stays out of scope, loudly.** Writing FBX means hand-rolling
  the proprietary binary container (Blender and Maya do not read ASCII FBX),
  and no permissive library writes it. `exportMesh` to `.fbx` SHALL fail with
  a typed error that names the supported alternatives rather than a bare
  "unsupported format".
- **Format-agnostic binding I/O.** Add `cyber_mesh_load` / `cyber_mesh_save`
  as the canonical C names for the extension-dispatching behaviour that
  already exists, keeping `cyber_mesh_load_obj` / `cyber_mesh_save_obj` as
  documented aliases so no caller breaks. Python gains `Mesh.load` /
  `Mesh.save` alongside the existing `_obj` pair.
- **Subdivision binding.** Python `Mesh.subdivide(project_to=None)` over
  `cyber_retopo_subdivide`: linear (Catmull-Clark topology, no smoothing)
  subdivision splitting every n-gon into n quads, optionally reprojecting the
  result onto another mesh's surface — the step that turns linear subdivision
  into curvature-recovering "subdivide + reproject".
- **Test coverage for the Python surface and the examples.** New binding tests
  for the format matrix and for subdivision, registered with CTest alongside
  the existing ones, plus a smoke test that executes the offline examples
  end-to-end. The examples are the project's showcase and its most-run Python
  code, and nothing currently executes them in the suite.
- A `16_subdivide.py` example showing import → subdivide → subdivide+reproject.

## Capabilities

### Modified Capabilities

- `mesh-io`: FBX added to the import formats; FBX export explicitly refused
  with a typed, actionable error.
- `engine-bindings`: format-agnostic load/save and subdivision reachable from
  Python; the Python integration suite runs in CI.

## Impact

- `thirdparty/ufbx` (vendored, MIT) + `thirdparty/manifest.json` +
  `THIRD_PARTY_NOTICES.md`; `src/core/src/io_fbx.cpp` and the dispatch in
  `io.cpp`; additive C ABI entry points in `capi`; `python/cyberremesh`;
  `tests/core/test_io_fbx.cpp` and the Python test suite; `examples/`;
  `.github/workflows/ci.yml`.
- Additive throughout: no existing entry point changes signature or behaviour.
- Non-goals: FBX export; FBX animation, skinning, materials, cameras and
  lights (geometry only, as with the glTF importer); USD.
