# Tasks: bootstrap-v1-platform

Ordered by dependency; groups 2–5 unlock everything else. Each group ends with its capability's spec scenarios turned into tests.

## Autonomous completion loop (resume protocol)

Goal: drive this roadmap to completion in a resumable loop. **Durable state =
these checkboxes + one git commit per task.** If a session is interrupted
(token/context depletion), resume like this:

1. `git pull` and read this file top-to-bottom.
2. Run the verify block below; it must be green before new work.
3. Pick the **first unchecked `[ ]` task** in group order (2→14). Skip only
   tasks whose entire scope is hardware/platform-blocked *and* already noted
   as best-effort below.
4. Implement it; for verifiable tasks build + test + ASan + audits must pass;
   commit (`feat(<area>): <task> — …`) and push. Then tick the box in the same
   or next commit. One task per commit keeps resume boundaries clean.

**Scope decision (user, 2026-07-18):** close the *whole* roadmap. Tasks that
cannot be compiled/run in this headless Linux CLI env (GPU Metal/CUDA/OpenCL,
Vulkan/Metal render, desktop/iPad/Android GUI shells, Apple USDZ/Swift, signed
release lanes) get **best-effort source, explicitly marked UNVERIFIED** in the
task note and commit body — real hardware must still validate them. Everything
CPU/headless is verified done in the normal sense.

## Session handoff (state as of 2026-07-18)

**Verify the checkout works before continuing:**
```sh
cmake --preset cpu-headless && cmake --build --preset cpu-headless && ctest --preset cpu-headless
python3 tools/license_audit.py && openspec validate --all --strict
```
Expected: 2 ctest suites green (75 doctest cases ~101k assertions + Python CLI
integration), both audits clean. `cmake --preset cpu-headless-debug` is the
ASan/UBSan preset — run it after touching the mesh kernel.

**Note (GCC portability):** the initial commit built only under clang; GCC 13
(`-Werror` with `-Wsign-conversion`/`-Wrange-loop-construct`) flagged missing
`<algorithm>` includes, a `std::array` brace-init, a tuple range-loop copy and
a signed literal in a test. All fixed — the tree now builds warning-clean on
both toolchains.

**What exists:** `src/core` (radial-edge half-edge kernel, BVH, OBJ/PLY/STL/glTF
I/O with typed errors, parameters, isotropic remesher, pipeline), `src/accel`
(backend interface + CPU parallelFor seed), `apps/cli` (cyberremesh, full
exit-code map + JSON reports). `src/render`, `src/app`, mobile shells: empty.

**Interim decisions a new session must know:**
- Quadrangulation runs behind the `IQuadrangulator` seam
  (`cyber/core/quadrangulate.hpp`) with a **greedy triangle-pairing baseline**.
  Task 5.4 replaces it with the QuadCover implementation (frame field + MIQ +
  isoline extraction, exploragram/BSD port); design.md's `IParameterizer` will
  live *inside* that implementation. Output today is quad-dominant but without
  global edge-flow alignment.
- Pure-quads mode = remesh at quarter density + one linear subdivision
  (`pipeline.cpp`); replace/augment when real extraction lands.
- Adaptive scales are computed ONCE from input geometry and carried as the
  `__isotropicTargetScale` vertex attribute (per-iteration recompute compounds
  refinement and runs away — see the regression test in `test_pipeline.cpp`).
- `smoothNormalDegrees` drives Curved PN-triangle projection in the isotropic
  stage (`cyber/core/reference_surface.hpp`): the isotropic remesher now
  projects onto a `ReferenceSurface` that reconstructs the hit triangle as a
  Vlachos PN patch, with the angle as a crease threshold for corner-normal
  averaging. 0 degrades exactly to flat closest-point projection (5.3 done).
- `holeFillMaxBoundary` is accepted+clamped but hole filling itself belongs to
  the 5.5 extractor (rest of 5.6 — patch policies, island diagnostics,
  partial status — is done).
- Vendored deps so far: doctest, tinyobjloader, happly, tinygltf (manifest at
  `thirdparty/manifest.json`, enforced by `tools/license_audit.py`). Eigen,
  stb, miniz, geogram-slice get vendored when their consumer tasks start.

**Recommended next steps on a GPU machine:** group 4 (`linux-cuda` /
`windows-cuda` presets already exist; `CYBER_ENABLE_CUDA` currently only sets a
define — 4.1's buffer/primitive abstraction comes first, then the CUDA backend
and the CPU-vs-GPU parity harness of 4.6), or 5.4 (QuadCover port, CPU-only,
biggest single item). 5.9 (golden corpus) is cheap and pays off immediately.

## 1. Foundation

