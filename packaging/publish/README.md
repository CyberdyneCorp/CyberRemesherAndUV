# Binding publish lanes (PyPI + SwiftPM)

Publish infrastructure for CyberRemesher's two language bindings — the Python
package (`cyberremesh` + the `cyberbridge` client) on PyPI and the Swift package
on SwiftPM. Implements OpenSpec change `bootstrap-v1-platform`, task **13.5**
(`engine-bindings`: publish lanes + cross-binding parity checks).

> **Status: best-effort / UNVERIFIED.** These lanes require GitHub-hosted
> platform runners (macOS/Windows), QEMU for the Linux aarch64 wheel, an Apple
> toolchain (Swift 5.9 / Xcode 15+) for the Swift package, a native
> `cyber_capi` library on every target, and registry credentials (PyPI Trusted
> Publisher / token). None of that is available in the headless Linux core CI
> validated by `.github/workflows/ci.yml`, so the workflows here cannot be
> exercised in this repo's default job. Each workflow carries an `UNVERIFIED:`
> header naming what it needs. The lanes are **additive** to `ci.yml` (which is
> unchanged) and independent of the platform-artifact `release.yml` lane.

## Workflows

| File | Trigger | Does |
|------|---------|------|
| `.github/workflows/publish-python.yml` | `py-v*` tag / dispatch | Builds `cyberremesh` wheels with cibuildwheel across the wheel matrix, builds sdists + the pure-Python `cyberbridge` dist, runs the parity gate, publishes to PyPI. |
| `.github/workflows/publish-swift.yml` | `swift-v*` tag / dispatch | Validates the SwiftPM manifest + builds the package on macOS, runs the parity gate, and (best-effort) pushes the release tag consumers resolve by. |

Each binding releases on its **own** tag namespace so the three lanes never
contend for one trigger:

| Tag | Lane |
|-----|------|
| `v*`        | platform artifacts + GitHub Release (`release.yml`) |
| `py-v*`     | PyPI wheels/sdists (`publish-python.yml`) |
| `swift-v*`  | SwiftPM release tag (`publish-swift.yml`) |

## PyPI (Python bindings)

Wheels are built with [`cibuildwheel`](https://cibuildwheel.pypa.io) so the
native `cyber_capi` shared library the ctypes layer (`cyberremesh/_ffi.py`)
loads is compiled once per wheel platform (`CIBW_BEFORE_ALL`) and shipped inside
the wheel. Matrix:

| Platform | Arch(s) |
|----------|---------|
| macOS    | `arm64` (macos-14), `x86_64` (macos-13) |
| Windows  | `x86_64` (`AMD64`) |
| Linux    | `x86_64`, `aarch64` (QEMU) |

`cyberbridge` is pure Python and ships as a single sdist + wheel. `cyberremesh`
also ships an sdist for source installs. `cyberremesh` and `cyberbridge` are
version-locked — the `version` job fails if the two `pyproject.toml` versions
disagree, and asserts the `py-v<version>` tag matches.

**Publishing** uses [PyPI Trusted Publishing (OIDC)][tp] when the project is
configured for it (`id-token: write`, `environment: pypi`); it falls back to the
`PYPI_API_TOKEN` secret otherwise. `skip-existing` makes re-runs idempotent.

[tp]: https://docs.pypi.org/trusted-publishers/

### Required secrets / config (PyPI)

| Secret / config | Used by |
|-----------------|---------|
| PyPI Trusted Publisher for this repo+workflow (preferred) | OIDC upload, no long-lived token |
| `PYPI_API_TOKEN` | fallback token upload when OIDC is not configured |

## SwiftPM (Swift package)

SwiftPM has **no central registry upload** — "publishing" a Swift package means
publishing a git **release tag** that consumers resolve by semantic version:

```swift
.package(url: "https://github.com/<owner>/CyberRemesherAndUV.git", from: "0.1.0")
```

`publish-swift.yml` therefore, on macOS:

1. builds the native `cyber_capi` library the package links,
2. validates the manifest (`swift package dump-package`) and compiles the
   package (`swift build -c release`) against the C ABI header + built library,
3. runs the parity gate, then
4. best-effort creates + pushes the `swift-v*` release tag (a no-op when the run
   was itself triggered by that tag) and attaches a GitHub Release pointing at
   the tag.

## Parity checks for both bindings

Both bindings wrap **one** C ABI — the versioned facade declared in
`capi/include/cyber_capi.h` (`cyber_mesh_*`, `cyber_remesh*`, `cyber_session_*`,
`cyber_version`, `cyber_last_error`, …). The product guarantee (see the
`engine-bindings` spec) is that the Python and Swift wrappers expose the **same
ABI surface**, so third parties get identical capability on desktop and iPad.
Both publish lanes gate on a parity check enforcing this:

- **Header is the source of truth.** `capi/include/cyber_capi.h` defines the
  symbol surface.
- **Python** (`publish-python.yml` `parity` job): every `cyber_*` symbol the
  ctypes layer (`cyberremesh/_ffi.py`) binds must exist in the header — a
  binding that resolves a symbol the ABI no longer exports fails the lane.
- **Swift** (`publish-swift.yml` parity step): the system-library shim
  (`swift/Sources/CCyberRemesher/shim.h`) must `#include <cyber_capi.h>`, i.e.
  the Swift surface is generated from the same header, not a hand-copied subset.

This catches the two ways the bindings drift: a wrapper binding a stale/renamed
symbol, or one binding pointing at a different header than the other. A richer
future harness can diff the *typed* signatures (arity + argument types) across
`_ffi.py` and the generated Swift interface; today's gate enforces the symbol
surface + single-header provenance, which is what keeps the two wrappers ABI
compatible.

## Relationship to the iPad shell (task 13.5, partial)

Task 13.5 also calls for **re-basing the first-party iPadOS shell on the Swift
package**. That shell is tracked as best-effort under the mobile app layer and
is not yet present, so the re-base remains **partial**: this directory ships the
publish + parity infrastructure and the Swift package (`swift/`) is the
consumption path the shell will build on, but the shell-on-package migration
lands with the iPad shell itself.

## Local dry runs

On a matching host, without touching any registry:

```console
# Python: build wheels for the local platform only (no upload)
$ pipx run cibuildwheel --output-dir wheelhouse python/cyberremesh

# Swift (macOS): validate the manifest + build against a local capi build
$ cmake -S . -B build/swift -DCYBER_BUILD_CAPI=ON -DCYBER_BUILD_TESTS=OFF
$ cmake --build build/swift --target cyber_capi_shared
$ cd swift && swift build -c release \
    -Xcc -I"$PWD/../capi/include" -Xlinker -L"$PWD/../build/swift/capi"
```
