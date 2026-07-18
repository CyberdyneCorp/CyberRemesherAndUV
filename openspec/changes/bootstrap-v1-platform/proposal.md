# Proposal: bootstrap-v1-platform

## Why

Artists today need two separate, closed or aging tools to get from a high-poly sculpt to a production-ready quad mesh: AutoRemesher (desktop-only, Qt5/C++14, unmaintained, no cancellation, quad-dominant-only output, CPU-only) for automatic remeshing, and CozyBlanket (iPad-only, closed-source) for manual stroke-based retopology, UV layout, and baking. No single tool combines automatic field-guided quad remeshing with manual stroke-driven retopology and a UV/bake pipeline, runs on both desktop and mobile, and exploits modern GPU compute. This change establishes the founding specification for exactly that product: a pure C++20 engine with CUDA/OpenCL/Metal acceleration and a touch-first, pencil-aware application layer.

## What Changes

- Create a portable, dependency-light **C++20 core engine** (no Qt, no GUI toolkit in the engine) with a half-edge mesh kernel that is robust to non-manifold input.
- Re-implement the **AutoRemesher pipeline** (island separation → adaptive isotropic remeshing → frame-field parameterization → quad extraction) with the known defects fixed by spec: cooperative cancellation, validated/clamped parameters, no divide-by-zero on zero targets, deterministic island merging, non-manifold-safe island detection, holes and small patches handled by policy instead of hard-coded magic numbers.
- Add a **compute acceleration layer** with a single dispatch abstraction and four backends: CPU (always present, reference), CUDA, OpenCL, and Metal, with runtime capability detection and automatic CPU fallback.
- Add **manual retopology** capability re-implementing the CozyBlanket interaction model: a read-only **Target** (reference sculpt) plus an **EditMesh** whose vertices snap to the Target surface; a small set of Actions (Pencil, Relax, Move, Tweak, Erase) reused coherently across stages; a Pencil stroke grammar (closed-shape face creation, X-delete, merge lines, loop insert/slide, edge rotation, bridging); advanced tools (quad/tri build, strip draw, camera-driven boundary extend with grid fill, patch clone, lasso transform, pins, surface cut); symmetry; and Auto Relax.
- Add **UV editing** (stroke-drawn seams, unfold, shell move/rotate/scale, per-vertex tweak, distortion visualization, packing) and **surface baking** (normal, ambient occlusion, displacement, vertex-color transfer from the reference high-poly mesh).
- Add a **cross-platform viewport renderer** (Metal on Apple platforms, Vulkan on Windows/Linux/Android) with the preview overlays AutoRemesher had (source/isotropic/param/result stages, singularity markers, wireframe) plus retopo-mode rendering (semi-transparent reference, on-surface working mesh).
- Add an **application shell** for desktop (mouse/keyboard) and mobile/tablet (touch + stylus chording) with unbounded undo/redo (CozyBlanket caps at 30 steps), autosave with history persistence, document versioning, and robust gesture/palm-rejection handling — the top user complaints against both reference products.
- Add a **network bridge** (opt-in local-network protocol + Python client) for DCC integration — push a Target from Blender/ZBrush, pull the EditMesh back, remote action buttons, live camera streaming — matching CozyBlanket Standard's pipeline story.
- Add **mesh import/export** (OBJ, PLY, STL, glTF 2.0; USDZ export on Apple platforms) that triangulates or preserves polygons correctly and fails loudly — fixing AutoRemesher's silent mis-parse of non-triangulated OBJ and silent write failures.
- Add a **CLI/headless mode** with validated arguments, correct exit codes, and machine-readable (JSON) reports.
- Add **engine bindings**: a stable, versioned C ABI facade over the **full library surface** (pipeline, document/session layer, tool command layer with synthetic input injection) with official **Python** bindings — the desktop test harness through which every capability is exercisable and the integration test suites run — and a **Swift** package that is the supported way to use the library on iPad (consumed by the first-party shell itself).
- Establish **build & packaging**: CMake + presets, CI with per-platform artifacts and smoke tests, unit-test gate (absent in AutoRemesher), and an enforced permissive-license dependency policy.
- **Out of scope permanently**: CozyBlanket 2.1's Papercraft mode (distortion-free unfolding, glue tabs, PDF export) — a product decision, not a deferral; no capability will be created for it.
- **Planned for a future version** (separate follow-up change, not in this one): AI-assisted topology prediction (autocomplete fill suggestions, hole repair with loop rerouting, guide lines to steer flow — CozyBlanket Pro territory). This change only requires that v1 architecture not preclude it: the stroke-recognition layer and EditMesh operations must be drivable programmatically (already implied by the network bridge's remote actions and the toolkit-free `src/app` layer).

## Capabilities

### New Capabilities

- `mesh-core`: C++20 geometry kernel — half-edge mesh, attributes, spatial indices (BVH), manifoldness diagnostics and repair, feature (sharp) edge tagging.
- `mesh-io`: Import/export of OBJ, PLY, STL, glTF 2.0 (+ USDZ export on Apple), with triangulation policy, validation, and loud failure semantics.
- `remeshing-pipeline`: The automatic quad-remeshing pipeline — islands, adaptive isotropic pass, frame-field parameterization, quad extraction, cleanup — with progress reporting and cooperative cancellation.
- `remeshing-parameters`: Single source of truth for user-facing remesh parameters (target quads, edge scale, sharp-edge angle, smooth-normal angle, adaptivity, pure-quad toggle), their defaults, ranges, and clamping in every entry point.
- `compute-acceleration`: Backend-abstracted GPU/CPU compute dispatch — CUDA, OpenCL, Metal, CPU reference — with runtime detection, parity testing, and fallback.
- `manual-retopology`: Target/EditMesh scene model with surface snapping and the stroke-based retopology toolset (draw, extend, connect, loop insert/slide/merge, delete, tweak, relax, symmetry, patch clone, pins, visibility control).
- `uv-editing`: Seam drawing, unfold, shell layout, per-vertex UV tools, distortion display, automatic packing.
- `surface-baking`: High-poly → low-poly projection baking of normal, AO, displacement, and vertex-color maps.
- `viewport-rendering`: Cross-platform GPU viewport (Metal/Vulkan) with stage previews, overlays, and retopo/UV/bake mode rendering.
- `application-shell`: Desktop + mobile application layer — input chording (pencil draws, fingers navigate, held modifier buttons), mode structure (Remesh/Retopo/UV/Bake), undo/redo, autosave, document lifecycle.
- `cli-headless`: Headless batch remeshing with validated flags, exit codes, JSON reports.
- `network-bridge`: Opt-in local-network DCC integration protocol (push Target / pull EditMesh / remote actions / camera stream) with a reference Python client.
- `engine-bindings`: Full-surface C ABI facade; Python bindings as the desktop test harness for all capabilities; Swift package as the iPad consumption path (used by the first-party shell).
- `build-and-packaging`: CMake/C++20 build, platform packages (macOS/Windows/Linux/iOS/Android), CI gates (unit tests, smoke tests, format/lint).

### Modified Capabilities

_None — greenfield repository; `openspec/specs/` is empty._

## Impact

- **New codebase**: everything under this repo is created by this change (engine `src/core`, backends `src/accel`, app `src/app`, CLI `src/cli`, tests `tests/`).
- **Dependencies**: permissively-licensed only (MIT/BSD/Apache). Candidate third-party: Eigen (MPL2), libigl (MPL2), TBB (Apache-2.0) or std-based job system, tinygltf/tinyobjloader (MIT), Dear ImGui (MIT) for desktop tooling UI. GPL/LGPL dependencies (e.g. CoMISo-based mixed-integer solvers) are excluded by policy; the parameterization solver must be re-implemented or use a permissive alternative — this is the highest-risk item, detailed in design.md.
- **Reference material**: `/Users/leonardoaraujo/work/autoremesher` (MIT) is the algorithmic reference for the remeshing pipeline; CozyBlanket is a behavioral reference only (closed source — we re-implement observed behavior, no assets or code).
- **Platforms**: macOS (Metal), iOS/iPadOS (Metal, Apple Pencil), Windows (Vulkan viewport; CUDA/OpenCL compute), Linux (Vulkan; CUDA/OpenCL), Android (Vulkan viewport; OpenCL where available) — with CPU compute fallback everywhere.
