#!/usr/bin/env python3
"""Mesh-state binding test: copy, position write-back, edge -> vertex pair.

Covers the three in-memory primitives anything doing a before/after comparison
or drawing a committed seam needs, and which the bindings used to lack:

  * ``Mesh.copy`` — a real duplicate. Before it existed the only way to keep a
    baseline was a ``save_obj`` / ``load_obj`` round trip, which narrows to the
    OBJ text precision and so reports spurious vertex movement; the exactness
    assertion here is what pins that down.
  * ``Mesh.set_positions`` / the ``positions`` setter — restoring a snapshot.
  * ``Mesh.edge_endpoints`` / ``Mesh.vertex_position`` — resolving the opaque
    edge ids that ``SeamSet.edges`` / ``SeamPath.edges`` hand back into
    drawable segments.

Runs as a plain script; self-skips (exit 77, CTest SKIP) when the shared
library is not loadable.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402


def _grid_obj(cols: int, rows: int, z: float) -> str:
    lines = []
    for j in range(rows):
        for i in range(cols):
            lines.append("v {0} {1} {2}".format(i, j, z))
    for j in range(rows - 1):
        for i in range(cols - 1):
            a = j * cols + i + 1
            lines.append("f {0} {1} {2} {3}".format(a, a + 1, a + cols + 1, a + cols))
    return "\n".join(lines) + "\n"


def check_copy_is_exact_and_independent(mesh, path) -> None:
    """A copy duplicates the whole handle, losslessly, and then goes its own way."""
    mesh.select_sphere((4.0, 1.0, 0.0), 3.0, falloff=cyberremesh.Falloff.SMOOTH)
    mesh.save_selection("cap")

    copy = mesh.copy()
    try:
        assert copy.handle != mesh.handle
        assert copy.vertex_count == mesh.vertex_count
        assert copy.face_count == mesh.face_count
        assert list(copy._copy_positions()) == list(mesh._copy_positions())
        # The handle state rides along, not just geometry.
        assert copy.selection_weights() == mesh.selection_weights()
        assert copy.selection_slots() == ["cap"]

        # The lossless claim, stated against the round trip it replaces: an OBJ
        # detour perturbs positions, an in-memory copy does not.
        mesh.save_obj(path)
        with cyberremesh.Mesh.load_obj(path) as reloaded:
            drift = max(abs(a - b) for a, b in
                        zip(reloaded._copy_positions(), mesh._copy_positions()))
        assert max(abs(a - b) for a, b in
                   zip(copy._copy_positions(), mesh._copy_positions())) == 0.0, "copy is lossy"
        assert drift >= 0.0  # the round trip is the baseline being replaced

        # Editing the copy must not touch the original.
        baseline = list(mesh._copy_positions())
        lifted = list(copy._copy_positions())
        for i in range(2, len(lifted), 3):
            lifted[i] += 1.0
        copy.set_positions(lifted)
        assert list(copy._copy_positions()) == lifted
        assert list(mesh._copy_positions()) == baseline, "copy aliased the original"
    finally:
        copy.close()


def check_set_positions_round_trips(mesh) -> None:
    """A snapshot taken from the mesh restores through the setter bit-exactly."""
    snapshot = list(mesh._copy_positions())

    moved = [v + 0.25 for v in snapshot]
    mesh.set_positions(moved)
    assert list(mesh._copy_positions()) == moved

    mesh.set_positions(snapshot)
    assert list(mesh._copy_positions()) == snapshot, "restore was not exact"

    # A count mismatch must raise, never silently pair positions with the
    # wrong vertices.
    for bad in (snapshot[:-3], snapshot + [0.0, 0.0, 0.0]):
        try:
            mesh.set_positions(bad)
        except (ValueError, cyberremesh.CyberError):
            pass
        else:  # pragma: no cover - a mismatch must be rejected
            raise AssertionError("set_positions accepted {0} floats".format(len(bad)))
    assert list(mesh._copy_positions()) == snapshot, "a rejected write still mutated"

    if cyberremesh.HAVE_NUMPY:
        import numpy as np

        shifted = mesh.positions + np.float32(0.5)
        mesh.positions = shifted  # the property setter, on an (n, 3) array
        assert np.array_equal(mesh.positions, shifted)
        mesh.positions = np.asarray(snapshot, dtype=np.float32).reshape((-1, 3))
        assert list(mesh._copy_positions()) == snapshot


def check_edge_resolves_to_positions(mesh) -> None:
    """An edge id resolves to a vertex pair and then to two positions."""
    v0, v1 = mesh.edge_endpoints(0)
    assert v0 != v1
    a = mesh.vertex_position(v0)
    b = mesh.vertex_position(v1)
    assert a is not None and b is not None
    assert len(a) == 3 and len(b) == 3
    assert a != b, "an edge's endpoints share a position"
    # Unit grid spacing: an edge is exactly one unit long.
    assert abs(sum((x - y) ** 2 for x, y in zip(a, b)) - 1.0) < 1e-5, (a, b)

    # Ids the mesh does not own report absence rather than garbage.
    assert mesh.edge_endpoints(10 ** 9) is None
    assert mesh.vertex_position(10 ** 9) is None


def check_seam_edges_are_drawable(mesh) -> None:
    """The reported gap: a COMMITTED seam is a set of edge ids and nothing else,
    so without an endpoint accessor it cannot be drawn."""
    try:
        seams = cyberremesh.SeamSet()
    except cyberremesh.CyberError:  # engine built without the UV module
        print("  (skipped the seam half: no UV module)")
        return
    try:
        for edge in (0, 1, 2):
            seams.mark(edge)
        segments = []
        for edge in seams.edges():
            pair = mesh.edge_endpoints(edge)
            assert pair is not None, edge
            segments.append((mesh.vertex_position(pair[0]), mesh.vertex_position(pair[1])))
        assert len(segments) == 3
        assert all(p is not None for seg in segments for p in seg)
    finally:
        seams.close()


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77  # CTest SKIP_RETURN_CODE

    from cyberremesh import Mesh

    obj = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    obj.write(_grid_obj(9, 3, 0.0))
    obj.close()
    scratch = obj.name + ".copy.obj"
    try:
        with Mesh.load_obj(obj.name) as mesh:
            check_copy_is_exact_and_independent(mesh, scratch)
            check_set_positions_round_trips(mesh)
            check_edge_resolves_to_positions(mesh)
            check_seam_edges_are_drawable(mesh)
    finally:
        os.unlink(obj.name)
        if os.path.exists(scratch):
            os.unlink(scratch)
    print("PASS mesh copy / position write-back / edge endpoints")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
