#!/usr/bin/env python3
"""Core showcase: turn a triangle mesh into a quad-dominant one.

Loads a triangulated UV sphere and runs the default remeshing pipeline through
the Python binding, then renders input vs output. Blue faces are quads.
"""

import os
import tempfile

import common as c


def main() -> None:
    c.require_engine()
    with tempfile.TemporaryDirectory() as tmp:
        src_path = os.path.join(tmp, "sphere.obj")
        c.sphere_obj(src_path, rings=18, segments=28)
        src = c.load_obj(src_path)

        params = c.RemeshParams(target_quad_count=1200, adaptivity=1.0)
        out, stats = c.remesh_obj(src_path, params)

        print(c.stat_line("input (triangles)", _fake_stats(src), src))
        print(c.stat_line("remeshed (quads)", stats, out))

        c.render_panels(
            [src, out],
            [f"input · {len(src['faces'])} triangles", f"remeshed · quad-dominant"],
            os.path.join(c.OUTPUT_DIR, "01_quad_remesh.png"),
            suptitle="CyberRemesher — triangle mesh → quad-dominant remesh",
        )


class _fake_stats:  # noqa: N801 - tiny shim so stat_line works for the raw input
    def __init__(self, mesh):
        self.vertices = len(mesh["positions"])


if __name__ == "__main__":
    main()
