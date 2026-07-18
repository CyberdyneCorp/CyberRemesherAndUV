# cyberremesh

Python bindings for the **CyberRemesher** quad-remeshing engine.

These are thin, Pythonic `ctypes` bindings over the engine's versioned C ABI
(the `capi` module, shipped as `libcyber_capi_shared.so` / `.dylib` /
`cyber_capi_shared.dll`). No C++ type crosses the boundary — opaque handles,
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
2. The installed package directory (a wheel bundles the library alongside).
3. Conventional in-tree build directories (`build/`, `out/`, `cmake-build-*`) relative to the repo root.

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

## Tests

`tests/test_api.py` runs as a plain script. It skips (exit 0) when the native
library is absent, so it is safe in an environment where the `capi` module has
not been built:

```sh
python python/cyberremesh/tests/test_api.py
```
