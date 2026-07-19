#!/usr/bin/env python3
"""Golden-baseline integration suite for the cyberremesh bindings.

Mirrors the C++ golden suite, but drives the pipeline through the Python
ctypes bindings so the *packaged* surface (Mesh / RemeshParams / remesh /
Statistics) is exercised end-to-end.

Runnable as a plain script (no pytest / unittest required):

    python python/cyberremesh/tests/test_golden.py

Behaviour (matches ``test_api.py``'s capability gate):
  * ``import cyberremesh`` must always succeed, even when the engine was never
    built — it never dlopens anything at import time.
  * If the C ABI shared library cannot be located/loaded, prints ``SKIP`` and
    exits 77 (CTest SKIP). Safe to run unconditionally in CI and locally.
  * Otherwise builds small procedural OBJ meshes (a cube and a UV-sphere
    approximation), remeshes them through the bindings and asserts invariant
    baselines: the result is quad-dominant (``stats.quads > 0``), the run is
    deterministic across two invocations, and vertex/face counts fall inside a
    sane band relative to the requested target.
"""

import math
import os
import sys
import tempfile

# Make the package importable when run directly from a source checkout.
_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh
from cyberremesh import Mesh, RemeshParams, Statistics, remesh

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


def _uv_sphere_obj(stacks=12, slices=16, radius=1.0):
    """Return an OBJ string for a UV-sphere with triangle caps + quad body.

    Poles are single vertices fanned with triangles; the interior rings are
    joined by quads. This is a deliberately procedural, deterministic mesh so
    the golden baselines below are reproducible without any asset on disk.
    """
    verts = []
    verts.append((0.0, radius, 0.0))  # north pole == index 1 (OBJ is 1-based)
    for i in range(1, stacks):
        phi = math.pi * i / stacks
        y = radius * math.cos(phi)
        r = radius * math.sin(phi)
        for j in range(slices):
            theta = 2.0 * math.pi * j / slices
            verts.append((r * math.cos(theta), y, r * math.sin(theta)))
    verts.append((0.0, -radius, 0.0))  # south pole

    lines = ["v {0:.6f} {1:.6f} {2:.6f}".format(*v) for v in verts]

    def ring(i):
        # 1-based OBJ index of the first vertex on interior ring i (1..stacks-1).
        return 2 + (i - 1) * slices

    south = len(verts)  # 1-based index of the south pole (last vertex)

    # North cap: fan of triangles from the pole to the first ring.
    for j in range(slices):
        a = ring(1) + j
        b = ring(1) + (j + 1) % slices
        lines.append("f 1 {0} {1}".format(a, b))

    # Body: quads between consecutive interior rings.
    for i in range(1, stacks - 1):
        for j in range(slices):
            a = ring(i) + j
            b = ring(i) + (j + 1) % slices
            c = ring(i + 1) + (j + 1) % slices
            d = ring(i + 1) + j
            lines.append("f {0} {1} {2} {3}".format(a, b, c, d))

    # South cap: fan of triangles from the last ring to the pole.
    last = stacks - 1
    for j in range(slices):
        a = ring(last) + j
        b = ring(last) + (j + 1) % slices
        lines.append("f {0} {1} {2}".format(a, south, b))

    return "\n".join(lines) + "\n"


def _write_obj(tmpdir, name, text):
    path = os.path.join(tmpdir, name)
    with open(path, "w") as fh:
        fh.write(text)
    return path


def _stats_tuple(stats):
    """Order-stable tuple of the Statistics counters for equality checks."""
    assert isinstance(stats, Statistics), stats
    return (
        stats.vertices,
        stats.quads,
        stats.triangles,
        stats.other_polygons,
        stats.islands,
        stats.islands_failed,
    )


def _remesh_stats(obj_path, params):
    """Load ``obj_path``, remesh with ``params`` and return the result stats."""
    with Mesh.load_obj(obj_path) as mesh:
        result = remesh(mesh, params)
        with result:
            assert result.stats is not None, "remesh must populate stats"
            return result.stats


def _golden_cube(tmpdir):
    obj_path = _write_obj(tmpdir, "cube.obj", _CUBE_OBJ)
    params = RemeshParams(target_quad_count=200)

    stats = _remesh_stats(obj_path, params)

    # Invariant: quad-dominant output.
    assert stats.quads > 0, ("cube must remesh to quads", stats)
    assert stats.quads >= stats.triangles, ("expected quad-dominant", stats)
    # Invariant: a closed genus-0 surface yields a single island, no failures.
    assert stats.islands_failed == 0, ("no island should fail", stats)
    # Invariant: vertex count is positive and finite.
    assert stats.vertices > 0, ("cube must have vertices", stats)
    print("PASS golden cube: {0} quads, {1} verts, {2} islands".format(
        stats.quads, stats.vertices, stats.islands))


def _golden_sphere(tmpdir):
    obj_path = _write_obj(tmpdir, "sphere.obj", _uv_sphere_obj())
    target = 500
    params = RemeshParams(target_quad_count=target)

    stats = _remesh_stats(obj_path, params)

    assert stats.quads > 0, ("sphere must remesh to quads", stats)
    assert stats.quads >= stats.triangles, ("expected quad-dominant", stats)
    assert stats.islands_failed == 0, ("no island should fail", stats)

    # Invariant band: total face count stays within a generous factor of the
    # requested target. The engine is free to over/under-shoot for topology, so
    # the band is intentionally wide (0.1x .. 10x) — it catches gross blowups,
    # not tuning drift.
    faces = stats.quads + stats.triangles + stats.other_polygons
    assert faces >= target * 0.1, ("face count far below target", faces, target)
    assert faces <= target * 10, ("face count far above target", faces, target)
    print("PASS golden sphere: {0} faces (target {1}), {2} quads".format(
        faces, target, stats.quads))


def _determinism(tmpdir):
    """Two identical runs on the same input must produce identical stats."""
    obj_path = _write_obj(tmpdir, "cube_det.obj", _CUBE_OBJ)
    params = RemeshParams(target_quad_count=300)

    first = _stats_tuple(_remesh_stats(obj_path, params))
    second = _stats_tuple(_remesh_stats(obj_path, params))

    assert first == second, ("remesh must be deterministic", first, second)
    print("PASS determinism: identical stats across two runs {0}".format(first))


def main():
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable "
              "(set CYBER_CAPI_LIB or build the `capi` module)")
        return 77  # CTest SKIP_RETURN_CODE — reported as Skipped, never a vacuous pass

    print("cyberremesh engine version: {0}".format(cyberremesh.version()))
    tmpdir = tempfile.mkdtemp(prefix="cyberremesh_golden_")
    _golden_cube(tmpdir)
    _golden_sphere(tmpdir)
    _determinism(tmpdir)
    print("ALL GOLDEN CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
