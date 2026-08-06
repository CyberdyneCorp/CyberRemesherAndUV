#!/usr/bin/env python3
"""Soft-selection binding test.

Two halves:

* ``check_parity`` is a SOURCE-LEVEL gate over ``cyber_retopo_selection_*``: it
  parses ``capi/include/cyber_capi.h`` and asserts every entry point is bound in
  ``_ffi.py``, reachable through a public method in ``api.py``, and referenced by
  the Swift wrapper. It needs no shared library, so it runs everywhere. The repo
  has no project-wide binding-parity harness to register with; this covers this
  change's surface only.
* the functional cases drive the real ABI and self-skip (exit 77, CTest SKIP)
  when the shared library is not loadable.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402

_REPO = os.path.dirname(  # <repo>/python/cyberremesh/tests -> <repo>
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)

# A 9 x 3 quad grid in the plane z = 0 (x = 0..8, y = 0..2), as a Target.
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


def _read(path: str) -> str:
    with open(os.path.join(_REPO, path), "r", encoding="utf-8") as handle:
        return handle.read()


def check_parity() -> None:
    """Every soft-selection ABI symbol is bound in Python and Swift."""
    header = _read("capi/include/cyber_capi.h")
    symbols = sorted(set(re.findall(r"\bcyber_retopo_selection_[a-z_]+", header)))
    assert len(symbols) >= 16, symbols

    ffi = _read("python/cyberremesh/cyberremesh/_ffi.py")
    api = _read("python/cyberremesh/cyberremesh/api.py")
    swift = _read("swift/Sources/CyberRemesher/SoftSelection.swift")

    for symbol in symbols:
        assert symbol in ffi, "{0}: no ctypes declaration in _ffi.py".format(symbol)
        assert symbol in api, "{0}: not reachable from api.py".format(symbol)
        assert symbol in swift, "{0}: not wrapped in SoftSelection.swift".format(symbol)

    # The report struct must mirror the header field-for-field: a silent layout
    # mismatch reads garbage instead of failing loudly.
    fields = [name for name, _ in cyberremesh.SoftTransformReport.__annotations__.items()]
    assert fields == ["moved", "resnapped", "max_snap_distance"], fields
    print("PASS parity: {0} soft-selection symbols bound in C ABI / Python / Swift".format(
        len(symbols)))


def check_line_gradient(mesh) -> None:
    """Ramp 0 -> 1 along the gradient, saturating past the end."""
    mesh.select_line((2.0, 0.0, 0.0), (6.0, 0.0, 0.0), falloff=cyberremesh.Falloff.LINEAR)
    w = mesh.selection_weights()
    assert len(w) == mesh.vertex_count, (len(w), mesh.vertex_count)
    assert w[2] == 0.0, w[2]      # at the anchor
    assert w[6] == 1.0, w[6]      # at the end
    assert w[8] == 1.0, w[8]      # beyond the end
    assert w[0] == 0.0, w[0]      # behind the anchor
    assert w[3] < w[4] < w[5], w[3:6]
    assert all(0.0 <= x <= 1.0 for x in w)


def check_paint_accumulates_and_subtracts(mesh) -> None:
    """Overlapping dabs accumulate toward 1; a subtract dab reduces them."""
    mesh.clear_selection()
    mesh.paint_selection((3.0, 1.0, 0.0), radius=2.5, pressure=0.5)
    once = mesh.selection_weights()
    mesh.paint_selection((5.0, 1.0, 0.0), radius=2.5, pressure=0.5)
    twice = mesh.selection_weights()
    assert any(b > a for a, b in zip(once, twice)), "second dab added nothing"
    assert all(b <= 1.0 for b in twice)

    for _ in range(8):
        mesh.paint_selection((4.0, 1.0, 0.0), radius=2.5, pressure=0.5)
    saturated = mesh.selection_weights()
    assert max(saturated) == 1.0, max(saturated)

    for _ in range(12):
        mesh.paint_selection((4.0, 1.0, 0.0), radius=2.5, pressure=1.0, subtract=True)
    erased = mesh.selection_weights()
    assert erased[13] == 0.0, erased[13]  # centre vertex, fully erased, clamped

    # The stroke route reaches the same region as the per-dab route.
    mesh.clear_selection()
    mesh.paint_selection_stroke(
        [(3.0, 1.0, 0.0, 0.5), (5.0, 1.0, 0.0, 0.5)], radius=2.5
    )
    stroked = mesh.selection_weights()
    assert stroked == twice, "stroke route diverged from the per-dab route"


def check_selection_ops(mesh) -> None:
    """Clear / invert / expand / contract / smooth and the named slots."""
    mesh.clear_selection()
    assert set(mesh.selection_weights()) == {0.0}

    mesh.invert_selection()
    assert set(mesh.selection_weights()) == {1.0}
    mesh.invert_selection()
    assert set(mesh.selection_weights()) == {0.0}

    mesh.set_selection_weights([1.0 if i == 13 else 0.0 for i in range(mesh.vertex_count)])
    mesh.save_selection("seed")
    assert mesh.selection_slots() == ["seed"]

    mesh.expand_selection(1)
    assert sum(1 for w in mesh.selection_weights() if w == 1.0) == 5
    mesh.contract_selection(1)
    assert sum(1 for w in mesh.selection_weights() if w == 1.0) == 1

    mesh.load_selection("seed")
    assert mesh.selection_weights()[13] == 1.0

    mesh.smooth_selection(5)
    fractional = [w for w in mesh.selection_weights() if 0.0 < w < 1.0]
    assert fractional, "smoothing produced no gradient"
    assert all(0.0 <= w <= 1.0 for w in mesh.selection_weights())

    try:
        mesh.load_selection("missing")
    except cyberremesh.CyberError:
        pass
    else:  # pragma: no cover - the ABI must reject an unknown slot
        raise AssertionError("loading an unknown slot should raise")


def check_weighted_transform_glue(mesh, snapper) -> None:
    """Weighted move glues the affected vertices to the Target in one call."""
    before = list(mesh._copy_positions())  # always available, unlike .positions
    mesh.select_line((4.0, 0.0, 0.0), (8.0, 0.0, 0.0), falloff=cyberremesh.Falloff.LINEAR)
    weights = mesh.selection_weights()
    report = mesh.transform_selection(
        [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, -0.3], snapper=snapper
    )
    assert report.moved == sum(1 for w in weights if w > 0.0), report
    assert report.moved > 0
    assert report.resnapped > 0, report  # the glue actually pulled vertices back
    assert report.max_snap_distance > 0.0, report

    after = list(mesh._copy_positions())
    for v, w in enumerate(weights):
        if w <= 0.0:
            # Zero weight: bit-identical, never re-snapped.
            assert after[v * 3:v * 3 + 3] == before[v * 3:v * 3 + 3], v
        else:
            # Glued onto the Target plane by the transform itself.
            assert abs(after[v * 3 + 2]) < 1e-5, (v, after[v * 3 + 2])


def main() -> int:
    check_parity()

    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77  # CTest SKIP_RETURN_CODE

    from cyberremesh import Mesh, Snapper

    target_obj = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    target_obj.write(_grid_obj(9, 3, 0.0))
    target_obj.close()
    edit_obj = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    edit_obj.write(_grid_obj(9, 3, 0.3))
    edit_obj.close()
    try:
        with Mesh.load_obj(edit_obj.name) as mesh:
            check_line_gradient(mesh)
            check_paint_accumulates_and_subtracts(mesh)
            check_selection_ops(mesh)

        with Mesh.load_obj(target_obj.name) as target, \
                Mesh.load_obj(edit_obj.name) as mesh:
            with Snapper(target) as snapper:
                check_weighted_transform_glue(mesh, snapper)
    finally:
        os.unlink(target_obj.name)
        os.unlink(edit_obj.name)
    print("PASS soft selection bindings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
