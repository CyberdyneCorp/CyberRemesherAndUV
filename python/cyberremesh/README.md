# cyberremesh

Python bindings for the **CyberRemesher** quad-remeshing engine.

These are thin, Pythonic `ctypes` bindings over the engine's versioned C ABI
(the `capi` module, shipped as `libcyber_capi.so` / `.dylib` /
`cyber_capi.dll`). No C++ type crosses the boundary — opaque handles,
plain C structs, status codes and C function-pointer callbacks only.

Importing the package never loads the shared library; that happens lazily on
the first engine call, so `import cyberremesh` works on a machine that has
never built the engine.

## Install

```sh
pip install ./python/cyberremesh
pip install "./python/cyberremesh[numpy]"   # optional ndarray helpers
```

## Locating the native library

At first use the loader searches, in order:

1. `CYBER_CAPI_LIB` — a full path to the shared library, or a directory holding it.
2. The installed package directory. A published wheel carries the library here —
   `packaging/publish/stage_native_lib.py` copies it in before the wheel is built,
   and `pyproject.toml`'s `package-data` glob keeps it. A wheel built without that
   step is pure Python and reaches this step with nothing to find.
3. Conventional in-tree build directories (`build/`, `out/`, `cmake-build-*`) relative to the
   repo root, including the per-preset trees CMake presets create (`build/<preset>/capi`).
   This step is a developer convenience and applies only when the package genuinely sits
   inside a checkout of this project — an ancestor must carry `CMakeLists.txt` and
   `capi/include/cyber_capi.h`, and neither it nor the build directory may be world-writable
   or owned by another user. An installed package (venv, `/opt`, a CI scratch tree) therefore
   never loads a build tree that merely happens to sit above it; use `CYBER_CAPI_LIB` to point
   at a library outside a checkout.
4. The platform loader search path, for a system-installed library (`cmake --install`).

## Usage

```python
from cyberremesh import Mesh, RemeshParams, remesh, CyberError

mesh = Mesh.load_obj("highpoly.obj")

def on_progress(fraction, stage):
    print(f"{stage}: {fraction:.0%}")

result = remesh(
    mesh,
    RemeshParams(target_quad_count=5000, pure_quads=True),
    progress=on_progress,
    cancel=lambda: False,
)
print(result.stats.quads, "quads")
result.save_obj("retopo.obj")

# With numpy installed:
#   arr = result.positions   # (n, 3) float32 snapshot
```

`RemeshParams` mirrors `cyber::remesh::Parameters` field-for-field; the engine
clamps out-of-range values and reports the clamps. Any non-OK C ABI status is
raised as `CyberError`, carrying the numeric status and the engine's
`cyber_last_error()` message.

### Export presets

`--preset` is not CLI-only: the whole preset surface is bound.

```python
from cyberremesh import ExportPreset, builtin_presets, write_bundle

builtin_presets()                                # ['blender', 'unity', ...]
with ExportPreset.resolve("blender") as preset:  # or a path to a preset JSON
    preset.resolution = 1024
    [m.map for m in preset.maps]                 # ['normal', 'ao', ...]
    bundle = write_bundle(low, high, preset, "out/hero.obj")
bundle.files                                     # mesh + one file per map
bundle.warnings                                  # never silently dropped
```

`write_bundle` unwraps `low` **in place** when it carries no UVs (baking is
impossible without them) — pass `low.copy()` to keep the original. Listing,
resolving and reading a preset works in every build; only `write_bundle` needs
the UV-gated bundle module, and raises `CyberError` naming it where absent. A
preset from an unsupported schema raises `IncompatibleVersionError`.

### Quadrangulator choice

`quad_method` selects how triangles become quads:

- `"quad-cover"` (default) — a QuadCover seamless-UV isoline extractor. Beats
  QuadriFlow on both median quad angle and irregular-vertex count on 3 of the 5
  corpus models (spot, rocker-arm, stanford-bunny), losing fandisk and
  cheburashka, and stays manifold on flat CAD input where QuadriFlow tears.
- `"field-aligned"` — maximum-matching over a smoothed cross field.
  Highest quad-dominance (~95%+ on clean input) with curvature-following flow.
- `"instant-meshes"` — the Instant-Meshes-style **position-field extractor**
  (per-vertex 4-RoSy orientation + lattice position field, collapse, extract).
  More uniform, field-aligned edge flow with fewer, better-placed singularities;
  it matches QuadriFlow on edge-length uniformity. Best for organic/scanned
  surfaces where flow matters more than raw dominance.
- `"integer"` — the experimental integer-parametrization extractor
  (watertight/manifold; degrades at coarse target counts).

```python
RemeshParams(target_quad_count=5000, pure_quads=True, quad_method="instant-meshes")
```

## Tests

`tests/test_api.py` runs as a plain script. When the native library is absent it
exits **77** — CTest's `SKIP_RETURN_CODE`, which `tests/CMakeLists.txt` sets — so
the run shows as "Skipped" rather than a vacuous pass. Treat 0 as pass, 77 as
skip and anything else as failure:

```sh
python python/cyberremesh/tests/test_api.py
```
