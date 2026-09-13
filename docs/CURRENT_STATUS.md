# Current capability and integration contract

This is the current delivery view. [The long roadmap](ROADMAP.md) is a dated
research log, not a statement of today's guarantees; OpenSpec specifications
remain normative. The delivery index is [epic #46](https://github.com/CyberdyneCorp/CyberRemesherAndUV/issues/46).

## Capability status

| Capability | Status | Contract / evidence |
|---|---|---|
| Quad-cover remesh | Supported | C++ API, C ABI, CLI and Python. Validity and quality claims are corpus- and density-specific; do not read all-quads/manifold output as animation-ready topology. |
| ZRemesher method | Experimental | Layout, guides, symmetry and best-of-two are exposed, but exact organic layout injection into final meshes remains unproven. It is not parity with commercial ZRemesher. |
| UV atlas, baking, manual mesh edits | Supported engine capabilities | Exposed through the C ABI where declared in `cyber_capi.h`; consult the matching OpenSpec capability for guarantees. |
| Swift package | Build-verified on macOS | SwiftPM and ABI-parity lanes compile/check it. A host supplies the C ABI library; this is not a shipped XCFramework. |
| iPadOS / Android shells | Scaffold / cross-compile only | CI cross-compiles presets. It does not prove device, simulator, touch, GPU, or app-store behavior. |
| CUDA/OpenCL/Metal | Build-dependent | CPU is always available. Ask `cyber_available_backends` / `cyber_active_backend`; a configured backend is not assumed available. |

## Versions and reproducibility

Engine version (`cyber_version`), C ABI version (`cyber_abi_version`) and solver
identity (`cyber_seamless_solver`) answer different questions. ABI compatibility
means a compiled caller can use the header surface; it does **not** promise the
same mesh. Reproducible output requires pinning engine revision, parameters,
backend, and solver/build identity.

Target quad count is a request, not an exact guarantee. Read achieved counts and
calibration outcome from reports; compare algorithms only at matched achieved
counts. Cancellation and import ceilings are per documented ABI entry point:
callers retain ownership of callbacks and opaque handles, and must set resource
ceilings appropriate to their own device.

## Integration paths

Use the versioned C ABI for all external integrations. CMake consumers install
and link `cyber::capi`; validate with `cyber_abi_check` at startup. Python,
Rust and Swift are bindings over that same ABI, so their coverage cannot exceed
declared `cyber_*` entry points. The Swift package's current linking recipe is
in [swift/README.md](../swift/README.md); it is a system-library wrapper, not a
binary distribution. Android support is future work, not a published SDK.

Never retain borrowed mesh-pointer views across a mutating call. Free opaque
objects with their matching `cyber_*_free` function. Configure callbacks and
cancellation for the lifetime of the synchronous call; asynchronous job
isolation is tracked in [#51](https://github.com/CyberdyneCorp/CyberRemesherAndUV/issues/51).

## Release and contribution policy

Additive ABI changes increment the ABI minor; incompatible layouts require a
new ABI major. The checked-in ABI manifest and retained v0.8 client test protect
declarations, compiler layouts and guarded output buffers; the source of truth
remains `capi/include/cyber_capi.h`. Every output-affecting claim needs a dated corpus/configuration,
and historical measurements stay in the research log. Report security issues
privately through the repository's GitHub security-advisory channel; ordinary
bugs and proposals belong in GitHub Issues. Contributions follow the repository
OpenSpec, formatting, test and license gates described in the root README.
