# Tasks: add-claycore-bridge

- [~] 1. Handoff format spec (versioned PLY/GLB profile + buffer layout),
       agreed with ClayCore's export-profile change — one shared document,
       two implementations

  **Landed:** `docs/sculpt-handoff-format.md` defines sculpt-handoff **v1.0**:
  the normative PLY profile (required vertex properties `x/y/z`, `nx/ny/nz`,
  `red/green/blue`, `material_mix`; triangles only), the
  `comment cyber_sculpt_handoff <major> <minor>` version declaration and the
  optional `comment cyber_handoff_producer <label>`, the GLB profile
  (`asset.extras.cyberSculptHandoff`), the in-memory `BufferView` layout, and
  the compatibility rule (same major + minor ≤ supported; anything else is a
  typed `IncompatibleVersion` rejection, and a newer *minor* is refused rather
  than read with the unknown parts dropped).

  **NOT landed — the agreement half.** ClayCore is not in this repository or
  this environment. There is no way to negotiate the profile with it, and no
  way to commit a shared document to the other repo. The format was therefore
  defined **unilaterally**, and the doc says so at the top in as many words.
  Consequence: the version gate is proven only against files this repo writes,
  so "loud on both sides" is demonstrated on our side only.

- [~] 2. Handoff reader → Target (file + in-memory buffer), version gating,
       typed errors

  **PARTIAL — the PLY and buffer routes are done and tested; the glTF/GLB route
  is untested and does not match its own format doc.** No test anywhere touches
  it: `.gltf`/`.glb` appears in no case in `tests/handoff/test_handoff.cpp`,
  `tests/capi/test_capi.cpp`, `tests/cli/test_cli.py` or
  `python/cyberremesh/tests/test_handoff.py`. Driving it by hand with a v1.0
  `.gltf` (data-URI buffer, POSITION + NORMAL, `asset.extras.cyberSculptHandoff`
  exactly as documented) shows two gaps against
  `docs/sculpt-handoff-format.md:95-108`: the declared `producer` is never read
  (`readGltfFile`, `handoff.cpp:349-380`, fills `out.version` but never
  `out.producer`; `GltfDeclaration::producer` at `:309` is dead), so the report
  says `"producer": ""`; and vertex normals do not survive — the glTF importer
  writes them to the CORNER attribute set (`io_gltf.cpp:151,177`) while
  `Handoff::hasVertexNormals` reads the VERTEX set (`handoff.hpp:54-56`), so the
  report says `"hasVertexNormals": false` for a file that declares a NORMAL
  accessor. Geometry and the version gate itself do work (`"version": {1,0}`,
  4 vertices / 2 faces read back). The doc has been corrected to describe what
  the reader does; making glTF carry normals and the producer is follow-up work.

  **Landed:** new `cyber_handoff` static module (`src/handoff`, namespace
  `cyber::handoff`), linked from `cyber_core` only — no consumer inherits a
  dependency on any sculpting engine. `readFile` (`.ply` native, `.gltf`/`.glb`
  via `asset.extras` + the existing glTF importer), `readStream` (stdin/socket),
  `readBytes` and `readBuffers` all return `cyber::io::Result<Handoff>` — the
  existing typed error model, reusing `io::ErrorCode::IncompatibleVersion`
  established by `export_preset.cpp` rather than inventing a second one.
  Positions land in a `cyber::Mesh`; normals and colors land on the VERTEX
  attribute set under `io::kNormalAttribute` / `io::kColorAttribute`, and
  `material_mix` under `handoff::kMaterialMixAttribute`.

  The version gate reads the PLY **header text directly**, before happly parses
  anything, so a rejected handoff can never leave a partial Target behind.
  Pinned by `tests/handoff/test_handoff.cpp` — including a case that feeds a
  v2.0 header followed by unparseable geometry and asserts the code is still
  `IncompatibleVersion`, never `ParseError`.

