# CyberRemesher — iPadOS shell (task 8.5)

> **UNVERIFIED / best-effort.** Requires Xcode 15+ / iPadOS 15+ SDK and the
> built `cyber_capi` library. Not built in CI; not wired into the repo CMake.
> Every `.swift` file starts with `UNVERIFIED:`.

A thin SwiftUI shell over the first-party **`CyberRemesher` Swift package**
(`swift/`). It contributes **no** engine logic: stroke recognition, chording and
tool routing all live in C++ (`src/app`) behind the package. The shell only
wires SwiftUI/PencilKit/Metal to the package types (`Session`,
`TouchInputForwarder`, `MetalViewport`, `RemeshOperation`).

## What this shell demonstrates (8.5)

- **Pencil/touch event feed** — `PencilCanvasView` (a `UIViewRepresentable`)
  captures `UITouch`/PencilKit strokes and forwards them via the package's
  `TouchInputForwarder` into the active `Session`. Fingers navigate, Pencil
  draws (chording) — the routing decision is the engine's; the shell only tags
  input source.
- **Backgrounding autosave** — `AppModel` observes SwiftUI `ScenePhase`; on
  `.background`/`.inactive` it snapshots the session and writes an autosave
  document synchronously before the app is suspended.
- **CAMetalLayer viewport host** — `MetalViewportView` hosts a `CAMetalLayer`
  and hands it to the package's `MetalViewport`, which attaches it to the
  engine render backend and forwards drawable-size changes.

It also renders the **8.6** shared UI (stage switcher, progress/cancel overlay,
Action Gallery toolbar) from `../shared/*.json`.

## Building

This is source-only scaffolding. To make it a runnable app:

1. Build the engine + `capi/` (`-DCYBER_BUILD_CAPI=ON`) to get `libcyber_capi`
   and `cyber_capi.h`.
2. Create an iOS App target in Xcode, add the local `swift/` package as a
   package dependency, and add these `Sources/` files to the app target.
3. Set `HEADER_SEARCH_PATHS` → capi `include/`, `LIBRARY_SEARCH_PATHS` → the
   built dylib (see `swift/README.md`).
4. Copy `../shared/stages.json` and `../shared/toolbar.default.json` into the
   app bundle (Copy Bundle Resources).

`Package.swift` here declares the shell logic as a library target that depends
on the local package, so the non-UIKit parts can be type-checked with
`swift build` on macOS; the SwiftUI/PencilKit views compile only inside an iOS
app target.
