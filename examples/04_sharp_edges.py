#!/usr/bin/env python3
"""Feature preservation: a cube keeps its sharp 90-degree edges through remesh.

The sharp-edge angle threshold marks the cube's creases as features; the
pipeline never collapses across them or rounds the corners.
"""

import os
import tempfile

import common as c


def main() -> None:
    c.require_engine()
    with tempfile.TemporaryDirectory() as tmp:
        src_path = os.path.join(tmp, "cube.obj")
        c.cube_obj(src_path, subdiv=12)
        src = c.load_obj(src_path)

        out, stats = c.remesh_obj(
            src_path, c.RemeshParams(target_quad_count=900, sharp_edge_degrees=60.0)
        )
        print(c.stat_line("cube remesh", stats, out))

        panels = [src, out]
        titles = [f"input · {len(src['faces'])} triangles",
                  c.quad_label("CyberRemesher · edges kept", out, vs_source=src)]
        ref = c.reference_panel(src_path, c.face_counts(out)[0], source=src)
        if ref:
            panels.append(ref[0])
            titles.append(ref[1])
        c.render_panels(
            panels, titles,
            os.path.join(c.OUTPUT_DIR, "04_sharp_edges.png"),
            suptitle="Sharp-edge preservation — CyberRemesher (feature-aware) vs QuadriFlow",
        )


if __name__ == "__main__":
    main()
