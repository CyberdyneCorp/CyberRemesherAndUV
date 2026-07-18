# Design: bootstrap-v1-platform

## Context

Two reference products define the problem space:

- **AutoRemesher** (MIT, `/Users/leonardoaraujo/work/autoremesher`): automatic quad remeshing via isotropic remesh → frame-field QuadCover parameterization (Geogram/exploragram, BSD-3) → integer-isoline quad extraction. Qt5/C++14 desktop app, CPU-only (TBB), quad-dominant output, no cancellation, several documented defects. Its `openspec/specs/` baseline (9 capability specs with file:line citations) is our behavioral reference for the pipeline.
- **CozyBlanket 2.1** (Sparseal, closed-source, iPadOS 15+): manual stroke-based retopology over a read-only **Target** with an **EditMesh** snapped to its surface; organized as Stages (Retopology → UV → Baking) with a small Action set (Pencil, Relax, Move, Tweak, Erase + a customizable gallery) whose semantics stay coherent across stages; Apple-Pencil-draws / fingers-navigate chording; a "camera-as-tool" pattern (select with a stroke, transform by moving the camera); 30-step undo, autosave, OBJ-only import (with vertex colors), OBJ/STL/ZIP export, tangent-normal + color baking with an editable cage, and an opt-in local-network bridge (Python module, Blender addon, ZBrush plugin). Behavioral reference only — reverse-engineered from public materials, App Store history, and tutorials; no code or assets are used.

This repo is empty (LICENSE + git only). This change is the founding spec for v1.

## Goals / Non-Goals

**Goals:**
- One engine, one document model, four workflow modes: **Remesh (automatic), Retopo (manual), UV, Bake**.
- Pure C++20 core engine with zero UI-toolkit dependency; runs identically on desktop and mobile.
- GPU acceleration behind one dispatch abstraction: CUDA, OpenCL, Metal, plus a mandatory CPU reference backend; all backends bit-compatible within tolerance and covered by parity tests.
- Fix every defect catalogued in AutoRemesher's baseline specs (see "Inherited defect ledger" below).
- Touch-first interaction parity with CozyBlanket's stroke grammar, improved where it is known weak (unbounded undo/redo vs. 30 steps, visible brush radius, stroke-registration feedback, robust palm rejection and camera gestures, multi-format I/O, no paywalled saving).
- Testable from day one: unit tests per capability, golden-mesh regression suite, CLI smoke tests in CI.

**Non-Goals (v1):**
- Sculpting, painting, or texturing beyond baking outputs.
- Animation, rigging, or scene composition (single-object documents only; a scene outliner is a known follow-up).
- Papercraft mode (CozyBlanket 2.1) — permanently out of scope by product decision; will not be built.
- AI topology prediction (CozyBlanket Pro territory) — planned for a future version as its own change. v1 must not preclude it: tool operations on the EditMesh are exposed as programmatic commands (the same command layer undo and the network bridge use), so a future suggestion engine can propose and apply topology edits without UI changes. No ML runtime ships in v1.
- Cloud features (sync, telemetry, licensing servers). The opt-in **local-network** DCC bridge is in scope; nothing leaves the local network and it is off by default.
- An in-app plugin system and a stable public **C++** API. (Scripting/embedding IS in scope via the C ABI + Python/Swift bindings — see D10 — but the C++ layer underneath stays free to change.)
- Windows-on-ARM, WebAssembly, visionOS (architecture must not preclude them).
- Feature-parity with CozyBlanket's business model (tiers/paywalls) — the spec treats all capabilities as available.

## Decisions

### D1. Layering
```
apps/            desktop shell, mobile shell, cli
src/app/         document model, modes, tools, undo, autosave   (C++20, no UI toolkit)
src/render/      viewport renderer (Metal | Vulkan), overlays
src/accel/       compute dispatch: cpu | cuda | opencl | metal
src/core/        mesh kernel, io, remeshing pipeline, uv, bake  (pure C++20)
```
`src/core` depends only on the C++20 standard library plus vetted permissive third-party (Eigen/MPL2, tinyobjloader/tinygltf MIT). `src/accel` and `src/render` are the only layers containing platform/GPU code. Rationale: mobile support and headless CLI both demand an engine that links without Qt/AppKit/UIKit.

