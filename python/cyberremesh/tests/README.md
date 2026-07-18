# cyberremesh binding tests

Integration tests for the `cyberremesh` ctypes bindings. Each file is a plain,
dependency-free Python script (no pytest / unittest) that takes no required
arguments and can be run directly:

```sh
python python/cyberremesh/tests/test_api.py
python python/cyberremesh/tests/test_golden.py
python python/cyberremesh/tests/test_document_invariants.py
```

| File | Roadmap | Covers |
|------|---------|--------|
| `test_api.py` | — | Import contract + a single load/remesh/save smoke run. |
| `test_golden.py` | 13.3 | Golden baselines through the bindings: quad-dominant output, determinism across two runs, vertex/face counts within a band. Mirrors the C++ golden suite via Python. |
| `test_document_invariants.py` | 13.3 | Mesh lifecycle round-trip (load→save→reload counts equal), monotonic progress reaching ~1.0, cancel raising `CANCELLED`, and NumPy `positions` shape/dtype. |

## Capability-coverage gate (CI intent)

Every script is a **capability-gated** integration test, not a pure unit test:

1. `import cyberremesh` must always succeed — the package never dlopens the
   shared library at import time, so these files import cleanly on a machine
   that has never built the engine.
2. If `cyberremesh.is_available()` is `False` (the `libcyber_capi_shared`
   library cannot be located or loaded), the script prints `SKIP …` and exits
   `0`.

This means CI can invoke every file unconditionally: on a runner where the
`capi` module was built, the assertions execute and gate the pipeline's
behaviour; on a runner without it, the scripts self-skip green instead of
failing spuriously. Point the loader at a built library with the
`CYBER_CAPI_LIB` environment variable (a full path to the `.so`/`.dylib`/`.dll`
or a directory containing it) when it is not discoverable in a conventional
in-tree build directory.

Optional dependencies degrade the same way: the NumPy `positions` check inside
`test_document_invariants.py` self-skips (printing `SKIP numpy positions`) when
NumPy is not installed, so the file stays green without it.
