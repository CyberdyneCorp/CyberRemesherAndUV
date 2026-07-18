# CyberRemesher — mobile shells (tasks 8.5 / 8.6)

> **UNVERIFIED / best-effort.** Nothing in this directory is built by the
> headless Linux CI, and it is **not wired into the repo CMake tree**. The
> iPadOS shell needs an Apple toolchain (Xcode 15+); the Android shell needs
> the Android SDK + NDK. Both are thin native shells over the same engine and
> have **not been compiled**. Every source file carries an `UNVERIFIED:`
> header. These are the first-party reference shells the roadmap calls for —
> structured for real, but treated as scaffolding until they get a first build
> on real toolchains.

The mobile philosophy (see `openspec/.../design.md` D6/D10): the engine is pure
C++20 with no UI toolkit. All stroke recognition, chording and tool routing live
**once**, in C++ (`src/app`), and are exposed through the versioned **C ABI**
(`capi/cyber_capi.h`). Each platform ships a *thin* shell that only:

1. forwards native pencil/touch/lifecycle events into the engine, and
2. hosts a GPU layer the engine renders into.

The iPadOS shell consumes the first-party **Swift package** (`swift/`) — the
same public surface third parties get, with **no private hooks**. The Android
shell talks to the C ABI directly through a small JNI shim.

## Layout

```
apps/mobile/
  README.md              # this file
  ipados/                # 8.5 — iPadOS SwiftUI shell (consumes swift/ package)
  android/               # 8.5 — Android Kotlin shell (JNI over capi)
  shared/                # 8.6 — cross-platform UI model + content (data + docs)
```

## Task coverage

| Task | Where |
|------|-------|
| 8.5 iPadOS: pencil/touch feed, backgrounding autosave, CAMetalLayer host | `ipados/` |
| 8.5 Android: touch feed, lifecycle autosave, Vulkan/GLSurface host | `android/` |
| 8.6 Stage switcher, long-op progress/cancel (atomic commit), Action Gallery + configurable toolbar, tutorial content, in-app log view | `shared/` (data-driven, consumed by both shells) |

## Why data-driven shared UI (8.6)

The stage switcher, the Action Gallery and the configurable toolbar are
described **once** as data (`shared/stages.json`, `shared/toolbar.default.json`)
and loaded by both shells, so a tool added to the engine surfaces identically on
iPad and Android without per-platform edits. The interaction contracts that are
*behavioural* rather than data (atomic progress commit, in-app log view) are
specified in `shared/README.md` and implemented natively per platform against
that contract.

## Relationship to other roadmap items

- **13.4 Swift package** (`swift/`) — the iPadOS consumption path. `ipados/`
  depends on it; it does not reach past it into the C ABI.
- **13.5** — re-basing the shell on the Swift package (done here by
  construction) plus publish lanes; the SwiftPM release tag lives in CI.
- **14.1 / 14.2** — packaging (iOS archive, Android AAB) and simulator/emulator
  boot smoke tests live in `.github/workflows/` + `packaging/`, not here.
