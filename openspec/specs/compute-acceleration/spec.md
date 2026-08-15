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

The override SHALL be reachable without rebuilding or recompiling anything: in addition to the in-process selection API and the CLI flag, the `CYBER_BACKEND` environment variable (`cpu` | `metal` | `cuda` | `opencl`) SHALL pin the process-wide default, because the installed artifact exposes only the C ABI and a consumer debugging a GPU-specific problem cannot patch the host application. An unset or unrecognized value SHALL keep the automatic choice rather than fail. Probing SHALL happen once per process — a backend enumeration SHALL NOT rebuild device contexts or recompile kernels on each call.

#### Scenario: GPU failure mid-run
- **WHEN** a GPU kernel fails during a remesh
- **THEN** the affected stage SHALL rerun on the CPU backend, the run SHALL complete, and the report SHALL note the fallback

#### Scenario: Backend pinned from the environment
- **WHEN** a host process that offers no backend UI runs with `CYBER_BACKEND` set to a compiled-in backend
- **THEN** every accelerated primitive SHALL dispatch to that backend for the life of the process, and an unrecognized value SHALL leave the automatic best-first selection in place

### Requirement: Accelerated hot spots
The following pipeline stages SHALL be dispatchable to GPU backends: surface projection / closest-point queries (isotropic stage and retopo snapping), curvature and frame-field smoothing, sparse solver matrix–vector products, and bake ray casting. Irregular topological surgery (collapses, flips, extraction graph edits) SHALL remain on CPU and the specification SHALL NOT promise GPU execution for it.

Selecting a GPU backend SHALL NOT degrade any CPU-side work: the library's host-side parallel loops SHALL keep running on the worker pool whichever backend is active, so choosing a GPU can only add acceleration, never remove it. Data a caller queries repeatedly SHALL NOT be re-uploaded per query — a BVH handed to successive ray or closest-point calls SHALL be resident on the device, keyed so that a different structure is detected — and the residency contract (the caller SHALL hand over a rebuilt snapshot rather than mutate arrays already passed) SHALL be documented on the interface.

#### Scenario: Bake rays on GPU
- **WHEN** a bake runs with a GPU backend active
- **THEN** ray casting SHALL execute on the GPU and produce maps within the parity tolerance of the CPU path

#### Scenario: A GPU does not serialize the CPU
- **WHEN** the same workload runs with a GPU backend selected and with the CPU backend selected
- **THEN** the library's CPU-side parallel loops SHALL be multi-threaded in both cases

#### Scenario: Repeated queries against one BVH do not re-upload it
- **WHEN** a bake issues many ray or closest-point queries against the same BVH
- **THEN** the per-query cost SHALL be independent of the BVH's size once it is resident, rather than growing with the triangle count

### Requirement: Backend parity testing
Every primitive SHALL have automated parity tests running the CPU backend against each available GPU backend on randomized inputs, asserting agreement within documented per-primitive tolerances.

CI SHALL at minimum COMPILE every GPU backend on every change, so a backend cannot be broken by a change no lane builds. Parity itself needs a device, which hosted runners do not have and a CPU OpenCL ICD does not supply (the OpenCL backend takes GPU-type devices only): the parity run SHALL therefore exist as a lane that can be pointed at a runner that owns the hardware, and the distinction between the compile gate and the hardware lane SHALL be stated where each is defined rather than left for a reader to infer.

A parity test SHALL be incapable of passing vacuously. It SHALL compare the CPU backend against every OTHER compiled-in backend and SHALL fail — not skip silently — when a backend the build compiled in is not exercised. Its generated inputs SHALL cover the shapes the engine actually dispatches, in particular non-square sparse matrices in both orientations, since the seamless solver's transfer operators are rectangular and a square-only generator hides a whole class of indexing defect.

#### Scenario: Parity gate
- **WHEN** a change causes a GPU primitive to diverge from CPU beyond tolerance
- **THEN** CI SHALL fail identifying the primitive, backend, and observed error

#### Scenario: A backend nobody compiles
- **WHEN** a change breaks a GPU backend's translation unit
- **THEN** an ordinary CI run SHALL fail on the compile, without needing a device

#### Scenario: A self-comparison is not parity
- **WHEN** the parity suite runs in a configuration where a compiled-in backend is unavailable, or where the only backend it would compare is the CPU backend itself
- **THEN** it SHALL report that state as a failure or an explicit skip rather than passing as though parity had been demonstrated

#### Scenario: Rectangular operands are covered
- **WHEN** the sparse matrix–vector parity case runs
- **THEN** it SHALL exercise square, wide (more columns than rows) and tall (more rows than columns) matrices, so a backend that sizes its operand buffers by row count is caught

