#!/usr/bin/env python3
"""Auto-routed seam-path binding test.

Two halves, mirroring test_soft_selection.py:

* ``check_parity`` is a SOURCE-LEVEL gate over the ``cyber_seam_*`` ABI: every
  entry point published in ``capi/include/cyber_capi.h`` must be bound in
  ``_ffi.py``, reachable from ``api.py`` and wrapped in Swift. It needs no
  shared library, so it runs everywhere.
* the functional cases drive the real ABI and self-skip (exit 77, CTest SKIP)
  when the shared library is not loadable.
"""

import gc
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402

_REPO = os.path.dirname(  # <repo>/python/cyberremesh/tests -> <repo>
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)

COLS = 7
ROWS = 5


def vid(x: int, y: int) -> int:
    return y * COLS + x


def _groove_obj(depth: float) -> str:
    """A COLS x ROWS quad grid with the y == 1 row sunk into a concave valley."""
    lines = []
    for j in range(ROWS):
        for i in range(COLS):
            z = -depth if j == 1 else 0.0
            lines.append("v {0} {1} {2}".format(i, j, z))
    for j in range(ROWS - 1):
        for i in range(COLS - 1):
            a = j * COLS + i + 1
            lines.append("f {0} {1} {2} {3}".format(a, a + 1, a + COLS + 1, a + COLS))
    return "\n".join(lines) + "\n"


def _read(path: str) -> str:
    with open(os.path.join(_REPO, path), "r", encoding="utf-8") as handle:
        return handle.read()


def check_parity() -> None:
    """Every seam-path ABI symbol is bound in Python and Swift."""
    header = _read("capi/include/cyber_capi.h")
    symbols = sorted(set(re.findall(r"\bcyber_seam_(?:set|path)_[a-z_]+", header)))
    symbols.append("cyber_default_seam_path_options")
    symbols.append("cyber_mesh_edge_signed_dihedral")
    assert len(symbols) >= 20, symbols

    ffi = _read("python/cyberremesh/cyberremesh/_ffi.py")
    api = _read("python/cyberremesh/cyberremesh/api.py")
    swift = _read("swift/Sources/CyberRemesher/SeamPath.swift")

    for symbol in symbols:
        assert symbol in ffi, "{0}: no ctypes declaration in _ffi.py".format(symbol)
        assert symbol in api, "{0}: not reachable from api.py".format(symbol)
        assert symbol in swift, "{0}: not wrapped in SeamPath.swift".format(symbol)

    # The options struct must mirror the header field-for-field.
    fields = [name for name, _ in cyberremesh._ffi.CyberSeamPathOptions._fields_]
    assert fields == [
        "flat_weight",
        "feature_weight",
        "concave_weight",
        "convex_weight",
        "crease_degrees",
        "min_weight",
    ], fields
    print("PASS parity: {0} seam-path symbols bound in C ABI / Python / Swift".format(
        len(symbols)))


def check_defaults() -> None:
    """The engine's defaults come back through the ABI and bias toward grooves."""
    from cyberremesh import SeamCostParams

    defaults = SeamCostParams.defaults()
    assert defaults.flat_weight > defaults.concave_weight, defaults
    assert defaults.flat_weight > defaults.convex_weight, defaults
    assert defaults.flat_weight > defaults.feature_weight, defaults
    assert defaults.min_weight > 0.0, defaults


def check_routes_the_groove(mesh) -> None:
    """The route drops into the valley instead of running straight across."""
    from cyberremesh import SeamPath

    groove = [vid(0, 2)] + [vid(x, 1) for x in range(COLS)] + [vid(COLS - 1, 2)]

    with SeamPath(mesh) as path:
        assert path.add_waypoint(vid(0, 2))
        assert path.add_waypoint(vid(COLS - 1, 2))
        assert path.is_routed()
        assert path.vertices() == groove, path.vertices()

        # ... and it went there because the valley floor really is concave.
        dihedrals = [mesh.edge_signed_dihedral(e) for e in path.edges()]
        assert sum(1 for d in dihedrals if d > 20.0) == COLS - 1, dihedrals


