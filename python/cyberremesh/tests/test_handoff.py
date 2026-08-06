#!/usr/bin/env python3
"""Sculpt handoff bridge binding test (pipeline-bridge).

Covers the three Python surfaces the bridge adds: `Mesh.load_handoff`, the
typed version-mismatch exception, `bake_field` through a Python `FieldEvaluator`
subclass, and `conform`'s max/RMS/flagged report.

Self-skips (exit 77, CTest SKIP) when the shared library is not loadable, so it
is safe to run unconditionally in CI.
"""

import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402

_UV_PLANE = (
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
    "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
    "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n"
)

_LIFTED_PLANE = "v 0 0 0.25\nv 1 0 0.25\nv 1 1 0.25\nv 0 1 0.25\nf 1 2 3\nf 1 3 4\n"


def handoff_ply(major: int, minor: int) -> str:
    """The PLY profile from docs/sculpt-handoff-format.md, for a red box."""
    corners = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
               (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
             (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = [t for q in quads for t in ((q[0], q[1], q[2]), (q[0], q[2], q[3]))]
    lines = ["ply", "format ascii 1.0",
             f"comment cyber_sculpt_handoff {major} {minor}",
             "comment cyber_handoff_producer python-test",
             f"element vertex {len(corners)}",
             "property float x", "property float y", "property float z",
             "property float nx", "property float ny", "property float nz",
             "property uchar red", "property uchar green", "property uchar blue",
             "property float material_mix",
             f"element face {len(tris)}",
             "property list uchar int vertex_indices",
             "end_header"]
    for i, c in enumerate(corners):
        n = [c[k] - 0.5 for k in range(3)]
        lines.append(f"{c[0]} {c[1]} {c[2]} {n[0]} {n[1]} {n[2]} 255 0 0 {i / 8.0}")
    for t in tris:
        lines.append(f"3 {t[0]} {t[1]} {t[2]}")
    return "\n".join(lines) + "\n"


def write_temp(text: str, suffix: str) -> str:
    handle = tempfile.NamedTemporaryFile(suffix=suffix, delete=False, mode="w")
    handle.write(text)
    handle.close()
    return handle.name


def check_handoff_ingest(tmpdir: str) -> None:
    from cyberremesh import IncompatibleVersionError, Mesh

    good = os.path.join(tmpdir, "sculpt.ply")
    with open(good, "w") as f:
        f.write(handoff_ply(1, 0))
    mesh, info = Mesh.load_handoff(good)
    with mesh:
        assert info.version == cyberremesh.HANDOFF_VERSION, info.version
        assert info.producer == "python-test", info.producer
        assert info.vertex_count == 8, info.vertex_count
        assert info.face_count == 12, info.face_count
        assert info.has_vertex_colors, info
        assert info.has_vertex_normals, info
        assert info.has_material_mix, info

    future = os.path.join(tmpdir, "future.ply")
    with open(future, "w") as f:
        f.write(handoff_ply(2, 0))
    try:
        Mesh.load_handoff(future)
    except IncompatibleVersionError as exc:
        assert "2.0" in str(exc) and "1.0" in str(exc), str(exc)
    else:
        raise AssertionError("a future handoff version must raise")
    print("PASS handoff: version 1.0 ingests with its payloads; 2.0 raises naming both")


def check_field_bake(tmpdir: str) -> None:
    from cyberremesh import BakeMap, BakeParams, FieldEvaluator, Mesh, bake_field

    class Plane(FieldEvaluator):
        """z = 0 as a signed distance field; a fixed 0.25 openness."""

        def distance(self, p):
            return p[2]

        def gradient(self, p):
            return (0.0, 0.0, 1.0)

        def occlusion(self, p, n, radius):
            return 0.25

    obj = os.path.join(tmpdir, "plane.obj")
    with open(obj, "w") as f:
        f.write(_UV_PLANE)
    field = Plane()
    params = BakeParams(width=12, height=12)
    with Mesh.load_obj(obj) as low:
        # No Target mesh at all: the field answers the normal bake alone.
        with bake_field(low, BakeMap.NORMAL, field, params) as img:
            assert img.channels == 3, img.channels
            pixels = img.to_numpy() if cyberremesh.HAVE_NUMPY else None
            if pixels is not None:
                blue = float(pixels[6, 6, 2])
                assert abs(blue - 1.0) < 0.05, ("tangent-space up", blue)
        with bake_field(low, BakeMap.AO, field, params) as ao:
            assert ao.channels == 1, ao.channels
            if cyberremesh.HAVE_NUMPY:
                openness = float(ao.to_numpy()[6, 6, 0])
                assert abs(openness - 0.25) < 0.02, ("evaluator occlusion", openness)
    print("PASS field bake: a Python FieldEvaluator drives normal and AO with no Target mesh")


def check_conform(tmpdir: str) -> None:
    from cyberremesh import Mesh, conform

    edit_path = os.path.join(tmpdir, "edit.obj")
    target_path = os.path.join(tmpdir, "target.obj")
    with open(edit_path, "w") as f:
        f.write(_UV_PLANE)
    with open(target_path, "w") as f:
        f.write(_LIFTED_PLANE)

    with Mesh.load_obj(edit_path) as edit, Mesh.load_obj(target_path) as target:
        report = conform(edit, target, threshold=0.1)
        assert report.moved_vertices == 4, report.moved_vertices
        assert math.isclose(report.max_deviation, 0.25, rel_tol=0.05), report.max_deviation
        assert math.isclose(report.rms_deviation, 0.25, rel_tol=0.05), report.rms_deviation
        # Every vertex diverged past the threshold, so all four are flagged and
        # the operation still completed.
        assert sorted(report.flagged) == [0, 1, 2, 3], report.flagged

        # Conforming again onto the same Target is now a no-op.
        again = conform(edit, target, threshold=0.1)
        assert again.max_deviation < 1e-4, again.max_deviation
        assert again.flagged == [], again.flagged
    print("PASS conform: re-snap reports max/RMS deviation and flags every diverged vertex")


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77  # CTest SKIP_RETURN_CODE — reported as Skipped, never a vacuous pass

    tmpdir = tempfile.mkdtemp(prefix="cyber_py_handoff_")
    check_handoff_ingest(tmpdir)
    check_field_bake(tmpdir)
    check_conform(tmpdir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
