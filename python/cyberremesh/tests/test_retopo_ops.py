#!/usr/bin/env python3
"""Retopology mesh operations through the binding.

Eight of the C ABI's retopo entry points had no Python at all — triangulate,
relax, merge_vertices, dissolve_edges, rotate_edge, insert_loop, delete_faces
and snap_all — so a caller could subdivide a cage from Python and then had no
way to clean it up, tighten it, or pull it back onto the Target. These assert
each one reaches the engine and behaves the way cyber_capi.h documents:
element counts, the skip-don't-fail contract on the batch ops, the
mesh-unchanged contract on the argument failures, and the pin/brush limits.

Self-skips (exit 77, CTest SKIP) when the shared library is not loadable.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402

# 3x3 lattice of vertices in the z = 0 plane welded into 2x2 quads. Small
# enough to reason about by hand and the smallest grid with a real interior:
# vertex 4 is the only interior vertex and edges 1-4 / 3-4 / 4-5 / 4-7 are the
# only interior edges, which is what the local ops below need.
_GRID = "".join(
    "v {0} {1} 0\n".format(x, y) for y in range(3) for x in range(3)
) + "f 1 2 5 4\nf 2 3 6 5\nf 4 5 8 7\nf 5 6 9 8\n"

FAILURES = []
_TEMP_FILES = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok: {name}")
    else:
        FAILURES.append(name)
        print(f"FAIL: {name} {detail}")


def raises(fn, *args, **kwargs):
    """True when the call raised CyberError specifically.

    Deliberately narrow: the ABI reports these argument failures as a non-OK
    status carrying a message, so a bare ``except Exception`` would also pass
    on a TypeError from a mis-declared argtype and hide a broken binding.
    """
    try:
        fn(*args, **kwargs)
    except cyberremesh.CyberError:
        return True
    return False


def _grid_path():
    f = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    f.write(_GRID)
    f.close()
    _TEMP_FILES.append(f.name)
    return f.name


def _grid(path):
    return cyberremesh.Mesh.load_obj(path)


def _positions(mesh):
    return [mesh.vertex_position(i) for i in range(mesh.vertex_count)]


def _distance(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def test_triangulate(path):
    with _grid(path) as mesh:
        before = _positions(mesh)
        faces = mesh.triangulate()
        check("triangulate fans each quad into two triangles", faces == 8,
              f"faces={faces}")
        check("triangulate reports the resulting face count",
              faces == mesh.face_count, f"{faces} vs {mesh.face_count}")
        # Documented id stability: vertices are untouched, only faces split.
        check("triangulate adds no vertices", mesh.vertex_count == 9,
              f"v={mesh.vertex_count}")
        check("triangulate moves no vertex", _positions(mesh) == before)


def test_delete_faces(path):
    with _grid(path) as mesh:
        removed = mesh.delete_faces([0])
        check("delete_faces removes the listed face", removed == 1,
              f"removed={removed}")
        check("delete_faces leaves the rest", mesh.face_count == 3,
              f"faces={mesh.face_count}")

    with _grid(path) as mesh:
        # Skip-don't-fail: an out-of-range id is not an error, it is a no-op,
        # which is what lets a caller replay a stale selection.
        removed = mesh.delete_faces([9999])
        check("delete_faces skips out-of-range ids", removed == 0,
              f"removed={removed}")
        check("delete_faces leaves the mesh whole when it skips",
              mesh.face_count == 4, f"faces={mesh.face_count}")
        check("delete_faces accepts an empty list",
              mesh.delete_faces([]) == 0)


def test_dissolve_edges(path):
    with _grid(path) as mesh:
        interior = mesh.edge_between(1, 4)
        assert interior is not None, "no edge between vertices 1 and 4"
        dissolved = mesh.dissolve_edges([interior])
        check("dissolve_edges merges the two faces of an interior edge",
              dissolved == 1, f"dissolved={dissolved}")
        check("dissolve_edges leaves one face fewer", mesh.face_count == 3,
              f"faces={mesh.face_count}")
        check("dissolve_edges keeps every vertex", mesh.vertex_count == 9,
              f"v={mesh.vertex_count}")

    with _grid(path) as mesh:
        # A boundary edge has one face, so there is nothing to merge: the
        # header says such edges are skipped, and dissolving none is CYBER_OK
        # with 0 rather than a failure.
        boundary = mesh.edge_between(0, 1)
        assert boundary is not None, "no edge between vertices 0 and 1"
        dissolved = mesh.dissolve_edges([boundary])
        check("dissolve_edges skips a boundary edge", dissolved == 0,
              f"dissolved={dissolved}")
        check("a skipped dissolve leaves the mesh unchanged",
              mesh.face_count == 4, f"faces={mesh.face_count}")


def test_rotate_edge(path):
    with _grid(path) as mesh:
        interior = mesh.edge_between(1, 4)
        mesh.rotate_edge(interior)
        # The quad pair is dissolved into a hexagon and re-split one corner
        # over, so the pair survives as a pair — the face count is the tell
        # that it re-split rather than merged.
        check("rotate_edge keeps the pair a pair", mesh.face_count == 4,
              f"faces={mesh.face_count}")
        check("rotate_edge adds no vertices", mesh.vertex_count == 9,
              f"v={mesh.vertex_count}")
        check("rotate_edge turns the pair's diagonal",
              mesh.edge_between(1, 4) is None)

    with _grid(path) as mesh:
        boundary = mesh.edge_between(0, 1)
        check("rotate_edge refuses a boundary edge",
              raises(mesh.rotate_edge, boundary))
        check("a refused rotate leaves the mesh unchanged",
              mesh.face_count == 4 and mesh.vertex_count == 9)


def test_insert_loop(path):
    with _grid(path) as mesh:
        interior = mesh.edge_between(1, 4)
        new_faces = mesh.insert_loop(interior)
        # The ring through an interior edge of a 2x2 grid is both quads, so
        # the loop splits both: two new faces and three new vertices (the two
        # ring midpoints plus the one on the crossed edge).
        check("insert_loop splits the whole quad ring", new_faces == 2,
              f"new={new_faces}")
        check("insert_loop leaves six faces", mesh.face_count == 6,
              f"faces={mesh.face_count}")
        check("insert_loop adds the ring midpoints", mesh.vertex_count == 12,
              f"v={mesh.vertex_count}")

    with _grid(path) as mesh:
        interior = mesh.edge_between(1, 4)
        # t is the split parameter and must be strictly inside (0, 1): an
        # endpoint would split onto an existing vertex.
        check("insert_loop refuses t = 0", raises(mesh.insert_loop, interior, 0.0))
        check("insert_loop refuses t = 1", raises(mesh.insert_loop, interior, 1.0))
        check("a refused insert_loop leaves the mesh unchanged",
              mesh.face_count == 4 and mesh.vertex_count == 9)

    with _grid(path) as mesh:
        # "Borders no quad" is the failure the header names, and a boundary
        # edge is NOT that — it still borders one quad and the loop runs off
        # the ring. Triangulating first removes every quad, which is.
        mesh.triangulate()
        check("insert_loop refuses an edge that borders no quad",
              raises(mesh.insert_loop, mesh.edge_between(0, 1)))
        check("insert_loop leaves the mesh unchanged when it refuses",
              mesh.face_count == 8 and mesh.vertex_count == 9)


def test_merge_vertices(path):
    with _grid(path) as mesh:
        mesh.merge_vertices(0, 1)
        check("merge_vertices retires the removed vertex",
              mesh.vertex_count == 8, f"v={mesh.vertex_count}")
        check("merge_vertices parks the survivor at keep's position",
              mesh.vertex_position(0) == (0.0, 0.0, 0.0),
              f"p={mesh.vertex_position(0)}")

    with _grid(path) as mesh:
        mesh.merge_vertices(0, 1, at_midpoint=True)
        check("at_midpoint moves the survivor to the midpoint",
              mesh.vertex_position(0) == (0.5, 0.0, 0.0),
              f"p={mesh.vertex_position(0)}")

    with _grid(path) as mesh:
        check("merge_vertices refuses keep == remove",
              raises(mesh.merge_vertices, 3, 3))
        check("merge_vertices refuses an out-of-range vertex",
              raises(mesh.merge_vertices, 0, 9999))
        check("a refused merge leaves the mesh unchanged",
              mesh.vertex_count == 9 and mesh.face_count == 4)


def _lift(mesh, dz):
    mesh.set_positions([(p[0], p[1], p[2] + dz) for p in _positions(mesh)])


def test_relax(path):
    with _grid(path) as mesh:
        # Vertex 4 is the interior one; dragging it off the plane gives relax
        # something to pull back toward its one-ring centroid at (1, 1, 0).
        moved = list(_positions(mesh))
        moved[4] = (4.0, 4.0, 4.0)
        mesh.set_positions(moved)
        before = _distance(mesh.vertex_position(4), (1.0, 1.0, 0.0))
        mesh.relax(strength=1.0, iterations=20)
        after = _distance(mesh.vertex_position(4), (1.0, 1.0, 0.0))
        check("relax with no center relaxes the whole mesh", after < before,
              f"{before} -> {after}")
        check("relax adds and removes nothing",
              mesh.vertex_count == 9 and mesh.face_count == 4)

    with _grid(path) as mesh:
        moved = list(_positions(mesh))
        moved[4] = (1.0, 1.0, 3.0)
        mesh.set_positions(moved)
        # A brush centred far away must not reach vertex 4 — this is the
        # check that `radius` is really passed through and not swallowed.
        mesh.relax(center=(50.0, 50.0, 50.0), radius=1.0, strength=1.0,
                   iterations=5)
        check("a brush that misses the mesh moves nothing",
              mesh.vertex_position(4) == (1.0, 1.0, 3.0),
              f"p={mesh.vertex_position(4)}")

    with _grid(path) as mesh:
        moved = list(_positions(mesh))
        moved[4] = (1.0, 1.0, 3.0)
        mesh.set_positions(moved)
        # radius is documented as ignored when there is no center: the ABI has
        # no relax-all call, a non-positive radius IS relax-all, so a stray
        # radius must not turn this into a brush at the origin.
        mesh.relax(radius=0.001, strength=1.0, iterations=5)
        check("radius is ignored without a center",
              mesh.vertex_position(4) != (1.0, 1.0, 3.0),
              f"p={mesh.vertex_position(4)}")

    with _grid(path) as mesh:
        moved = list(_positions(mesh))
        moved[4] = (1.0, 1.0, 3.0)
        mesh.set_positions(moved)
        mesh.relax(strength=1.0, iterations=20, pinned=[4])
        check("a pinned vertex resists relax",
              mesh.vertex_position(4) == (1.0, 1.0, 3.0),
              f"p={mesh.vertex_position(4)}")
        check("relax refuses a strength outside [0, 1]",
              raises(mesh.relax, strength=2.0))
        check("relax refuses zero iterations",
              raises(mesh.relax, iterations=0))


def test_snap_all(path):
    from cyberremesh import Snapper

    with _grid(path) as target, _grid(path) as mesh:
        _lift(mesh, 0.5)
        with Snapper(target) as snapper:
            report = mesh.snap_all(snapper)
        check("snap_all projects every live vertex", report.moved == 9,
              f"moved={report.moved}")
        check("snap_all reports the largest correction",
              abs(report.max_distance - 0.5) < 1e-5,
              f"max={report.max_distance}")
        check("snap_all lands the vertices on the Target",
              all(abs(p[2]) < 1e-5 for p in _positions(mesh)))
        check("snap_all preserves the topology",
              mesh.vertex_count == 9 and mesh.face_count == 4)

    with _grid(path) as target, _grid(path) as mesh:
        _lift(mesh, 0.5)
        with Snapper(target) as snapper:
            report = mesh.snap_all(snapper, pinned=[0])
        check("a pinned vertex is left exactly where it was",
              mesh.vertex_position(0) == (0.0, 0.0, 0.5),
              f"p={mesh.vertex_position(0)}")
        check("snap_all skips the pinned vertex in its count",
              report.moved == 8, f"moved={report.moved}")

    with _grid(path) as mesh:
        # There is nothing to snap to without a snapper, and the header says
        # so: this must be an error, never a silent no-op.
        check("snap_all needs a snapper", raises(mesh.snap_all, None))


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77

    path = _grid_path()
    try:
        test_triangulate(path)
        test_delete_faces(path)
        test_dissolve_edges(path)
        test_rotate_edge(path)
        test_insert_loop(path)
        test_merge_vertices(path)
        test_relax(path)
        test_snap_all(path)
    finally:
        for p in _TEMP_FILES:
            try:
                os.unlink(p)
            except OSError:
                pass

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed: {FAILURES}")
        return 1
    print("\nall retopo mesh-operation checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
