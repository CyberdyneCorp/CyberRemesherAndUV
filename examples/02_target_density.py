#!/usr/bin/env python3
"""Density control: the same surface remeshed at three target quad counts."""

import os
import tempfile

import common as c


def main() -> None:
    c.require_engine()
    with tempfile.TemporaryDirectory() as tmp:
        src_path = os.path.join(tmp, "torus.obj")
        c.torus_obj(src_path, major=48, minor=24)

        meshes, titles = [], []
        for target in (300, 900, 2500):
            out, stats = c.remesh_obj(src_path, c.RemeshParams(target_quad_count=target))
            print(c.stat_line(f"target={target}", stats, out))
            q, t, _ = c.face_counts(out)
            meshes.append(out)
            titles.append(f"target {target} · {q} quads")

        c.render_panels(
            meshes, titles, os.path.join(c.OUTPUT_DIR, "02_target_density.png"),
            suptitle="Target quad count drives output density",
        )


if __name__ == "__main__":
    main()
