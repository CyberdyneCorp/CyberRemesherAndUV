#!/usr/bin/env python3
"""Integration test for the cyberremesh bindings.

Runnable as a plain script (no pytest/unittest required):

    python python/cyberremesh/tests/test_api.py

Behaviour:
  * Imports the package WITHOUT dlopening anything — this must always succeed,
    even on a machine that never built the engine.
  * If the C ABI shared library cannot be found, prints ``SKIP`` and exits 0
    (this is an integration test, gated on the built `capi` module).
  * Otherwise loads a temporary cube OBJ, remeshes it and asserts that the
    result contains quads.
"""

import os
import sys
import tempfile

# Make the package importable when run directly from a source checkout.
_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh
from cyberremesh import CyberError, Mesh, RemeshParams, remesh

# A unit cube as an OBJ (8 verts, 6 quad faces).
_CUBE_OBJ = """\
v -0.5 -0.5 -0.5
v  0.5 -0.5 -0.5
v  0.5  0.5 -0.5
v -0.5  0.5 -0.5
v -0.5 -0.5  0.5
v  0.5 -0.5  0.5
v  0.5  0.5  0.5
v -0.5  0.5  0.5
f 1 2 3 4
f 5 8 7 6
f 1 5 6 2
f 2 6 7 3
f 3 7 8 4
f 4 8 5 1
"""


def _check_import_contract():
    # Import-time must not have loaded the shared library.
    assert hasattr(cyberremesh, "Mesh")
    assert hasattr(cyberremesh, "RemeshParams")
    assert hasattr(cyberremesh, "remesh")
    assert hasattr(cyberremesh, "CyberError")
    assert issubclass(CyberError, RuntimeError)


def _run_remesh():
    tmpdir = tempfile.mkdtemp(prefix="cyberremesh_test_")
    obj_path = os.path.join(tmpdir, "cube.obj")
    out_path = os.path.join(tmpdir, "out.obj")
    with open(obj_path, "w") as fh:
        fh.write(_CUBE_OBJ)

    progress_seen = []

    def on_progress(fraction, stage):
        progress_seen.append((fraction, stage))

    with Mesh.load_obj(obj_path) as mesh:
        assert mesh.vertex_count == 8, mesh.vertex_count
        result = remesh(
            mesh,
            RemeshParams(target_quad_count=200),
            progress=on_progress,
            cancel=lambda: False,
        )
        with result:
            assert result.stats is not None
            assert result.stats.quads > 0, result.stats
            result.save_obj(out_path)

    assert os.path.isfile(out_path)
    print("PASS: remesh produced {0} quads".format(
        # re-report from a fresh load to prove the file round-trips
        _quads_in(out_path)
    ))


def _quads_in(path):
    quads = 0
    with open(path) as fh:
        for line in fh:
            if line.startswith("f ") and len(line.split()) == 5:
                quads += 1
    return quads


def main():
    _check_import_contract()

    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not found "
              "(set CYBER_CAPI_LIB or build the `capi` module)")
        return 0

    print("cyberremesh engine version: {0}".format(cyberremesh.version()))
    _run_remesh()
    return 0


if __name__ == "__main__":
    sys.exit(main())