### D2. Parameterization solver: port exploragram QuadCover, keep it swappable
AutoRemesher's novel value is orchestration around Geogram's exploragram `quad_cover` (BSD-3, permissive). We port/embed that solver path (frame field + mixed-integer quad-cover + OpenNL) into `src/core/param/` rather than re-deriving MIQ from papers, preserving its license attribution. The pipeline defines a `IParameterizer` seam so an alternative (e.g. libigl MIQ, also MPL2) can be swapped for benchmarking. Risk drops from "re-implement a research solver" to "port + harden a BSD one". The Geogram global-progress spinlock problem (which serialized island solves in AutoRemesher) is eliminated by making solver progress state instance-local in the port.

### D3. Compute abstraction: kernel-graph dispatch, CPU as source of truth
`src/accel` exposes typed buffers + a small set of primitive parallel ops (map, reduce, scan, sort, BVH build/traverse, sparse matvec for the solver, closest-point projection). Backends: **CPU** (std::execution / own work-stealing pool — always compiled, defines correct results), **Metal** (Apple), **CUDA** (NVIDIA desktop), **OpenCL 1.2+** (fallback GPU path on Windows/Linux/Android). GPU targets the pipeline's measured hot spots: surface projection/AABB queries in the isotropic pass, curvature/frame-field smoothing, solver matvecs, bake ray-casting. Graph-topology mutation (edge collapse/flip/split, extraction graph surgery) stays CPU — irregular pointer-chasing work that GPUs do badly; the spec must not promise "everything on GPU".

