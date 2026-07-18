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

        c.render_panels(
            [src, out],
            [f"input · {len(src['faces'])} triangles", "remeshed · sharp edges kept"],
            os.path.join(c.OUTPUT_DIR, "04_sharp_edges.png"),
            suptitle="Sharp-edge preservation (feature-aware remesh)",
        )


if __name__ == "__main__":
    main()
