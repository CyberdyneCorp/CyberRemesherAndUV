# Tasks: FBX import + subdivision and format-agnostic I/O in the bindings

## 1. Vendor ufbx

- [x] 1.1 Vendor `ufbx.h` / `ufbx.c` + `LICENSE` under `thirdparty/ufbx`, pinned to a release tag
- [x] 1.2 Register it in `thirdparty/manifest.json` and reproduce its MIT notice in `THIRD_PARTY_NOTICES.md`
- [x] 1.3 Confirm `tools/license_audit.py` passes
- [x] 1.4 Compile it as a C translation unit with warnings suppressed as a SYSTEM include, matching the other vendored libraries

## 2. FBX import

- [x] 2.1 `src/core/src/io_fbx.cpp`: `importFbx` reading positions, faces at exact arity, corner normals/UVs, vertex colors
- [x] 2.2 Apply node world transforms; normalize unit scale and axis convention to the engine frame
- [x] 2.3 Honour `PolygonPolicy` (preserve vs triangulate) and record skipped-degenerate warnings like the other importers
- [x] 2.4 Wire `.fbx` into `importMesh` dispatch + declare `importFbx` in `io_internal.hpp` + add the source to `src/core/CMakeLists.txt`
- [x] 2.5 Make `exportMesh` refuse `.fbx` with a typed error naming the writable formats

## 3. C ABI

- [x] 3.1 Add `cyber_mesh_load` / `cyber_mesh_save`; redefine `cyber_mesh_load_obj` / `cyber_mesh_save_obj` as aliases
- [x] 3.2 Document in `cyber_capi.h` that load/save dispatch by extension and that FBX is import-only

## 4. Python binding

- [x] 4.1 Declare `cyber_mesh_load`, `cyber_mesh_save`, `cyber_snapper_create`, `cyber_snapper_free`, `cyber_retopo_subdivide` in `_ffi.py`
- [x] 4.2 `Mesh.load` / `Mesh.save` (classmethod / method), with `load_obj` / `save_obj` kept as aliases
- [x] 4.3 `Mesh.subdivide(project_to=None) -> int` creating and freeing a snapper when a projection target is given
- [x] 4.4 Export the new surface from `__init__.py` and document the formats in the module docstring

## 5. Tests

- [x] 5.1 C++ `tests/core/test_io_fbx.cpp`: quad arity, UVs/normals/colors, unit+axis normalization, multi-node transforms, corrupt-file rejection, `.fbx` export refusal
- [x] 5.2 Commit a small binary FBX fixture and generate it reproducibly from a checked-in script
- [x] 5.3 Python `tests/test_io_formats.py`: `Mesh.load`/`save` round-trip across OBJ/PLY/STL/glTF, FBX load, unsupported-format error
- [x] 5.4 Python `tests/test_subdivide.py`: face-count quadrupling, idempotent id invalidation, reprojection moving vertices onto the target, empty-mesh error
- [x] 5.5 Python `tests/test_examples.py`: run the offline examples end-to-end and assert their outputs appear
- [x] 5.6 Register all three new files with CTest (`tests/CMakeLists.txt`), under the existing `SKIP_RETURN_CODE 77` convention — CI already runs `ctest`, so no new workflow lane was needed; `python_test_examples` gets a raised timeout since it runs the real pipeline six times

## 6. Examples and docs

- [x] 6.1 `examples/16_subdivide.py`: load → subdivide → subdivide+reproject, rendered to `output/16_subdivide.png`
- [x] 6.2 Update `examples/README.md` (FBX now supported; new example row) and `examples/08_load_model.py` docstring
- [x] 6.3 Update the root `README.md` I/O statement and `CHANGELOG.md`
- [x] 6.4 `openspec validate --all --strict` clean (21/21); every file this change adds or edits is clang-format clean. NOTE: `just format-check` fails repo-wide on HEAD independently of this change (e.g. `capi/src/capi.cpp` has 137 violations before and after it, `src/core/src/quad/quadcover.cpp` more) — pre-existing, not addressed here
