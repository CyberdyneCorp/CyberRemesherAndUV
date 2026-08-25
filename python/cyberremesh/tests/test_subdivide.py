#!/usr/bin/env python3
"""Subdivision through the bindings (engine-bindings spec: "Subdivision binding").

Runnable as a plain script (no pytest/unittest required):

    python python/cyberremesh/tests/test_subdivide.py

Skips with 77 (CTest SKIP) when the C ABI shared library is not loadable.
"""

import math
import os
import sys
import tempfile

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh
from cyberremesh import CyberError, Mesh, Subdivision

# A unit cube: 8 verts, 6 quad faces.
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

# A single triangle: subdivision turns an n-gon into n quads, so this becomes 3.
_TRI_OBJ = """\
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
"""


# An OPEN 2x2 quad patch in the z = 0 plane: every border edge is a boundary
# edge, which is what the crease rule has to hold on to.
_PATCH_OBJ = """\
v 0 0 0
v 1 0 0
v 2 0 0
v 0 1 0
v 1 1 0
v 2 1 0
v 0 2 0
v 1 2 0
v 2 2 0
f 1 2 5 4
f 2 3 6 5
f 4 5 8 7
f 5 6 9 8
"""


def _write(tmpdir, name, text):
    path = os.path.join(tmpdir, name)
    with open(path, "w") as fh:
        fh.write(text)
    return path


def _test_quadruples_a_quad_mesh(tmpdir):
    path = _write(tmpdir, "cube.obj", _CUBE_OBJ)
    with Mesh.load(path) as mesh:
        assert mesh.face_count == 6, mesh.face_count

        faces = mesh.subdivide()

        # Every quad splits into 4 around the face centroid.
        assert faces == 24, faces
        assert mesh.face_count == 24, mesh.face_count
        # 8 corners + 12 edge midpoints + 6 face centroids.
        assert mesh.vertex_count == 26, mesh.vertex_count
        # The returned count is the mesh's own, not a stale copy.
        assert faces == mesh.face_count

        # Subdivision is repeatable: ids are rebuilt, but the handle stays live.
        assert mesh.subdivide() == 96, mesh.face_count
    print("PASS: quad mesh 6 -> 24 -> 96 faces")


def _test_splits_ngons_into_n_quads(tmpdir):
    path = _write(tmpdir, "tri.obj", _TRI_OBJ)
    with Mesh.load(path) as mesh:
        assert mesh.face_count == 1
        assert mesh.subdivide() == 3, mesh.face_count
    print("PASS: triangle -> 3 quads")


def _test_subdivision_alone_stays_on_the_facets(tmpdir):
    """Linear subdivision adds vertices but no curvature: every new vertex sits
    on the cube's flat faces, so the bounding box cannot grow or shrink."""
    path = _write(tmpdir, "cube_flat.obj", _CUBE_OBJ)
    with Mesh.load(path) as mesh:
        mesh.subdivide()
        for x, y, z in _positions(mesh):
            assert max(abs(x), abs(y), abs(z)) <= 0.5 + 1e-5, (x, y, z)
    print("PASS: linear subdivision stays inside the original hull")


def _test_reprojection_moves_vertices_onto_the_target(tmpdir):
    """subdivide(project_to=...) is what recovers curvature. Projecting the
    cube's subdivision onto a sphere must pull the new vertices out to the
    sphere's surface — proof the snapper ran at all."""
    sphere_path = _write(tmpdir, "sphere.obj", _sphere_obj(radius=1.0))
    cube_path = _write(tmpdir, "cube_proj.obj", _CUBE_OBJ)

    with Mesh.load(sphere_path) as target, Mesh.load(cube_path) as mesh:
        mesh.subdivide(project_to=target)
        radii = [math.sqrt(x * x + y * y + z * z) for x, y, z in _positions(mesh)]

    # Un-projected, every vertex of the cube's subdivision is within 0.87 of the
    # origin (the corner distance). On the sphere they sit at radius ~1.
    assert min(radii) > 0.9, min(radii)
    assert max(radii) < 1.1, max(radii)
    print("PASS: reprojection put {0} vertices on the target surface".format(len(radii)))


