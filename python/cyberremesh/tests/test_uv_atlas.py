#!/usr/bin/env python3
"""Automatic UV-atlas binding test. Self-skips (exit 77, CTest SKIP) when the
shared library is not loadable, so it is safe to run unconditionally in CI."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cyberremesh  # noqa: E402

# Unit cube, six welded quad faces — every face has a distinct axis-aligned
# normal, so normal-coherent chart growth must isolate each into its own chart.
_CUBE = (
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
    "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
    "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 3 4 8 7\nf 2 3 7 6\nf 4 1 5 8\n"
)


def main() -> int:
    if not cyberremesh.is_available():
        print("SKIP: cyber_capi shared library not loadable")
        return 77  # CTest SKIP_RETURN_CODE

    from cyberremesh import AtlasParams, Mesh

    obj = tempfile.NamedTemporaryFile(suffix=".obj", delete=False, mode="w")
    obj.write(_CUBE)
    obj.close()
    out = tempfile.NamedTemporaryFile(suffix=".obj", delete=False)
    out.close()
    try:
        with Mesh.load_obj(obj.name) as mesh:
            res = mesh.unwrap_atlas(AtlasParams(max_chart_angle_degrees=40.0))
            # One normal-coherent chart per cube face.
            assert res.chart_count == 6, res.chart_count
            # Each chart is a single planar quad -> LSCM is near-perfectly
            # conformal and never mirrored.
            assert res.max_angle_distortion < 1e-3, res.max_angle_distortion
            assert res.flipped_charts == 0, res.flipped_charts
            # Charts packed into the unit square cover a non-trivial fraction.
            assert 0.0 < res.packed_area <= 1.0, res.packed_area

            mesh.save_obj(out.name)
            text = open(out.name, "r", encoding="utf-8").read()
            assert "vt " in text, "atlas did not write UV coordinates"
            f_lines = [ln for ln in text.splitlines() if ln.startswith("f ")]
            assert f_lines and "/" in f_lines[0], "faces missing vt indices"

        # A second run on a fresh handle must be deterministic.
        with Mesh.load_obj(obj.name) as mesh2:
            res2 = mesh2.unwrap_atlas(AtlasParams(max_chart_angle_degrees=40.0))
            assert res2.chart_count == res.chart_count
            assert res2.seam_edges == res.seam_edges
        print(f"PASS uv atlas: {res.chart_count} charts, "
              f"max distortion {res.max_angle_distortion:.4f}, "
              f"packed {res.packed_area * 100:.0f}%")
    finally:
        os.unlink(obj.name)
        os.unlink(out.name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
