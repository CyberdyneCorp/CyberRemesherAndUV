# compute-acceleration Specification

## Purpose
TBD - created by archiving change bootstrap-v1-platform. Update Purpose after archive.
## Requirements
### Requirement: Backend abstraction with mandatory CPU reference
Compute-intensive engine work SHALL dispatch through a single backend abstraction exposing typed device buffers and a fixed primitive set (parallel map, reduce, scan, sort, BVH build/traverse, sparse matrix–vector multiply, closest-point projection, ray casting). A CPU backend SHALL always be compiled in, SHALL implement every primitive, and SHALL define correct results. GPU backends are optional accelerators, never functional requirements.

#### Scenario: CPU-only machine runs everything
- **WHEN** the application runs on hardware with no supported GPU
- **THEN** every feature (remesh, snapping, baking) SHALL complete correctly on the CPU backend

### Requirement: Supported GPU backends
The system SHALL provide a Metal backend on Apple platforms (macOS, iPadOS/iOS) and a CUDA backend on NVIDIA hardware (Windows, Linux) as tier-1 backends, and an OpenCL 1.2+ backend (Windows, Linux, Android) as tier-2. Tier-2 absence SHALL degrade gracefully to CPU without feature loss.

#### Scenario: Backend per platform
- **WHEN** the application starts on an Apple-silicon Mac
- **THEN** the Metal backend SHALL be available for selection and used by default for accelerated primitives

### Requirement: Runtime detection, selection, and fallback
At startup the system SHALL enumerate available backends with device names and capability info, select the best available by a documented priority (Metal/CUDA > OpenCL > CPU), allow the user to override the selection, and fall back to CPU automatically when a GPU backend fails at runtime (device lost, out of memory, compile error) — surfacing a warning, never crashing or producing partial results.

#### Scenario: GPU failure mid-run
- **WHEN** a GPU kernel fails during a remesh
- **THEN** the affected stage SHALL rerun on the CPU backend, the run SHALL complete, and the report SHALL note the fallback

### Requirement: Accelerated hot spots
The following pipeline stages SHALL be dispatchable to GPU backends: surface projection / closest-point queries (isotropic stage and retopo snapping), curvature and frame-field smoothing, sparse solver matrix–vector products, and bake ray casting. Irregular topological surgery (collapses, flips, extraction graph edits) SHALL remain on CPU and the specification SHALL NOT promise GPU execution for it.

#### Scenario: Bake rays on GPU
- **WHEN** a bake runs with a GPU backend active
- **THEN** ray casting SHALL execute on the GPU and produce maps within the parity tolerance of the CPU path

### Requirement: Backend parity testing
Every primitive SHALL have automated parity tests running the CPU backend against each available GPU backend on randomized inputs, asserting agreement within documented per-primitive tolerances. Parity tests SHALL run in CI on at least Metal and CUDA hardware lanes.

#### Scenario: Parity gate
- **WHEN** a change causes a GPU primitive to diverge from CPU beyond tolerance
- **THEN** CI SHALL fail identifying the primitive, backend, and observed error