def _test_catmull_clark_rounds_the_cube_without_a_target(tmpdir):
    """The smooth mode is what makes a Target optional. Linear leaves the cube's
    corners at radius 0.866 and its face centres at 0.5 — a faceted shell.
    Catmull-Clark pulls every vertex into the band [0.481, 0.531], so the
    radius spread drops from 0.517 of the mean to 0.097: a 5.3x rounder shape
    after a single level, with no snapper involved."""
    path = _write(tmpdir, "cube_cc.obj", _CUBE_OBJ)
    with Mesh.load(path) as mesh:
        assert mesh.subdivide(mode=Subdivision.CATMULL_CLARK) == 24
        radii = [math.sqrt(x * x + y * y + z * z) for x, y, z in _positions(mesh)]
    assert min(radii) > 0.48 and min(radii) < 0.49, min(radii)
    assert max(radii) > 0.52 and max(radii) < 0.54, max(radii)
    spread = (max(radii) - min(radii)) / (sum(radii) / len(radii))
    assert spread < 0.11, spread

    # The default is still linear, and still leaves everything on the facets.
    path = _write(tmpdir, "cube_default.obj", _CUBE_OBJ)
    with Mesh.load(path) as mesh:
        mesh.subdivide()
        linear = [math.sqrt(x * x + y * y + z * z) for x, y, z in _positions(mesh)]
    linear_spread = (max(linear) - min(linear)) / (sum(linear) / len(linear))
    assert linear_spread > 0.5, linear_spread
    assert spread < linear_spread / 5.0, (spread, linear_spread)
    print("PASS: Catmull-Clark radius spread {0:.3f} vs linear {1:.3f}".format(
        spread, linear_spread))


def _test_catmull_clark_keeps_an_open_border(tmpdir):
    """A boundary edge takes the sharp-crease rule, so an open patch keeps its
    outline instead of shrinking inward."""
    path = _write(tmpdir, "patch.obj", _PATCH_OBJ)
    with Mesh.load(path) as mesh:
        for _ in range(3):
            mesh.subdivide(mode=Subdivision.CATMULL_CLARK)
        xs = [p[0] for p in _positions(mesh)]
        ys = [p[1] for p in _positions(mesh)]
        zs = [p[2] for p in _positions(mesh)]
    assert abs(min(xs)) < 1e-5 and abs(max(xs) - 2.0) < 1e-5, (min(xs), max(xs))
    assert abs(min(ys)) < 1e-5 and abs(max(ys) - 2.0) < 1e-5, (min(ys), max(ys))
    assert max(abs(z) for z in zs) < 1e-5
    print("PASS: open patch kept its 2x2 outline through 3 smooth levels")


def _test_unknown_mode_is_rejected(tmpdir):
    path = _write(tmpdir, "cube_mode.obj", _CUBE_OBJ)
    with Mesh.load(path) as mesh:
        try:
            mesh.subdivide(mode=7)
        except CyberError as exc:
            print("PASS: unknown subdivision mode rejected ({0})".format(exc))
        else:
            raise AssertionError("expected CyberError for an unknown subdivision mode")
        # The mesh is untouched by the rejected call.
        assert mesh.face_count == 6, mesh.face_count


def _test_empty_mesh_fails_loudly():
    with Mesh() as mesh:
        try:
            mesh.subdivide()
        except CyberError as exc:
            print("PASS: empty mesh rejected ({0})".format(exc))
        else:
            raise AssertionError("expected CyberError subdividing a mesh with no faces")


def _positions(mesh):
    """Vertex positions as (x, y, z) tuples, without requiring NumPy."""
    flat = mesh._copy_positions()
    return [tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)]


def _sphere_obj(radius, rings=12, segments=16):
    """A UV sphere as OBJ text — the projection target."""
    lines, faces = [], []
    for i in range(rings + 1):
        theta = math.pi * i / rings
        for j in range(segments):
            phi = 2.0 * math.pi * j / segments
            lines.append("v {0} {1} {2}".format(
                radius * math.sin(theta) * math.cos(phi),
                radius * math.cos(theta),
                radius * math.sin(theta) * math.sin(phi)))
    for i in range(rings):
        for j in range(segments):
            a = i * segments + j + 1
            b = i * segments + (j + 1) % segments + 1
            faces.append("f {0} {1} {2} {3}".format(a, b, b + segments, a + segments))
    return "\n".join(lines + faces) + "\n"


def main():
    assert hasattr(Mesh, "subdivide"), "Mesh.subdivide is missing from the binding"

    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable "
              "(set CYBER_CAPI_LIB or build the `capi` module)")
        return 77

    tmpdir = tempfile.mkdtemp(prefix="cyberremesh_subdiv_")
    _test_quadruples_a_quad_mesh(tmpdir)
    _test_splits_ngons_into_n_quads(tmpdir)
    _test_subdivision_alone_stays_on_the_facets(tmpdir)
    _test_reprojection_moves_vertices_onto_the_target(tmpdir)
    _test_catmull_clark_rounds_the_cube_without_a_target(tmpdir)
    _test_catmull_clark_keeps_an_open_border(tmpdir)
    _test_unknown_mode_is_rejected(tmpdir)
    _test_empty_mesh_fails_loudly()
    return 0


if __name__ == "__main__":
    sys.exit(main())