- [x] 1.1 Repo scaffolding: `src/core`, `src/accel`, `src/render`, `src/app`, `apps/{desktop,mobile,cli}`, `tests/`, `thirdparty/`
- [x] 1.2 CMake + presets (cpu-headless, macos-metal, windows-cuda, linux-cuda, ios, android), C++20, warnings-as-errors
- [x] 1.3 Third-party manifest + license audit script; deps vendored on demand (so far: doctest, tinyobjloader, happly, tinygltf — Eigen/stb/miniz arrive with their consumer tasks)
- [x] 1.4 CI skeleton: build matrix, clang-format + clang-tidy gates, license audit job, unit-test job on the cpu-headless preset

## 2. mesh-core

- [x] 2.1 Half-edge kernel (tri/quad/n-gon), indexed round-trip, O(1) adjacency
- [x] 2.2 Mutating operators (collapse, flip, split, insert/delete, merge, subdivide, triangulate) + debug invariant validator
- [x] 2.3 Non-manifold ingestion + diagnostics; face-complete island detection
- [x] 2.4 Feature-edge tagging; generic typed attributes with propagation policy
- [x] 2.5 BVH (closest point, raycast) with backend-consumable interface
- [x] 2.6 Property-based tests (random operator sequences), determinism tests

## 3. mesh-io

