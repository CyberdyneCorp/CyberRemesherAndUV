#!/usr/bin/env python3
"""Pure-quad mode: quad-dominant (some residual triangles) vs all-quads."""

import os
import tempfile

import common as c


def main() -> None:
    c.require_engine()
    with tempfile.TemporaryDirectory() as tmp:
        src_path = os.path.join(tmp, "torus.obj")
        c.torus_obj(src_path, major=44, minor=22)

        dominant, s1 = c.remesh_obj(src_path, c.RemeshParams(target_quad_count=1400))
        pure, s2 = c.remesh_obj(src_path, c.RemeshParams(target_quad_count=1400, pure_quads=True))
        print(c.stat_line("quad-dominant", s1, dominant))
        print(c.stat_line("pure quads", s2, pure))

        q1, t1, _ = c.face_counts(dominant)
        q2, t2, _ = c.face_counts(pure)
        src = c.load_obj(src_path)
        panels = [dominant, pure]
        titles = [f"quad-dominant · {t1} triangles left",
                  c.quad_label("pure quads", pure, vs_source=src)]
        for panel in c.reference_panels(src_path, q2, source=src):
            panels.append(panel[0])
            titles.append(panel[1])
        c.render_panels(
            panels, titles,
            os.path.join(c.OUTPUT_DIR, "05_pure_quads.png"),
            suptitle="Pure-quad mode eliminates residual triangles (vs QuadriFlow & AutoRemesher, matched)",
        )


if __name__ == "__main__":
    main()
