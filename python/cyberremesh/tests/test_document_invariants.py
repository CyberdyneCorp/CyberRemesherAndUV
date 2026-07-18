#!/usr/bin/env python3
"""Document / lifecycle invariant suite for the cyberremesh bindings.

Exercises the *document* semantics of the binding surface — mesh load/save
round-tripping, cooperative progress and cancel callbacks, and the optional
NumPy positions accessor — through the packaged Python API.

Runnable as a plain script (no pytest / unittest required):

    python python/cyberremesh/tests/test_document_invariants.py

Behaviour (matches ``test_api.py``'s capability gate):
  * ``import cyberremesh`` must always succeed even without a built engine.
  * If the C ABI shared library cannot be located/loaded, prints ``SKIP`` and
    exits 0 — safe to run unconditionally in CI and locally.
  * Otherwise asserts the following invariants:
      - load -> save -> reload preserves vertex and face counts (round-trip);
      - the progress callback fires with monotonically non-decreasing fractions
        that reach ~1.0 by the end of a run;
      - a cancel callback that returns True aborts the run, raising a
        ``CyberError`` carrying the ``CANCELLED`` status;
      - when NumPy is available, ``mesh.positions`` has shape
        ``(vertex_count, 3)`` and float32 dtype (skipped otherwise).
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
from cyberremesh import _ffi

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


def _write_cube(tmpdir, name="cube.obj"):
    path = os.path.join(tmpdir, name)
    with open(path, "w") as fh:
        fh.write(_CUBE_OBJ)
    return path


def _roundtrip(tmpdir):
    """load -> save -> reload must preserve vertex and face counts."""
    src = _write_cube(tmpdir, "rt_src.obj")
    dst = os.path.join(tmpdir, "rt_dst.obj")

    with Mesh.load_obj(src) as mesh:
        v0, f0 = mesh.vertex_count, mesh.face_count
        assert v0 == 8, ("cube must load 8 vertices", v0)
        assert f0 == 6, ("cube must load 6 faces", f0)
        mesh.save_obj(dst)

    with Mesh.load_obj(dst) as reloaded:
        v1, f1 = reloaded.vertex_count, reloaded.face_count

    assert (v0, f0) == (v1, f1), ("round-trip must preserve counts",
                                  (v0, f0), (v1, f1))
    print("PASS round-trip: {0} verts / {1} faces preserved".format(v1, f1))


def _progress_monotonic(tmpdir):
    """Progress fractions must be non-decreasing and reach ~1.0."""
    src = _write_cube(tmpdir, "prog.obj")
    seen = []

    def on_progress(fraction, stage):
        seen.append(float(fraction))

    with Mesh.load_obj(src) as mesh:
        result = remesh(
            mesh,
            RemeshParams(target_quad_count=300),
            progress=on_progress,
            cancel=lambda: False,
        )
        result.close()

    assert seen, "progress callback must fire at least once"
    for prev, cur in zip(seen, seen[1:]):
        assert cur >= prev - 1e-6, ("progress must be monotonic", prev, cur)
    for f in seen:
        assert -1e-6 <= f <= 1.0 + 1e-6, ("fraction out of [0,1]", f)
    assert max(seen) >= 0.999, ("progress must reach ~1.0", max(seen))
    print("PASS progress: {0} callbacks, peaked at {1:.3f}".format(
        len(seen), max(seen)))


def _cancel_aborts(tmpdir):
    """A cancel callback returning True must abort with CANCELLED."""
    src = _write_cube(tmpdir, "cancel.obj")

    with Mesh.load_obj(src) as mesh:
        try:
            result = remesh(
                mesh,
                RemeshParams(target_quad_count=100_000),
                cancel=lambda: True,  # request cancellation immediately
            )
        except CyberError as exc:
            assert exc.status == _ffi.STATUS_CANCELLED, (
                "cancel must raise CANCELLED, got", exc.status)
            print("PASS cancel: aborted with {0}".format(
                _ffi.status_name(exc.status)))
            return
        # If no exception, the run must at least not have produced a result the
        # contract promises to cancel — treat a completed run as a failure.
        result.close()
        raise AssertionError("cancel callback did not abort the remesh")


def _numpy_positions(tmpdir):
    """When NumPy is present, positions is an (n, 3) float32 array."""
    if not cyberremesh.HAVE_NUMPY:
        print("SKIP numpy positions: numpy not installed")
        return

    import numpy as np

    src = _write_cube(tmpdir, "np.obj")
    with Mesh.load_obj(src) as mesh:
        pos = mesh.positions
        assert isinstance(pos, np.ndarray), ("positions must be ndarray", type(pos))
        assert pos.dtype == np.float32, ("positions must be float32", pos.dtype)
        assert pos.shape == (mesh.vertex_count, 3), (
            "positions shape must be (vertex_count, 3)",
            pos.shape, mesh.vertex_count)
    print("PASS numpy positions: shape {0} dtype {1}".format(pos.shape, pos.dtype))


def main():
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not found "
              "(set CYBER_CAPI_LIB or build the `capi` module)")
        return 0

    print("cyberremesh engine version: {0}".format(cyberremesh.version()))
    tmpdir = tempfile.mkdtemp(prefix="cyberremesh_invariants_")
    _roundtrip(tmpdir)
    _progress_monotonic(tmpdir)
    _cancel_aborts(tmpdir)
    _numpy_positions(tmpdir)
    print("ALL DOCUMENT INVARIANTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