- [x] 3.1 OBJ import (n-gon-correct, vertex colors incl. polypaint) + OBJ/MTL export with UVs/normals
- [x] 3.2 PLY, STL, glTF 2.0 import/export; quad preservation where supported
- [x] 3.3 Typed error model; loud-failure paths wired to the CLI; scale round-trip (GUI wiring is inherently task 8.6's work)
- [ ] 3.4 PNG/EXR map output; ZIP package export; USDZ export hook (Apple shell) — deferred until baking (group 11) consumes it
- [x] 3.5 Corpus round-trip and malformed-file tests

## 4. compute-acceleration

- [x] 4.1 Backend abstraction: typed buffers + primitive set (map/reduce/scan/sort/BVH/spmv/closest-point/raycast) — `Buffer<T>` (`accel/buffer.hpp`) + `accel/primitives.hpp` (all primitives dispatch through `IBackend::parallelFor`)
- [x] 4.2 CPU reference backend — the primitive set above is the CPU reference (defines correct results); built on the existing `IBackend`/`parallelFor`/registry; covered by `tests/accel/test_primitives.cpp`
- [ ] 4.3 Metal backend (tier-1) + device enumeration/selection/override
- [ ] 4.4 CUDA backend (tier-1)
- [ ] 4.5 OpenCL backend (tier-2, graceful absence)
- [ ] 4.6 Runtime fallback on GPU failure; parity test harness (randomized inputs, per-primitive tolerances); CI hardware lanes

## 5. remeshing-pipeline + remeshing-parameters

- [x] 5.1 Canonical parameter table, validation/clamping module shared by all entry points
- [x] 5.2 Guarded target-edge-length computation; island split/merge orchestration with deterministic ordering
- [x] 5.3 Adaptive isotropic remesher (feature-safe flips, PN smooth projection, curvature adaptivity) — PN smooth projection lands via `ReferenceSurface` (Curved PN triangles, crease-aware corner normals); `smoothNormalDegrees > 0` now curves projection, 0 stays flat
- [ ] 5.4 Port exploragram QuadCover behind `IParameterizer` (frame field, MIQ solve, OpenNL slice); instance-local progress; concurrent island solves — plugs in behind the existing `IQuadrangulator` seam, replacing the greedy-pairing baseline
- [ ] 5.5 Quad extraction (isoline tracing, graph simplification, face enumeration) + pure-quad post-pass with honest residual reporting — pure-quad mode shipped (quarter-density + subdivide); isoline extraction pending with 5.4
- [ ] 5.6 Cleanup policies (hole fill limit, patch policy) as parameters; per-island failure diagnostics; partial-run status — done EXCEPT hole filling itself (parameter plumbed; implementation belongs to the 5.5 extractor)
- [x] 5.7 ProgressSink/CancelToken plumbing (≤100 ms cancel latency, atomic commit)
- [ ] 5.8 GPU dispatch of hot spots (projection, field smoothing, spmv) via accel layer
- [ ] 5.9 Golden-mesh regression suite (permissive corpus incl. armadillo) with recorded baselines

## 6. cli-headless

- [x] 6.1 CLI binary (core+accel only), full flag set, argument validation, exit-code map
- [x] 6.2 JSON report + stdout summary; TTY-aware progress; --version/--verbose/--quiet
- [x] 6.3 CLI integration tests covering every exit code and clamp warning

## 7. viewport-rendering

- [ ] 7.1 RHI abstraction; Metal backend; Vulkan backend
- [ ] 7.2 Mesh streaming for multi-million-triangle Targets; barycentric wireframe; overlays (pins, loops, symmetry plane, brush radius)
- [ ] 7.3 Remesh stage previews (Source/Isotropic/Param/Result, singularities, cross-pattern UV)
- [ ] 7.4 Camera/navigation with strict contact-count gestures + palm rejection; multi-viewport; downscale option
- [ ] 7.5 Performance benchmarks vs. floors (60 fps @ 5 M tris reference hardware)

## 8. application-shell

- [ ] 8.1 Document model (Target/EditMesh/parameters/bake state), versioned container format, autosave
- [ ] 8.2 Command-journal undo/redo (memory-budgeted, persisted with autosave)
- [ ] 8.3 Shared C++ input layer: stroke capture, chorded modifiers, double-tap/press-hold recognizers, pressure, hover
- [ ] 8.4 Desktop shell (window/input/panels); keyboard shortcut map
- [ ] 8.5 iPadOS shell (pencil/touch event feed, backgrounding autosave); Android shell after tier-1
- [ ] 8.6 Stage switcher; long-op progress/cancel UI with atomic commit (no stale flash); Action Gallery + configurable toolbar; tutorial content; in-app log view (quiet by default)

## 9. manual-retopology

- [ ] 9.1 Target/EditMesh snapping (surface + vertex-snap modifier, image targets)
- [ ] 9.2 Core actions: Pencil grammar (face create, extend, loop insert, X-delete, merge, bridge, edge rotate, cylinder extrude, double-tap tweak), Relax (corner auto-pin, visible radius), Move (geodesic falloff), Tweak (vertex/loop slide), Erase (pressure)
- [ ] 9.3 Advanced tools: BuildQuad/BuildTri, DrawStrip, ExtendBoundary (+grid/fan fill, camera extrusion), PatchClone, TransformVertices, PathDistribute, SurfaceCut, LoopInfo
- [ ] 9.4 Pins (vertex + loop), symmetry (mirror, plane snap, apply), Auto Relax, visibility lassos/occlusion controls, whole-mesh commands, landmark loop tagging
- [ ] 9.5 Unrecognized-stroke feedback; recorded-trace recognizer test suite; interactivity benchmark (≤33 ms @ 5 M-tri Target)

## 10. uv-editing

- [ ] 10.1 Seam draw/erase/sew on 3D mesh; island cut-state display
- [ ] 10.2 X-gesture unwrap (conformal, auto corner pins), island symmetrize; UV pins
- [ ] 10.3 UV3D on-model island transforms (multitouch move/rotate/scale, tileable texture preview), UV clone between matching islands
- [ ] 10.4 UV2D layout tools (tweak rotate/scale halves, grid straightening, partial symmetry, overlap distribute, island merge/stitch)
- [ ] 10.5 Packing (auto with correct scale; manual with grid/pixel/symmetry snapping; texel-density readouts; 1 000-island scale test)
- [ ] 10.6 Distortion overlay + checker + flipped-island indicators

## 11. surface-baking

- [ ] 11.1 Bake core: tangent-space normal, AO, displacement, color (vertex colors + texture-to-texture), GPU rays with CPU fallback, cancellable
- [ ] 11.2 Editable cage (falloff tweak, per-vertex distance, relax, reset) persisted in document
- [ ] 11.3 Component links (draw high→low, X-to-bake per component); nearest-surface default
- [ ] 11.4 Bake preview with movable light; exported-tangent-basis consistency test in a standard glTF viewer

## 12. network-bridge

- [ ] 12.1 Protocol spec + versioned handshake; opt-in server with listening indicator, local-only binding, malformed-message hardening
- [ ] 12.2 Command set (push/pull meshes+textures, scene ops, messages, remote actions, symmetry/change queries, camera stream)
- [ ] 12.3 Python client package + CLI; protocol integration tests
- [ ] 12.4 Blender addon (thin consumer of the Python client)

## 13. engine-bindings

- [ ] 13.1 C ABI facade (`capi/`) over the full surface: pipeline + document/session layer + tool command layer with synthetic input injection (strokes, taps, chords); opaque handles, error codes, progress/cancel callbacks, runtime version query; ABI-compatibility test harness
- [ ] 13.2 Python package: wheels (macOS arm64/x86_64, Windows x86_64, Linux x86_64/aarch64), NumPy mesh views, exceptions, GIL release, progress/cancel objects, stroke-injection API
- [ ] 13.3 Migrate/author integration + interaction test suites (stroke traces, golden meshes, undo/document invariants) on the Python bindings; capability-coverage gate in CI
- [ ] 13.4 Swift package (SwiftPM, iPadOS + macOS): typed errors, async/await with progress, Task-cancellation bridging, UIKit/PencilKit event forwarding, `CAMetalLayer` viewport attachment
- [ ] 13.5 Re-base the first-party iPadOS shell (8.5) on the Swift package; parity checks for both bindings; publish lanes (PyPI, SwiftPM tag)

## 14. build-and-packaging (release lanes)

- [ ] 14.1 Package jobs: macOS DMG (signed/notarized), Windows zip+installer, Linux AppImage, iOS archive, Android AAB; versioned names from single source
- [ ] 14.2 Packaged-form smoke tests (CLI remesh + launch screenshot; mobile boot in simulator/emulator)
- [ ] 14.3 GitHub Release publication on tags with all artifacts
- [x] 14.4 openspec validate --all --strict job in CI (in `.github/workflows/ci.yml` since the foundation commit)