### D4. Rendering: Metal + Vulkan only
Metal on macOS/iOS/iPadOS; Vulkan on Windows/Linux/Android. No OpenGL backend (AutoRemesher's GL 3.3 approach is EOL on Apple platforms). One thin RHI (render hardware interface) with two implementations; MoltenVK is explicitly rejected for the viewport to keep Apple Pencil latency paths native. Wireframe rendering uses barycentric-based shading (not geometry shaders — unsupported on Metal/mobile).

### D5. UI shells
The document/tool/undo layer (`src/app`) is toolkit-free and fully unit-testable; shells are thin:
- **Desktop shell**: windowing + input via a single cross-platform layer (SDL3 or GLFW + Dear ImGui for panels; final pick is an implementation detail behind the shell boundary).
- **Mobile shell (iPadOS first, Android later)**: native thin shell (Swift/UIKit wrapper ~hundreds of lines) feeding pencil/touch events into the same `src/app` tool router — consuming the official Swift package (D10), not private hooks. Stroke recognition, chording, gestures are implemented once, in C++, in `src/app` — not per platform.

### D6. Document model and undo
Single-document sessions adopting CozyBlanket's proven vocabulary: one **Target** (immutable high-poly reference) + one **EditMesh** (working retopo mesh with UVs and bake state) + remesh parameter state. Undo/redo is a command journal with structural mesh deltas, unbounded within a memory budget, persisted with autosave so a crash restores both the mesh and its history. This improves on CozyBlanket's 30-step undo and AutoRemesher's absent undo.

### D7. Cancellation and progress as first-class engine contracts
Every long-running engine call takes a `ProgressSink&` (monotonic per stage, weighted merge across islands — same mapping AutoRemesher used: isotropic 0.0–0.3, param 0.3–0.9, extraction 0.9–1.0) and a `CancelToken`. Cancellation is cooperative, checked at bounded intervals (< 100 ms worst case), and leaves the document untouched (results are committed atomically at the end).

### D8. File formats
Import: OBJ (with correct n-gon triangulation), PLY, STL, glTF 2.0 (.glb/.gltf). Export: OBJ (positions + UVs + normals), glTF 2.0, PLY, STL, plus USDZ on Apple platforms (via Apple's USD toolchain in the shell, not in core). Bake outputs: PNG/EXR. All parse/write failures are loud (typed error + user-visible message + nonzero CLI exit).

### D9. Network bridge as the pipeline story
Rather than chasing every DCC's plugin API in v1, we spec a small documented local-network protocol (push Target geometry, pull EditMesh, remote action buttons, camera stream — the same command set CozyBlanket exposes) plus a reference Python client. Blender/ZBrush plugins become thin consumers of that client and can evolve independently of the app. Off by default; binds to localhost/LAN only; no cloud endpoint exists.

### D10. Engine bindings: Python as the desktop test harness, Swift as the iPad consumption path
All language bindings sit on one **versioned C ABI facade** (`capi/`) — opaque handles, plain C types, error codes, C function-pointer callbacks; no C++ types cross the boundary. The facade covers the **full library surface**, including the document/session layer and the tool command layer with synthetic input injection (stroke sequences, taps, chords), not just the headless pipeline. Two consumers with distinct roles:
- **Python** (pip package, desktop): the project's test harness. Every capability must be exercisable from Python, and the integration/interaction test suites are written against it — so binding coverage is enforced by the tests the project already needs. GIL released during long calls; NumPy views over mesh data.
- **Swift** (SwiftPM package, iPadOS/macOS): the supported way to use the library on iPad. The first-party iPadOS shell itself consumes this package (no private hooks), which guarantees third parties get the same surface: document control, all tools, UIKit/PencilKit event forwarding, viewport attachment to a `CAMetalLayer`.
This also gives the future AI-topology-prediction work and DCC plugins a supported programmatic entry point.

### D11. Testing strategy
- Unit tests (per core module) + property tests on the mesh kernel invariants (halfedge integrity after every operator).
- Golden regression corpus: a set of CC0/permissive meshes (incl. armadillo) remeshed in CI; quad count, non-quad count, singularity count, and Hausdorff distance to input tracked against recorded baselines with tolerances.
- Backend parity: every accel kernel runs CPU-vs-GPU on randomized inputs with documented tolerances.
- Interaction tests: stroke-grammar recognizers driven by recorded synthetic stroke traces (no UI needed).

### Inherited defect ledger (must-fix by spec)
From AutoRemesher's baseline specs — each becomes a normative requirement in the deltas:
1. No cancellation → D7.
2. `--target-quads 0` → division by zero → parameter clamping capability.
3. CLI: non-numeric args silently 0; exit 0 on failure; empty `--version`; unenforced ranges → cli-headless spec.
4. OBJ n-gons silently mis-parsed; silent export/report write failures → mesh-io spec.
5. Non-manifold island detection drops faces (last-writer-wins edge map) → mesh-core spec.
6. Feature-edge flips cross sharp edges (guard commented out) → remeshing-pipeline spec.
7. Islands that fail parameterization silently vanish → pipeline must report per-island diagnostics.
8. Hard-coded hole-fill limit (65 verts) and largest-island-only policy → explicit user-facing policies.
9. Save doesn't clear dirty flag; stale-result flash; render misclassification race → application-shell spec.
10. Dead `ModelType` parameter → not carried over.
11. Unconditional debug spew to stderr → diagnostics requirements in cli/app specs.
12. Quad-dominant only → v1 adds a "pure quad" post-pass option (topological cleanup of non-quads where achievable; report residual non-quads honestly).

CozyBlanket known weaknesses (from review/App Store analysis) fixed by spec: 30-step undo cap (D6), invisible relax-brush radius, no stroke-registration feedback, erratic camera with >2 touch contacts / weak palm rejection, OBJ-only I/O (no FBX/glTF/USD — a top user request), no multi-object management (deferred, but the document format must not preclude it), save/export behind paywall (we don't paywall capabilities in the spec).

## Risks / Trade-offs

- **Exploragram port complexity** — the quad_cover path drags in a slice of Geogram (OpenNL, mesh internals). Mitigation: port behind `IParameterizer` with its own namespace; keep the vendored slice buildable standalone; budget the port as its own task group. Fallback: link Geogram as a static third-party lib for v1 and defer the extraction of the slice.
- **Four compute backends is a large surface.** Mitigation: CPU backend is the contract; Metal and CUDA are tier-1, OpenCL is tier-2 (may ship after v1.0 without spec change — the capability spec requires graceful absence, not presence). Parity tests keep drift out.
- **Stroke-grammar fidelity** — CozyBlanket's recognizers are unpublished; ours are behavioral reconstructions and will need tuning. Mitigation: recognizers are data-driven and covered by recorded-trace tests, so tuning doesn't destabilize the toolset.
- **Mobile + GPL-free UI constraint** rules out Qt; Dear ImGui-style desktop panels look less native. Accepted for v1 (tool-heavy audience), revisit post-v1.
- **Performance targets on mobile** (multi-million-poly reference sculpts on iPad) depend on Metal buffer streaming; the spec sets measurable floors (see viewport-rendering spec) rather than aspirational ceilings.
- **Patent/IP**: QuadCover is published academic work implemented in BSD code; CozyBlanket behaviors are re-implemented from public observation — standard clean-room practice, no assets copied. Product name/branding must not reference CozyBlanket.
