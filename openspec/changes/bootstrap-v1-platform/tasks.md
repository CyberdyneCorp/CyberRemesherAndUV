# Tasks: bootstrap-v1-platform

Ordered by dependency; groups 2–5 unlock everything else. Each group ends with its capability's spec scenarios turned into tests.

## 1. Foundation

- [x] 1.1 Repo scaffolding: `src/core`, `src/accel`, `src/render`, `src/app`, `apps/{desktop,mobile,cli}`, `tests/`, `thirdparty/`
- [x] 1.2 CMake + presets (cpu-headless, macos-metal, windows-cuda, linux-cuda, ios, android), C++20, warnings-as-errors
- [x] 1.3 Third-party manifest + vendored permissive deps (Eigen, tinyobjloader, tinygltf, stb/miniz, test framework); automated license audit script
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
- [ ] 3.3 Typed error model; loud-failure paths wired to CLI/GUI; scale round-trip
- [ ] 3.4 PNG/EXR map output; ZIP package export; USDZ export hook (Apple shell)
- [x] 3.5 Corpus round-trip and malformed-file tests

## 4. compute-acceleration

- [ ] 4.1 Backend abstraction: typed buffers + primitive set (map/reduce/scan/sort/BVH/spmv/closest-point/raycast)
- [ ] 4.2 CPU reference backend (work-stealing pool or std::execution) — contract implementation
- [ ] 4.3 Metal backend (tier-1) + device enumeration/selection/override
- [ ] 4.4 CUDA backend (tier-1)
- [ ] 4.5 OpenCL backend (tier-2, graceful absence)
- [ ] 4.6 Runtime fallback on GPU failure; parity test harness (randomized inputs, per-primitive tolerances); CI hardware lanes

## 5. remeshing-pipeline + remeshing-parameters

- [x] 5.1 Canonical parameter table, validation/clamping module shared by all entry points
- [x] 5.2 Guarded target-edge-length computation; island split/merge orchestration with deterministic ordering
- [ ] 5.3 Adaptive isotropic remesher (feature-safe flips, PN smooth projection, curvature adaptivity)
- [ ] 5.4 Port exploragram QuadCover behind `IParameterizer` (frame field, MIQ solve, OpenNL slice); instance-local progress; concurrent island solves
- [ ] 5.5 Quad extraction (isoline tracing, graph simplification, face enumeration) + pure-quad post-pass with honest residual reporting
- [ ] 5.6 Cleanup policies (hole fill limit, patch policy) as parameters; per-island failure diagnostics; partial-run status
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

- [ ] 13.1 Package jobs: macOS DMG (signed/notarized), Windows zip+installer, Linux AppImage, iOS archive, Android AAB; versioned names from single source
- [ ] 13.2 Packaged-form smoke tests (CLI remesh + launch screenshot; mobile boot in simulator/emulator)
- [ ] 13.3 GitHub Release publication on tags with all artifacts
- [ ] 13.4 openspec validate --all --strict job in CI
