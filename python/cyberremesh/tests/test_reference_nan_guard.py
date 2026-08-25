#!/usr/bin/env python3
"""Non-finite geometry from a reference solver must be refused, not measured.

A solver can exit 0 and still write "v nan nan nan": QuadriFlow does,
non-deterministically, on the flat-CAD cube used by 04_sharp_edges.py. Before
this guard the NaN flowed straight into rendering and matplotlib raised
"Axis limits cannot be NaN or Inf" from three frames deeper, which is why that
example failed roughly 6 runs in 8. The quieter half of the same bug: NaN
positions yield a 0-degree median angle and a 74% sliver rate -- numbers that
read as "the reference did badly" rather than "the reference produced nothing
measurable" -- so a benchmark could record them and no one would notice.

Skips with 77 (CTest SKIP) when NumPy is missing.
"""
import os
import sys
import tempfile

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(_REPO, "examples"))
sys.path.insert(0, os.path.join(_REPO, "tools", "bench"))

try:
    import numpy as np  # noqa: F401
except ImportError:
    print("SKIP: NumPy not available")
    sys.exit(77)

_CLEAN = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n"
_NAN = "v 0 0 0\nv nan nan nan\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n"
_INF = "v 0 0 0\nv 1 0 0\nv inf 1 0\nv 0 1 0\nf 1 2 3 4\n"


def _write(text: str) -> str:
    fd, path = tempfile.mkstemp(suffix=".obj")
    with os.fdopen(fd, "w") as fh:
        fh.write(text)
    return path


def _expect_refusal(loader, text: str, label: str) -> None:
    path = _write(text)
    try:
        loader(path)
    except ValueError as exc:
        assert "non-finite" in str(exc), f"{label}: unhelpful message {exc!r}"
        return
    finally:
        os.unlink(path)
    raise AssertionError(f"{label}: non-finite geometry was accepted")


def main() -> None:
    import common
    import mesh_metrics

    # Both loaders refuse NaN and Inf, and both still accept clean geometry --
    # a guard that rejected everything would pass the first half of this test.
    for name, loader in (("examples/common.py", common.load_obj),
                         ("tools/bench/mesh_metrics.py", mesh_metrics.load_obj)):
        _expect_refusal(loader, _NAN, f"{name} (nan)")
        _expect_refusal(loader, _INF, f"{name} (inf)")
        path = _write(_CLEAN)
        try:
            loader(path)
        finally:
            os.unlink(path)
        print(f"PASS {name}: refuses nan and inf, accepts clean geometry")

    # The renderer is the last line of defence: reference panels are filtered
    # at load, so non-finite reaching _draw means OUR mesh produced it, and the
    # error has to name the panel instead of surfacing as a matplotlib
    # axis-limits failure with no clue which of N panels was responsible.
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from mpl_toolkits.mplot3d import Axes3D  # noqa: F401
    except ImportError:
        print("SKIP-PART: matplotlib missing, renderer guard not exercised")
        return

    fig = plt.figure()
    ax = fig.add_subplot(111, projection="3d")
    mesh = {"positions": np.array([[0.0, 0.0, 0.0], [float("nan")] * 3,
                                   [1.0, 1.0, 0.0]]), "faces": [[0, 1, 2]]}
    try:
        common._draw(ax, mesh, "panel-under-test")
    except ValueError as exc:
        assert "panel-under-test" in str(exc), f"error does not name the panel: {exc!r}"
        print("PASS renderer: names the offending panel instead of failing inside matplotlib")
    else:
        raise AssertionError("_draw accepted non-finite positions")
    finally:
        plt.close(fig)


if __name__ == "__main__":
    main()
