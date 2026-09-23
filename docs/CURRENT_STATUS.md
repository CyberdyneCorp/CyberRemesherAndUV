# Current capability and integration contract

This is the current delivery view. [The long roadmap](ROADMAP.md) is a dated
research log, not a statement of today's guarantees; OpenSpec specifications
remain normative. The delivery index is [epic #46](https://github.com/CyberdyneCorp/CyberRemesherAndUV/issues/46).

## Capability status

| Capability | Status | Contract / evidence |
|---|---|---|
| Quad-cover remesh | Supported | C++ API, C ABI, CLI and Python. Validity and quality claims are corpus- and density-specific; do not read all-quads/manifold output as animation-ready topology. |
| ZRemesher method | Experimental | Layout, guides, symmetry and best-of-two are exposed, but exact organic layout injection into final meshes remains unproven. It is not parity with commercial ZRemesher. |
| UV atlas, baking, manual mesh edits | Supported engine capabilities | Exposed through the C ABI where declared in `cyber_capi.h`; consult the matching OpenSpec capability for guarantees. An external map consumer drives baking through the `cyber_bake_provider_*` surface: it enumerates the maps this build produces, requests them into its own buffers with progress and cancellation, and receives the encoding, up axis, green-channel convention, padding record and id table alongside the pixels. A map outside the advertised set is refused by name, never substituted. |
| Interactive retopology tools | Supported engine capabilities | Stroke gesture recognition, Target snapping, PolyPen-style face building, stroke-to-quad-strip, Contours (cross-section rings lofted into a tube), boundary grid/fan fill, knife cut, patch clone, loop edits, soft selection with surface glue and exact-border partial retopology. Reachable from C, Python and Swift — including stroke interpretation from Python and UV/bake from Swift, so either binding runs sculpt → topology → unwrap → bake end to end. Per-binding coverage is gated for both bindings by one shared parity check, not assumed. |
| Swift package | Build-verified on macOS; XCFramework shipped | SwiftPM and ABI-parity lanes compile/check it. Since v0.9.0 a versioned arm64 device+simulator XCFramework is published and was validated on a signed physical iPad. Parity is now gated in both directions: every ABI entry point is bound or listed in `PENDING_REGISTRATIONS`. |
| iPadOS / Android shells | Scaffold / cross-compile only | CI cross-compiles presets. iOS additionally has device+simulator XCFramework validation and bounded hardware smoke evidence (v0.9.0); Android does not. Neither shell is an application — they do not prove touch, GPU, or app-store behavior. |
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

The C ABI is **2.1** (soname `libcyber_capi.so.2`; 2.1 appended `maxWorkingSetTexels` to the sized parameter structs and added the regioned bake). Additive changes increment
the ABI minor; incompatible layouts require a new ABI major, and a client of a
different major is refused -- by the loader and by `cyber_abi_check` -- rather
than served. 2.0 exists because `CyberBakeParams` and `CyberBundleParams` grew by
appending during the 1.x series, which let a newer library write past the end of
an older caller's struct; see the 0.10.0 CHANGELOG for the two-line migration.
The checked-in ABI manifest (`capi/abi/cyber_capi-2.1.json`) protects
declarations, compiler layouts and guarded output buffers; the retained v0.8
client test only proves that a separate process loads the shared library, and
cross-major compatibility is not claimed. The source of truth remains
`capi/include/cyber_capi.h`.

**Sized structs** are the one exception to "appending to a struct is MAJOR":
`CyberBakeParams`, `CyberBundleParams` and the three bake-provider descriptors
carry `size_t structSize` as their first member and are passed one at a time by
pointer, never as an array. Set it to `sizeof` the struct before any call; the
library reads and writes only what that size covers, a member it does not cover
takes its documented default, and appending to these structs is additive. The
rule is stated once, in the ABI block at the top of the header. Every output-affecting claim needs a dated corpus/configuration,
and historical measurements stay in the research log. Report security issues
privately through the repository's GitHub security-advisory channel; ordinary
bugs and proposals belong in GitHub Issues. Contributions follow the repository
OpenSpec, formatting, test and license gates described in the root README.
