#!/usr/bin/env python3
"""Curvature adaptivity: uniform edge lengths vs curvature-adaptive density.

On a bumpy sphere, adaptivity=0 spreads faces evenly, while adaptivity=1 packs
smaller faces into the high-curvature bumps and coarsens the flat regions.
"""

import os
import tempfile

import common as c


def main() -> None:
    c.require_engine()
    with tempfile.TemporaryDirectory() as tmp:
        src_path = os.path.join(tmp, "bumpy.obj")
        c.bumpy_sphere_obj(src_path, rings=36, segments=54)

        meshes, titles = [], []
        for adapt in (0.0, 1.0):
            out, stats = c.remesh_obj(
                src_path, c.RemeshParams(target_quad_count=1600, adaptivity=adapt)
            )
            print(c.stat_line(f"adaptivity={adapt}", stats, out))
            meshes.append(out)
            titles.append(f"adaptivity {adapt:g}")

        c.render_panels(
            meshes, titles, os.path.join(c.OUTPUT_DIR, "03_adaptivity.png"),
            suptitle="Curvature adaptivity (uniform vs curvature-adaptive)",
        )


if __name__ == "__main__":
    main()
