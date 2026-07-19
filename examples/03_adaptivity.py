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
            titles.append(f"adaptivity {adapt:g} · {c.face_counts(out)[0]} quads")

        # QuadriFlow only sizes uniformly — the contrast to our adaptive panel.
        src = c.load_obj(src_path)
        ref = c.reference_panel(src_path, c.face_counts(meshes[-1])[0], source=src)
        if ref:
            meshes.append(ref[0])
            titles.append(ref[1])
        c.render_panels(
            meshes, titles, os.path.join(c.OUTPUT_DIR, "03_adaptivity.png"),
            suptitle="Curvature adaptivity (uniform vs adaptive) vs QuadriFlow (uniform-only)",
        )


if __name__ == "__main__":
    main()
