# CyberRemesher — Swift package (SwiftPM)

Idiomatic Swift bindings for the CyberRemesher engine, for **iPadOS 15+** and
**macOS 12+**. This package wraps the versioned **C ABI facade** (`cyber_capi.h`)
produced by the `capi/` module — no C++ types cross the boundary. The first-party
iPadOS shell consumes exactly this package (no private hooks), so third parties
get the same surface: document/session control, all tools, UIKit/PencilKit event
forwarding, and viewport attachment to a `CAMetalLayer`.

> **UNVERIFIED / best-effort.** This package is **not built in headless Linux
> CI** — it needs a Swift toolchain (Xcode 15+) and the `cyber_capi` shared
> library. Every source file carries an `UNVERIFIED:` header. The code is
> written for real (no throwing stubs), but it has not been compiled. Covers
> tasks **13.4** and **13.5** (bootstrap-v1-platform).

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
      CyberRuntime.swift            # version / ABI-compat / last-error
      Mesh.swift                    # RAII wrapper over the opaque CyberMesh
      Session.swift                 # document/session + input injection
      Remesh.swift                  # async remesh + AsyncStream progress + cancel
      InputForwarding.swift         # UIKit + PencilKit event forwarding
      MetalViewport.swift           # CAMetalLayer attach (placeholder)
```

## Building / consuming

1. Build the engine + `capi/` module with CMake (`-DCYBER_BUILD_CAPI=ON`) to get
   `cyber_capi.h` and the `libcyber_capi` shared library.
2. Point the Swift build at them:
   - **Xcode app target**: add the package as a local SwiftPM dependency and set
     `HEADER_SEARCH_PATHS` to the capi `include/` dir and `LIBRARY_SEARCH_PATHS`
     to the built dylib. Drop the `unsafeFlags` in `Package.swift`.
   - **`swift build` from this dir**: `Package.swift` already adds
     `-I../src/capi/include` and links `cyber_capi`; add `-L` for the dylib via
     `swift build -Xlinker -L<path>`.

Because the `capi/` module is authored in parallel, the Swift layer is written
against the **assumed ABI contract** below. If capi lands with different
spellings, update this contract and the thin call sites — the Swift-facing API
(errors, `Mesh`, `Session`, `RemeshOperation`, forwarders) does not change.

## Usage sketch

```swift
import CyberRemesher

let mesh = try Mesh(positions: positions, indices: indices)

// async/await remesh with progress + cancellation
let op = mesh.remesh(params: .init(targetQuads: 5000, pureQuad: true))
let progressTask = Task { for await p in op.progress { updateBar(p) } }
let quadMesh = try await op.value()          // throws .cancelled if the Task cancels
progressTask.cancel()

// interactive session + PencilKit forwarding
let session = try Session(mesh: quadMesh)
let forwarder = TouchInputForwarder(session: session, view: canvasView)
try forwarder.forward(pencilStroke: stroke, in: canvasView)

// viewport
let viewport = MetalViewport(session: session, layer: metalLayer)
try viewport.attach()
```

## Assumed C ABI contract (`cyber_capi.h`)

The Swift layer expects these symbols. Types are plain C; handles are opaque.

```c
/* Status codes */
typedef enum CyberStatus {
    CYBER_STATUS_OK = 0,
    CYBER_STATUS_INVALID_ARGUMENT,
    CYBER_STATUS_OUT_OF_MEMORY,
    CYBER_STATUS_CANCELLED,
    CYBER_STATUS_IO,
    CYBER_STATUS_OUT_OF_RANGE,
    CYBER_STATUS_PIPELINE
} CyberStatus;

/* Runtime */
const char *cyber_version(void);
uint32_t    cyber_abi_version(void);       /* (major << 16) | minor */
const char *cyber_last_error_message(void);/* thread-local, may be NULL */

/* Callbacks */
typedef void (*CyberProgressCb)(double progress, const char *stage, void *user);
typedef int  (*CyberCancelCb)(void *user); /* return nonzero to request cancel */

/* Opaque handles */
typedef struct CyberMesh         CyberMesh;
typedef struct CyberRemeshParams CyberRemeshParams;
typedef struct CyberSession      CyberSession;

/* Mesh */
CyberStatus cyber_mesh_create_indexed(const float *positions, size_t vertexCount,
                                      const uint32_t *indices, size_t indexCount,
                                      CyberMesh **out);
void        cyber_mesh_destroy(CyberMesh *mesh);
size_t      cyber_mesh_vertex_count(const CyberMesh *mesh);
size_t      cyber_mesh_face_count(const CyberMesh *mesh);
size_t      cyber_mesh_index_count(const CyberMesh *mesh);
size_t      cyber_mesh_copy_positions(const CyberMesh *mesh, float *out, size_t cap);
size_t      cyber_mesh_copy_indices(const CyberMesh *mesh, uint32_t *out, size_t cap);
CyberStatus cyber_mesh_load(const char *path, CyberMesh **out);
CyberStatus cyber_mesh_save(const CyberMesh *mesh, const char *path);

/* Remesh params */
CyberRemeshParams *cyber_remesh_params_create(void);
void cyber_remesh_params_destroy(CyberRemeshParams *params);
void cyber_remesh_params_set_target_quads(CyberRemeshParams *params, size_t quads);
void cyber_remesh_params_set_pure_quad(CyberRemeshParams *params, int enabled);
void cyber_remesh_params_set_preserve_sharp(CyberRemeshParams *params, int enabled);
void cyber_remesh_params_set_sharp_angle(CyberRemeshParams *params, double degrees);

/* Remesh */
CyberStatus cyber_remesh(const CyberMesh *input, const CyberRemeshParams *params,
                         CyberProgressCb progressCb, void *progressUser,
                         CyberCancelCb cancelCb, void *cancelUser,
                         CyberMesh **out);

/* Session / document + input injection */
typedef struct CyberStrokeSample {
    double x, y;            /* normalized viewport coords, 0..1 */
    double pressure;        /* 0..1 */
    double altitude_angle;  /* radians */
    double azimuth_angle;   /* radians */
    double timestamp;       /* seconds */
} CyberStrokeSample;

CyberStatus cyber_session_create(const CyberMesh *mesh, CyberSession **out);
void        cyber_session_destroy(CyberSession *session);
CyberStatus cyber_session_snapshot_mesh(const CyberSession *session, CyberMesh **out);
CyberStatus cyber_session_inject_stroke(CyberSession *session,
                                        const CyberStrokeSample *samples, size_t count);
CyberStatus cyber_session_inject_tap(CyberSession *session, double x, double y);
CyberStatus cyber_session_inject_chord(CyberSession *session,
                                       const uint64_t *buttons, size_t count);

/* Metal viewport */
CyberStatus cyber_session_attach_metal_layer(CyberSession *session, const void *caMetalLayer);
CyberStatus cyber_session_set_drawable_size(CyberSession *session, double w, double h);
CyberStatus cyber_session_detach_metal_layer(CyberSession *session);
```

## Task 13.5 (iPadOS shell re-base) — status

The shell re-base, cross-binding parity checks, and publish lanes (PyPI /
SwiftPM tag) require the Apple toolchain and are tracked as best-effort here.
This package is the consumption path the shell builds on; the parity harness and
the SwiftPM release tag live in the app/CI layer, not in this directory.
