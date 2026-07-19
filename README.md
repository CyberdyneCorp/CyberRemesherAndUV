# CyberRemesher

Quad remeshing, stroke-based retopology, UV editing, and baking — one pure C++20 engine for
desktop and mobile, with CPU/CUDA/OpenCL/Metal compute backends.

Re-implements and improves on [AutoRemesher](https://github.com/huxingyi/autoremesher)
(automatic field-guided quad remeshing) and the workflow pioneered by CozyBlanket
(manual retopology / UV / bake on tablets). Specifications live in [`openspec/`](openspec/) —
see `openspec/changes/bootstrap-v1-platform/` for the founding change (proposal, design,
capability specs, task plan).

### Auto-retopology

Two triangle→quad strategies (`RemeshParams.quad_method`), both clean-room and
permissively licensed:

- **`field-aligned`** (default) — maximum-matching over a smoothed cross field;
  highest quad-dominance (~95%+) with curvature-following flow.
- **`instant-meshes`** — an Instant-Meshes-style position-field extractor
  (4-RoSy orientation + lattice position field → collapse → extract). More
  uniform, field-aligned flow; **matches [QuadriFlow](https://github.com/hjwdzh/QuadriFlow)
  on edge-length uniformity** (CV ≈ 0.17–0.21 vs 0.12–0.17) and trails it a few
  degrees on median quad angle.

Both feed a pure-quad path (subdivision + surface-projected relaxation) for a
100%-quad result. `examples/10_vs_reference.py` renders the side-by-side against
QuadriFlow. GPL sources (AutoRemesher, its QuadCover/CoMISo path) were used only
as idea references, never copied — see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Layout

```
apps/            desktop shell, mobile shells, headless CLI
src/app/         document model, tools, undo (toolkit-free)
src/render/      viewport renderer (Metal | Vulkan)
src/accel/       compute backends: cpu | metal | cuda | opencl
src/core/        mesh kernel, io, remeshing pipeline, uv, bake
tests/           unit + property + golden regression tests
thirdparty/      vendored permissive dependencies (manifest.json)
```

## Build

Requires CMake ≥ 3.24, Ninja, and a C++20 compiler.

```sh
cmake --preset cpu-headless      # core + accel + CLI + tests, no GPU SDK needed
cmake --build --preset cpu-headless
ctest --preset cpu-headless
```

Other presets: `cpu-headless-debug` (ASan/UBSan), `macos-metal`, `linux-cuda`,
`windows-cuda`, `ios`, `android`.

## Development

- Specs first: medium/large changes go through OpenSpec (`openspec list`).
- `python3 tools/license_audit.py` — dependency license gate (permissive only).
- `.clang-format` / `.clang-tidy` are enforced in CI.