- [x] 3. `FieldEvaluator` interface + evaluator-backed sampling in the bake
       dispatcher (normal, AO, curvature); fallback parity tests vs raycast

  **Landed:** `src/bake/include/cyber/bake/field_evaluator.hpp` — header-only,
  depends on `cyber/core/math.hpp` alone, pure-virtual `distance` / `gradient` /
  `occlusion` plus a non-pure `curvature` defaulting to half the divergence of
  the normalized gradient by central differences (so implementers get a correct
  one free). The Lipschitz-≤1 lower-bound contract sphere tracing relies on is
  documented in the header; it cannot be enforced.

  `BakeParams::field` drives it. `bake.cpp` was split into a dispatcher plus two
  shading passes: `shadeFromMesh` (the existing BVH arithmetic, moved verbatim,
  with the AO branch lifted into `aoOpennessFromMesh`) and `shadeFromField`
  (sphere-traces the cage ray, reads exact gradients, takes AO straight from the
  evaluator, and derives the auto curvature range from the sampled texels
  through the same `curvatureScale` percentile helper). Cavity/curvature/normal/
  AO are the four maps a field can serve; `highPoly` may be empty only for
  those, and Displacement/Position/Color still require the Target.

  **Bit-identity proven, not asserted:** FNV-1a checksums over all seven maps
  were captured from the unmodified binary BEFORE the refactor, diffed
  byte-for-byte after it, and hard-coded into
  `tests/bake/test_field_bake.cpp` ("bake without an evaluator reproduces the
  pre-bridge pixels"). The whole pre-existing `tests/bake` suite,
  `tests/exportbundle`, the 17 CLI preset checks and `python_test_bake` all pass
  unmodified.

  **Parity vs raycast:** an analytic `SphereField` bakes against a finely
  tessellated cap of the same sphere; per-texel normal disagreement stays under
  3° (tessellation error). The raycast path's own "coincident flat surfaces are
  tangent-space up" correctness case is re-run through the evaluator.

- [x] 4. Conform operation (topology-preserving re-snap over the snapper,
       max/RMS deviation report, threshold flagging)

  **Landed:** `src/retopo/include/cyber/retopo/conform.hpp` — header-only, built
  on the existing `SurfaceSnapper::snapToSurface` and `PinSet`. Returns
  `ConformReport{movedVertices, maxDeviation, rmsDeviation, flagged}`. Topology
  preservation is structural: the only mutation performed is
  `Mesh::setPosition`. Pinned vertices are skipped AND excluded from the
  statistics (a pin is the user saying "this one does not follow the Target").

  `tests/retopo/test_conform.cpp` asserts topology preservation by comparing a
  full snapshot — vertex/edge/face counts plus every face's vertex list — before
  and after, and asserts every vertex now lies on the new surface by re-querying
  a fresh snapper. Threshold flagging is checked positively AND negatively (the
  unaffected region must NOT be flagged) and the run still completes.

  Also closes the manual-retopology spec's whole-mesh "snap all vertices to
  Target" command, which had never been implemented: `retopo::snapAll` is
  `conform` with a zero threshold, and a test pins the two to identical results
  so they cannot diverge.

- [~] 5. CLI: handoff Target input (path/stdin), report coverage; end-to-end
       demo `clay export --for-retopo | cyber remesh --bake ...`

  **Landed:** `--target <path|->` (`-` reads stdin, `_setmode(_O_BINARY)` on
  Windows for binary PLY), mutually exclusive with `--input` (exit 2 when both
  are given). `--bake <csv>` overrides the preset's map set and implies
  `--preset gltf-generic` when no preset is named (`main.cpp:317-319`); an
  unknown map name is exit 2 naming the offender. **`--bake` is UV-only:**
  the override lives behind `CYBER_CLI_HAVE_PRESETS` (`main.cpp:330-370`), so in
  a `-DCYBER_BUILD_UV=OFF` build the implied preset hits the
  `#ifndef CYBER_CLI_HAVE_PRESETS` branch at `main.cpp:727-731` and the flag is
  an exit-2 "this build has no export-preset support" rather than a bake. The
  `--target` ingest half is unconditional. A rejected handoff — including `IncompatibleVersion` — is
  **exit 3, names both versions, and writes no output file**. The JSON report
  gains a `handoff` block: source, declared version, supported version,
  producer, vertex/face counts, which optional payloads were present, and
  `fieldSampledMaps` (always empty today — see below). Covered by 13 new checks
  in `tests/cli/test_cli.py`.

  **NOT landed — the literal demo.** `clay export --for-retopo | cyber remesh`
  cannot be run: the `clay` binary does not exist in this environment.
  Substituted with `examples/18_sculpt_handoff.py`, a dependency-free synthetic
  producer that emits the same format and drives the real CLI both from a file
  and over a pipe (verified end to end: mesh + normal/ao/curvature written, and
  a future-version handoff rejected with exit 3). The ClayCore-producer half of
  this task remains outstanding.

  **Also not landed:** no field evaluator is reachable *from the CLI*. The
  evaluator is a C++/C-ABI interface and this build links no volumetric engine,
  so the report's `fieldSampledMaps` is always `[]`. It is emitted explicitly
  rather than omitted so a consumer can tell "none" from "not reported".

- [x] 6. Bindings reachability + parity entries

  **Landed:** C ABI — `CYBER_ERR_INCOMPATIBLE_VERSION` appended to `CyberStatus`
  (append-only; existing values keep their numbers), `CyberHandoffInfo`,
  `cyber_handoff_open`, `CyberHandoffBuffers`, `cyber_handoff_open_buffers`,
  `CyberFieldEvaluator` (three C callbacks + `void* user`) with a **separate**
  `cyber_bake_field` entry point rather than growing `CyberBakeParams` (whose
  ABI is deliberately untouched), and `CyberConformReport` + `cyber_conform`
  (which builds the snapper internally). `cyber_status_string` extended;
  `io::ErrorCode::IncompatibleVersion` mapped onto the new code. Five new cases
  in `tests/capi/test_capi.cpp`.

  Python — `_ffi` mirrors every new struct/CFUNCTYPE and declares the four new
  entry points; `api` adds `Mesh.load_handoff` and `Mesh.load_handoff_buffers`
  (the buffer profile shipped declared-but-unwrapped and was wired up during
  review — `api.py:783`, gated by `test_handoff.py:104-112` so a declaration
  without a wrapper fails), `HandoffInfo`,
  `IncompatibleVersionError`, `FieldEvaluator` (a subclassable base whose ctypes
  trampolines are kept alive on the instance), `bake_field` and `conform` +
  `ConformReport`. All exported from `cyberremesh/__init__.py`. Covered by
  `python/cyberremesh/tests/test_handoff.py`, registered in the ctest
  `python_*` loop with the SKIP_RETURN_CODE 77 convention, and verified passing
  against the real shared library.

  Note on the Python evaluator: it works and is tested, but every texel calls
  back into Python, so it is a correctness surface rather than a fast path. The
  docstring says so.

  **Not done:** no Swift binding for the bridge. Swift was not touched by this
  change; the C ABI it wraps is stable and additive, so this is a follow-up, not
  a breakage.

- [~] 7. Docs (handoff format doc committed to both repos) + CHANGELOG

  **Landed:** `docs/sculpt-handoff-format.md` (this repo), a "Sculpt handoff
  bridge" section in `README.md` covering the one-command example, the format
  pointer, the evaluator interface, conform, and an explicit statement that
  there is no ClayCore dependency; `src/handoff/` added to the README layout
  table; a `## Unreleased` CHANGELOG entry naming all three parts **and both
  honest gaps**.

  **NOT landed — "both repos".** Only this repo exists here. The doc is
  committed once, and both it and the CHANGELOG record that the agreement is
  outstanding.

## Verification

- `ctest -j 4`: 15/17 pass. The two failures are the pre-existing ones present
  on unmodified `main` — `seamlessUvResidual` in
  `tests/quadrangulate/test_seamless_solver.cpp:646`, and the `bench` sphere
  singularity/angle baseline. Unit suite: **399 cases, 398 passed, 1 failed**
  (that one pre-existing case), 128997 assertions / 128996 passed.
- `clang-format 18.1.8 --dry-run --Werror` clean over every file touched.
- Full build with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion -Wold-style-cast`: no warnings in any file this change
  touched.
