#!/usr/bin/env python3
"""Hole filling: a punched plane with the cleanup pass off vs on."""

import os
import tempfile
from collections import Counter

import common as c


def open_edges(mesh) -> int:
    """Boundary edges (used by exactly one face)."""
    ec: Counter = Counter()
    for f in mesh["faces"]:
        for i in range(len(f)):
            a, b = f[i], f[(i + 1) % len(f)]
            ec[(min(a, b), max(a, b))] += 1
    return sum(1 for v in ec.values() if v == 1)


def main() -> None:
    c.require_engine()
    with tempfile.TemporaryDirectory() as tmp:
        src_path = os.path.join(tmp, "holed.obj")
        c.plane_with_hole_obj(src_path, n=14)
        src = c.load_obj(src_path)

        # hole_fill_max_boundary = 0 disables filling. A moderate limit (64)
        # fills the small interior hole but leaves the large outer perimeter
        # open — set it huge and the outer border would close too.
        no_fill, _ = c.remesh_obj(
            src_path, c.RemeshParams(target_quad_count=800, hole_fill_max_boundary=0)
        )
        filled, _ = c.remesh_obj(
            src_path, c.RemeshParams(target_quad_count=800, hole_fill_max_boundary=64)
        )
        print(f"hole left open : {open_edges(no_fill)} boundary edges (hole + outer border)")
        print(f"hole filled    : {open_edges(filled)} boundary edges (only the outer border left)")

        panels = [src, no_fill, filled]
        titles = ["input · square hole", "remeshed · hole left open",
                  "remeshed · hole filled (grey patch)"]
        # QuadriFlow has no targeted hole-fill pass — the contrast panel.
        ref = c.reference_panel(src_path, c.face_counts(filled)[0])
        if ref:
            panels.append(ref[0])
            titles.append(ref[1])
        c.render_panels(
            panels, titles,
            os.path.join(c.OUTPUT_DIR, "06_hole_fill.png"),
            suptitle="Hole filling as a cleanup pass (holeFillMaxBoundary) — vs QuadriFlow",
        )


if __name__ == "__main__":
    main()
