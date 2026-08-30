# CyberRemesher — Swift package (SwiftPM)

Idiomatic Swift bindings for the CyberRemesher engine, for **iPadOS 15+** and
**macOS 12+**. This package wraps the versioned **C ABI facade**
(`capi/include/cyber_capi.h`) — no C++ types cross the boundary.

The package binds the **real header**, symbol for symbol. Two checks keep it
that way:

* **`swift build`** on a macOS runner, in `ci.yml` (the `swift-package` job) and
  in `publish-swift.yml`. This is the only thing that proves it compiles.
* **`tests/packaging/test_swift_abi_parity.py`**, registered in ctest as
  `swift_abi_parity`. Pure text parsing — no Swift toolchain, no engine — so it
  runs everywhere. It asserts that every `cyber_*` / `CYBER_*` / `Cyber*`
  identifier under `Sources/` is declared in `cyber_capi.h`, that every
  `cyber_*` call passes the declared number of arguments, and that every C
  struct literal uses real field names.

The parity test exists because this package once carried a hand-written
"assumed ABI contract" that drifted from the header for its whole life: nothing
built it, so nothing noticed.

## Layout

```
swift/
  Package.swift                     # products + targets, header/link wiring
  Sources/
    CCyberRemesher/                 # C system-library target
      module.modulemap
      shim.h                        # #include <cyber_capi.h>
    CyberRemesher/                  # Swift target
      CyberError.swift              # typed errors over CyberStatus
      CyberRuntime.swift            # engine version / status strings / last error
      Mesh.swift                    # RAII wrapper over the opaque CyberMesh
      Remesh.swift                  # async remesh + AsyncStream progress + cancel
      ZRemesher.swift               # ZRemesher params/report, guides + guide modes
      SoftSelection.swift           # per-vertex weight field + weighted ops
      Document.swift                # persisted Target/EditMesh + named slots
      SeamPath.swift                # UV Path tool: seam sets + routed paths
      StrokeInterpretation.swift    # gesture grammar over cyber_stroke_interpret
      InputForwarding.swift         # UIKit + PencilKit stroke capture
  Tests/
    CyberRemesherTests/             # runtime parity checks (macOS lane; `swift test`)
```

## Building / consuming

1. Build the engine + `capi/` module with CMake (`-DCYBER_BUILD_CAPI=ON`) to get
   the `cyber_capi` shared library. The header is already in the repo at
   `capi/include/cyber_capi.h`.
2. Point the Swift build at them:
   - **Xcode app target**: add the package as a local SwiftPM dependency and set
     `HEADER_SEARCH_PATHS` to `capi/include` and `LIBRARY_SEARCH_PATHS` to the
     built dylib. Drop the `unsafeFlags` in `Package.swift`.
   - **`swift build` from this dir**: `Package.swift` already adds
     `-I../capi/include` and links `cyber_capi`; add `-L` for the dylib via
     `swift build -Xlinker -L<path>`.

## Usage sketch

```swift
import CyberRemesher

let mesh = try Mesh.loadOBJ("model.obj")

// async/await remesh with progress + cancellation
var params = RemeshParameters(targetQuads: 5000)   // starts from cyber_default_params
params.pureQuads = true
params.quadMethod = .quadCover
let op = mesh.remesh(params: params)
let progressTask = Task { for await p in op.progress { updateBar(p) } }
let quadMesh = try await op.value()          // throws .cancelled if the Task cancels
progressTask.cancel()
try quadMesh.saveOBJ(to: "model_quads.obj")

// ZRemesher-class retopology: quality mode, symmetry, guide modes, run report.
// `params.quadMethod` is ignored on this path — the call IS the method.
let result = try await mesh.remesh(
    params: RemeshParameters(targetQuads: 2000),
    zremesher: ZRemesherParameters(quality: .best, symmetry: .x),
    guidance: Guidance(guides: [
        FlowGuide(points: eyeLoop, radius: 0.05, mode: .topology, closed: true)
    ])
).value()
print(result.report.singularities,
      result.report.selectedCandidate,        // which cross field `.best` kept
      result.report.topologicallySymmetric)   // checked on the result, not assumed
for warning in result.guidanceWarnings { print(warning) }

// soft selection: paint a weight field, then move it, re-snapping to the Target
try quadMesh.selectSphere(SphereRegion(center: (0, 1, 0), radius: 0.4))
try quadMesh.transformSelection(identityAffine, snapper: snapper)

// UV Path tool
let seams = try SeamSet()
let path = try SeamPath(mesh: quadMesh)
path.addWaypoint(startVertex)
path.addWaypoint(endVertex)
let undoRecord = path.commit(into: seams)

// PencilKit / UIKit stroke -> gesture grammar
let stroke = forwarder.stroke(from: pkStroke, in: canvasView)
let interpretation = try StrokeInterpretation(
    stroke: stroke, mesh: quadMesh, viewProjection: renderer.viewProjection)
if interpretation.action(0) == .insertLoop {
    // apply it with the cyber_retopo_* ops
}
```

## ZRemesher parity

`QuadMethod.zremesher` reaches the method through plain `cyber_remesh`, which
carries no ZRemesher controls and returns no report.
`remesh(params:zremesher:guidance:)` is the parity entry point
(`cyber_remesh_zremesher`): everything the headless CLI can ask of the
ZRemesher path is reachable there, and it is the only path that forwards
`adaptivity` the way the CLI always has.

`targetQuads` names the **whole** model under `symmetry`, so the half is solved
for half of it.

Unknown values are rejected, never reinterpreted — an unknown quality mode,
symmetry axis or guide mode throws `.invalidParameter` rather than quietly
becoming something else. A symmetry axis clamped to `.none` would hand back an
asymmetric mesh for a symmetry request.

`Tests/CyberRemesherTests` asserts these at runtime against the same properties
the Python binding gates assert, so the two surfaces cannot drift apart
silently. `swift build` alone never caught a marshalling bug: a pointer that
dies before the engine reads it, a `char[32]` decoded wrong and a dropped guide
mode all compile perfectly.

## What the C ABI does *not* provide

Two areas the package used to claim, which have no entry points in
`cyber_capi.h` and are therefore not bound:

* **A session / document object.** There is no `cyber_session_*` family. The ABI
  is handle-based: the caller owns the `Mesh`, the undo stack (`Mesh.clone()`
  preserves element ids, which is what makes it a usable snapshot) and the tool
  state. Gesture interpretation is a pure function of one completed stroke —
  `cyber_stroke_interpret` — wrapped by `StrokeInterpretation`.
* **Viewport / Metal attachment.** There is no render backend across the ABI.
  What it exposes instead are the zero-copy render-buffer views
  (`cyber_mesh_positions_ptr` and friends): a renderer owns its own
  `CAMetalLayer`, device and queue, and uploads from those pointers. Their
  lifetime contract — valid until the next mutating call on that handle — is
  documented in the header.

Two more places where the Swift surface is a composition, not a 1:1 binding:

* `Mesh(positions:indices:)` has no bulk-topology entry point behind it. It
  builds faces one at a time with `cyber_retopo_build_face`, mapping each source
  index to the engine vertex id the first face using it created. That is
  O(triangles) ABI calls — use `Mesh.loadOBJ(_:)` for real assets — and it
  inherits the op's rejection of degenerate triangles.
* `CyberRuntime` reports the engine's semantic version (`cyber_version`). There
  is no separate ABI-version symbol; format compatibility is negotiated per
  format (e.g. `CYBER_HANDOFF_VERSION_*`, which fails an unsupported handoff
  file with `CYBER_ERR_INCOMPATIBLE_VERSION`).
