#!/usr/bin/env python3
"""Loop subdivision through the bindings (engine-bindings spec).

The bindings could densify a triangle mesh only by turning it into quads:
`Mesh.subdivide` is Catmull-Clark topology, so a triangle came back as three
quads and there was no triangles-in-triangles-out path at all. These assert the
new one, and in particular that the two modes are not interchangeable.

Runnable as a plain script (no pytest/unittest required):

    python python/cyberremesh/tests/test_loop_subdivide.py

Skips with 77 (CTest SKIP) when the C ABI shared library is not loadable.
"""

import os
import sys
import tempfile

_PKG_PARENT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _PKG_PARENT not in sys.path:
    sys.path.insert(0, _PKG_PARENT)

import cyberremesh  # noqa: E402

# A tetrahedron: 4 vertices, 4 triangles, closed (every vertex interior).
_TET_OBJ = """\
v 0 0 0
v 1 0 0
v 0 1 0
v 0 0 1
f 1 3 2
f 1 2 4
f 2 3 4
f 3 1 4
"""

# A unit cube of six quads — a legal mesh, but not one Loop is defined on.
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

# Two triangles forming a flat square in z = 0 — an OPEN mesh, so the whole
# thing is boundary.
_FLAT_OBJ = """\
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
f 1 2 3
f 1 3 4
"""


def _write(tmpdir, name, text):
    path = os.path.join(tmpdir, name)
    with open(path, "w") as fh:
        fh.write(text)
    return path


def _positions(mesh):
    """Live vertex positions, rounded so float noise cannot fake a match.

    Ids are compact right after a load or a rebuild, but vertex_position
    returns None for a dead id, so the None guard stays.
    """
    out = []
    for i in range(mesh.vertex_count):
        p = mesh.vertex_position(i)
        if p is not None:
            out.append(tuple(round(c, 6) for c in p))
    return sorted(out)


def _test_quadruples_triangle_count(tmpdir):
    from cyberremesh import LoopSubdivideMode, Mesh

    path = _write(tmpdir, "tet.obj", _TET_OBJ)
    for mode in (LoopSubdivideMode.SMOOTH, LoopSubdivideMode.LINEAR):
        with Mesh.load(path) as mesh:
            assert mesh.face_count == 4, mesh.face_count
            faces = mesh.loop_subdivide(mode)
            # 1 triangle -> 4 triangles, not 3 quads (which is what subdivide
            # would have produced: 12).
            assert faces == 16, (mode, faces)
            assert mesh.face_count == 16, mesh.face_count
            # 4 originals + 6 edge points.
            assert mesh.vertex_count == 10, mesh.vertex_count
            # Triangles out, so it feeds back in.
            assert mesh.loop_subdivide(mode) == 64
    print("PASS: 4 triangles -> 16 -> 64, in both modes")


def _test_linear_leaves_the_shape_alone(tmpdir):
    from cyberremesh import LoopSubdivideMode, Mesh

    path = _write(tmpdir, "tet.obj", _TET_OBJ)
    with Mesh.load(path) as mesh:
        before = set(_positions(mesh))
        mesh.loop_subdivide(LoopSubdivideMode.LINEAR)
        after = set(_positions(mesh))
        missing = before - after
        assert not missing, "linear mode moved original vertices: {0}".format(missing)

    # Smooth mode, by contrast, moves every one of them — a caller who wanted
    # "same shape" must not be able to get this by accident.
    with Mesh.load(path) as mesh:
        before = set(_positions(mesh))
        mesh.loop_subdivide(LoopSubdivideMode.SMOOTH)
        survivors = before & set(_positions(mesh))
        assert not survivors, "smooth mode left vertices unmoved: {0}".format(survivors)
    print("PASS: linear preserves original positions, smooth does not")


def _test_default_mode_is_named_smooth(tmpdir):
    from cyberremesh import LoopSubdivideMode, Mesh

    path = _write(tmpdir, "tet.obj", _TET_OBJ)
    with Mesh.load(path) as mesh:
        mesh.loop_subdivide()
        default = _positions(mesh)
    with Mesh.load(path) as mesh:
        mesh.loop_subdivide(LoopSubdivideMode.SMOOTH)
        explicit = _positions(mesh)
    assert default == explicit
    print("PASS: the default mode is SMOOTH, as documented")


def _test_open_mesh_keeps_its_boundary(tmpdir):
    from cyberremesh import LoopSubdivideMode, Mesh

    path = _write(tmpdir, "flat.obj", _FLAT_OBJ)
    with Mesh.load(path) as mesh:
        mesh.loop_subdivide(LoopSubdivideMode.SMOOTH)
        # Every vertex of this mesh is on the boundary, so the boundary curve
        # rule governs all of them; it combines in-plane positions only and
        # cannot leave z = 0. The interior mask would.
        for (_x, _y, z) in _positions(mesh):
            assert abs(z) < 1e-6, "boundary vertex left the plane: z={0}".format(z)
        # The square's corners stay exactly on their corner (their two boundary
        # neighbours are not collinear, but they do not leave the outline).
        assert mesh.face_count == 8
    print("PASS: an open mesh keeps its boundary in plane")


def _test_quad_input_is_refused_by_name(tmpdir):
    from cyberremesh import LoopSubdivideMode, Mesh, UnsupportedTopologyError

    path = _write(tmpdir, "cube.obj", _CUBE_OBJ)
    with Mesh.load(path) as mesh:
        try:
            mesh.loop_subdivide(LoopSubdivideMode.LINEAR)
        except UnsupportedTopologyError as exc:
            message = str(exc)
            assert "triangle" in message, message
            assert "4 sides" in message, message
        else:
            raise AssertionError("a quad mesh was silently subdivided")
        # Untouched: the refusal is total, not a partial rebuild.
        assert mesh.face_count == 6, mesh.face_count
        assert mesh.vertex_count == 8, mesh.vertex_count

        # triangulate() is the explicit opt-in, and then it works.
        assert mesh.triangulate() == 12
        assert mesh.loop_subdivide(LoopSubdivideMode.LINEAR) == 48
    print("PASS: a quad input raises UnsupportedTopologyError naming the face")


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77

    with tempfile.TemporaryDirectory() as tmpdir:
        _test_quadruples_triangle_count(tmpdir)
        _test_linear_leaves_the_shape_alone(tmpdir)
        _test_default_mode_is_named_smooth(tmpdir)
        _test_open_mesh_keeps_its_boundary(tmpdir)
        _test_quad_input_is_refused_by_name(tmpdir)
    print("all loop-subdivision binding tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