def check_edit_is_local(mesh) -> None:
    """Dragging a waypoint re-routes only the segments that touch it."""
    from cyberremesh import SeamPath

    with SeamPath(mesh) as path:
        for x in (0, 2, 4, 6):
            assert path.add_waypoint(vid(x, 4))
        assert path.segment_count() == 3
        tail = path.segment(2)
        tail_rev = path.segment_revision(2)
        rev0, rev1 = path.segment_revision(0), path.segment_revision(1)

        assert path.move_waypoint(1, vid(2, 3))
        assert path.segment_revision(0) == rev0 + 1
        assert path.segment_revision(1) == rev1 + 1
        assert path.segment(2) == tail
        assert path.segment_revision(2) == tail_rev

        # Deleting the interior waypoint merges its two segments into one.
        assert path.remove_waypoint(1)
        assert path.segment_count() == 2
        assert path.segment(1) == tail
        assert path.segment_revision(1) == tail_rev


def check_commit_resume_drop(mesh) -> None:
    """Commit marks seams and arms a marker; dropping it changes no seam."""
    from cyberremesh import SeamPath, SeamSet

    with SeamSet() as seams, SeamPath(mesh) as path:
        assert path.add_waypoint(vid(3, 0))
        assert path.add_waypoint(vid(3, ROWS - 1))
        pending = path.edges()
        assert len(pending) == ROWS - 1, pending

        marked = path.commit(seams)
        assert sorted(marked) == sorted(pending), (marked, pending)
        assert len(seams) == len(pending)
        assert all(seams.is_seam(e) for e in marked)
        assert path.waypoints() == []
        assert path.resume_marker == vid(3, ROWS - 1)

        # Re-adding the resume vertex is a repeat: it must change nothing, not
        # seed the pending path on its way to reporting failure.
        assert not path.add_waypoint(path.resume_marker)
        assert path.waypoints() == []
        assert path.resume_marker == vid(3, ROWS - 1)

        # Resume: the next waypoint continues from the last committed point.
        assert path.add_waypoint(vid(0, ROWS - 1))
        assert path.waypoints()[0] == vid(3, ROWS - 1)
        assert path.segment(0)[0] == vid(3, ROWS - 1)

        # Drop instead: fresh path, committed seams byte-identical.
        before = seams.edges()
        path.clear()
        path.drop_resume_marker()
        assert path.resume_marker is None
        assert path.add_waypoint(vid(0, ROWS - 1))
        assert path.waypoints() == [vid(0, ROWS - 1)]
        assert seams.edges() == before

        # The returned ids are the undo record.
        seams.revert_commit(marked)
        assert len(seams) == 0


def check_mesh_outlives_path(obj_path: str) -> None:
    """Regression: the engine borrows the mesh, so the path has to pin it.

    A path built from an otherwise unreferenced mesh used to route through freed
    memory (SIGSEGV on a large mesh, silently wrong routes on a small one), and
    closing the mesh by hand has to raise rather than dereference it.
    """
    from cyberremesh import Mesh, SeamPath

    groove = [vid(0, 2)] + [vid(x, 1) for x in range(COLS)] + [vid(COLS - 1, 2)]

    with SeamPath(Mesh.load_obj(obj_path)) as path:
        assert any(isinstance(r, Mesh) for r in gc.get_referents(path)), \
            "SeamPath kept no reference to its Mesh"
        gc.collect()
        churn = [bytearray(4096) for _ in range(256)]  # reclaim any freed block
        del churn
        assert path.add_waypoint(vid(0, 2))
        assert path.add_waypoint(vid(COLS - 1, 2))
        assert path.vertices() == groove, path.vertices()

    mesh = Mesh.load_obj(obj_path)
    with SeamPath(mesh) as path:
        mesh.close()
        for call in (lambda: path.add_waypoint(vid(0, 2)), path.waypoint_count, path.edges):
            try:
                call()
            except ValueError:
                continue
            raise AssertionError("a closed Mesh must invalidate the path")


def main() -> int:
    check_parity()

    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77  # CTest SKIP_RETURN_CODE

    from cyberremesh import Mesh

    obj = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    obj.write(_groove_obj(0.5))
    obj.close()
    try:
        with Mesh.load_obj(obj.name) as mesh:
            check_defaults()
            check_routes_the_groove(mesh)
            check_edit_is_local(mesh)
            check_commit_resume_drop(mesh)
        check_mesh_outlives_path(obj.name)
    finally:
        os.unlink(obj.name)
    print("PASS seam path bindings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
